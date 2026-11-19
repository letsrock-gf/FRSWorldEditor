#pragma once

#include "nfs/World.h"

#include <string>
#include <vector>

namespace io {

// Physical props (trash cans, pop machines, parked cars: knocked about when
// hit). Read from nfsw.exe and measured on every shipped section:
//   - section file, top-level chunk 0x34027 (loader 0x6F68F0, spawner
//     0x6F6730): after the 16-alignment, 8 bytes the game links, i16 the
//     scenery container the props belong to (@+8), i16 count (@+0xA), then 64
//     bytes a prop from +0x10: rotation quaternion x y z w (@0), position
//     (@0x10), both in collision axes (x = -game y, y = up, z = game x), three
//     hashes, id (@0x28), a hash, the record's flags (@0x30), 0, 0, runtime
//     pointer (@0x3C, 0 in the file). 432 of 439 files have one.
//   - the container's 0x34106 rows: u16 id, u16 record (loader 0x746060).
//   - L5RA.BUN chunk 0x34108 (loader 0x74602D, count = size / 6): one entry
//     per id, u16 container, u16 record, u16 flags. A row is honoured when its
//     entry names the same container and record.
// The game spawns a prop's physics and collision from 0x34027, not from the
// scenery record, so a record placed without one is scenery you drive through.
constexpr size_t kPropSize = 64;

// The prop's pose from a placement's matrix (rows = axes, scale taken out).
void SetPropPose(u8* prop, const Mat4& m);

// New entries of L5RA.BUN's 0x34108, numbered on from the shipped ones. Each
// save makes them again from the shipped files, in the same order.
struct PropIds {
    ByteSpan shipped;                       // the shipped chunk's data
    std::vector<u8> added;                  // 6 bytes an entry
    u32 Next() const { return (u32)(shipped.size / 6 + added.size() / 6); }
};

// Adds the section's editor placements to `bytes`, a copy of its file: new
// scenery records, with everything the game needs to draw them, and a prop
// (with an id in `ids`) for a model the game makes physical. The bytes grow,
// by a multiple of 128. `added` counts what went in. False, with why in
// `error` and `bytes` in an unknown state, when one could not be added.
bool AppendPlacements(const nfs::World& world, const nfs::LoadedSection& loaded, std::vector<u8>& bytes,
                      size_t& added, PropIds& ids, std::wstring& error);

} // namespace io
