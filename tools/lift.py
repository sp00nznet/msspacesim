#!/usr/bin/env python3
"""
lift.py - lift Microsoft Space Simulator (SPACESIM.EXE, 1994) to C.

    py -3 tools/lift.py            # game/SPACESIM.EXE -> work/spacesim/gen

SPACESIM.EXE is a real-mode 386 program written largely in assembly by the
Bruce Artwick Organization, LZEXE-packed, with no C runtime at all. The
pipeline:

  1. unpack      pcrecomp tools/drm/unlzexe.py
  2. discover    recursive descent from the entry point, following near and far
                 calls by *decoding* them, plus every far code pointer the
                 relocation table names in data, plus whatever a previous run
                 recorded as a dispatch miss.
  3. lift        pcrecomp lift16.Lifter, one C function per entry point. A
                 function is the closure of everything reachable from its entry
                 without a call, so shared tails are simply lifted twice.
  4. emit        work/spacesim/gen/: chunked C, prototypes, segment constants,
                 and the address -> function table indirect calls go through.

The driver started as Blake Stone's (same toolkit, same DOS machine); what this
game needed on top is in docs/lifting.md. The output is generated from your
own copy of the game and is never committed.
"""
import filecmp
import json
import re
import shutil
import os
import struct
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

# The game, and the programs its installer ran once to build the star and
# planet files (FINISH.BAT): MAKEDATA, MAKEHMO, then S01DATA..S19DATA. Same
# authors, same style of code, so the same pipeline lifts all of them, and
# each runs on the same DOS machine -- tools/setup.py drives them.
GAMES = {'spacesim': ('', 'SPACESIM.EXE'), 'makedata': ('', 'MAKEDATA.EXE'),
         'makehmo': ('', 'MAKEHMO.EXE')}
GAMES.update({f's{n:02d}data': ('', f'S{n:02d}DATA.EXE') for n in range(1, 20)})

# Where the program image goes in guest memory. DOS would put the PSP wherever
# the last TSR ended; this is a clean machine, so it goes low, and the game gets
# everything from just above the image to A000 for its heap. Baked into the
# lifted C (relocated segment immediates), so the runtime must agree:
# src/dos.c LOAD_SEG.
PSP_SEG = 0x0100
LOAD_SEG = PSP_SEG + 0x10

NL = chr(10)

# LIFT_EXIT_HOOK=fn_X,fn_Y wraps those functions so the runtime sees them
# return (debug builds snapshot there for tools/fdiff.py). Off by default.
EXIT_HOOKS = set(filter(None, os.environ.get('LIFT_EXIT_HOOK', '').split(',')))
CHUNK = 150          # functions per generated .c file: MSVC is slow on huge TUs


def _pcrecomp_home():
    """PCRECOMP_HOME, else a sibling checkout -- never one person's drive."""
    env = os.environ.get('PCRECOMP_HOME')
    cands = [env] if env else []
    cands += [os.path.join(ROOT, 'work', 'pcrecomp'),          # where Setup checks it out
              os.path.join(os.path.dirname(ROOT), 'pcrecomp'),
              os.path.join(os.path.dirname(ROOT), 'pcrecomp-main')]
    for c in cands:
        if c and os.path.isdir(os.path.join(c, 'tools', 'lift')):
            return os.path.abspath(c)
    sys.exit('cannot find the pcrecomp toolkit; set PCRECOMP_HOME to a checkout of\n'
             '  https://github.com/sp00nznet/pcrecomp')


PCR = _pcrecomp_home()
for sub in ('disasm', 'lift', 'drm'):
    sys.path.insert(0, os.path.join(PCR, 'tools', sub))
import decode16                     # noqa: E402
decode16.WRAP_NEAR_TARGETS = True   # IP arithmetic wraps inside the segment
from decode16 import Decoder, OpType  # noqa: E402
import lift16                       # noqa: E402
from lift16 import Lifter           # noqa: E402
import unlzexe                      # noqa: E402
import unexepack                    # noqa: E402

TERMINATORS = {'ret', 'retf', 'iret', 'hlt'}
JCC = {'jo', 'jno', 'jb', 'jae', 'je', 'jne', 'jbe', 'ja', 'js', 'jns', 'jp',
       'jnp', 'jl', 'jge', 'jle', 'jg', 'loop', 'loopz', 'loopnz', 'jcxz'}


class Image:
    def __init__(self, path):
        raw = open(path, 'rb').read()
        if raw[0x1C:0x20] in (b'LZ09', b'LZ91'):
            raw = unlzexe.unpack(raw)
        elif unexepack.is_packed(raw):                # the S??DATA programs
            raw = unexepack.unpack(raw)
        self.exe = raw
        h = struct.unpack_from('<14H', raw, 0)
        self.hdr = h[4] * 16
        self.img = raw[self.hdr:]
        self.ss, self.sp, self.ip, self.cs = h[7], h[8], h[10], h[11]
        self.relocs = [struct.unpack_from('<HH', raw, h[12] + 4 * i) for i in range(h[3])]  # e_lfarlc, not always 1Ch
        # Linear positions of the relocated words, and what is stored there.
        self.reloc_at = {s * 16 + o for o, s in self.relocs}
        # No C runtime, so no DGROUP to stop at: code and data share segments
        # (segment 0 holds both), and only the walk decides what is code.
        # `dgroup` is kept as "the paragraph the image ends at".
        self.dgroup = (len(self.img) + 15) // 16
        self._dec = {}

    def decode(self, cs, ip):
        d = self._dec.get(cs)
        if d is None:
            d = self._dec[cs] = Decoder(self.img[cs * 16:cs * 16 + 0x10000], cs * 16)
        d.pos = ip
        try:
            return d.decode_one()
        except (IndexError, KeyError):
            return None


def jump_table(img, fn, ins):
    """Arms of a Borland switch, `jmp word cs:[bx+disp]`, as IPs.

    Dense:  cmp bx, N / ja default / shl bx,1 / jmp cs:[bx+table]
    Sparse: mov cx, N / mov bx, values / ... loop ... / jmp cs:[bx+N*2]
            (the arm offsets follow the N case values; N*4 when they are dwords)
    The bound comes from the code in front of the jump; without one the table
    is not trusted, and the jump goes to the runtime dispatcher instead.
    """
    o = ins.op1
    if not (o and o.type == OpType.MEM and o.base == 'bx' and not o.index
            and o.seg == 'cs'):
        return None
    prev = [fn[a] for a in sorted(fn) if ins.address - 48 <= a < ins.address]
    bound = bx_imm = cx_imm = None
    for j in prev[-16:]:
        if not (j.op1 and j.op1.type == OpType.REG16 and j.op2
                and j.op2.type in (OpType.IMM8, OpType.IMM16)):
            continue
        reg = repr(j.op1)
        if j.mnemonic == 'cmp':
            bound = (j.op2.disp & 0xFFFF) + 1
        elif j.mnemonic == 'mov' and reg == 'bx':
            bx_imm = j.op2.disp & 0xFFFF
        elif j.mnemonic == 'mov' and reg == 'cx':
            cx_imm = j.op2.disp & 0xFFFF
    # Sparse: the arms follow the case values -- one word per case for a
    # 16-bit switch, two (low words, then high words) for a 32-bit one.
    if bx_imm is not None and cx_imm and (o.disp & 0xFFFF) in (cx_imm * 2, cx_imm * 4):
        bound, start = cx_imm, (bx_imm + o.disp) & 0xFFFF
    else:
        start = o.disp & 0xFFFF
    if not bound or bound > 0x1000:
        return None
    base = ins.offset - ins.address               # cs * 16
    return [struct.unpack_from('<H', img, base + ((start + 2 * k) & 0xFFFF))[0]
            for k in range(bound)]


def exits(img, ins):
    """`int 21h` with AH=4Ch loaded right before it: the process ends here, and
    what follows is usually the error messages, not code."""
    if ins.mnemonic != 'int' or ins.op1 is None or ins.op1.disp != 0x21:
        return False
    p = ins.offset
    return img[p - 2:p] == bytes((0xB4, 0x4C)) or (img[p - 3] == 0xB8 and img[p - 1] == 0x4C)


def push_call(fn, ins):
    """(label, push address, bytes pushed after it) if this jmp is a call made
    by hand, else None.

    BAO's assembly calls a near routine in another segment as
        push cs / push label / push 0 / jmp far seg:routine
    -- the routine's `ret` pops the 0 into seg:0000, a lone retf, which pops
    label:cs. The label is the instruction after the jmp; that is what makes
    it a call and not a pushed constant. The walk then continues there, in
    this function, and pcrecomp lift16 lifts the jmp as a call
    (`Lifter.push_calls`)."""
    ret = (ins.address + ins.length) & 0xFFFF
    ends = {(i.address + i.length) & 0xFFFF: i for i in fn.values()}
    cur, extra = ins.address, 0
    far = ins.mnemonic == 'retf' or ins.op1.type == OpType.FAR
    if ins.mnemonic == 'retf':
        # `push seg / push off / retf`: the target, popped by the retf itself
        # (the SMDL interpreter calls a model's code so). Not part of the frame.
        for _ in range(2):
            p = ends.get(cur)
            if p is None or p.mnemonic != 'push' or p.op1 is None:
                return None
            cur = p.address
    for _ in range(4):
        p = ends.get(cur)
        if p is None or p.mnemonic != 'push' or p.op1 is None:
            return None
        o = p.op1
        # The label is the instruction after the jmp -- except with retf, whose
        # frame the push cs / push target / retf shape already proves, and
        # whose label may sit past the next instruction (the SMDL caller's
        # lands after the `call ax` it uses for a target in its own segment).
        is_label = (o.type in (OpType.IMM8, OpType.IMM16) and
                    ((o.disp & 0xFFFF) == ret or (ins.mnemonic == 'retf' and 0 < ((o.disp - ret) & 0xFFFF) < 16)))
        if is_label:
            ret = o.disp & 0xFFFF
            if far:
                q = ends.get(p.address)
                if q is None or q.mnemonic != 'push' or repr(q.op1) != 'cs':
                    return None
            return ret, p.address, extra
        extra += 4 if o.type == OpType.REG32 or o.size == 4 else 2
        cur = p.address
    return None


def discover(im, extra):
    """{linear entry: cs}, {linear entry: {ip: Instruction}}, {entry: {jmp ip: arms}}"""
    entries = {}
    work = []

    def add(cs, ip):
        lin = cs * 16 + ip
        if lin >= im.dgroup * 16:                      # code ends where DGROUP begins
            return
        if lin not in entries:
            entries[lin] = cs
            work.append(lin)

    add(im.cs, im.ip)
    # Recovered pointers wait until everything reachable by calls has been
    # walked: a call states its target's CS, a stored pointer only implies one,
    # and the first CS an address is reached with is the one it keeps.
    pending = sorted(extra)

    bodies, tables, bad = {}, {}, []
    pcalls = {}
    codesegs = set()
    while work or pending:
        if not work:
            for cs, ip in pending:
                add(cs, ip)
            pending = []
            continue
        lin = work.pop()
        cs = entries[lin]
        codesegs.add(cs)
        fn, tbl, pc = {}, {}, {}
        todo = [lin - cs * 16]
        while todo:
            ip = todo.pop()
            if ip in fn:
                continue
            ins = im.decode(cs, ip)
            if ins is None or ins.mnemonic == 'db':
                bad.append((cs, ip, lin))
                continue
            fn[ip] = ins
            m, o = ins.mnemonic, ins.op1
            nxt = (ip + ins.length) & 0xFFFF
            if m == 'retf' and o is None:
                c = push_call(fn, ins)
                if c:
                    pc[ip] = c
                    todo.append(c[0])
                    continue
            if m in TERMINATORS or m == 'jmp far' or exits(im.img, ins):
                continue
            if m == 'jmp' and o.type in (OpType.REL8, OpType.REL16, OpType.FAR):
                c = push_call(fn, ins)
                if c:
                    pc[ip] = c
                    if o.type == OpType.FAR:
                        add(o.far_seg, o.disp)
                    else:
                        add(cs, o.disp & 0xFFFF)
                    todo.append(c[0])
                    continue
            if m == 'jmp':
                if o.type in (OpType.REL8, OpType.REL16):
                    todo.append(o.disp & 0xFFFF)
                elif o.type == OpType.FAR:
                    add(o.far_seg, o.disp)
                else:
                    arms = jump_table(im.img, fn, ins)
                    if arms:
                        tbl[ip] = arms
                        todo.extend(arms)
                continue
            if m in JCC:
                todo.append(o.disp & 0xFFFF)
            elif m == 'call' and o and o.type == OpType.REL16:
                add(cs, o.disp & 0xFFFF)
            elif m == 'call' and o and o.type == OpType.FAR:
                add(o.far_seg, o.disp)
            todo.append(nxt)
        bodies[lin] = fn
        tables[lin] = tbl
        pcalls[lin] = pc
    return entries, bodies, tables, codesegs, bad, pcalls


# How a function in this program starts, when something stores a pointer to
# it rather than calling it: a Borland frame, `enter`, or the register saves
# that open an interrupt handler or a hand-written asm routine.
# The first group is distinctive enough to make a segment code on its own;
# the second only counts inside a segment already known to be code.
STRONG_SIGS = tuple(bytes(x) for x in (
    (0x55, 0x8B, 0xEC), (0x50, 0x53, 0x51, 0x52), (0x66, 0x56, 0x66, 0x57), (0x66, 0x60)))
ENTRY_SIGS = STRONG_SIGS + tuple(bytes(x) for x in (
    (0xC8,), (0x1E, 0x50), (0x60,), (0x1E, 0x56, 0x57), (0x56, 0x57),
    (0x1E, 0xB8)))                               # push ds / mov ax, DGROUP


def code_pointers(im, entries, bodies, codesegs, tables=False):
    """Entry points nothing calls directly, found from where their address is
    stored. Every one of these used to surface at run time as a dispatch miss:

      far, in data      a relocated code segment with its offset in front of it:
                        the actor state tables, the menu handler tables
      far, in code      a relocated code-segment immediate with the offset as a
                        neighbouring immediate -- `push seg / push off` is how
                        setvect() gets the timer, keyboard and SB handlers
      near, in code     `mov ax, offset` / `push offset` of a routine in the
                        same segment: atexit-style callbacks

    The near forms are only taken when the target opens like a function
    (ENTRY_SIGS); a plain immediate is far more often a number."""
    out = set()
    img = im.img

    # A near pointer can only name code in its own segment, which ends where
    # the next one begins. Without this a word that happens to point at a
    # prologue in a later segment becomes an entry with the wrong CS -- and
    # every near call and cs: read inside it resolves against that CS. (Planet
    # Strike's `push cs / call 000Fh` landed in the startup code and ran main
    # a second time.)
    known = sorted(codesegs)

    def seg_end(seg):
        later = [c for c in known if c > seg]
        return min(later[0] if later else im.dgroup, im.dgroup) * 16

    def opens_function(seg, off, sigs=ENTRY_SIGS):
        t = seg * 16 + off
        if seg in codesegs and t >= seg_end(seg):
            return False
        return t < len(img) and img[t:t + 4].startswith(sigs) and im.decode(seg, off)

    def after_exit(t):
        return (img[t - 1] in (0xC3, 0xCB, 0xCF) or img[t - 3] in (0xC2, 0xCA, 0xE9)
                or img[t - 2] == 0xEB)

    def stored_handler(seq, i, cs, v):
        """`mov ax, offset / mov [vector], ax`: how the 2D driver installs its
        entry points into the game's table at DS:BDD0 (one pair per function,
        at the top of the driver). The offset lands right after a ret or jmp
        in the same segment -- a routine's first byte -- and decodes."""
        ins = seq[i]
        if ins.mnemonic != 'mov' or i + 1 >= len(seq) or ins.op1 is None or ins.op1.type != OpType.REG16:
            return False
        nx = seq[i + 1]
        if not (nx.mnemonic == 'mov' and nx.op1 is not None and nx.op1.type in (OpType.MEM, OpType.MOFFS)
                and repr(nx.op2) == repr(ins.op1)):
            return False
        t = cs * 16 + v
        # Into the driver vector table (DS:BDD0-BDFF) the store itself says it
        # is a routine; elsewhere, the routine must start after a ret or jmp.
        vec = nx.op1.type in (OpType.MEM, OpType.MOFFS) and not nx.op1.base and not nx.op1.index \
            and 0xBDD0 <= (nx.op1.disp & 0xFFFF) < 0xBE00
        return (t < seg_end(cs) and t >= cs * 16 + 3 and (vec or after_exit(t))
                and im.decode(cs, v) is not None)

    # bytes the walk decoded as instructions: a relocation there is an
    # immediate, handled below; anywhere else it is data
    covered = {ins.offset + k for body in bodies.values() for ins in body.values()
               for k in range(ins.length)}
    for p in sorted(im.reloc_at):
        seg = struct.unpack_from('<H', img, p)[0]
        if seg in codesegs and p >= 2 and p not in covered:
            off = struct.unpack_from('<H', img, p - 2)[0]
            if seg * 16 + off < seg_end(seg) and im.decode(seg, off) is not None:
                out.add((seg, off))

    for lin, body in bodies.items():
        cs = entries[lin]
        seq = [body[a] for a in sorted(body)]
        for i, ins in enumerate(seq):
            imms = [o for o in (ins.op1, ins.op2) if o is not None and o.type == OpType.IMM16]
            if ins.mnemonic not in ('mov', 'push') or not imms:
                continue
            v = imms[-1].disp & 0xFFFF
            at = ins.offset + ins.length - 2
            if at in im.reloc_at:
                # `push seg / pop es` (or ds) loads a data segment -- a string
                # in a code segment, say -- and the offset beside it is data.
                nx = seq[i + 1] if i + 1 < len(seq) else None
                loads_sreg = (ins.mnemonic == 'push' and nx is not None and nx.mnemonic == 'pop'
                              and repr(nx.op1) in ('es', 'ds', 'fs', 'gs'))
                if v < im.dgroup and not loads_sreg:    # maybe a far pointer's segment half
                    for j in seq[max(0, i - 3):i + 4]:
                        for o in (j.op1, j.op2):
                            if (j is not ins and o is not None and o.type == OpType.IMM16
                                    and j.mnemonic in ('mov', 'push')):
                                off = o.disp & 0xFFFF
                                # a segment nothing calls yet must prove it is code
                                if (im.decode(v, off) is not None if v in codesegs
                                        else opens_function(v, off, STRONG_SIGS)):
                                    out.add((v, off))
            elif opens_function(cs, v):
                out.add((cs, v))
            elif stored_handler(seq, i, cs, v):
                out.add((cs, v))

    # Far pointers to its own code, built from CS: `mov dx, cs` beside
    # `mov ax, offset` (then written into the vector table) or `lea dx, [offset]`
    # (then INT 21h AH=25h). That is how the timer (INT 8) and divide (INT 0)
    # handlers are installed, and nothing else ever names them.
    for lin, body in bodies.items():
        cs = entries[lin]
        seq = [body[a] for a in sorted(body)]
        for i, ins in enumerate(seq):
            if not (ins.mnemonic in ('mov', 'push') and ins.op2 is not None and repr(ins.op2) == 'cs'
                    or ins.mnemonic == 'push' and repr(ins.op1) == 'cs'):
                continue
            for j in seq[max(0, i - 6):i + 7]:
                o = j.op2
                if j.mnemonic == 'mov' and o is not None and o.type == OpType.IMM16:
                    v = o.disp & 0xFFFF
                elif (j.mnemonic == 'lea' and o is not None and o.type == OpType.MEM
                      and not o.base and not o.index):
                    v = o.disp & 0xFFFF
                else:
                    continue
                t = cs * 16 + v
                if cs * 16 + 3 <= t < seg_end(cs) and after_exit(t) and im.decode(cs, v) is not None:
                    out.add((cs, v))

    # Indexed handler tables in the data segment: `call word ds:[eax*2+802Eh]`
    # is the SMDL model interpreter's opcode dispatch (85 handlers), and there
    # are a few more like it. DS is segment 0 for the whole program, so the
    # table's address is its displacement. Take words while each lands on
    # plausible code in the caller's segment -- right after a ret or jmp, or
    # an entry already known -- and stop at the first that does not (the
    # opcode table runs straight into the "Bad SMDL Opcode" string).
    for lin, body in bodies.items():
        cs = entries[lin]
        for ins in body.values():
            o = ins.op1
            if not (ins.mnemonic in ('call', 'jmp') and o is not None and o.type == OpType.MEM
                    and o.seg in ('', 'ds') and (o.index or o.base) and o.size in (0, 2)):
                continue
            tab = o.disp & 0xFFFF
            # The bound, when the code states it: `cmp al, 54h / ja bad` in
            # front of the dispatch. With it every entry is taken (a handler
            # that falls into the next one does not end the table); without
            # it, entries are taken while they look like routine starts.
            bound = None
            reg = (o.index or o.base or '')[-2:]          # eax -> ax, bx -> bx
            prev = [body[a] for a in sorted(body) if ins.address - 24 <= a < ins.address]
            for j in reversed(prev):
                if (j.mnemonic == 'cmp' and j.op1 is not None and j.op2 is not None
                        and j.op2.type in (OpType.IMM8, OpType.IMM16)
                        and repr(j.op1)[-2:] in (reg, reg[0] + 'l')):
                    bound = (j.op2.disp & 0xFFFF) + 1
                    break
            misses_in_row = 0
            for k in range(bound if bound and bound <= 512 else 512):
                p = tab + 2 * k
                if p + 2 > len(img):
                    break
                v = img[p] | img[p + 1] << 8
                t = cs * 16 + v
                if not v or not (cs * 16 + 3 <= t < seg_end(cs) and im.decode(cs, v) is not None):
                    break
                if not bound:
                    # handlers often fall into the next one, so one entry that
                    # is not after a ret proves nothing; three in a row is text
                    misses_in_row = 0 if (after_exit(t) or t in entries) else misses_in_row + 1
                    if misses_in_row == 3:
                        break
                out.add((cs, v))

    # Tables of near pointers inside a code segment, walked with lodsw/call ax
    # (the video-card probe list): runs of three or more words in bytes the
    # walk never decoded, each naming a decodable, not-yet-walked spot in the
    # same segment that the instruction before cannot fall into. Only once
    # the walk has converged -- before that, most of the image is "not
    # decoded" and data passes for tables by the thousand.
    if not tables:
        return out

    starts = sorted(codesegs) + [im.dgroup]
    found = []
    for cs, nxt in zip(starts, starts[1:]):
        lo, hi = cs * 16, min(nxt * 16, cs * 16 + 0x10000, len(img) - 1)
        p = lo
        while p < hi - 6:
            best = None
            for stride in (2, 3, 4):                  # bare words, or word + flag fields
                run, q = [], p
                while q + 1 < hi and q not in covered and q + 1 not in covered:
                    v = img[q] | img[q + 1] << 8
                    t = lo + v
                    if not (lo <= t < hi and t not in covered and not (p <= t < q + stride)
                            and after_exit(t) and im.decode(cs, v)):
                        break
                    run.append(v)
                    q += stride
                if len(set(run)) >= 3 and (best is None or len(run) > len(best[0])):
                    best = (run, q)
            if best:
                out.update((cs, v) for v in best[0])
                found.append((cs, p, best[0]))
                p = best[1]
            else:
                p += 1
    if os.environ.get('LIFT_SHOW_TABLES'):
        for cs, p, run in found:
            print(f'  table {cs:04X}:{p - cs * 16:04X} -> ' + ' '.join(f'{v:04X}' for v in run))
    return out


WRITES = {'mov', 'add', 'sub', 'adc', 'sbb', 'and', 'or', 'xor', 'inc', 'dec',
          'not', 'neg', 'shl', 'sal', 'shr', 'sar', 'rol', 'ror', 'pop', 'xchg'}


def smc_targets(entries, bodies):
    """Linear addresses the code writes through a constant `cs:` address --
    the self-modifying inner loops patch their own immediates this way."""
    out = set()
    for lin, body in bodies.items():
        cs = entries[lin]
        for ins in body.values():
            o = ins.op1
            if (ins.mnemonic in WRITES and o is not None and o.type in (OpType.MEM, OpType.MOFFS)
                    and o.seg == 'cs' and not o.base and not o.index):
                for k in range(o.size or 2):
                    out.add(cs * 16 + ((o.disp + k) & 0xFFFF))
    return out


def load_misses(path, codesegs, img_len):
    """The dispatch misses a run recorded, as (cs, ip) in the code segment that
    holds each one. The segment a miss was reached with is not that: the game
    jumps through normalised far pointers (21C6:0001 is 1B87:52F1), and taking
    their segment as a CS invents code segments -- every cs: read and near
    call inside the function would then resolve against the wrong one."""
    out = set()
    if not os.path.exists(path):
        return out
    for line in open(path):
        line = line.strip()
        if ':' not in line:
            continue
        s, o = line.split(':')[:2]
        lin = ((int(s, 16) - LOAD_SEG) * 16 + int(o, 16)) & 0xFFFFF
        cs = max((c for c in codesegs if c * 16 <= lin < c * 16 + 0x10000), default=None)
        if cs is not None and lin < img_len:
            out.add((cs, lin - cs * 16))
    return out


def main():
    names = sys.argv[1:] or ['spacesim']
    if names == ['all']:
        names = list(GAMES)
    for game in names:
        lift_one(game)


# The 2D driver region. The game's built-in VGA driver sits at 293C:D010; a
# Super VGA driver file (SVGA.DRV 640x400, SVGA8.DRV 800x600) is read over it at
# run time -- its first 1700h bytes to the driver header at DS:DE74, the next
# 2D00h to 293C:D010 -- and its entry point installs the routines the game
# calls through DS:BDD0. So the same addresses hold different code depending on
# SPACESIM.INI. Each driver is lifted as a variant of that region (fn_X_vga,
# fn_X_svga, fn_X_svga8); every address in it gets one trampoline fn_X that the
# rest of the program calls, and src/drv.c sends it to the variant whose bytes
# are loaded. See docs/lifting.md, "Super VGA".
DRV_SEG, DRV_IP, DRV_LEN, DRV_HDR, DRV_HDR_LEN = 0x293C, 0xD010, 0x2D00, 0xDE74, 0x1700
DRV_FILES = ('SVGA.DRV', 'SVGA8.DRV')


def in_drv(lin):
    return DRV_SEG * 16 + DRV_IP <= lin < DRV_SEG * 16 + DRV_IP + DRV_LEN


def walk(im, extra):
    """discover + code_pointers to a fixed point: (entries, bodies, tables,
    codesegs, bad, pcalls, fps)."""
    fps = set()
    for final in (False, True):
        while True:
            entries, bodies, tables, codesegs, bad, pcalls = discover(im, extra | fps)
            more = code_pointers(im, entries, bodies, codesegs, tables=final) - fps
            if not more:
                break
            fps |= more
    return entries, bodies, tables, codesegs, bad, pcalls, fps


def driver_variants(src, im, codesegs0, work):
    """[(name, Image, walk result)] for each driver file beside the game."""
    out = []
    for fname in DRV_FILES:
        path = os.path.join(os.path.dirname(src), fname)
        if not os.path.exists(path):
            continue
        drv = open(path, 'rb').read()
        if len(drv) < DRV_HDR_LEN + DRV_LEN:
            continue
        vim = Image(src)
        img = bytearray(vim.img)
        img[DRV_SEG * 16 + DRV_IP:DRV_SEG * 16 + DRV_IP + DRV_LEN] = drv[DRV_HDR_LEN:DRV_HDR_LEN + DRV_LEN]
        img[DRV_HDR:DRV_HDR + DRV_HDR_LEN] = drv[:DRV_HDR_LEN]
        vim.img = bytes(img)
        vim._dec = {}
        name = fname.lower().split('.')[0]
        extra = {(DRV_SEG, DRV_IP)}
        extra |= {e for e in load_misses(os.path.join(work, f'misses_{name}.txt'), codesegs0, len(img))
                  if in_drv(e[0] * 16 + e[1])}
        out.append((name, vim, walk(vim, extra)))
    return out


def lift_one(game):
    sub, exe = GAMES[game]
    src = os.path.join(ROOT, 'game', exe)
    if not os.path.exists(src):
        sys.exit(f'{src} not found -- install your own copy into game/ first '
                 '(Setup.cmd does it from the three floppy images; see README)')
    work = os.path.join(ROOT, 'work', game)
    out = os.path.join(work, 'gen')
    os.makedirs(out, exist_ok=True)

    im = Image(src)
    open(os.path.join(work, 'unpacked.exe'), 'wb').write(im.exe)
    # Anything a previous run jumped to that had no lifted function. Local to
    # this checkout (work/ is never committed); see docs/lifting.md.
    codesegs0 = discover(im, set())[3]
    extra = load_misses(os.path.join(work, 'misses.txt'), codesegs0, len(im.img))

    # Walk from the entry point, then add the stored code pointers the walk
    # makes visible and walk again until nothing new turns up.
    entries, bodies, tables, codesegs, bad, pcalls, fps = walk(im, extra)
    variants = driver_variants(src, im, codesegs0, work) if game == 'spacesim' else []
    # A driver may call game code nothing else reaches: make it an entry here.
    outside = {(cs, lin - cs * 16) for _n, _i, w in variants for lin, cs in w[0].items()
               if not in_drv(lin) and lin not in entries}
    if outside:
        entries, bodies, tables, codesegs, bad, pcalls, fps = walk(im, extra | outside)

    print(f'{exe}: DGROUP {im.dgroup:04X}, {len(codesegs)} code segments, '
          f'{len(entries)} functions, {sum(len(b) for b in bodies.values())} '
          f'instructions lifted, {len(fps)} stored code pointers, '
          f'{len(extra)} extra entries, {sum(len(t) for t in tables.values())} switch tables, '
          f'{sum(len(p) for p in pcalls.values())} hand-made calls')
    if bad:
        print(f'  {len(bad)} undecodable targets, first: '
              + ', '.join(f'{c:04X}:{i:04X} (in fn_{l:05X})' for c, i, l in bad[:5]))

    names = {lin: f'fn_{lin:05X}' for lin in entries}         # what callers call
    drv_lins = sorted({lin for lin in entries if game == 'spacesim' and in_drv(lin)} |
                      {lin for v in variants for lin in v[2][0] if in_drv(lin)})
    for lin in drv_lins:
        names[lin] = f'fn_{lin:05X}'                             # the trampoline
    defname = {lin: names[lin] + ('_vga' if lin in drv_lins else '') for lin in entries}
    lifter = Lifter(known_funcs=names, reloc_words=im.reloc_at, load_seg=LOAD_SEG)
    lifter.far_base = 0
    lifter.dispatch = True
    lifter.iret_frame = True
    lifter.x87 = True
    # BAO's code leaves deep nesting by resetting SP (the SMDL interpreter's
    # handlers do `mov sp, [7E60] / ret`); see docs/lifting.md.
    lifter.unwind_sp = not os.environ.get('LIFT_NO_UNWIND')
    lifter.smc_imm = smc_targets(entries, bodies)
    print(f'  {len(lifter.smc_imm)} self-modified code bytes')
    lift16.DIV0_FN = 'recomp_div0'

    # Write into a fresh directory, then sync() it over the real one.
    final, out = out, out + '.new'
    shutil.rmtree(out, ignore_errors=True)
    os.makedirs(out)

    order = sorted(entries)
    unhandled = {}
    files = []
    for ci in range(0, len(order), CHUNK):
        chunk = order[ci:ci + CHUNK]
        path = os.path.join(out, f'recomp_{ci // CHUNK:03d}.c')
        files.append(path)
        with open(path, 'w') as f:
            f.write('/* generated by tools/lift.py from your own copy of the game -- do not commit */\n')
            f.write('#include "lifted.h"\n\n')
            for lin in chunk:
                cs = entries[lin]
                insts = [bodies[lin][a] for a in sorted(bodies[lin])]
                insts = _seal_fallthrough(insts)
                lift16._CODE_SEG = f'{cs:04X}'
                lifter.jump_tables = {ip: [cs * 16 + a for a in arms]
                                      for ip, arms in tables[lin].items()}
                lifter.push_calls = pcalls[lin]
                hooked = names[lin] in EXIT_HOOKS
                body = lifter.lift_function(defname[lin] + ('_body' if hooked else ''), insts,
                                            cs * 16, entry_addr=lin - cs * 16)
                for line in body.split('\n'):
                    if 'UNHANDLED' in line or 'needs dispatch' in line:
                        k = line.split('/*')[-1].strip(' */')
                        unhandled[k] = unhandled.get(k, 0) + 1
                f.write(body + '\n\n')
                if hooked:                                # tools/fdiff.py: a snapshot as it returns
                    f.write(f'void {names[lin]}(CPU *cpu) {{ {names[lin]}_body(cpu); '
                            f'recomp_exit_hook("{names[lin]}_body"); }}\n\n')

    # The driver variants, each with its own self-modified bytes.
    for vname, vim, (ventries, vbodies, vtables, _vc, _vb, vpcalls, _vf) in variants:
        path = os.path.join(out, f'recomp_drv_{vname}.c')
        files.append(path)
        vl = Lifter(known_funcs=names, reloc_words=vim.reloc_at, load_seg=LOAD_SEG)
        vl.far_base, vl.dispatch, vl.iret_frame, vl.x87 = 0, True, True, True
        vl.unwind_sp = lifter.unwind_sp
        vl.smc_imm = smc_targets(ventries, vbodies)
        with open(path, 'w') as f:
            f.write(f'/* generated by tools/lift.py: the {vname.upper()}.DRV variant of the 2D driver -- do not commit */\n')
            f.write('#include "lifted.h"\n\n')
            for lin in sorted(l for l in ventries if in_drv(l)):
                cs = ventries[lin]
                insts = _seal_fallthrough([vbodies[lin][a] for a in sorted(vbodies[lin])])
                lift16._CODE_SEG = f'{cs:04X}'
                vl.jump_tables = {ip: [cs * 16 + a for a in arms] for ip, arms in vtables[lin].items()}
                vl.push_calls = vpcalls[lin]
                f.write(vl.lift_function(f'{names[lin]}_{vname}', insts, cs * 16, entry_addr=lin - cs * 16) + '\n\n')
        print(f'  {vname.upper()}.DRV: {sum(1 for l in ventries if in_drv(l))} functions in the driver region')
    if drv_lins:
        with open(os.path.join(out, 'recomp_drv.c'), 'w') as f:
            f.write('/* generated by tools/lift.py -- do not commit */\n#include "lifted.h"\n')
            f.write('void drv_call(CPU *cpu, uint32_t lin);\n')
            for lin in drv_lins:
                f.write(f'void {names[lin]}(CPU *cpu) {{ drv_call(cpu, 0x{lin:05X}); }}\n')
        files.append(os.path.join(out, 'recomp_drv.c'))

    # A far call lift16 could not resolve names a function that does not
    # exist (in practice: bytes decoded past a call that never returns).
    # Define each as a dispatch, which logs a miss if it is ever reached,
    # rather than let the link fail.
    strays = set()
    for path in files:
        strays |= set(re.findall(r'\b(far_[0-9A-F]{4}_[0-9A-F]{4})\(cpu\)', open(path).read()))
    if strays:
        with open(files[-1], 'a') as f:
            for n in sorted(strays):
                f.write(f'void {n}(CPU *cpu) {{ cpu->sp += 4; '
                        f'dispatch_far(cpu, 0x{n[4:8]}, 0x{n[9:13]}); }}\n')
        print(f'  {len(strays)} far calls into nothing (stubbed as dispatch)')

    with open(os.path.join(out, 'recomp_all.h'), 'w') as f:
        f.write('/* generated by tools/lift.py -- do not commit */\n#pragma once\n')
        for cs in sorted(codesegs):
            f.write(f'#define SEG_{cs:04X} 0x{(cs + LOAD_SEG) & 0xFFFF:04X}\n')
        for lin in sorted(set(order) | set(drv_lins)):
            f.write(f'void {names[lin]}(CPU *cpu);\n')
            if lin in drv_lins:
                for vn in ['vga'] + [v[0] for v in variants]:
                    f.write(f'void {names[lin]}_{vn}(CPU *cpu);\n')
        for n in sorted(strays):
            f.write(f'void {n}(CPU *cpu);\n')
    with open(os.path.join(out, 'recomp_dispatch.c'), 'w') as f:
        f.write('/* generated by tools/lift.py -- do not commit */\n#include "lifted.h"\n')
        f.write(f'const uint16_t g_load_seg = 0x{LOAD_SEG:04X};\n')
        f.write(f'const uint16_t g_psp_seg = 0x{PSP_SEG:04X};\n')
        f.write(f'const uint16_t g_entry_cs = 0x{im.cs:04X}, g_entry_ip = 0x{im.ip:04X};\n')
        f.write(f'const uint16_t g_entry_ss = 0x{im.ss:04X}, g_entry_sp = 0x{im.sp:04X};\n')
        f.write(f'const char g_game_id[] = "{game}";\n')
        table = sorted(set(order) | set(drv_lins))
        f.write(f'const unsigned g_func_count = {len(table)};\n')
        f.write('const RecompFunc g_funcs[] = {\n')
        for lin in table:
            f.write(f'  {{0x{lin:05X}, {names[lin]}}},\n')
        f.write('};\n')
        # The driver variants: which functions each has, and the first 32 bytes
        # of its code, which src/drv.c matches against what the game loaded.
        vlist = [('vga', im, set(l for l in entries if l in drv_lins))] + \
                [(v[0], v[1], set(l for l in v[2][0] if in_drv(l))) for v in variants]
        if drv_lins:
            for vn, vim, vset in vlist:
                f.write(f'static const RecompFunc drv_{vn}[] = {{\n')
                for lin in sorted(vset):
                    f.write(f'  {{0x{lin:05X}, {names[lin]}_{vn}}},\n')
                f.write('  {0, 0}\n};\n')
        lo = DRV_SEG * 16 + DRV_IP
        f.write('const DrvVariant g_drv_variants[] = {\n')
        for vn, vim, vset in (vlist if drv_lins else []):
            sig = ','.join(str(b) for b in vim.img[lo:lo + 32])
            f.write(f'  {{"{vn}", {{{sig}}}, drv_{vn}, {len(vset)}}},\n')
        f.write('  {0}\n};\n')
        f.write(f'const unsigned g_drv_count = {len(vlist) if drv_lins else 0};\n')
        f.write(f'const uint32_t g_drv_lo = 0x{lo:05X}, g_drv_len = 0x{DRV_LEN:X};\n')
        f.write(f'const uint16_t g_drv_seg = 0x{DRV_SEG:04X};\n')
    # The load module itself, so the built exe needs only the data files: the
    # code is lifted, but the image's data -- DGROUP, the far tables -- is
    # loaded and relocated at boot exactly as DOS would.
    with open(os.path.join(out, 'image.c'), 'w') as f:
        f.write('/* generated by tools/lift.py -- do not commit */' + NL + '#include <stdint.h>' + NL)
        f.write(f'const unsigned g_image_len = {len(im.img)};' + NL)
        f.write(f'const unsigned g_nrelocs = {len(im.relocs)};' + NL)
        f.write('const uint16_t g_relocs[][2] = {' + NL)
        f.write(','.join(f'{{0x{o:X},0x{s:X}}}' for o, s in im.relocs) + NL)
        f.write('};' + NL + 'const uint8_t g_image[] = {' + NL)
        for i in range(0, len(im.img), 32):
            f.write(','.join(str(b) for b in im.img[i:i + 32]) + ',' + NL)
        f.write('};' + NL)
    json.dump({'game': game, 'dgroup': im.dgroup, 'functions': len(order),
               'codesegs': sorted(codesegs), 'unhandled': unhandled},
              open(os.path.join(work, 'lift.json'), 'w'), indent=1)
    if unhandled:
        print(f'  unhandled instruction forms: {sum(unhandled.values())} '
              f'({len(unhandled)} kinds), first: {list(unhandled)[:6]}')
    changed = sync(out, final)
    print(f'  -> {final} ({len(files)} files, {changed} changed)')


def sync(src, dst):
    """Move src's files over dst's, touching only those whose contents
    differ, and drop dst's files src does not have. An unchanged lift then
    leaves every timestamp alone and the next build compiles nothing -- which
    is what lets Setup be re-run cheaply -- and a smaller lift cannot leave a
    bigger one's chunks behind for the build's glob to pick up."""
    changed = 0
    for f in os.listdir(dst):
        if not os.path.exists(os.path.join(src, f)):
            os.remove(os.path.join(dst, f))
            changed += 1
    for f in os.listdir(src):
        a, b = os.path.join(src, f), os.path.join(dst, f)
        if os.path.exists(b) and filecmp.cmp(a, b, shallow=False):
            continue
        shutil.copyfile(a, b)
        changed += 1
    shutil.rmtree(src)
    return changed


def _seal_fallthrough(insts):
    """lift16 emits instructions in list order and lets C fall from one to the
    next. If a fallthrough successor is not the next one in the list -- two
    paths decoded the same bytes at different alignments -- make the edge an
    explicit jmp so C cannot fall into the wrong instruction."""
    out = []
    for i, ins in enumerate(insts):
        out.append(ins)
        m = ins.mnemonic
        if m in TERMINATORS or m in ('jmp', 'jmp far'):
            continue
        nxt = (ins.address + ins.length) & 0xFFFF
        if i + 1 < len(insts) and insts[i + 1].address == nxt:
            continue
        j = decode16.Instruction()
        j.mnemonic, j.address, j.offset, j.length = 'jmp', -1, ins.offset, 0
        j.raw = b''
        j.op1 = decode16.Operand(type=OpType.REL16, disp=nxt, size=2)
        out.append(j)
    return out


if __name__ == '__main__':
    main()
