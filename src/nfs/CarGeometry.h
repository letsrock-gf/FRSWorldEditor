#pragma once

#include "core/Types.h"
#include "nfs/TexturePack.h"

#include <string>
#include <vector>

namespace nfs {

// NFS World's cars as models the editor can place (ui/CarsExtension.h).
//
// CARS\<NAME>\GEOMETRY.BIN is one solid list pack (0x80134000) whose object
// table (0x134004, 36 bytes a row: hash, file offset, stored size, size) points
// at each 0x80134010 object chunk, stored whole or as CIP blocks (0x55441122,
// unpacked size, block size with its 24-byte header, unpacked offset, ...; the
// block a JDLZ / HUFF / RAWW one). The objects are the world's own kind, their
// materials drawing with CarShader (effect 8, 32-byte packed vertices; see
// SolidList.cpp). A part is named <CAR>_<PART>_<LOD>, LOD A (nearest) to E.
// TEXTURES.BIN is a pack of the same CIP blocks, each ending in the world's
// texture header and format entry (TexturePack::ParseCompressed).

// The folders of CARS with a GEOMETRY.BIN, upper case, sorted.
std::vector<std::string> ListCars(const std::wstring& gameFolder);

struct CarPart {
    std::string name;
    std::vector<u8> object;   // the whole 0x80134010 chunk, as if at a 16-aligned offset
};

// The car's stock body at one LOD: <CAR>_BASE and every <CAR>_KIT00_* part,
// without drivers, damage pieces and the vinyl UV helper (*_CARSKINUV). The
// tyre and brakes are modelled at the origin; the caller puts them at the
// wheels (ecar.TireOffsets). No rim: the stock rim part is not known.
// ponytail: chosen by name, not by the car's part database (VLT).
bool ReadCarParts(const std::wstring& gameFolder, const std::string& car, char lod, std::vector<CarPart>& out,
                  std::wstring& error);

struct CarTexture {
    Texture texture;            // pixels point into `pixels`
    std::vector<u8> header;     // the world texture header, 0x90 bytes
    std::vector<u8> pixels;
};

// The textures of CARS\<car>\TEXTURES.BIN and the shared CARS\TEXTURES.BIN.
bool ReadCarTextures(const std::wstring& gameFolder, const std::string& car, std::vector<CarTexture>& out,
                     std::wstring& error);

// The light material (chunk 0x135200, 252 bytes) a placed car's paint is: a
// copy of the game's REGPAINT (gloss) or METPAINT (metallic), its diffuse
// colours set to `rgb`; without gloss, the environment map is turned off as
// the game's MATTEPAINT does. `name` gives its hash (bStringHash, as all 159
// of GlobalB's). nfsw.exe: loader 0x7AA100 links it into 0xCB4BD8, which the
// solid loader 0x74D550 searches (0x7AB650) for every 0x134013 entry.
std::vector<u8> CarPaintMaterial(const std::string& name, const float rgb[3], bool metallic, bool gloss);

// HLS (hue 0..360, lightness and saturation 0..1) to RGB 0..1.
void HlsToRgb(float hue, float lightness, float saturation, float rgb[3]);

// The light material hash car body paint strips name (none of GlobalB's): the
// game puts the player's paint there (CarRenderInfo 0x738300, +0x784).
constexpr u32 kCarPaintLightMaterial = 0xD6D6080Au;

} // namespace nfs
