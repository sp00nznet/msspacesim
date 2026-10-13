# Security

## What the program touches

- **Files.** The game's own file calls are reduced to a bare file name and
  opened in `game\` (or `--data`), and anything it writes goes to the save
  folder (`--save`, `game\` by default); a DOS path cannot reach outside
  them. The host writes `settings.ini` beside the exe, save states to
  `states\` and screenshots to `screenshots\` in the current folder.
- **Processes.** `--record` starts `ffmpeg` from `PATH`. Nothing else is
  started.

It needs no administrator rights, writes nothing to the registry, makes no
network connections, and handles no credentials.

## How it runs code

The game's code is compiled C running in-process, on a modelled DOS machine
with 1 MB of guest memory; it reaches the host only through that machine's
DOS, BIOS, EMS and port calls. A malformed situation or save-state file is
parsed by the game's 1994 code or by `src/state.c`, so it should be treated
with the caution due any old program opening untrusted files.

## Reporting

Report a problem privately through GitHub's *Report a vulnerability* on this
repository, rather than in a public issue.
