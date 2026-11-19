#pragma once

#include "core/Math.h"
#include "nfs/Collision.h"

#include <string>
#include <vector>

namespace nfs { class World; struct SolidObject; }

// Collision made from an object's own mesh, for the objects placed in the
// editor (and any of the map's that lack it).
//
// Every face of the mesh becomes a collision polygon where it stands, at its
// own angle; nothing is drawn round the object. Faces that look up are ground
// (ROAD), the others are sides, an upright one stored both ways round so it
// stops a car from either. Faces that look down are left out.
//
// `shape` only says what the object looks like: Drivable when an upward face
// that is not level, or a level one at the foot, reaches down to the ground.
//
// Side surfaces follow the model's name: posts and lights POST, trees and
// bushes PLANT, fences CHAINLINK, glass GLASS, wood WOOD, else SOLID_WALL.
namespace collision {

enum class GeneratedShape { Solid, Drivable };

struct GenerateResult {
    GeneratedShape shape = GeneratedShape::Solid;
    size_t walls = 0, ground = 0;   // side faces, upward faces
    bool   groundCapped = false;   // more faces than kMaxGround: the rest left out
    std::wstring note;             // why nothing was made, when nothing was
};

constexpr size_t kMaxGround = 2000;

// Appends the polygons for `model` placed by `transform`. `world` (optional)
// gives the ground under it, which sets the foot of the walls.
GenerateResult GenerateObjectCollision(const nfs::SolidObject& model, const Mat4& transform,
                                       const nfs::World* world,
                                       std::vector<nfs::CollisionPolygon>& out);

} // namespace collision
