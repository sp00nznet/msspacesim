#!/usr/bin/env python3
"""
fdiff.py - does a lifted function draw what the original does?

difftest16 checks one instruction at a time. This checks a whole function,
from a real moment in a real run: a debug build snapshots the machine at the
function's first and second entry (SPACESIM_SNAP=<fn>, src/machine.c), this
runs the ORIGINAL bytes from the first snapshot under Unicorn until the
function returns, and compares the VGA planes it leaves with the second one.

    cmake -B build-dbg -DSPACESIM_TRACE=ON ...; cmake --build build-dbg --config Release
    set SPACESIM_SNAP=fn_38551 && build-dbg\\Release\\spacesim.exe --headless --seconds 3
    py -3 tools/fdiff.py fn_38551

Unicorn is run as a black box, the reference for what an x86 does (repo
rules, section 3). The VGA under it is the part of the card a blitter uses:
write mode 0 through the Map Mask, chain-4 or planar. Anything else the
function does to the card is reported, not modelled.

Whatever runs between the two entries (the caller, interrupt handlers) also
lands in snap2, so a difference is a lead, not a verdict: look at where it is.
"""
import os
import struct
import sys

from unicorn import Uc, UC_ARCH_X86, UC_MODE_16, UC_HOOK_MEM_WRITE, UC_HOOK_INSN, UC_HOOK_INTR
from unicorn.x86_const import (UC_X86_REG_EAX, UC_X86_REG_EBX, UC_X86_REG_ECX, UC_X86_REG_EDX,
                               UC_X86_REG_ESI, UC_X86_REG_EDI, UC_X86_REG_EBP, UC_X86_REG_ESP,
                               UC_X86_REG_CS, UC_X86_REG_DS, UC_X86_REG_ES, UC_X86_REG_SS,
                               UC_X86_REG_FS, UC_X86_REG_GS, UC_X86_REG_EFLAGS, UC_X86_REG_IP,
                               UC_X86_INS_OUT, UC_X86_REG_DX, UC_X86_REG_AL, UC_X86_REG_AX, UC_X86_REG_BX)
from unicorn.x86_const import UC_X86_REG_EFLAGS as _EF  # noqa: F401

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
MEM = 1024 * 1024 + 65536
VRAM = 0x40000


def load(path):
    d = open(path, 'rb').read()
    regs = struct.unpack_from('<8I', d, 0)
    segs = struct.unpack_from('<7H', d, 32)
    p = 32 + 14
    mem = bytearray(d[p:p + MEM]); p += MEM
    vram = bytearray(d[p:p + VRAM]); p += VRAM
    seq = bytearray(d[p:p + 8]); gc = d[p + 8:p + 24]
    seq_i = d[p + 28]
    p += 30
    ems = None
    if p + 4 <= len(d):                                  # src/ems.c ems_snap
        n = struct.unpack_from('<I', d, p)[0]; p += 4
        store = bytearray(d[p:p + n * 16384]); p += n * 16384
        mapped = list(struct.unpack_from('<4i', d, p)); p += 16
        hb = struct.unpack_from('<32I', d, p); p += 128
        hp = struct.unpack_from('<32I', d, p); p += 128
        ems = {'store': store, 'mapped': mapped, 'base': hb, 'pages': hp}
    load.ems = ems
    return regs, segs, mem, vram, seq, gc, seq_i


def entry_of(fn):
    """fn_XXXXX is the linear image address; its CS is the code segment that
    holds it, which recomp_all.h names (SEG_xxxx = runtime segment)."""
    lin = int(fn[3:], 16)
    segs = []
    for line in open(os.path.join(ROOT, 'work', 'spacesim', 'gen', 'recomp_all.h')):
        if line.startswith('#define SEG_'):
            parts = line.split()
            segs.append((int(parts[1][4:], 16), int(parts[2], 16)))
    img, run = max((s for s in segs if s[0] * 16 <= lin), key=lambda s: s[0])
    return run, lin - img * 16


def main():
    fn = sys.argv[1]
    regs, segs, mem, vram, seq, gc, seq_i = load(os.path.join(ROOT, 'work', 'snap1.bin'))
    ems_first = load.ems
    _, _, _, want, _, _, _ = load(os.path.join(ROOT, 'work', 'snap2.bin'))
    cs, ip = entry_of(fn)

    uc = Uc(UC_ARCH_X86, UC_MODE_16)
    uc.mem_map(0, 0x120000)
    uc.mem_write(0, bytes(mem))
    for r, v in zip((UC_X86_REG_EAX, UC_X86_REG_EBX, UC_X86_REG_ECX, UC_X86_REG_EDX,
                     UC_X86_REG_ESI, UC_X86_REG_EDI, UC_X86_REG_EBP, UC_X86_REG_ESP), regs):
        uc.reg_write(r, v)
    for r, v in zip((UC_X86_REG_DS, UC_X86_REG_ES, UC_X86_REG_SS, UC_X86_REG_FS, UC_X86_REG_GS),
                    segs[1:6]):
        uc.reg_write(r, v)
    uc.reg_write(UC_X86_REG_EFLAGS, segs[6] | 2)
    uc.reg_write(UC_X86_REG_CS, cs)

    state = {'seq_i': seq_i, 'other': []}
    got = bytearray(vram)

    def on_write(uc_, access, addr, size, value, _):
        if not 0xA0000 <= addr < 0xB0000:
            return
        for k in range(size):
            off, v = addr + k - 0xA0000, (value >> (8 * k)) & 0xFF
            if seq[4] & 8:                                  # chain-4: linear
                got[off] = v
                continue
            mask = seq[2] & 15
            for p in range(4):
                if mask >> p & 1:
                    got[(off << 2) | p] = v

    def on_out(uc_, port, size, value, _):
        for k in range(size):
            pt, v = port + k, (value >> (8 * k)) & 0xFF
            if pt == 0x3C4:
                state['seq_i'] = v
            elif pt == 0x3C5:
                seq[state['seq_i'] & 7] = v
            else:
                state['other'].append((pt, v))

    ems = load.ems if hasattr(load, 'ems') else None
    ems = ems_first

    files = {}

    def dos_file(ah, al):
        """Files, as the game opens them from game/ -- writes stay in memory,
        so the install is never touched by a differential run."""
        import io
        ds, dx = uc.reg_read(UC_X86_REG_DS), uc.reg_read(UC_X86_REG_DX)
        bx, cx = uc.reg_read(UC_X86_REG_BX), uc.reg_read(UC_X86_REG_ECX) & 0xFFFF
        def ok(ax):
            uc.reg_write(UC_X86_REG_AX, ax & 0xFFFF)
            uc.reg_write(UC_X86_REG_EFLAGS, uc.reg_read(UC_X86_REG_EFLAGS) & ~1)
        def fail(code):
            uc.reg_write(UC_X86_REG_AX, code)
            uc.reg_write(UC_X86_REG_EFLAGS, uc.reg_read(UC_X86_REG_EFLAGS) | 1)
        if ah in (0x3C, 0x3D):
            raw = bytes(uc.mem_read(ds * 16 + dx, 80)).split(bytes(1))[0].decode('latin-1')
            name = os.path.basename(raw.replace(chr(92), '/'))
            path = next((os.path.join(ROOT, 'game', f) for f in os.listdir(os.path.join(ROOT, 'game'))
                         if f.lower() == name.lower()), None)
            if ah == 0x3D and path is None:
                return fail(2)
            data = b'' if ah == 0x3C else open(path, 'rb').read()
            h = max(files, default=4) + 1
            files[h] = io.BytesIO(data)
            return ok(h)
        f = files.get(bx)
        if f is None:
            return fail(6)
        if ah == 0x3E:
            del files[bx]; return ok(0)
        if ah == 0x3F:
            b_ = f.read(cx); uc.mem_write(ds * 16 + dx, b_); return ok(len(b_))
        if ah == 0x40:
            f.write(bytes(uc.mem_read(ds * 16 + dx, cx))); return ok(cx)
        off = (cx << 16) | dx
        if off >= 0x80000000:
            off -= 0x100000000
        pos = f.seek(off, al)
        uc.reg_write(UC_X86_REG_DX, (pos >> 16) & 0xFFFF)
        return ok(pos)

    def on_intr(uc_, intno, _):
        """INT 67h function 44h, the bank switch the interpreter does, done as
        src/ems.c does it: copy the frame page out, the new page in."""
        ah = uc.reg_read(UC_X86_REG_AX) >> 8
        al = uc.reg_read(UC_X86_REG_AX) & 0xFF
        if intno == 0x21 and ah == 0x25:                  # set vector: just the IVT
            uc.mem_write(al * 4, struct.pack('<HH', uc.reg_read(UC_X86_REG_DX), uc.reg_read(UC_X86_REG_DS)))
            return
        if intno == 0x21 and ah == 0x35:
            off, seg = struct.unpack('<HH', uc.mem_read(al * 4, 4))
            uc.reg_write(UC_X86_REG_BX, off); uc.reg_write(UC_X86_REG_ES, seg)
            return
        if intno == 0x21 and ah in (0x3C, 0x3D, 0x3E, 0x3F, 0x40, 0x42):
            dos_file(ah, al)
            return
        if intno != 0x67 or ems is None or ah != 0x44:
            raise SystemExit(f'INT {intno:02X} AH={ah:02X} not modelled')
        p = uc.reg_read(UC_X86_REG_AX) & 0xFF
        log = uc.reg_read(UC_X86_REG_BX)
        h = uc.reg_read(UC_X86_REG_DX)
        frame = 0xE0000 + p * 16384
        st = ems['store']
        s_ = ems['base'][h] + log
        if ems['mapped'][p] != s_:
            for q in range(4):
                if q != p and ems['mapped'][q] == s_:
                    st[s_ * 16384:(s_ + 1) * 16384] = uc.mem_read(0xE0000 + q * 16384, 16384)
                    ems['mapped'][q] = -1
            if ems['mapped'][p] >= 0:
                m = ems['mapped'][p]
                st[m * 16384:(m + 1) * 16384] = uc.mem_read(frame, 16384)
            uc.mem_write(frame, bytes(st[s_ * 16384:(s_ + 1) * 16384]))
            ems['mapped'][p] = s_
        uc.reg_write(UC_X86_REG_AX, uc.reg_read(UC_X86_REG_AX) & 0x00FF)

    uc.hook_add(UC_HOOK_INTR, on_intr)
    uc.hook_add(UC_HOOK_MEM_WRITE, on_write)
    uc.hook_add(UC_HOOK_INSN, on_out, None, 1, 0, UC_X86_INS_OUT)
    try:
        uc.emu_start(cs * 16 + ip, cs * 16 + 0xFFFF, count=200_000_000)
    except Exception as e:
        print(f'stopped: {e} at {uc.reg_read(UC_X86_REG_CS):04X}:{uc.reg_read(UC_X86_REG_IP):04X}')

    # conventional memory too (not the stack page the function ran on, which
    # holds different sentinels by design)
    regs2, segs2, mem2, _, _, _, _ = load(os.path.join(ROOT, 'work', 'snap2.bin'))
    mine = uc.mem_read(0, 0xA0000)
    ss = segs[3] * 16
    mdiff = [a for a in range(0xA0000) if mine[a] != mem2[a] and not (ss <= a < ss + 0x10000)]
    print(f'memory: {len(mdiff)} bytes differ outside the stack segment')
    for a in mdiff[:16]:
        print(f'  {a:05X}: original {mine[a]:02X}  lifted {mem2[a]:02X}  (was {mem[a]:02X})')

    names = ('eax', 'ebx', 'ecx', 'edx', 'esi', 'edi', 'ebp', 'esp')
    ucr = (UC_X86_REG_EAX, UC_X86_REG_EBX, UC_X86_REG_ECX, UC_X86_REG_EDX,
           UC_X86_REG_ESI, UC_X86_REG_EDI, UC_X86_REG_EBP, UC_X86_REG_ESP)
    bad = [f'{n} original {uc.reg_read(r):08X} lifted {v:08X}'
           for n, r, v in zip(names, ucr, regs2) if uc.reg_read(r) != v]
    for n, r, v in zip(('ds', 'es', 'fs', 'gs'), (UC_X86_REG_DS, UC_X86_REG_ES, UC_X86_REG_FS, UC_X86_REG_GS),
                       (segs2[1], segs2[2], segs2[4], segs2[5])):
        if uc.reg_read(r) != v:
            bad.append(f'{n} original {uc.reg_read(r):04X} lifted {v:04X}')
    print('registers: ' + ('all agree' if not bad else '; '.join(bad)))

    diff = [i for i in range(VRAM) if got[i] != want[i]]
    print(f'{fn} from {cs:04X}:{ip:04X}: {len(diff)} plane bytes differ '
          f'(original wrote {sum(1 for i in range(VRAM) if got[i] != vram[i])}, '
          f'lifted {sum(1 for i in range(VRAM) if want[i] != vram[i])})')
    if state['other']:
        print(f'  ports besides the sequencer: {sorted(set(p for p, _ in state["other"]))}')
    for i in diff[:12]:
        off, p = i >> 2, i & 3
        print(f'  offset {off:04X} plane {p}: original {got[i]:02X}  lifted {want[i]:02X}')
    return 1 if diff else 0


if __name__ == '__main__':
    sys.exit(main())
