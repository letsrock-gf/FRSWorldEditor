// Water made in the editor, built the way the game's own is: ordinary meshes
// with its water textures.
//
//   - a lake is a flat surface over its outline, run on under the banks by
//     `shore`, with the game's animated water (ANM_WATERA_);
//   - a stream is a ribbon along its path at the height of its points; where
//     it drops `fallHeight` or more between two points, the water holds its
//     level to the next point and falls there, a curtain of the game's
//     waterfall texture (TRN_WATERFALL_A_LL) with its white water
//     (TRN_WATER_SURFACE_A_LL) at the foot; steep stretches get white water
//     too.
//
// A stream's bed is carved by RoadField::AddWaters, so the terrain made in
// the editor and the game's own ground are cut down under it and eased up to
// the banks the way they are fitted to roads. A lake is not: the ground simply
// runs on under its surface.

#include "roads/RoadNetwork.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace roads {

namespace {

constexpr const char* kWaterTexture = "ANM_WATERA_";
constexpr const char* kFallTexture = "TRN_WATERFALL_A_LL";
constexpr const char* kFoamTexture = "TRN_WATER_SURFACE_A_LL";
constexpr u8 kSurfaceDeepWater = 0x0E;    // DEEP_WATER in the game's surface table

// One point of a stream as built: where, the water's height, which way it
// flows, and whether the ribbon is broken before it (a fall).
struct StreamPoint {
    Vec3  p;            // z = the water's surface
    Vec3  forward;      // horizontal, unit
    float along = 0.0f;
    bool  fallBefore = false;
    bool  steep = false;  // the stretch before it is steep: white water
};

Vec3 CatmullRom(const Vec3& p0, const Vec3& p1, const Vec3& p2, const Vec3& p3, float t) {
    const float t2 = t * t, t3 = t2 * t;
    return (p1 * 2.0f + (p2 - p0) * t + (p0 * 2.0f - p1 * 5.0f + p2 * 4.0f - p3) * t2 +
            (p1 * 3.0f - p0 - p2 * 3.0f + p3) * t3) * 0.5f;
}

Vec3 Flat(const Vec3& v) {
    const float l = std::sqrt(v.x * v.x + v.y * v.y);
    return l > 1e-6f ? Vec3{ v.x / l, v.y / l, 0.0f } : Vec3{ 1.0f, 0.0f, 0.0f };
}

// A stream's path, smoothed and sampled every couple of metres, with its
// water's height worked out stretch by stretch.
std::vector<StreamPoint> StreamPath(const Water& w) {
    std::vector<StreamPoint> out;
    const std::vector<Vec3>& c = w.points;
    if (c.size() < 2) return out;
    for (size_t k = 0; k + 1 < c.size(); ++k) {
        const Vec3& p0 = c[k > 0 ? k - 1 : 0];
        const Vec3& p1 = c[k];
        const Vec3& p2 = c[k + 1];
        const Vec3& p3 = c[std::min(k + 2, c.size() - 1)];
        const float drop = p1.z - p2.z;
        const bool fall = drop >= std::max(0.3f, w.fallHeight);
        const float length = std::sqrt((p2.x - p1.x) * (p2.x - p1.x) + (p2.y - p1.y) * (p2.y - p1.y));
        const int steps = std::clamp((int)std::ceil(length / 2.0f), 2, 400);
        for (int i = k == 0 ? 0 : 1; i <= steps; ++i) {
            const float t = (float)i / steps;
            Vec3 q = CatmullRom(p0, p1, p2, p3, t);
            StreamPoint s;
            if (fall) {
                // Level to the end, then down: the fall stands at the next point.
                q.z = p1.z;
                if (i == steps) {
                    StreamPoint top;
                    top.p = q;
                    out.push_back(top);
                    s.fallBefore = true;
                    q.z = p2.z;
                }
            } else {
                q.z = p1.z + (p2.z - p1.z) * t;
                s.steep = length > 0.1f && drop / length > 0.12f;
            }
            s.p = q;
            out.push_back(s);
        }
    }
    // Directions and distances along it.
    for (size_t i = 0; i < out.size(); ++i) {
        const Vec3& a = out[i > 0 ? i - 1 : 0].p;
        const Vec3& b = out[std::min(i + 1, out.size() - 1)].p;
        out[i].forward = Flat(b - a);
        if (i > 0) {
            const Vec3 d = out[i].p - out[i - 1].p;
            out[i].along = out[i - 1].along + std::sqrt(d.x * d.x + d.y * d.y);
        }
    }
    return out;
}

float SignedArea(const std::vector<Vec3>& ring) {
    float a = 0.0f;
    for (size_t i = 0, j = ring.size() - 1; i < ring.size(); j = i++)
        a += (ring[j].x - ring[i].x) * (ring[j].y + ring[i].y);
    return a * 0.5f;     // positive anticlockwise
}

// Ear clipping of an anticlockwise ring.
std::vector<u32> Triangulate(const std::vector<Vec3>& ring) {
    std::vector<u32> out, left(ring.size());
    for (u32 i = 0; i < (u32)ring.size(); ++i) left[i] = i;
    const auto cross = [&](u32 a, u32 b, u32 c) {
        return (ring[b].x - ring[a].x) * (ring[c].y - ring[a].y) - (ring[b].y - ring[a].y) * (ring[c].x - ring[a].x);
    };
    size_t guard = 0;
    while (left.size() > 3 && guard++ < ring.size() * ring.size()) {
        bool clipped = false;
        for (size_t k = 0; k < left.size(); ++k) {
            const u32 a = left[(k + left.size() - 1) % left.size()], b = left[k], c = left[(k + 1) % left.size()];
            if (cross(a, b, c) <= 1e-6f) continue;
            bool ear = true;
            for (u32 q : left)
                if (q != a && q != b && q != c && cross(a, b, q) > 1e-6f && cross(b, c, q) > 1e-6f &&
                    cross(c, a, q) > 1e-6f) { ear = false; break; }
            if (!ear) continue;
            out.insert(out.end(), { a, b, c });
            left.erase(left.begin() + (ptrdiff_t)k);
            clipped = true;
            break;
        }
        if (!clipped) break;
    }
    if (left.size() == 3 && cross(left[0], left[1], left[2]) > 0.0f) out.insert(out.end(), { left[0], left[1], left[2] });
    return out;
}

// A lake's outline, anticlockwise, grown outwards by `by` (mitred, the mitre
// kept short at sharp corners).
std::vector<Vec3> Grow(std::vector<Vec3> ring, float by) {
    if (SignedArea(ring) < 0.0f) std::reverse(ring.begin(), ring.end());
    std::vector<Vec3> out;
    const size_t n = ring.size();
    for (size_t i = 0; i < n; ++i) {
        const Vec3& prev = ring[(i + n - 1) % n];
        const Vec3& here = ring[i];
        const Vec3& next = ring[(i + 1) % n];
        const Vec3 d0 = Flat(here - prev), d1 = Flat(next - here);
        // Outwards of an anticlockwise ring is to the right of travel.
        const Vec3 n0{ d0.y, -d0.x, 0.0f }, n1{ d1.y, -d1.x, 0.0f };
        Vec3 bisector = Flat(n0 + n1);
        const float cosHalf = std::max(0.35f, Dot(bisector, n1));
        out.push_back(here + bisector * (by / cosHalf));
    }
    return out;
}

void AddVertex(MeshGroup& group, const Vec3& p, const Vec3& normal, const Vec2& uv) {
    MeshVertex v;
    v.position = p;
    v.normal = normal;
    v.uv = uv;
    group.vertices.push_back(v);
}

} // namespace

void Network::BuildWaters(std::vector<MeshPiece>& pieces) const {
    for (const Water& w : waters_) {
        if (w.points.size() < 2) continue;
        MeshPiece piece;
        char name[64];
        std::snprintf(name, sizeof(name), "ROAD_Q%u", w.id);
        piece.name = name;
        // The water owns its pieces as a road does: a click on them picks it.
        piece.segment = w.id;
        const std::string surface = w.texture.empty() ? kWaterTexture : w.texture;
        const Vec3 up{ 0.0f, 0.0f, 1.0f };

        if (w.kind == Water::kLake) {
            if (w.points.size() < 3) continue;
            std::vector<Vec3> ring = Grow(w.points, std::max(0.0f, w.shore));
            for (Vec3& q : ring) q.z = w.level;
            const std::vector<u32> triangles = Triangulate(ring);
            MeshGroup& group = piece.Group(surface);
            for (const Vec3& q : ring) AddVertex(group, q, up, { q.x / 12.0f, -q.y / 12.0f });
            group.indices = triangles;
            if (w.deep)
                for (size_t k = 0; k + 2 < triangles.size(); k += 3) {
                    CollisionArea area;
                    area.surface = kSurfaceDeepWater;
                    area.outline = { ring[triangles[k]], ring[triangles[k + 1]], ring[triangles[k + 2]] };
                    piece.areas.push_back(std::move(area));
                }
        } else {
            const std::vector<StreamPoint> path = StreamPath(w);
            if (path.size() < 2) continue;
            const float half = std::max(0.5f, w.width * 0.5f) + std::max(0.0f, w.shore);
            MeshGroup& water = piece.Group(surface);
            // The surface: a ribbon, broken at each fall.
            for (size_t i = 0; i < path.size(); ++i) {
                const StreamPoint& s = path[i];
                const Vec3 right{ s.forward.y, -s.forward.x, 0.0f };
                const u32 base = (u32)water.vertices.size();
                AddVertex(water, s.p - right * half, up, { -half / 8.0f, -s.along / 8.0f });
                AddVertex(water, s.p + right * half, up, { half / 8.0f, -s.along / 8.0f });
                if (i == 0 || s.fallBefore) continue;
                const u32 a = base - 2, b = base - 1, c = base, d = base + 1;
                water.indices.insert(water.indices.end(), { a, c, b, b, c, d });
                if (w.deep) {
                    CollisionStrip strip;
                    strip.surface = kSurfaceDeepWater;
                    strip.left = { path[i - 1].p - right * half, s.p - right * half };
                    strip.right = { path[i - 1].p + right * half, s.p + right * half };
                    piece.strips.push_back(std::move(strip));
                }
            }
            // Every triangle counter-clockwise seen from above.
            for (size_t k = 0; k + 2 < water.indices.size(); k += 3) {
                const Vec3& p0 = water.vertices[water.indices[k]].position;
                const Vec3& p1 = water.vertices[water.indices[k + 1]].position;
                const Vec3& p2 = water.vertices[water.indices[k + 2]].position;
                if (Cross(p1 - p0, p2 - p0).z < 0.0f) std::swap(water.indices[k + 1], water.indices[k + 2]);
            }

            const float stream = std::max(0.5f, w.width * 0.5f);
            for (size_t i = 1; i < path.size(); ++i) {
                const StreamPoint& s = path[i];
                const Vec3 right{ s.forward.y, -s.forward.x, 0.0f };
                if (s.fallBefore) {
                    // The fall: a curtain from the lip down into the water below,
                    // leaning out a little, seen from both sides.
                    const float top = path[i - 1].p.z + 0.05f, bottom = s.p.z - 0.3f;
                    const float drop = top - bottom;
                    MeshGroup& fall = piece.Group(kFallTexture);
                    const int rows = std::clamp((int)std::ceil(drop / 2.0f), 1, 20);
                    const u32 base = (u32)fall.vertices.size();
                    for (int r = 0; r <= rows; ++r) {
                        const float f = (float)r / rows;
                        const float z = top - drop * f;
                        const Vec3 c = Vec3{ s.p.x, s.p.y, z } + s.forward * (drop * 0.15f * f * f);
                        AddVertex(fall, c - right * stream, s.forward, { 0.0f, (top - z) / 6.0f });
                        AddVertex(fall, c + right * stream, s.forward, { w.width / 6.0f, (top - z) / 6.0f });
                    }
                    for (int r = 0; r < rows; ++r) {
                        const u32 a = base + r * 2, b = a + 1, c = a + 2, d = a + 3;
                        fall.indices.insert(fall.indices.end(), { a, b, c, b, d, c, a, c, b, b, c, d });
                    }
                    if (w.foam) {
                        // White water where it lands.
                        MeshGroup& foam = piece.Group(kFoamTexture);
                        const float reach = std::max(4.0f, w.width * 1.5f);
                        const float side = stream + std::max(0.0f, w.shore) * 0.5f;
                        const Vec3 nearEdge = Vec3{ s.p.x, s.p.y, s.p.z + 0.04f } - s.forward * 0.5f;
                        const Vec3 farEdge = nearEdge + s.forward * reach;
                        const Vec3 corners[4] = { nearEdge - right * side, nearEdge + right * side, farEdge - right * side,
                                                  farEdge + right * side };
                        const u32 at = (u32)foam.vertices.size();
                        for (const Vec3& q : corners) AddVertex(foam, q, up, { q.x / 8.0f, -q.y / 8.0f });
                        foam.indices.insert(foam.indices.end(), { at, at + 2, at + 1, at + 1, at + 2, at + 3 });
                    }
                } else if (s.steep && w.foam) {
                    // Rapids: white water over the steep stretch.
                    const StreamPoint& p = path[i - 1];
                    const Vec3 pr{ p.forward.y, -p.forward.x, 0.0f };
                    MeshGroup& foam = piece.Group(kFoamTexture);
                    const u32 at = (u32)foam.vertices.size();
                    AddVertex(foam, p.p - pr * stream + up * 0.03f, up, { -stream / 8.0f, -p.along / 8.0f });
                    AddVertex(foam, p.p + pr * stream + up * 0.03f, up, { stream / 8.0f, -p.along / 8.0f });
                    AddVertex(foam, s.p - right * stream + up * 0.03f, up, { -stream / 8.0f, -s.along / 8.0f });
                    AddVertex(foam, s.p + right * stream + up * 0.03f, up, { stream / 8.0f, -s.along / 8.0f });
                    foam.indices.insert(foam.indices.end(), { at, at + 2, at + 1, at + 1, at + 2, at + 3 });
                }
            }
            if (MeshGroup* foam = piece.groups.empty() ? nullptr : &piece.Group(kFoamTexture))
                for (size_t k = 0; k + 2 < foam->indices.size(); k += 3) {
                    const Vec3& p0 = foam->vertices[foam->indices[k]].position;
                    const Vec3& p1 = foam->vertices[foam->indices[k + 1]].position;
                    const Vec3& p2 = foam->vertices[foam->indices[k + 2]].position;
                    if (Cross(p1 - p0, p2 - p0).z < 0.0f) std::swap(foam->indices[k + 1], foam->indices[k + 2]);
                }
        }
        piece.groups.erase(std::remove_if(piece.groups.begin(), piece.groups.end(),
                                          [](const MeshGroup& g) { return g.indices.empty(); }),
                           piece.groups.end());
        if (piece.VertexCount() || !piece.areas.empty() || !piece.strips.empty()) pieces.push_back(std::move(piece));
    }
}

void RoadField::AddWaters(const Network& network) {
    bool added = false;
    for (const Water& w : network.Waters()) {
        if (!w.carve || w.points.size() < 2) continue;
        // A lake is never carved: the ground runs on under its flat surface.
        if (w.kind == Water::kLake) continue;
        {
            const std::vector<StreamPoint> path = StreamPath(w);
            const float half = std::max(0.5f, w.width * 0.5f);
            const float depth = std::max(0.2f, w.depth);
            for (size_t i = 1; i < path.size(); ++i) {
                if (path[i].fallBefore) continue;
                samples_.push_back({ path[i - 1].p - Vec3{ 0, 0, depth }, path[i].p - Vec3{ 0, 0, depth }, half });
            }
            added = true;
        }
    }
    if (added) Index();
}

} // namespace roads
