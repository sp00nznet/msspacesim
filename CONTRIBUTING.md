# Contributing

This is a recompilation project, which changes what a useful contribution looks
like. Three rules matter more than the rest.

## No game files, ever

Nothing from the Microsoft Space Simulator floppies goes in the repository, in
any form: no floppy image, executable, driver, situation, star system, music or
data file, and nothing regenerated from one -- no lifted C, disassembly
listing, miss list, address list, or test with a lifted function in it.

The tool ships; the game does not. Everyone brings their own floppy images and
points the tools at it. `.gitignore` covers `original/`, `game/`, `work/` (the
lift, its miss lists and the pcrecomp worktree) and the game's file types; if
something slips past it, that is a bug worth reporting on its own. For the
same reason there are no binary releases: `spacesim.exe` has the lifted game
linked into it.

Screenshots and recordings of the game running are fine, and are the point of
the README.

## Where your code comes from

Contributions have to be your own work or under a licence compatible with MIT.
The live risk is a fix ported from a GPL project, which would relicense it by
accident and is very hard to untangle later. The ones most likely to be open
next to this are **DOSBox** and **DOSBox-X** (GPL), whose DOS, EMS, VGA and
OPL code covers exactly what `src/` models. Reading them to understand a
behaviour is fine; copying code from them is not. Unicorn (GPL) is only ever
run as a black box, from `tools/fdiff.py`. If something in your PR came from
somewhere, say where.

## Claims are measured, not assumed

Runs are deterministic headless (`--headless --seconds N --shot-at`,
`--record`), so a change that claims to fix a behaviour should come with the
run that shows it: the situation, the keys, and the frame before and after.
`tools/fdiff.py` runs the original bytes of a function under Unicorn and
compares what it draws with the lifted one. A change to the lifter itself
belongs in [pcrecomp](https://github.com/sp00nznet/pcrecomp), with a
`difftest` case.

## The usual

- Imperative commit subjects; the body explains *why* when it is not obvious.
- Community pull requests are merged with a merge commit, never squashed or
  rebased, so your commits stay yours. Anything the maintainer adds goes in
  separate commits on top.
- Comments explain the reasoning, not the syntax: a comment recording how
  something was found out is worth more than one restating the line under it.
- AI-assisted contributions are welcome, provided a human understood and
  verified the change -- ran it, and can say what it does.
