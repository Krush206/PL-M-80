#!/usr/bin/env python3
"""Focused Z80 + stub-SMS runner for the PL/M-80 compiler's code subset.

Purpose: actually execute a compiled ROM and detect hangs / wild jumps
that a static read can miss.  Implements the 8080->Z80 instruction subset
the PL/M-80 compiler emits, plus the common extras: all eight conditional
JP/CALL/RET, PUSH/POP AF, JP (HL), EX (SP),HL, LD SP,HL, RLCA/RRCA/RLA/RRA,
CPL/CCF/SCF/DAA, DJNZ/JR and LDIR.  Unknown opcodes abort loudly.

Usage: z80run.py [maxsteps] [mode] [rom path]
  defaults: 40000000, 'fire', /opt/mygame/river.sms

VDP/PAD/PSG ports are stubbed: status always reads VBlank-set (0x80) so
VDPWAITBLANK never blocks, pads read 'nothing pressed' (0xFF), PSG writes
are counted.  RAM is 8K mirrored at $C000-$FFFF.
"""
import sys

ROM_PATH = sys.argv[3] if len(sys.argv) > 3 else '/opt/mygame/river.sms'
ROM = bytearray(open(ROM_PATH, 'rb').read())
RAM = bytearray(0x2000)
MODE = sys.argv[2] if len(sys.argv) > 2 else 'fire'

# --- registers ---------------------------------------------------------
a = b = c = d = e = h = l = f = 0
pc = sp = 0
halted = False

# flag bit masks
FS, FZ, FH, FPV, FN, FC_ = 0x80, 0x40, 0x10, 0x04, 0x02, 0x01

PARITY = [bin(i).count('1') % 2 == 0 for i in range(256)]

rom_writes = []
vdp_writes = [0]
psg_writes = [0]
in_reads = {}


def rd(addr):
    addr &= 0xFFFF
    if addr >= 0xC000:
        return RAM[addr & 0x1FFF]
    if addr < len(ROM):
        return ROM[addr]
    return 0xFF


def wr(addr, val):
    addr &= 0xFFFF
    val &= 0xFF
    if addr >= 0xC000:
        RAM[addr & 0x1FFF] = val
    else:
        rom_writes.append((pc, addr, val))


def rd16(addr):
    return rd(addr) | (rd(addr + 1) << 8)


def wr16(addr, val):
    wr(addr, val & 0xFF)
    wr(addr + 1, (val >> 8) & 0xFF)


def push(val):
    global sp
    sp = (sp - 2) & 0xFFFF
    wr16(sp, val)


def pop():
    global sp
    v = rd16(sp)
    sp = (sp + 2) & 0xFFFF
    return v


def in_port(n):
    in_reads[n] = in_reads.get(n, 0) + 1
    if n == 0xBF:
        return 0x80          # VBlank flag always set -> no waiting
    if n in (0xDC, 0xDD):
        # active-low pad: 0 = pressed.  Hold Btn1 with a periodic release
        # (gives PADJUST1 a press edge every 30 frames -> restarts after
        # game over), and cycle the d-pad to exercise collision paths.
        fc = rd16(FC_ADDR)
        v = 0xFF
        if MODE == 'idle':
            if fc % 30 == 0:
                v &= ~0x10       # only presses Btn1 briefly -> sits & dies
        else:
            if fc % 30 != 0:
                v &= ~0x10       # Btn1 (fire / restart)
            m = (fc // 90) % 5
            if m == 1: v &= ~0x01    # up
            elif m == 2: v &= ~0x04  # left
            elif m == 3: v &= ~0x08  # right
            elif m == 4: v &= ~0x02  # down
        return v
    return 0xFF


def out_port(n, val):
    if n in (0xBE, 0xBF):
        vdp_writes[0] += 1
    elif n == 0x7F:
        psg_writes[0] += 1


def sz(v):
    return (FS if v & 0x80 else 0) | (FZ if v == 0 else 0)


def add8(x, y, carry=0):
    global f
    r = x + y + carry
    v = r & 0xFF
    f = sz(v)
    if r & 0x100:
        f |= FC_
    if ((x ^ y ^ v) & 0x10):
        f |= FH
    if (~(x ^ y) & (x ^ v) & 0x80):
        f |= FPV
    return v


def sub8(x, y, carry=0):
    global f
    r = x - y - carry
    v = r & 0xFF
    f = FN | sz(v)
    if r & 0x100:
        f |= FC_
    if ((x ^ y ^ v) & 0x10):
        f |= FH
    if ((x ^ y) & (x ^ v) & 0x80):
        f |= FPV
    return v


def logic8(v):
    global f
    f = sz(v) | (FPV if PARITY[v] else 0)
    return v


def inc8(v):
    global f
    r = (v + 1) & 0xFF
    f = (f & FC_) | sz(r)
    if (r & 0x0F) == 0:
        f |= FH
    if r == 0x80:
        f |= FPV
    return r


def dec8(v):
    global f
    r = (v - 1) & 0xFF
    f = (f & FC_) | FN | sz(r)
    if (v & 0x0F) == 0:
        f |= FH
    if r == 0x7F:
        f |= FPV
    return r


RP = [lambda: (b << 8) | c, lambda: (d << 8) | e,
      lambda: (h << 8) | l, lambda: sp]


def setrp(i, v):
    global b, c, d, e, h, l, sp
    v &= 0xFFFF
    if i == 0: b, c = v >> 8, v & 0xFF
    elif i == 1: d, e = v >> 8, v & 0xFF
    elif i == 2: h, l = v >> 8, v & 0xFF
    else: sp = v


def getr(i):
    return (b, c, d, e, h, l, rd((h << 8) | l), a)[i]


def setr(i, v):
    global a, b, c, d, e, h, l
    v &= 0xFF
    if i == 0: b = v
    elif i == 1: c = v
    elif i == 2: d = v
    elif i == 3: e = v
    elif i == 4: h = v
    elif i == 5: l = v
    elif i == 6: wr((h << 8) | l, v)
    else: a = v


COND = {
    0: lambda: not (f & FZ), 1: lambda: bool(f & FZ),
    2: lambda: not (f & FC_), 3: lambda: bool(f & FC_),
    4: lambda: not (f & FPV), 5: lambda: bool(f & FPV),
    6: lambda: not (f & FS), 7: lambda: bool(f & FS),
}

steps = 0
MAXSTEPS = int(sys.argv[1]) if len(sys.argv) > 1 else 40_000_000
FC_ADDR = 0xC33C          # RIVER_FC (ADDRESS)
last_fc = -1
max_frame = 0
last_fc_change_step = 0
STALL = 4_000_000
stalled = False
last_state = -1
game_overs = 0
restarts = 0


def check_progress():
    global last_fc, max_frame, last_fc_change_step, last_state, game_overs, restarts
    fc = rd16(FC_ADDR)
    if fc != last_fc:
        last_fc = fc
        last_fc_change_step = steps
        if fc > max_frame:
            max_frame = fc
    st = rd(0xC359)
    if st != last_state:
        if st == 1:
            game_overs += 1
        elif st == 0 and last_state == 1:
            restarts += 1
        last_state = st
    return fc


print('running budget', MAXSTEPS, 'instructions')
while steps < MAXSTEPS:
    steps += 1
    op = rd(pc)
    pc = (pc + 1) & 0xFFFF
    if op < 0x40:
        if op == 0x00:
            pass
        elif (op & 0x0F) == 0x01:
            setrp((op >> 4) & 3, rd16(pc)); pc += 2
        elif (op & 0x0F) == 0x03:
            i = (op >> 4) & 3; setrp(i, RP[i]() + 1)
        elif (op & 0x0F) == 0x0B:
            i = (op >> 4) & 3; setrp(i, RP[i]() - 1)
        elif (op & 0x0F) == 0x09:
            i = (op >> 4) & 3
            x = (h << 8) | l; y = RP[i]()
            r = x + y
            global_h = ((x ^ y ^ r) & 0x1000)
            f = (f & (FS | FZ | FPV)) | ((FH if global_h else 0)) | (FC_ if r & 0x10000 else 0)
            setrp(2, r & 0xFFFF)
        elif (op & 0x07) == 0x04:
            i = (op >> 3) & 7; setr(i, inc8(getr(i)))
        elif (op & 0x07) == 0x05:
            i = (op >> 3) & 7; setr(i, dec8(getr(i)))
        elif (op & 0x07) == 0x06:
            i = (op >> 3) & 7; setr(i, rd(pc)); pc += 1
        elif op in (0x02, 0x12, 0x32):
            if op == 0x02: wr((b << 8) | c, a)
            elif op == 0x12: wr((d << 8) | e, a)
            else:
                wr(rd16(pc), a); pc += 2
        elif op in (0x0A, 0x1A, 0x3A):
            if op == 0x0A: a = rd((b << 8) | c)
            elif op == 0x1A: a = rd((d << 8) | e)
            else:
                a = rd(rd16(pc)); pc += 2
        elif op == 0x22:
            wr16(rd16(pc), (h << 8) | l); pc += 2
        elif op == 0x2A:
            v = rd16(rd16(pc)); pc += 2; h, l = v >> 8, v & 0xFF
        elif op == 0x07:          # RLCA
            nc = (a >> 7) & 1
            a = ((a << 1) | nc) & 0xFF
            f = (f & (FS | FZ | FPV)) | (FC_ if nc else 0)
        elif op == 0x0F:          # RRCA
            nc = a & 1
            a = ((a >> 1) | (a << 7)) & 0xFF
            f = (f & (FS | FZ | FPV)) | (FC_ if nc else 0)
        elif op == 0x17:          # RLA
            nc = (a >> 7) & 1
            a = ((a << 1) | (1 if f & FC_ else 0)) & 0xFF
            f = (f & (FS | FZ | FPV)) | (FC_ if nc else 0)
        elif op == 0x1F:          # RRA
            nc = a & 1
            a = ((a >> 1) | (0x80 if f & FC_ else 0)) & 0xFF
            f = (f & (FS | FZ | FPV)) | (FC_ if nc else 0)
        elif op == 0x27:          # DAA
            t = a
            c_old = 1 if f & FC_ else 0
            n_old = 1 if f & FN else 0
            d = 0
            if (t & 0x0F) > 9 or f & FH:
                d |= 0x06
            if t > 0x99 or c_old:
                d |= 0x60
            if n_old:
                r = (t - d) & 0xFF
                half = (t & 0x0F) < (d & 0x0F)
            else:
                r = (t + d) & 0xFF
                half = ((t & 0x0F) + (d & 0x0F)) > 0x0F
            a = r
            f = sz(r) | (FPV if PARITY[r] else 0)
            if n_old:
                f |= FN
            if t > 0x99 or c_old:
                f |= FC_
            if half:
                f |= FH
        elif op == 0x2F:          # CPL
            a ^= 0xFF
            f = (f & (FS | FZ | FC_ | FPV)) | FH | FN
        elif op == 0x37:          # SCF
            f = (f & (FS | FZ | FPV)) | FC_
        elif op == 0x3F:          # CCF
            f = (f & (FS | FZ | FPV)) | (FH if f & FC_ else 0) \
                | (0 if f & FC_ else FC_)
        elif op == 0x10:          # DJNZ
            b = (b - 1) & 0xFF
            off = rd(pc); pc = (pc + 1) & 0xFFFF
            if b != 0:
                pc = (pc + (off - 256 if off > 127 else off)) & 0xFFFF
        elif op in (0x18, 0x20, 0x28, 0x30, 0x38):    # JR cc
            off = rd(pc); pc = (pc + 1) & 0xFFFF
            if op == 0x18 or COND[{0x20: 0, 0x28: 1, 0x30: 2, 0x38: 3}[op]]():
                pc = (pc + (off - 256 if off > 127 else off)) & 0xFFFF
        else:
            print('UNKNOWN opcode %02X at %04X' % (op, pc - 1)); break
    elif op < 0x80:
        if op == 0x76:
            print('HALT reached at %04X (steps %d)' % (pc - 1, steps)); break
        setr((op >> 3) & 7, getr(op & 7))
    elif op < 0xC0:
        grp = (op >> 3) & 7
        v = getr(op & 7)
        if grp == 0: a = add8(a, v)
        elif grp == 1: a = add8(a, v, 1 if f & FC_ else 0)
        elif grp == 2: a = sub8(a, v)
        elif grp == 3: a = sub8(a, v, 1 if f & FC_ else 0)
        elif grp == 4: a = logic8(a & v); f |= FH
        elif grp == 5: a = logic8(a ^ v)
        elif grp == 6: a = logic8(a | v)
        else: sub8(a, v)
    elif op in (0xC0, 0xC8, 0xD0, 0xD8, 0xE0, 0xE8, 0xF0, 0xF8, 0xC9):
        if op == 0xC9:
            pc = pop()
        elif COND[(op >> 3) & 7]():
            pc = pop()
    elif op in (0xC2, 0xCA, 0xD2, 0xDA, 0xE2, 0xEA, 0xF2, 0xFA, 0xC3):
        t = rd16(pc); pc += 2
        if op == 0xC3 or COND[(op >> 3) & 7]():
            pc = t
    elif op in (0xC4, 0xCC, 0xD4, 0xDC, 0xE4, 0xEC, 0xF4, 0xFC, 0xCD):
        t = rd16(pc); pc += 2
        if op == 0xCD or COND[(op >> 3) & 7]():
            push(pc); pc = t
    elif op in (0xC5, 0xD5, 0xE5, 0xF5):
        if op == 0xF5:
            push((a << 8) | f)          # PUSH AF
        else:
            push(RP[(op >> 4) & 3]())
    elif op in (0xC1, 0xD1, 0xE1, 0xF1):
        if op == 0xF1:
            v = pop()                   # POP AF
            a, f = v >> 8, v & 0xFF
        else:
            setrp((op >> 4) & 3, pop())
    elif op in (0xC6, 0xCE, 0xD6, 0xDE, 0xE6, 0xEE, 0xF6, 0xFE):
        v = rd(pc); pc += 1
        if op == 0xC6: a = add8(a, v)
        elif op == 0xCE: a = add8(a, v, 1 if f & FC_ else 0)
        elif op == 0xD6: a = sub8(a, v)
        elif op == 0xDE: a = sub8(a, v, 1 if f & FC_ else 0)
        elif op == 0xE6: a = logic8(a & v); f |= FH
        elif op == 0xEE: a = logic8(a ^ v)
        elif op == 0xF6: a = logic8(a | v)
        else: sub8(a, v)
    elif op == 0xD3:
        out_port(rd(pc), a); pc += 1
    elif op == 0xDB:
        a = in_port(rd(pc)); pc += 1
    elif op == 0xE9:          # JP (HL) - DO CASE dispatch
        pc = (h << 8) | l
    elif op == 0xE3:          # EX (SP),HL
        t = rd16(sp)
        wr16(sp, (h << 8) | l)
        h, l = t >> 8, t & 0xFF
    elif op == 0xEB:
        old_de = (d << 8) | e
        old_hl = (h << 8) | l
        d, e = old_hl >> 8, old_hl & 0xFF
        h, l = old_de >> 8, old_de & 0xFF
    elif op == 0xF9:          # LD SP,HL
        sp = (h << 8) | l
    elif op == 0xF3:          # DI (no interrupt model - no-op)
        pass
    elif op == 0xFB:          # EI (no interrupt model - no-op)
        pass
    elif op == 0xED:
        op2 = rd(pc); pc += 1
        if op2 == 0xB0:      # LDIR
            n = (b << 8) | c
            src = (h << 8) | l; dst = (d << 8) | e
            for _ in range(n):
                wr(dst, rd(src)); src += 1; dst += 1
            h, l, d, e = src >> 8, src & 0xFF, dst >> 8, dst & 0xFF
            b = c = 0
            f &= ~(FH | FN | FPV)
        else:
            print('UNKNOWN ED %02X at %04X' % (op2, pc - 2)); break
    else:
        print('UNKNOWN opcode %02X at %04X' % (op, pc - 1)); break
    if steps % 200000 == 0:
        check_progress()
        if steps - last_fc_change_step > STALL:
            print('*** STALL: FC stuck at %d for %d steps, PC=%04X ***'
                  % (last_fc, steps - last_fc_change_step, pc))
            stalled = True
            break
        if steps % 2000000 == 0:
            print('  steps %d  FC(frame)=%d  PC=%04X' % (steps, last_fc, pc))

check_progress()
print('core result: %02X' % rd(0xC400))
print('steps executed:', steps)
print('last PC: %04X  SP: %04X' % (pc, sp))
print('max RIVER_FC frame counter reached:', max_frame)
print('RAM state: STATE=%d LIVES=%d FUEL=%d SCORE=%d%d%d%d' % (
    rd(0xC359), rd(0xC356), rd(0xC354),
    rd(0xC364), rd(0xC365), rd(0xC366), rd(0xC367)))
print('ET slots:', [rd(0xC380 + i) for i in range(4)])
print('TOPR=%d ROWT=%d BON=%d DON=%d SPX=%d SPY=%d' % (
    rd(0xC348), rd(0xC36A), rd(0xC36D), rd(0xC375), rd(0xC34B), rd(0xC34C)))
print('rom writes:', rom_writes[:5], 'count', len(rom_writes))
print('vdp writes:', vdp_writes[0], 'psg writes:', psg_writes[0])
print('port reads:', in_reads)
print('game-overs observed:', game_overs, ' restarts observed:', restarts,
      ' stall:', stalled)
