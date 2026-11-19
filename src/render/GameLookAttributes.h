#pragma once

// Read-only reader for the two lighting classes of NFS World's VLT attribute
// database (GLOBAL/attributes.bin): "timeofdaylighting" and "sunandsky".
//
// The file is a VPAK pack of vaults. Each vault is a .vlt part (Vers, StrN,
// DepN, DatN, ExpN, PtrN chunks) and a .bin part (StrE string pool + raw data).
// Classes, collections and field definitions are found through the ExpN
// export table; pointers are stored as zero and resolved through PtrN.
// Names are not stored: classes, collections and fields are keyed by the
// Bob Jenkins lookup2 hash (initval 0xABCDEF00) of their name.

#include "../core/Types.h"

#include <string>
#include <unordered_map>
#include <vector>

namespace gamelook {

// lookup2 hash used by the VLT database for every key.
u32 VltHash(const std::string& text);

// The game's day cycle. Not stored in attributes.bin: nfsw.exe hashes these
// names at static init (code at 0xB09E00) into the table at 0xD1EB88, and
// FUN_006EAC10 steps an index 0..7 (wraps after 7) every 10800 units of its
// clock, blending with factor elapsed / 10800 (static reading, not traced live).
// When DAT_00D12040 is 1 or 2 (and this+0x500 is null) it picks "fe_main"
// (0x648D666F) instead; a zero key falls back to "default" (0xEEC2271A).
inline constexpr const char* kDayCycle[8] = {
    "day1_1", "day1_1a", "day1_2", "day1_3", "day1_4", "day1_5", "day1_5a", "day1_6",
};

struct LightingStage {
    std::string name;          // collection name, or "0x%08X" when unknown
    std::string parentName;    // empty for the root collection
    u32 key = 0;
    u32 parentKey = 0;

    float diffuse[4]  = {};    // DiffuseColour
    float ambient[4]  = {};    // AmbientColour
    float specular[4] = {};    // SpecularColour
    float fogSky[4]   = {};    // FogSkyColour
    float fogHaze[4]  = {};    // FogHazeColour
    float fogSkyScale = 0.f;        // FogSkyColourScale
    float fogHazeScale = 0.f;       // FogHazeColourScale
    float fogDistanceScale = 0.f;   // FogDistanceScale
    float fogInLightScatter = 0.f;  // FogInLightScatter
    float fogSunFalloff = 0.f;      // FogSunFalloff
    float envSkyBrightness = 0.f;   // EnvSkyBrightness
    float shadowStrength = 0.f;     // ShadowStrength

    std::string sunAndSky;     // SunAndSky RefSpec collection name
    u32 sunAndSkyKey = 0;

    // Every instance field after parent inheritance, by field hash. Values are
    // the field's raw 32-bit words reinterpreted as float (UInt32/Int32/Bool
    // and hash fields keep their bit pattern: use std::memcpy to get them back).
    std::unordered_map<u32, std::vector<float>> fields;
};

struct SunAndSkyStage {
    std::string name;
    std::string parentName;
    u32 key = 0;
    u32 parentKey = 0;

    float skyDomeRotation = 0.f;
    u32 skyDomeModel = 0;      // SkyDomeModelStruct hash
    int skyDomeStyle = 0;
    int lensFlareIndex = 0;

    // The unnamed float fields (0x036F58D1, 0x1BCB3B79, ...), by hash.
    std::unordered_map<u32, float> angles;
};

// Class-level (static) values of timeofdaylighting.
struct LightingStatics {
    bool  valid = false;
    float rate = 0.f;          // 0x98E9A723, static offset 0x14
    float unknown10 = 0.f;     // 0x36C9AF8D, static offset 0x10
    bool  debugMode = false;   // DebugMode, static offset 0x18
    std::string refA;          // 0xB916AF2D ClassRefSpec, static offset 0x00
    std::string refB;          // 0x97A4DE50 ClassRefSpec, static offset 0x08
};

// Loads <globalFolder>\attributes.bin. Stages keep the export order of the
// file (that order is not the game's day cycle). Returns false and fills
// `error` when the file or one of the classes cannot be read.
bool LoadLighting(const std::wstring& globalFolder,
                  std::vector<LightingStage>& lighting,
                  std::vector<SunAndSkyStage>& sunAndSky,
                  std::string& error,
                  LightingStatics* statics = nullptr);

} // namespace gamelook
