# Roadmap

## Next

- **Playable**: verify keyboard, mouse and joystick in the simulator; fly a
  shipped situation; save and load a situation.
- **Conformance harness**: scripted headless runs (logo, title, simulator,
  a situation load) with frame checks, in CI where the game is present and
  SKIP where it is not.

## The remaster

What the sibling projects added on top (Gunman, SimCity 2000, Theme Park),
brought here:

Done: the Direct3D 11 presenter, the F11 settings menu, save states, speed
control, the Xbox controller (as keys), music and effects (SS1.EXE lifted).
Still to come:

- **Xbox controller as the joystick** the game supports (analog attitude),
  instead of keys.
- **Hi-res**: the 3D view drawn at the window's resolution.
- **S10DATA**: the installer's `FINISH.BAT` says `s10ata`, so the tenth
  star system was never generated on a real install. Offer it as an option.
- **Mods**: an overlay folder for situations, models and textures.

## Deferred

- Linux and macOS builds (the runtime is portable C; the host is Win32 today).

## Out of scope

- Distributing anything derived from the game. The toolkit ships; the output
  never does.
