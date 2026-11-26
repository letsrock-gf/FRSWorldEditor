#pragma once

#include "core/Math.h"
#include "core/Types.h"
#include "nfs/World.h"

#include <d3d9.h>

#include <vector>

// The collision of one streaming section, turned into something drawable.
//
// Collision has no texture or lighting of its own, so it is drawn as flat
// colour: ground tinted by surface and shaded by slope so relief reads,
// barriers as red walls, race barrier groups in orange, and every polygon outlined so the tessellation --
// which is much coarser than the render mesh -- can be seen.
class CollisionMesh {
public:
    ~CollisionMesh();

    // `showRaceGroups` false leaves out the polygons of race barrier groups,
    // which the game only switches on during the races that use them.
    bool Upload(IDirect3DDevice9* device, const nfs::LoadedSection& section,
                bool showRaceGroups = true);

    // Builds one pack again and leaves the others as they are. False when the
    // mesh was not made from this many packs: upload the whole section then.
    bool UploadPack(IDirect3DDevice9* device, const nfs::LoadedSection& section, size_t pack,
                    bool showRaceGroups = true);

    // The colour a surface is drawn in (0xRRGGBB), for the viewport and a legend.
    static u32 SurfaceRgb(u8 surface);
    void Release();

    // Filled ground and barrier walls, then the outlines. The caller sets the
    // render states for each pass.
    // With a frustum, packs wholly outside it are skipped: NFS World keeps the
    // collision of the whole map in one section.
    // Ground without culling; the two sides of walls and barriers with
    // `sidedCull` (the world's own back-face mode), each wound to face its way.
    void DrawSurfaces(IDirect3DDevice9* device, DWORD sidedCull, const Frustum* frustum = nullptr) const;
    void DrawOutlines(IDirect3DDevice9* device, const Frustum* frustum = nullptr) const;

    const Aabb& Bounds() const { return bounds_; }
    // What one pack was drawn within, or nullptr when it is not known.
    const Aabb* PackBounds(size_t pack) const { return pack < parts_.size() ? &parts_[pack].bounds : nullptr; }
    u32 SectionNumber() const  { return sectionNumber_; }
    u32 PolygonCount() const   { return polygonCount_; }
    u32 BarrierCount() const   { return barrierCount_; }

    static constexpr DWORD kFvf = D3DFVF_XYZ | D3DFVF_DIFFUSE;
    static inline double resolveMs = 0.0;   // time spent in ResolveBarriers, for the log

private:
    u32  sectionNumber_    = 0;
    u32  polygonCount_     = 0;
    u32  barrierCount_     = 0;
    Aabb bounds_;

    // One pack, in a buffer of its own so an edit rebuilds that pack alone:
    // surfaces first, then the sided ones (walls), then the outlines.
    struct Part {
        IDirect3DVertexBuffer9* buffer = nullptr;
        u32  surfaceVertices = 0, sidedVertices = 0, lineVertices = 0;
        u32  polygons = 0, barriers = 0;
        Aabb bounds;
    };
    std::vector<Part> parts_;
    void Build(IDirect3DDevice9* device, const nfs::CollisionPack& pack, bool showRaceGroups, Part& part);
    void Sum();
};
