#pragma once

#include "core/Math.h"
#include "core/Types.h"
#include "nfs/ChunkReader.h"

#include <string>
#include <vector>

namespace nfs {

// World collision: what the car actually drives on and bounces off.
//
// It is not the render mesh. Each streaming section carries its own
// 0x80034130 pack of simplified ground polygons and barrier lines, stored in
// 16-bit fixed point and bucketed on a coarse grid so the game can find the
// few polygons under a wheel without searching the section. See
// docs/NFSU2-FORMATS.md for the byte layout.

// One collision primitive.
struct CollisionPolygon {
    // Corners in world space. A triangle repeats its third corner in the
    // fourth slot, so `corners[3]` is always safe to read.
    Vec3 corners[4];
    bool quad    = false;
    // A vertical wall along corners[0] -> corners[2], unbounded in height.
    // Its z values are left at zero; a viewer has to pick its own height.
    bool barrier = false;
    // NFS World: a barrier that carries its own height. corners[0] is one end
    // at the bottom, corners[2] the other end at the top.
    bool wall    = false;
    u8   surface = 0;   // byte 2: index into the game's surface table (SurfaceName)
    u8   raceGroup = 0; // byte 0: race barrier group, 0 = always there (see World)
    u8   flags   = 0;   // byte 3: 0x10 quad, 0x02 vertical, 0x16 barrier
    // Bytes 0 to 3 of the record as stored. These are what the encoder
    // writes; the named fields above are read from them (SyncFromAttributes).
    // Byte 1 is recomputed on every encode.
    u8   attributes[4] = {};
    // NFS World: which polygon of the shipped pack this is, in the order the
    // parser lists them; kNoSource for one the editor made.
    static constexpr u32 kNoSource = 0xFFFFFFFFu;
    u32  source = kNoSource;

    void SyncFromAttributes() {
        raceGroup = attributes[0];
        surface   = attributes[2];
        flags     = attributes[3];
    }
    // A wall with real height: flags 0x02 without the barrier's 0x04.
    bool Vertical() const { return !barrier && (flags & 0x02) != 0; }
};

// The game's surface table (SPEED2.EXE 0x800EE0, 0x98 bytes an entry):
// what byte 2 of a polygon selects -- grip, sound, skid marks, sparks.
constexpr u8 kSurfaceCount     = 29;
constexpr u8 kSurfaceRoad      = 0x01;
constexpr u8 kSurfaceGrass     = 0x0F;
constexpr u8 kSurfaceSidewalk  = 0x10;
constexpr u8 kSurfaceSolidWall = 0x14;
// The name the game gives a surface index ("ROAD", "SOLID_WALL"...), or
// "SURFACE_<n>" past the end of the table.
const char* SurfaceName(u8 surface);

// The collision of one streaming section.
struct CollisionPack {
    u32  sectionNumber = 0;
    u16  gridWidth  = 0;    // cells along x
    u16  gridHeight = 0;    // cells along y
    Vec3 gridOrigin;        // corner of cell (0, 0); cells are 32 units square
    Aabb bounds;            // barriers stretch it to z = +/-127
    std::vector<CollisionPolygon> polygons;
    u32  shippedPolygons = 0;   // NFS World: how many the pack held as parsed

    // The order polygons are listed in within a grid cell, as a rank per
    // polygon (lower first). It is not the file order: the game's tool lists
    // them grouped -- ground, then vertical faces, then barriers, roughly --
    // by some order of its own sources that the file does not record. Every
    // shipped pack is consistent with one global order, which the parser
    // recovers from the cell lists so an encode can reproduce them. Empty
    // means file order.
    std::vector<u32> gridRank;

    // Changed in the editor since it was read: its cells are ordered by the
    // rules below rather than kept as the file had them.
    bool edited = false;

    size_t BarrierCount() const;

    // Height of the ground corner nearest to `point` in plan, or `fallback`
    // when the pack has no ground.
    float GroundHeightNear(const Vec3& point, float fallback) const;
    // The height of every layer of ground under `point` (within `reach` of a
    // polygon's edge in plan): a road and the bridge over it are two.
    void GroundHeightsAt(const Vec3& point, float reach, std::vector<float>& out) const;

    // A barrier drawn as a wall standing on the ground at both ends, from
    // `below` under it to `above` over it. The file gives it no height, so
    // this is a presentation choice shared by the viewport and the exporter.
    // Corners come out as bottom-start, top-start, top-end, bottom-end.
    void BarrierQuad(const CollisionPolygon& barrier, Vec3 out[4]) const;
    static constexpr float kBarrierBelow = 0.5f;
    static constexpr float kBarrierAbove = 3.0f;

    // Settles the height of every barrier at once: barriers run in chains,
    // end to end, so one standing where roads cross takes its level from the
    // neighbours that stand where there is only one ground. BarrierQuad uses
    // the result until the polygons are replaced; the collision view calls
    // this whenever it rebuilds.
    void ResolveBarriers() const;
    // The heights a barrier gets on its own, without its neighbours.
    void BarrierEndsAlone(const CollisionPolygon& barrier, float& za, float& zb) const { BarrierEnds(barrier, za, zb); }

private:
    void BarrierEnds(const CollisionPolygon& barrier, float& za, float& zb) const;
    mutable std::vector<std::pair<float, float>> barrierZ_;   // per polygon, for barriers
    mutable const CollisionPolygon* barrierFor_ = nullptr;
    mutable size_t barrierSize_ = 0;
    static constexpr float kWallShown    = 4.0f;   // drawn height of an NFS World wall
};

// A fingerprint of the polygons, so an import can tell which state of a pack
// an export was taken from.
u32 CollisionFingerprint(const CollisionPack& pack);

// Parses one 0x80034130 chunk.
bool ParseCollision(const Chunk& collisionChunk, CollisionPack& out);

// Decodes a run of 32-byte polygon records (0x00034134; Underground 1 keeps
// the same records for a whole track in its bundle's 0x00034132).
void ParseCollisionPolygons(ByteSpan polygons, std::vector<CollisionPolygon>& out);
// Parses one NFS World pack, chunk 0x0003B801 (see WorldCollision.cpp). Read
// only: `surface` is a byte folded from the surface's hash, `attributes` holds
// the hash itself and `flags` the triangle's or barrier's flag byte.
bool ParseWorldCollision(const Chunk& collisionChunk, CollisionPack& out);

// Takes polygons out of an NFS World pack where it stands: `payload` is the
// chunk's payload, filler included, and `removed` the `source` of each polygon
// to drop. The articles that lose polygons are written again -- a strip cut in
// two keeps its centre, its vertices and its winding -- and the pack is laid
// out again inside the same bytes, so no size changes anywhere. Nothing else
// can be written yet: a moved or new polygon would have to change its
// instance's box, which the map's own index of instances (L5RA.BUN 0x3B800,
// not decoded) was built from.
// False, with `payload` untouched, when the pack is not laid out the way every
// shipped one is or the result does not fit.
bool RemoveFromWorldCollision(u8* payload, size_t size, const std::vector<u32>& removed,
                              std::wstring& error);

// The same with polygons to add, and the pack allowed to grow: `out` is the new
// payload (filler included), longer than `payload` by a multiple of 0x100 or
// the same size. An added polygon goes into the article of the smallest
// instance whose box holds it in plan, as single-triangle strips (or barrier
// records); only that instance's half height grows. The game reaches instances
// through a grid built from their boxes (nfsw.exe 0x6F2570; the grid itself,
// L5RA.BUN 0x3B800, is not decoded), so a polygon outside every box of its
// pack cannot be reached and is not written: `skipped` counts those. Ground no
// single box holds is cut along the boxes it crosses instead, a piece per box;
// `spill` is other packs' ground of that kind, cut into this pack's boxes only.
// `placed`, when given, gets per polygon (added, then spill) 0 = nothing
// written, 1 = pieces, 2 = whole into one box.
// `grown`, when given, gets the instances that got polygons, for the grid.
// `pack` is the pack as loaded, for the height the editor shows its barriers at.
struct GridEntry { u32 key; float x0, x1, z0, z1; };   // pack << 16 | instance; box in collision x and z
bool RewriteWorldCollision(ByteSpan payload, const CollisionPack& pack, const std::vector<u32>& removed,
                           const std::vector<CollisionPolygon>& added, size_t& skipped,
                           std::vector<u8>& out, std::wstring& error,
                           const std::vector<CollisionPolygon>& spill = {}, std::vector<u8>* placed = nullptr,
                           std::vector<GridEntry>* grown = nullptr);

// Ground that lies a little under added ground (a speed bump, cobbles, a ramp
// laid on the road) loses the part under it. A wheel keeps the face it found
// while its point stays inside it in plan (nfsw.exe 0x6dcbc0, per wheel from
// 0x6fc8f0 through 0x6f5f50), so with the road still under a bump it rides the
// road's plane, then the bump's once it searches again: the height jumps by the
// bump's in one step and the shock throws the car, at any speed. With one face
// under each point the wheel follows the bump. True when `ground` was cut:
// `pieces` (triangles, possibly none) then stand for it.
bool CutUnderRaised(const CollisionPolygon& ground, const std::vector<const CollisionPolygon*>& raised,
                    std::vector<CollisionPolygon>& pieces);

// Adds each entry's key to list 0 of every cell of the section's collision grid
// (chunk 0x3B802, `raw` with its filler) that its box touches, making cells
// that are missing. False when nothing changed or the layout is not the shipped
// one; `out` is then left alone.
bool AddToCollisionGrid(ByteSpan raw, const std::vector<GridEntry>& entries, std::vector<u8>& out);

// A check of that writer on a copy of one shipped pack: every `every`-th
// polygon, then a block of neighbours, taken out and read back. The totals
// are logged by LogWorldCollisionSelfTest. Run on the whole map by starting
// the editor with FRS_WORLD_COLLISION_SELFTEST=<every>; nothing is written.
void WorldCollisionSelfTest(const Chunk& chunk, const CollisionPack& shipped, u32 every);
void LogWorldCollisionSelfTest();

// Writes a pack back out as a complete 0x80034130 chunk, header included.
//
// Only the section number and the polygons are taken from `pack`. The bounds,
// the grid and its cell lists are rebuilt from the polygons with the rules the
// game's own tool follows, which reproduce every shipped pack of all eight
// regions byte for byte -- so re-encoding an untouched pack changes nothing,
// and an edited one is indexed the way the game expects.
//
// Ground corners are quantised to 1/8 in plan and 1/256 in height; a triangle
// must repeat its third corner in the fourth slot. Fails when a coordinate or a
// count no longer fits the file's 16-bit fields.
bool EncodeCollision(const CollisionPack& pack, std::vector<u8>& chunk, std::wstring& error);

} // namespace nfs
