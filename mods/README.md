# Mods

A mod is a folder here laid over the game folder: any file it has replaces the
game's file of the same name when the game reads it, and any situation (`.STN`)
it adds shows up in **Open Situation**. Files it doesn't have come from
`game\` as usual, and nothing in `game\` is ever changed.

```
mods\
  mars-start\
    FLIGHT.STN        replaces game\FLIGHT.STN: the game starts at the Mars base
    MYTRIP.STN        a new situation
```

Play with one: `spacesim.exe --mod mars-start`. `--mod` also takes a full path.

The game's files are all in one flat folder, so a mod is flat too. A mod's files
are opened read-only; saves and the game's settings still go to `game\` (or
`--save`). `--trace` prints every file the game opens, and `(mod)` marks the
ones a mod supplied.

A situation you save in the game is a `.STN` in `game\` (or `--save`) you can
move into a mod folder to share. Mods built from the game's own files are the
game's material: share the tool or the changes, not the files. Only this README
is committed; `.gitignore` keeps the rest of `mods\` out.
