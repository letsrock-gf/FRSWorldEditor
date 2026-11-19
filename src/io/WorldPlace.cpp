#include "io/WorldPlace.h"

#include "io/WorldMesh.h"

#include "core/Log.h"
#include "nfs/ChunkReader.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <map>

// Adding scenery to an NFS World section.
//
// How the game draws a section's scenery, read from nfsw.exe (base 0x400000):
//
//   0x746060  the loader. For each 0x80034100 container it keeps pointers to
//             the children: infos (0x34102, count = size / 72), records
//             (0x34103, count = size / 96 after the alignment filler), the
//             tree (0x34105, 48 bytes a node), 0x34107 (count = size / 128)
//             and, for every record i up to size / 32 of 0x3410D, a pointer
//             to that 32-byte entry at record + 0x5C.
//   0x73DB90  the tree walk: a node's box is tested, then each of its children
//             (count at +0x0C, 16-bit numbers at +0x1C) is either a record's
//             index, drawn by 0x73D7C0, or minus the index of another node.
//   0x73D7C0  draws one record: its flags (+0x0C), box (+0x00, +0x10), the
//             16-bit model index at +0x54 into the infos, and the transform.
//   0x73DAF0  the other caller, for two view kinds: every record in order.
//
// So a record the tree does not name is never drawn, and one that has no
// 0x3410D entry is left with a null pointer. A new object therefore needs a
// record, an entry in 0x3410D, a place among some node's children (a node
// holds ten and ships with four or five), and its model among the infos. The
// box and link tables (0x3410E, 0x3410F) are not read by that path and are
// left as they are.
//
// The section's file grows, by a multiple of 128 bytes so that every chunk
// after the scenery keeps the alignment it was written with, and the section
// table in L5RA.BUN has its sizes raised by the same amount (io/WorldWrite.cpp).
namespace io {

namespace {

constexpr size_t kRecordSize = 96;
constexpr size_t kInfoSize   = 72;
constexpr size_t kExtraSize  = 32;    // 0x3410D
constexpr size_t kNodeSize   = 48;
constexpr size_t kNodeSlots  = 10;

constexpr size_t kRecordBoundsMin   = 0x00;
constexpr size_t kRecordBoundsMax   = 0x10;
constexpr size_t kRecordFlags       = 0x0C;
constexpr size_t kRecordVisibility  = 0x1C;   // row of the 0x34107 bit sets
constexpr size_t kRecordTranslation = 0x20;
constexpr size_t kRecordRotation    = 0x2C;
constexpr size_t kRecordNameHash    = 0x50;
constexpr size_t kRecordInfoIndex   = 0x54;
constexpr size_t kRecordExtra       = 0x5C;   // filled in by the game

constexpr size_t kInfoHashes = 0x18;
constexpr size_t kInfoRadius = 0x38;

constexpr size_t kNodeMin      = 0x00;
constexpr size_t kNodeCount    = 0x0C;
constexpr size_t kNodeMax      = 0x10;
constexpr size_t kNodeChildren = 0x1C;

constexpr u32 kIdExtra      = 0x0003410D;
constexpr u32 kIdVisibility = 0x00034107;
constexpr u32 kIdBoxes      = 0x0003410E;
constexpr u32 kIdLinks      = 0x0003410F;
constexpr u32 kIdViews      = 0x00034C03;

constexpr size_t kListRow  = 32;   // a row of 0x3410E and of 0x3410F
constexpr size_t kViewSize = 20;   // a view kind in 0x34C03

template <class T>
T Read(const u8* at) { T value; std::memcpy(&value, at, sizeof(T)); return value; }
template <class T>
void Write(u8* at, T value) { std::memcpy(at, &value, sizeof(T)); }

void WriteVec3(u8* at, const Vec3& v) {
    const float f[3] = { v.x, v.y, v.z };
    std::memcpy(at, f, sizeof(f));
}
Vec3 ReadVec3(const u8* at) { return { Read<float>(at), Read<float>(at + 4), Read<float>(at + 8) }; }

struct Child {
    u32 id = 0;
    std::vector<u8> payload;   // without the alignment filler
};

// The children whose data the loader rounds up to sixteen bytes.
bool Aligned(u32 id) {
    switch (id) {
        case nfs::kChunkSceneryHeader: case nfs::kChunkSceneryInstances: case nfs::kChunkWorldSceneryNodes:
        case nfs::kChunkWorldSceneryBoxes: case nfs::kChunkWorldSceneryLinks:
        case 0x0003410A: case 0x00034C03:
            return true;
        default:
            return false;
    }
}

// The container as bytes, starting at `fileOffset` of the section's file.
std::vector<u8> Emit(const std::vector<Child>& children, size_t fileOffset) {
    std::vector<u8> out(8);
    for (const Child& child : children) {
        const size_t header = out.size();
        out.resize(out.size() + 8);
        const size_t filler = Aligned(child.id) ? (16 - (fileOffset + out.size()) % 16) % 16 : 0;
        out.insert(out.end(), filler, (u8)0x11);
        out.insert(out.end(), child.payload.begin(), child.payload.end());
        Write<u32>(out.data() + header, child.id);
        Write<u32>(out.data() + header + 4, (u32)(out.size() - header - 8));
    }
    Write<u32>(out.data(), (u32)nfs::kChunkTrackScenery);
    Write<u32>(out.data() + 4, (u32)(out.size() - 8));
    return out;
}

Child* Find(std::vector<Child>& children, u32 id) {
    for (Child& child : children)
        if (child.id == id) return &child;
    return nullptr;
}

// --- physical props (io/WorldPlace.h) -------------------------------------------
constexpr u32 kIdProps       = 0x00034027;
constexpr u32 kIdPropRecords = 0x00034106;

// Where a 0x34027 chunk's props begin: (its payload + 15) & ~15, as 0x6F68F0.
size_t PropBase(const u8* file, const nfs::Chunk& chunk) {
    return ((size_t)(chunk.raw.data - file) + 15) & ~(size_t)15;
}

// The section file's 0x34027 chunk; false when it has none.
bool FindProps(const u8* data, size_t size, u64 streamOffset, nfs::Chunk& out) {
    nfs::ChunkReader reader(ByteSpan{ data, size }, streamOffset);
    nfs::Chunk chunk;
    while (reader.Next(chunk))
        if (chunk.id == kIdProps) { out = chunk; return true; }
    return false;
}

// The prop a shipped placement of `modelHash` has, and its 0x34108 flags: the
// physics a copy of it gets. False when no shipped placement of it is one.
bool ShippedProp(const nfs::World& world, u32 modelHash, const PropIds& ids, std::vector<u8>& prop, u16& flags) {
    for (const auto& loaded : world.Loaded()) {
        if (!loaded->info) continue;
        for (const nfs::SceneryInstance& instance : loaded->scenery.instances) {
            if (loaded->scenery.ModelHash(instance) != modelHash) continue;
            const ByteSpan bytes = world.SectionBytes(*loaded->info);
            nfs::Chunk props;
            if (!FindProps(bytes.data, bytes.size, loaded->info->streamOffset, props)) break;
            // The record's id, from its container's 0x34106 rows.
            u32 id = UINT32_MAX;
            nfs::ChunkReader reader(bytes, loaded->info->streamOffset);
            nfs::Chunk chunk;
            while (id == UINT32_MAX && reader.Next(chunk)) {
                if (chunk.id != nfs::kChunkTrackScenery) continue;
                if (nfs::FindChunk(chunk, nfs::kChunkSceneryHeader).data.read<u32>(0x0C) != instance.sceneryNumber) continue;
                const nfs::Chunk rows = nfs::FindChunk(chunk, kIdPropRecords);
                for (size_t at = 0; at + 4 <= rows.data.size; at += 4)
                    if (rows.data.read<u16>(at + 2) == instance.record) { id = rows.data.read<u16>(at); break; }
            }
            if (id == UINT32_MAX) continue;   // this placement is not physical; another may be
            const size_t base = PropBase(bytes.data, props);
            const size_t count = (size_t)Read<i16>(bytes.data + base + 0xA);
            for (size_t k = 0; k < count; ++k) {
                const size_t at = base + 0x10 + k * kPropSize;
                if (at + kPropSize > bytes.size || Read<u32>(bytes.data + at + 0x28) != id) continue;
                if ((size_t)(id + 1) * 6 > ids.shipped.size) return false;
                prop.assign(bytes.data + at, bytes.data + at + kPropSize);
                flags = ids.shipped.read<u16>((size_t)id * 6 + 4);
                return true;
            }
        }
    }
    return false;
}

// A prop to add once its container is written.
struct NewProp {
    u32 id;
    std::vector<u8> bytes;   // the template's, pose and id set
};

// A shipped record that places `modelHash`, with its 0x3410D entry and its
// info: what the game already uses for that model, flags and all.
struct Template {
    std::vector<u8> record, extra, info;
};

bool ShippedTemplate(const nfs::World& world, u32 modelHash, Template& out) {
    for (const auto& loaded : world.Loaded()) {
        if (!loaded->info) continue;
        for (const nfs::SceneryInstance& instance : loaded->scenery.instances) {
            if (loaded->scenery.ModelHash(instance) != modelHash) continue;

            const ByteSpan bytes = world.SectionBytes(*loaded->info);
            nfs::ChunkReader reader(bytes, loaded->info->streamOffset);
            nfs::Chunk chunk;
            while (reader.Next(chunk)) {
                if (chunk.id != nfs::kChunkTrackScenery) continue;
                u32 number = 0;
                ByteSpan records, extras;
                nfs::ChunkReader children = nfs::ChunkReader::Children(chunk);
                nfs::Chunk child;
                while (children.Next(child)) {
                    if (child.id == nfs::kChunkSceneryHeader)         number = child.data.read<u32>(0x0C);
                    else if (child.id == nfs::kChunkSceneryInstances) records = child.data;
                    else if (child.id == kIdExtra)                    extras = child.data;
                }
                if (number != instance.sceneryNumber) continue;
                const ByteSpan record = records.sub((size_t)instance.record * kRecordSize, kRecordSize);
                if (record.size != kRecordSize) continue;
                out.record.assign(record.data, record.data + record.size);
                const ByteSpan extra = extras.sub((size_t)instance.record * kExtraSize, kExtraSize);
                out.extra.assign(extra.data, extra.data + extra.size);
                out.info = loaded->scenery.infos[instance.infoIndex].raw;
                return out.info.size() == kInfoSize;
            }
        }
    }
    return false;
}

// How far the game's tree walk (0x73DB90) stacks nodes still to visit: it keeps
// them in a 64-entry array on its stack, pushing every node child of a node it
// visits (child < 0 is node -child, 48 bytes each from node 0, the root). The
// shipped trees go to 15 at most (792 section files).
size_t TreeStackPeak(const std::vector<u8>& nodes) {
    const size_t count = nodes.size() / kNodeSize;
    std::vector<int> stack{ 0 };
    size_t peak = 1, visited = 0;
    while (!stack.empty()) {
        const int at = stack.back();
        stack.pop_back();
        if (at < 0 || (size_t)at >= count || ++visited > count) return SIZE_MAX;   // a loop or a bad index
        const u8* node = nodes.data() + (size_t)at * kNodeSize;
        const int children = std::min<int>(Read<i16>(node + kNodeCount), (int)kNodeSlots);
        for (int c = 0; c < children; ++c) {
            const i16 child = Read<i16>(node + kNodeChildren + (size_t)c * 2);
            if (child < 0) stack.push_back(-child);
        }
        peak = std::max(peak, stack.size());
    }
    return peak;
}

constexpr size_t kTreeStack = 64;

// Makes every node from `at` up to the root hold `box`.
void GrowUp(std::vector<u8>& nodes, int at, const Aabb& box) {
    const size_t count = nodes.size() / kNodeSize;
    for (int guard = 0; guard < 64; ++guard) {
        u8* grown = nodes.data() + (size_t)at * kNodeSize;
        WriteVec3(grown + kNodeMin, Min(ReadVec3(grown + kNodeMin), box.min));
        WriteVec3(grown + kNodeMax, Max(ReadVec3(grown + kNodeMax), box.max));
        if (at == 0) break;
        int parent = -1;
        for (size_t n = 0; n < count && parent < 0; ++n) {
            const u8* other = nodes.data() + n * kNodeSize;
            const int kids = std::min<int>(Read<i16>(other + kNodeCount), (int)kNodeSlots);
            for (int c = 0; c < kids; ++c)
                if (Read<i16>(other + kNodeChildren + (size_t)c * 2) == (i16)-at) { parent = (int)n; break; }
        }
        if (parent < 0) break;
        at = parent;
    }
}

// Puts `record` among the children of the node best placed to hold `box`, and
// makes every node above it hold the box too. When every node has its ten
// children, the tree grows: the nearest node's child nearest the box (a record
// rather than a node) moves into a new node at the end of the table, beside the
// new record, and takes that child's place -- no count changes anywhere and the
// loader (0x746060) takes the node count from the chunk's size. False, with
// `why`, when the game's walk would then stack more than it can.
bool AddLeaf(std::vector<u8>& nodes, const std::vector<u8>& records, int record, const Aabb& box, std::wstring& why) {
    const size_t count = nodes.size() / kNodeSize;
    const Vec3 centre = box.Center();

    // A node of records rather than of nodes is preferred, the nearest one.
    int best = -1, nearest = -1;
    float bestScore = 0.0f, nearestScore = 0.0f;
    for (size_t n = 0; n < count; ++n) {
        const u8* node = nodes.data() + n * kNodeSize;
        const int children = Read<i16>(node + kNodeCount);
        if (children < 0 || children > (int)kNodeSlots) continue;
        bool leaves = true;
        for (int c = 0; c < children; ++c)
            if (Read<i16>(node + kNodeChildren + (size_t)c * 2) < 0) leaves = false;
        const Vec3 middle = (ReadVec3(node + kNodeMin) + ReadVec3(node + kNodeMax)) * 0.5f;
        const float score = Length(middle - centre) + (leaves ? 0.0f : 1.0e6f);
        if (children > 0 && (nearest < 0 || score < nearestScore)) { nearest = (int)n; nearestScore = score; }
        if (children == (int)kNodeSlots) continue;
        if (best < 0 || score < bestScore) { best = (int)n; bestScore = score; }
    }

    if (best < 0) {
        // Full: a new node takes one child of the nearest node and the record.
        if (nearest < 0) { why = L"its scenery tree has no node"; return false; }
        if (count + 1 > 0x7FFF || record > 0x7FFF) { why = L"its scenery tree would need more than 32767 nodes"; return false; }
        u8* parent = nodes.data() + (size_t)nearest * kNodeSize;
        const int children = Read<i16>(parent + kNodeCount);
        int pick = -1;
        float pickScore = 0.0f;
        Aabb pickBox;
        for (int c = 0; c < children; ++c) {
            const i16 child = Read<i16>(parent + kNodeChildren + (size_t)c * 2);
            Aabb b;
            if (child >= 0) {
                if ((size_t)(child + 1) * kRecordSize > records.size()) continue;
                const u8* r = records.data() + (size_t)child * kRecordSize;
                b.Add(ReadVec3(r + kRecordBoundsMin));
                b.Add(ReadVec3(r + kRecordBoundsMax));
            } else {
                if ((size_t)-child >= count) continue;
                const u8* other = nodes.data() + (size_t)-child * kNodeSize;
                b.Add(ReadVec3(other + kNodeMin));
                b.Add(ReadVec3(other + kNodeMax));
            }
            const float score = Length(b.Center() - centre) + (child < 0 ? 1.0e6f : 0.0f);
            if (pick < 0 || score < pickScore) { pick = c; pickScore = score; pickBox = b; }
        }
        if (pick < 0) { why = L"its scenery tree names children it does not have"; return false; }
        const i16 moved = Read<i16>(parent + kNodeChildren + (size_t)pick * 2);
        std::vector<u8> made(kNodeSize, 0);
        Aabb both = pickBox;
        both.Add(box.min);
        both.Add(box.max);
        WriteVec3(made.data() + kNodeMin, both.min);
        WriteVec3(made.data() + kNodeMax, both.max);
        Write<i16>(made.data() + kNodeCount, 2);
        Write<i16>(made.data() + kNodeChildren, moved);
        Write<i16>(made.data() + kNodeChildren + 2, (i16)record);
        nodes.insert(nodes.end(), made.begin(), made.end());
        parent = nodes.data() + (size_t)nearest * kNodeSize;   // the table moved
        Write<i16>(parent + kNodeChildren + (size_t)pick * 2, (i16)-(int)count);
        GrowUp(nodes, nearest, box);
    } else {
        u8* node = nodes.data() + (size_t)best * kNodeSize;
        const int children = Read<i16>(node + kNodeCount);
        Write<i16>(node + kNodeChildren + (size_t)children * 2, (i16)record);
        Write<i16>(node + kNodeCount, (i16)(children + 1));
        GrowUp(nodes, best, box);
    }

    const size_t peak = TreeStackPeak(nodes);
    if (peak > kTreeStack) {
        why = peak == SIZE_MAX ? L"its scenery tree came out with a loop or a bad index"
                               : L"its scenery tree would be deeper than the game walks (" + std::to_wstring(peak) +
                                     L" nodes waiting, the game keeps 64)";
        return false;
    }
    return true;
}

// The lists the game draws from when its scenery job runs (DAT_00C9E48D set:
// 0x743410 -> 0x73F7A0, which reads 0x34C03 + kind * 0x14: +0x04 row count,
// +0x08 first row, into 0x3410E). 0x34C03 holds 20 bytes a view kind (first
// kind of the list, count, first row at +0x08 and +0x0C, a word kept as is);
// the lists lie back to back, each made up to four rows with zero rows.
// 0x3410E row: record box min, half diagonal, max, half diagonal. 0x3410F row:
// 16 bytes the game fills, record @0x10, info @0x14, record @0x18, 0 (loader
// 0x746060 turns the first two into pointers). A record no list names is not
// drawn on that path, whatever the tree says; moves showed in game because
// they patch an existing row.
// A new record joins the list of view kind 1 and every list that already
// holds a record with its flags (lists are subsets by kind: shadows,
// reflections...).
bool AddToViewLists(Child& views, Child& boxes, Child& links, const std::vector<u8>& records,
                    const std::vector<int>& added, std::wstring& error) {
    const size_t rows = boxes.payload.size() / kListRow;
    const size_t kinds = views.payload.size() / kViewSize;
    if (boxes.payload.size() % kListRow || links.payload.size() / kListRow != rows || kinds < 2) {
        error = L"The draw lists of this section are laid out in a way the editor does not write yet.";
        return false;
    }
    std::map<u32, u32> lists;   // first row -> count
    for (size_t k = 0; k < kinds; ++k) {
        const u8* view = views.payload.data() + k * kViewSize;
        if (Read<u32>(view + 4) == 0) continue;
        u32& count = lists[Read<u32>(view + 8)];
        count = std::max(count, Read<u32>(view + 4));
    }
    // Back to back, each padded to four rows, nothing else in between.
    size_t expected = 0;
    for (const auto& [start, count] : lists) {
        if (start != expected) { expected = SIZE_MAX; break; }
        expected += (count + 3) / 4 * 4;
    }
    const u32 mainStart = Read<u32>(views.payload.data() + kViewSize + 8);
    if (expected != rows || !Read<u32>(views.payload.data() + kViewSize + 4)) {
        error = L"The draw lists of this section are laid out in a way the editor does not write yet.";
        return false;
    }

    auto flagsOf = [&](u32 record) { return Read<u32>(records.data() + (size_t)record * kRecordSize + kRecordFlags); };
    std::vector<u8> newBoxes, newLinks;
    std::map<u32, std::pair<u32, u32>> moved;   // old first row -> new first row, new count
    for (const auto& [start, count] : lists) {
        const u32 first = (u32)(newBoxes.size() / kListRow);
        newBoxes.insert(newBoxes.end(), boxes.payload.begin() + (std::ptrdiff_t)(start * kListRow),
                        boxes.payload.begin() + (std::ptrdiff_t)((start + count) * kListRow));
        newLinks.insert(newLinks.end(), links.payload.begin() + (std::ptrdiff_t)(start * kListRow),
                        links.payload.begin() + (std::ptrdiff_t)((start + count) * kListRow));
        u32 total = count;
        for (const int record : added) {
            bool joins = start == mainStart;
            for (u32 r = start; r < start + count && !joins; ++r) {
                const u32 other = Read<u32>(links.payload.data() + (size_t)r * kListRow + 0x10);
                joins = (size_t)(other + 1) * kRecordSize <= records.size() && flagsOf(other) == flagsOf((u32)record);
            }
            if (!joins) continue;
            const u8* rec = records.data() + (size_t)record * kRecordSize;
            const Vec3 lo = ReadVec3(rec + kRecordBoundsMin), hi = ReadVec3(rec + kRecordBoundsMax);
            const float half = Length(hi - lo) * 0.5f;
            u8 box[kListRow], link[kListRow] = {};
            WriteVec3(box, lo);      Write<float>(box + 12, half);
            WriteVec3(box + 16, hi); Write<float>(box + 28, half);
            const u32 filled[4] = { 0xAAAAAAAAu, 0xBBBBBBBBu, 0xCCCCCCCCu, 0xDDDDDDDDu };
            std::memcpy(link, filled, sizeof(filled));
            Write<u32>(link + 0x10, (u32)record);
            Write<u32>(link + 0x14, (u32)Read<i16>(rec + kRecordInfoIndex));
            Write<u32>(link + 0x18, (u32)record);
            newBoxes.insert(newBoxes.end(), box, box + kListRow);
            newLinks.insert(newLinks.end(), link, link + kListRow);
            ++total;
        }
        const size_t padded = (size_t)(total + 3) / 4 * 4;
        newBoxes.resize((first + padded) * kListRow, 0);
        newLinks.resize((first + padded) * kListRow, 0);
        moved[start] = { first, total };
    }
    for (size_t k = 0; k < kinds; ++k) {
        u8* view = views.payload.data() + k * kViewSize;
        if (Read<u32>(view + 4) == 0) continue;
        const u32 start = Read<u32>(view + 8);
        const auto& [first, total] = moved[start];
        Write<u32>(view + 0x0C, first + (Read<u32>(view + 0x0C) - start));
        Write<u32>(view + 8, first);
        Write<u32>(view + 4, Read<u32>(view + 4) + (total - lists[start]));
    }
    boxes.payload = std::move(newBoxes);
    links.payload = std::move(newLinks);
    return true;
}

// Appends the placements meant for scenery chunk `number` to that container.
bool AppendToContainer(const nfs::World& world, const nfs::LoadedSection& loaded,
                       std::vector<u8>& bytes, u32 number,
                       const std::vector<const nfs::PlacedInstance*>& placed,
                       const std::vector<WorldModelCopy>& copies, PropIds& ids,
                       const std::map<u32, std::pair<std::vector<u8>, u16>>& physical,
                       std::vector<NewProp>& newProps, std::wstring& error) {
    // --- the container, and its children as they stand ---------------------------
    size_t start = 0, size = 0;
    std::vector<Child> children;
    {
        nfs::ChunkReader reader(ByteSpan{ bytes.data(), bytes.size() }, loaded.info->streamOffset);
        nfs::Chunk chunk, chosen;
        while (reader.Next(chunk)) {
            if (chunk.id != nfs::kChunkTrackScenery) continue;
            const nfs::Chunk header = nfs::FindChunk(chunk, nfs::kChunkSceneryHeader);
            if (chosen.id == 0 || header.data.read<u32>(0x0C) == number) chosen = chunk;
            if (header.data.read<u32>(0x0C) == number) break;
        }
        if (chosen.id == 0) { error = L"The section has no scenery to add to."; return false; }

        start = (size_t)(chosen.raw.data - 8 - bytes.data());
        size  = chosen.raw.size + 8;
        nfs::ChunkReader inside = nfs::ChunkReader::Children(chosen);
        inside.SetSkipZeroPadding(false);
        nfs::Chunk child;
        while (inside.Next(child))
            children.push_back({ child.id, std::vector<u8>(child.data.data, child.data.data + child.data.size) });
    }

    // Written back unchanged it has to come out byte for byte, or the layout
    // is not the one this was written for and nothing is touched.
    const std::vector<u8> same = Emit(children, start);
    if (same.size() != size || std::memcmp(same.data(), bytes.data() + start, size) != 0) {
        error = L"The scenery of section " + std::wstring(loaded.info->name.begin(), loaded.info->name.end()) +
                L" is laid out in a way the editor does not write yet.";
        return false;
    }

    Child* infos   = Find(children, nfs::kChunkSceneryInfos);
    Child* records = Find(children, nfs::kChunkSceneryInstances);
    Child* extras  = Find(children, kIdExtra);
    Child* nodes   = Find(children, nfs::kChunkWorldSceneryNodes);
    if (!infos || !records || !extras || !nodes || records->payload.size() % kRecordSize ||
        extras->payload.size() / kExtraSize != records->payload.size() / kRecordSize) {
        error = L"The scenery of this section lacks a table a new object needs.";
        return false;
    }
    // Slack bytes from an earlier pass would sit between the entries and the new ones.
    infos->payload.resize(infos->payload.size() / kInfoSize * kInfoSize);
    extras->payload.resize(extras->payload.size() / kExtraSize * kExtraSize);

    std::map<u32, Template> templates;
    std::vector<int> added;
    for (const nfs::PlacedInstance* placement : placed) {
        // A model copied into this section (io/WorldMesh.h) is named by its copy's hash.
        u32 hash = placement->modelHash;
        bool copied = false;
        for (const WorldModelCopy& copy : copies)
            if (copy.source == hash) { hash = copy.copy; copied = true; }
        const size_t index = records->payload.size() / kRecordSize;
        if (index > 32000) { error = L"That section cannot take more objects."; return false; }

        auto found = templates.find(hash);
        if (found == templates.end()) {
            Template shipped;
            ShippedTemplate(world, placement->modelHash, shipped);
            if (copied) shipped.info.clear();   // it names the source's levels of detail
            found = templates.emplace(hash, std::move(shipped)).first;
        }
        const Template& shipped = found->second;

        // --- the model among this section's infos ----------------------------------
        size_t infoIndex = infos->payload.size() / kInfoSize;
        for (size_t i = 0; i < infos->payload.size() / kInfoSize; ++i)
            if (Read<u32>(infos->payload.data() + i * kInfoSize + kInfoHashes) == hash) { infoIndex = i; break; }
        if (infoIndex == infos->payload.size() / kInfoSize) {
            std::vector<u8> info = shipped.info.size() == kInfoSize ? shipped.info
                                 : !copied && placement->infoTemplate.size() == kInfoSize ? placement->infoTemplate
                                 : std::vector<u8>();
            if (info.empty()) {
                // Never placed by the game: the three near levels are the model
                // itself, the far one none -- the commonest shipped pattern (2425 of
                // 6369 infos, tools/nfsw/newmodel_fields.py); +0x40 is 0 in 5932.
                // The radius is the farthest vertex from the model's origin.
                info.assign(kInfoSize, 0);
                std::memcpy(info.data(), placement->modelName.c_str(), std::min<size_t>(placement->modelName.size(), 23));
                for (int level = 0; level < 3; ++level) Write<u32>(info.data() + kInfoHashes + (size_t)level * 4, hash);
                const nfs::SolidObject* model = world.FindModel(placement->modelHash);
                Write<float>(info.data() + kInfoRadius, model ? ModelRadius(*model) : placement->radius);
            }
            infos->payload.insert(infos->payload.end(), info.begin(), info.end());
        }

        // --- the record -------------------------------------------------------------
        std::vector<u8> record, extra;
        for (size_t i = 0; i < index && record.empty(); ++i) {
            const u8* other = records->payload.data() + i * kRecordSize;
            if ((size_t)Read<i16>(other + kRecordInfoIndex) != infoIndex) continue;
            record.assign(other, other + kRecordSize);
            extra.assign(extras->payload.data() + i * kExtraSize, extras->payload.data() + (i + 1) * kExtraSize);
        }
        if (record.empty() && shipped.record.size() == kRecordSize) {
            record = shipped.record;
            extra  = shipped.extra;
            Write<i16>(record.data() + kRecordVisibility, 0);   // another section's row means nothing here
        }
        if (record.empty()) {
            record.assign(kRecordSize, 0);
            Write<u16>(record.data() + kRecordVisibility + 2, 0xFFFF);
            Write<u32>(record.data() + 0x58, 8);
        }
        extra.resize(kExtraSize, 0);

        const nfs::SolidObject* model = world.FindModel(hash);
        const Aabb modelBox = model && model->bounds.Valid() ? model->bounds : placement->modelBounds;
        Aabb box = Transform(modelBox, placement->transform);
        if (!box.Valid()) {
            const Vec3 at{ placement->transform.m[3][0], placement->transform.m[3][1], placement->transform.m[3][2] };
            box.Add(at);
        }

        const Mat4& m = placement->transform;
        WriteVec3(record.data() + kRecordBoundsMin, box.min);
        WriteVec3(record.data() + kRecordBoundsMax, box.max);
        WriteVec3(record.data() + kRecordTranslation, { m.m[3][0], m.m[3][1], m.m[3][2] });
        for (int row = 0; row < 3; ++row)
            WriteVec3(record.data() + kRecordRotation + (size_t)row * 12, { m.m[row][0], m.m[row][1], m.m[row][2] });
        Write<u32>(record.data() + kRecordNameHash, 0xED170000u + (u32)index);
        Write<i16>(record.data() + kRecordInfoIndex, (i16)infoIndex);
        Write<u32>(record.data() + kRecordExtra, 0);
        // Flag 0x400: 0x73D7C0 draws the model with no matrix (its vertices are
        // where the game placed it, record matrix identity). A new placement has
        // a matrix of its own, so the flag goes.
        Write<u32>(record.data() + kRecordFlags, Read<u32>(record.data() + kRecordFlags) & ~0x400u);

        records->payload.insert(records->payload.end(), record.begin(), record.end());
        extras->payload.insert(extras->payload.end(), extra.begin(), extra.end());
        added.push_back((int)index);
        if (const auto prop = physical.find(placement->modelHash); prop != physical.end()) {
            // An id of its own: a 0x34108 entry naming this container and record,
            // a 0x34106 row here, and the prop itself (written after the container).
            if (!Find(children, kIdPropRecords)) {
                size_t at = children.size();
                for (size_t c = 0; c < children.size(); ++c)
                    if (children[c].id == kIdVisibility) { at = c; break; }
                children.insert(children.begin() + (std::ptrdiff_t)at, Child{ kIdPropRecords, {} });
                // The vector moved: find the tables again.
                infos   = Find(children, nfs::kChunkSceneryInfos);
                records = Find(children, nfs::kChunkSceneryInstances);
                extras  = Find(children, kIdExtra);
                nodes   = Find(children, nfs::kChunkWorldSceneryNodes);
            }
            Child* rows = Find(children, kIdPropRecords);
            const u32 id = ids.Next();
            const u16 entry[3] = { (u16)number, (u16)index, prop->second.second };
            ids.added.insert(ids.added.end(), (const u8*)entry, (const u8*)entry + sizeof(entry));
            const u16 row[2] = { (u16)id, (u16)index };
            rows->payload.insert(rows->payload.end(), (const u8*)row, (const u8*)row + sizeof(row));
            NewProp made{ id, prop->second.first };
            SetPropPose(made.bytes.data(), placement->transform);
            Write<u32>(made.bytes.data() + 0x28, id);
            Write<u32>(made.bytes.data() + 0x30, Read<u32>(record.data() + kRecordFlags));
            Write<u32>(made.bytes.data() + 0x3C, 0);
            newProps.push_back(std::move(made));
        }

        std::wstring why;
        if (!AddLeaf(nodes->payload, records->payload, (int)index, box, why)) {
            const nfs::SolidObject* model = world.FindModel(placement->modelHash);
            error = L"Section " + std::wstring(loaded.info->name.begin(), loaded.info->name.end()) +
                    L" cannot take " + (model ? std::wstring(model->name.begin(), model->name.end()) : std::wstring(L"the object")) +
                    L": " + why + L". Nothing was written.";
            return false;
        }
    }

    // --- the draw lists of the scenery job ----------------------------------------
    {
        Child* views = Find(children, kIdViews);
        Child* boxes = Find(children, kIdBoxes);
        Child* links = Find(children, kIdLinks);
        if (!views || !boxes || !links) { error = L"The scenery of this section lacks its draw lists."; return false; }
        if (!AddToViewLists(*views, *boxes, *links, records->payload, added, error)) return false;
    }

    // --- grown by a multiple of 128, so what follows keeps its alignment ----------
    // The counts the loader takes are whole entries, so spare bytes at the end
    // of one of these tables are never read.
    std::vector<u8> grown;
    bool fits = false;
    const std::pair<u32, size_t> slack[] = { { kIdVisibility, 127 }, { (u32)nfs::kChunkSceneryInfos, kInfoSize - 1 },
                                             { kIdExtra, kExtraSize - 1 } };
    for (const auto& [id, most] : slack) {
        Child* table = Find(children, id);
        if (!table) continue;
        const size_t whole = table->payload.size();
        for (size_t spare = 0; spare <= most && !fits; ++spare) {
            table->payload.resize(whole + spare, 0);
            grown = Emit(children, start);
            fits = (grown.size() - size) % 128 == 0;
        }
        if (fits) break;
        table->payload.resize(whole);
    }
    if (!fits) { error = L"The section could not be grown to a size that keeps its alignment."; return false; }

    bytes.erase(bytes.begin() + (std::ptrdiff_t)start, bytes.begin() + (std::ptrdiff_t)(start + size));
    bytes.insert(bytes.begin() + (std::ptrdiff_t)start, grown.begin(), grown.end());
    return true;
}

} // namespace

void SetPropPose(u8* prop, const Mat4& m) {
    // Rotation in game axes (columns = the matrix rows, scale taken out), then
    // in collision axes: Rc[i][j] = Rg[a(i)][a(j)] * s(i) * s(j) with game axis
    // a = (y, z, x) and sign s = (-1, 1, 1). Checked on the shipped props: the
    // record's matrix gives the stored quaternion for 17168 of 17244 (the rest
    // are mirrored).
    float g[3][3];
    for (int row = 0; row < 3; ++row) {
        const float len = Length(Vec3{ m.m[row][0], m.m[row][1], m.m[row][2] });
        for (int col = 0; col < 3; ++col) g[col][row] = len > 0.0f ? m.m[row][col] / len : 0.0f;
    }
    const int axis[3] = { 1, 2, 0 };
    const float sign[3] = { -1.0f, 1.0f, 1.0f };
    float r[3][3];
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j) r[i][j] = g[axis[i]][axis[j]] * sign[i] * sign[j];
    float q[4];   // x, y, z, w
    const float trace = r[0][0] + r[1][1] + r[2][2];
    if (trace > 0.0f) {
        const float s = std::sqrt(trace + 1.0f) * 2.0f;
        q[0] = (r[2][1] - r[1][2]) / s; q[1] = (r[0][2] - r[2][0]) / s; q[2] = (r[1][0] - r[0][1]) / s; q[3] = s / 4.0f;
    } else if (r[0][0] > r[1][1] && r[0][0] > r[2][2]) {
        const float s = std::sqrt(1.0f + r[0][0] - r[1][1] - r[2][2]) * 2.0f;
        q[0] = s / 4.0f; q[1] = (r[0][1] + r[1][0]) / s; q[2] = (r[0][2] + r[2][0]) / s; q[3] = (r[2][1] - r[1][2]) / s;
    } else if (r[1][1] > r[2][2]) {
        const float s = std::sqrt(1.0f + r[1][1] - r[0][0] - r[2][2]) * 2.0f;
        q[0] = (r[0][1] + r[1][0]) / s; q[1] = s / 4.0f; q[2] = (r[1][2] + r[2][1]) / s; q[3] = (r[0][2] - r[2][0]) / s;
    } else {
        const float s = std::sqrt(1.0f + r[2][2] - r[0][0] - r[1][1]) * 2.0f;
        q[0] = (r[0][2] + r[2][0]) / s; q[1] = (r[1][2] + r[2][1]) / s; q[2] = s / 4.0f; q[3] = (r[1][0] - r[0][1]) / s;
    }
    std::memcpy(prop, q, sizeof(q));
    const float position[3] = { -m.m[3][1], m.m[3][2], m.m[3][0] };
    std::memcpy(prop + 0x10, position, sizeof(position));
}

bool AppendPlacements(const nfs::World& world, const nfs::LoadedSection& loaded, std::vector<u8>& bytes,
                      size_t& added, PropIds& ids, std::wstring& error) {
    std::vector<WorldModelCopy> copies;
    if (!SectionModelCopies(world, loaded, copies, error)) return false;

    // The container this file's props belong to: a physical placement goes
    // there (17244 of 17244 shipped props are named by 0x34108 under it).
    nfs::Chunk props;
    const bool hasProps = FindProps(bytes.data(), bytes.size(), loaded.info->streamOffset, props);
    const u32 propContainer = hasProps ? (u32)(u16)Read<i16>(bytes.data() + PropBase(bytes.data(), props) + 8) : 0;
    bool containerHere = false;
    {
        nfs::ChunkReader reader(ByteSpan{ bytes.data(), bytes.size() }, loaded.info->streamOffset);
        nfs::Chunk chunk;
        while (reader.Next(chunk))
            if (chunk.id == nfs::kChunkTrackScenery &&
                nfs::FindChunk(chunk, nfs::kChunkSceneryHeader).data.read<u32>(0x0C) == propContainer)
                containerHere = true;
    }
    const bool canProp = hasProps && containerHere;

    std::map<u32, std::pair<std::vector<u8>, u16>> physical;   // model -> template prop, 0x34108 flags
    std::map<u32, bool> checked;
    std::map<u32, std::vector<const nfs::PlacedInstance*>> byChunk;
    for (const auto& placement : loaded.placements) {
        u32 number = placement.sceneryNumber;
        const u32 hash = placement.instance.modelHash;
        if (!checked[hash]) {
            checked[hash] = true;
            std::vector<u8> prop;
            u16 flags = 0;
            if (ShippedProp(world, hash, ids, prop, flags)) physical[hash] = { std::move(prop), flags };
        }
        if (physical.count(hash)) {
            if (canProp) number = propContainer;
            else LOG_WARN("%s in section %s: the file has no physical props, placed as scenery only",
                          placement.instance.modelName.c_str(), loaded.info->name.c_str());
        }
        byChunk[number].push_back(&placement.instance);
    }
    if (!canProp) physical.clear();

    std::vector<NewProp> newProps;
    static const std::map<u32, std::pair<std::vector<u8>, u16>> none;
    for (const auto& [number, placed] : byChunk) {
        if (!AppendToContainer(world, loaded, bytes, number, placed, copies, ids,
                               number == propContainer ? physical : none, newProps, error))
            return false;
        added += placed.size();
    }
    if (newProps.empty()) return true;

    // --- the props, after the last of the 0x34027 chunk; it grows by 128s -----------
    if (!FindProps(bytes.data(), bytes.size(), loaded.info->streamOffset, props)) {
        error = L"The props of the section were lost.";
        return false;
    }
    const size_t base = PropBase(bytes.data(), props);
    const size_t count = (size_t)Read<i16>(bytes.data() + base + 0xA);
    const size_t end = (size_t)(props.raw.data - bytes.data()) + props.raw.size;
    const size_t used = base + 0x10 + count * kPropSize;   // spare bytes may follow
    if (used > end || count + newProps.size() > 4096) {   // 0x6F6730 keeps a 4096-bit knocked set
        error = L"The props of the section are laid out in a way the editor does not write yet.";
        return false;
    }
    std::vector<u8> more;
    for (const NewProp& prop : newProps) more.insert(more.end(), prop.bytes.begin(), prop.bytes.end());
    const size_t spare = end - used;
    const size_t grow = more.size() > spare ? ((more.size() - spare + 127) & ~(size_t)127) : 0;
    const size_t header = (size_t)(props.raw.data - bytes.data()) - 8;
    const u32 newSize = (u32)(props.raw.size + grow);
    bytes.insert(bytes.begin() + (std::ptrdiff_t)end, grow, (u8)0);
    std::memcpy(bytes.data() + used, more.data(), more.size());
    Write<i16>(bytes.data() + base + 0xA, (i16)(count + newProps.size()));
    Write<u32>(bytes.data() + header + 4, newSize);
    LOG_INFO("section %s: %zu physical props added (ids from %u)", loaded.info->name.c_str(), newProps.size(),
             newProps.front().id);
    return true;
}

} // namespace io
