#pragma once

#include "core/Math.h"
#include "terrain/Terrain.h"

#include <functional>

namespace terrain {

// The ground of an empty region (nfs/CustomRegions.h), made from noise.
//
// A square `size` across centred on the start grid, cut to where the game
// streams anything at all (`inside`: a point some visibility zone holds --
// outside every zone the whole region unloads and the car would fall). Laid
// in patches of 64 x 64 cells (cell = size / 256, at least 8 m) so each bakes
// in pieces small enough to land in the section they stand over:
//   - heights: fractal value noise, `relief` metres from the start's height
//     (gentle 4, hills 30, mountains 110), flat for 80 m round the start and
//     easing in to 200 m, so the car is put down on level ground;
//   - a rim: the ground rises 45 m over the last three cells at every edge,
//     too steep to drive up -- past it there is no collision;
//   - paint: the game's grass (TRN_GRASSA_KT), with rock coming out on the slopes by itself
//     (Patch::autoRock), as beside the editor's roads.
struct ProceduralSettings {
    int   preset = 1;            // 0 gentle, 1 hills, 2 mountains
    float size = 2000.0f;
    u32   seed = 1;
    Vec3  start;                 // the start grid: the middle, and the height of the ground there
};

// Fills `out` (cleared first). False when no cell is inside.
bool GenerateGround(const ProceduralSettings& settings, const std::function<bool(float, float)>& inside,
                    Terrain& out, size_t& patches, size_t& cells);

} // namespace terrain
