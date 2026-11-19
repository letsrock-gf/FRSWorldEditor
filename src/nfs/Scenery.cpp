#include "nfs/Scenery.h"

#include "core/Log.h"
#include "nfs/GameFormat.h"

#include <algorithm>
#include <cstring>

namespace nfs {

namespace {

// --- SceneryInfo, chunk 0x00034102 -----------------------------------------
constexpr size_t kInfoEntrySize = 68;
constexpr size_t kInfoName      = 0x00;   // 32 bytes, zero terminated
constexpr size_t kInfoNameSize  = 32;
constexpr size_t kInfoSolidHash = 0x20;
constexpr size_t kInfoRadius    = 0x3C;

// --- SceneryInstance, chunk 0x00034103 -------------------------------------
constexpr size_t kInstanceEntrySize = 64;
constexpr size_t kInstBoundsMin     = 0x00;
constexpr size_t kInstBoundsMax     = 0x0C;
constexpr size_t kInstInfoIndex     = 0x18;
constexpr size_t kInstFlags         = 0x1A;
constexpr size_t kInstTranslation   = 0x20;
constexpr size_t kInstRotation      = 0x2C;   // 3x3 of i16, row major

// --- Underground 1 ------------------------------------------------------------
// SceneryInfo: u32 hash[6] (one per detail level, the first the closest), i16
// size thresholds at 0x18, eModel pointers filled at run time, radius at 0x44.
constexpr size_t kU1InfoEntrySize  = 72;
constexpr size_t kU1InfoRadius     = 0x44;
// Instance: i16 bounds min[3], max[3], u16 info, u16 flags, float position[3],
// i16 rotation[9], padding.
constexpr size_t kU1InstanceEntrySize = 48;
constexpr size_t kU1InstBounds        = 0x00;
constexpr size_t kU1InstInfoIndex     = 0x0C;
constexpr size_t kU1InstFlags         = 0x0E;
constexpr size_t kU1InstTranslation   = 0x10;
constexpr size_t kU1InstRotation      = 0x1C;

// The rotation matrix is fixed point with 1.0 stored as 0x2000. Scale is baked
// into it, so the rows are only unit length for instances drawn at 1:1.
constexpr float kRotationScale = 1.0f / 8192.0f;

// --- NFS World ---------------------------------------------------------------
// Measured on STREAML5RA_100.BUN: 72 bytes per model, with four detail-level
// hashes after a 24-byte name, and 96 bytes per placement, whose 3x3 matrix is
// plain floats.
constexpr size_t kWorldInfoEntrySize = 72;
constexpr size_t kWorldInfoNameSize  = 24;
constexpr size_t kWorldInfoSolidHash = 0x18;   // the most detailed of four
constexpr size_t kWorldInfoRadius    = 0x38;

constexpr size_t kWorldInstanceEntrySize = 96;
constexpr size_t kWorldInstBoundsMin     = 0x00;
constexpr size_t kWorldInstFlags         = 0x0C;
constexpr size_t kWorldInstBoundsMax     = 0x10;
constexpr size_t kWorldInstTranslation   = 0x20;
constexpr size_t kWorldInstRotation      = 0x2C;   // 3x3 of float, row major

// A placement record does not say which model it places. Chunk 0x3410F does:
// 32 bytes per row, sixteen of them filled in by the game at load, then the
// placement's index and the model's. The table is longer than the placements
// and ends in zeroed rows. Checked on every section: with this link the model's
// box lands on the box the record stores, median 0.02 units apart.
constexpr size_t kWorldLinkEntrySize = 32;
constexpr size_t kWorldLinkInstance  = 0x10;
constexpr size_t kWorldLinkInfo      = 0x14;

Vec3 ReadVec3(ByteSpan span, size_t offset) {
    return { span.read<float>(offset),
             span.read<float>(offset + 4),
             span.read<float>(offset + 8) };
}

std::string ReadName(ByteSpan span, size_t offset, size_t capacity) {
    if (offset >= span.size) return {};
    const size_t available = std::min(capacity, span.size - offset);
    const char* text = (const char*)span.data + offset;
    return std::string(text, ::strnlen(text, available));
}

Mat4 ReadTransform(ByteSpan entry, size_t rotationAt = kInstRotation, size_t translationAt = kInstTranslation) {
    Mat4 m = Mat4::Identity();

    for (int row = 0; row < 3; ++row) {
        for (int col = 0; col < 3; ++col) {
            const i16 fixed = entry.read<i16>(rotationAt + (size_t)(row * 3 + col) * 2);
            m.m[row][col] = (float)fixed * kRotationScale;
        }
    }

    const Vec3 translation = ReadVec3(entry, translationAt);
    m.m[3][0] = translation.x;
    m.m[3][1] = translation.y;
    m.m[3][2] = translation.z;
    return m;
}

} // namespace

bool ParseScenery(const Chunk& sceneryChunk, Scenery& out, bool underground1) {
    ByteSpan infoData, instanceData, linkData;

    ChunkReader reader = ChunkReader::Children(sceneryChunk);
    Chunk child;
    while (reader.Next(child)) {
        if (child.id == kChunkSceneryInfos)          infoData     = child.data;
        else if (child.id == kChunkSceneryInstances) instanceData = child.data;
        else if (child.id == kChunkWorldSceneryLinks) linkData    = child.data;
    }

    if (infoData.empty() || instanceData.empty()) return false;

    // A section can hold several scenery containers, and each one's instances
    // index its own info table from zero. The caller accumulates every
    // container into one Scenery, so an index has to be shifted by the tables
    // already gathered. Without this the second container's instances read the
    // first container's names: at the airport a palm tree came out as the
    // parking building, because index 0 meant a tree in its own table and a
    // building in the one that happened to be first.
    const u32 infoBase = (u32)out.infos.size();

    if (IsWorld()) {
        const size_t worldInfos = infoData.size / kWorldInfoEntrySize;
        for (size_t i = 0; i < worldInfos; ++i) {
            const ByteSpan entry = infoData.sub(i * kWorldInfoEntrySize, kWorldInfoEntrySize);

            SceneryInfo info;
            info.name      = ReadName(entry, kInfoName, kWorldInfoNameSize);
            info.solidHash = entry.read<u32>(kWorldInfoSolidHash);
            info.radius    = entry.read<float>(kWorldInfoRadius);
            info.raw.assign(entry.data, entry.data + entry.size);
            out.infos.push_back(std::move(info));
        }

        const size_t worldInstances = instanceData.size / kWorldInstanceEntrySize;
        const size_t links = linkData.size / kWorldLinkEntrySize;
        std::vector<bool> placed(worldInstances, false);
        for (size_t row = 0; row < links; ++row) {
            const ByteSpan link = linkData.sub(row * kWorldLinkEntrySize, kWorldLinkEntrySize);
            const size_t i = link.read<u32>(kWorldLinkInstance);
            // The zeroed rows at the end read as placement 0, which the first
            // row has already taken.
            if (i >= worldInstances || placed[i]) continue;
            placed[i] = true;

            const ByteSpan entry =
                instanceData.sub(i * kWorldInstanceEntrySize, kWorldInstanceEntrySize);

            SceneryInstance instance;
            instance.infoIndex       = infoBase + link.read<u32>(kWorldLinkInfo);
            instance.flags           = entry.read<u32>(kWorldInstFlags);
            instance.record          = (u32)i;
            instance.worldBounds.min = ReadVec3(entry, kWorldInstBoundsMin);
            instance.worldBounds.max = ReadVec3(entry, kWorldInstBoundsMax);

            Mat4 m = Mat4::Identity();
            for (int row = 0; row < 3; ++row)
                for (int col = 0; col < 3; ++col)
                    m.m[row][col] =
                        entry.read<float>(kWorldInstRotation + (size_t)(row * 3 + col) * 4);
            const Vec3 translation = ReadVec3(entry, kWorldInstTranslation);
            m.m[3][0] = translation.x;
            m.m[3][1] = translation.y;
            m.m[3][2] = translation.z;
            instance.transform = m;

            if (instance.infoIndex >= out.infos.size()) continue;
            out.instances.push_back(instance);
        }
        return !out.instances.empty();
    }

    if (underground1) {
        const size_t infoCount = infoData.size / kU1InfoEntrySize;
        out.infos.reserve(out.infos.size() + infoCount);
        for (size_t i = 0; i < infoCount; ++i) {
            const ByteSpan entry = infoData.sub(i * kU1InfoEntrySize, kU1InfoEntrySize);
            SceneryInfo info;
            info.solidHash = entry.read<u32>(0);
            info.radius    = entry.read<float>(kU1InfoRadius);
            info.raw.assign(entry.data, entry.data + entry.size);
            out.infos.push_back(std::move(info));
        }

        const size_t instanceCount = instanceData.size / kU1InstanceEntrySize;
        out.instances.reserve(out.instances.size() + instanceCount);
        for (size_t i = 0; i < instanceCount; ++i) {
            const ByteSpan entry = instanceData.sub(i * kU1InstanceEntrySize, kU1InstanceEntrySize);
            SceneryInstance instance;
            instance.infoIndex = infoBase + entry.read<u16>(kU1InstInfoIndex);
            instance.flags     = entry.read<u16>(kU1InstFlags);
            instance.record    = (u32)i;
            instance.transform = ReadTransform(entry, kU1InstRotation, kU1InstTranslation);
            instance.worldBounds.min = { (float)entry.read<i16>(kU1InstBounds),
                                         (float)entry.read<i16>(kU1InstBounds + 2),
                                         (float)entry.read<i16>(kU1InstBounds + 4) };
            instance.worldBounds.max = { (float)entry.read<i16>(kU1InstBounds + 6),
                                         (float)entry.read<i16>(kU1InstBounds + 8),
                                         (float)entry.read<i16>(kU1InstBounds + 10) };
            // Whole units, rounded either way: a unit of margin keeps the
            // culling from clipping what pokes past the stored box.
            instance.worldBounds.min = instance.worldBounds.min - Vec3{ 1.0f, 1.0f, 1.0f };
            instance.worldBounds.max = instance.worldBounds.max + Vec3{ 1.0f, 1.0f, 1.0f };
            if (instance.infoIndex >= out.infos.size()) continue;
            out.instances.push_back(instance);
        }
        return !out.instances.empty();
    }

    const size_t infoCount = infoData.size / kInfoEntrySize;
    out.infos.reserve(infoCount);
    for (size_t i = 0; i < infoCount; ++i) {
        const ByteSpan entry = infoData.sub(i * kInfoEntrySize, kInfoEntrySize);

        SceneryInfo info;
        info.name      = ReadName(entry, kInfoName, kInfoNameSize);
        info.solidHash = entry.read<u32>(kInfoSolidHash);
        info.radius    = entry.read<float>(kInfoRadius);
        info.raw.assign(entry.data, entry.data + entry.size);
        out.infos.push_back(std::move(info));
    }

    const size_t instanceCount = instanceData.size / kInstanceEntrySize;
    out.instances.reserve(instanceCount);
    for (size_t i = 0; i < instanceCount; ++i) {
        const ByteSpan entry = instanceData.sub(i * kInstanceEntrySize, kInstanceEntrySize);

        SceneryInstance instance;
        instance.infoIndex        = infoBase + entry.read<u16>(kInstInfoIndex);
        instance.flags            = entry.read<u16>(kInstFlags);
        instance.record           = (u32)i;
        instance.transform        = ReadTransform(entry);
        instance.worldBounds.min  = ReadVec3(entry, kInstBoundsMin);
        instance.worldBounds.max  = ReadVec3(entry, kInstBoundsMax);

        if (instance.infoIndex >= out.infos.size()) continue;  // corrupt reference
        out.instances.push_back(instance);
    }

    return !out.instances.empty();
}

} // namespace nfs
