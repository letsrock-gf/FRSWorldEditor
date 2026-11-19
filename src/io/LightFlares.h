#pragma once

#include "core/Math.h"
#include "io/SectionSplice.h"

#include <map>
#include <string>
#include <vector>

namespace nfs { class World; }

// The street-light flares of street lights placed by the editor.
//
// The game does not take a street light's flare from its model: every section
// carries a pack of them (0x80135100, drive/fx/WorldFlaresU2.h), one per lamp,
// at the lamp's head. A street light the editor places therefore needs flares
// written into the pack of the section it stands in -- a copy of the shipped
// pack with ours added after, or a new pack where the section had none.
//
// Where a model's lamps are is read from the game: the flares of the shipped
// pack standing round each placed copy of the model, in that copy's own space.
namespace flares {

// The lamp heads of the game's street-light models, in model space, by model
// hash: from every shipped copy, the flares within reach of it (the first
// copy that has any).
std::map<u32, std::vector<Vec3>> LampOffsets(const nfs::World& world);

struct Plan {
    std::vector<SectionSpan>  spans;
    std::vector<std::wstring> superseded;
    size_t sections = 0;        // sections rebuilt
    size_t flares = 0;          // flares written
    std::wstring report;
};

// Section rebuilds giving the sections the flares at `positions` (world
// space) on top of the ones they shipped with; a section that had flares of
// the editor's and now has none goes back to its shipped pack.
bool PlanFlares(const nfs::World& world, const std::wstring& modelsFolder, const std::vector<Vec3>& positions,
                Plan& plan, std::wstring& error);

} // namespace flares
