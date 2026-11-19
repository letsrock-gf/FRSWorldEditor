#pragma once

#include "core/Types.h"
#include "nfs/CityZones.h"

#include <string>
#include <vector>

namespace io {

// Fog by district.
//
// The game sets its fog by place through parameter maps: chunk 0x8003B600 of
// TRACKS\<region>.BUN, one 0x8003B601 layer per named map, each a list of
// parameter sets and a quad tree that gives the set under a point
// (LoaderParameterMaps 0x60B810; docs/NFSU2-FORMATS.md §5d). Three layers carry
// the fog: "Normal Fog" (day), "Night Normal Fog" and "Night Raining Fog"
// (mixed in by the rain's intensity). sub_614300 reads the set under the
// camera every frame and eases into a new one over 50 m.
//
// Here each of the city's neighbourhoods (the 0x11 zones of PathsFreeRoam.bin,
// nfs::CityZone) can be given a fog of its own for day, night and rain. A
// neighbourhood left alone keeps exactly what the shipped maps give at each
// spot, so a plan with nothing in it changes nothing.
//
// The rebuilt chunk goes at the END of the region file and the shipped one is
// turned into padding of the same size: nothing in front of it moves, so the
// visibility-zone spans (ui/ZoneTools) and the marker beacons
// (io/MarkerWrite) written into the same file stay where they are.

// One look of the fog, as it shows on screen.
struct FogLook {
    // The most fog there is (field 0): at 250 m everything is this much fog.
    // The PC never shows less than 0.4 there (dword_784564: the FOGVALUE floor
    // is min(1 - strength, 0.6)), so fog cannot be turned off -- only given
    // the colour of the sky.
    float strength = 0.5f;
    float start = 200.0f;           // metres from the camera where it begins (field 1), 0..250
    float r = 96, g = 96, b = 96;   // on screen, 0..191: each field byte is scaled by 0.75
};

struct DistrictFogEntry {
    u32  zoneHash = 0;              // nfs::CityZone::hash
    bool custom = false;            // false: the shipped fog, as it is
    bool rainFollowsDay = true;     // the rain look is the day's, lighter (LighterRain)
    FogLook day, night, rain;
};

struct FogPlan {
    std::vector<DistrictFogEntry> entries;
    bool Any() const;
    DistrictFogEntry* Find(u32 zoneHash);
    const DistrictFogEntry* Find(u32 zoneHash) const;
    bool operator==(const FogPlan& other) const;
    bool operator!=(const FogPlan& other) const { return !(*this == other); }
};

// <models>\fog-<REGION>.txt: the plan, part of the world.
std::wstring FogPlanPath(const std::wstring& modelsFolder, const std::wstring& region);
bool ReadFogPlan(const std::wstring& modelsFolder, const std::wstring& region, FogPlan& out);
bool WriteFogPlan(const std::wstring& modelsFolder, const std::wstring& region, const FogPlan& plan);

// What the region file's fog layers give at (x, y): to start a neighbourhood
// from what the game shows there. False when the file has no fog maps.
bool FogAt(const std::vector<u8>& layout, float x, float y, FogLook& day, FogLook& night, FogLook& rain);

// The rain look made from the day's: rain darkens the sky on top of its fog, so
// a dark rain fog gives a nearly black picture. Lighter, towards grey.
FogLook LighterRain(const FogLook& day);

// Takes out of `layout` (a region file's bytes) any fog this wrote before, and
// with a plan that has something in it puts the rebuilt maps in.
bool ApplyFogPlan(std::vector<u8>& layout, const FogPlan& plan, const std::vector<nfs::CityZone>& zones,
                  std::wstring& error);

// The same with the plan and the zones read from disk. Every writer of
// <models>\files\TRACKS\<region>.BUN calls it last, so the fog survives them.
bool ApplyFogFromDisk(std::vector<u8>& layout, const std::wstring& gameFolder, const std::wstring& region,
                      std::wstring& error);

// Puts the plan into <models>\files\TRACKS\<region>.BUN, which the loader
// opens in place of the shipped file: built from the copy already there (the
// marker beacons) or from the shipped file. Removed when nothing is left in it.
bool WriteFogToGame(const std::wstring& gameFolder, const std::wstring& region, std::wstring& message);

// The region file the game will read: the copy in <models>\files when there is
// one, else the shipped file.
std::wstring RegionFileForGame(const std::wstring& gameFolder, const std::wstring& region);

} // namespace io
