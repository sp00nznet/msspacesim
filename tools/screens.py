"""screens.py - the README's screenshots and hero GIF, made headless.

    py -3 tools/screens.py            # all of them, into docs/screenshots/

Each shot starts the game in a shipped situation (SPACESIM.INI in a folder of
its own under work/screens/, written by make_ini.py) and grabs a frame at a
fixed point in emulated time; headless time is deterministic, so the same
build makes the same pictures. The GIF is a --record run cut down by ffmpeg
(on PATH). Needs build/Release/spacesim.exe and an installed game/.
"""
import os
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
EXE = os.path.join(ROOT, 'build', 'Release', 'spacesim.exe')
OUT = os.path.join(ROOT, 'docs', 'screenshots')

# name, situation, ms of emulated time, scripted keys
SHOTS = [
    ('title', 'FLIGHT', 3000, ''),
    ('mars-base', 'MARSBASE', 20000, ''),
    ('alpha-centauri', 'ALPHACEN', 20000, ''),
    ('lunar-module', '1972', 20000, ''),
    ('voyager-jupiter', 'FLYBY', 20000, ''),
    ('milky-way', 'MILKYWAY', 20000, ''),
]
# the hero: situation, start and length in seconds, keys
HERO = ('APROACH', 12, 8, '')


def run(name, sit, seconds, extra, keys):
    save = os.path.join(ROOT, 'work', 'screens', name)
    os.makedirs(save, exist_ok=True)
    subprocess.run([sys.executable, os.path.join(ROOT, 'tools', 'make_ini.py'), '--situation', sit,
                    '--out', os.path.join(save, 'SPACESIM.INI')], check=True, capture_output=True)
    cmd = [EXE, '--headless', '--mute', '--save', save, '--seconds', str(seconds)] + extra
    if keys:
        cmd += ['--keys', keys]
    subprocess.run(cmd, cwd=ROOT, check=True, capture_output=True)
    return save


def main():
    os.makedirs(OUT, exist_ok=True)
    for name, sit, ms, keys in SHOTS:
        save = run(name, sit, ms / 1000 + 0.5, ['--shot-at', f'{ms}:{os.path.join("work", "screens", name + ".bmp")}'], keys)
        bmp = os.path.join(ROOT, 'work', 'screens', name + '.bmp')
        subprocess.run(['ffmpeg', '-loglevel', 'error', '-y', '-i', bmp, os.path.join(OUT, name + '.png')], check=True)
        print(f'{name}.png ({sit} at {ms / 1000:g} s)')
    sit, start, length, keys = HERO
    mp4 = os.path.join(ROOT, 'work', 'screens', 'hero.mp4')
    run('hero', sit, start + length + 1, ['--record', mp4], keys)
    gif = os.path.join(OUT, 'hero.gif')
    vf = 'fps=15,scale=640:-1:flags=lanczos,split[a][b];[a]palettegen=max_colors=128[p];[b][p]paletteuse=dither=bayer'
    subprocess.run(['ffmpeg', '-loglevel', 'error', '-y', '-ss', str(start), '-t', str(length), '-i', mp4,
                    '-vf', vf, gif], check=True)
    print(f'hero.gif ({sit}, {length} s from {start} s, {os.path.getsize(gif) // 1024} KB)')
    return 0


if __name__ == '__main__':
    sys.exit(main())
