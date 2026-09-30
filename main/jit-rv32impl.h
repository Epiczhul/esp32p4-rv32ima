/*
 * Same-ISA block JIT for the rv32ima emulator.  The guest speaks the same
 * instruction set as the ESP32-P4 cores, so hot straight-line runs of guest
 * code are re-emitted as native machine words and executed on the metal.
 * Everything untranslatable (csr, amo, ecall, mmio, page crossings) ends
 * the block and falls back to the interpreter, which stays the reference
 * implementation for all the tricky semantics.
 *
 * Included from uc-rv32ima.c after mini-rv32ima.h, inside the same
 * translation unit, so the Sv32 walker and the ram image pointer are
 * shared with the interpreter.
 *
 * Emission is two pass: a dry run over the emitter counts the words, then
 * the real pass fills the buffer knowing where the shared exit tail sits.
 */

#ifdef CONFIG_EMU_JIT

/* Host register roles.  The guest register file lives in the host register
 * file itself, minus a handful of reserved slots:
 *
 *   x0       zero, shared with guest x0
 *   x2       host stack pointer, never touched
 *   x3, x4   host gp/tp, never touched
 *   x22      scratch
 *   x23      block budget, counts down at backward branch targets
 *   x24      scratch
 *   x25      load row of the inline tlb
 *   x27      store row of the inline tlb
 *   x28      dispatcher return address
 *   x29      scratch (holds the exit delta on the way into the tail)
 *   x30      scratch (holds the virtual address during memory ops)
 *   x31      guest state pointer
 *
 * 18 of the 31 guest registers map 1:1, the rest stay in the state struct
 * and are loaded/stored around the few instructions that touch them.
 */
static const int8_t jit_g2h[32] = {
        0,   1,  18,  -1,  -1,   5,   6,   7,   8,   9,  10,  11,  12,  13,  14,  15,
        16,  17,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  19,  20,  21,  -1
};

#define JIT_SCRATCH1    22
#define JIT_SCRATCH2    24
#define JIT_VA          30
#define JIT_DELTA       29
#define JIT_CTX         31
#define JIT_ROW_LOAD    25
#define JIT_ROW_STORE   27
#define JIT_BUDGET      23
#define JIT_RET         28

#define JIT_PC_OFFSET   128     /* offsetof(MiniRV32IMAState, pc), regs is first */
#define JIT_MTVAL_OFFSET 180

#define JIT_TLB_ENTRIES 256     /* power of two, indexed by virtual page */
#define JIT_HASH_ENTRIES 4096   /* power of two, indexed by guest pc */
#define JIT_MAX_BLOCKS  512
#define JIT_HOT_ENTRIES 8192    /* power of two, indexed by guest pc */
#define JIT_HOT_THRESHOLD 24
#define JIT_MIN_BLOCK   4
#define JIT_MAX_BLOCK   256

#ifndef CONFIG_EMU_JIT_KB
#define CONFIG_EMU_JIT_KB 128
#endif

struct jit_tlb_entry {
        uint32_t tag;   /* guest virtual page number */
        uint32_t base;  /* host page address minus the guest page address */
};

struct jit_block {
        uint32_t pc;        /* guest virtual pc of the first instruction */
        uint32_t n_instr;
        uint8_t *code;
};

static struct jit_tlb_entry jit_tlb[2][JIT_TLB_ENTRIES];
static struct jit_block *jit_hash[JIT_HASH_ENTRIES];
static struct jit_block jit_pool[JIT_MAX_BLOCKS];
static int jit_pool_used = 0;
static uint8_t *jit_buf;
static uint8_t *jit_ptr;
static uint8_t *jit_buf_end;
static uint8_t jit_hot[JIT_HOT_ENTRIES];
/* bitmap of guest physical pages holding translated code, for store watches */
static uint8_t jit_code_pages[32 * 1024 * 1024 / 4096 / 8];
static int jit_ready = 0;

static void jit_flush_all(void);

/* --- machine word encoders -------------------------------------------- */

/* emit cursor, NULL during the sizing pass */
static uint8_t *je;
static int je_words;

static void ew(uint32_t w)
{
        if (je) {
                je[0] = w;
                je[1] = w >> 8;
                je[2] = w >> 16;
                je[3] = w >> 24;
                je += 4;
        }
        je_words++;
}

static uint32_t enc_r(uint32_t f7, uint32_t rs2, uint32_t rs1, uint32_t f3, uint32_t rd)
{
        return (f7 << 25) | (rs2 << 20) | (rs1 << 15) | (f3 << 12) | (rd << 7) | 0x33;
}

static uint32_t enc_i(int32_t imm, uint32_t rs1, uint32_t f3, uint32_t rd, uint32_t op)
{
        return ((imm & 0xfff) << 20) | (rs1 << 15) | (f3 << 12) | (rd << 7) | op;
}

static uint32_t enc_s(int32_t imm, uint32_t rs2, uint32_t rs1, uint32_t f3)
{
        return (((imm >> 5) & 0x7f) << 25) | (rs2 << 20) | (rs1 << 15) |
               (f3 << 12) | ((imm & 0x1f) << 7) | 0x23;
}

static uint32_t enc_b(int32_t off, uint32_t rs2, uint32_t rs1, uint32_t f3)
{
        return (((off >> 12) & 1) << 31) | (((off >> 5) & 0x3f) << 25) |
               (rs2 << 20) | (rs1 << 15) | (f3 << 12) |
               (((off >> 1) & 0xf) << 8) | (((off >> 11) & 1) << 7) | 0x63;
}

static uint32_t enc_u(uint32_t op, uint32_t rd, int32_t imm)
{
        return ((imm & 0xfffff) << 12) | (rd << 7) | op;
}

static uint32_t enc_j(int32_t off, uint32_t rd)
{
        return (((off >> 20) & 1) << 31) | (((off >> 1) & 0x3ff) << 21) |
               (((off >> 11) & 1) << 20) | (((off >> 12) & 0xff) << 12) |
               (rd << 7) | 0x6f;
}

/* load an arbitrary 32 bit constant, lui + addi with the hi/lo split */
static void e_li(uint32_t rd, int32_t v)
{
        int32_t hi = (v + 0x800) >> 12;
        int32_t lo = v - (hi << 12);

        if (lo == 0) {
                ew(enc_u(0x37, rd, hi));
        } else {
                ew(enc_u(0x37, rd, hi));
                ew(enc_i(lo, rd, 0, rd, 0x13));
        }
}

/*
 * Emit one ALU instruction with remapped registers.  Spilled operands are
 * pulled in from the state struct, a spilled destination is written back.
 * is_reg selects between the register and immediate form.
 */
static void jit_emit_alu(uint32_t ir, int is_reg)
{
        uint32_t taken = 0;
        uint32_t h_rs1, h_rs2 = 0, h_rd, word;
        int rs1 = (ir >> 15) & 0x1f;
        int rd = (ir >> 7) & 0x1f;

        h_rs1 = jit_g2h[rs1];
        if (h_rs1 == (int8_t)-1) {
                h_rs1 = JIT_SCRATCH1;
                taken |= 1;
                ew(enc_i(4 * rs1, JIT_CTX, 2, h_rs1, 0x03));
        }
        if (is_reg) {
                int rs2 = (ir >> 20) & 0x1f;

                h_rs2 = jit_g2h[rs2];
                if (h_rs2 == (int8_t)-1) {
                        h_rs2 = (taken & 1) ? JIT_SCRATCH2 : JIT_SCRATCH1;
                        taken |= 2;
                        ew(enc_i(4 * rs2, JIT_CTX, 2, h_rs2, 0x03));
                }
        }

        h_rd = jit_g2h[rd];
        if (h_rd == (int8_t)-1) {
                h_rd = (taken & 1) ? ((taken & 2) ? 29 : JIT_SCRATCH2)
                                   : JIT_SCRATCH1;
                taken |= 4;
        }

        if (is_reg)
                word = (ir & 0xfe000000) | (h_rs2 << 20) | (h_rs1 << 15) |
                       (ir & 0x00007000) | (h_rd << 7) | (ir & 0x7f);
        else
                word = (ir & 0xfff00000) | (h_rs1 << 15) |
                       (ir & 0x00007000) | (h_rd << 7) | (ir & 0x7f);
        ew(word);

        if (taken & 4)
                ew(enc_s(4 * rd, h_rd, JIT_CTX, 2));
}

/*
 * Emit one load or store through the inline tlb.  The tlb holds host page
 * addresses pre-subtracted by the guest page address, so the native access
 * is one add away from the virtual address.  A tag miss or a misaligned
 * sub-word access exits to the tail with the delta of this instruction,
 * the interpreter then redoes it with full semantics and fills the tlb.
 */
static void jit_emit_mem(uint32_t ir, int access, uint32_t block_pc,
                         uint32_t cur_pc, int32_t tail_off)
{
        uint32_t row = access == 1 ? JIT_ROW_LOAD : JIT_ROW_STORE;
        int rs1 = (ir >> 15) & 0x1f;
        int rd = (ir >> 7) & 0x1f;
        uint32_t f3 = (ir >> 12) & 7;
        int32_t imm;
        int32_t delta = (int32_t)(cur_pc - block_pc);

        if (access == 1)
                imm = (int32_t)ir >> 20;
        else {
                uint32_t uimm = ((ir >> 7) & 0x1f) | ((ir & 0xfe000000) >> 20);
                if (ir & 0x80000000)
                        uimm |= 0xfffff000;
                imm = (int32_t)uimm;
        }

        /* va = rs1 + imm */
        if (jit_g2h[rs1] == (int8_t)-1) {
                ew(enc_i(4 * rs1, JIT_CTX, 2, JIT_VA, 0x03));
                if (imm)
                        ew(enc_i(imm, JIT_VA, 0, JIT_VA, 0x13));
        } else {
                ew(enc_i(imm, jit_g2h[rs1], 0, JIT_VA, 0x13));
        }

        /* sub-word accesses must stay aligned, the interpreter owns the
         * misaligned trap semantics */
        if (f3 != 2) {
                uint32_t mask = (f3 == 1 || f3 == 5) ? 1 : 3;

                ew(enc_i(mask, JIT_VA, 7, JIT_SCRATCH1, 0x13));      /* andi */
                ew(enc_b(4 * 4, 0, JIT_SCRATCH1, 1));                /* bne  */
                ew(enc_i(delta, 0, 0, JIT_DELTA, 0x13));             /* li   */
                ew(enc_j((tail_off - je_words) * 4, 0));             /* jal  */
        }

        /* inline tlb lookup, tag = (vpn << 1) | 1 so a zeroed entry can
         * never match, no matter what page the guest touches */
        ew(enc_i(11, JIT_VA, 5, JIT_DELTA, 0x13));               /* srli      */
        ew(enc_i(1, JIT_DELTA, 6, JIT_DELTA, 0x13));             /* ori  tag  */
        ew(enc_i(1, JIT_DELTA, 5, JIT_SCRATCH1, 0x13));          /* srli vpn  */
        ew(enc_i(JIT_TLB_ENTRIES - 1, JIT_SCRATCH1, 7, JIT_SCRATCH1, 0x13));
        ew(enc_i(3, JIT_SCRATCH1, 1, JIT_SCRATCH1, 0x13));       /* slli *8   */
        ew(enc_r(0, row, JIT_SCRATCH1, 0, JIT_SCRATCH1));        /* add  &pair*/
        ew(enc_i(0, JIT_SCRATCH1, 2, JIT_SCRATCH2, 0x03));       /* lw   tag  */
        ew(enc_b(3 * 4, JIT_DELTA, JIT_SCRATCH2, 0));            /* beq  hit  */
        ew(enc_i(delta, 0, 0, JIT_DELTA, 0x13));                 /* li   delta*/
        ew(enc_j((tail_off - je_words) * 4, 0));                 /* jal  tail */

        /* hit: host address = base + va */
        ew(enc_i(4, JIT_SCRATCH1, 2, JIT_SCRATCH2, 0x03));       /* lw base   */
        ew(enc_r(0, JIT_VA, JIT_SCRATCH2, 0, JIT_SCRATCH1));     /* add  addr */

        if (access == 1) {
                switch (f3) {
                case 0: /* lb */
                        if (jit_g2h[rd] == (int8_t)-1) {
                                ew(enc_i(0, JIT_SCRATCH1, 4, JIT_SCRATCH2, 0x03));
                                ew(enc_s(4 * rd, JIT_SCRATCH2, JIT_CTX, 2));
                        } else {
                                ew(enc_i(0, JIT_SCRATCH1, 4, jit_g2h[rd], 0x03));
                        }
                        break;
                case 1: /* lh */
                        if (jit_g2h[rd] == (int8_t)-1) {
                                ew(enc_i(0, JIT_SCRATCH1, 1, JIT_SCRATCH2, 0x03));
                                ew(enc_s(4 * rd, JIT_SCRATCH2, JIT_CTX, 2));
                        } else {
                                ew(enc_i(0, JIT_SCRATCH1, 1, jit_g2h[rd], 0x03));
                        }
                        break;
                case 2: /* lw */
                        if (jit_g2h[rd] == (int8_t)-1) {
                                ew(enc_i(0, JIT_SCRATCH1, 2, JIT_SCRATCH2, 0x03));
                                ew(enc_s(4 * rd, JIT_SCRATCH2, JIT_CTX, 2));
                        } else {
                                ew(enc_i(0, JIT_SCRATCH1, 2, jit_g2h[rd], 0x03));
                        }
                        break;
                case 4: /* lbu */
                        if (jit_g2h[rd] == (int8_t)-1) {
                                ew(enc_i(0, JIT_SCRATCH1, 4, JIT_SCRATCH2, 0x03));
                                ew(enc_s(4 * rd, JIT_SCRATCH2, JIT_CTX, 2));
                        } else {
                                ew(enc_i(0, JIT_SCRATCH1, 4, jit_g2h[rd], 0x03));
                        }
                        break;
                case 5: /* lhu */
                        if (jit_g2h[rd] == (int8_t)-1) {
                                ew(enc_i(0, JIT_SCRATCH1, 5, JIT_SCRATCH2, 0x03));
                                ew(enc_s(4 * rd, JIT_SCRATCH2, JIT_CTX, 2));
                        } else {
                                ew(enc_i(0, JIT_SCRATCH1, 5, jit_g2h[rd], 0x03));
                        }
                        break;
                }
        } else {
                int rs2 = (ir >> 20) & 0x1f;

                if (jit_g2h[rs2] == (int8_t)-1)
                        ew(enc_i(4 * rs2, JIT_CTX, 2, JIT_SCRATCH2, 0x03));
                switch (f3) {
                case 0: /* sb */
                        ew(enc_s(0, jit_g2h[rs2] == (int8_t)-1 ? JIT_SCRATCH2 : jit_g2h[rs2],
                                 JIT_SCRATCH1, 0));
                        break;
                case 1: /* sh */
                        ew(enc_s(0, jit_g2h[rs2] == (int8_t)-1 ? JIT_SCRATCH2 : jit_g2h[rs2],
                                 JIT_SCRATCH1, 1));
                        break;
                case 2: /* sw */
                        ew(enc_s(0, jit_g2h[rs2] == (int8_t)-1 ? JIT_SCRATCH2 : jit_g2h[rs2],
                                 JIT_SCRATCH1, 2));
                        break;
                }
        }
}

/* Shared exit tail, plus a variant for budget exits that marks mtval so
 * the C side knows the instruction count is only approximate. */
static void jit_emit_tail(uint32_t block_pc, int budget_kind)
{
        e_li(JIT_SCRATCH1, (int32_t)block_pc);
        ew(enc_r(0, JIT_DELTA, JIT_SCRATCH1, 0, JIT_DELTA));     /* add */
        ew(enc_s(JIT_PC_OFFSET, JIT_DELTA, JIT_CTX, 2));         /* sw pc */
        if (budget_kind)
                ew(enc_i(1, 0, 0, JIT_SCRATCH1, 0x13));          /* li 1 */
        else
                ew(enc_i(0, 0, 0, JIT_SCRATCH1, 0x13));          /* li 0 */
        ew(enc_s(JIT_MTVAL_OFFSET, JIT_SCRATCH1, JIT_CTX, 2));   /* sw mtval */
        for (int g = 1; g < 32; g++) {
                if (jit_g2h[g] > 0)
                        ew(enc_s(4 * g, jit_g2h[g], JIT_CTX, 2));
        }
        ew(enc_i(0, JIT_RET, 0, 0, 0x67));                       /* jr x28 */
}

/* Budget check before a backward branch target, x23 counts iterations. */
static void jit_emit_budget(uint32_t block_pc, uint32_t cur_pc, int32_t tail_off)
{
        int32_t delta = (int32_t)(cur_pc - block_pc);

        ew(enc_i(-1, JIT_BUDGET, 0, JIT_BUDGET, 0x13));          /* addi -1 */
        ew(enc_b(3 * 4, 0, JIT_BUDGET, 5));                      /* bge skip */
        ew(enc_i(delta, 0, 0, JIT_DELTA, 0x13));                 /* li delta */
        ew(enc_j((tail_off - je_words) * 4, 0));                 /* jal tail */
}

/* pull a possibly spilled operand into a register */
static uint32_t jit_operand_reg(uint32_t ir, int shift, int *used_scratch)
{
        int g = (ir >> shift) & 0x1f;
        uint32_t h = jit_g2h[g];

        if (h == (int8_t)-1) {
                h = *used_scratch ? JIT_SCRATCH2 : JIT_SCRATCH1;
                *used_scratch = 1;
                ew(enc_i(4 * g, JIT_CTX, 2, h, 0x03));
        }
        return h;
}

/* --- scan and translate ------------------------------------------------ */

enum {
        JIT_OP_ALU,
        JIT_OP_LUI,
        JIT_OP_AUIPC,
        JIT_OP_BRANCH,
        JIT_OP_JAL,
        JIT_OP_FENCE,
        JIT_OP_MEM,
        JIT_OP_END
};

struct jit_scan {
        uint32_t ir;
        uint32_t target;
        uint8_t kind;
        uint8_t f3;
        uint8_t access;
        uint8_t backtarget;
};

/* Decode one instruction.  Returns JIT_OP_END for anything the block must
 * not swallow: system ops, atomics, mmio boundaries, the illegal space.
 * Branch and jal targets are recorded optimistically, the post pass in
 * jit_translate_block demotes the ones that leave the block. */
static int jit_scan_classify(uint32_t ir, uint32_t pc, struct jit_scan *s)
{
        uint32_t op = ir & 0x7f;
        uint32_t f3 = (ir >> 12) & 7;

        s->ir = ir;
        s->f3 = f3;
        s->access = 0;
        s->backtarget = 0;
        s->target = 0;

        switch (op) {
        case 0x37: return JIT_OP_LUI;
        case 0x17: return JIT_OP_AUIPC;
        case 0x6f: { /* JAL */
                int32_t off = ((ir & 0x80000000) >> 11) | ((ir & 0x7fe00000) >> 20) |
                              ((ir & 0x00100000) >> 9) | (ir & 0x000ff000);
                if (off & 0x00100000) off |= 0xffe00000;
                s->target = pc + off;
                return (s->target & 3) ? JIT_OP_END : JIT_OP_JAL;
        }
        case 0x67: return JIT_OP_END; /* JALR, dynamic target */
        case 0x63: { /* branches */
                int32_t off = ((ir & 0xf00) >> 7) | ((ir & 0x7e000000) >> 20) |
                              ((ir & 0x80) << 4) | ((ir >> 31) << 12);
                if (off & 0x1000) off |= 0xffffe000;
                if (f3 == 2 || f3 == 3) return JIT_OP_END; /* illegal */
                s->target = pc + off;
                return (s->target & 3) ? JIT_OP_END : JIT_OP_BRANCH;
        }
        case 0x03: /* loads */
                if (f3 == 0 || f3 == 1 || f3 == 2 || f3 == 4 || f3 == 5) {
                        s->access = 1;
                        return JIT_OP_MEM;
                }
                return JIT_OP_END;
        case 0x23: /* stores */
                if (f3 == 0 || f3 == 1 || f3 == 2) {
                        s->access = 2;
                        return JIT_OP_MEM;
                }
                return JIT_OP_END;
        case 0x13: return JIT_OP_ALU; /* op-imm */
        case 0x33: return JIT_OP_ALU; /* op, includes the M extension */
        case 0x0f: return f3 == 0 ? JIT_OP_FENCE : JIT_OP_END;
        default:   return JIT_OP_END; /* SYSTEM, AMO, and the rest */
        }
}

static int jit_scan_block(struct MiniRV32IMAState *state, uint8_t *image,
                          uint32_t pc, struct jit_scan *scan)
{
        uint32_t block_pc = pc;
        int n = 0;

        while (n < JIT_MAX_BLOCK) {
                uint32_t trap = 0;
                uint32_t ppc = MiniRV32IMATranslate(state, image, pc, 0, &trap);

                if (trap)
                        break;
                uint32_t ofs = ppc - MINIRV32_RAM_IMAGE_OFFSET;
                if (ofs >= MINI_RV32_RAM_SIZE - 3)
                        break;

                struct jit_scan *s = &scan[n];
                uint32_t ir = MINIRV32_LOAD4(ofs);
                int kind = jit_scan_classify(ir, pc, s);

                if (kind == JIT_OP_END)
                        break;
                s->kind = kind;
                n++;
                pc += 4;
                if ((pc & 0xfff) == 0)
                        break; /* page boundary */
        }
        (void)block_pc;
        return n;
}

/* mark loop heads so they get a budget check */
static void jit_mark_backtargets(struct jit_scan *scan, int n, uint32_t block_pc)
{
        for (int i = 0; i < n; i++) {
                if ((scan[i].kind != JIT_OP_BRANCH && scan[i].kind != JIT_OP_JAL) ||
                    scan[i].target >= block_pc + 4 * (uint32_t)i)
                        continue;
                uint32_t idx = (scan[i].target - block_pc) >> 2;
                if (idx < (uint32_t)n)
                        scan[idx].backtarget = 1;
        }
}

static uint32_t jit_pos[JIT_MAX_BLOCK + 1];

static int jit_emit_block(struct jit_scan *scan, int n, uint32_t block_pc,
                          int32_t tail_off, int32_t tail_b_off)
{
        /* prologue: pull the mapped guest registers into host registers */
        for (int g = 1; g < 32; g++) {
                if (jit_g2h[g] > 0)
                        ew(enc_i(4 * g, JIT_CTX, 2, jit_g2h[g], 0x03));
        }

        for (int i = 0; i < n; i++) {
                struct jit_scan *s = &scan[i];
                uint32_t cur = block_pc + 4 * (uint32_t)i;

                /* where this instruction starts in the emitted stream,
                 * branches land here so the budget check is re-run */
                jit_pos[i] = je_words;

                if (s->backtarget)
                        jit_emit_budget(block_pc, cur, tail_b_off);

                switch (s->kind) {
                case JIT_OP_LUI: {
                        int rd = (s->ir >> 7) & 0x1f;
                        uint32_t h = jit_g2h[rd];
                        int spilled = h == (int8_t)-1;

                        if (spilled) h = JIT_SCRATCH1;
                        ew(enc_u(0x37, h, (int32_t)s->ir >> 12));
                        if (spilled)
                                ew(enc_s(4 * rd, JIT_SCRATCH1, JIT_CTX, 2));
                        break;
                }
                case JIT_OP_AUIPC: {
                        int rd = (s->ir >> 7) & 0x1f;
                        uint32_t h = jit_g2h[rd];
                        int spilled = h == (int8_t)-1;

                        if (spilled) h = JIT_SCRATCH1;
                        ew(enc_u(0x17, h, (int32_t)s->ir >> 12));
                        if (spilled)
                                ew(enc_s(4 * rd, JIT_SCRATCH1, JIT_CTX, 2));
                        break;
                }
                case JIT_OP_BRANCH: {
                        int used = 0;
                        uint32_t tidx = (s->target - block_pc) >> 2;
                        uint32_t h_rs1 = jit_operand_reg(s->ir, 15, &used);
                        uint32_t h_rs2 = jit_operand_reg(s->ir, 20, &used);

                        /* jump to the target's spot in the emitted stream */
                        ew(enc_b((int32_t)(jit_pos[tidx] - je_words) * 4,
                                 h_rs2, h_rs1, s->f3));
                        break;
                }
                case JIT_OP_JAL: {
                        int rd = (s->ir >> 7) & 0x1f;
                        uint32_t tidx = (s->target - block_pc) >> 2;
                        uint32_t h = jit_g2h[rd];
                        int spilled = h == (int8_t)-1;

                        if (spilled) h = JIT_SCRATCH1;
                        ew(enc_j((int32_t)(jit_pos[tidx] - je_words) * 4, h));
                        if (spilled)
                                ew(enc_s(4 * rd, JIT_SCRATCH1, JIT_CTX, 2));
                        break;
                }
                case JIT_OP_FENCE:
                        break; /* plain fence, nothing to do */
                case JIT_OP_MEM:
                        jit_emit_mem(s->ir, s->access, block_pc, cur, tail_off);
                        break;
                case JIT_OP_ALU:
                        jit_emit_alu(s->ir, (s->ir & 0x7f) == 0x33);
                        break;
                }
        }

        /* normal fall-through exit: the tail adds the delta to the block
         * pc, so it has to carry the full block length here */
        ew(enc_i(4 * n, 0, 0, JIT_DELTA, 0x13));
        return je_words;
}

static struct jit_block *jit_lookup(uint32_t pc)
{
        struct jit_block *b = jit_hash[(pc >> 2) & (JIT_HASH_ENTRIES - 1)];

        return (b && b->pc == pc) ? b : NULL;
}

static struct jit_block *jit_translate_block(struct MiniRV32IMAState *state,
                                             uint8_t *image, uint32_t pc)
{
        struct jit_scan scan[JIT_MAX_BLOCK];
        struct jit_block *b;
        int n, words, tail_words, total, len;

        scan[0].target = 0;
        n = jit_scan_block(state, image, pc, scan);
        if (n < JIT_MIN_BLOCK) {
                /* do not retry until the counters reset */
                jit_hot[(pc >> 2) & (JIT_HOT_ENTRIES - 1)] = 0xff;
                return NULL;
        }

        /* demote branches and jals that leave the block: the block simply
         * ends before them and the interpreter takes over.  Truncating can
         * push earlier forward targets outside again, so iterate until the
         * surviving prefix has no escaping branch left. */
        for (int changed = 1; changed; ) {
                changed = 0;
                for (int i = 0; i < n; i++) {
                        if ((scan[i].kind == JIT_OP_BRANCH || scan[i].kind == JIT_OP_JAL) &&
                            (scan[i].target < pc || scan[i].target >= pc + 4 * (uint32_t)n)) {
                                n = i;
                                changed = 1;
                                break;
                        }
                }
        }
        if (n < JIT_MIN_BLOCK) {
                jit_hot[(pc >> 2) & (JIT_HOT_ENTRIES - 1)] = 0xff;
                return NULL;
        }

        jit_mark_backtargets(scan, n, pc);

        /* sizing pass, tail offsets are placeholders here */
        je = NULL;
        je_words = 0;
        jit_emit_block(scan, n, pc, -1, -1);
        words = je_words;
        je_words = 0;
        jit_emit_tail(pc, 0);
        tail_words = je_words;
        total = (words + 2 * tail_words) * 4 + 64;

        if (jit_pool_used >= JIT_MAX_BLOCKS || jit_ptr + total > jit_buf_end)
                jit_flush_all();
        if (jit_ptr + total > jit_buf_end)
                return NULL;

        b = &jit_pool[jit_pool_used++];
        b->pc = pc;
        b->n_instr = n;
        b->code = jit_ptr;

        /* real pass */
        je = b->code;
        je_words = 0;
        jit_emit_block(scan, n, pc, words, words + tail_words);
        jit_emit_tail(pc, 0);
        jit_emit_tail(pc, 1);
        len = je - b->code;
        je = NULL;
        jit_exec_commit(b->code, len);
        jit_ptr += (len + 63) & ~63;

        /* remember which physical pages hold this code, for store watches */
        for (int i = 0; i < n; i++) {
                uint32_t ofs = (pc + 4 * (uint32_t)i) - MINIRV32_RAM_IMAGE_OFFSET;
                uint32_t idx = ofs >> 12;

                jit_code_pages[idx >> 3] |= 1 << (idx & 7);
        }

        jit_hash[(pc >> 2) & (JIT_HASH_ENTRIES - 1)] = b;
        return b;
}

/*
 * Fill the inline tlb for the memory instruction at pc, so the next run of
 * the block takes the fast path.  Returns 0 when the interpreter has to
 * own the instruction (mmio, fault, anything odd).
 */
static int jit_try_fill(struct MiniRV32IMAState *state, uint8_t *image, uint32_t pc)
{
        uint32_t trap = 0;
        uint32_t ppc = MiniRV32IMATranslate(state, image, pc, 0, &trap);
        uint32_t ir, access, f3, va, paddr;
        int rs1;
        int32_t imm;

        if (trap)
                return 0;
        uint32_t ofs = ppc - MINIRV32_RAM_IMAGE_OFFSET;
        if (ofs >= MINI_RV32_RAM_SIZE - 3 || (pc & 3))
                return 0;
        ir = MINIRV32_LOAD4(ofs);
        uint32_t op = ir & 0x7f;
        if (op == 0x03)
                access = 1;
        else if (op == 0x23)
                access = 2;
        else
                return 0;
        f3 = (ir >> 12) & 7;
        if ((access == 1 && (f3 == 3 || f3 == 6 || f3 == 7)) ||
            (access == 2 && f3 > 2))
                return 0;

        rs1 = (ir >> 15) & 0x1f;
        if (op == 0x03) {
                imm = (int32_t)ir >> 20;
        } else {
                uint32_t uimm = ((ir >> 7) & 0x1f) | ((ir & 0xfe000000) >> 20);

                if (ir & 0x80000000)
                        uimm |= 0xfffff000;
                imm = (int32_t)uimm;
        }
        va = state->regs[rs1] + (uint32_t)imm;

        paddr = MiniRV32IMATranslate(state, image, va, access, &trap);
        if (trap)
                return 0;
        ofs = paddr - MINIRV32_RAM_IMAGE_OFFSET;
        if (ofs >= MINI_RV32_RAM_SIZE - 3)
                return 0; /* mmio, let the interpreter handle it */

        uint32_t vpn = va >> 12;
        uint32_t hostpage = (uint32_t)(uintptr_t)image +
                            (paddr & ~0xfffU) - MINIRV32_RAM_IMAGE_OFFSET;
        struct jit_tlb_entry *e = &jit_tlb[access - 1][vpn & (JIT_TLB_ENTRIES - 1)];

        e->tag = (vpn << 1) | 1;
        e->base = hostpage - (va & ~0xfffU);
        return 1;
}

/*
 * Call into translated code.  The block returns with state->pc set to the
 * next guest pc; a budget exit also marks state->mtval so the caller knows
 * the returned count is approximate.
 */
struct jit_run_args {
        void *code;
        struct MiniRV32IMAState *st;
        void *tl;
        void *ts;
        int budget;
};

void jit_call_entry(struct jit_run_args *args);

static int jit_run_block(struct jit_block *b, struct MiniRV32IMAState *st,
                         int budget, uint32_t *next)
{
        struct jit_run_args args;

        args.code = b->code;
        args.st = st;
        args.tl = jit_tlb[0];
        args.ts = jit_tlb[1];
        args.budget = budget;
        jit_call_entry(&args);
        *next = st->pc;

        uint32_t npc = *next;

        if (npc > b->pc && (npc & 3) == 0 && npc <= b->pc + b->n_instr * 4)
                return (npc - b->pc) >> 2; /* exact, forward exit */
        return b->n_instr;                 /* budget exit, approximate */
}

/*
 * Hook called at the top of the interpreter loop.  Returns 1 when it ran
 * translated code and advanced *pc by itself.
 */
static int jit_hook(struct MiniRV32IMAState *state, uint8_t *image,
                    uint32_t *pc, int budget, int *n_out)
{
        struct jit_block *b;
        uint32_t next = 0;
        int n;

        if (!jit_ready || budget < 32 || (*pc & 3))
                return 0;

        uint32_t h = (*pc >> 2) & (JIT_HOT_ENTRIES - 1);
        if (++jit_hot[h] == JIT_HOT_THRESHOLD && !jit_lookup(*pc))
                jit_translate_block(state, image, *pc);

        b = jit_lookup(*pc);
        if (!b)
                return 0;

        n = jit_run_block(b, state, budget, &next);
        if (state->mtval == 1) {
                state->mtval = 0; /* budget exit, count is approximate */
                n = budget;
        }
        if (n < 1 || next == *pc)
                return 0;
        if ((uint32_t)n < b->n_instr)
                jit_try_fill(state, image, next);
        *pc = next;
        *n_out = n;
        return 1;
}

static void jit_flush_all(void)
{
        memset(jit_hash, 0, sizeof(jit_hash));
        memset(jit_hot, 0, sizeof(jit_hot));
        memset(jit_code_pages, 0, sizeof(jit_code_pages));
        memset(jit_tlb, 0, sizeof(jit_tlb));
        jit_ptr = jit_buf;
        jit_pool_used = 0;
}

/* called from the interpreter when its own translation state resets */
static void jit_tlb_flushed(void)
{
        if (jit_ready)
                jit_flush_all();
}

/* guest executed fence.i, code pages may have changed */
static void jit_code_fence(void)
{
        if (jit_ready)
                jit_flush_all();
}

/* guest stored into a page that holds translated code */
static void jit_store_notify(uint32_t ofs)
{
        if (!jit_ready ||
            !((jit_code_pages[ofs >> 15] >> ((ofs >> 12) & 7)) & 1))
                return;

        /* the block on this page would come back hot and immediately be
         * flushed again, so retire its pc until the counters reset */
        uint32_t page = ofs >> 12;
        for (int i = 0; i < JIT_HASH_ENTRIES; i++) {
                struct jit_block *b = jit_hash[i];
                if (b) {
                        uint32_t bpage = (b->pc - MINIRV32_RAM_IMAGE_OFFSET) >> 12;
                        if (bpage == page)
                                jit_hot[(b->pc >> 2) & (JIT_HOT_ENTRIES - 1)] = 0xff;
                }
        }
        jit_flush_all();
}

static void jit_init(void)
{
        int kb = CONFIG_EMU_JIT_KB;

        jit_buf = jit_exec_alloc(kb * 1024);
        if (!jit_buf) {
                printf("jit: no executable memory, staying on the interpreter\n");
                return;
        }
        jit_buf_end = jit_buf + kb * 1024;
        jit_flush_all();
        jit_ready = 1;
        printf("jit: %dKB code buffer at %p\n", kb, jit_buf);
}

#endif /* CONFIG_EMU_JIT */
