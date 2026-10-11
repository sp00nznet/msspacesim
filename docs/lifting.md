# Lifting Microsoft Space Simulator: what it took

The things that each cost real time, in the order they turned up, so the next
BAO title (Flight Simulator 4 and 5 are the same shop and the same style)
costs less. Code: [`tools/lift.py`](../tools/lift.py), and in pcrecomp
`decode16.py`, `lift16.py`, `unlzexe.py`, `unexepack.py`.

## 1. What is on the floppies

Three 1.44 MB images, each holding one LHA self-extractor (`DISKn.EXE`, LHA's
SFX 2.13S). Unpacked: 375 files, of which the game is `SPACESIM.EXE`, the
launcher-with-music-TSR is `SS1.EXE`, the Super VGA drivers are `SVGA.DRV` /
`SVGA8.DRV`, and 21 small programs the installer ran once and deleted:
`MAKEDATA`, `MAKEHMO` and `S01DATA` … `S19DATA`. They write the star, planet,
moon, asteroid, comet and sky files (543 of them) the game cannot start
without (`open sol.str -> -1`).

`SPACESIM.EXE` is LZEXE 0.91 (pcrecomp's `unlzexe.py`), 157,625 -> 473,527
bytes. The `S??DATA` tools are Microsoft EXEPACK, which pcrecomp could not
unpack; `drm/unexepack.py` was added for them. `MAKEDATA.EXE` keeps its
relocation table at 1Eh, not 1Ch -- read `e_lfarlc`, or no segment immediate
gets rebased and every file it creates is named "".

## 2. No C runtime, three code segments

There is no CRT: the entry point is hand-written (`mov ax, DGROUP` patched
into its own code, `mov word cs:[1], ax`). Code is in segments 0F96, 1B87 and
293C; segment 0 is the data segment and DS points at it for the whole run.
Each code segment starts with a lone `retf`, which turns out to matter (3).

## 3. Calls made by hand

A near routine in another segment cannot be reached with a near call, so the
code builds the frame itself:

```
push cs / push label / push 0 / jmp far seg:routine
```

The routine's `ret` pops the 0 and lands on `seg:0000` -- the lone `retf` --
which pops label:cs. 424 call sites do this. A recursive walk saw a tail jump
and stopped; the label was only reached as a run-time dispatch of the pushed
return address, from inside the callee, nesting a C frame per call. pcrecomp
lift16 now takes `push_calls` from the driver and lifts the jmp as a call
that continues at the label.

The SMDL interpreter (7) does the same with `retf` for a target it computed:
`push cs / push 186F / push 0 / push dx / push ax / retf`. Its label is two
bytes past the retf (it skips the `call ax` used for same-segment targets).

## 4. Instructions the decoder did not know, and two the lifter got wrong

All found by pcrecomp's `difftest16` against Unicorn, on this binary:

- `bsf`/`bsr` (0F BC/BD) decoded as `db 0xF`. The fixed-point maths uses `bsr`
  23 times; with the walk stopping there, maths routines ended in jumps to
  unlifted code and the host stack overflowed.
- `rcr eax, 1`: rotate-through-carry is 33 bits for a dword and the lifter
  built it in 32. The software floating point's 64-bit shifts went wrong and
  the Kepler solver never converged: the game sat on its logo forever.
- `bt word [mem], ax`: a register bit offset into memory addresses a bit
  string (ax = 37 means bit 5 of the word at +4). The key-down map is tested
  this way.

## 5. Expanded memory

The game needs 512 KB of EMS and finds it the documented way: open the device
`EMMXXXX0`, IOCTL 4400h says character device, then INT 67h. `src/ems.c`
answers with LIM EMS 4.0. The game allocates 40 pages in handles of four and
switches whole 64 KB banks; its model data lives there.

## 6. SETUP's files

`setup.ini` is two lines (the registered name and product ID). The settings
are in `SPACESIM.INI`: the 2D driver's file name, then 41 one-byte fields as
two hex digits per line. Without it: "Error loading 2D driver". The game has
a writer for it; `tools/make_ini.py` reads the field list out of that writer
and writes each field's initial value, so the file is the game's own default.

The reader has a bug the original shares: on a short file it jumps to the
hex reader's epilogue (`pop cx / pop bx / ret`) without having pushed
anything, which unbalances the stack by 4 and corrupts whatever sits above
it -- here, the logo animation's index. A truncated `SPACESIM.INI` breaks the
real game the same way.

## 7. The SMDL interpreter

Models are a byte-code (SMDL) run by an interpreter with an 85-entry opcode
table at DS:802E, dispatched with `call word ds:[eax*2+802Eh]` behind a
`cmp al, 54h / ja bad`. The driver reads data-segment handler tables like it,
to the bound the `cmp` states (one handler falls into the next, so "starts
after a ret" alone stopped the scan early). Missing handlers showed up as the
game's own "Bad SMDL Opcode".

The interpreter saves SP and its handlers can abort with
`mov sp, [7E60] / ret`, which in C leaves only one frame. pcrecomp lift16's
`unwind_sp` returns from every lifted function whose frame the guest has
unwound past.

## 8. Other tables

- The 2D driver installs its entry points with `mov ax, offset /
  mov [BDxx], ax` pairs at the top of its code; the driver takes those.
- The value reader dispatches on an operand type with `jmp cs:[ebp*2+18D9h]`.
- A dispatch miss records the segment it was reached with, and the game jumps
  through normalised far pointers (`21C6:0001` is `1B87:52F1`): a miss is
  mapped to the code segment that holds it, not taken as a new one.

## 9. Super VGA

The 640x400 and 800x600 modes are separate driver files, `SVGA.DRV` and
`SVGA8.DRV`. When `SPACESIM.INI` names one, the game reads its first 1700h
bytes over the built-in driver's header at DS:DE74 and the next 2D00h over the
built-in driver's code at 293C:D010, then calls D010, which installs the
driver's entry points into the vector table at DS:BDD0. The same addresses
hold different code depending on the INI.

`tools/lift.py` lifts each driver as a variant of that region: an image with
the file's bytes in place, walked from D010 (and from whatever its stores into
the vector table name), lifted as `fn_X_svga8` and so on, with the built-in
driver's functions becoming `fn_X_vga`. Every address in the region gets one
trampoline `fn_X`, which is what the rest of the program calls and what the
dispatch table holds; `src/drv.c` sends it to the variant whose code is loaded,
decided after each DOS read that lands in the region by matching the first 32
bytes of code. A dispatch miss inside the region goes to that variant's own
list (`misses_svga8.txt`).

With SETUP's chipset "VESA 1.2 Compatible" the drivers use VBE functions 01h,
02h and 07h and switch banks through the window function's far pointer;
`src/vga.c` answers them with a 1 MB framebuffer behind a 64 KB window, and
scans out at the mode's resolution. 800x600 is the default `tools/make_ini.py`
writes.

## 10. The launcher: a second program

Music and sound effects are not in `SPACESIM.EXE`. `SS1.EXE`, a Microsoft C
6 program, holds an AdLib player that stays resident while it runs the game;
the game finds it through INT 2Fh (AX=4D52h returns a far entry point) and
calls it with a function number in BX. With the player absent the game
programs the PIT itself and stays silent.

`tools/lift.py` lifts SS1 with the same pipeline (no BAO-specific rule is
needed: the CS-pair rule finds the INT 2Fh handler and the entry it returns),
loads it at `9600` instead of `0100`, and renames its symbols (`ss1_fn_...`,
`g_ss1_funcs`) so it links into the game's exe. Two things only a C program
asks of DOS turned up: `spawn` walks the MCB chain (an arena kept only in the
host made it loop forever), and `system()` EXECs `COMMAND.COM /C`, which the
EXEC model takes as "run the game". A miss inside SS1 stops the run: its C
runtime does not survive a skipped routine, and the garbage that followed
landed in the game's miss list.

## Debugging kit

| | |
|---|---|
| `work/spacesim/misses.txt` | every dispatch to an address with no lifted function; the next lift makes them entries |
| `--trace` | DOS calls, EMS handles, sequencer writes |
| `cmake -B build-dbg -DSPACESIM_TRACE=ON` | lifted-call ring buffer, PDB, the tools below |
| `SPACESIM_SNAP=fn[:N]` + `LIFT_EXIT_HOOK=fn` | snapshot the machine at a function's Nth entry and at its return |
| `tools/fdiff.py fn` | run that function's original bytes under Unicorn from the snapshot (with EMS, files and the sequencer modelled) and compare memory, registers and planes |
| `SPACESIM_WATCH=<linear>` | every write to a byte, with the host stack (the guest call chain) |
| `SPACESIM_CALLS=from-to` | lifted functions first entered in a window of game time |
| `SPACESIM_VRAMDUMP=file` | the planes and DAC at exit |
