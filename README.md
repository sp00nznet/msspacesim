# Microsoft Space Simulator, recompiled

A static recompilation of **Microsoft Space Simulator** (1994, Bruce Artwick
Organization / Microsoft Home) from its DOS executable to a native Windows
program. The game's 16-bit real-mode code, most of it hand-written 386
assembly, is translated to C by [pcrecomp](https://github.com/sp00nznet/pcrecomp)
and runs on a small DOS machine in `src/`. No DOSBox, no emulator. You supply
the three floppy images; nothing from the game is in this repository.

Part of the [pcrecomp](https://github.com/sp00nznet/pcrecomp) family, built in
its shared house style (the same pipeline, flags, headless recording and
harness as the other recomp repos).

## Status

**v0.1.0-dev, pre-alpha.** It boots to the simulator and the simulation runs.

| | |
|---|---|
| Install from the floppies | the 21 data generators the 1994 installer ran (MAKEDATA, MAKEHMO, S01–S19DATA) are recompiled too, and build the 543 star, planet and sky files |
| Microsoft Home logo, title screen | ✅ |
| Simulator: view window, star field, instrument panel, mission clock | ✅ renders and runs (seen headless) |
| Expanded memory (EMS 4.0) | ✅ the game's 40 pages, bank switching |
| Menus, dialogs, keyboard | ✅ menus, the Location dialogs (deep sky, stars, planets, comets, surface), Open Situation, Arrange Windows; scripted keys in headless runs |
| 3D models | ✅ flat-shaded (an asteroid in the Alpha Centauri system, seen headless) |
| Mouse / joystick in play | not yet verified |
| Sound and music | not yet (the AdLib player is a separate TSR, `SS1.EXE`) |
| Super VGA 640×400 and 800×600 | ✅ 800×600 by default: the driver files are lifted as variants of the driver region, over a VESA 1.2 model |
| Lift | 1,157 functions, 58,299 instructions, 3 code segments |

Nobody has played it with hands on a keyboard yet. Treat it as pre-alpha.

## Screenshots

Not committed yet. Every frame so far comes from headless runs
(`--headless --shot-at`), which is how they will be made.

## Getting Started

You need your own copy: the three 1.44 MB floppy images (`Microsoft Space
Simulator 1.IMA` … `3.IMA`), loose or in the zip they often come in.

### Quick start

1. Download this repository (Code → Download ZIP) and unpack it anywhere.
2. Put the floppy images (or their zip) in its `original` folder.
3. Double-click **`Setup.cmd`**.

Setup checks for Python 3, CMake, Git and the Visual Studio 2022 C++ build
tools and offers to install whatever is missing (it says what, why and how big
first). It finds the images in `original\`, the repository folder or your
Downloads, or asks. Then it runs the steps below and leaves
**`Microsoft Space Simulator.lnk`** in the folder. Building takes a few
minutes. If anything fails it stops with one sentence saying what to do, and
the details are in `setup.log`; running it again carries on from there.

### Step by step

Prerequisites: Windows 10/11; Visual Studio 2022 with "Desktop development
with C++" (or the Build Tools); CMake 3.20+; Git; Python 3.10+ with
`pip install capstone`. Use `py -3` -- a bare `python` can be the Microsoft
Store alias, which only opens the Store. Open a new terminal after installing
anything, so PATH is current.

1. The toolkit, at the commit this needs (its changes are open pull requests):
   ```
   git clone https://github.com/sp00nznet/pcrecomp work\pcrecomp
   git -C work\pcrecomp checkout 357136a
   ```
2. Unpack the floppy images (or their zip) into `game\`:
   ```
   py -3 tools\unpack_floppies.py original game
   ```
   `375 files unpacked to game` -- each archive member's CRC is checked.
3. Lift the game and the 21 install tools to C (`work\*\gen`, never committed):
   ```
   py -3 tools\lift.py all
   ```
   The first line should read `SPACESIM.EXE: ... 3 code segments, 1157 functions`
   or close to it.
4. Build:
   ```
   cmake -B build -G "Visual Studio 17 2022" -A x64
   cmake --build build --config Release -- -m
   ```
5. Generate the star systems, as the installer did:
   ```
   py -3 tools\generate.py
   ```
   It runs the recompiled MAKEDATA, MAKEHMO and S01–S19DATA and ends with
   `star systems generated`. S10DATA is skipped, exactly as the original
   `FINISH.BAT`'s typo (`s10ata`) skipped it; `--s10data` adds that system.
6. Write the settings the 1994 SETUP program wrote:
   ```
   py -3 tools\make_ini.py
   ```
   and a two-line `game\SETUP.INI` (your name, then a product ID).
7. Play the scripted inputs in `tools\exercise\` headless, lifting and
   rebuilding until no new code turns up:
   ```
   py -3 tools\converge.py
   ```
   Some user-interface code is only named by pointers in data, so the lifter
   finds it when a run reaches it (`work\spacesim\misses.txt`, never
   committed). Each pass prints how many new routines it found; it stops at 0.
8. Run `build\Release\spacesim.exe`.

## Usage

```
spacesim [options]
  --headless          no window, no audio device; deterministic time
  --record out.mp4    video + audio through ffmpeg
  --shot-at MS:FILE   frames at emulated times, e.g. 20000:title.bmp
  --seconds N         stop after N seconds of emulated time
  --keys MS:KEY,...   scripted input
  --trace             DOS calls to stderr
```

The full list is at the top of [`src/main.c`](src/main.c).

## Building from source

Steps 3 and 4 above. The lifter and its decisions are described in
[docs/lifting.md](docs/lifting.md); the parts and how they fit in
[docs/architecture.md](docs/architecture.md).

## License

MIT, see [LICENSE](LICENSE); third-party code is listed in [NOTICE](NOTICE).
That covers this repository's own code only. Microsoft Space Simulator is
Microsoft's; the lifted C is generated on your machine from your copy and is
never distributed.
