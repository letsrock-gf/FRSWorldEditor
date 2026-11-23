# Changelog

Changes of the NFS World port. The original editor's own history is in
[its repository](https://github.com/chrystianfarias/FRSWorldEditor).

## In the source, not in a package yet

Everything below is in the `nfsw-port` branch after 0.2.7-nfsw.2. The zip of
0.2.7-nfsw.2 does not have it; build the branch to use it.

### Roads

- **Roads > Bake writes to NFS World.** The road becomes new models, its
  collision becomes new polygons named asphalt, concrete or grass, and its
  street lights become placed objects. Ctrl+Z takes the bake back; Roads >
  Remove baked roads takes it out and keeps the ground fitted under it.
  Traffic and GPS are not written.
- Copy and paste of a road's properties from the right-click menu.

### Collision

- Wall shapes, copy and paste of collision, one-sided walls.
- Gizmo: a centre dot to move freely with snap to vertex, edge or face, and
  scale per axis (Ctrl scales all three).
- Ground that no collision box holds makes the nearest box grow as far as the
  game's grid reaches.
- Launch Game finds ground that was added by edits. Before, with the camera
  over added ground it found none and the car stayed where the game put it.

### Cars

- The game's cars are listed in the Library with a preview, and can be placed
  as scenery with paint, wheels and finish. Checked by script and in the
  editor; not confirmed in the game yet.

### New models and textures

- Copies of shipped models that the game instances are read and placed.
- A new texture can name the texture section that holds it.
- A section's culling tree grows when every node is full.
- Checked by script on the written files; not confirmed one by one in the
  game.

### Speed

- Save with many added collision polygons: the cut of the ground under raised
  pieces went from about 80 minutes to about 1 minute on a list with 160 000
  added polygons.
- Open: encoded textures and parsed OBJ files are kept between runs, and
  collision meshes are built the first time collision is shown.
- The Edits panel no longer reads the whole list every frame. With 28 000
  edits a frame took about 0.6 s; it takes about 18 ms now.

### Experimental

- The collision grid can grow west of the shipped map, and collision packs can
  live outside the shared collision section, for regions added beside Palmont.
- `FRS_DUMP_REGION` in the environment writes an opened region to OBJ, TGA and
  text files.

## 0.2.7-nfsw.2 (2026-11-20)

- **Launch Game / Fast test set themselves up.** The setup's plugins page
  installs an ASI loader (`dinput8.dll`, kept if the game already has one) and
  `editor-fast-test.asi`, which passes the title and the driver select and
  puts the car at the editor's camera. The first Launch Game asks what starts
  your game and keeps the answer as `editor-launch.cmd` in the game folder.
- The setup says when your `nfsw.exe` is another build than the one the port
  was made on.
- Launch Game no longer starts the game the slow way when there is no ground
  under the camera.

## 0.2.7-nfsw.1 (2026-11-20)

First package: the original at 0.2.7 plus the NFS World work.

- Opens Need for Speed World's map, Palmont, straight from the game's files.
- Save (Ctrl+Shift+S) writes into the map's own files; the shipped ones are
  kept in `TRACKS\frs-original`.
- Written to the game: objects removed, moved and placed (with their physics),
  models edited in Blender, new models, replaced and new textures, collision
  deleted, added, moved and re-surfaced.
- Dark theme, texture search, per-side collision walls, a setup row for Need
  for Speed World.
