#include "nfs/SectionTable.h"

#include <cstring>

#include "core/Log.h"
#include "nfs/GameFormat.h"

#include <algorithm>

namespace nfs {

namespace {

constexpr size_t kSectionEntrySize = 80;

// Field offsets inside one table entry.
constexpr size_t kOffName         = 0x00;  // four ASCII characters, zero padded
constexpr size_t kOffNumber       = 0x08;
constexpr size_t kOffStreamOffset = 0x14;
constexpr size_t kOffStreamSize   = 0x18;
constexpr size_t kOffGeometrySize = 0x1C;
constexpr size_t kOffOrder        = 0x20;
constexpr size_t kOffPosition     = 0x24;
constexpr size_t kOffHash         = 0x30;

// Underground 1 keeps its table in the track bundle (TRACKB<n>.LZC), chunk
// 0x00034107: 56 bytes {char tag[8], u32 number, 0, u32 type (0 for the
// "--" entry, else 1), offset, size, size without the texture data, a byte
// sum of the section, 5 x 0}. No position, no ordering key. A number with
// bit 16 set is a section that track leaves out; the bit is dropped, since
// the editor shows the whole region.
constexpr u32    kChunkSectionTableU1  = 0x00034107;
constexpr size_t kSectionEntrySizeU1   = 56;

// --- NFS World: 208 bytes per entry (measured on L5RA.BUN, 5789 entries) ------
// The tag is up to eight characters; numbered sections (A..G, Z) are the file
// STREAML5RA_<number>.BUN and the shared assets (U..Y) STREAML5RA_0x<hash>.BUN.
constexpr size_t kWorldEntrySize   = 208;
constexpr size_t kWorldOffNumber   = 0x08;
constexpr size_t kWorldOffFileHash = 0x1C;
constexpr size_t kWorldOffPosition = 0x28;
constexpr size_t kWorldOffParts    = 0x5C;
constexpr size_t kWorldOffSize     = 0x60;

std::string DecodeName(u32 packed) {
    char text[5] = {};
    for (int i = 0; i < 4; ++i) {
        const char c = (char)((packed >> (i * 8)) & 0xFF);
        if (c == '\0') break;
        text[i] = c;
    }
    return text;
}

} // namespace

bool SectionTable::Parse(ByteSpan bunFile) {
    sections_.clear();

    ChunkReader reader(bunFile, 0);
    Chunk chunk;
    ByteSpan table;
    u64 chunkDataOffset = 0;

    bool underground1 = false;
    while (reader.Next(chunk)) {
        if (chunk.id == kChunkSectionTable || chunk.id == kChunkSectionTableU1) {
            table = chunk.data;
            chunkDataOffset = chunk.dataOffset;
            underground1 = chunk.id == kChunkSectionTableU1;
            break;
        }
    }

    if (table.empty()) {
        LOG_ERROR("section table (chunk %08X) not found", (unsigned)kChunkSectionTable);
        return false;
    }

    if (IsWorld()) {
        const size_t worldCount = table.size / kWorldEntrySize;
        sections_.reserve(worldCount);
        for (size_t i = 0; i < worldCount; ++i) {
            const ByteSpan entry = table.sub(i * kWorldEntrySize, kWorldEntrySize);

            Section section;
            section.tableEntryOffset = chunkDataOffset + i * kWorldEntrySize;
            section.id         = entry.read<u32>(kOffName);
            section.name       = std::string((const char*)entry.data,
                                             ::strnlen((const char*)entry.data, 8));
            section.number     = entry.read<u32>(kWorldOffNumber);
            section.hash       = entry.read<u32>(kWorldOffFileHash);
            section.streamSize = entry.read<u32>(kWorldOffSize);
            section.streamParts = entry.read<u32>(kWorldOffParts);
            section.geometrySize = section.streamSize;
            section.position   = { entry.read<float>(kWorldOffPosition),
                                   entry.read<float>(kWorldOffPosition + 4),
                                   entry.read<float>(kWorldOffPosition + 8) };

            wchar_t file[64];
            if (section.hash) ::swprintf_s(file, L"STREAML5RA_0x%08X.BUN", section.hash);
            else              ::swprintf_s(file, L"STREAML5RA_%u.BUN", section.number);
            section.fileName = file;

            sections_.push_back(std::move(section));
        }
        LOG_INFO("section table: %zu sections (NFS World)", sections_.size());
        return !sections_.empty();
    }

    if (underground1) {
        const size_t count = table.size / kSectionEntrySizeU1;
        size_t trackOwn = 0;
        sections_.reserve(count);
        for (size_t i = 0; i < count; ++i) {
            const ByteSpan entry = table.sub(i * kSectionEntrySizeU1, kSectionEntrySizeU1);
            // Type 2 lives in the track's own STREAM<region>_<track>.BUN,
            // not in the region's stream these offsets would be read from.
            if (entry.read<u32>(0x10) == 2) { ++trackOwn; continue; }
            Section section;
            section.tableEntryOffset = chunkDataOffset + i * kSectionEntrySizeU1;
            section.id           = entry.read<u32>(kOffName);
            section.name         = DecodeName(section.id);
            section.number       = entry.read<u32>(kOffNumber) & 0xFFFFu;
            section.streamOffset = entry.read<u32>(kOffStreamOffset);
            section.streamSize   = entry.read<u32>(kOffStreamSize);
            section.geometrySize = entry.read<u32>(kOffGeometrySize);
            section.originalStreamOffset = section.streamOffset;
            section.originalStreamSize   = section.streamSize;
            section.originalOtherSize    = section.geometrySize;
            sections_.push_back(std::move(section));
        }
        LOG_INFO("section table (Underground 1): %zu sections, %zu of the track's own stream left out",
                 sections_.size(), trackOwn);
        return !sections_.empty();
    }

    const size_t count = table.size / kSectionEntrySize;
    sections_.reserve(count);

    for (size_t i = 0; i < count; ++i) {
        const ByteSpan entry = table.sub(i * kSectionEntrySize, kSectionEntrySize);

        Section section;
        section.tableEntryOffset = chunkDataOffset + i * kSectionEntrySize;
        section.id           = entry.read<u32>(kOffName);
        section.name         = DecodeName(section.id);
        section.number       = entry.read<u32>(kOffNumber);
        section.streamOffset = entry.read<u32>(kOffStreamOffset);
        section.streamSize   = entry.read<u32>(kOffStreamSize);
        section.geometrySize = entry.read<u32>(kOffGeometrySize);
        section.order        = entry.read<u32>(kOffOrder);
        section.hash         = entry.read<u32>(kOffHash);
        section.position     = { entry.read<float>(kOffPosition),
                                 entry.read<float>(kOffPosition + 4),
                                 entry.read<float>(kOffPosition + 8) };

        section.originalStreamOffset = section.streamOffset;
        section.originalStreamSize   = section.streamSize;
        section.originalOtherSize    = section.geometrySize;

        sections_.push_back(std::move(section));
    }

    LOG_INFO("section table: %zu sections", sections_.size());
    return !sections_.empty();
}

void SectionTable::SetPosition(size_t index, const Vec3& position) {
    if (index < sections_.size()) sections_[index].position = position;
}

void SectionTable::ApplyBytes(u64 fileOffset, const u8* data, size_t size) {
    if (IsWorld()) return;   // NFS World sections are files; nothing is repointed
    for (Section& section : sections_) {
        if (!section.tableEntryOffset) continue;

        const u64 entryBegin = section.tableEntryOffset;
        const u64 entryEnd   = entryBegin + kSectionEntrySize;
        if (fileOffset + size <= entryBegin || fileOffset >= entryEnd) continue;

        u8 entry[kSectionEntrySize];
        std::memcpy(entry, &section.id, 0);   // silence an unused warning on some builds

        // Rebuild the entry's bytes from what was parsed, lay the patch over
        // them, and read the fields back.
        std::memset(entry, 0, sizeof(entry));
        std::memcpy(entry + kOffStreamOffset, &section.streamOffset, 4);
        std::memcpy(entry + kOffStreamSize, &section.streamSize, 4);
        std::memcpy(entry + kOffGeometrySize, &section.geometrySize, 4);

        const u64 from = fileOffset > entryBegin ? fileOffset : entryBegin;
        const u64 to   = fileOffset + size < entryEnd ? fileOffset + size : entryEnd;
        std::memcpy(entry + (from - entryBegin), data + (from - fileOffset),
                    (size_t)(to - from));

        std::memcpy(&section.streamOffset, entry + kOffStreamOffset, 4);
        std::memcpy(&section.streamSize, entry + kOffStreamSize, 4);
        std::memcpy(&section.geometrySize, entry + kOffGeometrySize, 4);

        LOG_INFO("section %s repointed to %u, %u bytes", section.name.c_str(),
                 section.streamOffset, section.streamSize);
    }
}

const Section* SectionTable::FindByName(const std::string& name) const {
    for (const Section& section : sections_)
        if (section.name == name) return &section;
    return nullptr;
}

const Section* SectionTable::FindByNumber(u32 number) const {
    for (const Section& section : sections_)
        if (section.number == number) return &section;
    return nullptr;
}

namespace {

bool IsPositioned(const Section& section) {
    // The first entries describe global data (textures, shared geometry, the
    // backdrop) and carry no position; including them would drag any box built
    // from the table towards the origin.
    return section.position.x != 0.0f || section.position.y != 0.0f;
}

} // namespace

Aabb SectionTable::Bounds() const {
    Aabb bounds;
    for (const Section& section : sections_)
        if (IsPositioned(section)) bounds.Add(section.position);
    return bounds;
}

float SectionTable::TypicalSectionSpacing() const {
    std::vector<float> nearest;
    nearest.reserve(sections_.size());

    for (const Section& section : sections_) {
        if (!IsPositioned(section)) continue;

        float best = 0.0f;
        for (const Section& other : sections_) {
            if (&other == &section || !IsPositioned(other)) continue;
            const float distance = Length(other.position - section.position);
            if (distance > 0.0f && (best == 0.0f || distance < best)) best = distance;
        }
        if (best > 0.0f) nearest.push_back(best);
    }

    if (nearest.empty()) return 0.0f;

    const size_t middle = nearest.size() / 2;
    std::nth_element(nearest.begin(), nearest.begin() + middle, nearest.end());
    return nearest[middle];
}

} // namespace nfs
