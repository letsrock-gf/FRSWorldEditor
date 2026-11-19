#include "io/CollisionGenerate.h"

#include "io/CollisionEdit.h"
#include "nfs/FreeRoam.h"
#include "nfs/SolidList.h"
#include "nfs/World.h"

#include <algorithm>
#include <cctype>
#include <cmath>

namespace collision {

namespace {

bool NameHas(const std::string& upper, const char* word) { return upper.find(word) != std::string::npos; }

u8 WallSurfaceFor(const std::string& name) {
    std::string n = name;
    for (char& c : n) c = (char)std::toupper((unsigned char)c);
    // Words of the name (XO_STREETLIGHTC_1A_00 -> XO, STREETLIGHTC, 1A, 00):
    // a plant has to start one, or every STREET would be a tree.
    const auto startsWord = [&n](const char* word) {
        for (size_t at = n.find(word); at != std::string::npos; at = n.find(word, at + 1))
            if (at == 0 || n[at - 1] == '_') return true;
        return false;
    };
    if (NameHas(n, "POLE") || NameHas(n, "POST") || NameHas(n, "LIGHT") || NameHas(n, "LAMP") ||
        NameHas(n, "SIGN") || NameHas(n, "HYDRANT") || NameHas(n, "METER"))
        return 23;   // POST
    if (startsWord("TREE") || startsWord("BUSH") || startsWord("PLANT") || startsWord("HEDGE") ||
        startsWord("PALM"))
        return 22;   // PLANT
    if (NameHas(n, "FENCE") || NameHas(n, "CHAIN")) return 27;   // CHAINLINK
    if (NameHas(n, "GLASS") || NameHas(n, "WINDOW")) return 19;  // GLASS
    if (NameHas(n, "WOOD") || NameHas(n, "CRATE") || NameHas(n, "PALLET")) return 17;   // WOOD
    if (NameHas(n, "PILLAR") || NameHas(n, "COLUMN")) return 24;   // PILLAR
    return nfs::kSurfaceSolidWall;
}

} // namespace

GenerateResult GenerateObjectCollision(const nfs::SolidObject& model, const Mat4& transform,
                                       const nfs::World* world,
                                       std::vector<nfs::CollisionPolygon>& out) {
    GenerateResult result;
    const auto& vertices = model.vertices;
    const auto& indices = model.indices;
    if (vertices.empty() || indices.size() < 3) {
        result.note = L"the model has no mesh";
        return result;
    }

    // --- the mesh in the world ------------------------------------------------
    std::vector<Vec3> points(vertices.size());
    Aabb box;
    for (size_t i = 0; i < vertices.size(); ++i) {
        points[i] = transform.TransformPoint(vertices[i].position);
        box.Add(points[i]);
    }
    const float minZ = box.min.z, maxZ = box.max.z;
    const Vec3 centre = (box.min + box.max) * 0.5f;

    // The foot: the ground under the middle when the object stands on it or
    // is sunk into it, else its own lowest point.
    float foot = minZ;
    float ground = 0.0f;
    if (world && nfs::GroundBelow(*world, centre.x, centre.y, minZ + 1.0f, ground) &&
        ground > minZ - 3.0f && ground < maxZ)
        foot = std::max(minZ, ground);
    const float height = maxZ - foot;

    // --- which way is up for this mesh's triangles ------------------------------
    // The winding is the renderer's business, so the sign of a face normal is
    // settled by a vote: against the stored vertex normals when the model has
    // them, else by the highest faces, which have to face up.
    struct Face { Vec3 a, b, c; float nz; };
    std::vector<Face> faces;
    faces.reserve(indices.size() / 3);
    double vote = 0.0;
    for (size_t t = 0; t + 2 < indices.size(); t += 3) {
        const u16 i0 = indices[t], i1 = indices[t + 1], i2 = indices[t + 2];
        if (i0 >= points.size() || i1 >= points.size() || i2 >= points.size()) continue;
        const Vec3 &a = points[i0], &b = points[i1], &c = points[i2];
        const Vec3 n = Cross(b - a, c - a);
        const float length = Length(n);
        if (length < 1e-5f) continue;
        Face face{ a, b, c, n.z / length };
        if (model.hasNormals) {
            const Vec3 stored = transform.TransformDirection(vertices[i0].normal + vertices[i1].normal +
                                                             vertices[i2].normal);
            vote += Dot(n, stored) >= 0.0f ? length : -length;
        }
        faces.push_back(face);
    }
    if (!model.hasNormals) {
        const float top = maxZ - std::max(0.1f, (maxZ - minZ) * 0.1f);
        for (const Face& f : faces)
            if (std::min({ f.a.z, f.b.z, f.c.z }) >= top && std::fabs(f.nz) > 0.7f) vote += f.nz;
    }
    const float up = vote < 0.0 ? -1.0f : 1.0f;

    // --- solid or drivable --------------------------------------------------------
    bool drivable = height <= 0.6f;
    for (const Face& f : faces) {
        const float nz = f.nz * up;
        if (nz <= 0.6f || nz >= 0.98f) continue;
        const float low = std::min({ f.a.z, f.b.z, f.c.z }), high = std::max({ f.a.z, f.b.z, f.c.z });
        if (low <= foot + 0.6f && high >= foot + 0.3f) { drivable = true; break; }
    }
    result.shape = drivable ? GeneratedShape::Drivable : GeneratedShape::Solid;

    // --- collision on the mesh's own faces ----------------------------------------
    // Every face becomes a polygon where it stands, at its own angle: the ones
    // that look up are ground a car drives on, the rest are sides it hits. No
    // outline is drawn round the object. Faces that look down (undersides,
    // ceilings) are left out: stored as ground they would hold a car up from
    // below.
    const u8 sideSurface = WallSurfaceFor(model.name);
    for (const Face& f : faces) {
        const float nz = f.nz * up;
        if (nz < -0.3f) continue;
        if (result.ground + result.walls >= kMaxGround) { result.groundCapped = true; break; }
        const Vec3 corners[3] = { f.a, f.b, f.c };
        if (nz > 0.6f) {
            out.push_back(MakeGround(corners, 3, nfs::kSurfaceRoad));
            ++result.ground;
            continue;
        }
        nfs::CollisionPolygon side = MakeGround(corners, 3, sideSurface);
        out.push_back(side);
        ++result.walls;
        // An upright face has no up side to be stored by, so it is stored
        // both ways round and stops a car from either.
        if (side.Vertical()) {
            nfs::CollisionPolygon twin = side;
            twin.corners[1] = side.corners[2];
            twin.corners[2] = twin.corners[3] = side.corners[1];
            out.push_back(twin);
        }
    }    if (!result.ground && !result.walls) result.note = L"no face of it can be hit or driven on";
    return result;
}
} // namespace collision
