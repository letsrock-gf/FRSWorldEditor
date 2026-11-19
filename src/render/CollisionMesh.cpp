#include "render/CollisionMesh.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <vector>

namespace {

struct CollisionVertex {
    float    x, y, z;
    D3DCOLOR colour;
};

// Collision lies almost exactly on the road surface. Lifting it a hair keeps
// it from z-fighting with the render mesh without visibly floating.
constexpr float kLift = 0.05f;

constexpr D3DCOLOR kBarrierColour   = 0x90FF3030;
// The side of a barrier or wall that does not hold cars (NFS World barriers
// block on one side only: to the right of their first end towards the second,
// seen from above -- the road lies there for 5476 of 5510 shipped barriers).
constexpr D3DCOLOR kBackColour      = 0x903C78FF;
// Each side is its own copy, wound to face its way and drawn with culling, so
// only the side facing the camera is drawn (translucent, the two would mix).
constexpr D3DCOLOR kRaceColour      = 0x80FF9A20;   // a race barrier group: absent in free roam
constexpr D3DCOLOR kGroundOutline   = 0xFF101010;
constexpr D3DCOLOR kBarrierOutline  = 0xFFFFD040;
constexpr D3DCOLOR kVerticalOutline = 0xFFFF8040;
constexpr D3DCOLOR kRaceOutline     = 0xFFFFE080;

D3DCOLOR Scale(u32 rgb, float shade, u32 alpha) {
    auto channel = [&](int shift) {
        const float v = (float)((rgb >> shift) & 0xFF) * shade;
        return (u32)(v > 255.0f ? 255.0f : v);
    };
    return D3DCOLOR_ARGB(alpha, channel(16), channel(8), channel(0));
}

} // namespace

u32 CollisionMesh::SurfaceRgb(u8 surface) {
    switch (surface) {
        case 1:  return 0x8C8C96;   // ROAD
        case 2:  return 0x6E7A96;   // ROAD_WET
        case 3: case 4: case 5: case 6: return 0x9C8CB4;   // drift / smoke
        case 7:  return 0x7890B4;   // ROAD_BRIDGE
        case 8:  return 0xA0784A;   // DIRT
        case 9:  return 0xB4A07A;   // GRAVEL
        case 10: return 0x8C7860;   // ROUGH_ROAD
        case 11: return 0xA08C78;   // COBBLESTONE
        case 12: return 0xC8B48C;   // STAIRS
        case 13: return 0x50B4DC;   // PUDDLE
        case 14: return 0x2864DC;   // DEEP_WATER
        case 15: return 0x50B446;   // GRASS
        case 16: return 0xE6DCB4;   // SIDEWALK
        case 17: return 0xC88232;   // WOOD
        case 18: return 0xB45AC8;   // PLASTIC
        case 19: return 0x96E6F0;   // GLASS
        case 20: return 0xC84646;   // SOLID_WALL
        case 21: return 0xDC7878;   // SEE_THROUGH_WALL
        case 22: return 0x3C963C;   // PLANT
        case 23: case 24: return 0xB4B4B4;   // POST, PILLAR
        case 25: case 26: return 0x8CAAC8;   // METAL_GRATE, METAL
        case 27: return 0xA0A0C8;   // CHAINLINK
        default: return 0xF0F040;
    }
}

namespace {

void Push(std::vector<CollisionVertex>& out, const Vec3& p, D3DCOLOR colour) {
    out.push_back({ p.x, p.y, p.z + kLift, colour });
}

} // namespace

CollisionMesh::~CollisionMesh() {
    Release();
}

void CollisionMesh::Release() {
    for (Part& part : parts_)
        if (part.buffer) part.buffer->Release();
    parts_.clear();
    polygonCount_ = barrierCount_ = 0;
    bounds_ = {};
}

void CollisionMesh::Sum() {
    polygonCount_ = barrierCount_ = 0;
    bounds_ = {};
    for (const Part& part : parts_) {
        polygonCount_ += part.polygons;
        barrierCount_ += part.barriers;
        bounds_.Add(part.bounds);
    }
}

void CollisionMesh::Build(IDirect3DDevice9* device, const nfs::CollisionPack& pack, bool showRaceGroups,
                          Part& part) {
    if (part.buffer) part.buffer->Release();
    part = {};

    std::vector<CollisionVertex> surfaces, sided, lines;
    pack.ResolveBarriers();   // barrier heights, chain by chain

    for (const nfs::CollisionPolygon& polygon : pack.polygons) {
        ++part.polygons;
        const Vec3* p = polygon.corners;
        const bool race = polygon.raceGroup != 0;
        if (race && !showRaceGroups) continue;

        if (polygon.barrier) {
            ++part.barriers;
            Vec3 wall[4];
            pack.BarrierQuad(polygon, wall);
            const Vec3 &a0 = wall[0], &a1 = wall[1], &b1 = wall[2], &b0 = wall[3];

            const D3DCOLOR fill = race ? kRaceColour : kBarrierColour;
            const D3DCOLOR edge = race ? kRaceOutline : kBarrierOutline;
            // a0 a1 b1 faces away from the holding side (left of a -> b).
            for (const Vec3& v : { a0, b1, a1, a0, b0, b1 }) Push(sided, v, fill);
            for (const Vec3& v : { a0, a1, b1, a0, b1, b0 }) Push(sided, v, kBackColour);
            for (const Vec3& v : { a1, b1, a0, b0 })         Push(lines, v, edge);

            for (const Vec3& v : wall) part.bounds.Add(v);
            continue;
        }

        // Shaded by slope against a light from above and to one side, so a
        // ramp or a camber stands out from the flat road around it.
        const Vec3 normal = Normalize(Cross(p[1] - p[0], p[2] - p[0]));
        const Vec3 light  = Normalize(Vec3{ 0.4f, 0.3f, 1.0f });
        const float shade = 0.45f + 0.55f * std::fabs(Dot(normal, light));
        const D3DCOLOR colour = race ? kRaceColour
                              : Scale(SurfaceRgb(polygon.surface), shade, polygon.Vertical() ? 0xC0 : 0xA0);
        const D3DCOLOR outline = race ? kRaceOutline
                               : polygon.Vertical() ? kVerticalOutline : kGroundOutline;

        // A face written as a barrier (upright or steeper than |normal z| 0.6,
        // nfs::WallLike) holds cars on its front only: the back is drawn blue.
        // Lower than half a metre it is ground to drive over (a bump, a kerb).
        float low = p[0].z, high = p[0].z;
        for (int c = 1; c < (polygon.quad ? 4 : 3); ++c) { low = std::min(low, p[c].z); high = std::max(high, p[c].z); }
        const bool wallLike = high - low >= 0.5f && (polygon.Vertical() || std::fabs(normal.z) <= 0.6f);
        std::vector<CollisionVertex>& out = wallLike ? sided : surfaces;
        for (int c : { 0, 1, 2 }) Push(out, p[c], colour);
        if (polygon.quad)
            for (int c : { 0, 2, 3 }) Push(out, p[c], colour);
        if (wallLike) {
            for (int c : { 0, 2, 1 }) Push(sided, p[c], kBackColour);
            if (polygon.quad)
                for (int c : { 0, 3, 2 }) Push(sided, p[c], kBackColour);
        }

        const int corners = polygon.quad ? 4 : 3;
        for (int c = 0; c < corners; ++c) {
            Push(lines, p[c], outline);
            Push(lines, p[(c + 1) % corners], outline);
            part.bounds.Add(p[c]);
        }
    }
    if (surfaces.empty() && sided.empty()) return;

    const u32 surfaceVertices = (u32)surfaces.size(), sidedVertices = (u32)sided.size();
    const u32 lineVertices = (u32)lines.size();
    surfaces.insert(surfaces.end(), sided.begin(), sided.end());
    surfaces.insert(surfaces.end(), lines.begin(), lines.end());
    const UINT bytes = (UINT)(surfaces.size() * sizeof(CollisionVertex));
    if (FAILED(device->CreateVertexBuffer(bytes, D3DUSAGE_WRITEONLY, kFvf, D3DPOOL_MANAGED,
                                          &part.buffer, nullptr)))
        return;
    void* target = nullptr;
    if (FAILED(part.buffer->Lock(0, bytes, &target, 0))) {
        part.buffer->Release();
        part.buffer = nullptr;
        return;
    }
    std::memcpy(target, surfaces.data(), bytes);
    part.buffer->Unlock();
    part.surfaceVertices = surfaceVertices;
    part.sidedVertices   = sidedVertices;
    part.lineVertices    = lineVertices;
}

bool CollisionMesh::Upload(IDirect3DDevice9* device, const nfs::LoadedSection& section,
                           bool showRaceGroups) {
    Release();
    if (section.collision.empty()) return false;

    parts_.resize(section.collision.size());
    bool any = false;
    for (size_t p = 0; p < parts_.size(); ++p) {
        sectionNumber_ = section.collision[p].sectionNumber;
        Build(device, section.collision[p], showRaceGroups, parts_[p]);
        any = any || parts_[p].buffer;
    }
    Sum();
    return any;
}

bool CollisionMesh::UploadPack(IDirect3DDevice9* device, const nfs::LoadedSection& section, size_t pack,
                               bool showRaceGroups) {
    if (parts_.size() != section.collision.size() || pack >= parts_.size()) return false;
    Build(device, section.collision[pack], showRaceGroups, parts_[pack]);
    Sum();
    return true;
}

void CollisionMesh::DrawSurfaces(IDirect3DDevice9* device, DWORD sidedCull, const Frustum* frustum) const {
    for (const Part& part : parts_) {
        if (!part.buffer || (frustum && !frustum->Intersects(part.bounds))) continue;
        device->SetStreamSource(0, part.buffer, 0, sizeof(CollisionVertex));
        if (part.surfaceVertices) {
            device->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
            device->DrawPrimitive(D3DPT_TRIANGLELIST, 0, part.surfaceVertices / 3);
        }
        if (part.sidedVertices) {
            device->SetRenderState(D3DRS_CULLMODE, sidedCull);
            device->DrawPrimitive(D3DPT_TRIANGLELIST, part.surfaceVertices, part.sidedVertices / 3);
        }
    }
    device->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
}

void CollisionMesh::DrawOutlines(IDirect3DDevice9* device, const Frustum* frustum) const {
    for (const Part& part : parts_) {
        if (!part.buffer || !part.lineVertices || (frustum && !frustum->Intersects(part.bounds))) continue;
        device->SetStreamSource(0, part.buffer, 0, sizeof(CollisionVertex));
        device->DrawPrimitive(D3DPT_LINELIST, part.surfaceVertices + part.sidedVertices, part.lineVertices / 2);
    }
}