#!/usr/bin/env python3
"""
converge.py - find the routines nothing names statically, by playing the game.

    py -3 tools/converge.py              # play every script in tools/exercise/
    py -3 tools/converge.py --passes 3

Most of the game's code is found by tools/lift.py from the binary alone. What
is left is user-interface code reached only through pointers kept in data
(menu and dialog handlers, list fillers): a dispatch to one of those at run
time is a "miss", and the run records it in work/spacesim/misses.txt. This
plays each input script in tools/exercise/ headless -- deterministic time, so
the same inputs reach the same code -- and while new misses turn up, lifts
again and rebuilds the game. The list stays in work/ (it is derived from the
binary, so it is never committed); the scripts are ours.

Any miss a real session hits later is recorded the same way: run this, or
just `py -3 tools/lift.py` and a build, and it is lifted from then on.
"""
import argparse
import glob
import os
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
# The game's list and SS1.EXE's (the launcher it runs under; tools/lift.py COMPANIONS)
MISSES = [os.path.join(ROOT, 'work', p, 'misses.txt') for p in ('spacesim', 'ss1')]
EXE = os.path.join(ROOT, 'build', 'Release', 'spacesim.exe' if os.name == 'nt' else 'spacesim')


def misses():
    return {(m, l.strip()) for m in MISSES if os.path.exists(m) for l in open(m) if ':' in l}


def script(path):
    """(seconds, key list) from a .keys file: `# seconds N` and key lines
    (`ms:KEY[+holdms]`, comma or newline separated)."""
    secs, keys = 40, []
    for line in open(path):
        line = line.strip()
        if line.startswith('# seconds'):
            secs = float(line.split()[2])
        elif line and not line.startswith('#'):
            keys += [k for k in line.split(',') if k]
    return secs, ','.join(keys)


def play():
    """Every script at once: each run is its own process with its own copy of
    the machine, and each appends what it misses to the same list. They share
    game/ for reading; writes go to a folder per script."""
    procs = []
    for path in sorted(glob.glob(os.path.join(ROOT, 'tools', 'exercise', '*.keys'))):
        secs, keys = script(path)
        # its own folder for what it writes (the model caches it builds), so
        # parallel runs never read each other's half-written files
        save = os.path.join(ROOT, 'work', 'converge', os.path.basename(path)[:-5])
        os.makedirs(save, exist_ok=True)
        cmd = [EXE, '--headless', '--seconds', str(secs), '--save', save] + (['--keys', keys] if keys else [])
        procs.append(subprocess.Popen(cmd, cwd=ROOT, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL))
    for p in procs:
        try:
            p.wait(timeout=1800)
        except subprocess.TimeoutExpired:
            p.kill()


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n\n')[0])
    ap.add_argument('--passes', type=int, default=8)
    a = ap.parse_args()
    py = sys.executable
    for n in range(1, a.passes + 1):
        before = misses()
        play()
        new = misses() - before
        print(f'pass {n}: {len(new)} new routine(s) reached at run time')
        if not new:
            return 0
        subprocess.run([py, os.path.join(ROOT, 'tools', 'lift.py'), 'spacesim', 'ss1'], cwd=ROOT,
                       check=True, capture_output=True)
        subprocess.run(['cmake', '-B', 'build'], cwd=ROOT, check=True, capture_output=True)
        r = subprocess.run(['cmake', '--build', 'build', '--config', 'Release', '--target', 'spacesim'],
                           cwd=ROOT, capture_output=True, text=True)
        if r.returncode:
            sys.exit('rebuild failed:\n' + r.stdout[-3000:])
    print('still finding new routines after the last pass; run it again')
    return 0


if __name__ == '__main__':
    sys.exit(main())
