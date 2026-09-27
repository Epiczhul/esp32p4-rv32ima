#!/usr/bin/env python3
# Builds main/test-sv32.bin, the S-mode guest self test (make test in host/)
import struct
import os
# Builds the S-mode guest test binary (main/test-sv32.bin) for the host run.
# Exercises: bare-mode uart, Sv32 4KB walk, superpage store/fetch, AMO under
# translation, page fault delegation, SBI base/TIME/legacy putchar, STIP wake
# from WFI, U-mode delegation and U-permission translations.
#
# Checkpoint letters printed over the uart: O A B C M D E S T V W U K DONE

zero, ra, sp, gp, tp = 0, 1, 2, 3, 4
t0, t1, t2, t3, t4, t5, t6 = 5, 6, 7, 28, 29, 30, 31
a0, a1, a6, a7 = 10, 11, 16, 17

def r_type(op, rd, f3, rs1, rs2, f7=0):
    return (f7 << 25) | (rs2 << 20) | (rs1 << 15) | (f3 << 12) | (rd << 7) | op

def i_type(op, rd, f3, rs1, imm):
    imm &= 0xFFF
    return (imm << 20) | (rs1 << 15) | (f3 << 12) | (rd << 7) | op

def s_type(op, f3, rs1, rs2, imm):
    imm &= 0xFFF
    return ((imm >> 5) << 25) | (rs2 << 20) | (rs1 << 15) | (f3 << 12) | ((imm & 0x1F) << 7) | op

def b_type(f3, rs1, rs2, off):
    off &= 0x1FFF
    return (((off >> 12) & 1) << 31) | (((off >> 5) & 0x3F) << 25) | (rs2 << 20) | \
           (rs1 << 15) | (f3 << 12) | (((off >> 1) & 0xF) << 8) | (((off >> 11) & 1) << 7) | 0x63

def u_type(op, rd, imm20):
    return ((imm20 & 0xFFFFF) << 12) | (rd << 7) | op

def j_type(rd, off):
    off &= 0x1FFFFF
    return (((off >> 20) & 1) << 31) | (((off >> 1) & 0x3FF) << 21) | \
           (((off >> 11) & 1) << 20) | (((off >> 12) & 0xFF) << 12) | (rd << 7) | 0x6F

def lui(rd, imm20): return u_type(0x37, rd, imm20)
def addi(rd, rs1, imm): return i_type(0x13, rd, 0, rs1, imm)
def sw(rs2, rs1, imm): return s_type(0x23, 2, rs1, rs2, imm)
def lw(rd, rs1, imm): return i_type(0x03, rd, 2, rs1, imm)
def jal(rd, off): return j_type(rd, off)
def jalr(rd, rs1, imm): return i_type(0x67, rd, 0, rs1, imm)
def ecall(): return 0x73
def csrrw(rd, csr, rs1): return i_type(0x73, rd, 1, rs1, csr)
def csrrs(rd, csr, rs1): return i_type(0x73, rd, 2, rs1, csr)
def csrrc(rd, csr, rs1): return i_type(0x73, rd, 3, rs1, csr)
def csrw(csr, rs1): return csrrw(zero, csr, rs1)
def csrs(csr, rs1): return csrrs(zero, csr, rs1)
def csrc(csr, rs1): return csrrc(zero, csr, rs1)
def csrr(rd, csr): return csrrs(rd, csr, zero)
def sret(): return 0x10200073
def wfi(): return 0x10500073
def sfence(): return 0x12000073
def amoadd(rd, rs2, rs1): return r_type(0x2F, rd, 2, rs1, rs2, 0b00000)
def add(rd, rs1, rs2): return r_type(0x33, rd, 0, rs1, rs2)

def li32(dst, val):
    val &= 0xFFFFFFFF
    upper = (val + 0x800) >> 12
    lower = val - (upper << 12)
    return [lui(dst, upper & 0xFFFFF), addi(dst, dst, lower & 0xFFF)]

UART = 0x10000000

insns = []    # items: int word, or tuple ('jal', rd, label), ('b', f3, rs1, rs2, label)
labels = {'fail': 10**9}  # patched when the fail label is defined


def E(*items):
    insns.extend(items)

def L(name):
    labels[name] = len(insns)

NOP = addi(zero, zero, 0)

def _drift():
    # far branches to fail expand inline with one extra word each
    d = 0
    for i, item in enumerate(insns):
        if isinstance(item, tuple) and item[0] == 'b' and \
           abs((labels.get(item[4], i) - i) * 4) > 0xE00:
            d += 4
    return d

def pad_to(addr):
    while len(insns) * 4 + _drift() < addr:
        insns.append(NOP)

def putchar(ch):
    E(addi(a7, zero, 1), addi(a0, zero, ch), ecall())

def sbi_time(when):
    E(*li32(a7, 0x54494D45))
    E(*li32(a0, when))
    E(addi(a1, zero, 0), addi(a6, zero, 0), ecall())

# ------------------------------------------------------------------ main
L('start')
E(lui(t1, UART >> 12))
E(addi(t0, zero, ord('O')), sw(t0, t1, 0))

# markers
E(*li32(t2, 0x80300000))
E(*li32(t3, 0x12345678)); E(sw(t3, t2, 0))
E(addi(t4, zero, 100)); E(sw(t4, t2, 4))
E(*li32(t2, 0x80301000))
E(*li32(t3, 0x87654321)); E(sw(t3, t2, 0))

# routine words stored at 0x80500000, run via 0xC0100000
#   lui t1,0x10000; addi t0,'C'; sw t0,0(t1); ret
E(*li32(t2, 0x80500000))
E(*li32(t3, lui(t1, UART >> 12))); E(sw(t3, t2, 0))
E(*li32(t3, addi(t0, zero, ord('C')))); E(sw(t3, t2, 4))
E(*li32(t3, s_type(0x23, 2, t1, t0, 0))); E(sw(t3, t2, 8))
E(*li32(t3, jalr(zero, ra, 0))); E(sw(t3, t2, 12))

# root table at 0x80010000, one pointer register per store group
# (sw immediates must stay below 0x800)
#   root[0x040] uart identity, U flag for the U-mode print
#   root[0x044] syscon identity, U flag, final shutdown store is from S
#   root[0x100] pointer to L2
#   root[0x200] 0x80000000 identity 4MB, RWXAD, no U (S-mode fetches here)
#   root[0x300] 0xC0000000 -> 0x80400000, RWXAD, no U
E(*li32(t2, 0x80010100))
E(*li32(t3, 0x040000DF)); E(sw(t3, t2, 0))       # root[0x040]
E(*li32(t3, 0x044000DF)); E(sw(t3, t2, 0x10))    # root[0x044]
E(*li32(t3, 0x20008001)); E(sw(t3, t2, 0x300))   # root[0x100]
E(*li32(t2, 0x80010800))
E(*li32(t3, 0x200000CF)); E(sw(t3, t2, 0))       # root[0x200] identity 0x80000000
E(*li32(t3, 0x201000CF)); E(sw(t3, t2, 4))       # root[0x201] identity 0x80400000
E(*li32(t2, 0x80010C00))
E(*li32(t3, 0x201000CF)); E(sw(t3, t2, 0))       # root[0x300]
# L2 at 0x80020000
E(*li32(t2, 0x80020000))
E(*li32(t3, 0x200C00DF)); E(sw(t3, t2, 0))       # 0x40000000 -> 0x80300000 (S only, no U)
E(*li32(t3, 0x200C04DF)); E(sw(t3, t2, 4))       # 0x40001000 -> 0x80301000 (U)
# root[0x202] -> L2b, one U page so U-mode can run at 0x80801400
E(*li32(t2, 0x80010808))
E(*li32(t3, 0x20008401)); E(sw(t3, t2, 0))       # root[0x202] -> L2b @ 0x80021000
E(*li32(t2, 0x80021000))
E(*li32(t3, 0x200004DF)); E(sw(t3, t2, 4))       # 0x80801400 -> 0x80001400 (U RWX)

# stvec, satp
E(*li32(t0, 0x80001000)); E(csrw(0x105, t0))
E(*li32(t0, 0x80080010)); E(csrw(0x180, t0))   # Sv32, root PPN = 0x80010
E(sfence())
E(*li32(t0, 0x40000), csrs(0x100, t0))   # SUM, S-mode reaches U-flagged data pages

# A: load through the 4KB walk
E(*li32(t2, 0x40000000), lw(t3, t2, 0))
E(*li32(t4, 0x12345678))
E(('b', 1, t3, t4, 'fail'))
E(addi(t0, zero, ord('A')), sw(t0, t1, 0))

# B: store through the superpage, read back identity
E(*li32(t2, 0xC0000000), *li32(t3, 0xCAFEF00D), sw(t3, t2, 0))
E(*li32(t2, 0x80400000), lw(t4, t2, 0))
E(('b', 1, t3, t4, 'fail'))
E(addi(t0, zero, ord('B')), sw(t0, t1, 0))

# C: fetch through the translated superpage (routine prints C itself)
E(*li32(t1, 0xC0100000), ('jal', ra, 'retc'), jalr(zero, t1, 0))
L('retc')
E(lui(t1, UART >> 12))

# M: AMOADD through the 4KB walk, at the +4 marker
E(*li32(t2, 0x40000004))
E(addi(t3, zero, 5))
E(amoadd(t4, t3, t2))
E(*li32(t5, 100))
E(('b', 1, t4, t5, 'fail'))
E(addi(t0, zero, ord('M')), sw(t0, t1, 0))

# D: unmapped load -> delegated page fault, handler advances sepc
E(*li32(t2, 0x50000000), lw(t3, t2, 0))
# E: fault counter
E(*li32(t2, 0x80011000), lw(t4, t2, 0))
E(addi(t5, zero, 1))
E(('b', 1, t4, t5, 'fail'))
E(addi(t0, zero, ord('E')), sw(t0, t1, 0))

# S: SBI legacy putchar
putchar(ord('S'))

# T: SBI TIME accepts a set_timer
sbi_time(0x00FFFFFF)
E(('b', 1, a0, zero, 'fail'))
E(addi(t0, zero, ord('T')), sw(t0, t1, 0))

# V: SBI base spec version
E(*li32(a7, 0x10), addi(a6, zero, 0), ecall())
E(addi(t5, zero, 2))
E(('b', 1, a1, t5, 'fail'))
E(('b', 1, a0, zero, 'fail'))
E(addi(t0, zero, ord('V')), sw(t0, t1, 0))

# W: arm a timer ~50ms out, enable SIE/STIE, wfi, STIP must wake us
E(csrr(t2, 0xC01))
E(*li32(t3, 50000), add(a0, t2, t3))
E(addi(a1, zero, 0), addi(a6, zero, 0))
E(*li32(a7, 0x54494D45), ecall())
E(addi(t0, zero, 0x20), csrs(0x104, t0))         # sie.STIE
E(addi(t0, zero, 0x2), csrs(0x100, t0))          # sstatus.SIE
E(wfi())
E(*li32(t2, 0x80011000), lw(t4, t2, 4))
E(addi(t5, zero, 1))
E(('b', 1, t4, t5, 'fail'))
E(addi(t0, zero, ord('W')), sw(t0, t1, 0))
E(('jal', zero, 'umode_setup'))

# failure path: F then hang, only reached via branches
L('fail')
E(addi(t0, zero, ord('F')), sw(t0, t1, 0))
E(*li32(t3, 0xBAD0BAD0))                          # marker for debugging
E(jal(zero, 0) if False else 0)
insns[-1] = jal(zero, -4)

# U-mode: sepc = umode (via the U page), clear SPP, sret
L('umode_setup')
E(*li32(t0, 0x80801400), csrw(0x141, t0))
E(*li32(t1, 0x100), csrc(0x100, t1))
E(lui(t1, UART >> 12))
E(sret())

# ------------------------------------------------------------------ handler @ 0x80001000
pad_to(0x1000)
L('handler')
E(csrr(t6, 0x142))                                # scause
# interrupt? bit31 set
E(('b', 4, zero, t6, 'exception'))                # blt zero, t6 -> exception when positive
# STIP = 0x80000005
E(*li32(t5, 0x80000005))
E(('b', 0, t6, t5, 'timer_int'))
E(('jal', zero, 'hang'))

L('timer_int')
E(*li32(t2, 0x80011000))
E(lw(t3, t2, 4), addi(t3, t3, 1), sw(t3, t2, 4))
# re-arm +2s so we don't loop
E(csrr(t2, 0xC01))
E(*li32(t3, 2000000), add(a0, t2, t3))
E(addi(a1, zero, 0), addi(a6, zero, 0))
E(*li32(a7, 0x54494D45), ecall())
E(sret())

L('exception')
E(addi(t5, zero, 13))
E(('b', 0, t6, t5, 'pf_load'))
E(addi(t5, zero, 8))
E(('b', 0, t6, t5, 'ucall'))
E(('jal', zero, 'hang'))

L('pf_load')
E(*li32(t2, 0x80011000))
E(lw(t3, t2, 0), addi(t3, t3, 1), sw(t3, t2, 0))
E(csrr(t3, 0x141), addi(t3, t3, 4), csrw(0x141, t3))
E(sret())

L('ucall')
E(*li32(t2, 0x80011000))
E(lw(t3, t2, 8), addi(t3, t3, 1), sw(t3, t2, 8))
E(addi(t5, zero, 1))
E(('b', 0, t3, t5, 'ufirst'))
# second ecall: SPP = 1 so sret lands back in S-mode, resume at sresume
E(*li32(t3, 0x80001600))
E(csrw(0x141, t3))
E(addi(t3, zero, 0x100))
E(csrs(0x100, t3))                                # sstatus.SPP
E(sret())

L('ufirst')
E(lui(t1, UART >> 12))
E(addi(t0, zero, ord('U')), sw(t0, t1, 0))
E(csrr(t3, 0x141), addi(t3, t3, 4), csrw(0x141, t3))
E(sret())

# ---- runs in U-mode ----
pad_to(0x1400)
L('umode')
# K: U-mode load of a U page
E(*li32(t2, 0x40001000), lw(t3, t2, 0))
E(*li32(t4, 0x87654321))
E(('b', 1, t3, t4, 'fail2'))
E(addi(t0, zero, ord('K')), sw(t0, t1, 0))
# U: ecall from U-mode, delegated to S
E(ecall())
E(*li32(t2, 0x80400004))                          # bss zero, signals first ecall done
# second ecall makes the handler resume S-mode at sresume
E(ecall())

L('fail2')
E(addi(t0, zero, ord('F')), sw(t0, t1, 0))
E(('b', 0, zero, zero, 'fail2'))

# ---- back in S-mode ----
pad_to(0x1600)
L('sresume')
E(addi(t0, zero, ord('D')), sw(t0, t1, 0))
E(addi(t0, zero, ord('O')), sw(t0, t1, 0))
E(addi(t0, zero, ord('N')), sw(t0, t1, 0))
E(addi(t0, zero, ord('E')), sw(t0, t1, 0))
E(addi(t0, zero, 10), sw(t0, t1, 0))
# shutdown through syscon
E(*li32(t2, 0x11100000))
E(*li32(t3, 0x5555))
E(sw(t3, t2, 0))
L('hang')
E(jal(zero, 0))
E(jal(zero, 0))


# ------------------------------------------------------------------ resolve
# far branches expand to [inverted branch; jal trampoline], which shifts
# everything after them, so addresses are computed with the drift included
need = {}
for i, item in enumerate(insns):
    if isinstance(item, tuple) and item[0] == 'b':
        if abs((labels[item[4]] - i) * 4) > 0xE00:
            need[i] = True

addr = []
a = 0
for i in range(len(insns)):
    addr.append(a)
    a += 8 if need.get(i) else 4
label_addr = {name: addr[idx] for name, idx in labels.items()}

assert addr[labels['handler']] == 0x1000, hex(addr[labels['handler']])
assert addr[labels['umode']] == 0x1400, hex(addr[labels['umode']])
assert addr[labels['sresume']] == 0x1600, hex(addr[labels['sresume']])

out = bytearray()
for i, item in enumerate(insns):
    pc = addr[i]
    if isinstance(item, int):
        out += struct.pack('<I', item)
        continue
    kind = item[0]
    if kind == 'jal':
        _, rd, name = item
        out += struct.pack('<I', jal(rd, label_addr[name] - pc))
    elif kind == 'b':
        _, f3, rs1, rs2, name = item
        off = label_addr[name] - pc
        if -0xE00 <= off <= 0xE00:
            out += struct.pack('<I', b_type(f3, rs1, rs2, off))
        else:
            inv = {0: 1, 1: 0, 4: 5, 5: 4}[f3]
            out += struct.pack('<I', b_type(inv, rs1, rs2, 8))
            out += struct.pack('<I', jal(zero, label_addr[name] - (pc + 4)))
    else:
        raise ValueError(item)

out_path = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'main', 'test-sv32.bin')
open(out_path, 'wb').write(out)
print(f"test-sv32.bin written, {len(out)} bytes, {len(insns)} insns")
