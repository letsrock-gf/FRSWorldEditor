#include "io/DistrictFog.h"
#include "io/RegionCreate.h"
#include "io/RegionImport.h"
#include "nfs/CustomRegions.h"

#include "core/Log.h"
#include "nfs/FreeRoam.h"

#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>

namespace io {

namespace {

constexpr u32 kParameterMaps = 0x8003B600;
constexpr u32 kLayer         = 0x8003B601;
constexpr u32 kLayerHeader   = 0x0003B602;
constexpr u32 kFieldTypes    = 0x0003B603;
constexpr u32 kFieldOffsets  = 0x0003B604;
constexpr u32 kSets          = 0x0003B605;
constexpr u32 kTree8         = 0x0003B607;
constexpr u32 kTree16        = 0x0003B608;

// The padding chunk in front of the rebuilt maps carries this, and the offset
// of the shipped chunk it turned into padding, so a later write can take it
// all back out exactly.
constexpr char kMark[8] = { 'F', 'R', 'S', 'F', 'O', 'G', '1', 0 };
constexpr size_t kAppendAlign = 0x80;

// eDisplayFrame scales each colour byte by 0.75 (flt_785510).
constexpr float kColourScale = 0.75f;

u32 Get32(const std::vector<u8>& b, size_t at) { u32 v = 0; std::memcpy(&v, b.data() + at, 4); return v; }
void Set32(std::vector<u8>& b, size_t at, u32 v) { std::memcpy(b.data() + at, &v, 4); }
void Put32(std::vector<u8>& b, u32 v) { const size_t at = b.size(); b.resize(at + 4); Set32(b, at, v); }
void PutF(std::vector<u8>& b, float v) { u32 bits; std::memcpy(&bits, &v, 4); Put32(b, bits); }

u32 GameHash(const char* text) {
    u32 h = 0xFFFFFFFFu;
    for (const unsigned char* p = reinterpret_cast<const unsigned char*>(text); *p; ++p) h = h * 33u + *p;
    return h;
}

struct Child { u32 id; std::vector<u8> data; };

std::vector<Child> Children(const u8* data, size_t size) {
    std::vector<Child> out;
    size_t at = 0;
    while (at + 8 <= size) {
        u32 id, length;
        std::memcpy(&id, data + at, 4);
        std::memcpy(&length, data + at + 4, 4);
        if (at + 8 + length > size) break;
        out.push_back({ id, std::vector<u8>(data + at + 8, data + at + 8 + length) });
        at += 8 + (size_t)length;
    }
    return out;
}

void AppendChunk(std::vector<u8>& out, u32 id, const std::vector<u8>& data) {
    Put32(out, id);
    Put32(out, (u32)data.size());
    out.insert(out.end(), data.begin(), data.end());
}

// One parameter-map layer, as read.
struct Layer {
    u32 hash = 0;
    float box[4] = {};
    u32 sets = 0, stride = 0, fields = 0;
    std::vector<u8> types, offsets, data;
    std::vector<u16> tree;   // 4 per node; 8-bit trees widened

    bool Parse(const std::vector<u8>& layer) {
        bool header = false;
        std::vector<u8> q8, q16;
        u32 nodes = 0;
        for (const Child& c : Children(layer.data(), layer.size())) {
            if (c.id == kLayerHeader && c.data.size() >= 0x24) {
                header = true;
                hash = Get32(c.data, 0);
                std::memcpy(box, c.data.data() + 4, 16);
                nodes = Get32(c.data, 0x14);
                sets = Get32(c.data, 0x18);
                stride = Get32(c.data, 0x1C);
                fields = Get32(c.data, 0x20);
            } else if (c.id == kFieldTypes) types = c.data;
            else if (c.id == kFieldOffsets) offsets = c.data;
            else if (c.id == kSets) data = c.data;
            else if (c.id == kTree8) q8 = c.data;
            else if (c.id == kTree16) q16 = c.data;
        }
        if (!header || types.size() != 4ull * fields || offsets.size() != 4ull * fields ||
            data.size() != (size_t)sets * stride)
            return false;
        if (nodes && q8.size() == 4ull * nodes) for (u8 v : q8) tree.push_back(v);
        else if (nodes && q16.size() == 8ull * nodes) {
            tree.resize(4ull * nodes);
            std::memcpy(tree.data(), q16.data(), q16.size());
        }
        return true;
    }

    // GetParameterSetIndexFromQuadData8/16 (0x5F0930 / 0x5F0A30). Its bounds
    // test never passes, so a point outside the box walks into the edge cells.
    u32 SetAt(float x, float y) const {
        if (tree.empty()) return 0;
        float minX = box[0], minY = box[1], maxX = box[2], maxY = box[3];
        size_t n = 0;
        for (int guard = 0; guard < 64; ++guard) {
            if (n * 4 + 3 >= tree.size()) return 0;
            const u16* c = &tree[n * 4];
            if (c[0] == 0) return c[1] < sets ? c[1] : 0;
            const float mx = (minX + maxX) * 0.5f, my = (minY + maxY) * 0.5f;
            if (x < mx) {
                maxX = mx;
                if (y < my) { n = c[0]; maxY = my; } else { n = c[1]; minY = my; }
            } else {
                minX = mx;
                if (y < my) { n = c[2]; maxY = my; } else { n = c[3]; minY = my; }
            }
        }
        return 0;
    }

    float Field(u32 set, u32 field) const {
        if (set >= sets || field >= fields) return 0.0f;
        const u32 type = Get32(types, field * 4), offset = Get32(offsets, field * 4);
        if (offset + 4 > stride) return 0.0f;
        const u8* p = data.data() + (size_t)set * stride + offset;
        if (type == 0) { float v; std::memcpy(&v, p, 4); return v; }
        i32 v;
        std::memcpy(&v, p, 4);
        return (float)v;
    }

    FogLook Look(u32 set) const {
        FogLook look;
        look.strength = Field(set, 0);
        look.start = Field(set, 1);
        look.b = Field(set, 2) * kColourScale;
        look.g = Field(set, 3) * kColourScale;
        look.r = Field(set, 4) * kColourScale;
        return look;
    }

    // A set laid out as this layer's: fields 5.. (no reader on the PC) as set 0 has them.
    std::vector<u8> SetBytes(const FogLook& look) const {
        std::vector<u8> out(data.begin(), data.begin() + stride);
        const float byField[5] = { std::clamp(look.strength, 0.0f, 1.0f), std::clamp(look.start, 0.0f, 249.0f),
                                   std::clamp(look.b / kColourScale, 0.0f, 255.0f),
                                   std::clamp(look.g / kColourScale, 0.0f, 255.0f),
                                   std::clamp(look.r / kColourScale, 0.0f, 255.0f) };
        for (u32 f = 0; f < 5 && f < fields; ++f) {
            const u32 type = Get32(types, f * 4), offset = Get32(offsets, f * 4);
            if (offset + 4 > stride) continue;
            if (type == 0) std::memcpy(out.data() + offset, &byField[f], 4);
            else { const i32 v = (i32)std::lround(byField[f]); std::memcpy(out.data() + offset, &v, 4); }
        }
        return out;
    }
};

const char* const kFogLayers[3] = { "Normal Fog", "Night Normal Fog", "Night Raining Fog" };

// The top-level chunk offsets of `bytes`.
std::vector<size_t> TopChunks(const std::vector<u8>& bytes) {
    std::vector<size_t> out;
    for (size_t at = 0; at + 8 <= bytes.size();) {
        out.push_back(at);
        const size_t next = at + 8 + (size_t)Get32(bytes, at + 4);
        if (next <= at || next > bytes.size()) break;
        at = next;
    }
    return out;
}

// Takes out what an earlier ApplyFogPlan put in: the file back as it was.
void RemoveOurFog(std::vector<u8>& layout) {
    for (size_t at : TopChunks(layout)) {
        if (Get32(layout, at) != 0 || Get32(layout, at + 4) < 12 || std::memcmp(layout.data() + at + 8, kMark, 8) != 0)
            continue;
        const u32 original = Get32(layout, at + 16);
        layout.resize(at);
        if ((size_t)original + 8 <= layout.size() && Get32(layout, original) == 0) Set32(layout, original, kParameterMaps);
        return;
    }
}

// A quad tree over a square grid of set indices (n x n, n a power of two,
// row-major from the low corner): equal quarters merge into one leaf. Node 0
// is the root; children 0 = x low y low, 1 = x low y high, 2 = x high y low,
// 3 = x high y high; a node whose first child is 0 is a leaf, its set second.
bool BuildTree(const std::vector<u16>& grid, size_t n, std::vector<u16>& nodes) {
    // levels[k]: (n >> k)^2 cells, the set when the block is uniform, else 0xFFFF.
    std::vector<std::vector<u16>> levels{ grid };
    for (size_t size = n; size > 1; size /= 2) {
        const std::vector<u16>& below = levels.back();
        const size_t half = size / 2;
        std::vector<u16> up(half * half);
        for (size_t y = 0; y < half; ++y)
            for (size_t x = 0; x < half; ++x) {
                const u16 a = below[(2 * y) * size + 2 * x], b = below[(2 * y) * size + 2 * x + 1];
                const u16 c = below[(2 * y + 1) * size + 2 * x], d = below[(2 * y + 1) * size + 2 * x + 1];
                up[y * half + x] = (a != 0xFFFF && a == b && a == c && a == d) ? a : 0xFFFF;
            }
        levels.push_back(std::move(up));
    }
    nodes.clear();
    bool overflow = false;
    // Emits the node for block (x, y) of level k, returns its index.
    const auto emit = [&](auto&& self, size_t k, size_t x, size_t y) -> size_t {
        const size_t index = nodes.size() / 4;
        if (index > 0xFFFF) { overflow = true; return 0; }
        nodes.resize(nodes.size() + 4, 0);
        const size_t size = n >> k;
        const u16 value = levels[k][y * size + x];
        if (value != 0xFFFF || k == 0) {
            nodes[index * 4 + 1] = value == 0xFFFF ? 0 : value;
            return index;
        }
        const size_t children[4][2] = { { 2 * x, 2 * y }, { 2 * x, 2 * y + 1 }, { 2 * x + 1, 2 * y }, { 2 * x + 1, 2 * y + 1 } };
        for (int q = 0; q < 4; ++q) {
            const size_t child = self(self, k - 1, children[q][0], children[q][1]);
            nodes[index * 4 + (size_t)q] = (u16)child;
        }
        return index;
    };
    emit(emit, levels.size() - 1, 0, 0);
    return !overflow;
}

std::vector<u8> LayerChunk(const Layer& layer, const std::vector<std::vector<u8>>& sets, const float box[4],
                           const std::vector<u16>& tree) {
    std::vector<u8> header;
    Put32(header, layer.hash);
    for (int k = 0; k < 4; ++k) PutF(header, box[k]);
    Put32(header, (u32)(tree.size() / 4));
    Put32(header, (u32)sets.size());
    Put32(header, layer.stride);
    Put32(header, layer.fields);
    std::vector<u8> data;
    for (const auto& s : sets) data.insert(data.end(), s.begin(), s.end());
    std::vector<u8> treeBytes(tree.size() * 2);
    std::memcpy(treeBytes.data(), tree.data(), treeBytes.size());

    std::vector<u8> body;
    AppendChunk(body, kLayerHeader, header);
    AppendChunk(body, kFieldTypes, layer.types);
    AppendChunk(body, kFieldOffsets, layer.offsets);
    AppendChunk(body, kSets, data);
    AppendChunk(body, kTree16, treeBytes);
    return body;
}

std::vector<u8> ReadWhole(const std::wstring& path) {
    std::vector<u8> bytes;
    FILE* file = nullptr;
    if (::_wfopen_s(&file, path.c_str(), L"rb") != 0 || !file) return bytes;
    std::fseek(file, 0, SEEK_END);
    const long size = std::ftell(file);
    std::fseek(file, 0, SEEK_SET);
    if (size > 0) {
        bytes.resize((size_t)size);
        if (std::fread(bytes.data(), 1, bytes.size(), file) != bytes.size()) bytes.clear();
    }
    std::fclose(file);
    return bytes;
}

bool WriteWhole(const std::wstring& path, const std::vector<u8>& bytes) {
    FILE* file = nullptr;
    if (::_wfopen_s(&file, path.c_str(), L"wb") != 0 || !file) return false;
    const bool ok = std::fwrite(bytes.data(), 1, bytes.size(), file) == bytes.size();
    return std::fclose(file) == 0 && ok;
}

bool LayersOf(const std::vector<u8>& layout, size_t& chunkAt, std::vector<Child>& children) {
    chunkAt = SIZE_MAX;
    for (size_t at : TopChunks(layout))
        if (Get32(layout, at) == kParameterMaps) { chunkAt = at; break; }
    if (chunkAt == SIZE_MAX) return false;
    children = Children(layout.data() + chunkAt + 8, Get32(layout, chunkAt + 4));
    return true;
}

bool SameLook(const FogLook& a, const FogLook& b) {
    return a.strength == b.strength && a.start == b.start && a.r == b.r && a.g == b.g && a.b == b.b;
}

} // namespace

// --- the plan -----------------------------------------------------------------

bool FogPlan::Any() const {
    return std::any_of(entries.begin(), entries.end(), [](const DistrictFogEntry& e) { return e.custom; });
}

DistrictFogEntry* FogPlan::Find(u32 zoneHash) {
    for (DistrictFogEntry& e : entries) if (e.zoneHash == zoneHash) return &e;
    return nullptr;
}

const DistrictFogEntry* FogPlan::Find(u32 zoneHash) const {
    for (const DistrictFogEntry& e : entries) if (e.zoneHash == zoneHash) return &e;
    return nullptr;
}

bool FogPlan::operator==(const FogPlan& other) const {
    if (entries.size() != other.entries.size()) return false;
    for (size_t i = 0; i < entries.size(); ++i) {
        const DistrictFogEntry &a = entries[i], &b = other.entries[i];
        if (a.zoneHash != b.zoneHash || a.custom != b.custom || a.rainFollowsDay != b.rainFollowsDay ||
            !SameLook(a.day, b.day) || !SameLook(a.night, b.night) || !SameLook(a.rain, b.rain))
            return false;
    }
    return true;
}

std::wstring FogPlanPath(const std::wstring& modelsFolder, const std::wstring& region) {
    return modelsFolder + L"\\fog-" + region + L".txt";
}

bool ReadFogPlan(const std::wstring& modelsFolder, const std::wstring& region, FogPlan& out) {
    out = FogPlan{};
    std::ifstream file(FogPlanPath(modelsFolder, region));
    if (!file) return false;
    std::string line;
    while (std::getline(file, line)) {
        std::istringstream in(line);
        std::string word;
        if (!(in >> word) || word != "zone") continue;
        DistrictFogEntry e;
        std::string hash;
        int custom = 0, follows = 1;
        in >> hash >> custom >> follows;
        e.zoneHash = (u32)std::strtoul(hash.c_str(), nullptr, 16);
        e.custom = custom != 0;
        e.rainFollowsDay = follows != 0;
        for (FogLook* look : { &e.day, &e.night, &e.rain }) {
            std::string label;
            in >> label >> look->strength >> look->start >> look->r >> look->g >> look->b;
        }
        if (!in.fail()) out.entries.push_back(e);
    }
    return true;
}

bool WriteFogPlan(const std::wstring& modelsFolder, const std::wstring& region, const FogPlan& plan) {
    const std::wstring path = FogPlanPath(modelsFolder, region);
    if (plan.entries.empty()) {
        ::DeleteFileW(path.c_str());
        return true;
    }
    std::ofstream file(path, std::ios::trunc);
    if (!file) return false;
    file << "# FRS World Editor: fog by neighbourhood. zone <hash> <own fog> <rain follows day>\n"
            "# then day / night / rain: strength start r g b (colours as on screen, 0..191)\n";
    char text[512];
    for (const DistrictFogEntry& e : plan.entries) {
        std::snprintf(text, sizeof text, "zone %08X %d %d", e.zoneHash, e.custom ? 1 : 0, e.rainFollowsDay ? 1 : 0);
        file << text;
        const char* labels[3] = { "day", "night", "rain" };
        const FogLook* looks[3] = { &e.day, &e.night, &e.rain };
        for (int k = 0; k < 3; ++k) {
            std::snprintf(text, sizeof text, " %s %.3f %.1f %.1f %.1f %.1f", labels[k], looks[k]->strength,
                          looks[k]->start, looks[k]->r, looks[k]->g, looks[k]->b);
            file << text;
        }
        file << "\n";
    }
    return (bool)file;
}

FogLook LighterRain(const FogLook& day) {
    FogLook rain = day;
    rain.strength = std::max(day.strength, 0.5f);
    rain.start = std::min(day.start, 150.0f);
    // Halfway to a light grey: the game's own rain fog shows as grey 96 at
    // strength 0.5 from 200 m, and rain darkens the sky on top of it.
    rain.r = day.r * 0.5f + 140.0f * 0.5f;
    rain.g = day.g * 0.5f + 140.0f * 0.5f;
    rain.b = day.b * 0.5f + 140.0f * 0.5f;
    return rain;
}

bool FogAt(const std::vector<u8>& layoutIn, float x, float y, FogLook& day, FogLook& night, FogLook& rain) {
    std::vector<u8> layout = layoutIn;
    size_t at = 0;
    std::vector<Child> children;
    if (!LayersOf(layout, at, children)) return false;
    FogLook* outs[3] = { &day, &night, &rain };
    int found = 0;
    for (const Child& c : children) {
        if (c.id != kLayer) continue;
        Layer layer;
        if (!layer.Parse(c.data)) continue;
        for (int k = 0; k < 3; ++k)
            if (layer.hash == GameHash(kFogLayers[k])) { *outs[k] = layer.Look(layer.SetAt(x, y)); ++found; }
    }
    return found > 0;
}

// --- writing ------------------------------------------------------------------

bool ApplyFogPlan(std::vector<u8>& layout, const FogPlan& plan, const std::vector<nfs::CityZone>& zones,
                  std::wstring& error) {
    RemoveOurFog(layout);
    if (!plan.Any()) return true;

    size_t chunkAt = 0;
    std::vector<Child> children;
    if (!LayersOf(layout, chunkAt, children)) {
        error = L"The region file has no parameter maps (chunk 0x8003B600) to put the fog in.";
        return false;
    }

    // The neighbourhood of each zone with a fog of its own.
    std::vector<const DistrictFogEntry*> byZone(zones.size(), nullptr);
    for (size_t z = 0; z < zones.size(); ++z) {
        const DistrictFogEntry* e = plan.Find(zones[z].hash);
        if (e && e->custom) byZone[z] = e;
    }

    // One square box for the three layers: the day map's, grown to hold every
    // zone. Cells of box / 2^depth: 40 m over the shipped 10 240 m.
    Layer reference;
    for (const Child& c : children)
        if (c.id == kLayer && reference.Parse(c.data) && reference.hash == GameHash(kFogLayers[0])) break;
        else reference = Layer{};
    if (reference.hash != GameHash(kFogLayers[0])) {
        error = L"The region file has no \"Normal Fog\" map to start from.";
        return false;
    }
    float box[4] = { reference.box[0], reference.box[1], reference.box[2], reference.box[3] };
    for (const nfs::CityZone& z : zones) {
        box[0] = std::min(box[0], z.boundsMin.x); box[1] = std::min(box[1], z.boundsMin.y);
        box[2] = std::max(box[2], z.boundsMax.x); box[3] = std::max(box[3], z.boundsMax.y);
    }
    const float side = std::max(box[2] - box[0], box[3] - box[1]);
    box[2] = box[0] + side;
    box[3] = box[1] + side;

    std::vector<u8> rebuilt;
    size_t rebuiltLayers = 0;
    for (const Child& c : children) {
        Layer layer;
        int which = -1;
        if (c.id == kLayer && layer.Parse(c.data))
            for (int k = 0; k < 3; ++k)
                if (layer.hash == GameHash(kFogLayers[k])) which = k;
        if (which < 0 || layer.fields < 5 || layer.sets == 0) {
            AppendChunk(rebuilt, c.id, c.data);
            continue;
        }

        // The shipped sets, then one per neighbourhood with a fog of its own.
        std::vector<std::vector<u8>> sets;
        for (u32 s = 0; s < layer.sets; ++s)
            sets.emplace_back(layer.data.begin() + (ptrdiff_t)s * layer.stride,
                              layer.data.begin() + (ptrdiff_t)(s + 1) * layer.stride);
        std::vector<u16> setOfZone(zones.size(), 0xFFFF);
        for (size_t z = 0; z < zones.size(); ++z) {
            const DistrictFogEntry* e = byZone[z];
            if (!e) continue;
            const FogLook look = which == 0 ? e->day : which == 1 ? e->night
                                                     : (e->rainFollowsDay ? LighterRain(e->day) : e->rain);
            setOfZone[z] = (u16)sets.size();
            sets.push_back(layer.SetBytes(look));
        }
        if (sets.size() > 0xFFFF) { error = L"Too many fog sets."; return false; }

        std::vector<u16> tree;
        bool built = false;
        for (size_t depth = 8; depth >= 5 && !built; --depth) {
            const size_t n = (size_t)1 << depth;
            const float cell = side / (float)n;
            std::vector<u16> grid(n * n);
            for (size_t y = 0; y < n; ++y)
                for (size_t x = 0; x < n; ++x) {
                    const float px = box[0] + ((float)x + 0.5f) * cell, py = box[1] + ((float)y + 0.5f) * cell;
                    const int z = nfs::ZoneAt(zones, px, py);
                    grid[y * n + x] = (z >= 0 && setOfZone[(size_t)z] != 0xFFFF) ? setOfZone[(size_t)z]
                                                                                   : (u16)layer.SetAt(px, py);
                }
            built = BuildTree(grid, n, tree);
        }
        if (!built) { error = L"The fog map needs more nodes than the file can index."; return false; }
        AppendChunk(rebuilt, kLayer, LayerChunk(layer, sets, box, tree));
        ++rebuiltLayers;
    }
    if (!rebuiltLayers) {
        error = L"The region file's parameter maps have no fog layers.";
        return false;
    }

    // The shipped chunk becomes padding of its size; the rebuilt one goes last,
    // behind a padding chunk that marks it and aligns it.
    Set32(layout, chunkAt, 0);
    const size_t padAt = layout.size();
    size_t start = padAt + 8 + 12;
    start = (start + 8 + kAppendAlign - 1) / kAppendAlign * kAppendAlign - 8;   // the payload aligned
    std::vector<u8> pad(start - padAt, 0);
    Set32(pad, 4, (u32)(pad.size() - 8));
    std::memcpy(pad.data() + 8, kMark, 8);
    Set32(pad, 16, (u32)chunkAt);
    layout.insert(layout.end(), pad.begin(), pad.end());
    AppendChunk(layout, kParameterMaps, rebuilt);
    return true;
}

bool ApplyFogFromDisk(std::vector<u8>& layout, const std::wstring& gameFolder, const std::wstring& region,
                      std::wstring& error) {
    FogPlan plan;
    ReadFogPlan(gameFolder + L"\\models", region, plan);
    std::vector<nfs::CityZone> zones;
    if (plan.Any()) {
        std::vector<nfs::FreeRoamZone> freeRoam;
        nfs::ReadFreeRoamZones(gameFolder, region, freeRoam);
        zones = nfs::ReadCityZones(freeRoam);
    }
    // The other edit of this file made from disk: an empty region's zone lists.
    // Then whole regions brought into this one (io/RegionImport): their container goes last.
    return ApplyFogPlan(layout, plan, zones, error) && ApplyEmptyRegionLists(layout, gameFolder, region, error) &&
           ApplyImports(layout, gameFolder, region, error);
}

std::wstring RegionFileForGame(const std::wstring& gameFolder, const std::wstring& region) {
    const std::wstring copy = gameFolder + L"\\models\\files\\TRACKS\\" + region + L".BUN";
    if (::GetFileAttributesW(copy.c_str()) != INVALID_FILE_ATTRIBUTES) return copy;
    return nfs::ShippedTrackFile(gameFolder, region, region + L".BUN");
}

bool WriteFogToGame(const std::wstring& gameFolder, const std::wstring& region, std::wstring& message) {
    const std::wstring target = gameFolder + L"\\models\\files\\TRACKS\\" + region + L".BUN";
    const std::vector<u8> shipped = ReadWhole(nfs::ShippedTrackFile(gameFolder, region, region + L".BUN"));
    if (shipped.empty()) {
        message = L"Could not read TRACKS\\" + region + L".BUN.";
        return false;
    }
    std::vector<u8> layout = ReadWhole(target);
    if (layout.empty()) layout = shipped;
    std::wstring error;
    if (!ApplyFogFromDisk(layout, gameFolder, region, error)) {
        message = error;
        return false;
    }
    if (layout == shipped) {
        ::DeleteFileW(target.c_str());
        message = L"No neighbourhood has a fog of its own: the game uses its own fog maps.";
        return true;
    }
    for (const wchar_t* part : { L"\\models", L"\\models\\files", L"\\models\\files\\TRACKS" })
        ::CreateDirectoryW((gameFolder + part).c_str(), nullptr);
    if (!WriteWhole(target, layout)) {
        message = L"Could not write " + target;
        return false;
    }
    message = L"Fog by neighbourhood written to the game.";
    LOG_INFO("fog: %S written (%zu bytes)", target.c_str(), layout.size());
    return true;
}

} // namespace io
