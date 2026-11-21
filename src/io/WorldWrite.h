#pragma once

#include "nfs/World.h"

#include <string>

// Writing edits of the map's own scenery into NFS World.
//
// A section is a file of its own there (TRACKS\STREAML5RA_<n>.BUN), and
// removing or moving a shipped object changes bytes inside its 96-byte record
// without changing any size, so the file is patched where it stands and the
// section table in L5RA.BUN is left alone.
//
// The shipped file is kept before the first change, in TRACKS\frs-original,
// and the editor always reads that copy: what it shows is the game as it
// shipped plus the list of edits, which lives beside the copies as
// <region>-edits.txt. A section whose edits are all undone gets its shipped
// file back and loses its copy.
namespace io {

struct WorldWriteReport {
    size_t written  = 0;   // section files patched
    size_t restored = 0;   // section files put back as shipped
    size_t removed  = 0;
    size_t moved    = 0;
    size_t added    = 0;   // newly placed objects written as scenery records
    size_t meshes   = 0;   // edited models written into the file that stores them
    size_t collision = 0;  // collision polygons taken out of their packs
    size_t collisionAdded = 0;    // collision polygons written into a pack
    size_t collisionSkipped = 0;  // added ones outside every collision box: not in the game
    std::wstring error;
};

// Reads the edit list and applies it to the loaded sections: removed records
// are marked, moved ones take their new transform. Returns how many applied.
size_t LoadWorldEdits(nfs::World& world, const std::wstring& tracksFolder);

// Writes the edit list and brings every changed section file in line with it.
// Refuses while nfsw.exe runs: the game holds these files open.
bool WriteWorldEdits(nfs::World& world, const std::wstring& tracksFolder, WorldWriteReport& report);

bool GameIsRunning();

// The folder the edit list and the kept files live in (TRACKS\frs-original,
// or under FRS_WRITE_TRACKS for scripted runs). Not made here.
std::wstring WorldEditsFolder(const std::wstring& tracksFolder);

// --- streaming zones ------------------------------------------------------------
// The drivable section boundaries of the region as shipped (L5RA.BUN chunk
// 0x34152 in 0x80034150; nfsw.exe 0x7471f0 loads them). Record: u32 11, u32 11,
// u16 section, u8 point count (a signed byte to the game: at most 127), u8 flag (0 or 1), 12 bytes,
// min x/y, max x/y, centre x/y, then the points (float x, y: game axes).
// Drivable: letter (section / 1000) up to 19 and 1 <= section % 1000 < the
// first word of 0x34151 (400). The streamer looks the car up in these
// (0x741ba0: inside a polygon, or within 0.1 of one); outside all of them the
// zone is 0 and no section of the map is streamed (0x742c20, 0x742e00).
struct WorldZone {
    u32 section = 0;
    std::vector<Vec2> points;
};
std::vector<WorldZone> ReadWorldZones(const std::wstring& tracksFolder, const std::wstring& region);

// --- textures -------------------------------------------------------------------
// A texture is replaced where it stands: the image is encoded to each stored
// copy's own size, format and mip levels -- the pack's and every larger level
// kept apart -- so no file changes size. The image is kept in
// TRACKS\frs-original\textures and named in the edit list.

// The folder replacement images are kept in, made if need be.
std::wstring KeptTextureFolder(const std::wstring& tracksFolder);

// Encodes `imagePath` over every stored copy of the texture and marks those
// sections to be written. Returns how many copies took it; `error` says why
// when none did.
size_t StageTextureReplacement(nfs::World& world, u32 hash, const std::wstring& imagePath,
                               std::wstring& error);

// The folder edited meshes are kept in (TRACKS\frs-original\meshes), made if
// need be. See io/WorldMesh.h.
std::wstring KeptMeshFolder(const std::wstring& tracksFolder);

// Takes a staged replacement back out, so the shipped pixels show and are
// written again.
void UnstageTextureReplacement(nfs::World& world, u32 hash);

} // namespace io
