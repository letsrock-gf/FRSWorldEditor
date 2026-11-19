# NFS World port

This fork adds **Need for Speed World** to
[FRS World Editor](https://github.com/chrystianfarias/FRSWorldEditor) by
Chrystian Farias (FRS Mods). The editor opens the game's map, Palmont
(region `L5RA`), from a folder that holds `nfsw.exe`, and Save writes the edits
into the map's own files.

- Branch: `nfsw-port`.
- Based on upstream **0.2.7** (tag `v0.2.7`, merged on 19 Nov 2026).
- Licence: the same as upstream, [CC BY-NC 4.0](LICENSE). This file is the
  record of what the fork changed.
- No game file is in this repository. You need your own copy of the game.

The words used below:

- **scripted check**: a script in `tools/nfsw` ran the editor on a scratch copy
  of the written files and read the result back, outside the game;
- **in game**: seen in the running game.

A scripted check does not prove that the game accepts a file. Most of this
port has only scripted checks. See [What was checked where](#what-was-checked-where).

## How NFS World differs from the Underground 2 path

Upstream never changes the game's files: edits go to `<game>\models` and an ASI
plugin loads them. NFS World has no such plugin here, so the port writes into
the game's files and keeps the shipped ones:

- Every streaming section is a file of its own (`TRACKS\STREAML5RA_<n>.BUN`).
  Before the first change the shipped file is copied to `TRACKS\frs-original`.
- The edits are a text list beside those copies, `L5RA-edits.txt`. The editor
  always reads the shipped copy plus the list, and each Save makes the game's
  files again from the two.
- A section whose edits are all undone gets its shipped file back.
- Save refuses while `nfsw.exe` runs: the game holds these files open.
- The setup's "Your games" page has a row for Need for Speed World. Nothing is
  installed into its folder.

The readers branch on `nfs::IsWorld()` (`src/nfs/GameFormat.h`): NFS World keeps
Underground 2's chunk ids but lays the records out differently.

## What the port adds

### Reading and viewing

- The whole region: map sections, models, texture packs, scenery placements.
- Collision (`src/nfs/WorldCollision.cpp`): all 747 packs of
  `STREAML5RA_25000.BUN`, ground triangles and wall barriers.
- Visibility outlines and animated scenery, view only.
- Game look: a reader for the `timeofdaylighting` and `sunandsky` classes of
  `GLOBAL\attributes.bin` (`src/render/GameLookAttributes.cpp`).

### Edits written into the game

| Edit | How it is written |
| --- | --- |
| Remove or move a map object | Patched inside its 96-byte record; no file changes size. |
| Place an object (copy, or from the Library) | A new record with what the game needs to draw it. The section's culling tree grows when every node is full. |
| Physical props (trash cans, parked cars...) | A placed copy gets its own prop entry (chunks `0x34027`, `0x34106`, `0x34108` in `L5RA.BUN`), a removed one loses it, a moved one takes it along. |
| Replace a texture | Encoded over every stored copy. A different image size is allowed: the packs are laid out again and the section table follows. |
| Edit a model in Blender | The OBJ comes back into the model's own chunk of the section file. |
| New model, new texture | Added to the section's solid list and to the texture section its materials need. New textures are named `FRS_<name>`. |
| Collision: delete, add, move, change surface | Articles are written again inside their pack. Walls are barrier records, one per side. Ground that no collision box holds makes the nearest box grow. |
| Raised ground over a road (a speed bump) | The road under it is cut out, so the wheel does not jump between the two. |

`src/io/WorldWrite.cpp` is the entry point (`WriteWorldEdits`); `WorldPlace`,
`WorldTexture`, `WorldMesh` and `nfs/WorldCollision` do the parts. The layouts
were measured on every shipped file with the scripts in `tools/nfsw` and
matched against the loaders in `nfsw.exe`; the comments at the top of each
source file give the offsets and the addresses.

### Editor

- Dark theme for the whole program (`src/ui/DarkTheme.cpp`). `FRS_LIGHT=1` in
  the environment turns it off.
- Texture Browser: Replace, Restore, save as image, and a search field that
  filters by name.
- Collision tools: walls drawn per side (front red, back blue), key `I` flips
  a wall's side.
- The window shows at once and the map loads after it. Before, the editor
  stayed invisible for about half a minute and looked as if it had not started.
- View > Clip Start / End and Detail.
- Fast test and Launch Game on an NFS World folder start the real game: the
  built-in drive mode needs Underground 2's `SPEED2.EXE`.

### Tools

`tools/nfsw` holds the scripts: format measurements (`*_stats.py`,
`col_check.py`, `table_*.py`), scripted editor runs (`test_*.ps1`) and readers
of what was written (`verify_*.py`). `verify_table.py` checks every written
section against the section table of `L5RA.BUN`.

## What was checked where

- **Scripted checks.** Each kind of edit above has a `test_*.ps1` /
  `verify_*.py` pair. On 17 Nov 2026 a real edit list was replayed (2473 edits,
  466 sections written) and `verify_table.py` passed for all 466.
- **In game.** Reported by the user who drives the tests: ground collision
  added in the editor (a speed bump) is driven over without bouncing.
- **Not confirmed in game, item by item:** placed objects and their physics,
  edited and new models, new textures, walls after the one-side fix, key `I`,
  the grown `L5RA.BUN`.

One known way to break the game: a section that grows while its entry in the
section table keeps the old sizes crashes the game on load
(`nfsw.exe+0x34B5B0`, access violation). The writer now rewrites every part
size of the entry, and `verify_table.py` checks it.

## Limitations

- **Upstream 0.2.7's new tools on NFS World** (new regions, region import, the
  model library across games) were not tried on an NFS World folder.
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
- **Fast test** relies on `editor-fast-test.asi` to skip the title and the
  driver select. That ASI is not in this repository.
- **Game look and sky** on NFS World were not compared with the game.
- **Underground and Underground 2** were not run after the port's changes or
  after the merge of 0.2.7: there was no copy of those games on the machine.
  The port branches on the game, but regressions there are possible.
- **One game build.** The addresses in the comments are from one `nfsw.exe`
  (32-bit, image base `0x400000`). Other builds were not checked.
- **Weight.** The region takes about half a minute to load and about 3.5 GB of
  memory on the development machine.
- **Test scripts** have the development machine's paths in them
  (`D:\NFS World\...`). Change them before use.
- **No release build.** This fork has no releases, so the README's download
  links lead nowhere here. Build from source as the README says.

## Credits

FRS World Editor is by Chrystian Farias (FRS Mods). The NFS World port was
made by letsrock-gf with Claude (Anthropic). Need for Speed World is a
trademark of Electronic Arts; this is a fan project, not affiliated with or
endorsed by EA.
