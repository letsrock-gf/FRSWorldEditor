#pragma once

#include "core/Math.h"
#include "core/Types.h"

#include <string>
#include <vector>

namespace nfs { class World; }

namespace io {

// A whole region brought into another as sections of its own
// (docs/research/region-import.md): every section of `source` renumbered
// into letters past Z (the loader takes them as positioned sections,
// loader/src/Limits.cpp), moved by `offset` and turned by `yaw` about Z, and
// streamed by visibility zones of its own.
//
// <models>\imports-<TARGET>.txt holds one line per import:
//   import <SOURCE> <first letter> <x> <y> <z> <yaw degrees> [<shown x> <y> <z> <yaw>]
// the four last only while it is shown somewhere it was not baked.
// Everything else is made again from it: the sections by BakeImport, the
// region file's table entries and zones by ApplyImports.
struct RegionImport {
    std::wstring source;          // L4RC
    int   firstLetter = 27;       // the source's first letter becomes this one (27 = '[', numbers 2700+)
    Vec3  offset;                 // where the source's origin goes
    float yawDegrees = 0.0f;      // about Z

    // Where the editor shows it, when it was moved since the bake (the
    // gizmo moves only this; a bake makes it the placement above). The
    // game always has offset/yawDegrees.
    bool  moved = false;
    Vec3  shownOffset;
    float shownYaw = 0.0f;

    Mat4 Transform() const;       // row vectors: world' = world * Transform()
    Mat4 ShownTransform() const;  // Transform() unless moved
    RegionImport Shown() const;   // the placement shown, as one to bake
    // The target number of a source section number, 0 when its letter has no room.
    u32 MapNumber(u32 number, const std::vector<int>& letters) const;
};

std::wstring ImportsPath(const std::wstring& modelsFolder, const std::wstring& target);
bool ReadImports(const std::wstring& modelsFolder, const std::wstring& target, std::vector<RegionImport>& out);
bool WriteImports(const std::wstring& modelsFolder, const std::wstring& target, const std::vector<RegionImport>& list);

// The letters (1..26) the source region's sections use, in order: the k-th
// becomes firstLetter + k.
std::vector<int> SourceLetters(const std::wstring& gameFolder, const std::wstring& source);

// The centres of a region's sections on the ground (its own letters, not the
// model libraries), from its table.
std::vector<Vec2> SectionCentres(const std::wstring& gameFolder, const std::wstring& region);

// The game's collision holds plan positions in eighths of a metre in an i16:
// whether every section of the import, moved, stays inside +/-4096 (less what
// a section's ground reaches past its centre).
bool InsideCollisionRange(const std::vector<Vec2>& sourceCentres, const RegionImport& import);

// The place inside the collision range, and the turn, that leaves the most room
// between the source's sections and those already there, for a first import.
// firstLetter is 0 when the sections of either region could not be read.
RegionImport SuggestImport(const std::wstring& gameFolder, const std::wstring& target, const std::wstring& source);

// Writes every section of the import as a span of the target's stream, past
// the end of everything there (<models>\<TARGET>-<number>-stream.frssection),
// replacing the spans an earlier bake of it wrote, then the region file
// (ApplyImports, through io/DistrictFog's writer). `target` is the open world.
bool BakeImport(const nfs::World& target, const std::wstring& gameFolder, const RegionImport& import,
                std::wstring& message);

// Takes the import's spans out again.
bool RemoveImportSpans(const std::wstring& gameFolder, const std::wstring& target, const RegionImport& import);

// The region file's part (`layout` = the target's region file bytes): the
// section table grows at its end with an entry per imported section (the
// entries before it keep their offsets, so the editor's table spans stay
// true), and the visibility container moves to the end of the file -- the
// shipped one becomes padding of its size -- with the zone-list spans folded
// in and the imports' zone ids, outlines and lists added. What an earlier call
// added is taken out first. Nothing changes without imports.
bool ApplyImports(std::vector<u8>& layout, const std::wstring& gameFolder, const std::wstring& target,
                  std::wstring& error);

} // namespace io
