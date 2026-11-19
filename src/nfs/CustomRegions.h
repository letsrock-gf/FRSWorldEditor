#pragma once

#include "core/Types.h"

#include <string>
#include <vector>

namespace nfs {

// Regions the editor made: new worlds the game loads beside its own.
//
// A new region starts as a copy of a shipped one under a name of its own
// (L4RE...). Nothing is copied on disk: the loader opens the source region's
// files whenever the game asks for the new name -- TRACKS\L4RE.BUN reads
// TRACKS\L4RA.BUN, STREAML4RE.BUN reads STREAML4RA.BUN, ROUTESL4RE\x reads
// ROUTESL4RA\x -- and the edits made in the new region carry its name, so they
// apply to it alone and never to the region it came from. Files of its own
// (the per-track route files, an edited region file) live in
// <game>\models\files like every other replaced file and win over the alias.
//
// It is reached as a track of its own: a TrackInfo entry (GLOBALB 0x34201) the
// loader adds at run time with the region's name in it, listed in Quick Race >
// Free Run and unlocked there (loader/src/Tracks.cpp).
//
// Each one is <game>\models\regions\<NAME>.frsregion, part of the world:
//   FRSREGION 1
//   region L4RE
//   source L4RA
//   track 4950
//   name My City
//   start <race number whose start grid it uses>
//   kind empty                      (optional: the city hidden, see `empty`)
//   terrain <preset> <size> <seed>  (an empty region's procedural ground)
//   trackinfo <0x128 bytes in hex>
struct CustomRegion {
    std::wstring name;           // L4RE
    std::wstring source;         // L4RA: the shipped region its files come from
    u16 track = 0;               // its track number, as Quick Race lists it
    u16 startFrom = 0;           // the shipped race whose start grid it starts on
    std::string title;           // UTF-8, "My City"
    std::vector<u8> trackInfo;   // 0x128 bytes

    // An empty region: none of the source's city. The editor hides every
    // object of its positioned sections and starts their collision empty;
    // a section it writes goes out that way, and the region file's zone lists
    // load only the library sections and the ones the editor wrote
    // (io/RegionCreate.h, ApplyEmptyRegionLists).
    bool empty = false;
    int  terrainPreset = 1;      // 0 gentle, 1 hills, 2 mountains
    float terrainSize = 2000.0f; // metres across
    u32  terrainSeed = 1;
};

std::wstring CustomRegionFolder(const std::wstring& gameFolder);
std::vector<CustomRegion> ReadCustomRegions(const std::wstring& gameFolder);
bool FindCustomRegion(const std::wstring& gameFolder, const std::wstring& region, CustomRegion& out);
bool WriteCustomRegion(const std::wstring& gameFolder, const CustomRegion& region);
bool DeleteCustomRegion(const std::wstring& gameFolder, const std::wstring& region);

// The shipped region whose files a region reads: itself, or the source of a
// region the editor made.
std::wstring SourceRegion(const std::wstring& gameFolder, const std::wstring& region);

// <game>\TRACKS\<relative> as shipped, for `region`: a custom region's name in
// `relative` swapped for its source's ("ROUTESL4RE\PathsFreeRoam.bin" ->
// "ROUTESL4RA\PathsFreeRoam.bin"). For a shipped region, just the path.
std::wstring ShippedTrackFile(const std::wstring& gameFolder, const std::wstring& region,
                              const std::wstring& relative);

} // namespace nfs
