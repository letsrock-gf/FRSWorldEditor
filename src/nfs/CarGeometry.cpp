#include "nfs/CarGeometry.h"

#include "core/Log.h"
#include "nfs/EaCompress.h"

#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>
#include <functional>
#include <iterator>

namespace nfs {
namespace {

u32 ReadU32(const std::vector<u8>& b, size_t at) {
    u32 v = 0;
    if (at + 4 <= b.size()) std::memcpy(&v, b.data() + at, 4);
    return v;
}

bool ReadFile(const std::wstring& path, std::vector<u8>& out) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return false;
    out.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    return true;
}

std::wstring CarsFolder(const std::wstring& gameFolder) { return gameFolder + L"\\CARS"; }
std::wstring Widen(const std::string& s) { return std::wstring(s.begin(), s.end()); }

// CIP blocks of `stored` bytes at `at` into `unpacked` bytes.
bool CipUnpack(const std::vector<u8>& file, size_t at, size_t stored, size_t unpacked, std::vector<u8>& out) {
    out.assign(unpacked, 0);
    const size_t end = at + stored;
    if (end > file.size()) return false;
    std::vector<u8> block;
    while (at + 24 <= end) {
        if (ReadU32(file, at) != 0x55441122u) return false;
        const u32 size = ReadU32(file, at + 4), blockSize = ReadU32(file, at + 8), offset = ReadU32(file, at + 12);
        if (blockSize < 24 || at + blockSize > end || (size_t)offset + size > unpacked) return false;
        if (!EaDecompress(ByteSpan{ file.data() + at + 24, blockSize - 24 }, block) || block.size() < size) return false;
        std::memcpy(out.data() + offset, block.data(), size);
        at += blockSize;
    }
    return at == end;
}

// The object table of a GEOMETRY.BIN: rows of hash, offset, stored size, size.
const u8* ObjectTable(const std::vector<u8>& file, size_t& rows) {
    for (size_t at = 8; at + 8 <= file.size();) {
        const u32 id = ReadU32(file, at), size = ReadU32(file, at + 4);
        if (id == 0x00134004u) { rows = size / 36; return file.data() + at + 8; }
        if (id & 0x80000000u) at += 8; else at += 8 + (size_t)size;
    }
    rows = 0;
    return nullptr;
}

std::string ObjectName(const std::vector<u8>& object) {
    // The header chunk is the object's first; its payload is 16-aligned and the name is at +0xA0.
    if (object.size() < 16 || ReadU32(object, 8) != 0x00134011u) return {};
    size_t payload = 16;
    while (payload < object.size() && object[payload] == 0x11 && (payload % 16)) ++payload;
    payload = (payload + 15) & ~size_t(15);
    const size_t end = std::min(object.size(), (size_t)16 + ReadU32(object, 12));
    std::string name;
    for (size_t i = payload + 0xA0; i < end && object[i]; ++i) name += (char)object[i];
    return name;
}

bool StockPart(const std::string& name, const std::string& car, char lod) {
    if (name.size() < car.size() + 3 || name.compare(0, car.size() + 1, car + "_") != 0) return false;
    if (name[name.size() - 2] != '_' || name.back() != lod) return false;
    const std::string part = name.substr(car.size() + 1, name.size() - car.size() - 3);
    if (part == "BASE") return true;
    if (part.rfind("KIT00_", 0) != 0) return false;
    for (const char* skip : { "DRIVER", "DAMAGE", "CARSKINUV" })
        if (part.find(skip) != std::string::npos) return false;
    return true;   // the tyre and brakes come too: io/WorldMesh.cpp BuildCarNode puts them at the wheels
}

// The world texture header (TexturePack.cpp constants for NFS World).
constexpr size_t kHeader = 0x90, kFormat = 32;

void ReadPackTextures(const std::vector<u8>& file, std::vector<CarTexture>& out) {
    for (size_t at = 0; at + 8 <= file.size();) {
        const u32 id = ReadU32(file, at), size = ReadU32(file, at + 4);
        if (id == 0x33310003u) {
            for (size_t row = at + 8; row + 36 <= at + 8 + size; row += 36) {
                const u32 offset = ReadU32(file, row + 4), stored = ReadU32(file, row + 8);
                const u32 payload = ReadU32(file, row + 12), tail = ReadU32(file, row + 16);
                std::vector<u8> bytes;
                if (tail < kHeader + 1 + kFormat || tail > payload) continue;
                if (stored == payload) bytes.assign(file.begin() + offset, file.begin() + offset + payload);
                else if (!CipUnpack(file, offset, stored, payload, bytes)) continue;
                const size_t entry = payload - tail;
                CarTexture texture;
                texture.header.assign(bytes.begin() + (std::ptrdiff_t)entry, bytes.begin() + (std::ptrdiff_t)(entry + kHeader));
                const ByteSpan h{ texture.header.data(), kHeader };
                const u8 nameLength = bytes[entry + kHeader];
                for (size_t i = 0; i < nameLength && bytes[entry + kHeader + 1 + i]; ++i)
                    texture.texture.name += (char)bytes[entry + kHeader + 1 + i];
                // ponytail: the field offsets TexturePack.cpp reads (kWorldTex*), repeated here.
                texture.texture.hash          = h.read<u32>(0x0C);
                texture.texture.width         = (u16)h.read<u32>(0x20);
                texture.texture.height        = (u16)h.read<u32>(0x24);
                texture.texture.mipCount      = std::max<u8>(h.read<u8>(0x31), 1);
                texture.texture.baseLevelSize = h.read<u32>(0x1C);
                texture.texture.alphaUsage    = (AlphaUsage)std::min<u8>(h.read<u8>(0x38), 2);
                texture.texture.alphaBlend    = (AlphaBlend)std::min<u8>(h.read<u8>(0x39), 2);
                texture.texture.fourCc        = ReadU32(bytes, payload - kFormat + 0x0C);
                switch (texture.texture.fourCc) {   // the DXT ones cars ship (TexturePack.cpp has all)
                    case 0x31545844: texture.texture.format = TextureFormat::Dxt1; break;
                    case 0x33545844: texture.texture.format = TextureFormat::Dxt3; break;
                    case 0x35545844: texture.texture.format = TextureFormat::Dxt5; break;
                    case 21:         texture.texture.format = TextureFormat::Argb8888; break;
                    default:         texture.texture.format = TextureFormat::Unknown; break;
                }
                const u32 dataSize            = h.read<u32>(0x4C);
                if (dataSize > entry || texture.texture.format == TextureFormat::Unknown) continue;
                texture.pixels.assign(bytes.begin(), bytes.begin() + dataSize);
                out.push_back(std::move(texture));
            }
        }
        if (id & 0x80000000u) at += 8; else at += 8 + (size_t)size;
    }
}

} // namespace

std::vector<std::string> ListCars(const std::wstring& gameFolder) {
    std::vector<std::string> cars;
    WIN32_FIND_DATAW found{};
    const HANDLE find = ::FindFirstFileW((CarsFolder(gameFolder) + L"\\*").c_str(), &found);
    if (find == INVALID_HANDLE_VALUE) return cars;
    do {
        if (!(found.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || found.cFileName[0] == L'.') continue;
        const std::wstring folder = CarsFolder(gameFolder) + L"\\" + found.cFileName;
        if (::GetFileAttributesW((folder + L"\\GEOMETRY.BIN").c_str()) == INVALID_FILE_ATTRIBUTES) continue;
        std::string name;
        for (const wchar_t* c = found.cFileName; *c; ++c) name += (char)::towupper(*c);
        cars.push_back(name);
    } while (::FindNextFileW(find, &found));
    ::FindClose(find);
    std::sort(cars.begin(), cars.end());
    return cars;
}

namespace {

// The plain solids of CARS\<folder>\GEOMETRY.BIN that `want` takes, in name order.
bool ReadObjects(const std::wstring& gameFolder, const std::string& folder,
                 const std::function<bool(const std::string&)>& want, std::vector<CarPart>& out, std::wstring& error) {
    out.clear();
    std::vector<u8> file;
    const std::wstring path = CarsFolder(gameFolder) + L"\\" + Widen(folder) + L"\\GEOMETRY.BIN";
    if (!ReadFile(path, file)) { error = L"cannot read " + path; return false; }
    size_t rows = 0;
    const u8* table = ObjectTable(file, rows);
    if (!table) { error = path + L" has no object table (0x134004)"; return false; }
    size_t unreadable = 0;
    for (size_t r = 0; r < rows; ++r) {
        u32 row[4];
        std::memcpy(row, table + r * 36, 16);
        const u32 offset = row[1], stored = row[2], size = row[3];
        CarPart part;
        if (stored == size) {
            if ((size_t)offset + size > file.size()) { ++unreadable; continue; }
            part.object.assign(file.begin() + offset, file.begin() + offset + size);
        } else if (!CipUnpack(file, offset, stored, size, part.object)) {
            ++unreadable;
            continue;
        }
        if (ReadU32(part.object, 0) != 0x80134010u) continue;   // not a plain solid
        part.name = ObjectName(part.object);
        if (want(part.name)) out.push_back(std::move(part));
    }
    std::sort(out.begin(), out.end(), [](const CarPart& a, const CarPart& b) { return a.name < b.name; });
    if (unreadable) LOG_WARN("%s: %zu objects did not unpack", folder.c_str(), unreadable);
    if (out.empty()) { error = L"nothing wanted in " + path; return false; }
    return true;
}

} // namespace

bool ReadCarParts(const std::wstring& gameFolder, const std::string& car, char lod, std::vector<CarPart>& out,
                  std::wstring& error) {
    if (!ReadObjects(gameFolder, car, [&](const std::string& name) { return StockPart(name, car, lod); }, out, error)) {
        error = L"no stock parts at LOD " + std::wstring(1, (wchar_t)lod) + L": " + error;
        return false;
    }
    LOG_INFO("car %s: %zu stock parts at LOD %c", car.c_str(), out.size(), lod);
    return true;
}

bool ReadCarTextures(const std::wstring& gameFolder, const std::string& car, std::vector<CarTexture>& out,
                     std::wstring& error) {
    out.clear();
    std::vector<u8> file;
    for (const std::wstring& path : { CarsFolder(gameFolder) + L"\\" + Widen(car) + L"\\TEXTURES.BIN",
                                      CarsFolder(gameFolder) + L"\\TEXTURES.BIN" })
        if (ReadFile(path, file)) ReadPackTextures(file, out);
    for (CarTexture& t : out) t.texture.pixels = ByteSpan{ t.pixels.data(), t.pixels.size() };
    if (out.empty()) { error = L"no textures read for " + Widen(car); return false; }
    return true;
}

void HlsToRgb(float hue, float lightness, float saturation, float rgb[3]) {
    const float l = std::clamp(lightness, 0.0f, 1.0f), s = std::clamp(saturation, 0.0f, 1.0f);
    const float c = (1.0f - std::fabs(2.0f * l - 1.0f)) * s;
    const float h = std::fmod(std::fmod(hue, 360.0f) + 360.0f, 360.0f) / 60.0f;
    const float x = c * (1.0f - std::fabs(std::fmod(h, 2.0f) - 1.0f));
    float r = 0, g = 0, b = 0;
    if (h < 1) { r = c; g = x; } else if (h < 2) { r = x; g = c; } else if (h < 3) { g = c; b = x; }
    else if (h < 4) { g = x; b = c; } else if (h < 5) { r = x; b = c; } else { r = c; b = x; }
    const float m = l - c / 2.0f;
    rgb[0] = r + m; rgb[1] = g + m; rgb[2] = b + m;
}

std::vector<u8> CarPaintMaterial(const std::string& name, const float rgb[3], bool metallic, bool gloss) {
    // GlobalB.lzc REGPAINTWHITE / METPAINTWHITE from +0x34 (after the name): float i at 0x34 + 4i.
    static const char* const kReg =
        "00000000000000000000000000000000000000000000000000000000000000000000803f0000803f00000000cecc4c3e000000009b99"
        "593f0000803f0000803f0000803f6866663f0000803f0000803f0000803f0000803f0000803f0000c03f00000000000000000000000000"
        "00803f0000803f0000803fcecccc3d0000803f0000803f0000803fcecccc3d0000803f00000000000000400000803f0000803f0000803f"
        "0000803f0000803f0000803f0000803fcecc4c3e0000803f00000000000000000000803f";
    static const char* const kMet =
        "00000000000000000000000000000000000000000000000000000000000000000000a0400000803f00000000000000000000000000008"
        "03f0000803f0000803f0000803f000080400000803f0000803f0000803f0000803f0000803f9b99593f00004040000000000000000000"
        "00803f0000803f0000803fcecc4c3e0000803f0000803f0000803fcecccc3e0000803f00000000000000400000803f0000803f0000803f"
        "0000803f0000803f0000803f0000803fcecc4c3e0000803f00000000000000000000803f";
    std::vector<u8> out(252, 0);
    const u32 head[5] = { 0, 3, 3, 0, 4 };
    std::memcpy(out.data(), head, sizeof head);
    u32 hash = 0xFFFFFFFFu;
    for (char c : name) hash = hash * 33 + (u8)c;
    std::memcpy(out.data() + 0x0C, &hash, 4);
    std::memcpy(out.data() + 0x14, name.data(), std::min<size_t>(name.size(), 31));
    const char* hex = metallic ? kMet : kReg;
    for (size_t i = 0; i < 252 - 0x34 && hex[2 * i]; ++i) {
        const auto nibble = [](char c) { return (u8)(c <= '9' ? c - '0' : c - 'a' + 10); };
        out[0x34 + i] = (u8)(nibble(hex[2 * i]) << 4 | nibble(hex[2 * i + 1]));
    }
    const auto put = [&](size_t index, float value) { std::memcpy(out.data() + 0x34 + 4 * index, &value, 4); };
    for (int c = 0; c < 3; ++c) { put(14 + c, rgb[c]); put(18 + c, rgb[c]); }   // diffuse min / max colour
    if (!gloss) { put(34, 0.0f); put(37, 0.0f); put(41, 0.0f); }                 // envmap power, min, max scale
    return out;
}

} // namespace nfs
