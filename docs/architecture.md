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
| Host | Win32 window, raw input, waveOut; or headless frames to ffmpeg | [`src/host.c`](../src/host.c) |
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
| `01000` | PSP (`0100`) |
| `01100` | The program image, relocated by `0110` as DOS's loader would |
| … `A0000` | The DOS arena |
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
