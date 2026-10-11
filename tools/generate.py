#!/usr/bin/env python3
"""
generate.py - build the star systems, as the 1994 installer did.

    py -3 tools/generate.py               # game/, in FINISH.BAT's order
    py -3 tools/generate.py --s10data     # also the tenth system (see below)

The installer finished by running FINISH.BAT, which ran MAKEDATA, MAKEHMO and
S01DATA .. S19DATA and deleted each. They write the .sky, .cft, .str, .pnt,
.moo, .oid and .cmt files the game reads; without them it stops at
`open sol.str`. Here each is the recompiled program (build/Release/<name>.exe),
run headless on the same DOS machine as the game, with game/ as its folder.

FINISH.BAT says `s10ata`, so DOS answered "Bad command or file name" and the
tenth system (S10DATA) was never generated on a real install. That is kept,
since it is what every copy of the game saw; --s10data runs it as well.

Skipped when the files are already there (sol.str and the last system's star
exist), so a rerun after a failure is cheap; --force runs everything again.
"""
import argparse
import os
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
ORDER = ['makedata', 'makehmo'] + [f's{n:02d}data' for n in range(1, 20)]


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n\n')[0])
    ap.add_argument('--game', default=os.path.join(ROOT, 'game'))
    ap.add_argument('--bin', default=os.path.join(ROOT, 'build', 'Release'))
    ap.add_argument('--s10data', action='store_true', help='also generate the tenth star system')
    ap.add_argument('--force', action='store_true')
    a = ap.parse_args()

    have = {f.lower() for f in os.listdir(a.game)}
    done = {'sol.str', 'deneb.str'} <= have and 's19p01.pnt' in have
    if done and not a.force and (not a.s10data or 's10p01.pnt' in have):
        print('star systems already generated')
        return 0
    for name in ORDER:
        if name == 's10data' and not a.s10data:
            continue                              # FINISH.BAT's typo: `s10ata`
        exe = os.path.join(a.bin, name + ('.exe' if os.name == 'nt' else ''))
        if not os.path.exists(exe):
            sys.exit(f'{exe} is missing: lift and build first (py -3 tools/lift.py all, then cmake)')
        r = subprocess.run([exe, '--headless', '--data', a.game, '--seconds', '60'],
                           capture_output=True, text=True)
        msg = (r.stdout + r.stderr).strip().splitlines()
        err = [m for m in msg if not m.startswith('[')]
        if r.returncode or err:
            sys.exit(f'{name} failed: {" / ".join(err or msg[-3:])}')
        print(f'  {name}')
    print('star systems generated')
    return 0


if __name__ == '__main__':
    sys.exit(main())
