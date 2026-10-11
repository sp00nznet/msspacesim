# Architecture

```
original/*.IMA ─► game/ (unpacked, plus the generated star systems)
                    │
game/SPACESIM.EXE ──┼─► tools/lift.py ──► work/spacesim/gen/*.c ─┐
game/S??DATA.EXE ...┘     unlzexe / unexepack (pcrecomp)          ├─► MSVC ─► spacesim.exe
                          discover, lift16                        │          makedata.exe ...
                                         src/*.c (DOS machine) ───┘
```

## The parts

| Part | Owns | Code |
|------|------|------|
| Lift driver | Unpacking, finding every function, handing each to the lifter, writing the build tree; one tree per program | [`tools/lift.py`](../tools/lift.py) |
| pcrecomp | The x86-16 decoder, the lifter, the CPU model the C compiles against, the unpackers | [sp00nznet/pcrecomp](https://github.com/sp00nznet/pcrecomp) |
| Generated code | One C function per guest function, the address table for indirect calls, the load image | `work/<program>/gen/`, local only |
| DOS + BIOS | Boot (PSP, environment, relocation), files, memory, INT 10h/16h/1Ah/33h, the EMMXXXX0 device | [`src/dos.c`](../src/dos.c) |
| EMS | LIM EMS 4.0 behind INT 67h; mapping copies pages in and out of the E000 frame | [`src/ems.c`](../src/ems.c) |
| Machine | PIC, PIT, keyboard controller, interrupt delivery, indirect dispatch, debug snapshots | [`src/machine.c`](../src/machine.c) |
| VGA | Planar memory, latches, write modes, DAC, CRTC scanout (320x400 unchained), text mode | [`src/vga.c`](../src/vga.c) |
| Audio | OPL2 (ymfm), Sound Blaster DSP + DMA, PC speaker | [`src/audio.c`](../src/audio.c) |
| Host | Win32 window and settings menu, raw input, XInput, waveOut; or headless frames to ffmpeg | [`src/host.c`](../src/host.c) |
| Presenter | Direct3D 11 scaling shaders (sharp-bilinear, CRT, ...), GDI fallback in host.c | [`src/present_d3d.c`](../src/present_d3d.c) |
| Save states | The machine to a file and back, at a hook the host stack can be matched at | [`src/state.c`](../src/state.c) |
| Companion | SS1.EXE, lifted with prefixed symbols and linked into spacesim.exe | [`src/companion_ss1.c`](../src/companion_ss1.c) |
| Tools | `make_ini.py` (the settings SETUP wrote), `fdiff.py` (function-level differential runs) | [`tools/`](../tools) |

The DOS machine is one static library (`dosmachine`) linked into the game and
into each install tool, so the generators run on exactly what the game does.

## How a guest function becomes a C function

As in the Blake Stone recomp this started from: each code segment is decoded
as a 64 KB window, a function is the closure of everything reachable from its
entry without a call, and calls become C calls that push a sentinel return
address so stack-relative arguments sit where the guest expects them.
Indirect calls and jumps go through `recomp_dispatch` / `dispatch_near` /
`dispatch_far` and a sorted address table. What this game needed beyond that
is in [lifting.md](lifting.md).

## Memory

A flat 1 MB + 64 KB array, real-mode addressing.

| Linear | What |
|--------|------|
| `00000` | Interrupt vectors; unhooked ones point into `F000`, where `F000:nn` means "the BIOS/DOS handler for vector nn" |
| `00400` | BIOS data area |
| `01000` | The game's PSP (`0100`) |
| `01100` | The game's image, relocated by `0110` as DOS's loader would |
| … `96000` | The DOS arena: the game's heap |
| `96000` | SS1.EXE: its PSP (`9600`), image and the music player that stays resident |
| `A0000` | VGA window, routed to the planar model |
| `E0000` | The EMS page frame |

## Time

Lifted code cannot be interrupted, so every loop back-edge counts down a
budget and calls `recomp_tick()`, which advances emulated time, delivers the
timer and keyboard interrupts that fell due, and lets the host present.
Headless, time is deterministic (300 µs per tick), so two runs with the same
inputs produce the same frames; in a window it follows the wall clock.

## Files

The game reads and writes its own folder, `game/`, as it did in `C:\SPACESIM`:
preferences, saved situations and the model caches it builds there
(`*.tci`, `*.tsi`). `game/` is the install made from the floppy images, which
stay untouched in `original/`.

## Two programs: SS1.EXE and the game

The player ran `SS1.EXE`, not `SPACESIM.EXE`: it reads the sound preference
from `SPACESIM.INI`, finds the AdLib, loads the music into four EMS pages,
hooks INT 8 and INT 2Fh, then runs the game with `system()`. The game asks
INT 2Fh AX=4D52h for the player and calls the far entry point it returns, with
a function in BX: 0 the timer, 1 play a `.MDI`, 2 stop, 3 install the game's
timer routine, 4-7 the engine and effect sounds.

Both programs are lifted (`tools/lift.py COMPANIONS`); SS1's functions and
tables carry an `ss1_` prefix, and it is linked into `spacesim.exe` as its
own library. `dos_boot` loads SS1 at `9600`; its EXEC of `COMMAND.COM /C
SPACESIM.EXE` loads the game at `0100`, where the game's own lift expects it,
and calls the game's entry. Dispatch goes to whichever program owns the
segment. SS1's C runtime walks DOS's memory control blocks before it spawns,
so `src/dos.c` keeps a real MCB chain in guest memory.

## Save states

The lifted code keeps the guest's call chain on the host's stack, so a state
can only be loaded where the host's stack is the same as when it was saved.
`state_point()` runs on the way into every BIOS/DOS service; a state records
the host's return addresses there (`RtlCaptureStackBackTrace`, relative to the
exe), the host stack depth and the guest's SS:SP, and a load waits for a call
that matches all three -- the same place in the game, usually within a frame
-- then overwrites the machine and returns into the same frames. Guest time is
not saved; it runs on. The OPL's registers are saved and written back.

## Presenting

`vga_compose` turns VGA or Super VGA memory into 32-bit pixels at the mode's
resolution; the host lays them out 4:3 (or at whole multiples, for integer
scaling) and `d3d_present` scales them with a pixel shader. Windowed, frames
are presented at most 70 times a second of real time; the guest's speed is
the speed control times the wall clock.
