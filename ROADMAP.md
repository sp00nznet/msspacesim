# Roadmap

## Next

- **Playable**: verify the mouse in the simulator; fly a
  shipped situation; save and load a situation.
- **Conformance harness**: scripted headless runs (logo, title, simulator,
  a situation load) with frame checks, in CI where the game is present and
  SKIP where it is not.

## The remaster

What the sibling projects added on top (Gunman, SimCity 2000, Theme Park),
brought here:

Done: the Direct3D 11 presenter, the F11 settings menu, save states, speed
control, the Xbox controller (its left stick as the game's analog joystick),
music and effects (SS1.EXE lifted), the S10DATA option (`generate.py
--s10data`), and a mods overlay folder (`--mod`). Still to come:

- **Hi-res**: the 3D view drawn at the window's resolution.
- **Mods in the settings menu**: pick one from `mods\` without the command
  line.

## Deferred

- Linux and macOS builds (the runtime is portable C; the host is Win32 today).

## Out of scope

- Distributing anything derived from the game. The toolkit ships; the output
  never does.
