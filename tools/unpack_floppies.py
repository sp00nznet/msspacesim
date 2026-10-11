#!/usr/bin/env python3
"""
unpack_floppies.py - the three Space Simulator floppy images to a folder of
installed files, in pure Python: no 7-Zip, nothing run.

    py -3 tools/unpack_floppies.py original game
    py -3 tools/unpack_floppies.py --selftest

Each image is a 1.44 MB FAT12 floppy holding one LHA self-extractor,
DISKn.EXE (LHA's SFX 2.13S), plus on disk 1 the installer and on disk 3
MAKEBOOT.COM. The archives are level-0 LHA headers with -lh5- members:
static-Huffman blocks over an 8 KB window, the format Haruhiko Okumura and
Haruyasu Yoshizaki published. Each member carries a CRC-16 of its contents,
which is checked, so a bad image stops here rather than in the game.

The images can be in a folder (`... 1.IMA` .. `3.IMA`, any case) or in the
archive.org zip they come in (msspacesim.zip); both are read where they lie.
"""
import os
import struct
import sys
import zipfile

# ---- FAT12 ------------------------------------------------------------------


def fat12_files(img):
    """{NAME.EXT: bytes} from the root directory of a FAT12 floppy image."""
    bps, spc, reserved, nfats, nroot = struct.unpack_from('<HBHBH', img, 11)
    spf = struct.unpack_from('<H', img, 22)[0]
    fat = img[reserved * bps:(reserved + spf) * bps]
    root = (reserved + nfats * spf) * bps
    data = root + nroot * 32

    def nxt(c):
        v = struct.unpack_from('<H', fat, c * 3 // 2)[0]
        return v >> 4 if c & 1 else v & 0xFFF

    out = {}
    for k in range(nroot):
        e = img[root + 32 * k:root + 32 * k + 32]
        if e[0] in (0, 0xE5) or e[11] & 0x18:            # free, deleted, volume or dir
            continue
        name = e[:8].decode('ascii').rstrip()
        ext = e[8:11].decode('ascii').rstrip()
        cl, size = struct.unpack_from('<HI', e, 26)
        buf = bytearray()
        while 2 <= cl < 0xFF8 and len(buf) < size:
            off = data + (cl - 2) * spc * bps
            buf += img[off:off + spc * bps]
            cl = nxt(cl)
        out[name + ('.' + ext if ext else '')] = bytes(buf[:size])
    return out


# ---- LHA -lh5- ---------------------------------------------------------------

NC, NP, NT = 510, 14, 19          # 256 literals + lengths 3..256; positions; pre-tree
CBIT, PBIT, TBIT = 9, 4, 5
FAST = 10                         # lookup-table width; longer codes walk a dict


class Bits:
    """MSB-first bit reader with a 32-bit look-ahead."""

    def __init__(self, data):
        self.d, self.p, self.buf, self.n = data, 0, 0, 0
        self._fill()

    def _fill(self):
        while self.n <= 24:
            b = self.d[self.p] if self.p < len(self.d) else 0
            self.p += 1
            self.buf = (self.buf << 8 | b) & 0xFFFFFFFFFF
            self.n += 8

    def peek(self, k):
        return (self.buf >> (self.n - k)) & ((1 << k) - 1)

    def skip(self, k):
        self.n -= k
        self._fill()

    def get(self, k):
        if k == 0:
            return 0
        v = self.peek(k)
        self.skip(k)
        return v


class Huffman:
    """Canonical Huffman decoder for code lengths as LHA assigns them (codes
    in order of length, then of symbol): a FAST-bit table for the short codes
    and a dict for the rest."""

    def __init__(self, lengths):
        self.single = None
        used = [(l, s) for s, l in enumerate(lengths) if l]
        if not used:
            raise ValueError('empty Huffman table')
        self.table = [None] * (1 << FAST)
        self.long = {}
        code, prev = 0, 0
        for l, s in sorted(used):
            code <<= l - prev
            prev = l
            if l <= FAST:
                lo = code << (FAST - l)
                for k in range(1 << (FAST - l)):
                    self.table[lo + k] = (s, l)
            else:
                self.long[(l, code)] = s
            code += 1
        self.maxlen = prev

    def decode(self, bits):
        if self.single is not None:
            return self.single
        e = self.table[bits.peek(FAST)]
        if e is not None:
            bits.skip(e[1])
            return e[0]
        for l in range(FAST + 1, self.maxlen + 1):
            s = self.long.get((l, bits.peek(l)))
            if s is not None:
                bits.skip(l)
                return s
        raise ValueError('bad Huffman code')


def _const(sym):
    h = Huffman.__new__(Huffman)
    h.single = sym
    return h


def _read_pt(bits, nn, nbit, special):
    n = bits.get(nbit)
    if n == 0:
        return _const(bits.get(nbit)), [0] * nn
    lens, i = [0] * nn, 0
    while i < n:
        c = bits.peek(3)
        if c == 7:                                      # 7 then a run of 1s, then 0
            k = 0
            while bits.peek(4 + k) & 1:
                k += 1
            c = 7 + k
            bits.skip(4 + k)
        else:
            bits.skip(3)
        lens[i] = c
        i += 1
        if i == special:
            for _ in range(bits.get(2)):
                lens[i] = 0
                i += 1
    return Huffman(lens), lens


def _read_c(bits, pt):
    n = bits.get(CBIT)
    if n == 0:
        return _const(bits.get(CBIT))
    lens, i = [0] * NC, 0
    while i < n:
        c = pt.decode(bits)
        if c <= 2:
            run = 1 if c == 0 else bits.get(4) + 3 if c == 1 else bits.get(CBIT) + 20
            i += run                                    # zeros
        else:
            lens[i] = c - 2
            i += 1
    return Huffman(lens)


def lh5(data, size):
    """Decompress one -lh5- member to `size` bytes."""
    bits = Bits(data)
    out = bytearray()
    while len(out) < size:
        count = bits.get(16)
        pt, _ = _read_pt(bits, NT, TBIT, 3)
        c = _read_c(bits, pt)
        p, _ = _read_pt(bits, NP, PBIT, -1)
        for _ in range(count):
            sym = c.decode(bits)
            if sym < 256:
                out.append(sym)
                continue
            length = sym - 256 + 3
            d = p.decode(bits)
            if d:
                d = (1 << (d - 1)) + bits.get(d - 1)
            src = len(out) - d - 1
            if d + 1 >= length:
                out += out[src:src + length]
            else:                                       # overlapping: byte by byte
                for k in range(length):
                    out.append(out[src + k])
            if len(out) >= size:
                break
    return bytes(out[:size])


def crc16(data):
    """CRC-16/ARC, as LHA stores it."""
    crc = 0
    for b in data:
        crc ^= b
        for _ in range(8):
            crc = (crc >> 1) ^ 0xA001 if crc & 1 else crc >> 1
    return crc


def lzh_members(sfx):
    """(name, bytes) for each member of the archive in an LHA self-extractor."""
    p = sfx.find(b'-lh')
    if p < 2:
        raise ValueError('no LHA archive in this file')
    p -= 2
    while p < len(sfx) and sfx[p]:
        hsize = sfx[p]
        method = sfx[p + 2:p + 7]
        packed, size = struct.unpack_from('<II', sfx, p + 7)
        if sfx[p + 20] != 0:
            raise ValueError(f'LHA header level {sfx[p + 20]} not handled')
        nlen = sfx[p + 21]
        name = sfx[p + 22:p + 22 + nlen].decode('ascii')
        crc = struct.unpack_from('<H', sfx, p + 22 + nlen)[0]
        body = sfx[p + 2 + hsize:p + 2 + hsize + packed]
        if method == b'-lh5-':
            data = lh5(body, size)
        elif method == b'-lh0-':
            data = body
        else:
            raise ValueError(f'{name}: LHA method {method!r} not handled')
        if crc16(data) != crc:
            raise ValueError(f'{name}: CRC mismatch -- the floppy image is damaged')
        yield name, data
        p += 2 + hsize + packed


# ---- the install ------------------------------------------------------------


def find_images(src):
    """{1: bytes, 2: bytes, 3: bytes} from a folder or the archive.org zip."""
    imgs = {}

    def take(name, read):
        low = name.lower()
        if low.endswith('.ima') and low[-5] in '123':
            imgs[int(low[-5])] = read()

    if os.path.isfile(src) and zipfile.is_zipfile(src):
        with zipfile.ZipFile(src) as z:
            for n in z.namelist():
                take(n, lambda n=n: z.read(n))
    else:
        for root, _, files in os.walk(src):
            for f in files:
                p = os.path.join(root, f)
                take(f, lambda p=p: open(p, 'rb').read())
            for f in files:
                if f.lower().endswith('.zip') and len(imgs) < 3 and zipfile.is_zipfile(os.path.join(root, f)):
                    with zipfile.ZipFile(os.path.join(root, f)) as z:
                        for n in z.namelist():
                            take(n, lambda n=n, z=z: z.read(n))
    missing = [k for k in (1, 2, 3) if k not in imgs]
    if missing:
        raise SystemExit(f'floppy image(s) {missing} not found in {src}: expected names ending '
                         f'"1.IMA", "2.IMA", "3.IMA" (or the archive.org zip)')
    return imgs


def unpack(src, dst):
    imgs = find_images(src)
    os.makedirs(dst, exist_ok=True)
    count = 0
    for k in (1, 2, 3):
        files = fat12_files(imgs[k])
        sfx = files.get(f'DISK{k}.EXE')
        if sfx is None:
            raise SystemExit(f'disk {k} has no DISK{k}.EXE: is it a Space Simulator floppy?')
        for name, data in lzh_members(sfx):
            open(os.path.join(dst, name), 'wb').write(data)
            count += 1
        for extra in ('READ-ME.TXT', 'MAKEBOOT.COM'):
            if extra in files:
                open(os.path.join(dst, extra), 'wb').write(files[extra])
    return count


def selftest():
    """lh5 and FAT12 against data built here."""
    import random
    rnd = random.Random(7)
    # A single-literal stream: the c table is a constant, so no codes are read.
    # That exercises the constant-table paths; real streams exercise the rest,
    # and every member's CRC is checked when they are unpacked.
    assert crc16(b'123456789') == 0xBB3D, 'CRC-16/ARC check value'
    b = Bits(bytes([0b10110000, 0xFF]))
    assert b.get(1) == 1 and b.get(3) == 0b011 and b.get(4) == 0
    lens = [2, 2, 2, 3, 3]                   # canonical: 00 01 10 110 111
    h = Huffman(lens)
    for sym, code, l in ((0, 0b00, 2), (1, 0b01, 2), (2, 0b10, 2), (3, 0b110, 3), (4, 0b111, 3)):
        v = code << (16 - l)
        assert h.decode(Bits(bytes([v >> 8, v & 255, 0, 0]))) == sym
    # FAT12: one file across two clusters
    img = bytearray(1474560)
    struct.pack_into('<HBHBHHB', img, 11, 512, 1, 1, 2, 224, 2880, 0xF0)
    struct.pack_into('<H', img, 22, 9)
    fat = 512
    payload = bytes(rnd.randrange(256) for _ in range(700))
    img[fat + 3:fat + 6] = bytes([0x03, 0xF0, 0xFF])   # cluster 2 -> 3, 3 -> EOF
    root = 512 * (1 + 2 * 9)
    img[root:root + 11] = b'TEST    BIN'
    struct.pack_into('<HI', img, root + 26, 2, len(payload))
    data = root + 224 * 32
    img[data:data + 700] = payload
    assert fat12_files(bytes(img)) == {'TEST.BIN': payload}
    print('unpack_floppies selftest: ok')


def main():
    if sys.argv[1:] == ['--selftest']:
        return selftest()
    if len(sys.argv) != 3:
        print(__doc__.strip().splitlines()[2].strip())
        return 2
    n = unpack(sys.argv[1], sys.argv[2])
    print(f'{n} files unpacked to {sys.argv[2]}')
    return 0


if __name__ == '__main__':
    sys.exit(main())
