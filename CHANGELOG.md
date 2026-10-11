# Changelog

All notable changes to this project. Format: [Keep a Changelog](https://keepachangelog.com/en/1.1.0/);
versions follow [SemVer](https://semver.org/).

## [Unreleased]

### Added
- `tools/lift.py`: lifts `SPACESIM.EXE` and the 21 install-time data
  generators to C with pcrecomp. Finds BAO's hand-made far calls
  (`push cs / push label / push 0 / jmp far`, and the `retf` form), handler
  tables in the data segment (the SMDL interpreter's opcode table, read to the
  bound its `cmp` states), the 2D driver's installed entry points, and maps
  run-time dispatch misses to the code segment that holds them.
- A DOS machine for the game (`src/`), started from the Blake Stone recomp's:
  DOS and BIOS services, PIC/PIT/keyboard, a planar VGA (320x400 unchained),
  and expanded memory (`src/ems.c`, LIM EMS 4.0 behind the EMMXXXX0 device).
- `tools/make_ini.py`: writes `SPACESIM.INI` from the game's own writer
  routine and defaults, as the 1994 SETUP program did.
- `tools/fdiff.py`: runs a lifted function's original bytes under Unicorn
  from a snapshot of a real run and compares memory, registers and VGA planes
  with what the lifted code left.
- Debug builds (`-DSPACESIM_TRACE=ON`): `SPACESIM_SNAP`, `SPACESIM_WATCH`,
  `SPACESIM_CALLS`, a stack-depth guard naming runaway recursions.

### Toolkit changes this needed (pcrecomp, open as pull requests)
- lift16: hand-made calls (`push cs / push label / ... / jmp` or `retf`).
- drm: unpack Microsoft EXEPACK (`unexepack.py`), for the S01–S19DATA tools.
- decode16/lift16: `bsf` and `bsr`.
- lift16: `rcl`/`rcr` on a dword kept the carry in a 32-bit value and lost it.
- lift16: `bt`-family with a register offset into memory addresses a bit string.
- lift16: return when a callee has unwound the guest stack past the frame (opt-in).
