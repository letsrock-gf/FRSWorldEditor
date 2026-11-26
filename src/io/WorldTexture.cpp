#include "io/WorldTexture.h"

#include "core/Log.h"
#include "io/TextureImport.h"
#include "nfs/ChunkIds.h"

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <unordered_map>

#include <windows.h>

// What is written here was measured on the shipped files (tools/nfsw/
// tex_layout_stats.py), on every third section file of the map:
//
//   pack 0xB3300000
//     0xB3310000 > 0x33310004   one header per texture: 0x90 bytes, a length
//                               byte and the name. Size of all levels at +0x18
//                               and +0x4C (made up to 16 bytes), of the largest
//                               at +0x1C, width and height at +0x20/+0x24 and
//                               as powers of two at +0x28/+0x2C, level count at
//                               +0x31, where its pixels begin at +0x50.
//     0xB3320000 > 0x33320002   filler to a 128-byte boundary of the file, then
//                               the pixels, each texture on a 128-byte boundary.
//   larger levels 0xB3300011
//     0x33310022                32-byte rows: hash, offset, size, largest
//                               level's bytes, u16 width, u16 height, the two
//                               powers, level count. One level is stored, the
//                               size and the count being those of the chain
//                               from that level down to the pack's last.
//     0x33320002                as in the pack.

namespace io {

namespace {

using Patch = nfs::LoadedSection::BytePatch;

u32 ReadU32(const std::vector<u8>& bytes, size_t at) {
    u32 value = 0;
    std::memcpy(&value, bytes.data() + at, sizeof(value));
    return value;
}

void WriteU32(std::vector<u8>& bytes, size_t at, u32 value) { std::memcpy(bytes.data() + at, &value, sizeof(value)); }
void WriteU16(std::vector<u8>& bytes, size_t at, u16 value) { std::memcpy(bytes.data() + at, &value, sizeof(value)); }

size_t Align128(size_t value) { return (value + 127) & ~(size_t)127; }

u32 PowerOf(u32 size) {
    u32 power = 0;
    while ((1u << (power + 1)) <= size) ++power;
    return power;
}

// Bytes of `levels` levels from width x height down.
u32 ChainBytes(u32 width, u32 height, u32 levels, u32 blockBytes) {
    u32 total = 0;
    for (u32 level = 0; level < levels; ++level)
        total += std::max(1u, ((width >> level) + 3) / 4) * std::max(1u, ((height >> level) + 3) / 4) * blockBytes;
    return total;
}

struct Node {
    u32    id = 0;
    size_t header = 0;          // where the chunk's id is
    size_t begin = 0, end = 0;  // its payload
};

// The chunks lying directly in [begin, end).
std::vector<Node> Children(const std::vector<u8>& bytes, size_t begin, size_t end) {
    std::vector<Node> nodes;
    size_t at = begin;
    while (at + 8 <= end) {
        Node node;
        node.id     = ReadU32(bytes, at);
        node.header = at;
        node.begin  = at + 8;
        node.end    = node.begin + ReadU32(bytes, at + 4);
        if (node.end > end) break;
        nodes.push_back(node);
        at = node.end;
    }
    return nodes;
}

const Node* Find(const std::vector<Node>& nodes, u32 id) {
    for (const Node& node : nodes)
        if (node.id == id) return &node;
    return nullptr;
}

// One texture's pixels in a data chunk, before and after.
struct Piece {
    size_t       entry = 0;     // its header or row in the file
    size_t       oldOffset = 0, oldSize = 0;
    const Patch* patch = nullptr;
    size_t       newOffset = 0, newSize = 0;
};

// Writes the pieces' pixels into the data chunk `pixels` anew, each on a
// 128-byte boundary, and grows the chunk and the chunks it lies in (`within`,
// their headers) by a multiple of 128 when they no longer fit. Returns the
// growth, or false through `ok`.
size_t LayOut(std::vector<u8>& bytes, const Node& pixels, const std::vector<size_t>& within,
              std::vector<Piece>& pieces, bool& ok) {
    const size_t dataStart = Align128(pixels.begin);
    if (dataStart > pixels.end) { ok = false; return 0; }
    const size_t oldLength = pixels.end - dataStart;

    std::vector<Piece*> order;
    for (Piece& piece : pieces) order.push_back(&piece);
    std::sort(order.begin(), order.end(), [](const Piece* a, const Piece* b) { return a->oldOffset < b->oldOffset; });

    size_t running = 0;
    for (Piece* piece : order) {
        if (piece->oldOffset + piece->oldSize > oldLength) { ok = false; return 0; }
        piece->newOffset = Align128(running);
        piece->newSize   = piece->patch ? piece->patch->bytes.size() : piece->oldSize;
        running = piece->newOffset + piece->newSize;
    }
    const size_t growth = running > oldLength ? Align128(running - oldLength) : 0;

    std::vector<u8> region(oldLength + growth, 0);
    for (const Piece* piece : order) {
        const u8* from = piece->patch ? piece->patch->bytes.data() : bytes.data() + dataStart + piece->oldOffset;
        std::memcpy(region.data() + piece->newOffset, from, piece->newSize);
    }
    bytes.insert(bytes.begin() + (std::ptrdiff_t)pixels.end, growth, 0);
    std::memcpy(bytes.data() + dataStart, region.data(), region.size());

    if (growth) {
        WriteU32(bytes, pixels.header + 4, ReadU32(bytes, pixels.header + 4) + (u32)growth);
        for (size_t header : within) WriteU32(bytes, header + 4, ReadU32(bytes, header + 4) + (u32)growth);
    }
    return growth;
}

} // namespace

bool ResizeTextures(const nfs::LoadedSection& loaded, std::vector<u8>& bytes, std::wstring& error) {
    std::vector<const Patch*> resized;
    for (const auto& patch : loaded.texturePatches)
        if (patch->Resized()) resized.push_back(patch.get());
    if (resized.empty()) return true;

    const auto patchAt = [&](size_t shippedOffset) -> const Patch* {
        for (const Patch* patch : resized)
            if (patch->offset == shippedOffset) return patch;
        return nullptr;
    };
    const auto fail = [&]() {
        error = L"A texture pack of " + std::wstring(loaded.info->fileName) +
                L" is not laid out as the editor knows them. Nothing was written.";
        return false;
    };

    size_t placed = 0;
    size_t shift = 0;      // bytes added so far before the chunk in hand
    size_t at = 0;
    while (at + 8 <= bytes.size()) {
        const u32 id = ReadU32(bytes, at);
        const size_t end = at + 8 + ReadU32(bytes, at + 4);
        if (end > bytes.size()) break;
        size_t growth = 0;

        if (id == nfs::kChunkTexturePack) {
            const std::vector<Node> parts = Children(bytes, at + 8, end);
            const Node* info = Find(parts, nfs::kChunkTpkInfoBlock);
            const Node* data = Find(parts, nfs::kChunkTpkDataBlock);
            if (info && data) {
                // Both lists are kept: Find answers with a pointer into the one it is given.
                const std::vector<Node> infoParts = Children(bytes, info->begin, info->end);
                const std::vector<Node> dataParts = Children(bytes, data->begin, data->end);
                const Node* headers = Find(infoParts, nfs::kChunkTpkTextureHeaders);
                const Node* pixels = Find(dataParts, nfs::kChunkTpkPixelData);
                if (headers && pixels) {
                    const Node headerNode = *headers, pixelNode = *pixels;
                    const size_t dataStart = Align128(pixelNode.begin);
                    std::vector<Piece> pieces;
                    bool any = false;
                    for (size_t entry = headerNode.begin; entry + 0x91 <= headerNode.end;
                         entry += 0x91 + bytes[entry + 0x90]) {
                        Piece piece;
                        piece.entry     = entry;
                        piece.oldSize   = ReadU32(bytes, entry + 0x4C);
                        piece.oldOffset = ReadU32(bytes, entry + 0x50);
                        piece.patch     = patchAt(dataStart - shift + piece.oldOffset);
                        any = any || piece.patch;
                        pieces.push_back(piece);
                    }
                    if (any) {
                        bool ok = true;
                        growth = LayOut(bytes, pixelNode, { at, data->header }, pieces, ok);
                        if (!ok) return fail();
                        for (const Piece& piece : pieces) {
                            WriteU32(bytes, piece.entry + 0x50, (u32)piece.newOffset);
                            if (!piece.patch) continue;
                            const Patch& patch = *piece.patch;
                            WriteU32(bytes, piece.entry + 0x18, (u32)piece.newSize);
                            WriteU32(bytes, piece.entry + 0x4C, (u32)piece.newSize);
                            WriteU32(bytes, piece.entry + 0x1C, patch.base);
                            WriteU32(bytes, piece.entry + 0x20, patch.width);
                            WriteU32(bytes, piece.entry + 0x24, patch.height);
                            WriteU32(bytes, piece.entry + 0x28, PowerOf(patch.width));
                            WriteU32(bytes, piece.entry + 0x2C, PowerOf(patch.height));
                            bytes[piece.entry + 0x31] = patch.mipCount;
                            ++placed;
                        }
                    }
                }
            }
        } else if (id == nfs::kChunkWorldStreamMips) {
            const std::vector<Node> parts = Children(bytes, at + 8, end);
            const Node* rows   = Find(parts, nfs::kChunkWorldStreamMipList);
            const Node* pixels = Find(parts, nfs::kChunkTpkPixelData);
            if (rows && pixels) {
                const Node rowNode = *rows, pixelNode = *pixels;
                const size_t dataStart = Align128(pixelNode.begin);
                std::vector<Piece> pieces;
                bool any = false;
                for (size_t row = rowNode.begin; row + 32 <= rowNode.end; row += 32) {
                    Piece piece;
                    piece.entry     = row;
                    piece.oldOffset = ReadU32(bytes, row + 4);
                    piece.oldSize   = ReadU32(bytes, row + 12);
                    piece.patch     = patchAt(dataStart - shift + piece.oldOffset);
                    any = any || piece.patch;
                    pieces.push_back(piece);
                }
                if (any) {
                    bool ok = true;
                    growth = LayOut(bytes, pixelNode, { at }, pieces, ok);
                    if (!ok) return fail();
                    for (const Piece& piece : pieces) {
                        WriteU32(bytes, piece.entry + 4, (u32)piece.newOffset);
                        if (!piece.patch) continue;
                        const Patch& patch = *piece.patch;
                        const u32 levels = ReadU32(bytes, piece.entry + 28);
                        WriteU32(bytes, piece.entry + 8,
                                 (ChainBytes(patch.width, patch.height, levels, patch.blockBytes) + 15) & ~15u);
                        WriteU32(bytes, piece.entry + 12, (u32)piece.newSize);
                        WriteU16(bytes, piece.entry + 16, patch.width);
                        WriteU16(bytes, piece.entry + 18, patch.height);
                        WriteU32(bytes, piece.entry + 20, PowerOf(patch.width));
                        WriteU32(bytes, piece.entry + 24, PowerOf(patch.height));
                        ++placed;
                    }
                }
            }
        }

        shift += growth;
        at = end + growth;
    }

    if (placed != resized.size()) return fail();
    return true;
}

// --- new textures --------------------------------------------------------------

namespace {

constexpr u32 kFourCcDxt1 = 0x31545844, kFourCcDxt5 = 0x35545844;
constexpr u32 kMaxNewSide = 512;
constexpr size_t kTexHeader = 0x90;   // then the length byte and the name

u32 NameHash(const std::string& text) {
    u32 hash = 0xFFFFFFFFu;
    for (unsigned char c : text) hash = hash * 33u + c;
    return hash;
}

// The power of two nearest a side of the image, 4..512 (or the cap the edit list names for the texture).
u32 SideFor(u32 size, u32 cap = kMaxNewSide) {
    u32 side = 4;
    while (side < cap && side * 2 <= size + size / 2) side *= 2;
    return side;
}

bool IsTextureSection(const nfs::LoadedSection& loaded) {
    return loaded.info && !loaded.info->name.empty() && loaded.info->name[0] >= 'U' && loaded.info->name[0] <= 'Y';
}

bool PackHolds(const nfs::LoadedSection& loaded, u32 hash) {
    for (const auto& pack : loaded.texturePacks)
        if (pack->Find(hash)) return true;
    return false;
}

std::wstring HexOf(u32 value) {
    wchar_t text[16];
    ::_snwprintf_s(text, _TRUNCATE, L"%08X", value);
    return text;
}

void WriteVecU32(std::vector<u8>& bytes, size_t at, u32 value) { std::memcpy(bytes.data() + at, &value, 4); }

// New textures given a texture section of their own by the edit list (X line, fifth field): hash -> section number.
// ponytail: kept here, not in World::NewTexture, so the shared header nfs/World.h stays as it is.
std::unordered_map<u32, u32>& GivenHosts() {
    static std::unordered_map<u32, u32> hosts;
    return hosts;
}

// Whether the pack of this section takes the texture: the section the list names for it, else one that holds
// a texture it copies.
bool HostedBy(const nfs::World::NewTexture& texture, const nfs::LoadedSection& loaded) {
    const auto given = GivenHosts().find(texture.hash);
    if (given != GivenHosts().end()) return loaded.info && loaded.info->number == given->second;
    if (PackHolds(loaded, texture.like)) return true;
    for (const u32 like : texture.moreLikes)
        if (PackHolds(loaded, like)) return true;
    return false;
}

} // namespace

const nfs::World::NewTexture* NewTextureOf(const nfs::World& world, u32 hash) {
    for (const auto& texture : world.newTextures)
        if (texture.hash == hash) return &texture;
    return nullptr;
}

std::string NewTextureName(const nfs::World& world, const std::string& wanted) {
    std::string base = "FRS_";
    size_t from = 0;
    if (wanted.size() > 4 && ::_strnicmp(wanted.c_str(), "FRS_", 4) == 0) from = 4;
    for (size_t i = from; i < wanted.size(); ++i) {
        const char c = wanted[i];
        const bool plain = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9');
        base.push_back(plain ? (char)::toupper((unsigned char)c) : '_');
    }
    if (base.size() > 31) base.resize(31);
    std::string name = base;
    for (int n = 2;; ++n) {
        const u32 hash = NameHash(name);
        const nfs::World::NewTexture* mine = NewTextureOf(world, hash);
        if (mine ? mine->name == name : !world.FindTexture(hash)) return name;
        const std::string number = "_" + std::to_string(n);
        name = base.substr(0, 31 - number.size()) + number;
    }
}

void SetNewTextureHost(u32 hash, u32 section) {
    if (section) GivenHosts()[hash] = section;
    else GivenHosts().erase(hash);
}

u32 NewTextureHost(u32 hash) {
    const auto given = GivenHosts().find(hash);
    return given == GivenHosts().end() ? 0u : given->second;
}

// ponytail: beside GivenHosts for the same reason (nfs/World.h untouched).
static std::unordered_map<u32, u32>& GivenSides() {
    static std::unordered_map<u32, u32> sides;
    return sides;
}

void SetNewTextureSide(u32 hash, u32 side) {
    if (side == 1024 || side == 2048) GivenSides()[hash] = side;
    else GivenSides().erase(hash);
}

u32 NewTextureSide(u32 hash) {
    const auto given = GivenSides().find(hash);
    return given == GivenSides().end() ? 0u : given->second;
}

std::vector<const nfs::LoadedSection*> TextureHosts(const nfs::World& world, u32 like) {
    std::vector<const nfs::LoadedSection*> hosts;
    for (const auto& section : world.Loaded())
        if (IsTextureSection(*section) && PackHolds(*section, like)) hosts.push_back(section.get());
    return hosts;
}

bool HostsNewTextures(const nfs::World& world, const nfs::LoadedSection& loaded) {
    if (!IsTextureSection(loaded)) return false;
    for (const auto& texture : world.newTextures)
        if (HostedBy(texture, loaded)) return true;
    return false;
}

std::vector<const nfs::LoadedSection*> NewTextureHosts(const nfs::World& world, const nfs::World::NewTexture& texture) {
    std::vector<const nfs::LoadedSection*> hosts;
    for (const auto& section : world.Loaded())
        if (IsTextureSection(*section) && HostedBy(texture, *section)) hosts.push_back(section.get());
    return hosts;
}

bool StageCopiedWorldTexture(nfs::World& world, u32 hash, u32 like, std::wstring& error) {
    if (const nfs::World::NewTexture* have = NewTextureOf(world, hash)) {
        auto* entry = const_cast<nfs::World::NewTexture*>(have);
        // A texture made by the editor goes, the same way, beside `like` too.
        if (entry->like != like && std::find(entry->moreLikes.begin(), entry->moreLikes.end(), like) == entry->moreLikes.end())
            entry->moreLikes.push_back(like);
        return true;
    }
    const nfs::Texture* base = world.FindTexture(hash);
    if (!base || base->pixels.empty()) { error = L"texture " + HexOf(hash) + L" is not loaded"; return false; }
    if (TextureHosts(world, like).empty()) { error = L"texture " + HexOf(like) + L" is in no texture section"; return false; }

    // Its header as shipped, from a pack that holds it.
    std::vector<u8> header;
    for (const auto& section : world.Loaded()) {
        if (!section->info || !PackHolds(*section, hash)) continue;
        const ByteSpan shipped = world.SectionBytes(*section->info);
        if (shipped.empty()) continue;
        const std::vector<u8> file(shipped.data, shipped.data + shipped.size);
        for (const Node& pack : Children(file, 0, file.size())) {
            if (pack.id != nfs::kChunkTexturePack) continue;
            const std::vector<Node> parts = Children(file, pack.begin, pack.end);
            const Node* info = Find(parts, nfs::kChunkTpkInfoBlock);
            if (!info) continue;
            const std::vector<Node> infoParts = Children(file, info->begin, info->end);
            const Node* headers = Find(infoParts, nfs::kChunkTpkTextureHeaders);
            if (!headers) continue;
            for (size_t at = headers->begin; at + kTexHeader + 1 <= headers->end; at += kTexHeader + 1 + file[at + kTexHeader])
                if (ReadU32(file, at + 0x0C) == hash) {
                    header.assign(file.begin() + (std::ptrdiff_t)at, file.begin() + (std::ptrdiff_t)(at + kTexHeader));
                    break;
                }
            if (!header.empty()) break;
        }
        if (!header.empty()) break;
    }
    if (header.empty()) { error = L"the header of texture " + HexOf(hash) + L" was not found"; return false; }

    // Its whole chain where the larger levels are loaded, at most 512 a side.
    nfs::Texture full = *base;
    std::vector<u8> chain;
    if (!world.FullTexture(hash, full, chain)) chain.assign(base->pixels.begin(), base->pixels.end());
    const u32 block = (full.fourCc == 0x31545844 || full.fourCc == 0x31495441) ? 8u : 16u;   // DXT1, ATI1
    u32 width = full.width, height = full.height, mips = std::max<u32>(full.mipCount, 1);
    size_t skip = 0;
    while (mips > 1 && std::max(width, height) > kMaxNewSide) {
        skip += (size_t)std::max(1u, (width + 3) / 4) * std::max(1u, (height + 3) / 4) * block;
        width = std::max(1u, width / 2);
        height = std::max(1u, height / 2);
        --mips;
    }
    const size_t bytes = ChainBytes(width, height, mips, block);
    if (skip + bytes > chain.size()) { error = L"texture " + HexOf(hash) + L" has fewer pixels than its levels"; return false; }

    nfs::World::NewTexture& entry = world.newTextures.emplace_back();
    entry.hash      = hash;
    entry.like      = like;
    entry.name      = base->name;
    entry.width     = width;
    entry.height    = height;
    entry.mips      = mips;
    entry.fourCc    = full.fourCc;
    entry.baseBytes = std::max(1u, (width + 3) / 4) * std::max(1u, (height + 3) / 4) * block;
    entry.pixels.assign(chain.begin() + (std::ptrdiff_t)skip, chain.begin() + (std::ptrdiff_t)(skip + bytes));
    entry.header    = std::move(header);
    LOG_INFO("texture %s %08X copied (%ux%u, %u levels) beside texture %08X", base->name.c_str(), hash, width, height,
             mips, like);
    return true;
}

bool StageCarTexture(nfs::World& world, const nfs::Texture& texture, const std::vector<u8>& header, u32 like,
                     std::wstring& error) {
    if (NewTextureOf(world, texture.hash)) return true;
    const std::vector<const nfs::LoadedSection*> hosts = TextureHosts(world, like);
    if (hosts.empty()) { error = L"texture " + HexOf(like) + L" is in no texture section"; return false; }
    // Block-compressed, or D3DFMT_A8R8G8B8 (21: 4 bytes a pixel), which some imported cars ship (458ITALIA's
    // lights, tyre, interior: sized as DXT, only a quarter of each was copied and the lights drew black, 21/11).
    const u32 block = (texture.fourCc == 0x31545844 || texture.fourCc == 0x31495441) ? 8u : 16u;   // DXT1, ATI1
    const auto levelBytes = [&](u32 w, u32 h) {
        return texture.fourCc == 21 ? (size_t)w * h * 4 : (size_t)std::max(1u, (w + 3) / 4) * std::max(1u, (h + 3) / 4) * block;
    };
    u32 width = texture.width, height = texture.height, mips = std::max<u32>(texture.mipCount, 1);
    size_t skip = 0;
    while (mips > 1 && std::max(width, height) > kMaxNewSide) {
        skip += levelBytes(width, height);
        width = std::max(1u, width / 2);
        height = std::max(1u, height / 2);
        --mips;
    }
    size_t bytes = 0;
    for (u32 m = 0, w = width, h = height; m < mips; ++m, w = std::max(1u, w / 2), h = std::max(1u, h / 2)) bytes += levelBytes(w, h);
    if (skip + bytes > texture.pixels.size) { error = L"car texture " + HexOf(texture.hash) + L" has fewer pixels than its levels"; return false; }

    nfs::World::NewTexture& entry = world.newTextures.emplace_back();
    entry.hash      = texture.hash;
    entry.like      = like;
    entry.name      = texture.name;
    entry.width     = width;
    entry.height    = height;
    entry.mips      = mips;
    entry.fourCc    = texture.fourCc;
    entry.baseBytes = (u32)levelBytes(width, height);
    entry.pixels.assign(texture.pixels.data + skip, texture.pixels.data + skip + bytes);
    entry.header    = header;
    entry.fromCar   = true;

    nfs::Texture shown = texture;
    shown.width = (u16)width;
    shown.height = (u16)height;
    shown.mipCount = (u8)mips;
    shown.baseLevelSize = entry.baseBytes;
    shown.pixels = { entry.pixels.data(), entry.pixels.size() };
    nfs::LoadedSection* first = world.MutableLoaded(0);
    for (size_t i = 0; i < world.Loaded().size(); ++i)
        if (world.Loaded()[i].get() == hosts.front()) first = world.MutableLoaded(i);
    if (first && !first->texturePacks.empty()) first->texturePacks.front()->Add(shown);
    LOG_INFO("car texture %s %08X staged (%ux%u, %u levels) beside texture %08X", texture.name.c_str(), texture.hash,
             width, height, mips, like);
    return true;
}

// textures.cache, beside the textures folder: every encoded new texture and every image's size in one file,
// read through one handle. Opening a file costs about 8 ms here whatever its size (the antivirus looks at each
// one: 1 500 opens took 12 s on 24/11), and the edit list used to open two per new texture, 6 000 at a start.
// Records one after the other, the later of two with the same key the one that counts:
//   u32 "FTK1", u32 key length, the key (wide), u64 image size, u64 image time, u32 width, u32 height,
//   u64 byte count, the bytes.
// The key is the image's file name and what was asked of it; size and time are the image's own, so an image
// written again is encoded again. A record with no bytes only says the image's width and height.
// ponytail: only grows (an image encoded again leaves its old record behind): delete the file to make it
// again from the images. One editor at a time has it: a second one finds it taken and works without.
namespace {

struct KeptTextureRecord { u64 stamp[2] = {}; u32 width = 0, height = 0; u64 at = 0, size = 0; };
struct KeptTextureFile {
    std::unordered_map<std::wstring, KeptTextureRecord> known;
    std::wstring folder;                    // the textures folder it serves: the first one asked for
    HANDLE file = INVALID_HANDLE_VALUE;
    u64 end = 0;                            // where the next record goes
    bool opened = false;
};
constexpr u32 kKeptTextureMagic = 0x314B5446;   // "FTK1"

bool ReadAt(HANDLE file, u64 at, void* out, size_t size) {
    LARGE_INTEGER where{};
    where.QuadPart = (LONGLONG)at;
    if (!::SetFilePointerEx(file, where, nullptr, FILE_BEGIN)) return false;
    for (size_t done = 0; done < size;) {
        DWORD got = 0;
        const DWORD want = (DWORD)std::min<size_t>(size - done, 1u << 24);
        if (!::ReadFile(file, (u8*)out + done, want, &got, nullptr) || got == 0) return false;
        done += got;
    }
    return true;
}

KeptTextureFile& KeptTextures(const std::filesystem::path& texturesFolder) {
    static KeptTextureFile kept;
    if (kept.opened) return kept;
    kept.opened = true;
    kept.folder = texturesFolder.wstring();
    const std::wstring path = (texturesFolder.parent_path() / L"textures.cache").wstring();
    kept.file = ::CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ, nullptr, OPEN_ALWAYS,
                              FILE_ATTRIBUTE_NORMAL, nullptr);
    if (kept.file == INVALID_HANDLE_VALUE) return kept;
    LARGE_INTEGER length{};
    ::GetFileSizeEx(kept.file, &length);
    // Whatever follows the last whole record (a write cut short) is written over.
    for (u64 at = 0; at + 8 <= (u64)length.QuadPart;) {
        u32 head[2] = {};
        if (!ReadAt(kept.file, at, head, sizeof(head)) || head[0] != kKeptTextureMagic || head[1] > 1024) break;
        std::wstring key(head[1], L'\0');
        struct { u64 stamp[2]; u32 width, height; u64 size; } body{};
        const u64 bodyAt = at + 8 + (u64)head[1] * sizeof(wchar_t);
        if (bodyAt + sizeof(body) > (u64)length.QuadPart) break;
        if (head[1] && !ReadAt(kept.file, at + 8, key.data(), head[1] * sizeof(wchar_t))) break;
        if (!ReadAt(kept.file, bodyAt, &body, sizeof(body))) break;
        const u64 next = bodyAt + sizeof(body) + body.size;
        if (next > (u64)length.QuadPart) break;
        KeptTextureRecord& record = kept.known[key];
        record.stamp[0] = body.stamp[0];
        record.stamp[1] = body.stamp[1];
        record.width    = body.width;
        record.height   = body.height;
        record.at       = bodyAt + sizeof(body);
        record.size     = body.size;
        kept.end = at = next;
    }
    LOG_INFO("textures.cache: %zu records, %llu MiB", kept.known.size(), (unsigned long long)(kept.end >> 20));
    return kept;
}

// The record of `key` for an image with this size and time, or null.
const KeptTextureRecord* KeptRecord(KeptTextureFile& kept, const std::wstring& key, const u64 stamp[2]) {
    if (kept.file == INVALID_HANDLE_VALUE) return nullptr;
    const auto found = kept.known.find(key);
    if (found == kept.known.end() || found->second.stamp[0] != stamp[0] || found->second.stamp[1] != stamp[1])
        return nullptr;
    return &found->second;
}

void KeepRecord(KeptTextureFile& kept, const std::wstring& key, const u64 stamp[2], u32 width, u32 height,
                const u8* bytes, size_t size) {
    if (kept.file == INVALID_HANDLE_VALUE) return;
    std::vector<u8> head;
    const auto put = [&head](const void* data, size_t count) {
        head.insert(head.end(), (const u8*)data, (const u8*)data + count);
    };
    const u32 lead[2] = { kKeptTextureMagic, (u32)key.size() };
    const u64 byteCount = size;
    put(lead, sizeof(lead));
    put(key.data(), key.size() * sizeof(wchar_t));
    put(stamp, 2 * sizeof(u64));
    put(&width, sizeof(width));
    put(&height, sizeof(height));
    put(&byteCount, sizeof(byteCount));

    LARGE_INTEGER where{};
    where.QuadPart = (LONGLONG)kept.end;
    DWORD wrote = 0;
    if (!::SetFilePointerEx(kept.file, where, nullptr, FILE_BEGIN) ||
        !::WriteFile(kept.file, head.data(), (DWORD)head.size(), &wrote, nullptr) || wrote != head.size())
        return;
    for (size_t done = 0; done < size;) {
        const DWORD want = (DWORD)std::min<size_t>(size - done, 1u << 24);
        if (!::WriteFile(kept.file, bytes + done, want, &wrote, nullptr) || wrote != want) return;
        done += wrote;
    }
    KeptTextureRecord& record = kept.known[key];
    record.stamp[0] = stamp[0];
    record.stamp[1] = stamp[1];
    record.width    = width;
    record.height   = height;
    record.at       = kept.end + head.size();
    record.size     = size;
    kept.end        = record.at + size;
}

// The image's size and time of writing, without opening it. False when it cannot be asked.
bool ImageStamp(const std::filesystem::path& image, u64 stamp[2]) {
    std::error_code failed;
    stamp[0] = (u64)std::filesystem::file_size(image, failed);
    stamp[1] = (u64)std::filesystem::last_write_time(image, failed).time_since_epoch().count();
    return !failed;
}

// ImageSize, asked of textures.cache first: the image itself is opened only the first time.
bool KeptImageSize(const std::wstring& image, u32& width, u32& height) {
    const std::filesystem::path source(image);
    u64 stamp[2] = {};
    if (!ImageStamp(source, stamp)) return ImageSize(image, width, height);
    KeptTextureFile& kept = KeptTextures(source.parent_path());
    if (source.parent_path().wstring() != kept.folder) return ImageSize(image, width, height);
    const std::wstring key = source.filename().wstring() + L".size";
    if (const KeptTextureRecord* record = KeptRecord(kept, key, stamp)) {
        width  = record->width;
        height = record->height;
        return width && height;
    }
    if (!ImageSize(image, width, height)) return false;
    KeepRecord(kept, key, stamp, width, height, nullptr, 0);
    return true;
}

} // namespace

// EncodeForSlot, with the result kept: in textures.cache (above), and before 24/11 beside the image in
// cache\<image>.<w>x<h>.<fourcc>.<levels>.bin (the image's size and time of writing, then the levels). Those
// files are still read, once each: what they hold goes into textures.cache. A kept image that was written
// again is compressed again.
bool EncodeCached(const std::wstring& image, u32 width, u32 height, u32 fourCc, u32 mips, std::vector<u8>& pixels,
                  std::wstring& error) {
    namespace fs = std::filesystem;
    const fs::path source(image);
    u64 stamp[2] = {};
    const bool failed = !ImageStamp(source, stamp);
    wchar_t tail[64];
    ::swprintf_s(tail, L".%ux%u.%08X.%u.bin", width, height, fourCc, mips);
    const std::wstring key = source.filename().wstring() + tail;
    KeptTextureFile* packed = nullptr;
    if (!failed) {
        KeptTextureFile& kept = KeptTextures(source.parent_path());
        if (kept.file != INVALID_HANDLE_VALUE && source.parent_path().wstring() == kept.folder) packed = &kept;
    }
    if (packed)
        if (const KeptTextureRecord* record = KeptRecord(*packed, key, stamp); record && record->size) {
            pixels.resize((size_t)record->size);
            if (ReadAt(packed->file, record->at, pixels.data(), pixels.size())) return true;
        }

    const fs::path kept = source.parent_path() / L"cache" / key;
    if (!failed) {
        std::ifstream in(kept, std::ios::binary | std::ios::ate);
        const std::streamoff size = in ? (std::streamoff)in.tellg() : 0;
        u64 had[2] = {};
        if (size > (std::streamoff)sizeof(had)) {
            in.seekg(0);
            in.read((char*)had, sizeof(had));
            if (had[0] == stamp[0] && had[1] == stamp[1]) {
                pixels.resize((size_t)size - sizeof(had));
                in.read((char*)pixels.data(), (std::streamsize)pixels.size());
                if (in) {
                    if (packed) KeepRecord(*packed, key, stamp, width, height, pixels.data(), pixels.size());
                    return true;
                }
            }
        }
    }
    if (!EncodeForSlot(image, width, height, fourCc, mips, pixels, error)) return false;
    if (packed) {
        KeepRecord(*packed, key, stamp, width, height, pixels.data(), pixels.size());
    } else if (!failed) {
        std::error_code ignored;
        fs::create_directories(kept.parent_path(), ignored);
        std::ofstream out(kept, std::ios::binary | std::ios::trunc);
        out.write((const char*)stamp, sizeof(stamp));
        out.write((const char*)pixels.data(), (std::streamsize)pixels.size());
    }
    return true;
}

bool StageNewWorldTexture(nfs::World& world, const std::string& name, u32 like, const std::wstring& keptFile,
                          std::wstring& error) {
    const std::vector<const nfs::LoadedSection*> hosts = TextureHosts(world, like);
    const nfs::Texture* model = world.FindTexture(like);
    if (hosts.empty() || !model) {
        error = L"texture " + HexOf(like) + L" is in no texture section the editor can add to";
        return false;
    }
    u32 imageWidth = 0, imageHeight = 0;
    if (!KeptImageSize(keptFile, imageWidth, imageHeight)) {
        error = L"could not read the image " + keptFile;
        return false;
    }
    const u32 cap = std::max(kMaxNewSide, NewTextureSide(NameHash(name)));
    const u32 width = SideFor(imageWidth, cap), height = SideFor(imageHeight, cap);
    u32 mips = 1;
    while ((std::min(width, height) >> mips) >= 4) ++mips;
    // The alpha is opacity only where the texture copied says so.
    const bool alpha = (model->format == nfs::TextureFormat::Dxt5 || model->format == nfs::TextureFormat::Dxt3) &&
                       nfs::AlphaIsOpacity(*model);
    const u32 fourCc = alpha ? kFourCcDxt5 : kFourCcDxt1;
    std::vector<u8> pixels;
    if (!EncodeCached(keptFile, width, height, fourCc, mips, pixels, error)) return false;

    const u32 hash = NameHash(name);
    auto* entry = const_cast<nfs::World::NewTexture*>(NewTextureOf(world, hash));
    if (!entry) entry = &world.newTextures.emplace_back();
    const size_t slash = keptFile.find_last_of(L"\\/");
    entry->hash      = hash;
    entry->like      = like;
    entry->name      = name;
    entry->file      = keptFile.substr(slash == std::wstring::npos ? 0 : slash + 1);
    entry->width     = width;
    entry->height    = height;
    entry->mips      = mips;
    entry->fourCc    = fourCc;
    entry->baseBytes = std::max(1u, (width + 3) / 4) * std::max(1u, (height + 3) / 4) * (alpha ? 16u : 8u);
    entry->pixels    = std::move(pixels);

    // In memory, in the first host's pack, for the view.
    nfs::Texture shown = *model;   // its alpha usage and blend
    shown.name          = name;
    shown.hash          = hash;
    shown.width         = (u16)width;
    shown.height        = (u16)height;
    shown.mipCount      = (u8)mips;
    shown.format        = alpha ? nfs::TextureFormat::Dxt5 : nfs::TextureFormat::Dxt1;
    shown.fourCc        = fourCc;
    shown.scrollType    = 0;
    shown.baseLevelSize = entry->baseBytes;
    shown.pixels        = { entry->pixels.data(), entry->pixels.size() };
    shown.palette       = {};
    for (size_t i = 0; i < world.Loaded().size(); ++i) {
        nfs::LoadedSection* section = world.MutableLoaded(i);
        if (!section) continue;
        for (auto& pack : section->texturePacks) pack->Remove(hash);
        if (section == hosts.front() && !section->texturePacks.empty()) section->texturePacks.front()->Add(shown);
    }
    LOG_INFO("new texture %s %08X staged: %ux%u %s, %u levels, like %08X in %zu texture sections (first %s)",
             name.c_str(), hash, width, height, alpha ? "DXT5" : "DXT1", mips, like, hosts.size(),
             hosts.front()->info->name.c_str());
    return true;
}

// The header (0x90 bytes, length byte, name) and the 32-byte format row of a shipped texture, from the first
// texture section whose pack holds it, as shipped. For a new texture whose own section does not hold what it copies.
bool ShippedTextureRows(const nfs::World& world, u32 hash, std::vector<u8>& header, std::vector<u8>& format) {
    const std::vector<const nfs::LoadedSection*> hosts = TextureHosts(world, hash);
    if (hosts.empty() || !hosts.front()->info) return false;
    const ByteSpan shipped = world.SectionBytes(*hosts.front()->info);
    const std::vector<u8> bytes(shipped.data, shipped.data + shipped.size);
    const std::vector<Node> top = Children(bytes, 0, bytes.size());
    const Node* pack = Find(top, nfs::kChunkTexturePack);
    if (!pack) return false;
    const std::vector<Node> parts = Children(bytes, pack->begin, pack->end);
    const Node* info = Find(parts, nfs::kChunkTpkInfoBlock);
    if (!info) return false;
    const std::vector<Node> infoParts = Children(bytes, info->begin, info->end);
    const Node* hashes  = Find(infoParts, nfs::kChunkTpkHashTable);
    const Node* headers = Find(infoParts, nfs::kChunkTpkTextureHeaders);
    const Node* formats = Find(infoParts, nfs::kChunkTpkFormatTable);
    if (!hashes || !headers || !formats) return false;
    size_t at = headers->begin;
    for (size_t i = 0; i < (hashes->end - hashes->begin) / 8; ++i) {
        if (at + kTexHeader + 1 > headers->end) return false;
        const size_t size = kTexHeader + 1 + bytes[at + kTexHeader];
        if (at + size > headers->end || formats->begin + (i + 1) * 32 > formats->end) return false;
        if (ReadU32(bytes, hashes->begin + i * 8) == hash) {
            header.assign(bytes.begin() + (std::ptrdiff_t)at, bytes.begin() + (std::ptrdiff_t)(at + size));
            format.assign(bytes.begin() + (std::ptrdiff_t)(formats->begin + i * 32),
                          bytes.begin() + (std::ptrdiff_t)(formats->begin + (i + 1) * 32));
            return true;
        }
        at += size;
    }
    return false;
}

bool WriteNewTextures(const nfs::World& world, const nfs::LoadedSection& loaded, const std::vector<u32>& inUse,
                      std::vector<u8>& bytes, size_t& added, std::wstring& error) {
    if (!IsTextureSection(loaded)) return true;
    std::vector<const nfs::World::NewTexture*> mine;
    for (const auto& texture : world.newTextures)
        // A made texture is in its first host's pack in memory (for the view), never
        // in a shipped one; a copied game texture is left out where it shipped.
        if (std::find(inUse.begin(), inUse.end(), texture.hash) != inUse.end() && HostedBy(texture, loaded) &&
            (texture.header.empty() || texture.fromCar || !PackHolds(loaded, texture.hash)))
            mine.push_back(&texture);
    if (mine.empty()) return true;
    const auto fail = [&](const wchar_t* why) {
        error = L"The texture pack of " + std::wstring(loaded.info->fileName) + L" " + why + L". Nothing was written.";
        return false;
    };

    // The pack: the first at the top of the file.
    const std::vector<Node> top = Children(bytes, 0, bytes.size());
    const Node* found = Find(top, nfs::kChunkTexturePack);
    if (!found) return fail(L"is not there");
    const Node pack = *found;
    const std::vector<Node> parts = Children(bytes, pack.begin, pack.end);
    const Node* infoNode = Find(parts, nfs::kChunkTpkInfoBlock);
    const Node* dataNode = Find(parts, nfs::kChunkTpkDataBlock);
    if (!infoNode || !dataNode) return fail(L"is not laid out as the editor knows them");
    const std::vector<Node> infoParts = Children(bytes, infoNode->begin, infoNode->end);
    const std::vector<Node> dataParts = Children(bytes, dataNode->begin, dataNode->end);
    const Node* hashes  = Find(infoParts, nfs::kChunkTpkHashTable);
    const Node* headers = Find(infoParts, nfs::kChunkTpkTextureHeaders);
    const Node* formats = Find(infoParts, nfs::kChunkTpkFormatTable);
    if (!hashes || !headers || !formats || Find(infoParts, nfs::kChunkTpkCompressedIndex) || dataParts.empty() ||
        dataParts.back().id != nfs::kChunkTpkPixelData)
        return fail(L"is not laid out as the editor knows them");
    const Node pixelNode = dataParts.back();

    // The textures as they stand, in the table's order.
    struct Entry { u32 hash = 0; std::vector<u8> header, format; };
    std::vector<Entry> entries;
    const size_t count = (hashes->end - hashes->begin) / 8;
    size_t at = headers->begin;
    for (size_t i = 0; i < count; ++i) {
        if (at + kTexHeader + 1 > headers->end) return fail(L"has fewer headers than hashes");
        const size_t size = kTexHeader + 1 + bytes[at + kTexHeader];
        if (at + size > headers->end || formats->begin + (i + 1) * 32 > formats->end)
            return fail(L"has fewer headers or formats than hashes");
        Entry entry;
        entry.hash = ReadU32(bytes, hashes->begin + i * 8);
        entry.header.assign(bytes.begin() + (std::ptrdiff_t)at, bytes.begin() + (std::ptrdiff_t)(at + size));
        entry.format.assign(bytes.begin() + (std::ptrdiff_t)(formats->begin + i * 32),
                            bytes.begin() + (std::ptrdiff_t)(formats->begin + (i + 1) * 32));
        if (ReadU32(entry.header, 0x0C) != entry.hash) return fail(L"has a header out of step with its hashes");
        entries.push_back(std::move(entry));
        at += size;
    }
    if (at != headers->end || formats->end - formats->begin != count * 32) return fail(L"holds more than its hashes say");

    // The pixels as they stand, from the 128-byte boundary they start on.
    const size_t oldData = Align128(pixelNode.begin);
    if (oldData > pixelNode.end) return fail(L"is not laid out as the editor knows them");
    std::vector<u8> pixels(bytes.begin() + (std::ptrdiff_t)oldData, bytes.begin() + (std::ptrdiff_t)pixelNode.end);

    for (const nfs::World::NewTexture* texture : mine) {
        auto like = std::find_if(entries.begin(), entries.end(), [&](const Entry& e) { return e.hash == texture->like; });
        for (const u32 more : texture->moreLikes)
            if (like == entries.end())
                like = std::find_if(entries.begin(), entries.end(), [&](const Entry& e) { return e.hash == more; });
        // In a section the list names for it: what it copies is taken from that texture's own pack.
        const Entry* from = like == entries.end() ? nullptr : &*like;
        Entry borrowed;
        // A texture made by the editor in a pack without its `like` (a host the list gives it, or beside another
        // texture there: HostTexturesAlongside) takes its like's header, as it was made from that texture.
        const bool ownLike = like != entries.end() && like->hash == texture->like;
        if ((!from || (!ownLike && texture->header.empty())) &&
            ShippedTextureRows(world, texture->like, borrowed.header, borrowed.format))
            from = &borrowed;
        if (!from) return fail(L"does not hold the texture a new one copies");
        if (std::any_of(entries.begin(), entries.end(), [&](const Entry& e) { return e.hash == texture->hash; }))
            return fail(L"already holds a texture of a new one's name");
        Entry entry;
        entry.hash = texture->hash;
        // A copied game texture keeps its own header, a new one takes the one it copies.
        if (texture->header.size() == kTexHeader) entry.header = texture->header;
        else entry.header.assign(from->header.begin(), from->header.begin() + (std::ptrdiff_t)kTexHeader);
        const size_t length = ((kTexHeader + 1 + texture->name.size() + 1 + 3) & ~(size_t)3) - kTexHeader - 1;
        entry.header.push_back((u8)length);
        entry.header.insert(entry.header.end(), texture->name.begin(), texture->name.end());
        entry.header.resize(kTexHeader + 1 + length, 0);
        const size_t levels = texture->pixels.size();
        const u32 stored = (u32)((levels + 15) & ~(size_t)15);
        const size_t offset = Align128(pixels.size());
        WriteVecU32(entry.header, 0x0C, texture->hash);
        WriteVecU32(entry.header, 0x18, stored);
        WriteVecU32(entry.header, 0x1C, texture->baseBytes);
        WriteVecU32(entry.header, 0x20, texture->width);
        WriteVecU32(entry.header, 0x24, texture->height);
        WriteVecU32(entry.header, 0x28, PowerOf(texture->width));
        WriteVecU32(entry.header, 0x2C, PowerOf(texture->height));
        if (texture->header.empty()) entry.header[0x30] = texture->fourCc == kFourCcDxt5 ? 0x26 : 0x22;   // as every DXT1 / DXT5 shipped
        entry.header[0x31] = (u8)texture->mips;
        WriteVecU32(entry.header, 0x4C, stored);
        WriteVecU32(entry.header, 0x50, (u32)offset);
        entry.format.assign(32, 0);
        WriteVecU32(entry.format, 0x0C, texture->fourCc);
        pixels.resize(offset + stored, 0);
        std::memcpy(pixels.data() + offset, texture->pixels.data(), levels);
        entries.push_back(std::move(entry));
        ++added;
    }
    std::stable_sort(entries.begin(), entries.end(), [](const Entry& a, const Entry& b) { return a.hash < b.hash; });

    // The pack again, chunk by chunk, around the three tables and the pixels.
    std::vector<u8> out;
    const auto put = [&](const u8* data, size_t size) { out.insert(out.end(), data, data + size); };
    const auto open = [&](u32 id) {
        const size_t header = out.size();
        out.resize(header + 8, 0);
        WriteVecU32(out, header, id);
        return header;
    };
    const auto close = [&](size_t header) { WriteVecU32(out, header + 4, (u32)(out.size() - header - 8)); };
    std::vector<size_t> grows;   // the chunks the alignment zeros go into: pack, data block, pixels
    grows.push_back(open(nfs::kChunkTexturePack));
    for (const Node& part : parts) {
        if (part.header == infoNode->header) {
            const size_t info = open(part.id);
            for (const Node& child : infoParts) {
                const size_t chunk = open(child.id);
                if (child.id == nfs::kChunkTpkHashTable) {
                    for (const Entry& e : entries) {
                        const size_t where = out.size();
                        out.resize(where + 8, 0);
                        WriteVecU32(out, where, e.hash);
                    }
                } else if (child.id == nfs::kChunkTpkTextureHeaders) {
                    for (const Entry& e : entries) put(e.header.data(), e.header.size());
                } else if (child.id == nfs::kChunkTpkFormatTable) {
                    for (const Entry& e : entries) put(e.format.data(), e.format.size());
                } else {
                    put(bytes.data() + child.begin, child.end - child.begin);
                }
                close(chunk);
            }
            close(info);
        } else if (part.header == dataNode->header) {
            const size_t data = open(part.id);
            grows.push_back(data);
            for (const Node& child : dataParts) {
                const size_t chunk = open(child.id);
                if (child.header == pixelNode.header) {
                    grows.push_back(chunk);
                    // Filler to the file's next 128-byte boundary, as shipped.
                    const size_t start = pack.header + out.size();
                    out.resize(out.size() + (Align128(start) - start), 0x11);
                    put(pixels.data(), pixels.size());
                } else {
                    put(bytes.data() + child.begin, child.end - child.begin);
                }
                close(chunk);
            }
            close(data);
        } else {
            put(bytes.data() + part.header, part.end - part.header);
        }
    }
    close(grows.front());
    if (parts.back().header != dataNode->header) return fail(L"has something after its pixels");
    // Whatever follows keeps its alignment: the pack grows by 128s, the
    // pixels (its last chunk) taking the rest.
    const size_t oldSize = pack.end - pack.header;
    if (out.size() < oldSize) return fail(L"came out smaller");
    const size_t pad = oldSize + Align128(out.size() - oldSize) - out.size();
    out.resize(out.size() + pad, 0);
    for (const size_t header : grows) WriteVecU32(out, header + 4, (u32)(ReadU32(out, header + 4) + pad));
    bytes.erase(bytes.begin() + (std::ptrdiff_t)pack.header, bytes.begin() + (std::ptrdiff_t)pack.end);
    bytes.insert(bytes.begin() + (std::ptrdiff_t)pack.header, out.begin(), out.end());
    LOG_INFO("new textures: %zu written into %s (pack %zu -> %zu bytes)", mine.size(), loaded.info->name.c_str(), oldSize,
             out.size());
    return true;
}

} // namespace io
