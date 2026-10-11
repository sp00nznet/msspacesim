# Roadmap

## Next

- **Playable**: verify keyboard, mouse and joystick in the simulator; fly a
  shipped situation; save and load a situation.
- **Conformance harness**: scripted headless runs (logo, title, simulator,
  a situation load) with frame checks, in CI where the game is present and
  SKIP where it is not.
- **netlab recipe** (`projects/msspacesim.env`) for builds and QA off the
  workstation.

## The remaster

What the sibling projects added on top (Gunman, SimCity 2000, Theme Park),
brought here:

- **A modern presenter**: Direct3D 11, resizable window, borderless
  fullscreen, integer / sharp-bilinear / CRT scaling.
- **Settings menu (F10)**, save states, speed control (the sim already has a
  time scale; this is the host's pacing).
- **Xbox controller**, mapped to the joystick the game already supports.
- **Music and effects**: the AdLib music player is a TSR inside the
  launcher `SS1.EXE`; lift it, or play its `.MDI` files natively.
- **S10DATA**: the installer's `FINISH.BAT` says `s10ata`, so the tenth
  star system was never generated on a real install. Offer it as an option.
- **Mods**: an overlay folder for situations, models and textures.

## Deferred

- Linux and macOS builds (the runtime is portable C; the host is Win32 today).

## Out of scope

- Distributing anything derived from the game. The toolkit ships; the output
  never does.
