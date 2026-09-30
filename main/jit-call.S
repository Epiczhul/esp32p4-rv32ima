/*
 * Native call glue for the block JIT.  A plain function with standard
 * calling conventions: a0 carries the argument block, the callee saves
 * every register the translated code can trash, sets up the JIT register
 * roles and jumps into the block.  The block comes back through x28,
 * which this stub aims at the restore path with an auipc so it survives
 * being linked anywhere.
 */

    .text
    .globl jit_call_entry
jit_call_entry:
    addi    sp, sp, -64
    sw      ra, 60(sp)
    sw      x8, 56(sp)
    sw      x9, 52(sp)
    sw      x18, 48(sp)
    sw      x19, 44(sp)
    sw      x20, 40(sp)
    sw      x21, 36(sp)
    sw      x22, 32(sp)
    sw      x23, 28(sp)
    sw      x24, 24(sp)
    sw      x25, 20(sp)
    sw      x26, 16(sp)
    sw      x27, 12(sp)

    auipc   x28, 0              # x28 = address of this auipc
    addi    x28, x28, 52        # the block returns to the restore path

    lw      x5, 0(a0)           # block entry address
    lw      x6, 4(a0)           # guest state pointer
    lw      x7, 8(a0)           # load row of the inline tlb
    lw      x1, 12(a0)          # store row of the inline tlb
    lw      x22, 16(a0)         # loop budget
    mv      x31, x6
    mv      x25, x7
    mv      x27, x1
    mv      x23, x22
    fence   rw, rw
    jalr    x0, 0(x5)           # into the translated block

    lw      x8, 56(sp)
    lw      x9, 52(sp)
    lw      x18, 48(sp)
    lw      x19, 44(sp)
    lw      x20, 40(sp)
    lw      x21, 36(sp)
    lw      x22, 32(sp)
    lw      x23, 28(sp)
    lw      x24, 24(sp)
    lw      x25, 20(sp)
    lw      x26, 16(sp)
    lw      x27, 12(sp)
    lw      ra, 60(sp)
    addi    sp, sp, 64
    ret
