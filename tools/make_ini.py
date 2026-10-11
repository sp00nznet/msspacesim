#!/usr/bin/env python3
"""
make_ini.py - write SPACESIM.INI the way the game itself would.

    py -3 tools/make_ini.py                         # 800x600 Super VGA (VESA)
    py -3 tools/make_ini.py --driver vga.drv        # the original 320x400 VGA

The 1994 SETUP program wrote this file after asking for the video mode, the
Super VGA chipset, the joystick and the sound. Without it the game stops with
"Error loading 2D driver": the driver's file name is the file's first line.

The game has its own writer for it (it saves your preferences there), and
that routine is a flat list of fields: `mov si, <address> / call <string>`
or `mov bp, <bytes> / mov si, <address> / call <hex>`. This script finds the
writer in your copy of SPACESIM.EXE, reads the field list out of it, and
writes each field's initial value from the program image -- so the file is
the game's own defaults, in the game's own layout, with nothing copied from
the game into this repository. Format: one field per CRLF line, strings as
text, numbers as two hex digits per byte; `;` starts a comment.

Known fields (from what the game does with them):
    1  the 2D driver: vga.drv (320x400), svga.drv (640x400), svga8.drv (800x600)
    2  the Super VGA chipset, SETUP's menu order: 0 VESA 1.2, 1 ATI, 2 Video 7,
       3 Tseng ET4000, 4 Trident 8900, 5 Paradise, 6 S3, 7 Cirrus 542X,
       8 Trident alternate
    3  the situation to start in (FLIGHT)
    13 sound: 0 off, 1 effects, 2 music (SS1.EXE's player; see src/dos.c)
    last-but-one  the music file (danube.mdi)
"""
import argparse
import os
import struct
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, 'tools'))
import lift                                      # noqa: E402  (Image, the toolkit's decoder)


def writer_fields(im):
    """[(kind, ds offset, bytes)] in file order, read out of the game's writer."""
    img = im.img
    name = img.find(b'spacesim.ini\0')
    if name < 0:
        sys.exit('SPACESIM.EXE has no spacesim.ini name; is this the 1994 release?')
    cs = im.cs                                    # the writer lives in the entry segment
    # mov ah, 3Ch / mov dx, <name> : create the file
    pat = bytes((0xB4, 0x3C, 0xBA)) + struct.pack('<H', name)
    at = img.find(pat, cs * 16, cs * 16 + 0x10000)
    if at < 0:
        sys.exit('could not find the spacesim.ini writer in SPACESIM.EXE')
    ip = at - cs * 16
    calls = {}
    fields, bp, si = [], 1, None
    for _ in range(400):
        ins = im.decode(cs, ip)
        ip += ins.length
        m = ins.mnemonic
        if m == 'mov' and repr(ins.op1) == 'bp':
            bp = ins.op2.disp & 0xFFFF
        elif m == 'mov' and repr(ins.op1) == 'si':
            si = ins.op2.disp & 0xFFFF
        elif m == 'call' and si is not None:
            t = ins.op1.disp & 0xFFFF
            calls.setdefault(t, 'str' if not calls else 'hex')   # the first callee writes a string
            fields.append((calls[t], si, bp))
            bp, si = 1, None
        elif m in ('ret', 'jmp'):
            break
    if len(calls) != 2 or not fields:
        sys.exit(f'unexpected shape for the spacesim.ini writer ({len(calls)} callees)')
    return fields


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n\n')[0])
    ap.add_argument('--exe', default=os.path.join(ROOT, 'game', 'SPACESIM.EXE'))
    ap.add_argument('--out', default=os.path.join(ROOT, 'game', 'SPACESIM.INI'))
    # 800x600 by default: the remaster. The game shipped 320x400 as its default
    # because most 1994 cards needed a chipset-specific driver for more.
    ap.add_argument('--driver', default='svga8.drv', choices=('vga.drv', 'svga.drv', 'svga8.drv'))
    ap.add_argument('--chipset', type=int, default=0, help='0 = VESA 1.2, which src/vga.c answers')
    ap.add_argument('--situation', default='FLIGHT', help='the .STN file to start in, without .STN')
    ap.add_argument('--sound', default='music', choices=('off', 'effects', 'music'))
    a = ap.parse_args()

    im = lift.Image(a.exe)
    fields = writer_fields(im)
    img = im.img
    lines = []
    for n, (kind, off, size) in enumerate(fields):
        if kind == 'str':
            text = img[off:img.index(b'\0', off)].decode('ascii')
            if n == 0:
                text = a.driver
            if n == 2:
                text = a.situation.upper()
            lines.append(text)
        else:
            val = bytearray(img[off:off + size])
            if n == 1:
                val[0] = a.chipset & 0xFF
            if n == 12:
                val[0] = ('off', 'effects', 'music').index(a.sound)
            lines.extend(f'{v:02X}' for v in val)           # one byte per line, as the game writes
    open(a.out, 'wb').write(('\r\n'.join(lines) + '\r\n').encode('ascii'))
    print(f'{a.out}: {len(fields)} fields, driver {a.driver}, chipset {a.chipset}, sound {a.sound}')


if __name__ == '__main__':
    main()
