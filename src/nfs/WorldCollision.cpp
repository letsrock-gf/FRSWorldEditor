#include "nfs/Collision.h"

#include "core/Log.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <string>
#include <tuple>
#include <vector>

// NFS World collision: chunk 0x0003B801, one pack per collision section, all
// 747 of them in STREAML5RA_25000.BUN. Measured on every pack with
// tools/nfsw/col_check.py and matched against the game's loader
// (nfsw.exe 0x6E5CA0 -> 0x6D5AD0 -> 0x6D00B0).
//
// After the 0x11 alignment filler:
//   +0x00 u32 size, u32 section number, u32 flags (bit 0 set by the game once
//         it resolved the pack), u32 pointer to itself at run time
//   +0x10 16-byte rows {tag, size << 8 | kind, count, offset}; kind bit 1 means
//         the offset is relative to the row. Group 'CARP', group 'Arti' (count
//         = number of data rows), then 'Name' (the section number as text),
//         one 'ca' + u16 index row per article, one 'ci' row with the instances.
//
// Instance, 64 bytes: inverse matrix row 0 + half width, u16 flags, u16 0,
// half height, u16 group, u16 article index, pointer at run time, inverse
// matrix row 2 + half length, inverse position + radius. Every shipped instance
// has an identity rotation, and every article exactly one instance.
//
// Article: u16 strips, u16 strip bytes, u16 barriers, u16 barrier bytes, u8 0,
// u8 surfaces, u16 surface bytes, u32 flags; then
//   strip headers, 16 bytes each: float centre[3], u16 radius * 16, u16 offset
//       of the strip's vertices from the first header;
//   vertices, 8 bytes each: i16 xyz in 1/128 from the strip's centre, u8, u8.
//       The first u8 of vertex 0 is the vertex count; from vertex 2 on it is
//       the surface index of the triangle that vertex closes and the second u8
//       its flags (0, 4, 8, 12, 20 seen). Vertex 1's u16 is the strip's own:
//       bit 0 = the first triangle winds the other way round (it then
//       alternates), bit 1 = both faces count. Read from the game's strip
//       walkers, nfsw.exe 0x6D0450 and 0x6D5B50 (triangle built by 0x6CF710,
//       scale 1/128 at 0xC85B20): they take the count from vertex 0, skip a
//       strip whose vertex 1 word meets the query's mask and a triangle whose
//       flags have 0x08 or meet it. Their caller 0x6D6040 returns at once for
//       an article without strips and uses a strip header's centre and radius
//       (u16 / 16) as a circle to cull by;
//   barriers, 32 bytes each: float xyz of one end (y = bottom), u8 surface
//       index, u8 flags, u16, float xyz of the other end (y = top), float
//       1 / length in plan;
//   surfaces: u32 hash each.
//
// Collision axes are (x, up, z); the game's are (z, -x, up).

namespace nfs {
namespace {

constexpr u32 kTagPack     = 0x43415250;   // 'CARP'
constexpr u32 kTagArticles = 0x41727469;   // 'Arti'
constexpr u32 kTagArticle  = 0x6361;       // 'ca', the upper half of the tag
constexpr u32 kTagInstances = 0x63690000;  // 'ci'

constexpr size_t kInstanceSize = 64;
constexpr float  kVertexScale  = 1.0f / 128.0f;

// A stable byte for a surface hash, so the viewport's colour per surface id
// keeps telling materials apart.
u8 SurfaceId(u32 hash) {
    return (u8)((hash * 2654435761u) >> 24);
}

} // namespace

bool ParseWorldCollision(const Chunk& chunk, CollisionPack& out) {
    ByteSpan data = chunk.data;
    size_t filler = 0;
    while (filler < 16 && filler < data.size && data.data[filler] == 0x11) ++filler;
    data = data.sub(filler, data.size);

    if (data.read<u32>(0x10) != kTagPack || data.read<u32>(0x20) != kTagArticles) return false;
    const u32 rowCount = data.read<u32>(0x28);
    if (0x30 + (u64)rowCount * 16 > data.size) return false;

    out.sectionNumber = data.read<u32>(4);

    std::vector<ByteSpan> articles;
    ByteSpan instances;
    u32 instanceCount = 0;
    for (u32 r = 0; r < rowCount; ++r) {
        const size_t row = 0x30 + (size_t)r * 16;
        const u32 tag  = data.read<u32>(row);
        const u32 info = data.read<u32>(row + 4);
        size_t at = data.read<u32>(row + 12);
        if (info & 2) at += row;
        const ByteSpan body = data.sub(at, info >> 8);

        if (tag == kTagInstances) {
            instances = body;
            instanceCount = data.read<u32>(row + 8);
        } else if ((tag >> 16) == kTagArticle) {
            const size_t index = tag & 0xFFFF;
            if (articles.size() <= index) articles.resize(index + 1);
            articles[index] = body;
        }
    }

    bool rotated = false;
    for (u32 i = 0; i < instanceCount && (i + 1) * kInstanceSize <= instances.size; ++i) {
        const ByteSpan instance = instances.sub(i * kInstanceSize, kInstanceSize);
        const u16 index = instance.read<u16>(0x1A);
        if (index >= articles.size() || articles[index].size < 16) continue;
        const ByteSpan article = articles[index];

        if (std::fabs(instance.read<float>(0x00) - 1.0f) > 1e-4f ||
            std::fabs(instance.read<float>(0x28) - 1.0f) > 1e-4f) rotated = true;

        // world = local - inverse position, then to the game's axes.
        const Vec3 shift{ instance.read<float>(0x30), instance.read<float>(0x34),
                          instance.read<float>(0x38) };
        auto world = [&](float x, float y, float z) {
            return Vec3{ z - shift.z, -(x - shift.x), y - shift.y };
        };

        const size_t stripCount   = article.read<u16>(0);
        const size_t stripBytes   = article.read<u16>(2);
        const size_t barrierCount = article.read<u16>(4);
        const size_t barrierBytes = article.read<u16>(6);
        const size_t surfaceCount = article.read<u8>(9);
        const size_t strips   = 16;
        const size_t barriers = strips + stripBytes;
        const size_t surfaces = barriers + barrierBytes;

        auto fill = [&](CollisionPolygon& polygon, u8 surfaceIndex, u8 flags) {
            const u32 hash = surfaceIndex < surfaceCount
                           ? article.read<u32>(surfaces + (size_t)surfaceIndex * 4) : 0;
            polygon.surface = SurfaceId(hash);
            polygon.flags   = flags;
            std::memcpy(polygon.attributes, &hash, 4);
            for (const Vec3& corner : polygon.corners) out.bounds.Add(corner);
        };

        for (size_t s = 0; s < stripCount; ++s) {
            const size_t header = strips + s * 16;
            const Vec3 centre{ article.read<float>(header), article.read<float>(header + 4),
                               article.read<float>(header + 8) };
            const size_t first = strips + article.read<u16>(header + 14);
            const size_t count = article.read<u8>(first + 6);
            if (first + count * 8 > barriers) break;

            Vec3 previous[2];
            for (size_t k = 0; k < count; ++k) {
                const size_t at = first + k * 8;
                const Vec3 point = world(centre.x + article.read<i16>(at) * kVertexScale,
                                         centre.y + article.read<i16>(at + 2) * kVertexScale,
                                         centre.z + article.read<i16>(at + 4) * kVertexScale);
                if (k >= 2) {
                    CollisionPolygon polygon;
                    // `previous` is overwritten in turn, which flips every other
                    // triangle the way a strip needs.
                    polygon.corners[0] = previous[0];
                    polygon.corners[1] = previous[1];
                    polygon.corners[2] = polygon.corners[3] = point;
                    fill(polygon, article.read<u8>(at + 6), article.read<u8>(at + 7));
                    polygon.source = (u32)out.polygons.size();
                    out.polygons.push_back(polygon);
                }
                previous[k & 1] = point;
            }
        }

        for (size_t b = 0; b < barrierCount && barriers + (b + 1) * 32 <= surfaces; ++b) {
            const size_t at = barriers + b * 32;
            CollisionPolygon polygon;
            polygon.barrier = polygon.wall = true;
            polygon.corners[0] = polygon.corners[1] =
                world(article.read<float>(at), article.read<float>(at + 4), article.read<float>(at + 8));
            polygon.corners[2] = polygon.corners[3] =
                world(article.read<float>(at + 16), article.read<float>(at + 20),
                      article.read<float>(at + 24));
            fill(polygon, article.read<u8>(at + 12), article.read<u8>(at + 13));
            polygon.source = (u32)out.polygons.size();
            out.polygons.push_back(polygon);
        }
    }

    if (rotated)
        LOG_WARN("collision pack %u has a rotated instance, drawn unrotated", out.sectionNumber);
    out.shippedPolygons = (u32)out.polygons.size();
    return !out.polygons.empty();
}

// ---------------------------------------------------------------------------
// Removing polygons
// ---------------------------------------------------------------------------
//
// Layout rules the writer relies on, each true of all 747 shipped packs and
// 10 524 articles (tools/nfsw/col_check.py and the checks in this file):
//   - a pack's bodies follow one another in row order from the first body on,
//     each starting on a multiple of 16, 0xAA in every gap and after the last;
//   - an article is its header, strip headers, vertices back to back in strip
//     order (a zero vertex when their number is odd), barriers, surfaces, then
//     zeros up to a multiple of 64;
//   - vertex 1's first u8 says which way the strip winds: 0 as stored, 1 the
//     other way round, 2 on strips whose triangles face both ways. A strip
//     that starts on an odd triangle of another has to swap 0 and 1.
// Before anything is changed the pack and every article to be touched are
// written again unchanged and compared: a difference stops the write.

namespace {

template <class T>
void Put(std::vector<u8>& bytes, size_t at, T value) {
    std::memcpy(bytes.data() + at, &value, sizeof(T));
}

// How many polygons the parser lists for an article.
// A face a car should hit, not drive on: upright, or steeper than the editor's
// "looks up" (|normal z| > 0.6, CollisionGenerate). Written as a barrier.
// Lower than kStep (speed bumps, cobbles, kerbs) it stays ground: a barrier
// that short throws the car (0x6f0a00 gathers it like any wall), while ground
// is ridden over by the wheels. An exactly upright low face then has no area
// in plan and the game never touches it; the wheels step onto what is on top.
// ponytail: one height for all; per-face choice in the editor if 0.5 m is wrong.
bool WallLike(const CollisionPolygon& polygon) {
    constexpr float kStep = 0.5f;
    const int corners = polygon.quad ? 4 : 3;
    float bottom = polygon.corners[0].z, top = bottom;
    for (int i = 1; i < corners; ++i) {
        bottom = std::min(bottom, polygon.corners[i].z);
        top = std::max(top, polygon.corners[i].z);
    }
    if (top - bottom < kStep) return false;
    if (polygon.Vertical()) return true;
    const Vec3 n = Cross(polygon.corners[1] - polygon.corners[0], polygon.corners[2] - polygon.corners[0]);
    const float length = Length(n);
    return length > 1e-6f && std::fabs(n.z) / length <= 0.6f;
}

size_t PolygonCount(ByteSpan article) {
    const size_t stripCount = article.read<u16>(0);
    const size_t barriers   = 16 + (size_t)article.read<u16>(2);
    const size_t surfaces   = barriers + article.read<u16>(6);
    size_t count = 0;
    for (size_t s = 0; s < stripCount; ++s) {
        const size_t first = 16 + (size_t)article.read<u16>(16 + s * 16 + 14);
        const size_t vertices = article.read<u8>(first + 6);
        if (first + vertices * 8 > barriers) break;
        if (vertices > 2) count += vertices - 2;
    }
    for (size_t b = 0; b < article.read<u16>(4) && barriers + (b + 1) * 32 <= surfaces; ++b) ++count;
    return count;
}

// An article without the polygons marked in `drop`, which goes by the order
// the parser lists them in. False when it is not laid out as expected.
// `newStrips` holds whole one-triangle strips to add (16-byte header and three
// vertices, 40 bytes each), `newBarriers` barrier records (32 bytes each).
// `newSurfaces` are surface hashes appended to the article's table.
bool RebuildArticle(ByteSpan article, const std::vector<bool>& drop, std::vector<u8>& out,
                    const std::vector<u8>& newStrips = {}, const std::vector<u8>& newBarriers = {},
                    const std::vector<u32>& newSurfaces = {}) {
    const size_t addedStrips = newStrips.size() / 40, addedBarriers = newBarriers.size() / 32;
    const size_t stripCount   = article.read<u16>(0);
    const size_t stripBytes   = article.read<u16>(2);
    const size_t barrierCount = article.read<u16>(4);
    const size_t barrierBytes = article.read<u16>(6);
    const size_t surfaceBytes = article.read<u16>(10);
    const size_t strips   = 16;
    const size_t barriers = strips + stripBytes;
    const size_t surfaces = barriers + barrierBytes;
    if (barrierBytes != barrierCount * 32 || surfaces + surfaceBytes > article.size) return false;

    // The strips that are left: runs of triangles that stay, each a stretch
    // of the old strip's vertices.
    struct Run { size_t header, first, count; u8 winding; };
    std::vector<Run> runs;
    size_t polygon = 0;
    for (size_t s = 0; s < stripCount; ++s) {
        const size_t header = strips + s * 16;
        const size_t first  = strips + article.read<u16>(header + 14);
        const size_t count  = article.read<u8>(first + 6);
        if (count < 3 || first + count * 8 > barriers || polygon + count - 2 > drop.size()) return false;
        const u8 winding = article.read<u8>(first + 8 + 6);
        for (size_t t = 0; t + 2 < count;) {
            if (drop[polygon + t]) { ++t; continue; }
            size_t last = t;
            while (last + 3 < count && !drop[polygon + last + 1]) ++last;
            const u8 turned = (t & 1) && winding < 2 ? (u8)(winding ^ 1) : winding;
            runs.push_back({ header, first + t * 8, last - t + 3, turned });
            t = last + 1;
        }
        polygon += count - 2;
    }
    if (polygon + barrierCount != drop.size()) return false;

    std::vector<size_t> keptBarriers;
    for (size_t b = 0; b < barrierCount; ++b)
        if (!drop[polygon + b]) keptBarriers.push_back(b);

    size_t vertices = addedStrips * 3;
    for (const Run& run : runs) vertices += run.count;
    const size_t stripTotal = runs.size() + addedStrips;
    const size_t barrierTotal = keptBarriers.size() + addedBarriers;
    const size_t newStripBytes = (stripTotal * 16 + vertices * 8 + 15) & ~(size_t)15;
    if (newStripBytes > 0xFFFF || barrierTotal * 32 > 0xFFFF) return false;

    // The surface table is 4 bytes a hash, its count at byte 9 (true of every
    // shipped article).
    if (surfaceBytes != (size_t)article.read<u8>(9) * 4 || article.read<u8>(9) + newSurfaces.size() > 0xFF)
        return false;
    const size_t used = 16 + newStripBytes + barrierTotal * 32 + surfaceBytes + newSurfaces.size() * 4;
    out.assign((used + 63) & ~(size_t)63, 0);
    std::memcpy(out.data(), article.data, 16);
    Put<u16>(out, 0, (u16)stripTotal);
    Put<u16>(out, 2, (u16)newStripBytes);
    Put<u16>(out, 4, (u16)barrierTotal);
    Put<u16>(out, 6, (u16)(barrierTotal * 32));
    out[9] = (u8)(article.read<u8>(9) + newSurfaces.size());
    Put<u16>(out, 10, (u16)(surfaceBytes + newSurfaces.size() * 4));

    size_t at = 16 + stripTotal * 16;
    for (size_t r = 0; r < runs.size(); ++r) {
        const Run& run = runs[r];
        std::memcpy(out.data() + 16 + r * 16, article.data + run.header, 16);   // centre and radius stay
        Put<u16>(out, 16 + r * 16 + 14, (u16)(at - 16));
        std::memcpy(out.data() + at, article.data + run.first, run.count * 8);
        out[at + 6]  = (u8)run.count;
        out[at + 7]  = 0;
        out[at + 14] = run.winding;
        out[at + 15] = 0;
        at += run.count * 8;
    }
    for (size_t s = 0; s < addedStrips; ++s) {
        const size_t header = 16 + (runs.size() + s) * 16;
        std::memcpy(out.data() + header, newStrips.data() + s * 40, 16);
        Put<u16>(out, header + 14, (u16)(at - 16));
        std::memcpy(out.data() + at, newStrips.data() + s * 40 + 16, 24);
        at += 24;
    }
    at = 16 + newStripBytes;
    for (size_t b : keptBarriers) {
        std::memcpy(out.data() + at, article.data + barriers + b * 32, 32);
        at += 32;
    }
    if (addedBarriers) std::memcpy(out.data() + at, newBarriers.data(), newBarriers.size());
    at += newBarriers.size();
    std::memcpy(out.data() + at, article.data + surfaces, surfaceBytes);
    if (!newSurfaces.empty()) std::memcpy(out.data() + at + surfaceBytes, newSurfaces.data(), newSurfaces.size() * 4);
    return true;
}

} // namespace

bool RemoveFromWorldCollision(u8* payload, size_t size, const std::vector<u32>& removed,
                              std::wstring& error) {
    if (removed.empty()) return true;
    std::vector<u8> out;
    size_t skipped = 0;
    if (!RewriteWorldCollision(ByteSpan{ payload, size }, CollisionPack{}, removed, {}, skipped, out, error))
        return false;
    if (out.size() != size) {
        error = L"Collision section has no room for the strips these deletions cut in two.";
        return false;
    }
    std::memcpy(payload, out.data(), size);
    return true;
}

// The part of a polygon where in(p) >= 0 (one Sutherland-Hodgman step), heights
// interpolated.
template <class In>
static std::vector<Vec3> ClipHalf(const std::vector<Vec3>& polygon, In in) {
    std::vector<Vec3> kept;
    for (size_t i = 0; i < polygon.size(); ++i) {
        const Vec3& a = polygon[i];
        const Vec3& b = polygon[(i + 1) % polygon.size()];
        const float da = in(a), db = in(b);
        if (da >= 0.0f) kept.push_back(a);
        if ((da >= 0.0f) != (db >= 0.0f)) kept.push_back(a + (b - a) * (da / (da - db)));
    }
    return kept;
}

// The part of a polygon inside x0..x1, y0..y1 in plan, heights interpolated;
// empty when they do not meet.
static std::vector<Vec3> ClipToPlan(std::vector<Vec3> polygon, float x0, float x1, float y0, float y1) {
    for (int side = 0; side < 4 && !polygon.empty(); ++side)
        polygon = ClipHalf(polygon, [&](const Vec3& p) {
            return side == 0 ? p.x - x0 : side == 1 ? x1 - p.x : side == 2 ? p.y - y0 : y1 - p.y;
        });
    return polygon.size() >= 3 ? polygon : std::vector<Vec3>{};
}

// From the first corner: world coordinates (~7000 m) squared lose the area in float.
static float PlanArea(const std::vector<Vec3>& polygon) {
    float twice = 0.0f;
    for (size_t i = 1; i + 1 < polygon.size(); ++i) {
        const Vec3 a = polygon[i] - polygon[0], b = polygon[i + 1] - polygon[0];
        twice += a.x * b.y - b.x * a.y;
    }
    return std::fabs(twice) * 0.5f;
}

bool CutUnderRaised(const CollisionPolygon& ground, const std::vector<const CollisionPolygon*>& raised,
                    std::vector<CollisionPolygon>& pieces) {
    // ponytail: 1 cm^2 slivers are dropped; a wheel point there finds the face beside it next step.
    constexpr float kSliver = 1e-4f;
    // On top by 2 cm to 1 m (a bump's foot may sit a few cm under the road).
    constexpr float kAbove = 0.02f, kHighest = 1.0f, kSunk = -0.1f;
    if (ground.barrier || WallLike(ground)) return false;
    auto triangles = [](const CollisionPolygon& p) {
        std::vector<std::vector<Vec3>> out{ { p.corners[0], p.corners[1], p.corners[2] } };
        if (p.quad) out.push_back({ p.corners[0], p.corners[2], p.corners[3] });
        return out;
    };
    auto bounds = [](const CollisionPolygon& p) {
        Aabb box;
        for (int i = 0; i < (p.quad ? 4 : 3); ++i) box.Add(p.corners[i]);
        return box;
    };
    const Aabb home = bounds(ground);
    std::vector<std::vector<Vec3>> parts = triangles(ground);
    bool cut = false;
    for (const CollisionPolygon* top : raised) {
        if (top == &ground || top->barrier || WallLike(*top)) continue;
        const Aabb box = bounds(*top);
        if (box.max.x < home.min.x || box.min.x > home.max.x || box.max.y < home.min.y || box.min.y > home.max.y)
            continue;
        for (const std::vector<Vec3>& t : triangles(*top)) {
            const Vec3 n = Cross(t[1] - t[0], t[2] - t[0]);
            if (std::fabs(n.z) < 1e-6f) continue;
            const float turn = n.z > 0.0f ? 1.0f : -1.0f;   // counter-clockwise in plan when up
            auto edge = [&](int e, float sign) {
                const Vec3 a = t[(size_t)e], b = t[(size_t)(e + 1) % 3];
                return [=](const Vec3& p) { return sign * turn * ((b.x - a.x) * (p.y - a.y) - (b.y - a.y) * (p.x - a.x)); };
            };
            std::vector<std::vector<Vec3>> next;
            for (std::vector<Vec3>& part : parts) {
                std::vector<Vec3> inside = part;
                for (int e = 0; e < 3 && inside.size() >= 3; ++e) inside = ClipHalf(inside, edge(e, 1.0f));
                float lowest = 1e30f, highest = -1e30f;
                if (inside.size() >= 3 && PlanArea(inside) > kSliver)
                    for (const Vec3& p : inside) {
                        const float z = t[0].z - (n.x * (p.x - t[0].x) + n.y * (p.y - t[0].y)) / n.z;
                        lowest = std::min(lowest, z - p.z);
                        highest = std::max(highest, z - p.z);
                    }
                if (!(highest > kAbove && highest <= kHighest && lowest >= kSunk)) {
                    next.push_back(std::move(part));
                    continue;
                }
                // Ground minus the triangle: what lies past each edge in turn.
                std::vector<Vec3> rest = part;
                for (int e = 0; e < 3 && rest.size() >= 3; ++e) {
                    std::vector<Vec3> outside = ClipHalf(rest, edge(e, -1.0f));
                    if (outside.size() >= 3 && PlanArea(outside) > kSliver) next.push_back(std::move(outside));
                    rest = ClipHalf(rest, edge(e, 1.0f));
                }
                cut = true;
            }
            parts.swap(next);
        }
    }
    if (!cut) return false;
    pieces.clear();
    for (const std::vector<Vec3>& part : parts)
        for (size_t v = 1; v + 1 < part.size(); ++v) {
            CollisionPolygon piece = ground;
            piece.quad = false;
            piece.source = CollisionPolygon::kNoSource;
            piece.flags &= ~0x10;   // not a quad any more
            piece.attributes[3] = piece.flags;
            piece.corners[0] = part[0];
            piece.corners[1] = part[v];
            piece.corners[2] = piece.corners[3] = part[v + 1];
            pieces.push_back(piece);
        }
    return true;
}

bool RewriteWorldCollision(ByteSpan whole, const CollisionPack& pack, const std::vector<u32>& removed,
                           const std::vector<CollisionPolygon>& added, size_t& skipped,
                           std::vector<u8>& out, std::wstring& error,
                           const std::vector<CollisionPolygon>& spill, std::vector<u8>* placed,
                           std::vector<GridEntry>* grown) {
    if (placed) placed->assign(added.size() + spill.size(), 0);
    if (grown) grown->clear();
    const u8* payload = whole.data;
    const size_t size = whole.size;
    size_t filler = 0;
    while (filler < 16 && filler < size && payload[filler] == 0x11) ++filler;
    const ByteSpan data{ payload + filler, size - filler };
    const std::wstring name = L"Collision section " + std::to_wstring(data.read<u32>(4));
    auto fail = [&](const wchar_t* why) {
        error = name + L" " + why;
        return false;
    };
    const wchar_t* const kOdd = L"is not laid out the way the shipped packs are; nothing was written.";

    const u32 rowCount = data.read<u32>(0x28);
    const size_t end = 16 + (size_t)data.read<u32>(0);
    if (data.read<u32>(0x10) != kTagPack || data.read<u32>(0x20) != kTagArticles ||
        end != data.size || 0x30 + (u64)rowCount * 16 > end) return fail(kOdd);

    struct Row { size_t row, at, size; u32 info; };
    std::vector<Row> rows;
    std::vector<int> articleRow;   // article index -> row, -1 when there is none
    int instanceRow = -1;
    size_t start = end;
    for (u32 r = 0; r < rowCount; ++r) {
        Row row;
        row.row  = 0x30 + (size_t)r * 16;
        row.info = data.read<u32>(row.row + 4);
        row.size = row.info >> 8;
        row.at   = row.row + data.read<u32>(row.row + 12);
        if (!(row.info & 2) || row.at + row.size > end) return fail(kOdd);
        start = std::min(start, row.at);

        const u32 tag = data.read<u32>(row.row);
        if (tag == kTagInstances) instanceRow = (int)r;
        else if ((tag >> 16) == kTagArticle) {
            const size_t index = tag & 0xFFFF;
            if (articleRow.size() <= index) articleRow.resize(index + 1, -1);
            articleRow[index] = (int)r;
        }
        rows.push_back(row);
    }
    if (instanceRow < 0) return fail(kOdd);

    // Which row holds each polygon, going through the instances the way the
    // parser does.
    struct Range { u32 first, count; int row; };
    std::vector<Range> ranges;
    std::vector<int> uses(rows.size(), 0);
    {
        const Row& list = rows[(size_t)instanceRow];
        const u32 instanceCount = data.read<u32>(list.row + 8);
        u32 next = 0;
        for (u32 i = 0; i < instanceCount && (i + 1) * kInstanceSize <= list.size; ++i) {
            const u16 index = data.read<u16>(list.at + i * kInstanceSize + 0x1A);
            const int row = index < articleRow.size() ? articleRow[index] : -1;
            if (row < 0 || rows[(size_t)row].size < 16) continue;
            const u32 count = (u32)PolygonCount(data.sub(rows[(size_t)row].at, rows[(size_t)row].size));
            ranges.push_back({ next, count, row });
            ++uses[(size_t)row];
            next += count;
        }
    }

    std::vector<std::vector<bool>> drops(rows.size());
    for (u32 source : removed) {
        const auto range = std::find_if(ranges.begin(), ranges.end(), [&](const Range& candidate) {
            return source >= candidate.first && source - candidate.first < candidate.count;
        });
        if (range == ranges.end()) return fail(L"does not hold one of the polygons to remove; nothing was written.");
        if (uses[(size_t)range->row] != 1) return fail(L"shares an article between two instances; nothing was written.");
        std::vector<bool>& drop = drops[(size_t)range->row];
        if (drop.empty()) drop.assign(range->count, false);
        drop[source - range->first] = true;
    }

    // --- polygons to add ---------------------------------------------------------
    // Each goes into the article of an instance whose box holds it in plan: the
    // game finds instances through a grid made from those boxes (0x6F2570), so
    // inside one the polygon is reached and outside every one it is not.
    std::vector<std::vector<u8>> newStrips(rows.size()), newBarriers(rows.size());
    std::vector<std::vector<u32>> newSurfaces(rows.size());
    constexpr u32 kAsphaltSurface = 0x19DB2F1E;   // simsurface "asphalt" (Attrib hash)
    std::vector<u8> instanceBody;
    {
        const Row& list = rows[(size_t)instanceRow];
        instanceBody.assign(data.data + list.at, data.data + list.at + list.size);
        const size_t instanceCount = std::min<size_t>(data.read<u32>(list.row + 8), list.size / kInstanceSize);
        auto field = [&](size_t at) { float v; std::memcpy(&v, instanceBody.data() + at, 4); return v; };
        auto article16 = [&](size_t at) { u16 v; std::memcpy(&v, instanceBody.data() + at + 0x1A, 2); return v; };

        std::vector<bool> grew(instanceCount, false);
        for (size_t n = 0; n < added.size() + spill.size(); ++n) {
            // `spill`: another pack's ground no box there held whole, cut into ours.
            const bool foreign = n >= added.size();
            const CollisionPolygon& polygon = foreign ? spill[n - added.size()] : added[n];
            // Its primitives in the game's axes: triangles, or barrier ends.
            Vec3 points[8];
            int count = 0;
            // A wall or a steep side is written as a barrier record. The game
            // tests triangles only in plan (nfsw.exe 0x6CFBE0 / 0x6CFA20, from the
            // strip walkers 0x6D0450 / 0x6D5B50): a vertical triangle has no area
            // there and is never touched, and a steep one would only be ground to
            // climb. Cars hit barriers (0x6D6E10: the move crosses the segment in
            // plan, at a height between its two ends, normal turned towards the
            // car, so one record holds from both sides). 99 % of the shipped
            // triangles face up within 0.9; the walls are all barriers.
            const bool wallLike = !polygon.barrier && WallLike(polygon);
            const bool barrier = polygon.barrier || wallLike;
            const bool shipped = polygon.source != CollisionPolygon::kNoSource;
            if (wallLike) {
                // Ends: the two corners furthest apart in plan; from the lowest
                // corner up to the highest. A triangle comes out as the
                // rectangle round it.
                const int corners = polygon.quad ? 4 : 3;
                int ea = 0, eb = 1;
                float far = -1.0f, bottom = 1e30f, top = -1e30f;
                for (int i = 0; i < corners; ++i) {
                    bottom = std::min(bottom, polygon.corners[i].z);
                    top    = std::max(top, polygon.corners[i].z);
                    for (int j = i + 1; j < corners; ++j) {
                        const float d = std::hypot(polygon.corners[i].x - polygon.corners[j].x,
                                                   polygon.corners[i].y - polygon.corners[j].y);
                        if (d > far) { far = d; ea = i; eb = j; }
                    }
                }
                if (far < 0.05f || top - bottom < 0.05f) { skipped += !foreign; continue; }
                // A barrier holds cars on one side only: to the right of its
                // first end towards its second, seen from above (the road lies
                // there for 5476 of 5510 shipped barriers with ground on one
                // side; Gui's walls blocked from one side). That side is the
                // face's front (the editor draws the back in another colour).
                const Vec3 normal = Cross(polygon.corners[1] - polygon.corners[0],
                                          polygon.corners[2] - polygon.corners[0]);
                const Vec3 along = polygon.corners[eb] - polygon.corners[ea];
                if (along.y * normal.x - along.x * normal.y < 0.0f) std::swap(ea, eb);
                points[count++] = { polygon.corners[ea].x, polygon.corners[ea].y, bottom };
                points[count++] = { polygon.corners[eb].x, polygon.corners[eb].y, top };
            } else if (barrier && polygon.wall) {
                points[count++] = polygon.corners[0];
                points[count++] = polygon.corners[2];
            } else if (barrier) {
                // An editor barrier has no height: the wall the viewport shows,
                // a metre under it and 64 up like the game's own.
                Vec3 quad[4];
                pack.BarrierQuad(polygon, quad);
                const float bottom = std::min(quad[0].z, quad[3].z) - 1.0f, top = bottom + 64.0f;
                points[count++] = { quad[0].x, quad[0].y, bottom };
                points[count++] = { quad[3].x, quad[3].y, top };
            } else {
                for (int c : { 0, 1, 2 }) points[count++] = polygon.corners[c];
                if (polygon.quad) for (int c : { 0, 2, 3 }) points[count++] = polygon.corners[c];
            }

            // The smallest instance box that holds every point.
            auto usable = [&](size_t i) {
                const u16 index = article16(i * kInstanceSize);
                const int row = index < articleRow.size() ? articleRow[index] : -1;
                return row >= 0 && uses[(size_t)row] == 1 && rows[(size_t)row].size >= 16;
            };
            int best = -1;
            float bestArea = 1e30f;
            for (size_t i = 0; i < instanceCount; ++i) {
                if (!usable(i)) continue;
                const size_t at = i * kInstanceSize;
                const float width = field(at + 0x0C), length = field(at + 0x2C);
                const float sx = field(at + 0x30), sz = field(at + 0x38);
                bool inside = true;
                for (int k = 0; k < count && inside; ++k)
                    inside = std::fabs(sx - points[k].y) <= width && std::fabs(points[k].x + sz) <= length;
                if (inside && width * length < bestArea) { bestArea = width * length; best = (int)i; }
            }
            // (box, primitives) to write: the whole polygon into that box, or else
            // ground cut along the boxes it crosses, each piece into its box. Left
            // out, a polygon wider than every box is a hole where the car stands on
            // SimSurface::kNull (body + 2 wheels: 0x85c7c0 resets it).
            // ponytail: pieces are box intersections, so where boxes overlap the
            // ground is written twice (same plane); subtract boxes if an article
            // runs out of room. Barriers still need one box.
            std::vector<std::pair<int, std::vector<Vec3>>> jobs;
            if (best >= 0 && !foreign) jobs.push_back({ best, std::vector<Vec3>(points, points + count) });
            else if (!barrier)
                for (size_t i = 0; i < instanceCount; ++i) {
                    if (!usable(i)) continue;
                    const size_t at = i * kInstanceSize;
                    const float width = field(at + 0x0C), length = field(at + 0x2C);
                    const float sx = field(at + 0x30), sz = field(at + 0x38);
                    std::vector<Vec3> pieces;
                    for (int k = 0; k + 2 < count; k += 3) {
                        const float x0 = -sz - length, x1 = -sz + length, y0 = sx - width, y1 = sx + width;
                        const Vec3 &a = points[k], &b = points[k + 1], &c = points[k + 2];
                        if (std::max({ a.x, b.x, c.x }) < x0 || std::min({ a.x, b.x, c.x }) > x1 ||
                            std::max({ a.y, b.y, c.y }) < y0 || std::min({ a.y, b.y, c.y }) > y1) continue;
                        const std::vector<Vec3> cut = ClipToPlan({ points[k], points[k + 1], points[k + 2] },
                                                                 -sz - length, -sz + length, sx - width, sx + width);
                        for (size_t v = 1; v + 1 < cut.size(); ++v)
                            for (const Vec3& p : { cut[0], cut[v], cut[v + 1] }) pieces.push_back(p);
                    }
                    if (!pieces.empty()) jobs.push_back({ (int)i, std::move(pieces) });
                }
            // Held by no box (a barrier, or ground outside them all): the nearest
            // box in plan grows to hold it. The grid and the radius follow below
            // (AddToCollisionGrid, +0x3C), so the game reaches it there.
            // ponytail: grows at most kGrowLimit; past that it stays left out.
            constexpr float kGrowLimit = 200.0f;
            if (jobs.empty() && !foreign) {
                int nearest = -1;
                float gap = kGrowLimit;
                for (size_t i = 0; i < instanceCount; ++i) {
                    if (!usable(i)) continue;
                    const size_t at = i * kInstanceSize;
                    const float width = field(at + 0x0C), length = field(at + 0x2C);
                    const float sx = field(at + 0x30), sz = field(at + 0x38);
                    float worst = 0.0f;
                    for (int k = 0; k < count; ++k)
                        worst = std::max({ worst, std::fabs(sx - points[k].y) - width,
                                           std::fabs(points[k].x + sz) - length });
                    if (worst < gap) { gap = worst; nearest = (int)i; }
                }
                if (nearest >= 0) {
                    const size_t at = (size_t)nearest * kInstanceSize;
                    float width = field(at + 0x0C), length = field(at + 0x2C);
                    const float sx = field(at + 0x30), sz = field(at + 0x38);
                    for (int k = 0; k < count; ++k) {
                        width  = std::max(width, std::fabs(sx - points[k].y) + 0.5f);
                        length = std::max(length, std::fabs(points[k].x + sz) + 0.5f);
                    }
                    std::memcpy(instanceBody.data() + at + 0x0C, &width, 4);
                    std::memcpy(instanceBody.data() + at + 0x2C, &length, 4);
                    best = nearest;
                    jobs.push_back({ nearest, std::vector<Vec3>(points, points + count) });
                }
            }
            if (placed) (*placed)[n] = jobs.empty() ? 0 : best >= 0 && !foreign ? 2 : 1;
            if (jobs.empty()) { skipped += !foreign; continue; }
            for (const auto& [box, primitives] : jobs) {
                grew[(size_t)box] = true;
                const int pieceCount = (int)primitives.size();
                const size_t at = (size_t)box * kInstanceSize;
                const size_t row = (size_t)articleRow[article16(at)];
                const ByteSpan article = data.sub(rows[row].at, rows[row].size);
                const Vec3 shift{ field(at + 0x30), field(at + 0x34), field(at + 0x38) };
                // Game axes to the article's own: (x, up, z) = (-y, z, x) + shift.
                float tall = field(at + 0x14), around = field(at + 0x3C);
                std::vector<Vec3> local((size_t)pieceCount);
                for (int k = 0; k < pieceCount; ++k) {
                    local[k] = { shift.x - primitives[k].y, primitives[k].z + shift.y, primitives[k].x + shift.z };
                    tall = std::max(tall, std::fabs(local[k].y) + 0.01f);
                    around = std::max(around, std::hypot(local[k].x, local[k].z) + 0.01f);
                }
                // The height, and the radius the game tests instances by in plan
                // round a point (0x6f2570: centre distance < +0x3C + query radius).
                std::memcpy(instanceBody.data() + at + 0x14, &tall, 4);
                std::memcpy(instanceBody.data() + at + 0x3C, &around, 4);

                // Its surface: the same hash when the article has it (a shipped
                // polygon that was moved), else the article's first.
                const size_t surfaceCount = article.read<u8>(9);
                const size_t surfaces = 16 + (size_t)article.read<u16>(2) + article.read<u16>(6);
                u32 hash = 0;
                std::memcpy(&hash, polygon.attributes, 4);
                u8 surface = 0;
                bool known = false;
                for (size_t s = 0; s < surfaceCount; ++s)
                    if (shipped && article.read<u32>(surfaces + s * 4) == hash) { surface = (u8)s; known = true; }
                if (!known || (hash == 0 && !polygon.barrier)) {
                    // Never the null surface (hash 0) on ground that was added, moved
                    // or given another surface: the car's reset behaviour (nfsw.exe
                    // 0x85c7c0 -> 0x858a20) puts a player car back on its last safe
                    // spot as soon as the body and two wheels stand on
                    // SimSurface::kNull, which is how the shipped map fences off
                    // ground it does not want driven on (Set Surface of Selection on
                    // such ground opens it). The article's first surface that is not
                    // null, else asphalt added to its table.
                    // ponytail: first non-null, not the nearest triangle's surface;
                    // pick by neighbour if grip/sound on new ground looks wrong.
                    surface = 0xFF;
                    for (size_t s = 0; s < surfaceCount && surface == 0xFF; ++s)
                        if (article.read<u32>(surfaces + s * 4) != 0) surface = (u8)s;
                    if (surface == 0xFF) {
                        if (newSurfaces[row].empty()) newSurfaces[row].push_back(kAsphaltSurface);
                        surface = (u8)surfaceCount;
                    }
                }
                const u8 flags = shipped ? polygon.flags : 0;

                if (barrier) {
                    for (int k = 0; k + 1 < pieceCount; k += 2) {
                        u8 record[32] = {};
                        const float plan = std::hypot(local[k + 1].x - local[k].x, local[k + 1].z - local[k].z);
                        const float inverse = plan > 1e-4f ? 1.0f / plan : 0.0f;
                        std::memcpy(record, &local[k], 12);
                        record[12] = surface;
                        record[13] = polygon.barrier && shipped ? flags : 2;   // 2: what 96 % of the map's carry
                        std::memcpy(record + 16, &local[k + 1], 12);
                        std::memcpy(record + 28, &inverse, 4);
                        // The same record twice is written once. A face and its twin
                        // (the editor stores upright faces both ways round) come out
                        // as reversed records -- each blocks on its own front -- so
                        // both stay and the wall holds from both sides.
                        std::vector<u8>& list = newBarriers[row];
                        bool twice = false;
                        for (size_t r = 0; r + 32 <= list.size() && !twice; r += 32)
                            twice = std::memcmp(list.data() + r, record, 12) == 0 &&
                                    std::memcmp(list.data() + r + 16, record + 16, 12) == 0;
                        if (!twice) list.insert(list.end(), record, record + 32);
                    }
                    continue;
                }
                for (int k = 0; k + 2 < pieceCount; k += 3) {
                    Vec3 a = local[k], b = local[k + 1], c = local[k + 2];
                    // Winding 0 is a strip whose first triangle looks up as stored.
                    if ((b.z - a.z) * (c.x - a.x) - (b.x - a.x) * (c.z - a.z) < 0.0f) std::swap(a, b);
                    const Vec3 centre = (a + b + c) * (1.0f / 3.0f);
                    float reach = 0.0f;
                    bool fits = true;
                    u8 strip[40] = {};
                    std::memcpy(strip, &centre, 12);
                    const Vec3 corner[3] = { a, b, c };
                    for (int v = 0; v < 3; ++v) {
                        const Vec3 d = corner[v] - centre;
                        reach = std::max(reach, std::hypot(d.x, d.z));
                        const float scaled[3] = { d.x * 128.0f, d.y * 128.0f, d.z * 128.0f };
                        for (int axis = 0; axis < 3; ++axis) {
                            fits = fits && std::fabs(scaled[axis]) < 32767.0f;
                            const i16 value = (i16)std::lround(scaled[axis]);
                            std::memcpy(strip + 16 + v * 8 + axis * 2, &value, 2);
                        }
                    }
                    if (!fits) { skipped += !foreign; continue; }
                    const u16 radius = (u16)std::min(65535.0f, std::ceil(reach * 16.0f) + 1.0f);
                    std::memcpy(strip + 12, &radius, 2);
                    strip[16 + 6] = 3;                              // vertex 0: the count
                    strip[24 + 6] = polygon.Vertical() ? 2 : 0;     // vertex 1: winding, 2 = both faces
                    strip[32 + 6] = surface;                        // vertex 2: surface and flags
                    strip[32 + 7] = flags;
                    newStrips[row].insert(newStrips[row].end(), strip, strip + 40);
                }
            }
        }
        // Instances that got polygons, with their boxes in the grid's axes
        // (collision x = -game y, z = game x), for AddToCollisionGrid.
        const u32 section = data.read<u32>(4);
        for (size_t i = 0; grown && i < instanceCount; ++i)
            if (grew[i]) {
                const size_t at = i * kInstanceSize;
                const float width = field(at + 0x0C), length = field(at + 0x2C);
                const float sx = field(at + 0x30), sz = field(at + 0x38);
                grown->push_back({ (section << 16) | (u32)i, -sx - width, -sx + width, -sz - length, -sz + length });
            }
    }

    std::vector<std::vector<u8>> bodies(rows.size());
    for (size_t r = 0; r < rows.size(); ++r) {
        if (drops[r].empty() && newStrips[r].empty() && newBarriers[r].empty()) continue;
        const ByteSpan article = data.sub(rows[r].at, rows[r].size);
        if (drops[r].empty()) drops[r].assign(PolygonCount(article), false);
        std::vector<u8> same;
        if (!RebuildArticle(article, std::vector<bool>(drops[r].size(), false), same) ||
            same.size() != article.size || std::memcmp(same.data(), article.data, article.size) != 0)
            return fail(kOdd);
        if (!RebuildArticle(article, drops[r], bodies[r], newStrips[r], newBarriers[r], newSurfaces[r]))
            return fail(L"would hold more collision in one article than the format allows (about 1600 "
                        L"triangles); nothing was written.");
    }
    if (std::memcmp(instanceBody.data(), data.data + rows[(size_t)instanceRow].at, instanceBody.size()) != 0)
        bodies[(size_t)instanceRow] = instanceBody;

    // The pack laid out again: with nothing replaced it has to come out as it
    // is, which is what says the rules above hold for this pack.
    std::vector<u8> packed;
    auto lay = [&](bool replace) {
        // The pack grows by whole steps of 0x100, as it is sized when shipped.
        size_t total = start;
        for (size_t r = 0; r < rows.size(); ++r)
            total += ((replace && !bodies[r].empty() ? bodies[r].size() : rows[r].size) + 15) & ~(size_t)15;
        size_t newEnd = end;
        while (newEnd < total) newEnd += 0x100;
        packed.assign(data.data, data.data + end);
        packed.resize(newEnd);
        Put<u32>(packed, 0, (u32)(newEnd - 16));
        std::fill(packed.begin() + (long long)start, packed.end(), (u8)0xAA);
        size_t cursor = start;
        for (size_t r = 0; r < rows.size(); ++r) {
            const bool replaced = replace && !bodies[r].empty();
            const u8* body = replaced ? bodies[r].data() : data.data + rows[r].at;
            const size_t bytes = replaced ? bodies[r].size() : rows[r].size;
            std::memcpy(packed.data() + cursor, body, bytes);
            Put<u32>(packed, rows[r].row + 4, (u32)(bytes << 8) | (rows[r].info & 0xFF));
            Put<u32>(packed, rows[r].row + 12, (u32)(cursor - rows[r].row));
            cursor += (bytes + 15) & ~(size_t)15;
        }
        return true;
    };
    if (!lay(false) || packed.size() != end || std::memcmp(packed.data(), data.data, end) != 0) return fail(kOdd);
    lay(true);

    out.assign(payload, payload + filler);
    out.insert(out.end(), packed.begin(), packed.end());
    return true;
}

// The collision grid (chunk 0x3B802 of the section; nfsw.exe 0x6d8d20 loads it,
// 0x6f2840 -> 0x6df170 -> 0x6f2570 reads it): rows CARP, Arti, CGcn, Name. CGcn
// holds one record per 96 m cell: u32 0, u16 cell (80 * z cell + x cell), u16
// section, u8 counts[4], u16 offsets[4] (4 * the counts before), then the lists;
// list 0 is u32 (pack << 16 | instance), the instances the game tests around a
// point. Shipped lists name an instance only where its shipped polygons lie, so
// ground added elsewhere in its box is found only now and then (the car reads
// SimSurface::kNull there and is reset). Every cell a grown box touches gets it.
// Grid origin and size: L5RA.BUN CGrd (x -5088, z 0, 96 m, 80 x 115 cells).
bool AddToCollisionGrid(ByteSpan raw, const std::vector<GridEntry>& entries, std::vector<u8>& out) {
    constexpr float kOriginX = -5088.0f, kOriginZ = 0.0f, kCell = 96.0f;
    constexpr int kColumns = 80, kRowsZ = 115;
    size_t filler = 0;
    while (filler < 16 && filler < raw.size && raw.data[filler] == 0x11) ++filler;
    const ByteSpan data = raw.sub(filler, raw.size);
    if (data.size < 80 || data.read<u32>(0) != data.size - 16) return false;
    // The shipped layout, exactly: four rows, CGcn and Name bodies after them.
    const u32 tags[4] = { 0x43415250, 0x41727469, 0x4347636E, 0x4E616D65 };   // CARP Arti CGcn Name (REA: 0x6d8d20 asks for 0x41727469, 0x4347636e)
    for (int r = 0; r < 4; ++r)
        if (data.read<u32>(16 + r * 16) != tags[r]) return false;
    const size_t cellRow = 48, nameRow = 64;
    const u32 cellInfo = data.read<u32>(cellRow + 4), nameInfo = data.read<u32>(nameRow + 4);
    const size_t cellsAt = cellRow + data.read<u32>(cellRow + 12), cellsSize = cellInfo >> 8;
    const size_t nameAt = nameRow + data.read<u32>(nameRow + 12), nameSize = nameInfo >> 8;
    const u32 count = data.read<u32>(cellRow + 8);
    if (cellsAt % 16 || cellsAt + cellsSize > nameAt || nameAt + nameSize > data.size) return false;

    struct Cell { std::vector<u8> bytes; };
    std::vector<Cell> cells;
    std::vector<int> byIndex(kColumns * kRowsZ, -1);
    for (size_t at = cellsAt, n = 0; n < count; ++n) {
        if (at + 0x14 > cellsAt + cellsSize) return false;
        const size_t length = 0x14 + 4 * ((size_t)data.read<u8>(at + 8) + data.read<u8>(at + 9) +
                                          data.read<u8>(at + 10) + data.read<u8>(at + 11));
        if (at + length > cellsAt + cellsSize) return false;
        const u16 index = data.read<u16>(at + 4);
        if (index < byIndex.size()) byIndex[index] = (int)cells.size();
        cells.push_back({ std::vector<u8>(data.data + at, data.data + at + length) });
        at += length;
    }
    const u16 section = (u16)data.read<u32>(4);
    bool changed = false;
    // Cells hold a point by round() (0x6cff40), shipped lists look like floor:
    // take both, the game tests the instance's radius after.
    auto span = [&](float lo, float hi, float origin, int cellCount, int& a, int& b) {
        a = std::max(0, (int)std::floor((lo - origin) / kCell));
        b = std::min(cellCount - 1, (int)std::floor((hi - origin) / kCell + 0.5f));
    };
    for (const GridEntry& entry : entries) {
        int x0, x1, z0, z1;
        span(entry.x0, entry.x1, kOriginX, kColumns, x0, x1);
        span(entry.z0, entry.z1, kOriginZ, kRowsZ, z0, z1);
        for (int z = z0; z <= z1; ++z)
            for (int x = x0; x <= x1; ++x) {
                const u16 index = (u16)(z * kColumns + x);
                if (byIndex[index] < 0) {
                    std::vector<u8> fresh(0x14, 0);
                    std::memcpy(fresh.data() + 4, &index, 2);
                    std::memcpy(fresh.data() + 6, &section, 2);
                    byIndex[index] = (int)cells.size();
                    cells.push_back({ std::move(fresh) });
                }
                std::vector<u8>& bytes = cells[(size_t)byIndex[index]].bytes;
                const u8 listed = bytes[8];
                bool present = false;
                for (u8 k = 0; k < listed && !present; ++k) {
                    u32 key;
                    std::memcpy(&key, bytes.data() + 0x14 + 4 * k, 4);
                    present = key == entry.key;
                }
                if (present) continue;
                if (listed == 0xFF) return false;
                const u8* key = reinterpret_cast<const u8*>(&entry.key);
                bytes.insert(bytes.begin() + 0x14 + 4 * listed, key, key + 4);
                bytes[8] = (u8)(listed + 1);
                for (int k = 1; k < 4; ++k) {
                    u16 offset;
                    std::memcpy(&offset, bytes.data() + 12 + 2 * k, 2);
                    offset = (u16)(offset + 4);
                    std::memcpy(bytes.data() + 12 + 2 * k, &offset, 2);
                }
                changed = true;
            }
    }
    if (!changed) return false;

    std::vector<u8> body;
    for (const Cell& cell : cells) body.insert(body.end(), cell.bytes.begin(), cell.bytes.end());
    const size_t newNameAt = (cellsAt + body.size() + 15) & ~(size_t)15;
    size_t end = data.size;
    while (end < newNameAt + nameSize) end += 0x100;
    std::vector<u8> laid(data.data, data.data + cellsAt);
    laid.resize(end, 0xAA);
    std::memcpy(laid.data() + cellsAt, body.data(), body.size());
    std::memcpy(laid.data() + newNameAt, data.data + nameAt, nameSize);
    auto put32 = [&](size_t at, u32 v) { std::memcpy(laid.data() + at, &v, 4); };
    put32(0, (u32)(end - 16));
    put32(cellRow + 4, (u32)(body.size() << 8) | (cellInfo & 0xFF));
    put32(cellRow + 8, (u32)cells.size());
    put32(nameRow + 12, (u32)(newNameAt - nameRow));
    out.assign(raw.data, raw.data + filler);
    out.insert(out.end(), laid.begin(), laid.end());
    return true;
}

// ---------------------------------------------------------------------------
// Self-test (FRS_WORLD_COLLISION_SELFTEST=<n>)
// ---------------------------------------------------------------------------

namespace {

struct SelfTestTotals { size_t packs = 0, written = 0, noRoom = 0, wrong = 0, removed = 0; };
SelfTestTotals g_selfTest;

// A polygon as something two parses can be compared by: its corners in a
// fixed order (a strip cut in two lists a triangle's corners from another
// start), its surface and its flags.
struct Shape {
    std::array<std::array<float, 3>, 3> corner{};
    std::array<u8, 4> attributes{};
    u8   flags   = 0;
    bool barrier = false;
    bool operator<(const Shape& o) const {
        return std::tie(corner, attributes, flags, barrier) < std::tie(o.corner, o.attributes, o.flags, o.barrier);
    }
    bool operator==(const Shape& o) const {
        return std::tie(corner, attributes, flags, barrier) == std::tie(o.corner, o.attributes, o.flags, o.barrier);
    }
};

Shape ShapeOf(const CollisionPolygon& polygon) {
    Shape shape;
    const int order[3] = { 0, polygon.barrier ? 2 : 1, 2 };
    for (int c = 0; c < 3; ++c) {
        const Vec3& p = polygon.corners[order[c]];
        shape.corner[(size_t)c] = { p.x, p.y, p.z };
    }
    if (!polygon.barrier) std::sort(shape.corner.begin(), shape.corner.end());
    std::memcpy(shape.attributes.data(), polygon.attributes, 4);
    shape.flags   = polygon.flags;
    shape.barrier = polygon.barrier;
    return shape;
}

} // namespace

void WorldCollisionSelfTest(const Chunk& chunk, const CollisionPack& shipped, u32 every) {
    ++g_selfTest.packs;
    const size_t count = shipped.polygons.size();
    // Two patterns: every n-th polygon (strips cut in many places), and one
    // block of neighbours (whole strips and articles go).
    for (int pattern = 0; pattern < 2; ++pattern) {
        std::vector<u32> removed;
        for (u32 i = 0; i < (u32)count; ++i)
            if (pattern == 0 ? i % every == 0 : (i >= count / 4 && i < count / 2)) removed.push_back(i);
        if (removed.empty()) continue;

        std::vector<u8> copy(chunk.raw.data, chunk.raw.data + chunk.raw.size);
        Chunk again = chunk;
        again.raw  = ByteSpan{ copy.data(), copy.size() };
        again.data = ByteSpan{ copy.data() + (chunk.data.data - chunk.raw.data), chunk.data.size };

        std::wstring error;
        if (!RemoveFromWorldCollision(copy.data() + (chunk.data.data - chunk.raw.data), chunk.data.size,
                                      removed, error)) {
            const bool untouched = std::memcmp(copy.data(), chunk.raw.data, copy.size()) == 0;
            const bool room = error.find(L"no room") != std::wstring::npos;
            if (room && untouched) { ++g_selfTest.noRoom; continue; }
            ++g_selfTest.wrong;
            LOG_WARN("world collision self-test: pack %u pattern %d refused%s: %S", shipped.sectionNumber,
                     pattern, untouched ? "" : " AND changed the bytes", error.c_str());
            continue;
        }

        std::vector<Shape> expected, found;
        for (u32 i = 0, r = 0; i < (u32)count; ++i) {
            if (r < removed.size() && removed[r] == i) { ++r; continue; }
            expected.push_back(ShapeOf(shipped.polygons[i]));
        }
        CollisionPack read;
        ParseWorldCollision(again, read);
        for (const CollisionPolygon& polygon : read.polygons) found.push_back(ShapeOf(polygon));
        std::sort(expected.begin(), expected.end());
        std::sort(found.begin(), found.end());
        if (expected == found) {
            ++g_selfTest.written;
            g_selfTest.removed += removed.size();
        } else {
            ++g_selfTest.wrong;
            LOG_WARN("world collision self-test: pack %u pattern %d reads back %zu polygons, expected %zu",
                     shipped.sectionNumber, pattern, found.size(), expected.size());
        }
    }
}

void LogWorldCollisionSelfTest() {
    if (!g_selfTest.packs) return;
    LOG_INFO("world collision self-test: %zu packs, %zu rewrites read back right (%zu polygons removed), "
             "%zu refused for lack of room, %zu WRONG", g_selfTest.packs, g_selfTest.written,
             g_selfTest.removed, g_selfTest.noRoom, g_selfTest.wrong);
    g_selfTest = {};
}

} // namespace nfs
