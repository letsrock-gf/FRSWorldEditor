# Importing a whole region into another

Goal: bring every section of one Underground 2 region (L4RC, the drift
arenas, say) into another (L4RA, the city) as sections of its own, moved and
turned as one piece, and keep it editable as one piece (Move, Rotate about Z).
Decisions taken with the user (2026-11-15):

- **Sections of its own**, not merged into the target's: the import gets new
  section numbers, its own visibility zones and lists, its own streaming.
- **Move + rotate about Z only.** Collision walls stay upright, ground stays
  level, routes later stay sensible.
- **Phase 1**: scenery and terrain (models, instances, textures) and collision.
  Lights, flares, effects, breakable props, routes, AI and races come later
  (switched off in phase 1).

## What the data allows [files]

Survey of the shipped region files (`tools`-style scan, 2026-11-15):

| Region | Sections | Letters used | Section centres span |
|---|---|---|---|
| L4RA | 393 | A-J, M-R, X, Y, Z | (-3133, -2069) .. (1830, 3450) |
| L4RB | 23 | A, B, C, X, Y, Z | (-561, -2215) .. (2761, 1122) |
| L4RC | 29 | A, B, C, X, Y, Z | (-1355, -919) .. (1334, 1331) |
| L4RD | 410 | as L4RA | as L4RA (a second city) |
| L4RF | 25 | A-D, X, Y, Z | |
| L4RG | 48 | A, B, R, S, X, Y, Z | |
| L4RH | 9 | A, X, Y, Z | |
| L4RR | 9 | A-D, X, Y, Z | |

- Every region uses collision base 20 (`0x34151` +0), so zone N reads the
  collision pack of section N + 20 everywhere.
- A section number is letter x 100 + digits (A1 = 101, X1 = 2401). Drivable
  zones are N % 100 in 1..19, their collision sections N + 20.
- **L4RA leaves seven letters free: K, L, S, T, U, V, W.** An import renames
  each of its source letters to a free one and keeps the digits, so every
  "zone -> zone + 20" relation survives. The small regions need 4 to 7
  letters: one import at a time fits (two of the smallest); L4RD does not fit
  and is not a sensible import (it is the city again).
- The source's libraries (X, Y, Z: models and textures with no place of their
  own) come along renamed like any other letter; in the target they are
  ordinary sections that the imported zones' lists load.

## Formats involved

- Section table `0x34110` in `<R>.BUN`: 80-byte entries -- name (4 ASCII),
  `+0x08` number, `+0x10` 1, `+0x14` stream offset, `+0x18` size, `+0x1C`
  size without the texture pack, `+0x20` order (10000-19999 main set,
  20000+ the second set), `+0x24` centre (3 floats), `+0x30` a hash that is
  not the name's (kept from the source: the editor already rewrites sections
  without touching it and the game takes them).
- Visibility container `0x80034150`: `0x34151` header (u32 base, u32 count,
  u16 drivable zone ids, zero-filled), `0x34152` outlines (marker 0B 00 00 00
  0B 00 00 00, then u16 id, u16 point count, box min/max, the box's middle,
  the ring: 76 bytes for six points), `0x34153` lists (marker, u16 id, u16 0,
  u32 0, u32 count, then room for 72 u16 section numbers), `0x34154` (per-race
  Z sections), `0x34155` (CC groups).
- Scenery `0x80034100`: header number at `0x0C`, 64-byte instances
  (`io`: nfs/SceneryEdit), the 36-byte node tree.
- Collision `0x80034130`: header number at `+0`; polygons re-encoded
  (nfs/Collision EncodeCollision).
- Flares `0x80135100`, effects `0x35020`, spawners `0x34027`: switched off in
  phase 1 (count zeroed in place, as an empty region does).

## The transform

Rotation about Z by `yaw` (row vectors, as the game's matrices):
`Rz = [[c, s, 0], [-s, c, 0], [0, 0, 1]]`, then the offset `T`.

- An instance (rotation R, translation t): `R' = R Rz`, `t' = t Rz + T`; its
  box is the 8 corners of the old box transformed, boxed again.
- An instance with flag `0x0100` (a world-space mesh the game draws without
  the record's matrix): the flag is cleared and the record gets the model's
  own matrix (solid header `+0x40`) times the import transform -- right
  whether the vertices are world-space (identity solid matrix) or local.
- The scenery tree's node boxes: rebuilt from the instance boxes
  (`AppendScenery` with every record replaced does exactly that).
- Collision: every corner rotated and moved, the pack re-encoded with the new
  section number.
- Table centres and zone rings: rotated and moved; zone boxes and middles
  recomputed.

## Where it goes

- **Stream**: each imported section is written as a span past the end of
  `STREAM<target>.BUN` (2 KiB aligned, after every section and span there is),
  the same mechanism as a section the editor rebuilt and moved
  (`FileEdits.cpp` SupplyBeyondFile). Named `<target>-<SECTION>-stream.frssection`.
- **Region file** (the served copy `models\files\TRACKS\<target>.BUN`):
  - the table grows **in place, at its end**: the entries before keep their
    offsets, so the editor's table spans (12 bytes at an entry) stay true;
    the chunks after it move by a multiple of 0x8000 (`SpliceRealigned`).
  - the visibility container **moves to the end**: the shipped one becomes
    padding of its size (nothing in front of the parameter maps or the table
    moves), and a copy with the zone-list spans folded in and the imported
    zones' header ids, outlines and lists added goes last.
  - composed with the file's other writers in this order: markers (beacons),
    fog (`io/DistrictFog`), imports, an empty region's list cut.
- **Description**: `<models>\imports-<target>.txt` (source region, letter
  map, offset, yaw); everything else is regenerated from it on every bake.

## The engine's limits, and lifting them

The end goal (user, 2026-11-15) is one world joined from regions of several
games -- Underground 1, Most Wanted, Carbon... -- so the shipped limits are to
be raised, not worked within. What is known so far [code]:

| Limit | Where | Value | What lifting it takes |
|---|---|---|---|
| Collision slots | `TheTopologyManager` 0x88F768: trees at `+0x8`, barrier groups from `+0x82C` | 521 slots; `Slot(n)` (0x5D3C10) = (letter - 'A') x 20 + folded digits, so letters A..Z x 20 zones; a number past Z writes over the barrier groups | a larger slot array of the loader's own, `Slot` patched (callers 0x591388, 0x5AA2E7, 0x5E793E, 0x5E8234; readers of `[mgr + slot*4 + 8]`) |
| Section letters | the number scheme above, X0/Y0/Z0 by number (2400/2500/2600), split screen X/Y n+50, "no zone: draw n%100 < 10" | 26 letters x 100 | follows from the slots; numbers past 2699 to be checked against every rule |
| Sections per zone list | `0x34153` records, fixed stride 0xA4 (`sub_5E9C10`) | 72 | the list reader patched (or CC groups, capped at 0x180 entries by `sub_5DFF50`) |
| Streaming pool | `TrackStreamingPoolSizes` 0x79DC54 | 60 000 KiB (L4RG 36 000) | one constant; the process is **not** large-address-aware (2 GiB), so a few hundred MiB at most without the 4 GB flag on SPEED2.EXE |
| Stream file | offsets u32 | 4 GiB | none; sections past the end are served by the loader already |
| Instances per scenery chunk | the tree's child references (< 0x8000) | 32 767 | none needed (per section) |
| Models / textures loaded at once, draw lists | not yet read | ? | to find |

**Collision slots -- lifted (loader/src/Limits.cpp, 2026-11-15).** Every
reader and writer of the slot array takes its index from `Slot()` (0x5D3C10,
four callers: GetIndex 0x591388, 0x5AA2E7 which reads `[i*4 + 0x88F770]`, the
pack loader 0x5E793E, the unloader 0x5E8234); the constructor 0x5E81D0 only
zeroes the 520 entries. No inlined copy of the formula exists (the other
`add al, 0x40` sites only print section names). So `Slot()` alone is
replaced: it returns the index that puts `manager + 8 + i*4` on an array of
the loader's own, one entry per section number 0..65535, with the same fold
(n % 100 in base..2 base -> n - base). Once per change in the number of
loaded packs the loader logs whether each is found in its slot. The editor's
port (`TopologyU2::Slot`) follows the new rule. Awaiting the in-game check.

Confirmed in the game the same day (L4RA free roam, every pack found in its
slot).

A crash found later the same day: with the 240 MB streaming pool, starting a
race in another region hit the game's heap check (int3 at 0x440039 in
bMemoryPool::Verify 0x43FEF0: a freed 0x44-byte block in the main pool had a
byte written at +0x1C). Bisected with the [Limits] switches: pool 60 MB ok;
pool 240 MB with CollisionSlots=0 ok; both on: crash. TopologyManager::Close
(0x5E1180, game state 8 via 0x580F16) disconnects the trees and deletes
their slot pool whole; the plugin's slot array kept pointing at them. It is
now cleared as Close starts (its first 7 bytes hooked). The once-per-change
check in the log only walks the manager's list in gameplay.

**Section letters -- lifted (Limits.cpp).** Eleven streamer tests take only
A..T as positioned / drivable sections (`cmp al,'A'; jl; cmp al,'T'; jg`
after `n / 100 + 0x40`, at 0x5D5EFA, 0x5D5F39, 0x5D5F4D, 0x5D61C6, 0x5DAB8E,
0x5DF767, 0x5E0064, 0x5E62B9, 0x5E8421 (near jumps), 0x5E8501, 0x5E9C7E;
0x590D21 is a string test, not one of them). U..W are "other", X/Y the
libraries (also tested by number, 2400/2500), Z the race sections. Each test
now also takes '[' .. 0x7F: **numbers 2700..6399, 37 more letters** for
imported regions; U..Z behave as before; past 6399 the signed byte the game
compares is refused as it always was.

**Sections per zone list -- lifted (Limits.cpp).** The 0x34153 reader
(0x5E9C10) counts records as size / 0xA4 (0x5E9CCE, magic 0xC7CE0C7D) and
steps +0xA4 (0x5E9D4F). The first record's u16 at +0x0A (after the zone id;
zero in every shipped file) now gives the stride when it is 0x20 or more;
0xA4 otherwise. A writer that needs longer lists uses e.g. 0x2A4 (330
sections; the streamer's wanted list holds 384, a 0x308-byte stack buffer in
sub_5E5110). Awaiting the in-game check (no shipped file changes behaviour).

**Streaming pool and slot pools -- configurable (Limits.cpp).**
`TrackStreamingPoolSizes` 0x79DC54 (.rdata, read at 0x57A61C) gets
`[Limits] StreamingPoolKiB` (120000 by default). Every fixed-capacity pool
goes through `bNewSlotPool(size, count, name, flags)` 0x440DF0 (cdecl); its
first 8 bytes are hooked and a pool named in `[Limits]` gets at least that
many slots; each pool made is logged once. Pools that matter for a joined
world, as shipped (size, count):

| Pool | Bytes | Count |
|---|---|---|
| eModelSlotPool | 0x18 | 5000 |
| eStreamingPackSlotPool | 0xC0 | 64 (a static 0x100-dword array sits next to its creation: check before raising) |
| TexturePackSlotPool | 0x7C | 128 |
| g_textureHeaderPool, g_vertexBufferHeaderPool | 4 | 1024 each |
| eTextureBucketSlotPool | 0x20 | 1024 |
| eMeshRender | 0x2C | 4096 |
| WorldModelSlotPool | 0x70 | 160 |
| eLightMaterialPlatInfoSlotPool | 0x80 | 220 |
| TopologyTreeSlotPool | 0x70 | 48 or 96 |
| TrackPolygonCollisionBodySlotPool | 0x270 | 160 |
| AcidEmitterSlotPool / AcidActiveGroupSlotPool | 0xD0 / 0x2C | 250 / 400 |
| VirginSmokeableSlotPool / SmokeableSlotPool | 0x64 / 0x510 | 256 / 32 |
| SpaceNodeSlotPool | 0xF0 | 224 |

None is raised by default: a pool may have a fixed array of its size
elsewhere. Each is raised when an import shows it is reached.

So the work starts with **phase 0: the limits** -- read each one out of the
exe, then lift it from the loader, each with a test in the game -- before
the import itself depends on any of them.

## Phases

0. Engine limits (above) -- DONE 2026-11-15, confirmed in game: collision slots,
   section letters, zone lists, streaming pool (240 MB with the 4 GB flag), slot pool hook.
   then the zone lists, the pool, model and texture capacity.
1. Core: section transform + the bake into spans and the region file; the
   editor opens the served region file when imports exist. Tested offline
   (scratch game folder) and in the editor.
2. UI: World > Import Region..., the import as one object with the gizmo
   (Move, Rotate about Z), outline preview while dragging, re-bake on release,
   undo, remove.
3. In the game (the user runs it).
4. Later: flares/effects/props transformed instead of off; routes, AI and
   the region's races.
