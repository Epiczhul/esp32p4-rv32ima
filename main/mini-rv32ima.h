// Copyright 2022 Charles Lohr, you may use this file or any portions herein under any of the BSD, MIT, or CC0 licenses.

#ifndef _MINI_RV32IMAH_H
#define _MINI_RV32IMAH_H

/**
    To use mini-rv32ima.h for the bare minimum, the following:

        #define MINI_RV32_RAM_SIZE ram_amt
        #define MINIRV32_IMPLEMENTATION

        #include "mini-rv32ima.h"

        Though, that's not _that_ interesting. You probably want I/O!


        Notes:
                * There is a dedicated CLNT at 0x10000000.
                * There is free MMIO from there to 0x12000000.
                * You can put things like a UART, or whatever there.
                * Feel free to override any of the functionality with macros.
*/

#ifndef MINIRV32WARN
        #define MINIRV32WARN( x... );
#endif

#ifndef MINIRV32_DECORATE
        #define MINIRV32_DECORATE static
#endif

#ifndef MINIRV32_RAM_IMAGE_OFFSET
        #define MINIRV32_RAM_IMAGE_OFFSET  0x80000000
#endif

#ifndef MINIRV32_POSTEXEC
        #define MINIRV32_POSTEXEC(...);
#endif

#ifndef MINIRV32_HANDLE_MEM_STORE_CONTROL
        #define MINIRV32_HANDLE_MEM_STORE_CONTROL(...);
#endif

#ifndef MINIRV32_HANDLE_MEM_LOAD_CONTROL
        #define MINIRV32_HANDLE_MEM_LOAD_CONTROL(...);
#endif

#ifndef MINIRV32_OTHERCSR_WRITE
        #define MINIRV32_OTHERCSR_WRITE(...);
#endif

#ifndef MINIRV32_OTHERCSR_READ
        #define MINIRV32_OTHERCSR_READ(...);
#endif

#ifndef MINIRV32_CUSTOM_MEMORY_BUS
        #define MINIRV32_STORE4( ofs, val ) *(uint32_t*)(image + ofs) = val
        #define MINIRV32_STORE2( ofs, val ) *(uint16_t*)(image + ofs) = val
        #define MINIRV32_STORE1( ofs, val ) *(uint8_t*)(image + ofs) = val
        #define MINIRV32_LOAD4( ofs ) *(uint32_t*)(image + ofs)
        #define MINIRV32_LOAD2( ofs ) *(uint16_t*)(image + ofs)
        #define MINIRV32_LOAD1( ofs ) *(uint8_t*)(image + ofs)
#endif

#ifndef MINIRV32_MISA_VALUE
        #define MINIRV32_MISA_VALUE 0x40401101
#endif

#ifndef MINIRV32_TLB_ENTRIES
        #define MINIRV32_TLB_ENTRIES 64
#endif

#ifndef MINIRV32_EXT_IRQ_PENDING
        #define MINIRV32_EXT_IRQ_PENDING( state ) 0
#endif

#ifndef MINIRV32_SBI_ECALL
        #define MINIRV32_SBI_ECALL( state )
#endif

// As a note: We quouple-ify these, because in HLSL, we will be operating with
// uint4's.  We are going to uint4 data to/from system RAM.
//
// We're going to try to keep the full processor state to 12 x uint4.
struct MiniRV32IMAState
{
        uint32_t regs[32];

        uint32_t pc;
        uint32_t mstatus;
        uint32_t cyclel;
        uint32_t cycleh;

        uint32_t timerl;
        uint32_t timerh;
        uint32_t timermatchl;
        uint32_t timermatchh;

        uint32_t mscratch;
        uint32_t mtvec;
        uint32_t mie;
        uint32_t mip;

        uint32_t mepc;
        uint32_t mtval;
        uint32_t mcause;

        // Note: only a few bits are used.  (Machine = 3, User = 0)
        // Bits 0..1 = privilege.
        // Bit 2 = WFI (Wait for interrupt)
        // Bit 3+ = Load/Store reservation LSBs.
        uint32_t extraflags;

        // Delegation + supervisor CSRs.  Only used when the kernel runs in
        // S-mode, the M-mode boot path leaves them alone.
        uint32_t mideleg;
        uint32_t medeleg;
        uint32_t satp;
        uint32_t stvec;
        uint32_t sepc;
        uint32_t scause;
        uint32_t stval;
        uint32_t sscratch;
        uint32_t sie;
        uint32_t sip;
};

MINIRV32_DECORATE int32_t MiniRV32IMAStep( struct MiniRV32IMAState * state, uint8_t * image, uint32_t vProcAddress, uint32_t elapsedUs, int count );

#ifdef MINIRV32_IMPLEMENTATION

#define CSR( x ) state->x
#define SETCSR( x, val ) { state->x = val; }
#define REG( x ) state->regs[x]
#define REGSET( x, val ) { state->regs[x] = val; }

// Same-ISA block JIT, implemented in jit-rv32impl.h which uc-rv32ima.c
// includes after this header.  The hooks here keep the translated code
// coherent with the interpreter's own translation state.
#ifdef CONFIG_EMU_JIT
#define JIT_STORE_NOTIFY( ofs ) jit_store_notify( ofs )
static int jit_hook( struct MiniRV32IMAState * state, uint8_t * image, uint32_t * pc, int budget, int * n_out );
static void jit_store_notify( uint32_t ofs );
static void jit_code_fence( void );
static void jit_tlb_flushed( void );
#else
#define JIT_STORE_NOTIFY( ofs )
#endif

// Sv32 translation state, one TLB per access class.
static uint32_t tlb_tag[3][MINIRV32_TLB_ENTRIES];
static uint32_t tlb_base[3][MINIRV32_TLB_ENTRIES];

static void MiniRV32IMATLBFlush(void)
{
        memset( tlb_tag, 0, sizeof( tlb_tag ) );
#ifdef CONFIG_EMU_JIT
        jit_tlb_flushed();
#endif
}

// Translate a virtual address.  access: 0 = fetch, 1 = load, 2 = store/AMO.
// Returns the physical address, *trap carries the trap code on a fault.
// M-mode and satp mode = bare pass through unchanged.
static uint32_t MiniRV32IMATranslate( struct MiniRV32IMAState * state, uint8_t * image, uint32_t vaddr, int access, uint32_t * trap )
{
        (void) image;

        uint32_t mode = CSR( extraflags ) & 3;

        // MPRV reroutes data accesses in M-mode through MPP's translation.
        if( access != 0 && mode == 3 && ( CSR( mstatus ) & (1<<17) ) )
                mode = ( CSR( mstatus ) >> 11 ) & 3;

        if( mode == 3 || !( CSR( satp ) >> 31 ) )
                return vaddr;

        uint32_t vpn = vaddr >> 12;
        uint32_t idx = vpn & ( MINIRV32_TLB_ENTRIES - 1 );
        if( tlb_tag[access][idx] == (( vpn << 1 ) | 1) )
                return tlb_base[access][idx] | ( vaddr & 0xfff );

        // Slow path: walk the two-level Sv32 tables.
        uint32_t pte_addr = ( CSR( satp ) & 0x3fffff ) << 12;
        pte_addr += ( vaddr >> 22 ) << 2;
        uint32_t is_super = 0;
        uint32_t pte = 0;
        uint32_t pte_ofs = 0;

        for( int level = 0; level < 2; level++ )
        {
                pte_ofs = pte_addr - MINIRV32_RAM_IMAGE_OFFSET;
                if( pte_ofs >= MINI_RV32_RAM_SIZE - 3 )
                        goto fault;
                pte = MINIRV32_LOAD4( pte_ofs );

                if( !( pte & 0x01 ) ) // V
                        goto fault;

                if( pte & 0x0e ) // R|W|X, this one is a leaf.
                {
                        is_super = !level;
                        break;
                }

                if( level == 1 ) // Non-leaf at the last level.
                        goto fault;

                pte_addr = (( pte >> 10 ) << 12) | (( ( vaddr >> 12 ) & 0x3ff ) << 2);
        }

        if( is_super && ( pte & 0xffc00 ) ) // Misaligned superpage, ppn[0] must be zero.
                goto fault;

        // Permission checks, SUM and MXR included.
        uint32_t mstatus = CSR( mstatus );
        if( access == 0 )
        {
                if( !( pte & 0x08 ) ) goto fault; // X
        }
        else if( access == 1 )
        {
                if( !( ( pte & 0x02 ) || ( ( mstatus & (1<<19) ) && ( pte & 0x08 ) ) ) ) goto fault; // R, or X with MXR
        }
        else
        {
                if( !( ( pte & 0x02 ) && ( pte & 0x04 ) ) ) goto fault; // R+W
        }
        if( mode == 0 )
        {
                if( !( pte & 0x10 ) ) goto fault; // U pages are only reachable from U-mode.
        }
        else
        {
                if( ( pte & 0x10 ) && !( mstatus & (1<<18) ) ) goto fault; // SUM for U pages in S-mode.
        }

        // Hardware-managed A/D bits, written back into the PTE.
        if( !( pte & 0x40 ) || ( access == 2 && !( pte & 0x80 ) ) )
        {
                pte |= 0x40;
                if( access == 2 ) pte |= 0x80;
                MINIRV32_STORE4( pte_ofs, pte );
        }

        uint32_t paddr;
        if( is_super )
                paddr = (( pte & 0xfff00000 ) << 2) | ( vaddr & 0x3fffff );
        else
                paddr = (( pte >> 10 ) << 12) | ( vaddr & 0xfff );

        tlb_tag[access][idx] = ( vpn << 1 ) | 1;
        tlb_base[access][idx] = paddr & ~0xfff;
        return paddr;

fault:
        *trap = ( access == 0 ) ? (12+1) : ( access == 1 ) ? (13+1) : (15+1);
        return 0;
}

MINIRV32_DECORATE int32_t MiniRV32IMAStep( struct MiniRV32IMAState * state, uint8_t * image, uint32_t vProcAddress, uint32_t elapsedUs, int count )
{
        uint32_t new_timer = CSR( timerl ) + elapsedUs;
        if( new_timer < CSR( timerl ) ) CSR( timerh )++;
        CSR( timerl ) = new_timer;

        // Handle Timer interrupt.
        if( ( CSR( timerh ) > CSR( timermatchh ) || ( CSR( timerh ) == CSR( timermatchh ) && CSR( timerl ) > CSR( timermatchl ) ) ) && ( CSR( timermatchh ) || CSR( timermatchl ) ) )
        {
                CSR( extraflags ) &= ~4; // Clear WFI
                CSR( mip ) |= 1<<7; //MTIP of MIP // https://stackoverflow.com/a/61916199/2926815  Fire interrupt.
                CSR( mip ) |= 1<<5; //STIP, the S-mode view of the same timer.
        }
        else
        {
                CSR( mip ) &= ~(1<<7);
                CSR( mip ) &= ~(1<<5);
        }

        // SEIP comes from the PLIC, if one is wired up.
        if( MINIRV32_EXT_IRQ_PENDING( state ) )
                CSR( mip ) |= 1<<9;
        else
                CSR( mip ) &= ~(1<<9);

        // Pick a deliverable interrupt.  MTIP always stays in M-mode,
        // STIP/SEIP only reach S-mode when delegated via mideleg.
        uint32_t minterrupt = 0;
        uint32_t privmode = CSR( extraflags ) & 3;
        if( ( CSR( mip ) & (1<<7) ) && ( CSR( mie ) & (1<<7) /*mtie*/ ) && ( CSR( mstatus ) & 0x8 /*mie*/) )
        {
                minterrupt = 0x80000007;
        }
        else if( privmode != 3 )
        {
                // S-mode interrupts are taken from U-mode, or from S-mode with SIE.
                uint32_t sienable = ( privmode == 0 ) || ( CSR( mstatus ) & 0x2 /*sie*/ );
                if( sienable && ( CSR( mip ) & (1<<9) ) && ( CSR( sie ) & (1<<9) ) && ( CSR( mideleg ) & (1<<9) ) )
                        minterrupt = 0x80000009;
                else if( sienable && ( CSR( mip ) & (1<<5) ) && ( CSR( sie ) & (1<<5) ) && ( CSR( mideleg ) & (1<<5) ) )
                        minterrupt = 0x80000005;
        }

        // A deliverable interrupt also ends WFI.
        if( minterrupt )
                CSR( extraflags ) &= ~4;

        // If WFI, don't run processor.
        if( CSR( extraflags ) & 4 )
                return 1;

        uint32_t trap = 0;
        uint32_t rval = 0;
        uint32_t pc = CSR( pc );
        uint32_t cycle = CSR( cyclel );

        if( minterrupt )
        {
                // Interrupt waiting to be delivered.
                trap = minterrupt;
                pc -= 4;
        }
        else // No interrupt?  Execute a bunch of instructions.
        for( int icount = 0; icount < count; icount++ )
        {
#ifdef CONFIG_EMU_JIT
                // Hand a hot run of guest code to the block JIT, it
                // advances pc by itself and reports the instructions done.
                {
                        int jn = 0;
                        if( jit_hook( state, image, &pc, count - icount, &jn ) )
                        {
                                icount += jn - 1;
                                cycle += jn;
                                continue;
                        }
                }
#endif
                uint32_t ir = 0;
                rval = 0;
                cycle++;
                uint32_t ppc = MiniRV32IMATranslate( state, image, pc, 0, &trap );
                if( trap )  // Instruction page fault, mtval becomes pc.
                        break;

                uint32_t ofs_pc = ppc - MINIRV32_RAM_IMAGE_OFFSET;

                if( ofs_pc  >= MINI_RV32_RAM_SIZE )
                {
                        trap = 1 + 1;  // Handle access violation on instruction read.
                        break;
                }
                else if( ofs_pc & 3 )
                {
                        trap = 1 + 0;  //Handle PC-misaligned access
                        break;
                }
                else
                {
                        ir = MINIRV32_LOAD4( ofs_pc );
                        uint32_t rdid = (ir >> 7) & 0x1f;

                        switch( ir & 0x7f )
                        {
                                case 0b0110111: // LUI
                                        rval = ( ir & 0xfffff000 );
                                        break;
                                case 0b0010111: // AUIPC
                                        rval = pc + ( ir & 0xfffff000 );
                                        break;
                                case 0b1101111: // JAL
                                {
                                        int32_t reladdy = ((ir & 0x80000000)>>11) | ((ir & 0x7fe00000)>>20) | ((ir & 0x00100000)>>9) | ((ir&0x000ff000));
                                        if( reladdy & 0x00100000 ) reladdy |= 0xffe00000; // Sign extension.
                                        rval = pc + 4;
                                        pc = pc + reladdy - 4;
                                        break;
                                }
                                case 0b1100111: // JALR
                                {
                                        uint32_t imm = ir >> 20;
                                        int32_t imm_se = imm | (( imm & 0x800 )?0xfffff000:0);
                                        rval = pc + 4;
                                        pc = ( (REG( (ir >> 15) & 0x1f ) + imm_se) & ~1) - 4;
                                        break;
                                }
                                case 0b1100011: // Branch
                                {
                                        uint32_t immm4 = ((ir & 0xf00)>>7) | ((ir & 0x7e000000)>>20) | ((ir & 0x80) << 4) | ((ir >> 31)<<12);
                                        if( immm4 & 0x1000 ) immm4 |= 0xffffe000;
                                        int32_t rs1 = REG((ir >> 15) & 0x1f);
                                        int32_t rs2 = REG((ir >> 20) & 0x1f);
                                        immm4 = pc + immm4 - 4;
                                        rdid = 0;
                                        switch( ( ir >> 12 ) & 0x7 )
                                        {
                                                // BEQ, BNE, BLT, BGE, BLTU, BGEU
                                                case 0b000: if( rs1 == rs2 ) pc = immm4; break;
                                                case 0b001: if( rs1 != rs2 ) pc = immm4; break;
                                                case 0b100: if( rs1 < rs2 ) pc = immm4; break;
                                                case 0b101: if( rs1 >= rs2 ) pc = immm4; break; //BGE
                                                case 0b110: if( (uint32_t)rs1 < (uint32_t)rs2 ) pc = immm4; break;   //BLTU
                                                case 0b111: if( (uint32_t)rs1 >= (uint32_t)rs2 ) pc = immm4; break;  //BGEU
                                                default: trap = (2+1);
                                        }
                                        break;
                                }
                                case 0b0000011: // Load
                                {
                                        uint32_t rs1 = REG((ir >> 15) & 0x1f);
                                        uint32_t imm = ir >> 20;
                                        int32_t imm_se = imm | (( imm & 0x800 )?0xfffff000:0);
                                        uint32_t rsval = rs1 + imm_se;

                                        uint32_t paddr = MiniRV32IMATranslate( state, image, rsval, 1, &trap );
                                        if( trap )
                                        {
                                                rval = rsval; // Faulting address for mtval.
                                                break;
                                        }

                                        rsval = paddr - MINIRV32_RAM_IMAGE_OFFSET;
                                        if( rsval >= MINI_RV32_RAM_SIZE-3 )
                                        {
                                                rsval += MINIRV32_RAM_IMAGE_OFFSET;
                                                if( rsval >= 0x0C000000 && rsval < 0x12000000 )  // UART, CLNT, PLIC
                                                {
                                                        if( rsval == 0x1100bffc ) // https://chromitem-soc.readthedocs.io/en/latest/clint.html
                                                                rval = CSR( timerh );
                                                        else if( rsval == 0x1100bff8 )
                                                                rval = CSR( timerl );
                                                        else
                                                                MINIRV32_HANDLE_MEM_LOAD_CONTROL( rsval, rval );
                                                }
                                                else
                                                {
                                                        trap = (5+1);
                                                        rval = paddr;
                                                }
                                        }
                                        else
                                        {
                                                switch( ( ir >> 12 ) & 0x7 )
                                                {
                                                        //LB, LH, LW, LBU, LHU
                                                        case 0b000: rval = (int8_t)MINIRV32_LOAD1( rsval ); break;
                                                        case 0b001: rval = (int16_t)MINIRV32_LOAD2( rsval ); break;
                                                        case 0b010: rval = MINIRV32_LOAD4( rsval ); break;
                                                        case 0b100: rval = MINIRV32_LOAD1( rsval ); break;
                                                        case 0b101: rval = MINIRV32_LOAD2( rsval ); break;
                                                        default: trap = (2+1);
                                                }
                                        }
                                        break;
                                }
                                case 0b0100011: // Store
                                {
                                        uint32_t rs1 = REG((ir >> 15) & 0x1f);
                                        uint32_t rs2 = REG((ir >> 20) & 0x1f);
                                        uint32_t addy = ( ( ir >> 7 ) & 0x1f ) | ( ( ir & 0xfe000000 ) >> 20 );
                                        if( addy & 0x800 ) addy |= 0xfffff000;
                                        addy += rs1;
                                        rdid = 0;

                                        uint32_t paddr = MiniRV32IMATranslate( state, image, addy, 2, &trap );
                                        if( trap )
                                        {
                                                rval = addy; // Faulting address for mtval.
                                                break;
                                        }

                                        addy = paddr - MINIRV32_RAM_IMAGE_OFFSET;
                                        if( addy >= MINI_RV32_RAM_SIZE-3 )
                                        {
                                                addy += MINIRV32_RAM_IMAGE_OFFSET;
                                                if( addy >= 0x0C000000 && addy < 0x12000000 )
                                                {
                                                        // Should be stuff like SYSCON, 8250, CLNT
                                                        if( addy == 0x11004004 ) //CLNT
                                                                CSR( timermatchh ) = rs2;
                                                        else if( addy == 0x11004000 ) //CLNT
                                                                CSR( timermatchl ) = rs2;
                                                        else if( addy == 0x11100000 ) //SYSCON (reboot, poweroff, etc.)
                                                        {
                                                                SETCSR( pc, pc + 4 );
                                                                return rs2; // NOTE: PC will be PC of Syscon.
                                                        }
                                                        else
                                                                MINIRV32_HANDLE_MEM_STORE_CONTROL( addy, rs2 );
                                                }
                                                else
                                                {
                                                        trap = (7+1); // Store access fault.
                                                        rval = paddr;
                                                }
                                        }
                                        else
                                        {
                                                switch( ( ir >> 12 ) & 0x7 )
                                                {
                                                        //SB, SH, SW
                                                        case 0b000: MINIRV32_STORE1( addy, rs2 ); JIT_STORE_NOTIFY( addy ); break;
                                                        case 0b001: MINIRV32_STORE2( addy, rs2 ); JIT_STORE_NOTIFY( addy ); break;
                                                        case 0b010: MINIRV32_STORE4( addy, rs2 ); JIT_STORE_NOTIFY( addy ); break;
                                                        default: trap = (2+1);
                                                }
                                        }
                                        break;
                                }
                                case 0b0010011: // Op-immediate
                                case 0b0110011: // Op
                                {
                                        uint32_t imm = ir >> 20;
                                        imm = imm | (( imm & 0x800 )?0xfffff000:0);
                                        uint32_t rs1 = REG((ir >> 15) & 0x1f);
                                        uint32_t is_reg = !!( ir & 0b100000 );
                                        uint32_t rs2 = is_reg ? REG(imm & 0x1f) : imm;

                                        if( is_reg && ( ir & 0x02000000 ) )
                                        {
                                                switch( (ir>>12)&7 ) //0x02000000 = RV32M
                                                {
                                                        case 0b000: rval = rs1 * rs2; break; // MUL
                                                        case 0b001: rval = ((int64_t)((int32_t)rs1) * (int64_t)((int32_t)rs2)) >> 32; break; // MULH
                                                        case 0b010: rval = ((int64_t)((int32_t)rs1) * (uint64_t)rs2) >> 32; break; // MULHSU
                                                        case 0b011: rval = ((uint64_t)rs1 * (uint64_t)rs2) >> 32; break; // MULHU
                                                        case 0b100: if( rs2 == 0 ) rval = -1; else rval = ((int32_t)rs1 == INT32_MIN && (int32_t)rs2 == -1) ? rs1 : ((int32_t)rs1 / (int32_t)rs2); break; // DIV
                                                        case 0b101: if( rs2 == 0 ) rval = 0xffffffff; else rval = rs1 / rs2; break; // DIVU
                                                        case 0b110: if( rs2 == 0 ) rval = rs1; else rval = ((int32_t)rs1 == INT32_MIN && (int32_t)rs2 == -1) ? 0 : ((uint32_t)((int32_t)rs1 % (int32_t)rs2)); break; // REM
                                                        case 0b111: if( rs2 == 0 ) rval = rs1; else rval = rs1 % rs2; break; // REMU
                                                }
                                        }
                                        else
                                        {
                                                switch( (ir>>12)&7 ) // These could be either op-immediate or op commands.  Be careful.
                                                {
                                                        case 0b000: rval = (is_reg && (ir & 0x40000000) ) ? ( rs1 - rs2 ) : ( rs1 + rs2 ); break; 
                                                        case 0b001: rval = rs1 << (rs2 & 0x1F); break;
                                                        case 0b010: rval = (int32_t)rs1 < (int32_t)rs2; break;
                                                        case 0b011: rval = rs1 < rs2; break;
                                                        case 0b100: rval = rs1 ^ rs2; break;
                                                        case 0b101: rval = (ir & 0x40000000 ) ? ( ((int32_t)rs1) >> (rs2 & 0x1F) ) : ( rs1 >> (rs2 & 0x1F) ); break;
                                                        case 0b110: rval = rs1 | rs2; break;
                                                        case 0b111: rval = rs1 & rs2; break;
                                                }
                                        }
                                        break;
                                }
                                case 0b0001111:
                                        rdid = 0;   // fencetype = (ir >> 12) & 0b111; We ignore fences in this impl.
#ifdef CONFIG_EMU_JIT
                                        if( ( ( ir >> 12 ) & 7 ) == 1 )
                                                jit_code_fence(); // fence.i, code pages may have changed
#endif
                                        break;
                                case 0b1110011: // Zifencei+Zicsr
                                {
                                        uint32_t csrno = ir >> 20;
                                        int microop = ( ir >> 12 ) & 0b111;
                                        if( (microop & 3) ) // It's a Zicsr function.
                                        {
                                                int rs1imm = (ir >> 15) & 0x1f;
                                                uint32_t rs1 = REG(rs1imm);
                                                uint32_t writeval = rs1;

                                                // https://raw.githubusercontent.com/riscv/virtual-memory/main/specs/663-Svpbmt.pdf
                                                // Generally, support for Zicsr
                                                switch( csrno )
                                                {
                                                case 0x340: rval = CSR( mscratch ); break;
                                                case 0x305: rval = CSR( mtvec ); break;
                                                case 0x304: rval = CSR( mie ); break;
                                                case 0xC00: rval = cycle; break;
                                                case 0x344: rval = CSR( mip ); break;
                                                case 0x341: rval = CSR( mepc ); break;
                                                case 0x300: rval = CSR( mstatus ); break; //mstatus
                                                case 0x342: rval = CSR( mcause ); break;
                                                case 0x343: rval = CSR( mtval ); break;
                                                case 0x100: rval = CSR( mstatus ) & 0xc0122; break; //sstatus
                                                case 0x104: rval = CSR( sie ); break;
                                                case 0x105: rval = CSR( stvec ); break;
                                                case 0x106: break; //scounteren, no performance counters
                                                case 0x140: rval = CSR( sscratch ); break;
                                                case 0x141: rval = CSR( sepc ); break;
                                                case 0x142: rval = CSR( scause ); break;
                                                case 0x143: rval = CSR( stval ); break;
                                                case 0x144: rval = CSR( sip ); break;
                                                case 0x180: rval = CSR( satp ); break;
                                                case 0x302: rval = CSR( medeleg ); break;
                                                case 0x303: rval = CSR( mideleg ); break;
                                                case 0xC01: rval = CSR( timerl ); break; //time
                                                case 0xC81: rval = CSR( timerh ); break; //timeh
                                                case 0xf11: rval = 0xff0ff0ff; break; //mvendorid
                                                case 0x301: rval = MINIRV32_MISA_VALUE; break; //misa (XLEN=32, IMA+X)
                                                //case 0x3B0: rval = 0; break; //pmpaddr0
                                                //case 0x3a0: rval = 0; break; //pmpcfg0
                                                //case 0xf12: rval = 0x00000000; break; //marchid
                                                //case 0xf13: rval = 0x00000000; break; //mimpid
                                                //case 0xf14: rval = 0x00000000; break; //mhartid
                                                default:
                                                        MINIRV32_OTHERCSR_READ( csrno, rval );
                                                        break;
                                                }

                                                switch( microop )
                                                {
                                                        case 0b001: writeval = rs1; break;                      //CSRRW
                                                        case 0b010: writeval = rval | rs1; break;               //CSRRS
                                                        case 0b011: writeval = rval & ~rs1; break;              //CSRRC
                                                        case 0b101: writeval = rs1imm; break;                   //CSRRWI
                                                        case 0b110: writeval = rval | rs1imm; break;    //CSRRSI
                                                        case 0b111: writeval = rval & ~rs1imm; break;   //CSRRCI
                                                }

                                                switch( csrno )
                                                {
                                                case 0x340: SETCSR( mscratch, writeval ); break;
                                                case 0x305: SETCSR( mtvec, writeval ); break;
                                                case 0x304: SETCSR( mie, writeval ); break;
                                                case 0x344: SETCSR( mip, writeval ); break;
                                                case 0x341: SETCSR( mepc, writeval ); break;
                                                case 0x300: SETCSR( mstatus, writeval ); MiniRV32IMATLBFlush(); break; //mstatus
                                                case 0x342: SETCSR( mcause, writeval ); break;
                                                case 0x343: SETCSR( mtval, writeval ); break;
                                                case 0x100: //sstatus, only the S-mode view is writable
                                                        SETCSR( mstatus, ( CSR( mstatus ) & ~0xc0122 ) | ( writeval & 0xc0122 ) );
                                                        MiniRV32IMATLBFlush(); // SUM/MXR affect translations.
                                                        break;
                                                case 0x104: SETCSR( sie, writeval ); break;
                                                case 0x105: SETCSR( stvec, writeval ); break;
                                                case 0x106: break; //scounteren, WARL zero
                                                case 0x140: SETCSR( sscratch, writeval ); break;
                                                case 0x141: SETCSR( sepc, writeval ); break;
                                                case 0x142: SETCSR( scause, writeval ); break;
                                                case 0x143: SETCSR( stval, writeval ); break;
                                                case 0x144: SETCSR( sip, writeval ); break;
                                                case 0x180: SETCSR( satp, writeval ); MiniRV32IMATLBFlush(); break;
                                                case 0x301: break; //misa, WARL
                                                case 0x302: SETCSR( medeleg, writeval ); break;
                                                case 0x303: SETCSR( mideleg, writeval ); break;
                                                //case 0x3a0: break; //pmpcfg0
                                                //case 0x3B0: break; //pmpaddr0
                                                //case 0xf11: break; //mvendorid
                                                //case 0xf12: break; //marchid
                                                //case 0xf13: break; //mimpid
                                                //case 0xf14: break; //mhartid
                                                //case 0x301: break; //misa
                                                default:
                                                        MINIRV32_OTHERCSR_WRITE( csrno, writeval );
                                                        break;
                                                }
                                        }
                                        else if( microop == 0b000 ) // "SYSTEM"
                                        {
                                                rdid = 0;
                                                if( csrno == 0x105 ) //WFI (Wait for interrupts)
                                                {
                                                        CSR( mstatus ) |= 8;    //Enable interrupts
                                                        CSR( extraflags ) |= 4; //Infor environment we want to go to sleep.
                                                        SETCSR( pc, pc + 4 );
                                                        return 1;
                                                }
                                                else if( ( ( csrno & 0xff ) == 0x02 ) && csrno != 0x102 )  // MRET
                                                {
                                                        //https://raw.githubusercontent.com/riscv/virtual-memory/main/specs/663-Svpbmt.pdf
                                                        //Table 7.6. MRET then in mstatus/mstatush sets MPV=0, MPP=0, MIE=MPIE, and MPIE=1. La
                                                        // Should also update mstatus to reflect correct mode.
                                                        uint32_t startmstatus = CSR( mstatus );
                                                        uint32_t startextraflags = CSR( extraflags );
                                                        SETCSR( mstatus , (( startmstatus & 0x80) >> 4) | ((startextraflags&3) << 11) | 0x80 );
                                                        SETCSR( extraflags, (startextraflags & ~3) | ((startmstatus >> 11) & 3) );
                                                        pc = CSR( mepc ) -4;
                                                }
                                                else if( csrno == 0x102 ) // SRET
                                                {
                                                        // sstatus: SIE = SPIE, SPIE = 1, SPP = 0, privilege from SPP.
                                                        uint32_t startmstatus = CSR( mstatus );
                                                        uint32_t startextraflags = CSR( extraflags );
                                                        SETCSR( mstatus, ( startmstatus & ~0x122 ) | (( startmstatus & 0x20 ) >> 4) | 0x20 );
                                                        SETCSR( extraflags, ( startextraflags & ~3 ) | (( startmstatus >> 8 ) & 1) );
                                                        pc = CSR( sepc ) - 4;
                                                }
                                                else if( ( csrno & 0xfff8 ) == 0x120 ) // SFENCE.VMA, funct7 0x09 << 5
                                                {
                                                        MiniRV32IMATLBFlush();
                                                }
                                                else
                                                {
                                                        switch( csrno )
                                                        {
                                                        case 0: // ECALL
                                                                if( ( CSR( extraflags ) & 3 ) == 3 )
                                                                        trap = (11+1); // "Environment call from M-mode"
                                                                else if( ( CSR( extraflags ) & 3 ) == 1 )
                                                                {
                                                                        // S-mode ecall is an SBI call, no trap,
                                                                        // the firmware here answers it inline.
                                                                        MINIRV32_SBI_ECALL( state );
                                                                }
                                                                else
                                                                        trap = (8+1); // "Environment call from U-mode"
                                                                break;
                                                        case 1: trap = (3+1); break; // EBREAK 3 = "Breakpoint"
                                                        default: trap = (2+1); break; // Illegal opcode.
                                                        }
                                                }
                                        }
                                        else
                                                trap = (2+1);                           // Note micrrop 0b100 == undefined.
                                        break;
                                }
                                case 0b0101111: // RV32A
                                {
                                        uint32_t rs1 = REG((ir >> 15) & 0x1f);
                                        uint32_t rs2 = REG((ir >> 20) & 0x1f);
                                        uint32_t irmid = ( ir>>27 ) & 0x1f;

                                        uint32_t paddr = MiniRV32IMATranslate( state, image, rs1, 2, &trap );
                                        if( trap )
                                        {
                                                rval = rs1; // Faulting address for mtval.
                                                break;
                                        }

                                        rs1 = paddr - MINIRV32_RAM_IMAGE_OFFSET;

                                        // We don't implement load/store from UART or CLNT with RV32A here.

                                        if( rs1 >= MINI_RV32_RAM_SIZE-3 )
                                        {
                                                trap = (7+1); //Store/AMO access fault
                                                rval = paddr;
                                        }
                                        else
                                        {
                                                rval = MINIRV32_LOAD4( rs1 );

                                                // Referenced a little bit of https://github.com/franzflasch/riscv_em/blob/master/src/core/core.c
                                                uint32_t dowrite = 1;
                                                switch( irmid )
                                                {
                                                        case 0b00010: //LR.W
                                                                dowrite = 0;
                                                                CSR( extraflags ) = (CSR( extraflags ) & 0b111) | (rs1<<3);
                                                                break;
                                                        case 0b00011:  //SC.W (Make sure we have a slot, and, it's valid)
                                                                rval = ( CSR( extraflags ) >> 3 != ( rs1 & 0x1fffffff ) );  // Validate that our reservation slot is OK.
                                                                dowrite = !rval; // Only write if slot is valid.
                                                                break;
                                                        case 0b00001: break; //AMOSWAP.W
                                                        case 0b00000: rs2 += rval; break; //AMOADD.W
                                                        case 0b00100: rs2 ^= rval; break; //AMOXOR.W
                                                        case 0b01100: rs2 &= rval; break; //AMOAND.W
                                                        case 0b01000: rs2 |= rval; break; //AMOOR.W
                                                        case 0b10000: rs2 = ((int32_t)rs2<(int32_t)rval)?rs2:rval; break; //AMOMIN.W
                                                        case 0b10100: rs2 = ((int32_t)rs2>(int32_t)rval)?rs2:rval; break; //AMOMAX.W
                                                        case 0b11000: rs2 = (rs2<rval)?rs2:rval; break; //AMOMINU.W
                                                        case 0b11100: rs2 = (rs2>rval)?rs2:rval; break; //AMOMAXU.W
                                                        default: trap = (2+1); dowrite = 0; break; //Not supported.
                                                }
                                                if( dowrite ) { MINIRV32_STORE4( rs1, rs2 ); JIT_STORE_NOTIFY( rs1 ); }
                                        }
                                        break;
                                }
                                default: trap = (2+1); // Fault: Invalid opcode.
                        }

                        // If there was a trap, do NOT allow register writeback.
                        if( trap )
                                break;

                        if( rdid )
                        {
                                REGSET( rdid, rval ); // Write back register.
                        }
                }

                MINIRV32_POSTEXEC( pc, ir, trap );

                pc += 4;
        }

        // Handle traps and interrupts.
        if( trap )
        {
                uint32_t cause = ( trap & 0x80000000 ) ? trap : ( trap - 1 );
                uint32_t delegated = 0;

                // Delegate to S-mode when the trap is covered by medeleg/mideleg
                // and we aren't already in M-mode.  STIP/SEIP are only injected
                // when delegated, MTIP never is.
                if( ( CSR( extraflags ) & 3 ) != 3 )
                {
                        if( trap & 0x80000000 )
                                delegated = ( CSR( mideleg ) >> ( trap & 0x1f ) ) & 1;
                        else if( cause < 32 )
                                delegated = ( CSR( medeleg ) >> cause ) & 1;
                }

                if( delegated )
                {
#ifdef JIT_DEBUG_TRACE
                        if( CSR( stvec ) == 0 )
                                printf( "[DELEGTRAP cause=%08x pc=%08x stvec=0 stval=%08x]\n", cause, pc, ( trap & 0x80000000 ) ? 0 : ( ( ( trap > 5 && trap <= 8 ) || ( trap >= 14 && trap <= 16 ) ) ? rval : pc ) );
#endif
                        // Trap into S-mode.
                        uint32_t startmstatus = CSR( mstatus );
                        uint32_t startextraflags = CSR( extraflags );
                        SETCSR( sepc, pc + ( ( trap & 0x80000000 ) ? 4 : 0 ) );
                        SETCSR( scause, cause );
                        SETCSR( stval, ( trap & 0x80000000 ) ? 0 : ( ( ( trap > 5 && trap <= 8 ) || ( trap >= 14 && trap <= 16 ) ) ? rval : pc ) );
                        SETCSR( mstatus, ( startmstatus & ~0x122 ) | (( startextraflags & 1 ) << 8) | ((( startmstatus & 0x2 ) != 0) << 5) );
                        SETCSR( extraflags, ( startextraflags & ~3 ) | 1 );
                        pc = (CSR( stvec ) - 4);

                        trap = 0;
                        pc += 4;
                }
                else
                {
                        if( trap & 0x80000000 ) // If prefixed with 1 in MSB, it's an interrupt, not a trap.
                        {
                                SETCSR( mcause, trap );
                                SETCSR( mtval, 0 );
                                pc += 4; // PC needs to point to where the PC will return to.
                        }
                        else
                        {
                                SETCSR( mcause,  trap - 1 );
                                SETCSR( mtval, (trap > 5 && trap <= 8)? rval : pc );
                        }
                        SETCSR( mepc, pc ); //TRICKY: The kernel advances mepc automatically.
                        //CSR( mstatus ) & 8 = MIE, & 0x80 = MPIE
                        // On an interrupt, the system moves current MIE into MPIE
                        SETCSR( mstatus, (( CSR( mstatus ) & 0x08) << 4) | (( CSR( extraflags ) & 3 ) << 11) );
                        pc = (CSR( mtvec ) - 4);

                        // If trapping, always enter machine mode.
                        CSR( extraflags ) |= 3;

                        trap = 0;
                        pc += 4;
                }
        }

        if( CSR( cyclel ) > cycle ) CSR( cycleh )++;
        SETCSR( cyclel, cycle );
        SETCSR( pc, pc );
        return 0;
}

#endif

#endif


