# NFS Underground 2 track file formats

Everything here was derived by reading the shipped files in `TRACKS/` and
checking each rule against all 118 MB of `STREAML4RA.BUN` (1145 solid packs,
10 735 objects, 1836 textures) plus spot checks on the other regions. Offsets
are hexadecimal and byte offsets from the start of the structure. Fields still
unidentified are marked as such rather than guessed at.

Some sections fold in findings from porting other maps into the game
(Underground 1, its PS2 demo, the Underground 2 PS2 demo city, Hot Pursuit 2,
Midnight Club 3, Gran Turismo 4), each port played in the game. Those carry a
tag saying how they were established: **[game]** seen in the running game,
**[code]** read from the exe, **[files]** counted over the shipped data.
Addresses are for SPEED2.EXE 1.2.

## 1. Container format (BChunk)

Every asset file is a flat sequence of chunks:

```
struct ChunkHeader {
    u32 id;        // high bit set => the payload is a list of child chunks
    u32 size;      // payload size in bytes, not counting this 8-byte header
};
```

Two details make or break a parser:

**Alignment filler.** A payload may be preceded by a run of `0x11` bytes that
pushes the real data onto an alignment boundary. The boundary depends on the
chunk:

| Chunk | Alignment |
| --- | --- |
| `0x00134B01` (vertex buffer) | 128 |
| `0x33320002` (texture pixel data) | 128 |
| `0x00134012` (texture references) | 8 |
| everything else | 16 |

Getting one of these wrong is silent. The pixel block carries 120 bytes of
filler in every one of a region's 206 packs, and reading it 120 bytes early
displaces every texture in the game by a fraction of a row — the result still
looks like texture, so it survives a glance, while nothing in the world lines
up. The check that settles it: after skipping the filler, the texture slices
fill the payload exactly, with zero bytes spare, in all 206 packs.

The filler is only written when the writer needed it. Some chunks sit on an
unaligned offset and begin with real data that happens to be zero, so a parser
must check that the bytes it is about to skip really are `0x11` and skip nothing
otherwise. Skipping blindly corrupts the texture tables; skipping every `0x11`
it finds eats the first byte of four vertex buffers whose data legitimately
starts with `0x11`. The alignment is computed from the **absolute file offset**,
so nested readers have to track where their buffer starts.

**Zero gaps.** Streaming sections are padded out to a 2 KiB boundary with zeros.
A top-level reader has to walk past those runs instead of treating `id = 0,
size = 0` as a chunk.

## 2. Region layout — `L4Rx.BUN`

| Chunk | Contents |
| --- | --- |
| `0x00034112` | region bounds |
| `0x80034150` | visibility / preculler data |
| `0x00034110` | **streaming section table** |
| `0x80034020`, `0x8003B200` | AI paths, traffic, events |
| `0x8003B600` | **parameter maps**: fog, screen tint, rain, wind and reverb by place (§5d) |
| `0x00034107`, `0x00034108` | scenery override table and scenery groups: race barriers and other switched objects (§5b) |
| `0x00035021` | particle emitter definitions (§5c) |
| `0x00037080`, `0x00037090`, `0x00037110`, `0x00037150`, `0x00E34010` | world animations: scene nodes, placements, controllers, banks (§5e) |

### Section table entry — 80 bytes

| Offset | Type | Meaning |
| --- | --- | --- |
| `0x00` | `char[4]` | section tag, ASCII, zero padded (`"C29"`, `"G26"`, `"R99"`) |
| `0x08` | `u32` | section number |
| `0x14` | `u32` | offset into `STREAML4Rx.BUN`, always 2 KiB aligned |
| `0x18` | `u32` | total size in bytes |
| `0x1C` | `u32` | size excluding the texture pack; equal to `0x18` when there is none |
| `0x20` | `u32` | streaming order key |
| `0x24` | `float[3]` | section centre in world space |
| `0x30` | `u32` | hash |

`L4RA` has 393 entries. The first few describe shared data — global textures,
the backdrop, common props — and carry a zero position; the rest are the
positioned world blocks. The centre at `0x24` agrees with the geometry: section
`A30` is listed at `(-1225, 2557, 304)` and its terrain spans
`(-1377..-1065, 2343..2777, 232..360)`.

## 3. Geometry — `0x80134000` solid list

```
0x80134000  SolidListPack
├── 0x80134001  header
│   ├── 0x00134002  pack path at +0x10 ("Location4\RegionA\Pipeline\PC\...")
│   └── 0x00134003  (hash, offset) lookup pairs
└── 0x80134010  SolidObject                       (repeated)
    ├── 0x00134011  object header
    ├── 0x00134012  texture references
    └── 0x80134100  mesh
        ├── 0x00134900  descriptor
        ├── 0x00134B01  vertex buffer
        ├── 0x00134B02  material table
        └── 0x00134B03  index buffer
```

### Object header — `0x00134011`

| Offset | Type | Meaning |
| --- | --- | --- |
| `0x0C` | `u32` | version (`0x16` throughout) |
| `0x10` | `u32` | object hash |
| `0x14` | `u32` | triangle count |
| `0x20` | `float[3]` | bounds minimum |
| `0x30` | `float[3]` | bounds maximum |
| `0x40` | `float[16]` | 4x4 world matrix, row major, translation in row 3 |
| `0xA4` | `char[]` | zero-terminated name, runs to the end of the chunk |

Objects that the game never needs bounds for store the **empty-box sentinel**:
minimum `+FLT_MAX`, maximum `-FLT_MAX`. Treat `min > max` as "no bounds" or the
box explodes to the float range.

The name prefix is the artists' classification and is worth honouring:

| Prefix | Meaning | Transform |
| --- | --- | --- |
| `TRN_` | roads and terrain of a section | identity; vertices already in world space |
| `PAN_`, `SKYDOME` | painted backdrop | placed, but 8 000 to 19 000 units across |
| everything else | buildings, signs, street furniture | placed by the matrix |

Of the objects inside positioned sections, 4349 are `TRN_` with an identity
translation and 2650 are props with a real one.

### Descriptor — `0x00134900`

| Offset | Meaning |
| --- | --- |
| `0x10` | material count |
| `0x24` | triangle count |
| `0x34` | vertex count |

### Vertex buffer — `0x00134B01`

Stride is `payloadSize / vertexCount`. Only two layouts exist across every
shipped region:

| Stride | Layout |
| --- | --- |
| 24 | `float3 position`, `u32 colour (ARGB)`, `float2 uv` |
| 36 | `float3 position`, `float3 normal`, `u32 colour (ARGB)`, `float2 uv` |

10 585 objects use the 24-byte layout and 115 the 36-byte one. The colour is the
baked night lighting; the original renderer combines it with the texture at 2x
(`D3DTOP_MODULATE2X`), and plain modulation leaves the city almost black.

### Material table — `0x00134B02` — 60 bytes per entry

| Offset | Type | Meaning |
| --- | --- | --- |
| `0x00` | `float[3]` | bounds minimum |
| `0x0C` | `u32` | index count |
| `0x10` | `float[3]` | bounds maximum |
| `0x1C` | `u32` | index into the object's texture reference table |
| `0x20` | `u32` | flags, always `0xFF` in the shipped data |
| `0x34` | `u32` | first index |

The ranges tile the index buffer contiguously: entry *n* starts where entry
*n-1* ended. The texture index is in range for all 28 018 materials of `L4RA`.

### Index buffer — `0x00134B03`

`u16` triangle list, `triangleCount * 3` entries. The chunk is padded past that,
so trust the count rather than the byte size.

**Winding.** The meshes are wound for Direct3D's default left-handed convention,
where back faces are the counter-clockwise ones. A renderer that uses
right-handed view and projection matrices — as this editor does, to keep the
game's Z-up world without flipping an axis — inverts the parity a triangle ends
up with on screen, and has to cull clockwise instead (`D3DCULL_CW`). Getting
this backwards culls the front faces: the city still draws, but every surface is
inside out and it reads as a scatter of loose shells rather than solid buildings.

### Texture references — `0x00134012`

Eight bytes per entry: a `u32` texture hash followed by four reserved bytes.

## 4. Textures — `0xB3300000` texture pack

```
0xB3300000  TexturePack
├── 0xB3310000  info
│   ├── 0x33310001  pack name and path
│   ├── 0x33310002  hash table, 8 bytes per texture, sorted ascending
│   ├── 0x33310004  texture headers, 124 bytes each
│   └── 0x33310005  format table, 32 bytes each
└── 0xB3320000  data
    └── 0x33320002  pixel data for every texture, mip levels largest first
```

### Texture header — `0x33310004` — 124 bytes

| Offset | Type | Meaning |
| --- | --- | --- |
| `0x0C` | `char[20]` | name |
| `0x24` | `u32` | hash, the value materials reference |
| `0x28` | `u32` | pack id, identical for every texture in a pack |
| `0x30` | `u32` | offset into the pixel data |
| `0x38` | `u32` | byte size covering every mip level |
| `0x40` | `u32` | byte size of mip 0 |
| `0x44` | `u16` | width |
| `0x46` | `u16` | height |
| `0x48` | `u8` | log2 width |
| `0x49` | `u8` | log2 height |
| `0x4E` | `u8` | mip level count |

The hash is EA's string hash. Names that differ only in their last characters
produce hashes that differ by the same amount, which is what confirms the field:
`ARC_WHOUSE_DOOR_01/02/04/05` hash to `0x52DF42/43/45/46`.

### Format table — `0x33310005` — 32 bytes

`0x14` holds a `D3DFORMAT`. In `L4RA` that is `"DXT1"` for 1417 textures,
`"DXT3"` for 171 and `41` (`D3DFMT_P8`, 8-bit palettised) for 23.

### Palettised textures

Only 23 textures in a region are palettised, which makes them easy to write off
— but they are `RDP_LANEA`, `RDP_INTERSECTMIDTILE`, `ARC_SIDEWALK_E` and the
rest of the road and pavement set, together 3986 of the 27 987 material slots.
Skipping them leaves every street in the city untextured.

Two header fields are zero for DXT textures and filled in for these:

| Offset | Meaning |
| --- | --- |
| `0x34` | palette offset, addressed like the pixel offset at `0x30` |
| `0x3C` | palette size, always 1024 = 256 entries of 4 bytes |

The palettes sit in the gap the pixel block leaves before its first texture
slice — 21 504 bytes in `L4RA`, enough for all of them, packed one after
another from offset zero.

Each entry is a `D3DCOLOR`, so its low three bytes go straight into an
A8R8G8B8 surface. The fourth byte is **not** opacity: it holds a noisy gloss
pattern the game uses for wet road reflections, ranging over most of 0..255
with no bimodal structure. Treating it as alpha leaves every road nearly
invisible; road surfaces are opaque.

## 5. Scenery — `0x80034100`

This is where most of the city lives. L4RA holds 10 704 solid objects but
**48 270 scenery instances**: the trees, signs, railings, street furniture and
repeated buildings are each stored once and placed many times.

```
0x80034100  Scenery
├── 0x00034101  header
├── 0x00034102  SceneryInfo   -- 68 bytes, the models this section can place
├── 0x00034103  SceneryInstance -- 64 bytes, one placement each
├── 0x00034104  bounding-volume tree over the instances
├── 0x00034105  override hooks: (index, instance) pairs into the region's 0x34107 (§5b)
└── 0x00034106  preculler blocks, 128 bytes each
```

(This tree used to label `0x00034104` "groups" and `0x00034106` "overrides";
both readings were wrong, see below and §5b.)

### SceneryInfo — 68 bytes

| Offset | Type | Meaning |
| --- | --- | --- |
| `0x00` | `char[32]` | model name |
| `0x20` | `u32` | hash of the SolidObject to place |
| `0x24` | `u32` | the same hash again |
| `0x3C` | `float` | radius |

Every one of the 48 hashes in a sample section resolves to a solid object
somewhere in the region.

### SceneryInstance — 64 bytes

| Offset | Type | Meaning |
| --- | --- | --- |
| `0x00` | `float[3]` | world bounds minimum |
| `0x0C` | `float[3]` | world bounds maximum |
| `0x18` | `u16` | index into the SceneryInfo table |
| `0x1A` | `u16` | flags (see *Instance flags* below) |
| `0x1C` | `u32` | preculler block, 1-based (below) |
| `0x20` | `float[3]` | translation |
| `0x2C` | `i16[9]` | 3x3 matrix, row major, fixed point with 1.0 = 0x2000 |

Scale is baked into the 3x3 matrix, so its rows are only unit length for
instances drawn at 1:1. A row of `(5843, -5742, 0)` has length 8192.1, and where
a row measures 6863 the instance is drawn at 6863/8192 = 0.838 scale, which is
exactly the ratio between its stored bounds and the model's.

The transform is `rotation * scale` from `0x2C` with the translation from
`0x20`, applied to the model's own local geometry. The model's header matrix is
**not** composed with it. Rebuilding each instance's world bounds this way
reproduces the stored box with a median centre error of 0.03 units across all
44 158 instances that resolve; composing the header matrix instead gives a
median error of 1571 units.

### Instance flags — the `u16` at `0x1A`

An instance is drawn when `((flags & 0xFF) ^ 0x60) & view cull flags == 0`
[code].

| Flag | Meaning |
| --- | --- |
| `0x10` | hidden until a scenery group shows it (§5b) |
| `0x20` | rear view mirror (view 3) |
| `0x40` | road reflection and car environment views (cull flags `0x800` and `0x1000`) |
| `0x0100` | **the model is in world space**: drawn without the instance matrix (below) |
| `0x200` with `0x10` | race barrier, hidden until its race (`0x210`) |
| `0x400` | take the second LOD slot instead of the third for the reflection views |

Retail L4RA counts by flags [files]: 0 22381, `0x60` 10927, `0x210` 5610,
`0x20` 3629, `0x560` 2942 (terrain), `0x100` 1448, `0x40` 850, `0x10` 105,
`0x70` 102.

**`0x0100`, world space** [code, game]. `sub_5D6570` draws an instance with
this flag **without its matrix**. It is the switch behind the pre-placed mesh
heuristic (see *Pre-placed meshes*), and it matters when writing:

- Moving or rotating a `TRN_` chunk changes nothing in the game while `0x0100`
  stays set. Clear it, and rebase the mesh around its own origin.
- Setting `0x0100` on an ordinary object draws it at the world origin: a port
  that set it on all roads stacked every road piece at (0, 0, 0).
- Deleting a shipped instance therefore has to clear `0x0100` as well as zero
  the matrix: with the flag left on, the zeroed matrix is never applied and the
  mesh stays in the game. The editor's deletion (`DeletedRecord` in
  `nfs/SceneryEdit.cpp`) does both; deletions saved before that are upgraded
  at the next save.

**Road reflection** (view 4) draws the third LOD slot of the scenery info, or
the second with `0x400`, for models of 32 pixels and up; a slot of 0 is not
drawn. Each instance is mirrored about its own origin, so an object off the
ground mirrors in the wrong place. Retail leaves terrain (`0x100`) out of the
reflection [code].

**A pack is drawn by its number** [game]. The game draws a scenery pack only
when the pack's **number** (the one in its `0x00034101` header, not the
section's number) is on the current drivable section's loading list.
Underground 1's always-loaded section X99 holds 85 packs numbered after areas,
each drawn only from the areas that list it; merging them into one pack drew
them everywhere.

### The rest of the chunk, and adding placements

Measured over the 606 scenery chunks of L4RA (and the 1986 of all regions for
the writer's round trip):

- **Header `0x00034101`**, 60 bytes: all zero except the section number at
  `0x0C`. A section usually carries two chunks: one with its own number and a
  larger one numbered 20 lower (A21 = 121 also holds 101), which is not a
  section in the table.
- **Only `0x00034103` is aligned**: its records start on a 16-byte file
  boundary (12 bytes of `0x11` in the shipped files). Every other child
  starts straight after its header — one tree even begins with a real `0x11`
  byte, which a filler check would eat.
- **SceneryInfo `0x28`** is zero; `0x2C` and `0x2E` are u16 counts, `0x2C`
  equal to the model's triangle count for 74% of entries (the rest look like a
  second level of detail). The same model's entry differs between sections in
  942 of 6406 cases, so a new entry is best copied from one the game ships.
- **SceneryInstance `0x1C`** is a u32, 1-based index of a 128-byte block in
  `0x00034106` (0 = none); `0x3E` is always 0. The blocks are 32x32-bit masks
  whose meaning is still open; block 0 is all zeros. For new or moved
  instances a safe value is known [game]: ports that write a single all-zero
  block and point every instance at it (or write 0) get nothing culled by the
  preculler, and that works in the game. This answers the practical side of
  experiment E6 in `STREAMING-STUDY.md`; the mask semantics stay open.
- **`0x00034104` is a bounding-volume tree** over the records. 36-byte nodes:
  `float3 min, float3 max, u16 count, u16 child[5]`. A child below `0x8000`
  is a record index; above, it is node `0x10000 - child`. Node 0 is the root.
  Every record appears once, every other node is referenced once, and each
  box is the union of its children's (605 of 606 packs; the last one only
  looked otherwise to a parser that stripped its leading `0x11`). The shipped
  trees are minimal 5-ary trees, `ceil((n - 1) / 4)` nodes.
- **Models placed are local or library, never foreign.** Of 12 858 model
  entries, 7095 name a model stored in a library section (X/Y/Z, no world
  position, always loaded) and 5763 one stored in the same section. None
  names a model that lives in another positioned section — the game would
  only have it while that section happened to be streamed.

The editor adds placements (`nfs/SceneryEdit.cpp`) by appending records after
the shipped ones, so no index held elsewhere moves, and inserting each into
the shipped tree with the smallest change: into the first node on the way
down with a free slot, else down the child node that grows least, else a new
node pairing the new record with its nearest sibling. Bounds grow along the
path. Re-encoding a pack with nothing added reproduces it byte for byte; with
additions the result passes the same tree checks (`ValidateScenery`, run
before every save). Saves always start from the shipped chunk, so records an
earlier save appended are written again from the editor's list rather than
stacked, and the section is spliced through the same 128-byte-preserving path
as the collision import.

### Edits as a list, sections rebuilt from the shipped bytes

A span holds a whole section with every edit applied, which is what the game
needs but not what a person wants to undo. The edits are recovered by
comparing the section the editor shows with the shipped one
(`io/SectionEdits.cpp`), chunk by chunk and matched by id and order: in a solid
list, an object whose bytes changed is an edited mesh and one the shipped list
lacks was copied in; in a texture pack, textures added or changed; a changed
collision pack; scenery records past the shipped count are placements. Every
save -- placements, collision import, mesh import -- and every revert rebuilds
the section **from the shipped bytes** with the edits that are to remain, each
laid on the file position modulo 128 it was made for. So no kind of edit can
undo another, and a section left with none goes back to the shipped file
(its spans are removed). Checked on a Q38 carrying 15 edits: rebuilding with
all of them gives the same 15 back, with none gives the shipped bytes, and
each of the 15 left out in turn gives exactly the other 14.

A mesh import brings one rebuild per mesh, each from the shipped section with
that mesh alone, so the meshes of a section are gathered before the section is
written once. Every mesh is rebuilt from the OBJ whole -- faces, UVs, colours and
materials. There used to be a shortcut for a mesh whose vertex count had not
changed: only its positions were written back, as a `.frsmesh` overlay at the
vertices' shipped file position. A matching count says nothing about order
(Blender lists vertices by its `v` lines, a rebuilt mesh stores them by face
corner), so a mesh whose count happened to come out equal got its positions on
the wrong vertices and kept its old materials; and the overlay missed the game
once the section was served from a span elsewhere. Existing `.frsmesh`
overlays of a mesh are removed when it is imported again.

**Game textures on import.** A TGA is the format the export writes the game's
textures in, so on import a TGA -- beside the OBJ or painted by a material --
whose name matches a texture of the region is that texture, not a new one: it
is referenced by hash, and copied as it is (compressed levels untouched) into
the section's pack only if the section cannot already see it (it is neither in
a library pack nor in the section's own). A `tex_<hash>` material resolves the
same way. Any other image format is a new texture and goes through the DXT
compressor. Windows' imaging component has no TGA decoder, so TGAs are read by
the editor itself; before that, a TGA beside the OBJ made the import of every
mesh that went through a rebuild fail.

**Back faces.** The world shaders draw both sides of a triangle, so the
coplanar back-to-back pair a mesh tool writes for a two-sided face z-fights in
the game; lifting one of them apart still flickers from a distance [game]. The
OBJ reader (`io/ObjMesh.cpp`) drops a face lying back to back on an earlier one
of the same object (the same corners, facing the opposite way) and logs how
many it left out.

### Taking a model to another section

A solid list's header says how many objects it holds — u32 at `0x0C` of
`0x00134002` — and lists them in `0x00134003` as `(hash, 0)` pairs sorted by
hash (1142 of 1145 packs; the other three have extra pairs). Every pack has a
zero-id padding chunk between the header and the first object.

To place a model stored in another positioned section, the editor copies its
`0x80134010` chunk byte for byte into the target's first solid list, on the
same file position modulo 128 as the original (every alignment inside an
object divides 128), with the count raised and the pair inserted in order;
the padding after the header is remade so the shipped objects keep their
positions modulo 128. Textures the model uses that are neither in a library
pack nor in the target's own are copied into the target's pack as they are,
mips included. Example: `XB_OILTANK_1B_BS_00` lives in H24 and H29 (a
never-instanced model on the storage grid, 180 triangles); three of its four
textures are in Y0, the fourth (`9412C4BF`, 128x128 DXT1) only in H24/H29.

### Placements, and drawing each one once

Every solid object also carries a real world placement in its own header matrix
— none of them are parked at the origin — and scenery instances add further
ones. Two traps follow:

- A region stores some meshes twice at the same spot. In L4RA 1794 hashes appear
  on two objects, at identical positions and with identical triangle counts;
  section X50 is a verbatim partial copy of X0. Drawing both makes every shared
  surface z-fight, which looks like the city breaking into flickering fragments.
- A few hundred instances repeat a placement that an object's header matrix
  already covers — 532 of the terrain instances, for example, are simply the
  section's own roads listed again with an identity transform.

The rule that handles both: **draw each (model, transform) pair exactly once.**
Suppressing an object because something instances it is wrong and costs about
2500 correctly placed meshes, roads among them.

## 5a. Collision — `0x80034130`

The car does not drive on the render mesh. Each positioned streaming section
carries one collision pack of its own: simplified ground polygons plus barrier
lines, in 16-bit fixed point, bucketed on a 32-unit grid. Every rule below was
checked against every pack of all eight regions (L4RA: 255 packs, 153 625
polygons, 21 699 barriers) with zero exceptions.

```
0x80034130  collision pack
├── 0x00034131  header, 48 bytes
├── 0x00034134  polygons, 32 bytes each
├── 0x00034136  cell references, 4 bytes each
└── 0x00034137  grid cells, 4 bytes each, gridWidth * gridHeight of them
```

### Header — `0x00034131`

| Offset | Type | Meaning |
| --- | --- | --- |
| `0x00` | `u32` | section number, equal to the table's `0x08` |
| `0x04` | `u16` | grid width (cells along x) |
| `0x06` | `u16` | grid height (cells along y) |
| `0x08` | `float[2]` | grid origin, a multiple of 32 |
| `0x10` | `float[3]` + pad | bounds minimum |
| `0x20` | `float[3]` + pad | bounds maximum |

The bounds include the barriers, so their z runs to `-127` / `+127`
regardless of the real terrain.

### Polygon — `0x00034134`

| Offset | Type | Meaning |
| --- | --- | --- |
| `0x00` | `u8` | **race barrier group**, 0 = always present (below) |
| `0x01` | `u8` | **extreme-corner code** (below) — not a surface id |
| `0x02` | `u8` | **surface** (below): 1 `ROAD` on most ground, `0x14` `SOLID_WALL` on most barriers |
| `0x03` | `u8` | flags: `0x10` quad, `0x02` vertical, `0x04` infinite barrier, `0x01` compare heights, `0x08` not ground, `0x40` never cached (see *Flags* below); barriers are always `0x16` |
| `0x04` | `i16` | height base, whole units |
| `0x06` | `u16` | always `0xFFFF` |
| `0x08` | `i16[4]` | x of each corner, in eighths of a unit |
| `0x10` | `i16[4]` | y of each corner, in eighths of a unit |
| `0x18` | `i16[4]` | z of each corner, in 1/256 of a unit, added to the base |

So `x = X / 8`, `y = Y / 8`, `z = base + Z / 256`. The height is finer than the
plan position on purpose: the car rests on it. Checked against the render
mesh of section A21, every ground corner lands within 0.17 of a road vertex in
plan (the 1/8 quantisation) and within 0.02 of its height (median 0.002).

A **triangle** repeats its third corner in the fourth slot and has flag `0x10`
clear; a **quad** has four distinct corners in perimeter order and splits into
`(0,1,2)` and `(0,2,3)`.

A **barrier** is recognised by its z values `-32512, +32512, +32512, -32512`
with a zero base: a wall from corner 0 to corner 2 (corners 1 and 3 repeat
them) that is infinitely tall for any practical purpose. The file gives it no
real height, so the editor stands it on the nearest ground corner.

**Extreme-corner code (byte 1).** Two bits each name the corner (0-3) lying
furthest toward -x (bits 0-1), -y (2-3), +x (4-5) and +y (6-7), in the stored
eighths; a tie goes to the later corner. A triangle repeats its third corner,
so corner 3 wins its ties. It is a bounding box the game tests a car against
before the polygon itself, and it holds for every polygon of the eight regions
(about 432 000: ground, vertical faces and barriers). A polygon imported with
a code copied from another -- the material name used to carry it -- fails that
test wherever the two boxes disagree: in the game that was holes in new ground
that dropped the car (and reset it to the road), and walls it drove through.
The encoder computes it.

For a barrier, corners (a, a, b, b), the same rule gives the four heading codes
first found for walls:

**Barrier direction code.** Byte 1 of a barrier is computed from its segment
(corner 1 to corner 2, in the stored eighths): start from `0xFF` and clear bit 1
if dx > 0, bit 3 if dy > 0, bit 5 if dx < 0, bit 7 if dy < 0. So `F5` runs
0–90°, `D7` 90–180°, `5F` 180–270°, `7D` 270–360°, and `FD F7 DF 7F` are exactly
along an axis. It holds for all 51 202 barriers of the eight regions. The game
ignores a wall whose code disagrees with its heading: two side walls imported
with the `F5` of the wall they were copied from let the car through while the
walls that really ran 0–90° held. The encoder now writes the code itself.

**Barriers are one-sided.** In L4RA the road lies to the left of the segment's
direction (corner 0 to corner 2) for 13 868 barriers and to the right for 315
(the rest have ground on both sides or none). A wall imported facing the
wrong way let cars straight through in the game, so the importer writes every
barrier it reads in both directions.

**Vertical faces.** Every polygon with flags `0x12` — 17 788 of them in L4RA,
and no other — has zero area in plan: it is a wall with a real height (a kerb,
a building side), stored as an ordinary quad with its own z values. Barriers
(`0x16`) are the same thing with `0x04` added and the height made infinite.

**Race barrier group (byte 0).** Non-zero on 12 049 L4RA polygons: an index
into the region's `0x3412F` list (126 records of 32 bytes `{hash, count, 1,
name}`, names `BARRIERS_<race>` and `PLAYER_BARRIERS_<race>`; the counts match
the polygons carrying each index). The game switches a group on only in the
races that name it, so in free roam those polygons do not exist -- 97% of them
stand on an `XO_TRACKBARRIER*` placement. Anything the editor creates for free
roam must carry 0. (Found converting Bayview to Underground 1, where nothing
switched them off and they became invisible walls everywhere.)

**Surface (byte 2).** An index into SPEED2's surface table at `0x800EE0`
(`0x98` bytes per entry, name pointer first): 0 `NONE`, 1 `ROAD`, 2
`ROAD_WET`, 3 `ROAD_DRIFT`, 4..6 `ROAD_SMOKE_1..3`, 7 `ROAD_BRIDGE`, 8 `DIRT`,
9 `GRAVEL`, 10 `ROUGH_ROAD`, 11 `COBBLESTONE`, 12 `STAIRS`, 13 `PUDDLE`, 14
`DEEP_WATER`, 15 `GRASS`, 16 `SIDEWALK`, 17 `WOOD`, 18 `PLASTIC`, 19 `GLASS`,
20 `SOLID_WALL`, 21 `SEE_THROUGH_WALL`, 22 `PLANT`, 23 `POST`, 24 `PILLAR`, 25
`METAL_GRATE`, 26 `METAL`, 27 `CHAINLINK`, 28 `CAR`. Prop classes (`0x34026
+0xD4`) index the same table.

**Polygons repeated across packs.** 21 834 polygon shapes of L4RA are stored
in two or more packs (446 section pairs, mostly neighbours: the polygons along
a section edge). An edit that removes or moves such a polygon in one pack
leaves the other copy colliding; new geometry near an edge needs to go into
every pack the car may be using there.

### Flags — byte 3

`0x10` (quad), `0x02` (vertical) and `0x04` (infinite barrier) are above. The
other three bits, earlier listed as unknown, are all read by
`TopologyCoordinate::NewGetElevation` `0x5E72E0` [code]:

| Bit | Meaning |
| --- | --- |
| `0x01` | **Compare heights.** A floor without it is taken as soon as the point is strictly inside it, at any height. A floor with it competes: the highest floor at most 1 m above the car wins. Retail sets it on about 99 % of the floors that have another floor above or below them [files]. |
| `0x08` | **Not ground.** Tested together with `0x02` (`& 0x0A`), so a polygon with it is never a floor. |
| `0x40` | **Never cached.** Such a floor is not put into the four-entry recent-floor cache, and it does not end the walk of the cell. In retail L4RC (drift arenas) 44 % of the floors have it: the `ROAD_DRIFT` patches lie 2-14 cm over the floor of the lot, and both are flagged. L4RA has it on 2.3 % [files]. |

- **`0x01` missing** [game]: a bridge deck without it became the ground of the
  street under it, and cars on that street were pushed up onto the deck.
  Where only one floor lies at a point, `0x01` gives the same result as no
  flag, so it is safe to write it everywhere.
- **`0x40` is risky** [game]. Writing it on every floor that overlaps another
  2 cm to 4.5 m apart, as the drift arenas do, made cars on a port step up in
  one frame onto scenery floors lying 10-50 cm over the original ones (a plaza
  behind a fence over a sloping sidewalk) and be thrown into the air. Without
  the flag the cache keeps the car on the floor it is on until it leaves it,
  which hides small overlaps. Use `0x40` only for deliberate overlays (painted
  patches), and otherwise keep overlaps flat (a drift band laid 1/256 over the
  floor under it works). The editor never writes `0x40` (a shipped polygon
  keeps whatever it has).
- **`0x04` only on the infinite barriers.** All 21 699 walls with `0x16` in
  L4RA are the ±127 extruded barriers, and none stands over a road [files].
  Walls with a real height are `0x12`. Writing `0x16` on a bridge railing made
  it a wall through the street below [game].

### How the game walks a cell

`NewGetElevation` `0x5E72E0` walks the references of the cell under the point
in their stored order. Rejects of a single reference (sub-cell mask, corner
box, switched-off race group) jump to `0x5E783D`, the next reference. A
polygon with `flags & 0x0A` -- a wall, a barrier or a `0x08` face -- does not:

```
005E7554  test cl, 0Ah
005E7557  jnz  loc_5E765D     ; end of the cell walk: is there a best floor? else the next pass
```

So the walk **ends** at the first wall, and any floor listed after it does
not exist for the game (verified in the exe). Retail always lists floors
before walls, which is why shipped data never shows it; collision with a wall
first in a cell loses its ground there, the car sinks and the game resets it
("walls that teleport the car" [game]). The walk also ends at the first floor
within 4 m below `z + 1` (unless that floor has `0x40`), so where floors
overlap the higher one has to come first, or a car on it is pulled down
through it; retail lists the higher floor first in about 90 % of stacked
pairs [files].

The inside test is `max(-1, edge values) <= 0` and assumes counter-clockwise
corners seen from above: a clockwise or concave polygon is never "inside".

The test drive's port (`src/drive/world/TopologyU2.cpp`) used to skip such a
polygon (`continue`); it now ends the walk there (`break` to the "best found?"
check), so the test drive fails where the game fails.

### What resets the car

From `Car::Move` [code]:

- `sub_5ECBA0`: the ground is more than 0.25 m above the car.
- `sub_604740`: no topology, or the car is more than 5 km from the track path.
  It is checked every 32 frames, at the car and 1 m to the side.

None of the resets looks at the surface class [code]: "grass resets" seen on a
port were holes in the collision. A plain hole resets the car; a gap meant to
be jumped needs a floor under it **and** a `RESET` zone with a height (see
*Free-roam zones*).

### Export for editing

With the collision shown (Ctrl+K) a click selects collision polygons instead of
meshes: Ctrl-click adds or removes one, Shift-click takes the section's whole
pack. Ctrl+E then writes them as OBJ (`io/CollisionExport.cpp`): one object per
pack named `collision_<section>`, one face per polygon in file order, corners
welded where they coincide, Y-up and moved by the manifest's `origin` like the
mesh export. The material name carries bytes 0–3 of the record
(`ground_00_34_01_00`, `barrier_00_5F_14_16`) so a face keeps its attributes
through Blender; barriers are written as the viewport's wall quads, of which
only the bottom edge means anything. The JSON manifest lists, per object, the
pack's polygon indices in face order. Reading an exported Q38 (1560 polygons)
back and comparing with the file gave identical attributes and shapes and a
largest position error of 0.00008.

### Grid — `0x00034137` and `0x00034136`

Each cell is `(u16 first, u16 count)` into the reference list, row by row
(`index = cy * width + cx`), the lists contiguous. Each reference is
`(u16 polygon index, u16 mask)`; bit `sy * 4 + sx` of the mask is the 8-unit
sub-cell `(sx, sy)` of that cell.

The whole pack can be rebuilt from the polygons alone. These rules reproduce
every non-empty pack of all eight regions **byte for byte** (`EncodeCollision`,
checked with `FRS_COLLISION_SELFTEST=1`):

- **Header bounds** are the min/max of the stored corners, barriers counting
  as z = ±127.
- **Grid origin** is `floor(min / 32) * 32`; the size is
  `floor(max / 32) - floor(min / 32) + 1` cells on each axis.
- **Height base** of a ground polygon is `floor((zmin + zmax) / 2)` over its
  corners. (Not the floor of the lowest corner: that fails on 5% of them.)
- **Cell membership** is a polygon-against-square overlap in plan (separating
  axis test, polygon outline as stored, barriers and vertical faces as
  segments). A cell owns its low edges and not its high ones: a polygon whose
  lowest x or y equals the cell's far boundary belongs to the next cell only.
  Touching counts as overlap, with a tolerance of 1e-4 — without it float
  noise drops the polygons that meet a grid line exactly.
- **Mask**: inside a member cell each sub-cell is tested the same way, closed
  on all sides.
- **Order within a cell** is not file order. The tool lists polygons grouped
  (ground, vertical faces, barriers — by some source order the file does not
  record), but every pack is consistent with one global order, which a
  topological sort of the consecutive pairs in the cells recovers. The
  grouping is not cosmetic: the game's walk of a cell ends at the first wall
  (see *How the game walks a cell*), so floors must come first.
- **An empty pack** is still written: a 1x1 grid at the origin, bounds
  `+FLT_MAX` / `-FLT_MAX`, no polygons and one empty cell (84 bytes).

### Collision the game accepts

Byte-for-byte reproduction holds for shipped packs, and a shipped pack the
editor has not touched is still re-encoded exactly as it was. A pack that was
edited, or built from nothing, is first brought to the rules retail's tool
always follows and the game's lookup relies on (`EncodeCollision`,
`nfs/Collision.cpp`):

- **No degenerate polygons after quantising.** Retail has none [files], and a
  car touching one gets a NaN normal, which showed as random resets at walls
  [game]. Repeated corners are compared **in 3D**, plan in 1/8 and height in
  1/256: retail does have quads with two corners on one plan point at
  different heights, so a plan-only comparison is wrong. A quad that loses a
  corner becomes a triangle (third corner repeated in the fourth slot); a
  floor with no area left, or a wall or barrier with no length, is dropped.
- **Convex floors.** A concave floor quad is split into two triangles at its
  reflex corner (the inside test fails everywhere on a concave polygon).
- **Walls stand upright.** The contact normal comes from the polygon, so a
  wall leaning a few degrees launches the car that touches it [game]; retail
  walls are 98 % vertical [files], and rounding to 1/8 alone leans thin walls.
  The top of a rising wall edge is snapped onto its foot (same x, y) when the
  two lie within 0.15 m in plan.
- **Floors before walls in every cell**, then vertical faces, then barriers.
- **Stacked floors highest first.** One height per floor is not enough to sort
  by: a ramp that passes 1.5 m over a flat floor is lower on average than that
  floor (the encoder used to sort by average corner height). Instead every
  overlapping pair of floors in a cell is compared at the centre of its
  overlap, the higher one goes first, and the cell is sorted topologically;
  ties keep the existing order.
- **`0x01` on stacked floors.** A floor lying more than 0.5 m over or under
  another one it overlaps gets `0x01`, so the two compete by height. `0x40` is
  never added.

In short, for any writer of collision:

1. Floors before walls in every cell.
2. Overlapping floors ordered highest first, compared pairwise at the centre
   of each overlap.
3. Convex polygons, counter-clockwise from above.
4. No degenerate polygons after quantising (1/8 in plan, 1/256 in height).
5. Upright walls: corners of a rising edge share x, y.
6. `0x04` (`0x16`) only on infinite barriers; real-height walls are `0x12`.
7. `0x01` on floors with another floor above or below; `0x40` only for
   deliberate flat overlays.

### Import

`io/CollisionImport.cpp` reads an edited export back (Ctrl+I recognises it by
its `collision_<section>` object names). Faces replace the exported polygons in
their slots, extra ones are appended, the pack is re-encoded, and the section
is rebuilt by splicing the new chunk in place of the old one, followed by a
zero-id padding chunk that keeps everything behind it on its 128-byte
alignment. It starts from the bytes the editor shows — a span from an earlier
mesh import included — and replaces that span file rather than stacking a
second one on the same section.

Face to polygon: the material name gives the attribute bytes; the quad bit
(0x10) and the vertical bit (0x02, zero plan area) are set from the shape; a
quad with a reflex corner in plan is cut in two through it; larger faces are
fanned into triangles. A barrier face becomes the segment its bottom edge
spans, directed by `normal x up` so the wall keeps the side it faced.

### Editing in the editor

`io/CollisionEdit.*` makes and places polygons; `ui/CollisionTools.*` is the
Collision menu over it. What they rely on:

- **Making.** Ground is a triangle or a quad convex in plan (an outline of
  more points is ear-clipped into triangles); a barrier is the two-point form
  above; a wall with height is a quad stored like the shipped `0x12` faces --
  (A foot, A top, B top, B foot), corners 0/1 and 2/3 sharing their plan
  position -- with its foot sunk 0.5 below the drawn points. Walls of either
  kind are written in both directions and cut into pieces of at most 8 units.
  New polygons carry race group 0 and the surface chosen (default `ROAD` for
  ground, `SOLID_WALL` for walls). New floors (`collision::MakeGround`) carry
  flag `0x01`, so they compete by height with anything under or over them.
- **Real-height walls, not barriers, for generated walls.** The walls at
  junction corners of the road creator and the building walls of city zones
  are written as real-height walls (`0x12`) instead of ±127 barriers
  (`0x16`): a barrier walls off any road passing above or below.
- **Placing.** A polygon goes into every pack that has ground under one of its
  corners or its middle -- the same rule that makes the shipped edge copies --
  and, over no ground at all, into the nearest pack.
- **Copies.** Two polygons are the same when their stored shape is (corners in
  eighths, heights in 1/256, barrier or not); delete, move, surface and race
  group changes find and change every copy.
- **From an object.** `io/CollisionGenerate.*` reads the placed mesh. A ramp or
  a deck -- an upward face that is not level and reaches down to the ground,
  or anything under 0.6 tall -- becomes ground on its upward faces. Anything
  else is solid: walls with height round the convex hull, in plan, of the
  vertices up to 2.5 above its foot (a tree's trunk, not its crown), plus
  ground on upward faces higher up so a car can land on a roof. Which way a
  face points is voted from the stored vertex normals, or from the highest
  faces when a model has none. A polygon a pack already holds is not added
  again, so making it twice changes nothing.
- **Saving.** Edited packs live in the loaded sections until Ctrl+Shift+S,
  which re-encodes each changed pack (`EncodeCollision`) as the section's
  collision edit, next to its placements, through the same rebuild as every
  other edit. `FRS_COLLISION_TOOLS_SELFTEST=1` runs the whole path on the
  loaded region without writing anything and logs the result.

## 5b. Scenery groups, race barriers and breakable props

The collision side of race barriers is the race group byte of §5a (an index
into `0x3412F`). Loading race N (`sub_578060`) also switches on the
**objects** [code, game]:

- **Scenery groups**, chunk `0x34108` in the region file. Each record:

  | Offset | Meaning |
  | --- | --- |
  | `0x00` | `u32` 11, `u32` 11 |
  | `0x08` | name[32] |
  | `0x28` | hash |
  | `0x2C` | `u32` 0 |
  | `0x30` | count |
  | `0x34` | `u16` indices into the override table `0x34107`, padded to 4 |

  `sub_5710A0` clears the hidden bit `0x10` of those entries.
- **Names:** `PLAYER_BARRIERS_<N>` (retail 4001: 157 objects) and
  `BARRIERS_<N>`, plus `BARRIERS_CAREER0..4`, `FREE_ROAM`, `FREE_ROAM_STAGE0`
  and `SMOKEABLE`.
- **Override table `0x34107`:** 8-byte entries `(i16 pack, i16 instance, u16
  flags, u16 1)`, grouped by the low byte of the pack number. Each scenery
  pack has hooks in `0x34105` `(index, instance)`.
- **Race barrier instances** carry flags `0x210` (hidden plus barrier).
- **Path barrier lines** (`0x3414D`) are found by the group name hash
  (`sub_5D5010`).
- **Per-track packs:** packs 2601..2619 (Z1..Z19) are switched per track
  through a per-track pack list (`sub_5DB610`).

Without the group, a race shows its barrier walls but not the barriers
themselves [game].

### Breakable props — spawner chunk `0x34027`

A header `(0x0B, 0x0B, section, number of spawners in sections with a lower
number, count)`, then 0x70-byte records [code, game]:

| Offset | Meaning |
| --- | --- |
| `0x00` | type key (= `+0x80` of the `0x34026` type record) |
| `0x04` | index into the override table `0x34107` |
| `0x08` | model hash |
| `0x10` / `0x20` | bbox |
| `0x30` / `0x40` / `0x50` | rotation rows |
| `0x60` | position, 1.0 |

- A spawner whose `+0x04` points outside the override table crashes at
  `0x5DB0EE`.
- Spawning hides the stand-in instance and takes a slot from
  `VirginSmokeableSlotPool` (256). When the pool is empty the prop is just
  missing, with no crash. `SmokeableSlotPool` is 32.

## 5c. Particle effects (Acid) — `0x35021`, `0x35020`

The shop markers' emitters can be copied as they are; this is what the records
mean, how the game runs them, and what a new emitter needs.

| Address | What |
| --- | --- |
| `0x491C70` `LoaderAcidFX` | loads `0x35021` (emitter definitions) and `0x35020` (placed emitters), and the containers `0x80035000` and `0x80035010` |
| `0x48BC00` | links a placed emitter to its definition by key |
| `0x48BC50` `AcidEffectSystem::AddAutoEmitter` | starts a placed emitter |
| `0x491E50` `AcidEffectSystem::TriggerEmitter` | makes particles |
| `0x492E10` `AcidEffectSystem::DoTimeStep` | switches emitters on and off, moves particles |
| `0x48BDF0` | draws them, from `eDisplayFrame` |
| `0x4881D0` | a curve: four cubics |
| `0x487360` | random numbers: xorshift 17, 13, 5, then `(x & 0x7FFFFFFF) * 2^-31` |
| `CarRenderInfo::TriggerEffect` `0x615CC0` | the cars' effects: `TriggerEmitter` with a negative time |

Pools: 250 emitters (0xD0 bytes each in memory) and particles of 32 bytes.

**Chunk header.** Both chunks start their body on the next 16-byte boundary
after the chunk header; then `+0x08` version, `+0x0C` count, `+0x10` records.
In `0x35020` the first word is the section number: `(section, 0, 6, count)`.

### Emitter definition — `0x35021`, version 4, 544 bytes [code, files]

| Offset | Type | Meaning |
| --- | --- | --- |
| `+8` | `u32` | key: hash of the emitter's name |
| `+12` | `u32` | texture hash |
| `+16` | `u32` | effect hash: the emitters of one effect fire together |
| `+24` | `u8` | 1 = all round: direction `(cos a, sin a, 0)` at any `a`, tilted by the spread |
| `+25`, `+26` | `u8` | log2 of the texture's columns and rows of pictures |
| `+27` | `u8` | pictures per second (0 = none) |
| `+28` | `u8` | start on a random picture |
| `+29` | `u8` | random flip of the turn |
| `+30` | `u8` | follows the emitter's own motion |
| `+31` | `u8` | shape: width and height factors `(1,1) (1,0.5) (0.5,1) (0,0)` (table `0x7F3944`) |
| `+32` | `u8` | orientation: 0 faces the camera, 1 flat in XY (the ground), 2 XZ, 3 YZ |
| `+33` | `u8` | made even with particles switched off |
| `+36`, `+40` | `f32` | on time, its variance (a fraction) |
| `+44`, `+48` | `f32` | off time, its variance (0 = for ever) |
| `+52`, `+56` | `f32` | particles per second, variance (0 makes 10 per second) |
| `+60`, `+64` | `f32` | life in seconds, variance (at most 60) |
| `+68`, `+72` | `f32` | speed in m/s, variance |
| `+76` | `f32` | turn at birth, degrees (random within ± half) |
| `+80` | `f32` | spread, degrees |
| `+84`, `+88` | `f32` | share of the emitter's motion taken, variance |
| `+92` | `u32` | type: which car marker a car effect stands on |
| `+96` | `3 × f32` | offset |
| `+112` | `3 × f32` | box the particles start in, around the offset |
| `+128` | `f32` | range: particles are made only within this distance of a camera |
| `+132` | `f32` | gravity along Z in m/s² (negative rises) |
| `+136` | `f32` | drag on the square of the speed |
| `+140` | `f32` | largest size on screen (0 = 512) |
| `+144` | `28 × f32` | the authoring tool's keys of the curves |
| `+256` | `16 × f32` | colour curve: R, G, B, A, each `t³ t² t 1`, values 0..255 |
| `+320` | `16 × f32` | size curve: size in metres, turn (65536 = a full turn) |
| `+0x180` | `char[64]` | emitter name |
| `+0x1C0` | `char[48]` | texture name |
| `+0x1F0` | `char[48]` | effect name |

### Placed emitter — `0x35020`, version 6, 208 bytes [code, game]

| Offset | Type | Meaning |
| --- | --- | --- |
| `+8` | `u32` | key |
| `+12` | `u32` | key of the definition |
| `+16` | `u32` | state: 0 off, 1 on, 2 and 3 only when triggered |
| `+0x28` | `u16` | 0 |
| `+0x2A` | `u16` | section |
| `+0x2C` | `u32` | scenery group that switches it on (0 = always on) |
| `+0x30` | 8 bytes | 0 |
| `+0x40` | `4 × 4 f32` | matrix, D3D row vectors (position in the last row) |
| `+0x80` | | velocity |
| `+0x90` | `char[]` | name |

Emitters written with the `+0x28` block this way show in the game [game]. The
game overwrites `+20..+47` at run time (timer, next particle, definition
pointer, active group, seed), so the file values there do not matter.

### How the game runs them [code]

**AddAutoEmitter:** the timer starts at the on time (state 1) or the off time
(state 0), randomised as `rand * var * t + t - var * t`. A time of 0 means for
ever.

**DoTimeStep(dt):** state 1 calls `TriggerEmitter(dt)`. The timer runs down;
at its end, on becomes off and off becomes on, with a new time and a cleared
wait. An emitter with an on time and no off time fires once.

**TriggerEmitter(time):** works only within the range of a camera. The time is
limited to what is left of the on time. While the time covers the wait, it
makes a particle; the next wait is `1 / (rate * (1 - var + rand * var))`, at
most 240 s.

**Each new particle:**

| Property | How |
| --- | --- |
| Direction | the placed matrix's Z axis turned around X, Y and Z by up to half the spread each. All round: `(cos a, sin a, 0)` turned around Y by the tilt, then by the matrix |
| Position | `offset + rand * box - box / 2`, through the matrix |
| Life, turn, speed | each varied by its variance |
| Picture | `rand * pictures` when the start is random |

**Each step:** life goes down by `dt * 1024` and the particle is gone at 0; the
picture moves on every `1024 / fps` ticks; per axis `v -= sign(v) * v² * drag *
dt`, on Z also `- gravity * dt`; `position += v * dt`, and a following
particle also moves with the emitter.

### How they are drawn [code]

The age is `t = 1 - life / (emitter's life)`. Colour and size come from the
curves at `t`; the colour is clamped to 0..255.

- **Orientation 0:** a quad facing the camera, width `size * shape w`, height
  `size * shape h`, no turn. The batch leaves out quads with alpha 20 or less.
- **Orientations 1 to 3:** a quad flat in that plane, turned by the particle's
  turn plus or minus the curve's turn (by the flip).
- **Picture:** column `p & (columns - 1)`, row `(p >> log2 columns) & (rows - 1)`.
- **Effect `worldnodepth.efx`:** colour = `2 * texture * vertex colour`, alpha
  = `texture * vertex alpha`, no depth write.
- **Blend** from the texture record's `+0x56`: 1 = source alpha over, 2 =
  added, 5 and 6 = with the destination's alpha. Alpha test at 95 when
  `+0x55` is 1.

### What a new emitter needs [code, game]

- **The texture must already be loaded when the region loads.** `LoaderAcidFX`
  resolves it then with `GetTextureInfo(hash, 0, 0)`: there is no default
  texture, and an emitter without its texture draws nothing. In practice the
  texture has to be in `GLOBAL\InGameCommon` (113 textures), which loads
  before any region; a texture in the region's own stream is too late.
- **The definition goes into the region file (`L4Rx.BUN`); the placement goes
  into the stream section** where the emitter stands.
- **Where the effects live in retail:**

  | File | Emitters | What |
  | --- | --- | --- |
  | `GLOBAL\GlobalB` | 92 definitions | the cars' and the map's |
  | each region file (`L4RA.BUN` ...) | 37 definitions in L4RA | event rings, smoke, shop icons, industrial smoke |
  | `FRONTEND\PLATFORMS\Showroom.BIN` | 1 | the manhole steam |

## 5d. Parameter maps — `0x8003B600` (fog, tint, rain, wind, reverb)

Fog, screen tint, rain, wind and reverb are set by place through **parameter
maps**: chunk `0x8003B600` in the region file, read by `LoaderParameterMaps`
`0x60B810` [code]. Each `0x8003B601` is one layer:

| Chunk | Contents |
| --- | --- |
| `0x3B602` | 0x24 bytes: `u32` bStringHash of the layer name, `f32` box x0 y0 x1 y1, `u32` nodes, sets, set size, fields |
| `0x3B603` | `u32` per field: 0 float, 1 int |
| `0x3B604` | `u32` per field: offset in the set |
| `0x3B605` | `sets × set size` bytes: the values |
| `0x3B607` | quad tree, 4 × `u8` per node |
| `0x3B608` | quad tree, 4 × `u16` per node |

**Quad tree.** A node has children 0 = x low y low, 1 = x low y high, 2 = x
high y low, 3 = x high y high. A node whose first child is 0 is a leaf, and
its second value is the set index. Outside the box, or with no tree, the set
is 0. Retail maps cover 10240 m in cells of 160 m (6 levels); deeper trees
work, since the walk halves the box until it meets a leaf.

A layer is found by the hash of its name, so a layer the code does not ask for
does nothing.

| Layer name | Read by | Fields |
| --- | --- | --- |
| `Normal Fog`, `Night Normal Fog` | `sub_614300` | 0 strength, 1 start in m, 2..4 colour, 5..7 (no reader on the PC) |
| `Night Raining Fog` | `sub_614300` | the same; mixed in by rain intensity |
| `Fog Blend Distance` | `sub_614300` | metres of cross fade while it rains |
| `Sunrise Screen Tint`, `Night Screen Tint` | `sub_5D77F0` | 0..2 colour, 3 amount, 4..6 |
| `Horizon Fog Visibility` | `sub_5DB900` | 1 value |
| `Rain` | `GetDesiredRainIntensity` | 0 intensity, 1 share of the time it rains |
| `Wind` | | 0 strength, 1 direction in degrees |
| `Reverb` | `sub_47C6D0` | 0 preset of `g_REVERBFXMODULES` `0x7ED608`, 1, 2 blend |

Reverb presets: 0 Car_Show, 2 Car_Show_Bass, 3 Garage, 4 Garage_sml, 5
Simple_Tunnel, 6 Simple_Tunnel_Sml, 7 Hills_Close, 8 Hills, 9 City_Open, 10
City_Dense, 11 Alley. Retail L4RA uses only 8..11: the map gives the
neighbourhood, not the tunnels.

### How the fog is computed [code]

Each frame `sub_614300` uses the position of the view:

1. **The set under the camera.** `sub_5FA660` cross-fades over 50 m when the
   set changes.
2. **The time of day:** Normal against Night. `sub_60FAA0` turns the sky every
   300 s with 20 s fades.
3. **Rain:** with intensity `r`, the result is `(1 - r) * dry + r * Night
   Raining Fog`.

Rendering (`eDisplayFrame`): vertex fog runs from the start distance to a far
distance of 250 m (`1 / (250 - start)`). The most fog is `strength`, but never
less than 0.4 (`dword_784564`), so the PC cannot turn fog off; to hide it,
give it the sky's colour. The sky dome is fogged too.

**Colour byte order.** The colour sent as FOGCOLOR is `field2 | field3 << 8 |
field4 << 16`, each times 0.75 (`flt_785510`). D3DCOLOR takes the low byte as
blue, so for a colour `(r, g, b)` on screen write field 2 = `b / 0.75`, field
3 = `g / 0.75`, field 4 = `r / 0.75`. The PS2 reads the same bytes as red
first, so console and PC retail hold the same values, with red and blue
appearing swapped on the PC.

### Authoring fog by district [game]

1. Choose a square box over the city and a power-of-two number of cells (128
   cells over 4096 m works).
2. Define sets of `(strength, start, colour on screen)`, plus a lighter "rain"
   version of each. Rain darkens the sky on top of its fog, so a dark district
   colour in `Night Raining Fog` gave a nearly black picture. The game's own
   rain fog shows as grey 96 on screen at strength 0.5 from 200 m.
3. Give each cell the set of its nearest seed point. Build the quad tree,
   merging four equal leaves into one.
4. Replace the three fog layers (`Normal Fog`, `Night Normal Fog`, `Night
   Raining Fog`) and keep every other layer as it is. Set 0 is the fog outside
   the box.

Each layer's set size and field types come from the template layer, so only
the values and the tree change.

**A trap:** the retail test region L4RR carries the demo-era Street X fog map
(green, red and brown zones over x -1920..-640, y -2400..-1120). A region
built from a copy of L4RR inherits it, which gave one port an unexplained
green fog.

## 5e. World animations — moving objects

A moving object (tram, planes, doors, spotlights) is not a scenery instance
with a changing matrix. It is a **scene**: a small tree of nodes, an animation
bank that moves them, a controller, and a placement that puts the scene into
the world, all in the region file (`L4Rx.BUN`) [code, files, game]. Retail
examples: the jumbo planes (`ANM_JumboPlane`, `XV_PlaneC`) in Z0, trains
(`ANM_Graffiti_*`), the casino's moving lights (`ANM_BridgeSpotlightA`), doors
(one scene with 23 placements).

| Chunk | What |
| --- | --- |
| `0x00E34010` | **Animation bank**: an EAGL toolkit ELF. Channels are named `<name>_q`, `<name>_t`, `<name>_s` (rotation, translation, scale). The animation hash is the hash of `<name>` |
| `0x37080` | **Scene node**, 0x70 bytes. One root node and one child node per animated model |
| `0x37110` | **Scene controller**, 0x50 bytes: `u32` scene hash, then zeros |
| `0x37090` | **Placement** of the scene in the world, 0xA0 bytes, one per place the scene stands |
| `0x37150` | **Placement companion**, 0x10 bytes: `(id, scene hash, pack, 0)`, one per placement |

All five chunks keep their payload 16-byte aligned, with `0x11` fill in front,
like the other aligned leaves.

**Scene node `0x37080`** (the layout retail uses, as written by a working
port):

| Offset | Meaning |
| --- | --- |
| `0x00` | `u32` 11 |
| `0x04` | node name hash. The root uses the scene hash; the child the hash of `<scene>_node` |
| `0x08` | parent hash (0 on the root; the child names the root) |
| `0x0C` | model hash. The root uses the hash of a dummy `ANM_<SCENE>_00` that does not have to exist; the child carries the real model |
| `0x20` | scene hash |
| `0x24` | animation hash (0 on the root) |
| `0x28` | `u8` channel mask: rotation 1, translation 2, scale 4 (0 on the root) |
| `0x29` | `u8`: `0xFF` on the root, 0 on the child (meaning not known) |
| `0x30` | 4 × 4 floats: start matrix (identity works; the node is relative to the placement) |

**Placement `0x37090`**, 0xA0 bytes:

| Offset | Meaning |
| --- | --- |
| `0x00` | id |
| `0x04` | scene hash |
| `0x08` | number of the scenery pack the scene is anchored to (2600 = Z0, always loaded, for the planes); inferred: the scene lives while this pack is loaded |
| `0x0C` | flags: 8 play, `0x20` loop, `0x2000` update even out of view, `0x200` and `0x400` random start. Retail: `0x2028` on planes, `0x28` on the rest |
| `0x10` | time scale |
| `0x14` / `0x18` | start / end times (0 and 1.0 work) |
| `0x20` / `0x24` | hashes of the events that start and stop it (0 = plays from the start) |
| `0x3E` | two bytes `0x69 0x69` (as retail) |
| `0x40` | 4 × 4 floats: **world matrix**; the scene nodes are relative to it |
| `0x80` / `0x90` | box the animation moves in: min xyz, then max xyz. It decides when the scene is in view |

**Time.** Scenes run at 30 frames per second times the time scale. Retail jets
have 0.02 (23 seconds of animation take about 19 minutes). The Underground 1
tram, with 690 frames, runs at scale 1.0 at about 63 km/h.

**Order matters** [game]:

- The game creates its animation pools when the **first `0x37090` or
  `0x37150`** loads. A `0x37110` controller loaded before that crashes at
  `0x440D43`.
- Retail order in the file: all `0x37090`, then all `0x37150`, then the banks
  and the scenes.
- A scene with a hidden model (instance flag `0x10`) is shown by its
  animation; the planes work like that.

**Not known yet:** a bank has never been authored from scratch (only
Underground 1's converted and retail's reused by name), so a new moving object
needs channels in that ELF format, which is not documented. A scene moves the
picture only: Underground 1's trams had collision, Underground 2's trains have
none, and a converted tram has none either (never tested with a car).

### Porting Underground 1's moving objects [files, game]

The tram `XOu_TrolleyA_01` and the plane `XO_PlaneA_1b_01` on Olympic are
*entities* in the track bundle, chunk `0x3B300`: a 4 × 4 matrix, with the type
and the name in the 0x10 bytes before it and the model (3 or 4 levels of
detail) after it. The entity's name is the base name of its animation in
`GLOBAL\INGAMEB.BUN`.

- Recipe: put the bank from `INGAMEB.BUN` into the region as `0xE34010`
  (converted, below), write the scene (a root node, a child node with the model
  and mask 7, a controller), and one placement at the entity's matrix, with the
  box taken from the model's size.
- Entities that do not move (parked trams, the hotel's turnstile, a truck)
  become ordinary scenery in a new pack (2499 in X99 was used). A moving tram
  can stay parked at the start of its route when animations are switched off.
- Crowds and flag girls live in the NIS sections and are not world animations.

**Underground 1 banks do not load as they are** [code, game]: the bank header
is EAGL toolkit 2 in Underground 1 and 4 in Underground 2.

| | Underground 1 | Underground 2 |
| --- | --- | --- |
| Header at the symbol that ends with `bank` | `[0x18, count, 0, channels, names]` | `[0x1C, count, 0, 0, channels, names]` |
| Version string in the strtab | `EAGL_TOOLLIB_VERSION-2` | `EAGL_TOOLLIB_VERSION-4` |

The room for the extra word is there, so the conversion edits the bank in
place: rewrite the header; add 4 to the two relocations that point at `bank +
0x0C` and `bank + 0x10`; change the version string. Loaded unconverted, the
game reads a null name table and crashes at `0x434893`. A bank found in both
games differs only in these three places. The 2003 PS2 demo of Underground 1
has banks that crash the game at load (`0x4317A3`, `code/anim`).

## 5f. Light flares

Flare records are 0x30 bytes [code]:

| Offset | Meaning |
| --- | --- |
| `0x00` | `u32` 11, `u32` 11 (list links) |
| `0x08` | id |
| `0x0C` | colour `FFFFFF00` |
| `0x10` | position |
| `0x1C` | mirror height |
| `0x20` | direction |
| `0x2C` | type (retail 9) |
| `0x2D` | flags (retail 2) |
| `0x2E` | section |

`eRenderLightFlare` `0x490920` draws the reflection on the road at x, y and the
height at `+0x1C`: 999 means "compute once", `2 × floor - z` with the floor
from `GetElevation`; 9999 means no reflection (no floor under the flare, or the
flare sits on it); any other value is used as it is. Retail flares hang 5..15
m over the road; higher ones (Underground 1 neons up to 40 m) mirror far off
the road and want 9999.

The editor (`io/LightFlares.cpp`) gives a new flare 999 only when a floor lies
0.3-15 m under it, and 9999 otherwise.

## 5g. Sky

- `sub_60FAA0` looks up `SKYDOME` (`0xBE43EDBB`) with `eFindSolid` **once, on
  the first in-game frame** [code, game]. It must be in a section loaded at
  that moment (retail places it from Z0), and the solid index `0x134003` must
  be sorted, because the lookup is a binary search (`0x48DF90`).
- Its textures are in `TRACKS\LOC4DYNTEX.BIN`. The game swaps the picture pairs
  by hash every 300 s (eight hashes at `0x8A07A8`: sunrise, night, sunset,
  night).

## 6. Open questions

- ~~What the unknown collision flag bits (`0x01 0x08 0x40`) select.~~
  Answered: compare heights, not ground, never cached (§5a, *Flags*).
- The meaning of the preculler masks in `0x00034106` (an all-zero block is
  known to be safe, §5).

- Palette location for the 24 `D3DFMT_P8` textures.
- ~~Which materials want alpha blending rather than the alpha test.~~ Answered:
  it lives in the texture header, `+0x55` and `+0x56` (see below).
- Meaning of the second and third size fields in the section table for sections
  that carry a texture pack.
- The remaining fields of the object header between `0x80` and `0xA4`.

## Pre-placed meshes

A solid object's vertices are not always local to the object. Terrain -- road
and ground chunks, everything named `TRN_*` -- is authored already standing in
the world: a chunk's vertices are world coordinates and its header bounding box
sits wherever the piece belongs, a thousand units or more from the origin.
Props, buildings and trees are the opposite: their boxes straddle their own
origin.

This matters because scenery instances reference both kinds through the same
field. Applying an instance's translation to a mesh that already carries a
position adds the two together and throws the piece far from where it belongs.
In L4RA that was most of the terrain, with a median error of about 1,500 units
-- the city filled with real geometry in wrong places while the right pieces
were missing from the streets.

The two populations separate cleanly by measuring the header box against
itself: a mesh is pre-placed when the distance from the origin to its box
centre is greater than the box's own diagonal. In L4RA the terrain box centres
are a median 1,293 units out and the props' 7.3, so nothing sits near the
boundary. A pre-placed mesh has to be brought back to its own origin before the
instance transform is applied.

The game itself does not measure anything: the switch is instance flag
`0x0100`, which makes it draw the mesh without the instance matrix (§5,
*Instance flags*). The box test above is a heuristic standing in for that flag.

The check that catches this is in `WorldRenderer::CheckReferences`: rebuild each
instance's bounding box from the model it references and compare with the box
the file stores for that instance. Note that only the **centre** is worth
judging -- the file's box is tighter than one built by enclosing a rotated
model's transformed corners, so a size ratio above one is normal and says
nothing. Comparing the stored box against a copy of itself, which an earlier
version of this check did, proves nothing at all.

Median distance from the stored box, over L4RA's 48,074 instances: 198 units
before the fix, 3.6 after.

## Race routes and track maps

`TRACKS/ROUTESL4RA/Paths<id>.bin` is one `0x80034147` container per race. It holds
the free-roam road graph (`0x3414A`, `0x3414D`, identical to `PathsFreeRoam.bin`)
plus three race-specific chunks: `0x34148`, `0x34149` and `0x3414C`.

These three decide whether a race has a course, laps and split times.

### `0x34148` — path points, 24 bytes [code]

| Offset | Type | Meaning |
|---|---|---|
| 0x00 | f32 x2 | world X, Y |
| 0x08 | u16 | spline index (points of one spline are listed in order) |
| 0x0A | u8 | **near**, in half metres |
| 0x0B | u8 | **far**, in half metres |
| 0x0C | i16 x3 | **linked points**: indices into this same point table (-1 = none) |
| 0x12 | u16 | 0 |
| 0x14 | f32 | course distance (rewritten at race start) |

Earlier this table read `0x0A` as unknown and `0x0C` as links into the road
graph; they are indices into the point table itself, not into the road graph.
Some splines are side roads that the race does not use, so they are not part
of the drawn route.

`sub_5D9E30` updates a car's course position each frame. The search stays on
the spline of the last point and moves one point per update; a jump of more
than 128 m does a full search of the course. With `d` = twice the distance
from the path: `near <= d < far` tries the linked points; `d >= far` searches
the course within 128 m; `near = 255` does neither, and such a point can never
be taken. A candidate wins with `distance + 1 + max(|course distance
difference| - 40, 0) * 0.25` below the present distance.

**The links are the only way over a joint between splines.** Retail links the
last three points of a spline to the first point of the next one. Without
links a car stays on its first spline for good: on a loop it counts a lap at
the line, slides back along the spline and loses the lap again [game].

### `0x34149` — splines, 0xDC bytes [code, files]

| Offset | Meaning |
|---|---|
| 0x00 | name[16] |
| 0x10 | bStringHash(name), index |
| 0x1C, 0x1E, 0x20 | u16 first point, end, count |
| 0x22, 0x23 | u8 link counts |
| 0x24 / 0x44 | links at the first / last stored point: u16 spline, u16 which end (0 first, 1 last), u32 runtime pointer |
| 0x64 | f32 length |
| 0x68 | i32 1 |
| 0x6C | f32 length |
| 0x70 | f32 parameter origin |
| 0x74 / 0x78 | f32 course distance of the first / last stored point |
| 0x7C | f32 -1 (points are stored against the course direction in every retail file) |
| 0x80 / 0x84 | distance interval |
| 0x88 / 0x90 | f32 first / last xy |
| 0x98 | f32 bbox |
| 0xA8 | runtime spline |
| 0xAC | u8 count of tracks the spline is only for |
| 0xAD | u8 count of tracks it is not for |
| 0xAE | u16 track numbers (`sub_5D4F60`; both counts 0 = every track) |

### `0x3414C` — courses, 0x110 bytes, one record per race [code]

| Offset | Meaning |
|---|---|
| 0x00 | u16 track |
| 0x02 | i8 number of waypoints |
| 0x03 | u8 loop |
| 0x04 | u8 branches |
| 0x05 | u8 course width in metres (retail usually 30) |
| 0x06 | 2 bytes, zero |
| 0x08 | up to 33 × (f32 x, y) waypoints |

Every retail `Paths<n>.bin` holds the 60 records of its region.

How the course is built at race start:

1. `sub_57A650` calls `sub_5E6A40(track, reverse)` for every race type but 0.
2. That finds the record (`sub_5D5390`) and builds the course (`sub_5E5BB0`):
   the point nearest each waypoint picks its spline; the chord of the first two
   waypoints against the stored points picks the direction; `sub_5D53D0` walks
   the links between waypoints, to depth 10.
3. `sub_5E48B0` rewrites the course distance of each course spline and its
   points: 0 is where the first spline is entered, and the length is the sum
   of the course splines. The distances in the file do not matter.

**Without a record** the course is empty: every percent is 0, no boundary is
crossed, so no lap or split counts, and a FastTest / SkipFE race turns into a
one-lap point to point (`dword_89E7B4 = !loop`, laps 1). A spline whose
include list leaves the track out gives `sub_5E5BB0` no way over it, and it
gives up; retail `Paths4021.bin` has such a spline.

### Laps and split times [code, game]

`Race::FigureOutBoundaries` `0x5EE620` and `AddBoundaryPosition` `0x5EE4F0`
build the boundaries:

| Boundary | Where |
|---|---|
| start | the `ForwardStartLine` marker, default 5 % of the path |
| splits | `SplitTimeBoundary1..3`, or start + 33 % and start + 66 %; a third split only with its marker |
| finish | `ForwardFinishLine`, used only for point to point races |

Each marker is projected onto the course as a percent. The race object is at
`*0x890118`: `+0xBF4` count, `+0xBF6` start index, `+0xBF7` finish index,
`+0xBF8` f32 percents.

`RacingCar::UpdateLapCounts` `0x5F7C30`: a forward crossing of the finish
boundary is lap + 1, a backward crossing lap - 1; boundaries further than 25 %
from the car are skipped; racers start at lap -1, behind the line, and the
first crossing makes it 0. A start line placed exactly on the path's 100 → 0
jump is never crossed cleanly, so start the path somewhere else.

### Start grid markers [code, files]

Names are bStringHash values of exe strings:

| Hash | Name |
|---|---|
| `8E55C895` | ForwardStartingPosition |
| `6A56B90A` | ForwardStartLine |
| `8F90F7BF` | BackwardStartingPosition |
| `EEC70374` | BackwardStartLine |

Also used: ForwardFinishLine, BackwardFinishLine, SplitTimeBoundary1..3.

- `sub_5EB5D0` places the cars. The marker index is the grid slot; the
  reverse flag (`0x89E7A4`) picks Forward or Backward.
- With too few markers, cars go along the course path.
- A sprint without `ForwardFinishLine` ends at start line + 95 % of the path,
  which can lie behind the start.

**The end of a race** [game]: a few seconds after the player crosses the
finish, the game moves the car to another place for the race end. In logs this
looks like a reset or teleport through barriers; it is not a collision fault.

### Track maps

`TRACKS/TRACKMAP<id>.BIN` is a texture pack holding one 512x512 DXT3 minimap per
race (named `TRACKMAP<n>`, 7 mips, alpha 0; the HUD masks it). Every L4RA city
map uses the same framing:
`px = 0.0917 * x + 312`, `py = -0.0917 * y + 298.5`.
Drag and closed tracks (L4RB..G) use their own zoom. `tools/race_map.py` builds
`races-map-L4RA.png` from these.

The map's placement in the world comes only from the TrackInfo calibration
fields (see *New tracks and regions*), so any framing works. Retail frames one
map of the whole area for every race in it and lets the menu window and ZoomIn
zoom; a map cropped to the course enlarges the picture and its line.

## Race start triggers

The career chunk `0x80034A10` in `GLOBAL/GLOBALB.BUN` (the first one is the full
career) holds a string table `0x34A1D` and the race list `0x34A11`, 136 bytes per
race. In that list, u32 0 is the name offset into the string table
(`S3_SPRINT_6`), the low u16 of u32 6 is the track id (`4164`), and u32 10 is the
bin hash (`h = h*33 + c`, seed `0xFFFFFFFF`) of the trigger name (`EV_14`,
`EVENT_LOCATOR_02`).

Each trigger is a zone in the free-roam road graph (`0x3414A` in
`PathsFreeRoam.bin`), with the hash as the record anchor at offset `h`:
`h-0x30` u32 kind (`0x0D`), `h-0x2C` f32 x, y (the centre, where the start icon
sits), `h-0x20` f32 radius, `h-0x10` f32 bbox min x, min y, max x, max y. An 8x8
square polygon follows the hash. `tools/race_events.py` lists them all.
222 of the 224 races resolve to 116 points. The two `DDAY` intro races have none.

## Free-roam zones: `0x3414A` in `PathsFreeRoam.bin`

The free-roam road graph `TRACKS/ROUTES<region>/PathsFreeRoam.bin` holds the
region's zones in chunk `0x3414A`: shops, race triggers and the city's named
neighbourhoods. `src/nfs/FreeRoam.*` reads them.

`0x3414A` is a run of variable-length records that fill the chunk exactly (671
in L4RA, checked by walking it end to end):

| Offset | Type | Meaning |
|---|---|---|
| 0x00 | u32 | zone type: `0x0D` race trigger (196), `0x0E` shop (23), 12 others |
| 0x04 | f32 x2 | centre X, Y -- where the beacon stands. No Z: the zones are flat |
| 0x0C | f32 x2 | facing, a unit vector in plan |
| 0x14 | f32 | height (z): follows the ground, 10-61, while the footprints stay one size (the editor still reads it as a radius) |
| 0x18 | u32 | 1 on every shop, 0 on race triggers |
| 0x1C | u32 | 0 |
| 0x20 | f32 x4 | plan box of the polygon: min x, min y, max x, max y |
| 0x30 | u32 | hash the career data names the zone by |
| 0x34 | i32 x3 | the **GPS target**, x, y, z in whole units (`0x529410`/`0x529480` route to it): the centre on race triggers, a point on the street 13-115 units away on shops. Off the road, the GPS finds no route |
| 0x40 | u16 | polygon corner count |
| 0x42 | u16 | record size, `0x44 + 8 * count` |
| 0x44 | f32 x2 per corner | the footprint polygon |

Type `0x11` zones are the city's named neighbourhoods (Pigeon Park, Coal
Harbour East, Jackson Heights...): thirteen in L4RA. The hash at `0x30` is the
bin hash of the name as the game spells it (`JACKSON_HEIGHTS`); the names
themselves are nowhere in the file. `src/nfs/CityZones.*` matches them and
groups them into the five districts, which is the editor's own grouping.

### Zone types and what reads them [code]

The type names are `TrackPathZoneTypeNames` `0x800D80`:

| Type | Name | Type | Name |
|---|---|---|---|
| 0 | NO_WRONG_WAY | 11 | SHORTCUT |
| 1 | WRONG_WAY | 12 | STREAMER_PREDICTION |
| 2 | RESET | 13 (`0x0D`) | RACE_START_TRIGGER |
| 3 | RESET_TO_POINT | 14 (`0x0E`) | SHOP_TRIGGER |
| 4 | GUIDED_RESET | 15 | SHOWCASE_TRIGGER |
| 5 | TUNNEL | 16 | TRAFFIC_TYPE |
| 6 | OVERPASS | 17 (`0x11`) | NEIGHBOURHOOD |
| 7 | OVERPASS_SMALL | 18 | SMS_TRIGGER |
| 8 | DRIFT_GATE | 19 | BANK_TRIGGER |
| 9 | DRIFT_HOT_SPOT | 20 | GARAGE |
| 10 | TUTORIAL | | |

- `Player::DetectWrongWay` and `AICarController::Think` look for zones 2 and 3
  at the car and call `sub_5FBE00`. A **RESET** zone sets the reset flag
  (Car `+0x96C`) when its height (`+0x14`) is 0, or when the car is connected
  and under that height. **RESET_TO_POINT** puts the car on the zone's point,
  facing `+0x0C`.
- `Player::HandleReset` places a reset car using a **GUIDED_RESET** zone
  around it.
- `TrackPathManager::FindZone` `0x5D9BF0` returns the first zone of the type
  that holds the point, without regard to direction.
- TUNNEL and OVERPASS hold for a car below the zone height: rain stops there,
  and the sound asks with masks 32, 64, 128 and `0x100000`.
- NEIGHBOURHOOD zones (type 17) are read only in career free roam (race
  type 0).
- TRAFFIC_TYPE zones (type 16) are required for traffic to keep coming; see
  `research/road-paths.md`.

## Solved: the alpha test ate the asphalt

Track surfaces come out see-through, with only their lane markings left. The
cause is the editor alpha-testing every material at `ALPHAREF 128` while the
alpha channel of many track textures is not opacity at all.

Measured in L4RA:

- `RDP_LANEB_WD_AA_KT` (DXT3, used by **713** material slots) has only **5.5%**
  of its texels at alpha >= 128. Testing against it removes 94.5% of the road.
- `ARC_CONCRETEA_VK` 2.9% (84 slots), `ARC_COURTGD_04` 5.3% (229 slots),
  `SGN_CLOCKGLOW` 0.0% (13 slots).
- Genuine cut-outs sit at the other end: `SGN_NEON_OUTLINE` and
  `SGN_OPEN_24_HOURS_NEON` pass 100%.
- The P8 palette's fourth byte is already known to be gloss rather than opacity;
  the low DXT3 alpha looks like the same channel in another format.

What is NOT the answer: a "the alpha never passes anywhere" rule. It rescues
exactly one texture in the region (`SGN_CLOCKGLOW`) and leaves the road alone.
That rule is implemented in `TextureCache::Create` via `AlphaNeverPasses` and is
harmless, but it is not the fix.

The promising lead is the material flags field at +0x20 of the 60-byte material
entry, which is what the game would switch on. A first pass over 1,509 materials
found the value 255 everywhere, so either the field is somewhere else in the
entry or the switch is not per material. Worth resolving before inventing a
threshold on "fraction of texels that pass", which would be a guess.

**Resolution.** The switch is per texture, not per material: `+0x55` of the
124-byte texture header is the alpha usage (0 none, 1 punch-through, 2
modulated) and `+0x56` the blend (0 copy, 1 blend, 2 additive). Over the 1 821
textures of the eight track streams both only hold 0..2. `RDP_LANEB_WD_AA_KT`
is 2/0: its alpha is not opacity and it is drawn solid. Trees and chain-link
fences are 1/0 (alpha test), smoke and the race barriers' tape 2/1 (blended),
every neon sign, flare and light beam 2/2 (additive, black is invisible). The
editor draws 0/x and 2/0 opaque, 1/0 alpha tested, and 1 and 2 in a second pass
without depth writes, farthest first. `AlphaNeverPasses` is gone.

The race barriers (`XO_TRACKBARRIER*`) and a few effect sprites draw from packs
the game keeps loaded for every track: `GLOBAL\InGameCommon.bun` (which holds
`OBJ_TRACKBARRIERB_*`, `SFX_NEONBAR*`, `SFX_SMOKE_*`, `SFX_LIGHT_BEAM*`) and
`GLOBAL\GLOBALB.BUN`. The editor reads them as a display-only fallback.

A measurement trap to avoid: `tools/dump_textures.py` does
not decode DXT3 alpha, so any alpha figure taken from it reads as fully
transparent. Read the nibbles out of the block bytes instead.

## Adding a texture to a section — what the game insists on

Everything here was established against `L4RA` section `Q38` by changing one
thing at a time and watching the game, because the editor accepts several
layouts the game does not. Each rule below is one the game enforced by crashing
or by drawing nothing.

**The object header carries its own texture count.** One byte at `+0x19` of
`0x00134011` holds the number of entries in the object's `0x00134012` list. The
list's length is already implied by that chunk's size, so the field looks
redundant — but it is what the game sizes the object's texture array from.
Leave it behind when the list grows and a material referring to the new entry
indexes one past the end; the game dies on the spot. Confirmed equal to the
entry count in 155 of 155 objects in the section.

**The pack's three tables are searched, not scanned.** In every shipped pack the
texture headers run in ascending order of hash, with the hash table exactly
parallel to them. Appending a new texture at the end and leaving the order alone
does not crash — the game simply draws nothing for that texture, which is what a
binary search that cannot find a hash looks like. Headers, format entries and
hash table have to be sorted together.

**Those tables carry no filler.** Their payloads sit at offsets that are not
multiples of sixteen (84, 116, 112 in this pack), so the game reads them from the
first byte after the chunk header. Giving them the default sixteen-byte
alignment puts `0x11` bytes where the first texture header should be.

**Texture pixels are laid end to end.** The second texture in this pack ends at
131 104 and the third begins there; 131 104 is not a multiple of 128. Only the
pixel block itself is aligned, and it is aligned by its absolute position in the
file.

**The pack must not move what follows it.** It sits near the front of a section,
so growing it shifts every object behind it — and those objects are copied
verbatim, filler included, with filler sized for where they used to be. A vertex
chunk is aligned by absolute file offset, so the pack has to absorb the
remainder itself and grow by a whole number of 128-byte units.

**The table's second size field excludes pixel data only.** `streamSize` less
the payload of every `0xB3320000` block: 1 251 728 − 401 584 = 850 144, exactly
what the shipped table holds. The pack's own tables count as geometry.

### Fields in a 124-byte texture header

| Offset | Meaning | How it was established |
| --- | --- | --- |
| `+0x24` | name hash | matches `h = 0xFFFFFFFF; h = h*33 + c` |
| `+0x28` | a category, not the texture | two of three textures share a value |
| `+0x48/49` | `log2` of width and height | holds for all 26 textures |
| `+0x4A` | `0x22` for DXT1, `0x24` for DXT3 | tracks format, not size |
| `+0x4E` | mip count, stopping at eight pixels a side | holds for all 26 |
| `+0x55` | alpha usage: 0 none, 1 punch-through, 2 modulated | 1 on the two cut-out textures, 0 on the opaque ones |
| `+0x56` | alpha blend: 0 copy, 1 blend, 2 additive | 2 on every neon and flare, 1 on smoke and barrier tape |

Everything else is copied from a texture of the same format that the game
already loads. Those are values the game accepts for a texture of this kind,
which is more than can be said for anything invented.

## New tracks and regions (GlobalB)

The editor puts edits into existing regions. Adding a **new** track or region
(a separate world, or a port) takes the following [code, game].

### Regions

- **Region names are free.** The exe builds every path from TrackInfo
  `RegionName` (`+0x40`) with `%s`. Only `L4RG` is special:
  `RegionLoader::FinishedLoading` `0x57A580` picks pool index 2 (36000 KB
  instead of 60000) by that name. Regions such as L4GA..L4HF and L4JA run.
- Set `RegionDirectory` (`+0x48`) to a unique `Location4\Region<x>` per region.

### TrackInfo — chunk `0x34201`, 0x128 bytes per track

Loaded at `0x5D3EB0`. Useful fields:

| Offset | Field |
|---|---|
| 0x00 | RaceDescription[32] |
| 0x20 | TrackDirectory[32] |
| 0x40 | RegionName[8] |
| 0x48 | RegionDirectory[32] |
| 0x84 | IsValid |
| 0x85 | IsNotLoopingRace |
| 0x86 | ReverseVersionExists |
| 0x8A | TrackNumber |
| 0x8C | SameAs |
| 0x90 | SunInfoHash |
| 0x94 | RaceGameplayMode |
| 0x98 | RaceLength |
| 0xAC, 0xB0 | TrackMapCalibrationOffsetX, Y (world point at the map's top left) |
| 0xB4 | TrackMapCalibrationWidth (metres across) |
| 0xB8 | TrackMapCalibrationRotation (u16, 32768 = 180°) |
| 0xBA / 0xBC | start grid / finish line angle on the map |
| 0xC0 | TrackMapCalibrationZoomIn (HUD zoom) |
| 0xEC | MaxTrafficCars[8] |
| 0xF8.. | traffic distances |
| 0x108 | TrafOncomingFraction[4] |
| 0x118, 0x11C | MenuMapZoomOffsetX, Y |
| 0x120 | MenuMapZoomWidth (pixels of the map; retail 4001: 168, 194, 270) |
| 0x124 | MenuMapStartZoomed |

`RaceGameplayMode` values: `0x10` free roam, `0x71` circuit, `0x34` sprint, 2
drag, 8 drift, `0x800` Street X, `0x2010` URL.

Quick Race lists a track by mode bits (`UIQRTrackSelect::IsAvailable`
`0x4987C0`): Circuit 1, Sprint 4, Drag 2, Drift 8, Outrun and Free Run `0x10`,
Street X `0x800`, URL `0x2000`. A track that no career event owns stays locked
unless `UnlockAllThings` is set (`UnlockSystem::IsTrackUnlocked` `0x5156F0`).

### GlobalB pitfalls [game]

- **Keep the solid list at the end of GlobalB (`0x80134000`) at its offset
  modulo 0x80.** Adding 0x128-byte records without re-padding shifted it, and
  every race start then crashed at `0x493D5B` (solid loader).
- **Language strings are found by binary search, so keep them sorted by
  hash.** Track names are `TRACK_NAME_<n>` and `REVERSE_TRACK_NAME_<n>`.
- **Start markers carry the track number at `+0x24`.** Without markers of the
  new number, `InitCars` crashed at `0x5DA392`.
- A new track also needs its route files; see `research/road-paths.md`
  (which route file each mode reads).

### How the editor makes a new region

File > New Region (`ui/RegionTools`, `io/RegionCreate`, `nfs/CustomRegions`)
avoids every pitfall above by changing no game file and no GlobalB byte:

- **A region is an alias.** `<game>\models\regions\<R>.frsregion` names the
  new region (the first free `L4R<letter>`, never `L4RG`), the shipped region
  it copies, its track number (from 4950, past the shipped tracks and the race
  editor's), its name and its whole TrackInfo entry. The loader
  (`FileEdits.cpp`, `LoadRegionAliases`) answers `TRACKS\<R>.BUN`,
  `STREAM<R>.BUN` and every `ROUTES<R>\*` with the source region's files; the
  handle keeps the name the game asked for, so overlays written in R (their
  header names R) apply to R only. Files of R's own in `models\files` win over
  the alias. Nothing is copied: a new region costs a few hundred kilobytes.
- **The track is added at run time** (`loader/src/Tracks.cpp`, shared with the
  race editor's new races): the TrackInfo table is copied into a larger one at
  the end of the chunk loader (`0x5D3EDE`; table `0x883DA0`, count `0x883DA4`),
  `IsTrackUnlockedForQuickRace` (`0x5143C0`, track = first argument) answers 1
  for it, and `SearchForString` (`0x4FF9D0`) answers `TRACK_NAME_<n>`,
  `REVERSE_TRACK_NAME_<n>` and `QR_TRACK_<n>_COMMENT`. No language file and no
  GlobalB chunk changes size.
- **The entry** is the source's free-roam track's (its lowest number: 4000 for
  L4RA, with the whole-city map calibration) with the new number and `SameAs`,
  `RegionName` R, `RegionDirectory` `Location4\Region<letter>`, `TrackDirectory`
  `Location4\Track_<n>`, `ReverseVersionExists` 0 and mode `0x10`, so Quick
  Race lists it under Free Run only.
- **Its per-track files** go to `models\files\TRACKS` and are rewritten
  whenever the editor saves or launches the game, from the region's free-roam
  files as they stand (the baked roads and placed shops included):
  `ROUTES<R>\Routes<n>F.bin` and `B` = `RoutesFreeRoam.bin` (Free Run is a race
  of track n), `Paths<n>.bin` = `PathsFreeRoam.bin` (its zones, TRAFFIC_TYPE
  among them; no course record), `TrackPosMarkersAll.bin` = the source's plus
  the free-roam track's start grid (8 `ForwardStartingPosition` and the
  `ForwardStartLine`) renumbered n, and `TRACKMAP<n>.BIN` = the free-roam
  track's map with its texture renamed (name and bin hash, same-length digits).
- **Launch Game** in a new region writes FRSFastTest's dev start (`Mode=dev`,
  `DevTrack=<n>`, `DevRaceType=0`) for that launch only.
- **Remove This Region** deletes the `.frsregion`, `models\files\TRACKS\ROUTES<R>`,
  `<R>.BUN`, `TRACKMAP<n>.BIN` and the region's own edit files in `models`
  (`<R>-*`, `*-<R>.*`).

**Empty regions** (`kind empty` in the `.frsregion`, made with "Empty, with
ground made for it"): the same alias, with none of the source's city.

- The editor hides every shipped object of the positioned sections (all but
  the X, Y, Z libraries) by listing it in `LoadedSection::removedNatives`, so
  any section it writes goes out with those records zeroed; their shipped
  collision starts empty, and a written section goes out with every pack as
  the editor holds it. A section an import rebuilt from its bytes (the bake,
  a quick edit) that still holds city objects or a pack as shipped is marked
  `emptyPending` as it loads and written again at once.
- The region file's zone lists (`0x34153`) are cut in place to the library
  sections of each shipped list and the sections the editor wrote
  (`<R>-<SECTION>-stream.frssection`): the game never streams the city in.
- Its TrackInfo has `MaxTrafficCars` (`+0xEC`) zeroed; the source's road graph
  stays only so a reset car has somewhere to go. The game's resets read
  [code]: no topology at the car or 1 m to its side (`0x604928`, every 32
  frames), more than 5000 m from the track path (`0x60496D`, off when
  `0x81CB88` is set), a RESET zone (`0x5FBEA8`), and on tracks 4400-4404 and
  4709 only, 30 m from the path (`0x5EAEC0`). None is a distance to the road
  graph: a car on ground with collision is not reset for being off the roads.
- Its ground (`terrain/Procedural`): a square of noise terrain round the start
  grid, cut to the visibility zones, with a flat 80 m start, a 45 m rim and
  auto rock, written as terrain patches (`terrain-<R>.txt`, with
  `terraincollision 1` in `roads-<R>.txt`) and baked like any terrain. The
  bake takes no light from the hidden city (`SetGameLight`); the patches carry
  a neutral grey ambient (0x707070).

Not yet tried in the game: whether a free-roam start (race type 0) on a track
other than 4000 keeps the region's track, and whether Free Run without a
course record behaves (FastTest/SkipFE turns such a race into a one-lap point
to point; Free Run has no finish).

## Testing in the game

- **FRSFastTest / FRSFastBoot: `DevRaceType` 1 is a race and 0 is free roam.**
  The ini comment ("0 circuit, 1 sprint") is wrong: circuit or sprint comes
  from the course record's loop flag (`0x3414C`, see *Race routes and track
  maps*).
- **SkipFE globals** a test plugin can write:

  | Address | What |
  |---|---|
  | `0x864FB0` | number of AI racers (`SkipFENumAICars`) |
  | `0x864FBC` | traffic density: 0 in dev mode; copied to `QRSelectedTrafficDensity` `0x89E7A8` |
  | `0x864FB4` | reverse |
  | `0x7FAA28` | race type |

- **F6** (the game's own autopilot) drives the player along the routes, good
  enough for unattended race runs. On a track with no routes it crashes at
  `0x41C113` (an AI controller without route state). With a race start scene,
  the F6 press must wait for race state 4, or it is lost.
- **Useful car fields:** `+0x60` position, `+0x70` velocity, `+0x5C4`
  TopologyCoordinate, `+0x96C` reset flag.
- **Logs worth having:** a launch detector (vertical speed jumps by more than
  about 4 m/s in one frame) with the polygons under the car; a reset or
  teleport detector (a position jump); per-car lap, checkpoint and finish
  values from the race object.

## Need for Speed Underground (the first game)

The editor reads Underground 1 regions too (read-only). Its chunks are
Underground 2's with these differences:

| | Underground 1 | Underground 2 |
|---|---|---|
| Layout file | none per region: one bundle per track, `TRACKS\TRACKB<n>.LZC` (JDLZ), over the region's shared `STREAM<region>.BUN` | `<region>.BUN` |
| Which bundle | the one whose `0x00034108` {"--" size, region stream size, track part size} names this stream's size | -- |
| Section table | `0x00034107` in the bundle, 56 bytes: tag[8], number (bit 16 = left out by this track), 0, type (0 "--", 1 region stream, 2 the track's own `STREAM<region>_<n>.BUN`), offset, size, size without texture data, byte sum, 5 x 0. No position | `0x00034110`, 80 bytes |
| Visibility | same container `0x80034150`, children one id lower: `0x00034151` outlines (marker, u16 id, u16 count, up to 16 points, no box), `0x00034152` lists (as Underground 2's `0x00034153`). Zone id = section number | `0x00034152` / `0x00034153` |
| Collision | one pack for the whole track in the bundle: `0x00034132` polygons (same 32-byte records; byte 0 is a query flag, 0), `0x00034130` quadtree, `0x00034131` cells, `0x00034133` indices, `0x00034134` bounds | a `0x80034130` pack per section |
| SceneryInfo `0x00034102` | 72 bytes: u32 hash[6] (detail levels), i16 thresholds at `+0x18`, radius at `+0x44`; no name | 68 bytes with name |
| Scenery instance `0x00034103` | 48 bytes: i16 bounds min/max (whole units), u16 info `+0x0C`, flags `+0x0E`, float position `+0x10`, i16 rotation[9] `+0x1C` | 64 bytes |
| Solid header `0x00134011` | version `0x13` in the low word (crowd: `0x400013`), name at `+0x94` | `0x16`, name at `+0xA4` |
| Vertices | 24 (pos, colour, uv); the crowd's skinned people 60 (36-byte layout + bone weights) | 24 / 36 |
| Textures | also D3DFMT_A8R8G8B8 (format code 21) for the sky in `TEX<n>TRACK.BIN` | DXT, P8 |

The editor gives each drivable section the centre of its zone as a position
(a detail section `<letter>11..19` takes `<letter>1..9`'s), and splits the
track's collision by the zone that holds each polygon.

The rest of this section comes from converting all of Underground 1's regions
to Underground 2 [files, game]. It matters once Underground 1 is written, not
only read.

### Collision

- **Collision exists only on the race routes.** Off-route ground (parks,
  plazas, the skate park) has none; a port generated it from upward scenery
  triangles reachable from existing floors.
- **One race's collision closes side streets that another race uses.** Walls
  with drivable floor of another race on both sides are race barriers, not
  city walls.
- **Drift courses** are a `ROAD_DRIFT` (class 3) band 3-4 cm over the floor of
  the lot. In Underground 2 that made cars hop between the two (§5a, *Flags*);
  laying the band on the floor fixed it.
- **The 14th & Vine drag strip** has floors with byte 3 `0x08` seven metres
  over the road. Underground 2 reads `0x08` as not ground, so they do not
  carry the car.
- **All walls are `0x16`**, real-height ones included. Clear `0x04` on walls
  under about 200 m (§5a).

### Sections and scenery

- **The data of a drivable section is in that section itself** (A1 = 101),
  not in N + 20. Underground 2 needs the collision of N in stream section
  N + 20, or the car freezes at the start.
- **14 drivable sections have no stream data of their own**, like Underground
  2's drivable 101 whose data is in A21.
- **World-space flag:** the instance identity flag is `0x0200` in Underground 1
  and `0x0100` in Underground 2. The other flag values do not carry over
  (Underground 1 `0x40` is not Underground 2 `0x40`), and Underground 1 flag 2
  marks reflection-only copies at the mirror height.
- **Each race has a Z section** (2600 + k: Z1..Z8 = 1001..1008, Z10..Z17 =
  1102..1109) with its barriers and start or finish graphics, named
  `ZPM_<race>_FW/BW_STARTF...`.
- **Drag strips have unnamed packs.** They can be assigned to the races whose
  course passes 90 % of the pack's objects within 40 m. Two drags share a
  street (1201 and 1210, 1206 and 1214), so a pack can belong to both.

### Routes and paths

- **Routes `0x34121`:** a 0x10-byte prefix, then 0x26C-byte headers and
  0x88-byte float points. Racing lines are i16/256 at `+0x4A` (along the
  route) and `+0x4C` (against it, not mirrored). Lanes are 6 × i16/256 at
  `+0x52` and `+0x5E`, signed. Racing speeds are 5 + 5 bytes at `+0x6A`, by
  car class, fastest first.
- **Signposts** are 0x20 bytes without a prefix: `+4` target point, `+6`
  source point, `+9` target route, `+0xA` source route (bytes), `+0xB` racer
  type, `+0xC` traffic type, `+0xD` direction.
- **The race course in TRACKB** is `0x34148` (one 40-byte header holding the
  length) and `0x34149` (20-byte points: x, y, ..., course distance).
- **Zones (0xB4 bytes)** add what Underground 2 dropped: `+0x18` direction (0
  forward, 1 reverse, 255 both), `+0x1C` course distance of the point, `+0x20`
  and `+0x24` the stretch of course the zone covers. Each RESET_TO_POINT comes
  twice with one box, one point per direction. TRACKB1003 has 0 on both its
  zones, so use the direction of the point against the course instead.
- **Race numbers:** 10xx circuits, 11xx sprints, 12xx drag, 13xx drift.

### Fog

Underground 1 keeps fog in TRACKB chunk `0x34250` (`LoaderWeatherMan`
`0x588D10`; every race has a copy): 8 bytes of fill, then 3, 3, version 2,
count, then 232-byte records:

| Offset | Meaning |
|---|---|
| 0x08 | name[64], such as `FOG_MainB <0.8, 0.5, 1.75>01` |
| 0x48 | type (1 = fog) |
| 0x4C | colour |
| 0x50 | start in m |
| 0x58 | x, y, z |
| 0x64 | outer radius |
| 0x68 | strength |
| 0x6C | inner radius |

The fog regions are spheres. `sub_588F40` weighs them by distance to the
camera: full weight inside the inner radius, falling linearly to the outer
radius. The region fully inside with the smallest outer radius wins. Olympic
has 91 such spheres along its roads, and `FOG_Main` of radius 4000 at the
origin as the default. Underground 2 uses parameter maps instead (§5d).

### Particle effects

Underground 1 `0x35021` records have one word less before the numbers:
`+24..+31` are the same flags, Underground 1 `+32..+91` are Underground 2
`+36..+95`, and `+92..+95` are zero. To convert a record, move `+32..+91` to
`+36..+95` and write zero at `+32..+35` (orientation 0, as Underground 1 draws
them) [files: 35 of the 51 emitters both GlobalBs share match byte for byte
this way]. Placed emitters are version 5, 0xC0 bytes; Underground 2 inserts
the 0x10-byte block at `+0x28` (§5c).

Three particle textures do not exist in Underground 2. Swap them, with the
picture grid of the new texture:

| Underground 1 texture (grid) | Underground 2 replacement (grid) |
|---|---|
| `SMOKE_CS` (4 × 4) | `SFX_SMOKE_A_CS` (2 × 2) |
| `FIREA_CS` (4 × 2) | `SFX_FIREB` (2 × 2) |
| `SFX_RJBSPARKS` (4 × 2) | `XE_SPARKSA` (2 × 1) |

Moving objects and their animation banks: §5e.

### Textures and other chunks

- **Textures:** TPK version 4. Scroll speeds (`+0x5A`, `+0x5C`) are 32 times
  finer than Underground 2's (`TRN_WATERFALL_A` `0x7800` against `0x03C0`).
- **Track textures:** each race's own textures are in section Y0 (file type 2)
  of `STREAM<region>_<race>.BUN`. The sky and the old map are in
  `TEX<race>TRACK.BIN`.
- **Course thumbnails:** `Global\DYNTEX.BIN` is a streamed pack. Chunk
  `0x33310003` holds 20 bytes per texture (hash, file offset, packed size,
  0x100, 0), and each picture is its own JDLZ block. `TRACK_<id>_SPLINE` are
  the hand-drawn course thumbnails of the track select screen, too inexact to
  trace a course map from; draw from the routes instead.
- **Lights** `0x80135000` are version 3. **Flares** are 0x50 bytes with a
  name. **Entities** (parked trams, the moving tram, the plane) are in
  `0x3B300`.
- **Surface classes** index the same table in SPEED.EXE and SPEED2.EXE.

### The 2003 PS2 demo (Jampack)

Its files are COMP-packed with older records: section records of 0x34 bytes;
solid names at `+0x90`; textures in TPK version 3 (pixels directly in
`0x33320001`); an older surface class table; lights and flares with the name
first; 72-byte prop collision volume heads; outlines for the scenery sections
too.

## Appendix: Carbon-format data

Found converting Carbon-format data (the W2C mod's Palmont) to Underground 2
[files, game]:

- **Materials:** the solid material `+0x30` is the **vertex format**. There is
  one vertex buffer per format, and the colour and UV offsets differ by
  format:

  | Format | Colour at | UV at |
  |---|---|---|
  | 0, 1, 3, 4 | +24 | +28 |
  | 5, 25 | +20 | +12 |
  | 26 | +12 | +16 |

- **Solid material `+0x34`** (Underground 2 `0x134B02`) is the first index of
  the group in the index buffer, cumulative. Writing 0 made every group draw
  group 0's triangles.
- **Textures:** TPK mips at `+0x32`, tilable UV `+0x33`, alpha usage `+0x39`,
  blend `+0x3A`. Usage 2 with blend 0 is a specular mask: draw it opaque.
- **Instances:** instance flag `0x8` = hidden until a scenery group enables it
  (race barriers, seasonal objects).
- **Collision:** a collision point is the instance position **minus** the
  article point, and world = `(-z, x, -y)` (median error 3 mm against the
  road mesh). Carbon keeps building collision inside the objects, so the
  region's own packs hold only road floors.
- **Strip winding** is random: floors must be flipped up.
