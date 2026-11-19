#pragma once

#include "core/Math.h"
#include "nfs/CustomRegions.h"

#include <string>
#include <vector>

namespace io {

// Making a new region (nfs/CustomRegions.h) and keeping its own files current.
//
// A new region is a copy of a shipped one (`source`) under the first free name
// (L4RE, L4RI, ...: never L4RG, which the game gives a smaller streaming pool)
// and the first free track number from 4950. Its TrackInfo entry is the
// source's free-roam track's (4000 for L4RA: the whole-city map and its
// calibration) with the new number, region and name, and mode 0x10 so Quick
// Race lists it under Free Run.
struct NewRegion {
    std::wstring source = L"L4RA";
    std::string title;                // UTF-8; shown in Quick Race
    // Empty: none of the source's city, procedural ground instead
    // (terrain/Procedural.h). Its track has no traffic (MaxTrafficCars 0):
    // the source's road graph is kept only so a reset car has somewhere to go.
    bool empty = false;
    int  preset = 1;
    float size = 2000.0f;
    u32  seed = 1;
};

// Where the region's start grid stands (its first ForwardStartingPosition):
// an empty region's ground is flat there, at that height.
bool RegionStart(const std::wstring& gameFolder, const nfs::CustomRegion& region, Vec3& at);

// An empty region's zone lists, in a region file's bytes (`layout`): each
// zone of the visibility container (0x80034150, lists 0x34153) loads only the
// library sections (X, Y, Z) of its shipped list and the sections the editor
// wrote for the region (<models>\<R>-<SECTION>-stream.frssection), so none of
// the source's city is ever streamed in. Lists only shrink, in place: nothing
// in the file moves. Nothing happens for a region that is not empty.
bool ApplyEmptyRegionLists(std::vector<u8>& layout, const std::wstring& gameFolder, const std::wstring& region,
                           std::wstring& error);

bool CreateRegion(const std::wstring& gameFolder, const NewRegion& request, nfs::CustomRegion& made,
                  std::wstring& message);

// The files a track of its own needs, in <game>\models\files\TRACKS, made from
// the region's free-roam files as they stand now (the editor's copies when
// there are some -- baked roads, placed shops -- else the source's):
//   ROUTES<R>\Routes<n>F.bin, Routes<n>B.bin   RoutesFreeRoam.bin: Free Run is a
//                                     race of track n, and reads these for its
//                                     AI and traffic
//   ROUTES<R>\Paths<n>.bin            PathsFreeRoam.bin: the zones, traffic's
//                                     TRAFFIC_TYPE among them; no course record
//   ROUTES<R>\TrackPosMarkersAll.bin  the source's, plus the start grid of its
//                                     free-roam track renumbered n (InitCars
//                                     crashes at 0x5DA392 without one)
//   TRACKMAP<n>.BIN                   the free-roam track's map, renamed
// Run when the region is made and whenever the editor saves or starts the game.
bool SyncRegionFiles(const std::wstring& gameFolder, const nfs::CustomRegion& region, std::wstring& message);

// Removes the region's definition and every file in models\files it owns.
bool RemoveRegion(const std::wstring& gameFolder, const nfs::CustomRegion& region, std::wstring& message);

// Every track number the game or the editor already uses.
std::vector<u16> UsedTrackNumbers(const std::wstring& gameFolder);

} // namespace io
