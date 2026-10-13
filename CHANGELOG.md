# Changelog

All notable changes to this project. Format: [Keep a Changelog](https://keepachangelog.com/en/1.1.0/);
versions follow [SemVer](https://semver.org/).

## [Unreleased]

### Fixed
- The game's clock ran minutes ahead: timer 0's counter was out of phase with
  IRQ 0, and the game adds the two together.

### Added
- The game port: an analog joystick at 201h, timed by reads as the game
  times it, driven by the Xbox controller's left stick (the d-pad stays the
  arrows). `--keys` takes `JX`/`JY` for scripted runs.
- `tools/lift.py`: handler tables loaded into a variable and jumped through
  later (`mov ax, [bx+table] / mov [var], ax ... jmp [var]`) -- the
  joystick's per-Control-mode handlers.
- `--mod NAME`: a folder in `mods\` laid over the game's files, read-only;
  a mod's situations show in Open Situation.
- `CONTRIBUTING.md`, `SECURITY.md`.
- `tools/lift.py`: lifts `SPACESIM.EXE` and the 21 install-time data
  generators to C with pcrecomp. Finds BAO's hand-made far calls
  (`push cs / push label / push 0 / jmp far`, and the `retf` form), handler
  tables in the data segment (the SMDL interpreter's opcode table, read to the
  bound its `cmp` states), the 2D driver's installed entry points, and maps
  run-time dispatch misses to the code segment that holds them.
- A DOS machine for the game (`src/`), started from the Blake Stone recomp's:
  DOS and BIOS services, PIC/PIT/keyboard, a planar VGA (320x400 unchained),
  and expanded memory (`src/ems.c`, LIM EMS 4.0 behind the EMMXXXX0 device).
- `tools/unpack_floppies.py`: the floppy images (or their zip) to the installed
  files in pure Python: FAT12, LHA level-0 `-lh5-` with each member's CRC
  checked. Byte-identical to 7-Zip's extraction on all 375 files.
- `tools/generate.py`: the installer's data generators in `FINISH.BAT`'s order
  (S10DATA skipped as the batch file's typo skipped it; `--s10data` adds it).
- `Setup.cmd`: the Quick start, from floppy images to a shortcut.
- `tools/make_ini.py`: writes `SPACESIM.INI` from the game's own writer
  routine and defaults, as the 1994 SETUP program did.
- `tools/fdiff.py`: runs a lifted function's original bytes under Unicorn
  from a snapshot of a real run and compares memory, registers and VGA planes
  with what the lifted code left.
- Super VGA: `SVGA.DRV` (640x400) and `SVGA8.DRV` (800x600) lifted as variants
  of the 2D driver region, switched by which driver the game loads
  (`src/drv.c`), over a VESA 1.2 model in `src/vga.c` (VBE 00h-07h, banked
  1 MB framebuffer, the window function). 800x600 is now the default.
- `tools/converge.py` and `tools/exercise/`: scripted headless play that
  relifts the code only reached through pointers in data; Setup runs it.
- Divide faults reach the game's own INT 0 handler with the frame the CPU
  would push (the faulting CS:IP), so it can saturate the quotient and step
  over the instruction as it does on DOS.
- A Direct3D 11 presenter (`src/present_d3d.c`, from the Theme Park recomp):
  sharp-bilinear, smooth, CRT, nearest and integer scaling, vivid colour,
  borderless fullscreen, GDI as the fallback.
- A settings menu on F11 (save states, window size, scaling, speed, vivid
  colour, fullscreen, mute, screenshot), F12 to cycle scaling, Shift+F12 for
  a screenshot; choices kept in `settings.ini` beside the exe.
- Save states (`src/state.c`): nine slots holding the whole machine, loaded
  at the same place in the game they were saved. `SAVEn`/`LOADn` in `--keys`.
- Speed control: the wall clock at 0.5x to 4x.
- Music and sound effects: `SS1.EXE`, the launcher the player ran, is lifted
  as a companion program linked into `spacesim.exe`. It installs Microsoft's
  AdLib player and runs the game (INT 21h EXEC), and the game calls the
  player for its music (`.MDI`) or engine sounds, as on DOS.
  `tools/make_ini.py --sound music|effects|off` (music by default) and
  `--situation NAME`.
- The DOS model keeps memory control blocks in memory, answers EMS 47h/48h,
  INT 21h 29h and EXEC, and knows which program's PSP is current.
- `tools/exercise/50-situations.keys`: converge runs every shipped situation.
- Debug builds (`-DSPACESIM_TRACE=ON`): `SPACESIM_SNAP`, `SPACESIM_WATCH`,
  `SPACESIM_CALLS`, a stack-depth guard naming runaway recursions.

### Toolkit changes this needed (pcrecomp, open as pull requests)
- lift16: hand-made calls (`push cs / push label / ... / jmp` or `retf`).
- drm: unpack Microsoft EXEPACK (`unexepack.py`), for the S01–S19DATA tools.
- decode16/lift16: `bsf` and `bsr`.
- lift16: `rcl`/`rcr` on a dword kept the carry in a 32-bit value and lost it.
- lift16: `bt`-family with a register offset into memory addresses a bit string.
- lift16: return when a callee has unwound the guest stack past the frame (opt-in).
- lift16: a divide faults on quotient overflow too, with cs:ip at the divide.
