# FRS World Editor -- NFS World port

A fork of **[FRS World Editor](https://github.com/chrystianfarias/FRSWorldEditor)**
by Chrystian Farias (FRS Mods) that adds **Need for Speed World**. It opens the
game's map, Palmont, straight from the game's files, and **Save writes your
edits into the map's own files**, keeping the shipped ones.

The original editor is for the Black Box Need for Speed games (Underground 2
today, others on the way) and is where the editor itself comes from: this fork
is the original at **0.2.7** plus the NFS World work described below. For
Underground 2, use [the original](https://github.com/chrystianfarias/FRSWorldEditor).

> Fan project, not affiliated with or endorsed by Electronic Arts. No game file
> is in this repository or in its packages: you need your own copy of the game.

- [Download and first run](#download-and-first-run)
- [What this version adds](#what-this-version-adds)
- [How saving works on NFS World](#how-saving-works-on-nfs-world)
- [What was checked where](#what-was-checked-where)
- [Limitations](#limitations)
- [The original editor](#the-original-editor)
- [Building](#building)
- [Credits and licence](#credits-and-licence)

## Download and first run

1. Get the zip from the [Releases](../../releases) page and unzip it anywhere.
   It holds `FRSWorldEditor.exe` and, in `game`, two optional files for Launch
   Game.
2. **Back up your game's `TRACKS` folder first.** The editor keeps the shipped
   files by itself (see below), but this is a port still being tested.
3. Close the game, run `FRSWorldEditor.exe`, and on the setup's "Your games"
   page give the folder that holds `nfsw.exe` in the **Need for Speed World**
   row. The next page offers to install what Launch Game needs; saving needs
   nothing in the game, so you may untick both.
4. The window shows at once and the map loads after it: about half a minute and
   about 3.5 GB of memory on the development machine.
5. Right mouse looks around, `W` `A` `S` `D` moves, `Q` / `E` down / up, Shift
   faster. Edit, then **Ctrl+Shift+S** writes the edits into the game.

Save refuses while `nfsw.exe` runs: the game holds the map's files open.

## What this version adds

### Reading and viewing

- The whole region (`L5RA`): map sections, models, texture packs, scenery
  placements.
- Collision: all 747 packs of `STREAML5RA_25000.BUN`, ground triangles and wall
  barriers.
- Visibility outlines and animated scenery, view only.
- Game look: a reader for the `timeofdaylighting` and `sunandsky` classes of
  `GLOBAL\attributes.bin`.

### Edits written into the game

| Edit | How it is written |
| --- | --- |
| Remove or move a map object | Patched inside its 96-byte record; no file changes size. |
| Place an object (copy, or from the Library) | A new record with what the game needs to draw it. The section's culling tree grows when every node is full. |
| Physical props (trash cans, parked cars...) | A placed copy gets its own prop entry (chunks `0x34027`, `0x34106`, and `0x34108` in `L5RA.BUN`), a removed one loses it, a moved one takes it along. |
| Replace a texture | Encoded over every stored copy. A different image size is allowed: the packs are laid out again and the section table follows. |
| Edit a model in Blender | The OBJ comes back into the model's own chunk of the section file. |
| New model, new texture | Added to the section's solid list and to the texture section its materials need. New textures are named `FRS_<name>`. |
| Collision: delete, add, move, change surface | Articles are written again inside their pack. Walls are barrier records, one per side. Ground that no collision box holds makes the nearest box grow. |
| Raised ground over a road (a speed bump) | The road under it is cut out, so the wheel does not jump between the two. |

### Editor

- **Setup:** the "Your games" page has a row for Need for Speed World, and the
  plugins page installs what Launch Game needs there: an ASI loader
  (`dinput8.dll`, kept if the game already has one) and the fast-test plugin.
- **Dark theme** for the whole program. `FRS_LIGHT=1` in the environment turns
  it off.
- **Texture Browser:** Replace, Restore, save as image, and a search field that
  filters by name.
- **Collision tools:** walls drawn per side (front red, back blue); key `I`
  flips a wall's side.
- **Start:** the window shows at once and the map loads after it. Before, the
  editor stayed invisible for about half a minute and looked as if it had not
  started.
- **View > Clip Start / End and Detail**, to draw less of a large map.
- **Launch Game / Fast test** on an NFS World folder start the real game (the
  original's built-in drive mode needs Underground 2's `SPEED2.EXE`). The
  first time, the editor asks what starts your game -- your launcher, or a
  `.cmd` of your own -- and keeps the answer as `editor-launch.cmd` in the
  game folder. The fast-test plugin then passes the title and the driver
  select by code and puts the car at the editor's camera.

### Tools

`tools/nfsw` holds the scripts the port was made with: measurements of the
file formats (`*_stats.py`, `col_check.py`, `table_*.py`), scripted editor runs
(`test_*.ps1`) and readers of what was written (`verify_*.py`).
`verify_table.py` checks every written section against the section table of
`L5RA.BUN`. The scripts find the repository from their own place and read the
game from the folder in `FRS_GAME_FOLDER` (`tools/nfsw/game.ps1`); the editor
runs they start come from a build tree named in each script (`build-test` for
most). The layouts were measured on every shipped file and matched
against the loaders in `nfsw.exe`; the comments at the top of
`src/io/WorldWrite.h`, `WorldPlace.h`, `WorldTexture.h`, `WorldMesh.h` and
`src/nfs/WorldCollision.cpp` give the offsets and the addresses.

## How saving works on NFS World

The original never changes a game's files: edits go to `<game>\models` and an
ASI plugin loads them. NFS World has no such plugin here, so this port writes
into the game's files and keeps the shipped ones:

- Every streaming section is a file of its own (`TRACKS\STREAML5RA_<n>.BUN`).
  Before the first change the shipped file is copied to `TRACKS\frs-original`.
- The edits are a text list beside those copies, `L5RA-edits.txt`. The editor
  always reads the shipped copy plus the list, and each Save makes the game's
  files again from the two.
- A section whose edits are all undone gets its shipped file back.
- The shipped `.BUN` files stay in `TRACKS\frs-original`. Your own backup of
  `TRACKS` is the sure way back to the game as shipped.

## What was checked where

The words used here:

- **scripted check**: a script in `tools/nfsw` ran the editor on a scratch copy
  of the written files and read the result back, outside the game;
- **in game**: seen in the running game.

A scripted check does not prove that the game accepts a file. Most of this port
has only scripted checks.

- **Scripted checks.** Each kind of edit above has a `test_*.ps1` /
  `verify_*.py` pair. After the merge of 0.2.7 (19 Nov 2026) a real edit list
  was replayed: 1877 edits, 469 sections written. Every one of the 470 files
  came out byte for byte as the build before the merge had written them, and
  `verify_table.py` found an entry for all 469 sections.
- **In game.** Ground collision added in the editor (a speed bump) is driven
  over without bouncing. The fast-test plugin, as built from `fasttest-nfsw`
  and loaded by the Ultimate ASI Loader 10.0.0 the package carries, reaches
  free roam with no input, with and without a car position (started by
  script, on the author's setup). The setup installing the two files, and the
  editor asking for and running an `editor-launch.cmd`, have scripted checks
  only, on a scratch game folder.
- **Not confirmed in game, item by item:** placed objects and their physics,
  edited and new models, new textures, walls after the one-side fix, key `I`,
  the grown `L5RA.BUN`.
- **Not tried:** the package on a machine other than the author's.

One known way to break the game: a section that grows while its entry in the
section table keeps the old sizes crashes the game on load
(`nfsw.exe+0x34B5B0`, access violation). The writer rewrites every part size of
the entry, and `verify_table.py` checks it.

## Limitations

- **One game build.** The port was matched against one `nfsw.exe` (32-bit, MD5
  `4c32936debffcbdc208d39502b79a56f`). The setup says so when yours is another
  build; no other build was checked.
- **Not written to NFS World:** roads, terrain, markers, traffic and GPS
  routes. Those tools show on the map but their bake is Underground 2's format.
- **Publish World, `.frsworld` files and the game plugin** do not carry NFS
  World edits.
- **Collision import** (OBJ, Blender) is refused for NFS World. Collision is
  edited with the editor's own tools only.
- **Edited models:** only the edited level of detail changes. From far away the
  game may still show the shipped shape. Models whose vertices are
  integer-packed (effects 8 and 9) are not written.
- **New textures:** at most 512 px a side, DXT1 or DXT5.
- **Collision outside every box:** the nearest box grows by 200 m at most. Past
  that the polygon is left out of the save and the status bar counts it.
- **Culling tree:** the game walks it with a 64-entry stack. A save that would
  pass it is refused, and the message names the section and the object.
- **Launch Game / Fast test** were tried only on the author's setup. The
  plugin checks the hash of `nfsw.exe` and does nothing on another build: the
  game then starts the usual way. If your launcher asks for a login or a click
  before it starts the game, you still do that part; the plugin takes over
  when the game itself starts, within ten minutes of the click in the editor.
  A game that already has a `dinput8.dll` keeps it, and the plugin only loads
  if that file is an ASI loader. The game's own loading screens are never
  skipped. Delete `editor-launch.cmd` in the game folder to choose another
  way of starting the game.
- **Game look and sky** on NFS World were not compared with the game.
- **The original's newest tools on NFS World** (new regions, region import,
  the model library across games) were not tried on an NFS World folder. The
  package is built the way the original's releases are, without New Region and
  Import Region.
- **Underground and Underground 2** were not run after the port's changes or
  after the merge of 0.2.7: there was no copy of those games on the machine.
  The port branches on the game, but regressions there are possible, and the
  package does not carry the Underground 2 plugins. Use the original for them.

## The original editor

[FRS World Editor](https://github.com/chrystianfarias/FRSWorldEditor) reads a
game's own files directly -- nothing is converted or imported first -- so what
the viewport draws is what the game ships. In short, what it does (for
Underground 2; see its page for the full list, the keys and the other games):

- **The whole city as shipped**, with a free camera, the sky, animated
  textures and object animations; textured, unlit, vertex-colour and wireframe
  views; a texture browser.
- **Objects:** place any model from a Library with pictures, move, rotate,
  snap to the ground, copy, remove. Every edit is listed and can be reverted.
- **Meshes:** edit a shape in place (Tab), or in Blender with one key and
  every save coming back; bring in new models from Blender or OBJ.
- **Collision tools:** draw barriers, walls and ground on the map, move them,
  give them a surface, or make collision from an object's own mesh.
- **Roads, terrain, markers, visibility zones, quick travel** and a game
  plugin that loads the edits into the running game.

Its page has the download for Underground 2, the full documentation, the
[Discord](https://discord.gg/jTKwQXYQkb) and a way to
[support its author](https://buymeacoffee.com/chrystianfarias).

## Building

Visual Studio with the C++ workload and CMake 3.21 or newer; no other
dependencies. The port is built with the 2026 Build Tools:

```
cmake -S . -B build -G "Visual Studio 18 2026" -A x64
cmake --build build --config Release
```

The game is 32-bit, so the fast-test ASI is built for Win32 (MinHook is
downloaded at its 1.3.4 tag; the build runs the ASI's own self-test):

```
cmake -S fasttest-nfsw -B fasttest-nfsw/build -G "Visual Studio 18 2026" -A Win32
cmake --build fasttest-nfsw/build --config Release
```

`package-nfsw.ps1` builds both (the editor without New Region / Import Region,
as the original's releases) and writes the folder and the zip to `dist\`. It
takes Ultimate ASI Loader's 32-bit `dinput8.dll` and its `LICENSE.txt` from
`third_party\asi-loader`, which is not in the repository: put a release of
the loader there first.
`package.ps1` is the original's and also builds the Underground 2 plugins,
from repositories that are not here.

The source layout is the original's; the NFS World code is in `src/io/World*`,
`src/nfs/WorldCollision.*`, `src/nfs/GameFormat.h`, `src/render/GameLook*` and
behind `nfs::IsWorld()` elsewhere. `docs/NFSU2-FORMATS.md` is the original's
record of the Underground 2 formats.

## Credits and licence

- **FRS World Editor** is by Chrystian Farias (FRS Mods):
  <https://github.com/chrystianfarias/FRSWorldEditor>.
- **The NFS World port** is by letsrock-gf.
- [Dear ImGui](https://github.com/ocornut/imgui) (MIT),
  [Phosphor Icons](https://phosphoricons.com) (MIT) and the Win32 property grid
  by David MacDermot (CPOL) are in `third_party`.

Licensed, like the original, under [Creative Commons
Attribution-NonCommercial 4.0 International (CC BY-NC 4.0)](LICENSE): you may
share and adapt this work, with credit, for non-commercial purposes only -- it
may not be sold.

Need for Speed, Need for Speed World, Underground and Underground 2 are
trademarks of Electronic Arts.
