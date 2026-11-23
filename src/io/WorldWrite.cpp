#include "io/WorldWrite.h"

#include "core/Log.h"
#include "nfs/ChunkReader.h"
#include "io/TextureImport.h"
#include "io/WorldMesh.h"
#include "io/WorldPlace.h"
#include "io/WorldTexture.h"
#include "nfs/TextureDecode.h"

#include <windows.h>
#include <tlhelp32.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <tuple>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace io {

namespace {

// --- record layouts, as measured (see nfs/Scenery.cpp) ---------------------------
constexpr size_t kRecordSize       = 96;
constexpr size_t kRecordBoundsMin  = 0x00;
constexpr size_t kRecordBoundsMax  = 0x10;
constexpr size_t kRecordTranslation = 0x20;
constexpr size_t kRecordRotation   = 0x2C;   // 3x3 of float

constexpr size_t kHeaderNumber     = 0x0C;

// 0x3410E: one box per row of the link table, min and max each with a fourth
// float that is left as it is.
constexpr size_t kBoxRowSize = 32;

constexpr size_t kLinkRowSize  = 32;

// 0x34102: the models a container places, 72 bytes each: four levels of
// detail by hash, and the radius the game keeps for the model.
constexpr size_t kInfoSize   = 72;
constexpr size_t kInfoLevels = 0x18;
constexpr size_t kInfoRadius = 0x38;
constexpr size_t kRecordInfo = 0x54;   // i16
constexpr size_t kLinkInstance = 0x10;

// 0x34105: the tree the game culls a section's scenery with. 48 bytes a node:
// its box, a child count, and the children as 16-bit numbers -- a row of the
// tables above, or minus the index of another node.
constexpr size_t kNodeSize     = 48;
constexpr size_t kNodeMin      = 0x00;
constexpr size_t kNodeCount    = 0x0C;
constexpr size_t kNodeMax      = 0x10;
constexpr size_t kNodeChildren = 0x1C;
constexpr u32    kNodeMaxChildren = 10;

const wchar_t* kBackupFolder = L"\\frs-original";

std::wstring EditsPath(const std::wstring& tracks, const nfs::World& world) {
    return tracks + kBackupFolder + L"\\" + world.Region() + L"-edits.txt";
}

std::vector<u8> ReadWhole(const std::wstring& path) {
    std::vector<u8> out;
    HANDLE file = ::CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                                OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return out;
    LARGE_INTEGER size{};
    ::GetFileSizeEx(file, &size);
    out.resize((size_t)size.QuadPart);
    DWORD read = 0;
    if (!::ReadFile(file, out.data(), (DWORD)out.size(), &read, nullptr) || read != out.size()) out.clear();
    ::CloseHandle(file);
    return out;
}

bool Exists(const std::wstring& path) {
    return ::GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES;
}

// Where the files go. FRS_WRITE_TRACKS sends them to another folder, for
// checking what is written without touching the game.
std::wstring TargetFolder(const std::wstring& tracks, bool& redirected) {
    wchar_t other[MAX_PATH] = {};
    redirected = ::GetEnvironmentVariableW(L"FRS_WRITE_TRACKS", other, MAX_PATH) != 0;
    return redirected ? std::wstring(other) : tracks;
}

bool WriteWhole(const std::wstring& path, const u8* data, size_t size) {
    // Written beside the target and moved over it, so a failed write never
    // leaves half a section where the game will look for a whole one.
    const std::wstring temporary = path + L".frs-new";
    HANDLE file = ::CreateFileW(temporary.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                                FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    size_t done = 0;
    bool ok = true;
    while (ok && done < size) {
        DWORD wrote = 0;
        const DWORD want = (DWORD)std::min<size_t>(size - done, 1u << 24);
        ok = ::WriteFile(file, data + done, want, &wrote, nullptr) && wrote == want;
        done += wrote;
    }
    ::CloseHandle(file);
    if (!ok || !::MoveFileExW(temporary.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING)) {
        ::DeleteFileW(temporary.c_str());
        return false;
    }
    return true;
}

template <class T>
T Read(const u8* at) { T value; std::memcpy(&value, at, sizeof(T)); return value; }

// L5RA.BUN's top-level chunk `id`: offset of its payload and its size.
bool RegionChunk(const std::vector<u8>& region, u32 id, size_t& at, size_t& size) {
    for (size_t pos = 0; pos + 8 <= region.size();) {
        const size_t length = Read<u32>(region.data() + pos + 4);
        if (pos + 8 + length > region.size()) return false;
        if (Read<u32>(region.data() + pos) == id) { at = pos + 8; size = length; return true; }
        pos += 8 + length;
    }
    return false;
}

// The new prop ids (io/WorldPlace.h) at the end of 0x34108. Everything after
// it moves by a multiple of 16, so the chunks that are 16-aligned stay so;
// whole zero entries fill the rest (an entry no 0x34106 row names is never
// read). L5RA.HOT lists L5RA.BUN's chunks by offset and size (16 bytes a row:
// id, size, offset, a hash; it matched the shipped file row for row): its rows
// follow. False when the file is not laid out as expected.
bool GrowPropIds(std::vector<u8>& region, std::vector<u8>& hot, const std::vector<u8>& added) {
    size_t at = 0, size = 0;
    if (!RegionChunk(region, 0x00034108, at, size) || size % 6) return false;
    // Whole entries and a multiple of 16: the next multiple of 48.
    const size_t step = (added.size() + 47) / 48 * 48;
    std::vector<u8> more(step, 0);
    std::memcpy(more.data(), added.data(), added.size());
    region.insert(region.begin() + (std::ptrdiff_t)(at + size), more.begin(), more.end());
    const u32 grown = (u32)(size + step);
    std::memcpy(region.data() + at - 4, &grown, 4);
    if (hot.size() >= 16 && hot.size() % 16 == 0) {
        for (size_t row = 16; row + 16 <= hot.size(); row += 16) {
            const u32 offset = Read<u32>(hot.data() + row + 8);
            if (offset == at - 8) {
                std::memcpy(hot.data() + row + 4, &grown, 4);
            } else if (offset > at - 8) {
                const u32 moved = offset + (u32)step;
                std::memcpy(hot.data() + row + 8, &moved, 4);
            }
        }
    }
    return true;
}


// Chunk 0x34152 inside the top-level 0x80034150: where its payload starts and
// how long it is, and where the parent's header is.
bool ZoneChunk(const std::vector<u8>& region, size_t& parent, size_t& at, size_t& size) {
    size_t outer = 0, outerSize = 0;
    if (!RegionChunk(region, 0x80034150, outer, outerSize)) return false;
    parent = outer - 8;
    for (size_t pos = outer; pos + 8 <= outer + outerSize;) {
        const size_t length = Read<u32>(region.data() + pos + 4);
        if (pos + 8 + length > outer + outerSize) return false;
        if (Read<u32>(region.data() + pos) == 0x34152) { at = pos + 8; size = length; return true; }
        pos += 8 + length;
    }
    return false;
}

constexpr size_t kZoneHead = 48;   // marker, section, count, 12 bytes, min, max, centre

// The new outlines (World::zoneOutlines) over their records of 0x34152. The
// parent 0x80034150 and everything after it move by a multiple of 16 (an extra
// copy of an outline's last point makes it so: an edge of no length), and
// L5RA.HOT's rows follow as in GrowPropIds. Applied after every other change
// to the file, whose offsets are the shipped ones. False when the file is not
// laid out as expected or an outline has more points than the game reads.
bool GrowZones(std::vector<u8>& region, std::vector<u8>& hot, const std::map<u32, std::vector<Vec2>>& outlines) {
    size_t parent = 0, at = 0, size = 0;
    if (!ZoneChunk(region, parent, at, size)) return false;
    std::vector<u8> chunk;
    std::vector<size_t> lastPoint;   // in `chunk`, of each rewritten record, for the padding point
    for (size_t q = at; q + kZoneHead <= at + size;) {
        const u32 marker = Read<u32>(region.data() + q);
        const size_t count = Read<u8>(region.data() + q + 10);
        const size_t length = kZoneHead + 8 * count;
        if (marker != 11 || q + length > at + size) return false;
        const auto outline = outlines.find(Read<u16>(region.data() + q + 8));
        if (outline == outlines.end() || outline->second.size() < 3) {
            chunk.insert(chunk.end(), region.begin() + (std::ptrdiff_t)q, region.begin() + (std::ptrdiff_t)(q + length));
        } else {
            const std::vector<Vec2>& points = outline->second;
            if (points.size() > 126) return false;
            Vec2 lo = points[0], hi = points[0];
            for (const Vec2& p : points) {
                lo = { std::min(lo.x, p.x), std::min(lo.y, p.y) };
                hi = { std::max(hi.x, p.x), std::max(hi.y, p.y) };
            }
            const size_t from = chunk.size();
            chunk.insert(chunk.end(), region.begin() + (std::ptrdiff_t)q, region.begin() + (std::ptrdiff_t)(q + 24));
            chunk[from + 10] = (u8)points.size();   // byte 11 is a flag of its own (0 or 1), kept
            const float box[6] = { lo.x, lo.y, hi.x, hi.y, (lo.x + hi.x) * 0.5f, (lo.y + hi.y) * 0.5f };
            chunk.insert(chunk.end(), (const u8*)box, (const u8*)box + sizeof(box));
            for (const Vec2& p : points) {
                const float xy[2] = { p.x, p.y };
                chunk.insert(chunk.end(), (const u8*)xy, (const u8*)xy + 8);
            }
            lastPoint.push_back(chunk.size() - 8);
        }
        q += length;
    }
    if (lastPoint.empty()) return true;
    if ((chunk.size() - size) % 16) {
        // One more copy of the last rewritten record's last point.
        const size_t point = lastPoint.back();
        size_t record = 0;
        for (size_t q = 0; q + kZoneHead <= chunk.size(); q += kZoneHead + 8 * (size_t)Read<u8>(chunk.data() + q + 10))
            if (q < point) record = q;
        const size_t n = (size_t)Read<u8>(chunk.data() + record + 10) + 1;
        if (n > 127) return false;
        chunk[record + 10] = (u8)n;
        const std::vector<u8> copy(chunk.begin() + (std::ptrdiff_t)point, chunk.begin() + (std::ptrdiff_t)(point + 8));
        chunk.insert(chunk.begin() + (std::ptrdiff_t)(point + 8), copy.begin(), copy.end());
    }
    const long delta = (long)chunk.size() - (long)size;
    if (delta % 16) return false;
    region.erase(region.begin() + (std::ptrdiff_t)at, region.begin() + (std::ptrdiff_t)(at + size));
    region.insert(region.begin() + (std::ptrdiff_t)at, chunk.begin(), chunk.end());
    const u32 chunkSize = (u32)chunk.size(), parentSize = (u32)(Read<u32>(region.data() + parent + 4) + delta);
    std::memcpy(region.data() + at - 4, &chunkSize, 4);
    std::memcpy(region.data() + parent + 4, &parentSize, 4);
    if (delta && hot.size() >= 16 && hot.size() % 16 == 0) {
        for (size_t row = 16; row + 16 <= hot.size(); row += 16) {
            const u32 offset = Read<u32>(hot.data() + row + 8);
            if (offset == parent) std::memcpy(hot.data() + row + 4, &parentSize, 4);
            else if (offset > parent) {
                const u32 moved = (u32)((long)offset + delta);
                std::memcpy(hot.data() + row + 8, &moved, 4);
            }
        }
    }
    return true;
}

void WriteVec3(u8* at, const Vec3& v) {
    const float f[3] = { v.x, v.y, v.z };
    std::memcpy(at, f, sizeof(f));
}

Vec3 ReadVec3(const u8* at) { return { Read<float>(at), Read<float>(at + 4), Read<float>(at + 8) }; }

// Makes every node above `child` hold `box` as well. A moved object that left
// its node's box would be culled with the node, and vanish when looked at from
// some angles only.
// Every draw list that names a record has a row of its own (0x34C03 lists,
// see io/WorldPlace.cpp): 0x3410E min, half diagonal, max, half diagonal.
// Each of them takes `box`.
void SetBoxRows(std::vector<u8>& bytes, ByteSpan boxes, ByteSpan links, size_t record, const Aabb& box) {
    const float half = Length(box.max - box.min) * 0.5f;
    const size_t rows = std::min(links.size / kLinkRowSize, boxes.size / kBoxRowSize);
    for (size_t r = 0; r < rows; ++r) {
        if (links.read<u32>(r * kLinkRowSize + kLinkInstance) != record) continue;
        // Padding rows are all zeros and read as record 0; real ones start
        // with 0xAAAAAAAA, filled in at load.
        if (links.read<u32>(r * kLinkRowSize) == 0) continue;
        u8* row = bytes.data() + (boxes.data - bytes.data()) + r * kBoxRowSize;
        WriteVec3(row, box.min);
        std::memcpy(row + 12, &half, 4);
        WriteVec3(row + 16, box.max);
        std::memcpy(row + 28, &half, 4);
    }
}

void GrowNodes(u8* nodes, size_t nodeCount, int child, const Aabb& box) {
    for (int guard = 0; guard < 64; ++guard) {
        int parent = -1;
        for (size_t n = 0; n < nodeCount && parent < 0; ++n) {
            const u8* node = nodes + n * kNodeSize;
            const u32 count = std::min(Read<u32>(node + kNodeCount), kNodeMaxChildren);
            for (u32 c = 0; c < count; ++c)
                if (Read<i16>(node + kNodeChildren + c * 2) == (i16)child) { parent = (int)n; break; }
        }
        if (parent < 0) return;

        u8* node = nodes + (size_t)parent * kNodeSize;
        WriteVec3(node + kNodeMin, Min(ReadVec3(node + kNodeMin), box.min));
        WriteVec3(node + kNodeMax, Max(ReadVec3(node + kNodeMax), box.max));
        if (parent == 0) return;   // the root
        child = -parent;
    }
}

u32 FourCcOf(nfs::TextureFormat format) {
    switch (format) {
        case nfs::TextureFormat::Dxt1: return 0x31545844;
        case nfs::TextureFormat::Dxt3: return 0x33545844;
        case nfs::TextureFormat::Dxt5: return 0x35545844;
        default:                       return 0;
    }
}

// --- collision ------------------------------------------------------------------
// A pack is written as the shipped one less the polygons it lost, plus the
// ones it gained; a shipped polygon that was moved or given another surface
// counts as both. In the edit list:
//   C <collision section> <index> ...   deleted, by place in the shipped pack
//   A <collision section> <barrier> <quad> <wall> <source or -1> <4 attribute
//     bytes> <flags> <12 floats>        added, one polygon a line
// nfs::RewriteWorldCollision says what of the added ones can reach the game.

struct CollisionEdit {
    const nfs::CollisionPack* pack = nullptr;
    std::vector<u32> cuts;
    std::vector<nfs::CollisionPolygon> adds;
};

// What each pack of a section lost and gained against `shipped`, the section's
// bytes as they came. Packs without a change are left out.
std::vector<CollisionEdit> CollisionEdits(const nfs::LoadedSection& loaded, ByteSpan shipped) {
    std::vector<CollisionEdit> edits;
    std::unordered_map<u32, const nfs::CollisionPack*> packs;
    for (const nfs::CollisionPack& pack : loaded.collision) packs[pack.sectionNumber] = &pack;
    if (packs.empty()) return edits;

    nfs::ChunkReader reader(shipped, loaded.info->streamOffset);
    nfs::Chunk chunk;
    while (reader.Next(chunk)) {
        if (chunk.id != nfs::kChunkWorldCollisionPack) continue;
        nfs::CollisionPack was;
        if (!nfs::ParseWorldCollision(chunk, was)) continue;
        const auto found = packs.find(was.sectionNumber);
        if (found == packs.end()) continue;

        CollisionEdit edit;
        edit.pack = found->second;
        std::vector<bool> kept(was.polygons.size(), false);
        for (const nfs::CollisionPolygon& polygon : found->second->polygons) {
            const nfs::CollisionPolygon* same =
                polygon.source < kept.size() && !kept[polygon.source] ? &was.polygons[polygon.source] : nullptr;
            if (same && std::memcmp(polygon.corners, same->corners, sizeof(polygon.corners)) == 0 &&
                std::memcmp(polygon.attributes, same->attributes, sizeof(polygon.attributes)) == 0 &&
                polygon.flags == same->flags)
                kept[polygon.source] = true;
            else
                edit.adds.push_back(polygon);
        }
        for (u32 i = 0; i < (u32)kept.size(); ++i)
            if (!kept[i]) edit.cuts.push_back(i);
        if (!edit.cuts.empty() || !edit.adds.empty()) edits.push_back(std::move(edit));
    }
    return edits;
}

// The edits as written to the game: ground under raised added ground cut away
// (nfs::CutUnderRaised), shipped ground included, whichever pack holds it. Not
// in the edit list, so a bump moved later leaves no hole behind. Shipped ground
// that is cut is taken out of its article; no article of L5RA is shared by two
// instances (measured 18/11), which RewriteWorldCollision would refuse.
std::vector<CollisionEdit> CutUnderRaised(const nfs::LoadedSection& loaded, const std::vector<CollisionEdit>& edits,
                                          size_t& cut, size_t& pieces) {
    std::vector<CollisionEdit> out = edits;
    std::vector<const nfs::CollisionPolygon*> raised;
    Aabb all;
    for (const CollisionEdit& edit : edits)
        for (const nfs::CollisionPolygon& p : edit.adds)
            if (!p.barrier) {
                raised.push_back(&p);
                for (const Vec3& c : p.corners) all.Add(c);
            }
    if (raised.empty()) return out;
    // The raised polygons by 32 m plan cell: each ground polygon is tried only against those whose cells its
    // box touches, in their order in `raised`, which is what nfs::CutUnderRaised would not skip anyway. Against
    // all of them a save with 160 000 added polygons spent 22 minutes here (measured 22/11).
    constexpr float kCell = 32.0f;
    constexpr int kWide = 64;   // cells; a polygon over more goes into `wide` (tried against everything)
    auto cellsOf = [&](const nfs::CollisionPolygon& p, int& x0, int& x1, int& y0, int& y1) {
        Aabb box;
        for (int i = 0; i < (p.quad ? 4 : 3); ++i) box.Add(p.corners[i]);
        x0 = (int)std::floor(box.min.x / kCell); x1 = (int)std::floor(box.max.x / kCell);
        y0 = (int)std::floor(box.min.y / kCell); y1 = (int)std::floor(box.max.y / kCell);
        return (long long)(x1 - x0 + 1) * (y1 - y0 + 1) <= kWide;
    };
    auto cellKey = [](int x, int y) { return ((unsigned long long)(u32)x << 32) | (u32)y; };
    std::unordered_map<unsigned long long, std::vector<u32>> cells;
    std::vector<u32> wide;
    for (u32 r = 0; r < raised.size(); ++r) {
        int x0, x1, y0, y1;
        if (!cellsOf(*raised[r], x0, x1, y0, y1)) { wide.push_back(r); continue; }
        for (int x = x0; x <= x1; ++x)
            for (int y = y0; y <= y1; ++y) cells[cellKey(x, y)].push_back(r);
    }
    std::vector<const nfs::CollisionPolygon*> nearby;
    std::vector<u32> picked;
    // The raised polygons that may lie over `p`; all of them for a polygon over many cells.
    auto over = [&](const nfs::CollisionPolygon& p) -> const std::vector<const nfs::CollisionPolygon*>& {
        int x0, x1, y0, y1;
        if (!cellsOf(p, x0, x1, y0, y1)) return raised;
        picked = wide;
        for (int x = x0; x <= x1; ++x)
            for (int y = y0; y <= y1; ++y) {
                const auto found = cells.find(cellKey(x, y));
                if (found != cells.end()) picked.insert(picked.end(), found->second.begin(), found->second.end());
            }
        std::sort(picked.begin(), picked.end());
        picked.erase(std::unique(picked.begin(), picked.end()), picked.end());
        nearby.clear();
        for (const u32 r : picked) nearby.push_back(raised[r]);
        return nearby;
    };
    for (const nfs::CollisionPack& pack : loaded.collision) {
        const Aabb& b = pack.bounds;
        if (b.max.x < all.min.x || b.min.x > all.max.x || b.max.y < all.min.y || b.min.y > all.max.y) continue;
        auto edit = std::find_if(out.begin(), out.end(), [&](const CollisionEdit& e) { return e.pack == &pack; });
        std::vector<nfs::CollisionPolygon> made;
        if (edit != out.end()) {
            std::vector<nfs::CollisionPolygon> adds;
            for (const nfs::CollisionPolygon& p : edit->adds)
                if (nfs::CutUnderRaised(p, over(p), made)) { ++cut; pieces += made.size(); adds.insert(adds.end(), made.begin(), made.end()); }
                else adds.push_back(p);
            edit->adds.swap(adds);
        }
        // Shipped ground still in the pack as shipped.
        std::vector<u32> gone;
        std::vector<nfs::CollisionPolygon> extra;
        const std::unordered_set<u32> cuts = edit != out.end() ? std::unordered_set<u32>(edit->cuts.begin(), edit->cuts.end())
                                                               : std::unordered_set<u32>{};
        for (const nfs::CollisionPolygon& p : pack.polygons) {
            if (p.source == nfs::CollisionPolygon::kNoSource || cuts.count(p.source)) continue;
            if (!nfs::CutUnderRaised(p, over(p), made)) continue;
            gone.push_back(p.source);
            extra.insert(extra.end(), made.begin(), made.end());
            ++cut; pieces += made.size();
        }
        if (gone.empty()) continue;
        if (edit == out.end()) { out.push_back({ &pack, {}, {} }); edit = out.end() - 1; }
        edit->cuts.insert(edit->cuts.end(), gone.begin(), gone.end());
        edit->adds.insert(edit->adds.end(), extra.begin(), extra.end());
    }
    return out;
}

// Writes those edits into `bytes`, the section being built. A pack that needs
// more room grows by a multiple of 0x100, which keeps every later chunk's
// alignment; the caller sees the new size as it does a placed object's.
// Collision grid of the section (cells of 96 m naming the instances in them).
constexpr u32 kChunkWorldCollisionGrid = 0x0003B802;

// Collision packs outside the grid's section file (a city's own sections): the boxes of their instances that got
// polygons wait here for the grid's section, written later in the same pass (sections go in table order and
// 25000 is near its end), and their new articles are numbered apart from that section's own.
// ponytail: the grid's section is only reached when it has collision edits of its own (true of every list so
// far); write it unconditionally when a list comes along with collision in own sections only.
std::vector<nfs::GridEntry> pendingGrid;
u32 foreignArticleId = 1000000;
void ResetCollisionPass() {
    pendingGrid.clear();
    foreignArticleId = 1000000;
}

bool PatchCollision(const nfs::LoadedSection& loaded, const std::vector<CollisionEdit>& edits,
                    std::vector<u8>& bytes, WorldWriteReport& counts, std::wstring& error) {
    if (edits.empty()) return true;
    struct Splice { size_t at, size; std::vector<u8> payload; };
    std::vector<Splice> splices;

    auto numberOf = [](const nfs::Chunk& chunk) {
        // The section number sits after the 0x11 filler and the size.
        size_t filler = 0;
        while (filler < 16 && filler < chunk.raw.size && chunk.raw.data[filler] == 0x11) ++filler;
        return chunk.raw.read<u32>(filler + 4);
    };
    auto editOf = [&](u32 number) {
        return std::find_if(edits.begin(), edits.end(), [&](const CollisionEdit& candidate) {
            return candidate.pack->sectionNumber == number;
        });
    };

    // Ground no box of its own pack holds whole is cut into its boxes and, for
    // what lies outside them, into every other pack's: a pass that writes
    // nothing finds it. ponytail: every pack is rewritten once more to look
    // for room; fine at one collision section, index the boxes if saves slow.
    struct Spill { u32 home; nfs::CollisionPolygon polygon; u8 atHome; size_t abroad = 0; };
    std::vector<Spill> spill;
    // New articles (far polygons get instances of their own) carry the map's
    // running article number on from the highest of all packs.
    u32 nextArticleId = 0;
    // FRS_COLLISION_STRETCH=1: the old way (the nearest box grows), to compare saves.
    const bool stretch = std::getenv("FRS_COLLISION_STRETCH") != nullptr;
    {
        nfs::ChunkReader reader(ByteSpan{ bytes.data(), bytes.size() }, loaded.info->streamOffset);
        nfs::Chunk chunk;
        while (reader.Next(chunk))
            if (chunk.id == nfs::kChunkWorldCollisionPack)
                nextArticleId = std::max(nextArticleId, nfs::WorldCollisionArticleIdEnd(chunk.raw));
    }
    // FRS_COLLISION_ID_BASE=<n>: new articles numbered from n (an in-game test of whether the number matters).
    if (const char* base = std::getenv("FRS_COLLISION_ID_BASE")) nextArticleId = (u32)std::strtoul(base, nullptr, 10);
    bool gridHere = false;
    {
        nfs::ChunkReader reader(ByteSpan{ bytes.data(), bytes.size() }, loaded.info->streamOffset);
        nfs::Chunk chunk;
        while (reader.Next(chunk)) gridHere = gridHere || chunk.id == kChunkWorldCollisionGrid;
    }
    if (!gridHere) nextArticleId = std::max(nextArticleId, foreignArticleId);
    {
        nfs::ChunkReader reader(ByteSpan{ bytes.data(), bytes.size() }, loaded.info->streamOffset);
        nfs::Chunk chunk;
        while (reader.Next(chunk)) {
            if (chunk.id != nfs::kChunkWorldCollisionPack) continue;
            const auto edit = editOf(numberOf(chunk));
            if (edit == edits.end()) continue;
            size_t skipped = 0;
            std::vector<u8> scratch, placed;
            u32 scratchId = nextArticleId;
            if (!nfs::RewriteWorldCollision(chunk.raw, *edit->pack, edit->cuts, edit->adds, skipped, scratch, error,
                                            {}, &placed, nullptr, stretch ? nullptr : &scratchId))
                return false;
            // 3: far ground in an instance of its own; never cut into other packs' boxes.
            for (size_t i = 0; i < edit->adds.size(); ++i)
                if (!edit->adds[i].barrier && placed[i] != 2 && placed[i] != 3)
                    spill.push_back({ edit->pack->sectionNumber, edit->adds[i], placed[i] });
        }
    }

    std::vector<nfs::GridEntry> grown;
    nfs::Chunk grid;
    bool haveGrid = false;
    nfs::ChunkReader reader(ByteSpan{ bytes.data(), bytes.size() }, loaded.info->streamOffset);
    nfs::Chunk chunk;
    while (reader.Next(chunk)) {
        if (chunk.id == kChunkWorldCollisionGrid) { grid = chunk; haveGrid = true; continue; }
        if (chunk.id != nfs::kChunkWorldCollisionPack) continue;
        const u32 number = numberOf(chunk);
        const auto edit = editOf(number);
        std::vector<nfs::CollisionPolygon> foreign;
        std::vector<size_t> from;
        for (size_t i = 0; i < spill.size(); ++i)
            if (spill[i].home != number) { foreign.push_back(spill[i].polygon); from.push_back(i); }
        if (edit == edits.end() && foreign.empty()) continue;
        static const std::vector<u32> none;
        static const std::vector<nfs::CollisionPolygon> nothing;
        static const nfs::CollisionPack bare;

        Splice splice;
        splice.at   = (size_t)(chunk.raw.data - bytes.data());
        splice.size = chunk.raw.size;
        size_t skipped = 0;
        std::vector<u8> placed;
        std::vector<nfs::GridEntry> boxes;
        const bool own = edit != edits.end();
        if (!nfs::RewriteWorldCollision(chunk.raw, own ? *edit->pack : bare, own ? edit->cuts : none,
                                        own ? edit->adds : nothing, skipped, splice.payload, error, foreign, &placed,
                                        &boxes, stretch ? nullptr : &nextArticleId))
            return false;
        grown.insert(grown.end(), boxes.begin(), boxes.end());
        const size_t first = own ? edit->adds.size() : 0;
        for (size_t i = 0; i < foreign.size(); ++i) spill[from[i]].abroad += placed[first + i];
        if (!own) {
            if (splice.payload.size() == chunk.raw.size &&
                std::memcmp(splice.payload.data(), chunk.raw.data, chunk.raw.size) == 0) continue;
        } else {
            counts.collision        += edit->cuts.size();
            counts.collisionAdded   += edit->adds.size() - std::min(skipped, edit->adds.size());
            counts.collisionSkipped += skipped;
        }
        splices.push_back(std::move(splice));
    }
    // Left out at home but written into another pack's box: not left out.
    for (const Spill& item : spill)
        if (!item.atHome && item.abroad && counts.collisionSkipped) { --counts.collisionSkipped; ++counts.collisionAdded; }
    // The instances that got polygons into every grid cell their box touches,
    // or the game finds the new ground only now and then (nfs::AddToCollisionGrid).
    if (!haveGrid) {
        pendingGrid.insert(pendingGrid.end(), grown.begin(), grown.end());
        foreignArticleId = std::max(foreignArticleId, nextArticleId);
    } else {
        grown.insert(grown.end(), pendingGrid.begin(), pendingGrid.end());
        pendingGrid.clear();
    }
    if (haveGrid && !grown.empty()) {
        Splice splice;
        splice.at   = (size_t)(grid.raw.data - bytes.data());
        splice.size = grid.raw.size;
        // ponytail: false = nothing new or not the shipped layout; then left as it is.
        if (nfs::AddToCollisionGrid(grid.raw, grown, splice.payload)) splices.push_back(std::move(splice));
    }
    std::sort(splices.begin(), splices.end(), [](const Splice& a, const Splice& b) { return a.at < b.at; });
    // From the end, so the earlier offsets stay good.
    for (auto splice = splices.rbegin(); splice != splices.rend(); ++splice) {
        const u32 size = (u32)splice->payload.size();
        std::memcpy(bytes.data() + splice->at - 4, &size, 4);
        bytes.erase(bytes.begin() + (long long)splice->at, bytes.begin() + (long long)(splice->at + splice->size));
        bytes.insert(bytes.begin() + (long long)splice->at, splice->payload.begin(), splice->payload.end());
    }
    return true;
}
// Props the game makes physical when hit (trash cans, pop machines, parked
// cars): top-level chunk 0x34027 of the section file, 8 bytes (two words) and
// then 64 bytes a prop: +0x08 rotation quaternion (x, y, z, w), +0x18 position,
// both in collision axes (x = -game y, y = up, z = game x), three hashes, +0x30
// prop id, +0x34 hash, +0x38 the record's flags. The id is found in the
// container's 0x34106 rows (u16 id, u16 record). Measured on every shipped
// section: 17244 props, position = record translation, quaternion = record
// rotation in those axes for 17168 (the rest are mirrored). The game spawns
// the physical prop and its collision from here, not from the record, so a
// moved record leaves its prop behind, and a removed one keeps spawning it.
constexpr u32 kChunkProps       = 0x00034027;
constexpr u32 kChunkPropRecords = 0x00034106;

// Moves the props of moved records and takes out those of removed ones (the
// game spawns `count` props, i16 at +0xA of the header; the rest move down
// and the freed tail is zeroed). Returns how many were taken out.
size_t PatchProps(std::vector<u8>& bytes, ByteSpan data, const nfs::LoadedSection& loaded,
                  const std::map<u32, std::pair<u32, u32>>& propRecords) {
    size_t first = 0;
    while (first < data.size && data.data[first] == 0x11) ++first;   // alignment filler
    first += 8;
    if (first + 4 > data.size) return 0;
    u8* base = bytes.data() + (data.data - bytes.data());
    const size_t count = (size_t)std::max<i16>(Read<i16>(base + first + 2), 0);
    size_t kept = 0;
    for (size_t index = 0; index < count && first + (index + 1) * kPropSize <= data.size; ++index) {
        u8* prop = base + first + index * kPropSize;
        const auto found = propRecords.find(Read<u32>(prop + 0x30));
        if (found != propRecords.end()) {
            const auto [number, record] = found->second;
            if (std::find(loaded.removedNatives.begin(), loaded.removedNatives.end(),
                          std::make_pair(number, record)) != loaded.removedNatives.end())
                continue;
            for (const auto& moved : loaded.movedNatives) {
                if (moved.sceneryNumber != number || moved.record != record) continue;
                SetPropPose(prop + 0x08, moved.moved);   // the game's entry starts 8 bytes in
                break;
            }
        }
        // Entries are 64 bytes from first + 8; `prop` is 8 bytes before one.
        if (kept != index)
            std::memmove(base + first + 8 + kept * kPropSize, prop + 8, kPropSize);
        ++kept;
    }
    if (kept == count) return 0;
    std::memset(base + first + 8 + kept * kPropSize, 0, (count - kept) * kPropSize);
    const i16 newCount = (i16)kept;
    std::memcpy(base + first + 2, &newCount, 2);
    return count - kept;
}

// Applies one loaded section's edits to a copy of its shipped bytes.
void PatchSection(const nfs::World& world, const nfs::LoadedSection& loaded, std::vector<u8>& bytes,
                  WorldWriteReport& report) {
    // Those of another size than the texture had are laid out afterwards, by
    // ResizeTextures.
    for (const auto& patch : loaded.texturePatches)
        if (!patch->Resized() && patch->offset + patch->bytes.size() <= bytes.size())
            std::memcpy(bytes.data() + patch->offset, patch->bytes.data(), patch->bytes.size());

    // 0x34106 rows (u16 prop id, u16 record) of each container: which record
    // a 0x34027 prop stands for.
    std::map<u32, std::pair<u32, u32>> propRecords;   // prop id -> (scenery number, record)
    ByteSpan props;

    nfs::ChunkReader reader(ByteSpan{ bytes.data(), bytes.size() }, loaded.info->streamOffset);
    nfs::Chunk chunk;
    while (reader.Next(chunk)) {
        if (chunk.id == kChunkProps) { props = chunk.data; continue; }
        if (chunk.id != nfs::kChunkTrackScenery) continue;

        u32 number = 0;
        ByteSpan records, boxes, links, nodes, infos;
        nfs::ChunkReader children = nfs::ChunkReader::Children(chunk);
        nfs::Chunk child;
        while (children.Next(child)) {
            switch (child.id) {
                case nfs::kChunkSceneryHeader:     number  = child.data.read<u32>(kHeaderNumber); break;
                case nfs::kChunkSceneryInstances:  records = child.data; break;
                case nfs::kChunkWorldSceneryBoxes: boxes   = child.data; break;
                case nfs::kChunkWorldSceneryLinks: links   = child.data; break;
                case nfs::kChunkWorldSceneryNodes: nodes   = child.data; break;
                case nfs::kChunkSceneryInfos:      infos   = child.data; break;
                case kChunkPropRecords:
                    for (size_t at = 0; at + 4 <= child.data.size; at += 4)
                        propRecords[child.data.read<u16>(at)] = { number, child.data.read<u16>(at + 2) };
                    break;
                default: break;
            }
        }
        if (records.empty()) continue;

        // The spans point into `bytes`, which is ours to change.
        u8* recordBase = bytes.data() + (records.data - bytes.data());
        const size_t recordCount = records.size / kRecordSize;

        for (const auto& removed : loaded.removedNatives) {
            if (removed.first != number || removed.second >= recordCount) continue;
            // A record with no matrix draws nothing; it stays, so nothing
            // that counts records moves.
            std::memset(recordBase + removed.second * kRecordSize + kRecordRotation, 0, 36);
            ++report.removed;
        }

        for (const auto& moved : loaded.movedNatives) {
            if (moved.sceneryNumber != number || moved.record >= recordCount) continue;
            const bool gone = std::find(loaded.removedNatives.begin(), loaded.removedNatives.end(),
                                        std::make_pair(moved.sceneryNumber, moved.record)) !=
                              loaded.removedNatives.end();
            if (gone) continue;

            u8* record = recordBase + moved.record * kRecordSize;
            const Mat4& m = moved.moved;
            WriteVec3(record + kRecordTranslation, { m.m[3][0], m.m[3][1], m.m[3][2] });
            for (int row = 0; row < 3; ++row)
                WriteVec3(record + kRecordRotation + (size_t)row * 12, { m.m[row][0], m.m[row][1], m.m[row][2] });
            ++report.moved;

            // The boxes the game culls by have to follow the object.
            const nfs::SolidObject* model = nullptr;
            for (const nfs::SceneryInstance& instance : loaded.scenery.instances)
                if (instance.sceneryNumber == number && instance.record == moved.record) {
                    model = world.FindModel(loaded.scenery.ModelHash(instance));
                    break;
                }
            if (!model || !model->bounds.Valid()) continue;
            const Aabb box = Transform(model->bounds, m);
            WriteVec3(record + kRecordBoundsMin, box.min);
            WriteVec3(record + kRecordBoundsMax, box.max);

            SetBoxRows(bytes, boxes, links, moved.record, box);
            // The tree names records, not rows (0x73DB90 -> 0x73D7C0).
            if (!nodes.empty())
                GrowNodes(bytes.data() + (nodes.data - bytes.data()), nodes.size / kNodeSize, (int)moved.record, box);
        }

        // Scenery that places an edited mesh, at any level of detail: the
        // info's radius and each record's culling boxes take the new shape in.
        // They never shrink -- the other levels keep their shipped shape.
        if (world.meshReplacements.empty() || infos.empty()) continue;
        u8* infoBase = bytes.data() + (infos.data - bytes.data());
        const size_t infoCount = infos.size / kInfoSize;
        std::vector<const nfs::SolidObject*> edited(infoCount, nullptr);
        for (size_t i = 0; i < infoCount; ++i)
            for (size_t level = 0; level < 4 && !edited[i]; ++level) {
                const u32 hash = Read<u32>(infoBase + i * kInfoSize + kInfoLevels + level * 4);
                for (const auto& replacement : world.meshReplacements)
                    if (replacement.hash == hash) edited[i] = world.FindModel(hash);
            }
        for (size_t i = 0; i < infoCount; ++i)
            if (edited[i]) {
                u8* radius = infoBase + i * kInfoSize + kInfoRadius;
                const float wider = std::max(Read<float>(radius), ModelRadius(*edited[i]));
                std::memcpy(radius, &wider, sizeof(wider));
            }
        for (size_t r = 0; r < recordCount; ++r) {
            u8* record = recordBase + r * kRecordSize;
            const i16 info = Read<i16>(record + kRecordInfo);
            if (info < 0 || (size_t)info >= infoCount || !edited[(size_t)info] || !edited[(size_t)info]->bounds.Valid())
                continue;
            Mat4 m = Mat4::Identity();
            for (int row = 0; row < 3; ++row)
                for (int col = 0; col < 3; ++col)
                    m.m[row][col] = Read<float>(record + kRecordRotation + (size_t)(row * 3 + col) * 4);
            m.m[3][0] = Read<float>(record + kRecordTranslation);
            m.m[3][1] = Read<float>(record + kRecordTranslation + 4);
            m.m[3][2] = Read<float>(record + kRecordTranslation + 8);
            Aabb box = Transform(edited[(size_t)info]->bounds, m);
            box.Add(ReadVec3(record + kRecordBoundsMin));
            box.Add(ReadVec3(record + kRecordBoundsMax));
            WriteVec3(record + kRecordBoundsMin, box.min);
            WriteVec3(record + kRecordBoundsMax, box.max);
            SetBoxRows(bytes, boxes, links, r, box);
            if (!nodes.empty())
                GrowNodes(bytes.data() + (nodes.data - bytes.data()), nodes.size / kNodeSize, (int)r, box);
        }
    }
    if (props.data && (!loaded.movedNatives.empty() || !loaded.removedNatives.empty()))
        if (const size_t gone = PatchProps(bytes, props, loaded, propRecords))
            LOG_INFO("section %s: %zu physical props of removed objects taken out", loaded.info->name.c_str(), gone);
}

} // namespace

bool GameIsRunning() {
    HANDLE snapshot = ::CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE) return false;
    PROCESSENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    bool running = false;
    if (::Process32FirstW(snapshot, &entry)) {
        do {
            if (::lstrcmpiW(entry.szExeFile, L"nfsw.exe") == 0) { running = true; break; }
        } while (::Process32NextW(snapshot, &entry));
    }
    ::CloseHandle(snapshot);
    return running;
}

std::wstring WorldEditsFolder(const std::wstring& tracksFolder) {
    bool redirected = false;
    return TargetFolder(tracksFolder, redirected) + kBackupFolder;
}

std::vector<WorldZone> ReadWorldZones(const std::wstring& tracksFolder, const std::wstring& region) {
    // The shipped file: a copy kept by a save comes first (the live one may be an edited one).
    const std::wstring name = L"\\" + region + L".BUN";
    std::vector<u8> bytes = ReadWhole(WorldEditsFolder(tracksFolder) + name);
    if (bytes.empty()) bytes = ReadWhole(tracksFolder + kBackupFolder + name);
    if (bytes.empty()) bytes = ReadWhole(tracksFolder + name);
    std::vector<WorldZone> zones;
    size_t parent = 0, at = 0, size = 0;
    if (bytes.empty() || !ZoneChunk(bytes, parent, at, size)) return zones;
    // The base: the first word of 0x34151, the sibling before (400 in L5RA).
    u32 base = 0;
    for (size_t pos = parent + 8; pos + 12 <= at - 8; pos += 8 + Read<u32>(bytes.data() + pos + 4))
        if (Read<u32>(bytes.data() + pos) == 0x34151) { base = Read<u32>(bytes.data() + pos + 8); break; }
    for (size_t q = at; q + kZoneHead <= at + size;) {
        const size_t count = Read<u8>(bytes.data() + q + 10);
        if (Read<u32>(bytes.data() + q) != 11 || q + kZoneHead + 8 * count > at + size) break;
        const i16 number = Read<i16>(bytes.data() + q + 8);
        // As 0x7471f0 sorts them: the drivable list, the one 0x741ba0 searches.
        if (number != -1 && number / 1000 <= 0x13 && number % 1000 >= 1 && (u32)(number % 1000) < base) {
            WorldZone zone;
            zone.section = (u32)number;
            for (size_t k = 0; k < count; ++k)
                zone.points.push_back({ Read<float>(bytes.data() + q + kZoneHead + 8 * k),
                                        Read<float>(bytes.data() + q + kZoneHead + 8 * k + 4) });
            zones.push_back(std::move(zone));
        }
        q += kZoneHead + 8 * count;
    }
    return zones;
}

std::wstring KeptTextureFolder(const std::wstring& tracksFolder) {
    bool redirected = false;
    const std::wstring backups = TargetFolder(tracksFolder, redirected) + kBackupFolder;
    ::CreateDirectoryW(backups.c_str(), nullptr);
    const std::wstring folder = backups + L"\\textures";
    ::CreateDirectoryW(folder.c_str(), nullptr);
    return folder;
}

std::wstring KeptMeshFolder(const std::wstring& tracksFolder) {
    bool redirected = false;
    const std::wstring backups = TargetFolder(tracksFolder, redirected) + kBackupFolder;
    ::CreateDirectoryW(backups.c_str(), nullptr);
    const std::wstring folder = backups + L"\\meshes";
    ::CreateDirectoryW(folder.c_str(), nullptr);
    return folder;
}

void UnstageTextureReplacement(nfs::World& world, u32 hash) {
    for (size_t i = 0; i < world.Loaded().size(); ++i) {
        nfs::LoadedSection* loaded = world.MutableLoaded(i);
        if (!loaded || !loaded->info) continue;
        const ByteSpan shipped = world.SectionBytes(*loaded->info);

        auto& patches = loaded->texturePatches;
        for (size_t p = patches.size(); p-- > 0;) {
            const nfs::LoadedSection::BytePatch& patch = *patches[p];
            if (patch.hash != hash) continue;
            const u8* ours = patch.bytes.data();
            const ByteSpan back = shipped.sub(patch.offset, patch.shippedSize);
            const auto restore = [&](nfs::Texture& texture) {
                if (texture.pixels.data != ours) return;
                texture.pixels        = back;
                texture.width         = patch.shippedWidth;
                texture.height        = patch.shippedHeight;
                texture.mipCount      = patch.shippedMips;
                texture.baseLevelSize = patch.shippedBase;
            };
            for (auto& pack : loaded->texturePacks)
                for (nfs::Texture& texture : pack->MutableTextures()) restore(texture);
            for (nfs::Texture& level : loaded->streamMips) restore(level);
            patches.erase(patches.begin() + (std::ptrdiff_t)p);
            loaded->placementsChanged = true;
        }
    }
}

namespace {

// The power of two nearest to a size, as its exponent.
int NearestPower(u32 size) {
    int power = 0;
    while ((2u << power) <= size) ++power;
    return (size - (1u << power)) > ((2u << power) - size) ? power + 1 : power;
}

int PowerOf(u32 size) {
    int power = 0;
    while ((1u << (power + 1)) <= size) ++power;
    return power;
}

u32 Scaled(u32 size, int shift) { return shift >= 0 ? size << shift : size >> -shift; }

} // namespace

size_t StageTextureReplacement(nfs::World& world, u32 hash, const std::wstring& imagePath,
                               std::wstring& error) {
    UnstageTextureReplacement(world, hash);

    const nfs::Texture* base = world.FindTexture(hash);
    const u32 fourcc = base ? FourCcOf(base->format) : 0;
    if (!fourcc) {
        error = L"That texture is not one the editor can write (DXT1, DXT3 and DXT5 are).";
        return 0;
    }
    const u32 blockBytes = base->format == nfs::TextureFormat::Dxt1 ? 8u : 16u;

    // The texture takes the image's size. It is stored as a chain: the pack
    // holds the small levels, and each larger level is kept apart. The largest
    // becomes the size of the image -- to the nearest power of two, which
    // every texture of the game is -- and the others follow in step.
    nfs::Texture largest;
    u32 imageWidth = 0, imageHeight = 0;
    if (!world.LargestTexture(hash, largest) || !ImageSize(imagePath, imageWidth, imageHeight)) {
        error = L"Could not read " + imagePath;
        return 0;
    }
    const int shiftX = NearestPower(imageWidth)  - PowerOf(largest.width);
    const int shiftY = NearestPower(imageHeight) - PowerOf(largest.height);

    // The same texture is stored many times at a few sizes: encode each once.
    std::map<std::tuple<u32, u32, u32>, std::vector<u8>> encoded;
    size_t staged = 0, tooSmall = 0;

    for (size_t i = 0; i < world.Loaded().size(); ++i) {
        nfs::LoadedSection* loaded = world.MutableLoaded(i);
        if (!loaded || !loaded->info) continue;
        const ByteSpan shipped = world.SectionBytes(*loaded->info);

        auto stage = [&](nfs::Texture& slot, bool packCopy) {
            if (slot.hash != hash || slot.pixels.empty()) return true;
            if (slot.pixels.data < shipped.data || slot.pixels.data >= shipped.data + shipped.size) return true;

            const u32 width  = Scaled(slot.width, shiftX);
            const u32 height = Scaled(slot.height, shiftY);
            const u32 mips   = std::max<u32>(slot.mipCount, 1);
            if (width < 4 || height < 4 || (std::max(width, height) >> (mips - 1)) == 0) {
                error = L"The image is too small for this texture: its smallest stored copy would be "
                        L"under 4 pixels, or have fewer levels than the game expects.";
                return false;
            }
            if (width > 16384 || height > 16384) {
                error = L"The image is too large: no level of a texture can be over 16384 pixels.";
                return false;
            }
            const bool resized = width != slot.width || height != slot.height;

            const auto key = std::make_tuple(width, height, mips);
            auto found = encoded.find(key);
            if (found == encoded.end()) {
                // Alpha that is not opacity (gloss, on roads and walls) is not
                // in the saved picture: the copy's own stays where it is.
                // A larger level kept apart says only its size: the format is
                // the pack entry's.
                nfs::Texture copy = *base;
                copy.width         = slot.width;
                copy.height        = slot.height;
                copy.mipCount      = slot.mipCount;
                copy.baseLevelSize = slot.baseLevelSize;
                copy.pixels        = slot.pixels;
                std::vector<u32> stored;
                u32 storedWidth = 0, storedHeight = 0;
                const bool keepAlpha = !nfs::AlphaIsOpacity(*base) &&
                                       nfs::DecodeTexture(copy, 0, stored, storedWidth, storedHeight) &&
                                       storedWidth == slot.width && storedHeight == slot.height;
                std::vector<u8> bytes;
                if (!EncodeForSlot(imagePath, width, height, fourcc, mips, bytes, error,
                                   keepAlpha ? &stored : nullptr, storedWidth, storedHeight))
                    return false;
                found = encoded.emplace(key, std::move(bytes)).first;
            }
            if (!resized && found->second.size() > slot.pixels.size) { ++tooSmall; return true; }

            auto patch = std::make_unique<nfs::LoadedSection::BytePatch>();
            patch->hash          = hash;
            patch->offset        = (size_t)(slot.pixels.data - shipped.data);
            patch->shippedSize   = slot.pixels.size;
            patch->shippedWidth  = slot.width;
            patch->shippedHeight = slot.height;
            patch->shippedMips   = slot.mipCount;
            patch->shippedBase   = slot.baseLevelSize;
            patch->width         = (u16)width;
            patch->height        = (u16)height;
            patch->mipCount      = (u8)mips;
            patch->base          = std::max(1u, width / 4) * std::max(1u, height / 4) * blockBytes;
            patch->blockBytes    = (u8)blockBytes;
            if (resized) {
                // Laid out again when the section is written. A pack's entry
                // is its levels made up to 16 bytes; a level kept apart is
                // that level and nothing more.
                patch->bytes = found->second;
                if (packCopy) patch->bytes.resize((patch->bytes.size() + 15) & ~(size_t)15, 0);
                slot.width         = patch->width;
                slot.height        = patch->height;
                slot.baseLevelSize = patch->base;
            } else {
                // In place. The slot can be a few bytes longer than the levels
                // (padding): keep those.
                patch->bytes.assign(slot.pixels.data, slot.pixels.data + slot.pixels.size);
                std::memcpy(patch->bytes.data(), found->second.data(), found->second.size());
            }
            slot.pixels = ByteSpan{ patch->bytes.data(), patch->bytes.size() };
            loaded->texturePatches.push_back(std::move(patch));
            loaded->placementsChanged = true;
            ++staged;
            return true;
        };

        for (auto& pack : loaded->texturePacks)
            for (nfs::Texture& texture : pack->MutableTextures())
                if (!stage(texture, true)) { UnstageTextureReplacement(world, hash); return 0; }
        for (nfs::Texture& level : loaded->streamMips)
            if (!stage(level, false)) { UnstageTextureReplacement(world, hash); return 0; }
    }

    if (!staged && error.empty())
        error = tooSmall ? L"The encoded image does not fit the texture's space."
                         : L"That texture is not stored in this region's files.";
    LOG_INFO("texture %08X: image %ux%u staged over %zu stored copies, largest level %ux%u -> %ux%u (%zu did not fit)",
             hash, imageWidth, imageHeight, staged, (u32)largest.width, (u32)largest.height,
             Scaled(largest.width, shiftX), Scaled(largest.height, shiftY), tooSmall);
    return staged;
}

size_t LoadWorldEdits(nfs::World& world, const std::wstring& realTracksFolder) {
    // With FRS_WRITE_TRACKS the list is read from where a scripted run wrote it.
    bool redirected = false;
    const std::wstring tracksFolder = TargetFolder(realTracksFolder, redirected);
    FILE* file = nullptr;
    if (::_wfopen_s(&file, EditsPath(tracksFolder, world).c_str(), L"rt") != 0 || !file) return 0;

    std::unordered_map<u32, nfs::LoadedSection*> bySection;
    for (size_t i = 0; i < world.Loaded().size(); ++i)
        if (nfs::LoadedSection* section = world.MutableLoaded(i))
            if (section->info) bySection[section->info->number] = section;

    // Collision packs by their own number, which is not a section's.
    std::unordered_map<u32, std::pair<nfs::LoadedSection*, u32>> collisionPacks;
    for (const auto& [number, section] : bySection)
        for (u32 p = 0; p < (u32)section->collision.size(); ++p)
            collisionPacks[section->collision[p].sectionNumber] = { section, p };
    size_t cutPolygons = 0;

    size_t applied = 0;
    char line[1024];
    while (std::fgets(line, sizeof(line), file)) {
        if (line[0] == 'C') {
            // Deleted collision polygons: the pack, then each one's place in it as shipped.
            char* at = line + 1;
            const u32 number = (u32)std::strtoul(at, &at, 10);
            const auto found = collisionPacks.find(number);
            if (found == collisionPacks.end()) continue;
            nfs::LoadedSection* section = found->second.first;
            const u32 packIndex = found->second.second;
            nfs::CollisionPack& pack = section->collision[packIndex];

            std::vector<bool> gone(pack.shippedPolygons, false);
            for (char* end = nullptr;; at = end) {
                const u32 index = (u32)std::strtoul(at, &end, 10);
                if (end == at) break;
                if (index < gone.size()) gone[index] = true;
            }
            // One row of the Edits tab per pack, however many lines name it.
            auto& cuts = section->savedCollisionCuts;
            auto row = std::find_if(cuts.begin(), cuts.end(),
                                    [&](const auto& entry) { return entry.first == packIndex; });
            if (row == cuts.end()) row = cuts.insert(cuts.end(), { packIndex, {} });
            std::vector<nfs::CollisionPolygon> left;
            for (const nfs::CollisionPolygon& polygon : pack.polygons) {
                if (polygon.source < gone.size() && gone[polygon.source]) row->second.push_back(polygon);
                else left.push_back(polygon);
            }
            cutPolygons += pack.polygons.size() - left.size();
            pack.polygons = std::move(left);
            ++applied;
            continue;
        }
        if (line[0] == 'A') {
            // An added collision polygon: the pack, what it is, its corners.
            unsigned number = 0, a[4] = {}, flags = 0;
            int barrier = 0, quad = 0, wall = 0, source = -1, consumed = 0;
            if (std::sscanf(line, "A %u %d %d %d %d %u %u %u %u %u%n", &number, &barrier, &quad, &wall, &source,
                            &a[0], &a[1], &a[2], &a[3], &flags, &consumed) < 10) continue;
            const auto found = collisionPacks.find(number);
            if (found == collisionPacks.end()) continue;
            nfs::CollisionPolygon polygon;
            const char* at = line + consumed;
            bool whole = true;
            for (int i = 0; i < 12 && whole; ++i) {
                char* end = nullptr;
                const float value = std::strtof(at, &end);
                whole = end != at;
                at = end;
                (&polygon.corners[i / 3].x)[i % 3] = value;
            }
            if (!whole) continue;
            polygon.barrier = barrier != 0;
            polygon.quad    = quad != 0;
            polygon.wall    = wall != 0;
            for (int i = 0; i < 4; ++i) polygon.attributes[i] = (u8)a[i];
            if (source >= 0) {
                // A shipped polygon that was changed keeps what it is drawn by.
                polygon.source  = (u32)source;
                polygon.surface = (u8)((((u32)a[0] | (u32)a[1] << 8 | (u32)a[2] << 16 | (u32)a[3] << 24) * 2654435761u) >> 24);
            } else {
                polygon.SyncFromAttributes();
                const u32 hash = (u32)a[0] | (u32)a[1] << 8 | (u32)a[2] << 16 | (u32)a[3] << 24;
                if (nfs::IsWorldSurface(hash)) {
                    // Bytes that are a surface hash (the road bake's), not a race group and an index.
                    polygon.raceGroup = 0;
                    polygon.surface = nfs::WorldSurfaceId(hash);
                }
            }
            polygon.flags = (u8)flags;
            nfs::LoadedSection* section = found->second.first;
            const u32 packIndex = found->second.second;
            section->collision[packIndex].polygons.push_back(polygon);
            // The pack's box follows what is added: a ground lookup (Launch Game's pose) skips a pack by its box,
            // so ground added outside the shipped box was never found.
            for (int i = 0; i < (polygon.quad ? 4 : 3); ++i) section->collision[packIndex].bounds.Add(polygon.corners[i]);
            auto& adds = section->savedCollisionAdds;
            auto row = std::find_if(adds.begin(), adds.end(),
                                    [&](const auto& entry) { return entry.first == packIndex; });
            if (row == adds.end()) row = adds.insert(adds.end(), { packIndex, {} });
            row->second.push_back(polygon);
            ++applied;
            continue;
        }        if (line[0] == 'T') {
            unsigned hash = 0;
            char name[260] = {};
            if (std::sscanf(line, "T %x %259s", &hash, name) != 2) continue;
            const std::wstring fileName(name, name + std::strlen(name));
            std::wstring why;
            if (StageTextureReplacement(world, hash, tracksFolder + kBackupFolder + L"\\textures\\" + fileName, why)) {
                world.textureReplacements.push_back({ hash, fileName });
                ++applied;
            }
            continue;
        }
        if (line[0] == 'Z' && line[1] == 'M') {
            // A point of an outline widened by hand, in order: section, game x, y.
            unsigned section = 0;
            float x = 0.0f, y = 0.0f;
            if (std::sscanf(line, "ZM %u %f %f", &section, &x, &y) != 3) continue;
            world.manualZoneOutlines[section].push_back({ x, y });
            ++applied;
            continue;
        }
        if (line[0] == 'Z') {
            // A point of a widened streaming zone's outline, in order: section, game x, y.
            unsigned section = 0;
            float x = 0.0f, y = 0.0f;
            if (std::sscanf(line, "Z %u %f %f", &section, &x, &y) != 3) continue;
            world.zoneOutlines[section].push_back({ x, y });
            ++applied;
            continue;
        }
        if (line[0] == 'O') {
            // An edited mesh: the model's hash and the OBJ it was kept as.
            unsigned hash = 0;
            char name[260] = {};
            if (std::sscanf(line, "O %x %259s", &hash, name) != 2) continue;
            const std::wstring fileName(name, name + std::strlen(name));
            std::wstring why;
            if (StageWorldMesh(world, hash, tracksFolder + kBackupFolder + L"\\meshes\\" + fileName, why)) ++applied;
            else LOG_WARN("mesh edit %08X from the list not applied: %S", hash, why.c_str());
            continue;
        }
        if (line[0] == 'X') {
            // A new texture: its hash, the game texture it copies, its name, its kept image.
            unsigned hash = 0, like = 0, host = 0;
            char name[64] = {}, kept[260] = {};
            if (std::sscanf(line, "X %x %x %63s %259s %u", &hash, &like, name, kept, &host) < 4) continue;
            SetNewTextureHost(hash, host);   // the texture section that takes it, when the line names one
            std::wstring why;
            if (StageNewWorldTexture(world, name, like,
                                     tracksFolder + kBackupFolder + L"\\textures\\" + std::wstring(kept, kept + std::strlen(kept)),
                                     why) &&
                NewTextureOf(world, hash))
                ++applied;
            else
                LOG_WARN("new texture %08X from the list not applied: %S", hash, why.c_str());
            continue;
        }
        if (line[0] == 'N') {
            // A new model: the section whose file takes it, its hash, its kept OBJ.
            unsigned section = 0, hash = 0;
            char name[260] = {};
            if (std::sscanf(line, "N %u %x %259s", &section, &hash, name) != 3) continue;
            const std::wstring fileName(name, name + std::strlen(name));
            std::wstring why;
            if (StageNewWorldModel(world, section, hash, tracksFolder + kBackupFolder + L"\\meshes\\" + fileName, why))
                ++applied;
            else
                LOG_WARN("new model %08X from the list not applied: %S", hash, why.c_str());
            continue;
        }
        if (line[0] == 'K') {
            // A placed car (io/WorldMesh.h StageCarModel): section, hash, model name, car, paint.
            nfs::World::NewModel car;
            char name[64] = {}, folder[64] = {};
            int metallic = 0, gloss = 1;
            if (std::sscanf(line, "K %u %x %63s %63s %f %f %f %d %d", &car.section, &car.hash, name, folder, &car.hue,
                            &car.lightness, &car.saturation, &metallic, &gloss) != 9)
                continue;
            car.name = name;
            car.car = folder;
            car.metallic = metallic != 0;
            car.gloss = gloss != 0;
            const size_t slash = realTracksFolder.find_last_of(L"\\/");
            std::wstring why;
            if (StageCarModel(world, realTracksFolder.substr(0, slash == std::wstring::npos ? 0 : slash), car, why))
                ++applied;
            else
                LOG_WARN("car %08X from the list not applied: %S", car.hash, why.c_str());
            continue;
        }
        if (line[0] == 'P') {
            // A placed object: section, scenery chunk, model, transform.
            unsigned placedSection = 0, placedChunk = 0, hash = 0;
            int consumed = 0;
            if (std::sscanf(line, "P %u %u %x%n", &placedSection, &placedChunk, &hash, &consumed) < 3) continue;
            const auto home = bySection.find(placedSection);
            const nfs::SolidObject* model = world.FindModel(hash);
            if (home == bySection.end() || !model) continue;

            nfs::PlacedInstance placed;
            const char* at = line + consumed;
            bool whole = true;
            for (int i = 0; i < 16 && whole; ++i) {
                char* end = nullptr;
                placed.transform.m[i / 4][i % 4] = std::strtof(at, &end);
                whole = end != at;
                at = end;
            }
            if (!whole) continue;
            placed.modelHash   = hash;
            placed.modelName   = model->name;
            placed.modelBounds = model->bounds;
            placed.triangles   = model->TriangleCount();
            if (model->bounds.Valid()) placed.radius = Length(model->bounds.max - model->bounds.min) * 0.5f;
            home->second->placements.push_back({ placedChunk, std::move(placed) });
            ++applied;
            continue;
        }
        char kind = 0;
        u32 sectionNumber = 0, number = 0, record = 0;
        int read = 0;
        if (std::sscanf(line, " %c %u %u %u%n", &kind, &sectionNumber, &number, &record, &read) < 4) continue;
        const auto found = bySection.find(sectionNumber);
        if (found == bySection.end()) continue;
        nfs::LoadedSection* section = found->second;

        if (kind == 'R') {
            section->removedNatives.push_back({ number, record });
            ++applied;
        } else if (kind == 'M') {
            Mat4 moved;
            const char* at = line + read;
            bool whole = true;
            for (int i = 0; i < 16 && whole; ++i) {
                char* end = nullptr;
                moved.m[i / 4][i % 4] = std::strtof(at, &end);
                whole = end != at;
                at = end;
            }
            if (!whole) continue;
            for (nfs::SceneryInstance& instance : section->scenery.instances) {
                if (instance.sceneryNumber != number || instance.record != record) continue;
                section->movedNatives.push_back({ number, record, instance.transform, moved });
                instance.transform = moved;
                ++applied;
                break;
            }
        }
    }
    std::fclose(file);
    // Staging marks sections as changed; these were written before.
    for (size_t i = 0; i < world.Loaded().size(); ++i)
        if (nfs::LoadedSection* section = world.MutableLoaded(i)) section->placementsChanged = false;
    // A zone widened by hand and not again by the bake keeps its own outline.
    for (const auto& [section, outline] : world.manualZoneOutlines)
        world.zoneOutlines.emplace(section, outline);
    LOG_INFO("NFS World edits: %zu applied from the list, %zu collision polygons deleted by it", applied,
             cutPolygons);
    return applied;
}

namespace {

// Where each streamed part of a texture section ends in its file: the last
// chunk that is not filler before the larger-level packs, then each pack.
std::vector<u32> StreamedParts(const std::vector<u8>& bytes) {
    std::vector<u32> ends;
    size_t at = 0, last = 0;
    while (at + 8 <= bytes.size()) {
        const u32 id = Read<u32>(bytes.data() + at);
        const size_t end = at + 8 + Read<u32>(bytes.data() + at + 4);
        if (end > bytes.size()) break;
        if (id == nfs::kChunkWorldStreamMips) {
            if (ends.empty()) ends.push_back((u32)last);
            ends.push_back((u32)end);
        } else if (id != 0 && ends.empty()) {
            last = end;
        }
        at = end;
    }
    return ends;
}

// How much of a texture section's first part is not a texture pack's pixels:
// what stays in memory once the pixels have gone to the video card. The third
// word of the section's table entry is this size, made up to an alignment
// (tools/nfsw/table_third.py: never under it by more than 128, never over by
// more than 732, on all 3034 shipped entries).
size_t ResidentBytes(const u8* data, size_t size) {
    const auto children = [&](size_t begin, size_t end, u32 wanted, size_t& from, size_t& to) {
        for (size_t at = begin; at + 8 <= end;) {
            const size_t next = at + 8 + Read<u32>(data + at + 4);
            if (next > end) break;
            if (Read<u32>(data + at) == wanted) { from = at; to = next; return true; }
            at = next;
        }
        return false;
    };
    size_t last = 0, pixels = 0;
    for (size_t at = 0; at + 8 <= size;) {
        const u32 id = Read<u32>(data + at);
        const size_t end = at + 8 + Read<u32>(data + at + 4);
        if (end > size || id == nfs::kChunkWorldStreamMips) break;
        if (id != 0) last = end;
        size_t block = 0, blockEnd = 0, chunk = 0, chunkEnd = 0;
        if (id == nfs::kChunkTexturePack &&
            children(at + 8, end, nfs::kChunkTpkDataBlock, block, blockEnd) &&
            children(block + 8, blockEnd, nfs::kChunkTpkPixelData, chunk, chunkEnd))
            pixels += chunkEnd - chunk;
        at = end;
    }
    return last - std::min(last, pixels);
}

} // namespace

bool WriteWorldEdits(nfs::World& world, const std::wstring& tracksFolder, WorldWriteReport& report) {
    nfs::gridRowsWest = 0;   // set again by this save's collision grid, when it reaches west of the shipped one
    bool redirected = false;
    const std::wstring target = TargetFolder(tracksFolder, redirected);
    if (!redirected && GameIsRunning()) {
        report.error = L"NFS World is running and holds the map's files open.\n\n"
                       L"Close the game, then save again.";
        return false;
    }

    const std::wstring backups = target + kBackupFolder;
    ::CreateDirectoryW(backups.c_str(), nullptr);

    // Sections whose file is no longer laid out as the table in L5RA.BUN says.
    // By the entry's name: the table written is the shipped one, and the live
    // table the world was read from moves when the zones before it grew.
    struct Grown { u32 id; u32 delta; std::vector<u32> parts; u32 resident = 0; };
    std::vector<Grown> grownSections;

    // The shipped region file: the copy kept here, else the one kept in the
    // game's own folder (a scripted run writes elsewhere, and the game's live
    // file may already be an edited one), else the live one.
    const std::wstring regionName = world.Region() + L".BUN";
    const std::wstring regionKept = backups + L"\\" + regionName;
    const std::wstring regionKeptInGame = tracksFolder + kBackupFolder + L"\\" + regionName;
    const std::vector<u8> shippedRegion = ReadWhole(Exists(regionKept) ? regionKept
                                                    : Exists(regionKeptInGame) ? regionKeptInGame
                                                    : tracksFolder + L"\\" + regionName);
    PropIds propIds;
    {
        size_t at = 0, size = 0;
        if (RegionChunk(shippedRegion, 0x00034108, at, size)) propIds.shipped = ByteSpan{ shippedRegion.data() + at, size };
    }

    std::string list;
    for (const auto& replacement : world.textureReplacements) {
        char line[400];
        std::snprintf(line, sizeof(line), "T %08X %ls\n", replacement.hash, replacement.file.c_str());
        list += line;
    }
    for (const auto& [section, outline] : world.manualZoneOutlines)
        for (const Vec2& p : outline) {
            char line[96];
            std::snprintf(line, sizeof(line), "ZM %u %.9g %.9g\n", section, p.x, p.y);
            list += line;
        }
    for (const auto& [section, outline] : world.zoneOutlines) {
        // Only where the bake widened it again: the hand-made one is listed above.
        const auto manual = world.manualZoneOutlines.find(section);
        if (manual != world.manualZoneOutlines.end() && manual->second.size() == outline.size() &&
            std::equal(outline.begin(), outline.end(), manual->second.begin(),
                       [](const Vec2& a, const Vec2& b) { return a.x == b.x && a.y == b.y; }))
            continue;
        for (const Vec2& p : outline) {
            char line[96];
            std::snprintf(line, sizeof(line), "Z %u %.9g %.9g\n", section, p.x, p.y);
            list += line;
        }
    }
    for (const auto& replacement : world.meshReplacements) {
        const size_t slash = replacement.file.find_last_of(L"\\/");
        char line[400];
        std::snprintf(line, sizeof(line), "O %08X %ls\n", replacement.hash,
                      replacement.file.substr(slash == std::wstring::npos ? 0 : slash + 1).c_str());
        list += line;
    }
    // New textures before the new models that draw with them, and only those
    // a placed new model draws with (io/WorldTexture.h).
    // Game textures copied for models placed where they are not loaded come
    // with them, made again every time and not listed.
    std::vector<u32> texturesInUse = NewTexturesInUse(world);
    const std::vector<u32> copiedTextures = StageCopiedTextures(world);
    texturesInUse.insert(texturesInUse.end(), copiedTextures.begin(), copiedTextures.end());
    StageCarPaintTextures(world);
    const std::vector<u32> carTextures = CarTexturesInUse(world);
    texturesInUse.insert(texturesInUse.end(), carTextures.begin(), carTextures.end());
    std::unordered_set<const nfs::LoadedSection*> textureHosts;   // that write one of them
    for (const u32 hash : texturesInUse) {
        const nfs::World::NewTexture* texture = NewTextureOf(world, hash);
        for (const nfs::LoadedSection* host : NewTextureHosts(world, *texture)) textureHosts.insert(host);
        if (!texture->header.empty()) continue;
        char line[400];
        const u32 host = NewTextureHost(texture->hash);
        if (host)
            std::snprintf(line, sizeof(line), "X %08X %08X %s %ls %u\n", texture->hash, texture->like,
                          texture->name.c_str(), texture->file.c_str(), host);
        else
            std::snprintf(line, sizeof(line), "X %08X %08X %s %ls\n", texture->hash, texture->like,
                          texture->name.c_str(), texture->file.c_str());
        list += line;
    }
    // New models before the placements that use them; one nothing places is
    // left out, so taking its placement back takes the model back too.
    for (const auto& model : world.newModels) {
        if (!ModelPlaced(world, model.hash)) continue;
        if (!model.car.empty()) {
            char line[400];
            std::snprintf(line, sizeof(line), "K %u %08X %s %s %.2f %.4f %.4f %d %d\n", model.section, model.hash,
                          model.name.c_str(), model.car.c_str(), model.hue, model.lightness, model.saturation,
                          model.metallic ? 1 : 0, model.gloss ? 1 : 0);
            list += line;
            continue;
        }
        const size_t slash = model.file.find_last_of(L"\\/");
        char line[400];
        std::snprintf(line, sizeof(line), "N %u %08X %ls\n", model.section, model.hash,
                      model.file.substr(slash == std::wstring::npos ? 0 : slash + 1).c_str());
        list += line;
    }
    // The sections are gone over twice. The first time every file is built and
    // none is written: a section that cannot be built stops the save while the
    // game's files are still as they were, instead of leaving some written
    // beside a section table and an edit list that do not know of them.
    const size_t sectionCount = world.Loaded().size();
    const size_t listHead = list.size();
    WorldWriteReport rehearsal;
    for (size_t step = 0; step < sectionCount * 2; ++step) {
        const bool write = step >= sectionCount;
        const size_t i = step % sectionCount;
        if (step == sectionCount) { list.resize(listHead); propIds.added.clear(); }   // numbered again
        if (step % sectionCount == 0) ResetCollisionPass();
        WorldWriteReport& counts = write ? report : rehearsal;

        nfs::LoadedSection* loaded = world.MutableLoaded(i);
        if (!loaded || !loaded->info || loaded->info->fileName.empty()) continue;

        const std::wstring live   = target + L"\\" + loaded->info->fileName;
        const std::wstring backup = backups + L"\\" + loaded->info->fileName;
        const bool meshes    = SectionHasMeshEdits(world, *loaded);
        // A texture section that takes new textures; written when one in use is its.
        const bool hosts     = HostsNewTextures(world, *loaded);
        const bool newTextures = hosts && textureHosts.count(loaded);

        // What the collision lost and gained against the shipped packs.
        std::vector<CollisionEdit> collisionEdits;
        if (!loaded->collision.empty()) collisionEdits = CollisionEdits(*loaded, world.SectionBytes(*loaded->info));
        const bool cut = !collisionEdits.empty();
        for (const CollisionEdit& edit : collisionEdits) {
            for (size_t k = 0; k < edit.cuts.size(); ++k) {
                if (k % 100 == 0) list += (k ? "\nC " : "C ") + std::to_string(edit.pack->sectionNumber);
                list += " " + std::to_string(edit.cuts[k]);
            }
            if (!edit.cuts.empty()) list += "\n";
        }
        for (const CollisionEdit& edit : collisionEdits)
            for (const nfs::CollisionPolygon& p : edit.adds) {
                char text[700];
                int at = std::snprintf(text, sizeof(text), "A %u %d %d %d %d %u %u %u %u %u", edit.pack->sectionNumber,
                                       p.barrier ? 1 : 0, p.quad ? 1 : 0, p.wall ? 1 : 0,
                                       p.source == nfs::CollisionPolygon::kNoSource ? -1 : (int)p.source,
                                       p.attributes[0], p.attributes[1], p.attributes[2], p.attributes[3], p.flags);
                for (const Vec3& c : p.corners)
                    at += std::snprintf(text + at, sizeof(text) - (size_t)at, " %.9g %.9g %.9g", c.x, c.y, c.z);
                list += text;
                list += "\n";
            }
        const bool edited    = !loaded->removedNatives.empty() || !loaded->movedNatives.empty() ||
                               !loaded->texturePatches.empty() || !loaded->placements.empty() || meshes || cut ||
                               newTextures;
        const bool hasBackup = Exists(backup);

        char line[512];
        for (const auto& removed : loaded->removedNatives) {
            std::snprintf(line, sizeof(line), "R %u %u %u\n", loaded->info->number, removed.first, removed.second);
            list += line;
        }
        for (const auto& moved : loaded->movedNatives) {
            int at = std::snprintf(line, sizeof(line), "M %u %u %u", loaded->info->number,
                                   moved.sceneryNumber, moved.record);
            for (int k = 0; k < 16; ++k)
                at += std::snprintf(line + at, sizeof(line) - (size_t)at, " %.9g", moved.moved.m[k / 4][k % 4]);
            list += line;
            list += "\n";
        }

        for (const auto& placement : loaded->placements) {
            int at = std::snprintf(line, sizeof(line), "P %u %u %08X", loaded->info->number,
                                   placement.sceneryNumber, placement.instance.modelHash);
            for (int k = 0; k < 16; ++k)
                at += std::snprintf(line + at, sizeof(line) - (size_t)at, " %.9g",
                                    placement.instance.transform.m[k / 4][k % 4]);
            list += line;
            list += "\n";
        }

        // A section whose file is not the size it shipped with -- placed
        // objects, an edited mesh, a texture at another size -- is built again
        // every time: its new size is what the section table, made from the
        // shipped one on every save, is corrected with below.
        bool resized = false;
        for (const auto& patch : loaded->texturePatches) resized = resized || patch->Resized();
        // Edited collision too: its pack may have grown, and the table is
        // made again from the shipped one on every save.
        // Removed objects too: files written before their props were taken
        // out still spawn them (ponytail: rewritten on every save, 13 sections today).
        if (!loaded->placementsChanged && !loaded->collisionChanged && loaded->placements.empty() &&
            loaded->removedNatives.empty() && !meshes && !resized && !cut && !hosts)
            continue;   // the file already says this

        // The stream holds the section as it shipped: the editor reads the
        // kept copy whenever there is one.
        const ByteSpan shipped = world.SectionBytes(*loaded->info);
        if (shipped.empty()) continue;

        if (!edited) {
            if (!write) continue;
            loaded->collisionChanged = false;
            loaded->changedCollisionPacks.clear();
            if (hasBackup) {
                if (!WriteWhole(live, shipped.data, shipped.size)) {
                    report.error = L"Could not put back " + live;
                    return false;
                }
                ::DeleteFileW(backup.c_str());
                ++report.restored;
            }
            loaded->placementsChanged = false;
            continue;
        }

        std::vector<u8> bytes(shipped.data, shipped.data + shipped.size);
        PatchSection(world, *loaded, bytes, counts);
        // Before anything else moves: it finds the textures by where they
        // were in the shipped file.
        if (!ResizeTextures(*loaded, bytes, report.error)) return false;
        size_t texturesAdded = 0;
        if (newTextures && !WriteNewTextures(world, *loaded, texturesInUse, bytes, texturesAdded, report.error)) return false;
        // Edited meshes change the size of their solid list (the padding
        // after it takes that up); the scenery is found again below.
        if (!WriteWorldMeshes(world, *loaded, bytes, counts.meshes, report.error)) return false;
        // Last: it changes the size, and the patches above go by offset.
        if (!AppendPlacements(world, *loaded, bytes, counts.added, propIds, report.error)) return false;
        // After everything that goes by offset: a pack that gains polygons grows.
        size_t underCut = 0, underPieces = 0;
        const std::vector<CollisionEdit> written = CutUnderRaised(*loaded, collisionEdits, underCut, underPieces);
        if (write && underCut)
            LOG_INFO("collision: %zu ground polygons under raised added ground cut into %zu pieces", underCut, underPieces);
        if (!PatchCollision(*loaded, written, bytes, counts, report.error)) return false;
        // A texture section is streamed in parts, and the table says where
        // each ends. A file whose parts cannot be told is not written: the
        // game would read the larger levels from where they no longer are.
        std::vector<u32> parts;
        if (loaded->info->streamParts > 1) {
            parts = StreamedParts(bytes);
            if (parts.size() != loaded->info->streamParts) {
                report.error = L"The parts " + std::wstring(loaded->info->fileName) +
                               L" is streamed in could not be told apart. Nothing was written.";
                return false;
            }
        }
        if (!write) continue;
        if (bytes.size() != shipped.size || !parts.empty()) {
            // By how much what stays in memory grew: placed objects, an
            // edited model, collision. Made up to 128 like the word it is
            // added to; a part that shrank keeps the room it had.
            const size_t now = ResidentBytes(bytes.data(), bytes.size());
            const size_t was = ResidentBytes(shipped.data, shipped.size);
            const u32 resident = parts.empty() || now <= was ? 0u : (u32)((now - was + 127) & ~(size_t)127);
            grownSections.push_back({ loaded->info->id, (u32)(bytes.size() - shipped.size),
                                      std::move(parts), resident });
        }

        if (!hasBackup && !WriteWhole(backup, shipped.data, shipped.size)) {
            report.error = L"Could not keep a copy of the shipped file as " + backup;
            return false;
        }
        if (!WriteWhole(live, bytes.data(), bytes.size())) {
            report.error = L"Could not write " + live;
            return false;
        }
        loaded->placementsChanged = false;
        loaded->collisionChanged = false;
        loaded->changedCollisionPacks.clear();
        ++report.written;
        LOG_INFO("NFS World: section %s written to %S", loaded->info->name.c_str(), live.c_str());
    }

    // --- the section table -------------------------------------------------------
    // Each entry of chunk 0x34110 says at +0x5C in how many parts the file is
    // streamed, and from +0x60 has three words for each part. Measured on
    // every entry (tools/nfsw/table_levels.py):
    //   1 part, a section of the map: the file's size twice and a third word
    //     a little larger. A file that grew has all three raised by that much.
    //   5 parts, a texture section: for each part, twice where it ends in the
    //     file -- the last chunk before the larger-level packs, then each of
    //     the four packs. The game reads the larger levels by these, so one
    //     that is off shows the level as noise from close by. The third word
    //     is the same in all five: the size of what is not texture pixels in
    //     the first part (ResidentBytes). Larger pixels leave it as it is;
    //     anything else that grows -- STREAML5RA_25000.BUN holds the map's
    //     collision -- raises it by as much, or the game crashes loading the
    //     file (nfsw.exe+0x34B5B0, seen 2026-11-14 with 0x3800 bytes of
    //     collision added and the word left alone).
    // The shipped L5RA.BUN is kept like the sections are, and comes back when
    // nothing is grown any more.
    {
        const std::wstring name = world.Region() + L".BUN";
        const std::wstring live = target + L"\\" + name, kept = backups + L"\\" + name;
        const bool zones = !world.zoneOutlines.empty();
        const bool gridWest = nfs::gridRowsWest > 0;
        if (!grownSections.empty() || !propIds.added.empty() || zones || gridWest || Exists(kept)) {
            std::vector<u8> layout;
            // The shipped table: the copy kept here, else the one kept in the
            // game's own folder (a scripted run writes elsewhere, and the
            // game's live table may already be an edited one), else the live one.
            const std::wstring keptInGame = tracksFolder + kBackupFolder + L"\\" + name;
            const std::wstring source = Exists(kept) ? kept
                                      : Exists(keptInGame) ? keptInGame : tracksFolder + L"\\" + name;
            HANDLE file = ::CreateFileW(source.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                                        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
            const DWORD openError = file == INVALID_HANDLE_VALUE ? ::GetLastError() : 0;
            if (file != INVALID_HANDLE_VALUE) {
                LARGE_INTEGER size{};
                ::GetFileSizeEx(file, &size);
                layout.resize((size_t)size.QuadPart);
                DWORD read = 0;
                if (!::ReadFile(file, layout.data(), (DWORD)layout.size(), &read, nullptr) || read != layout.size())
                    layout.clear();
                ::CloseHandle(file);
            }
            if (layout.empty()) {
                report.error = L"Could not read the section table " + source + L" (error " +
                               std::to_wstring(openError) + L")";
                return false;
            }

            if (grownSections.empty() && propIds.added.empty() && !zones && !gridWest) {
                if (!WriteWhole(live, layout.data(), layout.size())) {
                    report.error = L"Could not put back " + live;
                    return false;
                }
                ::DeleteFileW(kept.c_str());
                const std::wstring hotKept = backups + L"\\" + world.Region() + L".HOT";
                if (Exists(hotKept)) {
                    const std::vector<u8> hot = ReadWhole(hotKept);
                    if (hot.empty() || !WriteWhole(target + L"\\" + world.Region() + L".HOT", hot.data(), hot.size())) {
                        report.error = L"Could not put back the region's HOT file";
                        return false;
                    }
                    ::DeleteFileW(hotKept.c_str());
                }
            } else {
                if (!Exists(kept) && !WriteWhole(kept, layout.data(), layout.size())) {
                    report.error = L"Could not keep a copy of the shipped file as " + kept;
                    return false;
                }
                size_t tableAt = 0, tableSize = 0;
                if (!grownSections.empty() && !RegionChunk(layout, 0x34110, tableAt, tableSize)) {
                    report.error = L"No section table (0x34110) in " + source;
                    return false;
                }
                for (const Grown& grown : grownSections) {
                    size_t entry = 0;
                    for (size_t e = tableAt; e + 208 <= tableAt + tableSize; e += 208)
                        if (Read<u32>(layout.data() + e) == grown.id) { entry = e; break; }
                    if (!entry) {
                        report.error = L"A grown section has no entry in the section table of " + source;
                        return false;
                    }
                    for (size_t part = 0; part < grown.parts.size(); ++part) {
                        u8* at = layout.data() + entry + 0x60 + 12 * part;
                        std::memcpy(at, &grown.parts[part], 4);
                        std::memcpy(at + 4, &grown.parts[part], 4);
                        const u32 third = Read<u32>(at + 8) + grown.resident;
                        std::memcpy(at + 8, &third, 4);
                    }
                    if (!grown.parts.empty()) continue;
                    for (size_t field : { (size_t)0x60, (size_t)0x64, (size_t)0x68 }) {
                        u8* at = layout.data() + entry + field;
                        const u32 value = Read<u32>(at) + grown.delta;
                        std::memcpy(at, &value, sizeof(value));
                    }
                }
                if (gridWest) {
                    if (!nfs::GrowCollisionGridWest(layout, nfs::gridRowsWest)) {
                        report.error = L"The collision grid of " + live + L" could not be widened to the west.";
                        return false;
                    }
                    LOG_INFO("NFS World: collision grid widened %d rows to the west in %S", nfs::gridRowsWest, live.c_str());
                }
                if (!propIds.added.empty() || zones) {
                    // The HOT index comes and goes with the region file.
                    const std::wstring hotName = world.Region() + L".HOT";
                    const std::wstring hotLive = target + L"\\" + hotName, hotKept = backups + L"\\" + hotName;
                    const std::wstring hotKeptInGame = tracksFolder + kBackupFolder + L"\\" + hotName;
                    std::vector<u8> hot = ReadWhole(Exists(hotKept) ? hotKept
                                                    : Exists(hotKeptInGame) ? hotKeptInGame
                                                    : tracksFolder + L"\\" + hotName);
                    if (!hot.empty() && !Exists(hotKept) && !WriteWhole(hotKept, hot.data(), hot.size())) {
                        report.error = L"Could not keep a copy of the shipped file as " + hotKept;
                        return false;
                    }
                    if (!propIds.added.empty() && !GrowPropIds(layout, hot, propIds.added)) {
                        report.error = L"The prop table of " + live + L" is laid out in a way the editor does not write yet.";
                        return false;
                    }
                    // Last: it moves what follows the visibility container.
                    if (zones && !GrowZones(layout, hot, world.zoneOutlines)) {
                        report.error = L"The streaming zones of " + live + L" could not be widened (an outline of more "
                                       L"than 127 points, or a layout the editor does not write yet). Save again after taking the road back.";
                        return false;
                    }
                    if (!hot.empty() && !WriteWhole(hotLive, hot.data(), hot.size())) {
                        report.error = L"Could not write " + hotLive;
                        return false;
                    }
                    if (!propIds.added.empty())
                        LOG_INFO("NFS World: %zu prop ids added to %S (0x34108)", propIds.added.size() / 6, live.c_str());
                    if (zones) LOG_INFO("NFS World: %zu streaming zones widened in %S (0x34152)", world.zoneOutlines.size(),
                                        live.c_str());
                }
                if (!WriteWhole(live, layout.data(), layout.size())) {
                    report.error = L"Could not write the section table " + live;
                    return false;
                }
            }
        }
    }

    const std::wstring edits = backups + L"\\" + world.Region() + L"-edits.txt";
    if (list.empty()) ::DeleteFileW(edits.c_str());
    else if (!WriteWhole(edits, (const u8*)list.data(), list.size())) {
        report.error = L"Could not write the edit list " + edits;
        return false;
    }
    return true;
}

} // namespace io
