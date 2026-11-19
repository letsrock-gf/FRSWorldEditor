#include "nfs/World.h"

#include "nfs/GameFormat.h"
#include "nfs/ForeignModels.h"
#include "nfs/CustomRegions.h"

#include "io/MeshOverlay.h"
#include "nfs/Jdlz.h"
#include "nfs/EaCompress.h"

#include "core/Log.h"

#include <windows.h>

#include <cmath>
#include <cstdlib>
#include <cstring>

#include <algorithm>
#include <iterator>
#include <map>

namespace nfs {

u64 PlacementKey(u32 modelHash, const Mat4& transform) {
    // Quantise so that values differing only by float noise agree. An eighth of
    // a unit is far below anything visible at this world scale, and the rotation
    // is folded in coarsely so two copies turned differently stay distinct.
    auto quantise = [](float v) { return (i64)std::lround(v * 8.0f); };

    u64 key = 1469598103934665603ull;                 // FNV-1a offset basis
    auto mix = [&key](u64 value) {
        key ^= value;
        key *= 1099511628211ull;
    };

    mix(modelHash);
    for (int axis = 0; axis < 3; ++axis)
        mix((u64)quantise(transform.m[3][axis]));
    for (int row = 0; row < 3; ++row)
        for (int col = 0; col < 3; ++col)
            mix((u64)(i64)std::lround(transform.m[row][col] * 64.0f));
    return key;
}

size_t LoadedSection::ObjectCount() const {
    size_t total = 0;
    for (const SolidList& list : solidLists) total += list.objects.size();
    return total;
}

size_t LoadedSection::TriangleCount() const {
    size_t total = 0;
    for (const SolidList& list : solidLists)
        for (const SolidObject& object : list.objects)
            total += object.TriangleCount();
    return total;
}

bool LoadedSection::HoldsModel(u32 hash) const {
    for (const SolidList& list : solidLists)
        for (const SolidObject& object : list.objects)
            if (object.hash == hash) return true;
    return std::find(importedModels.begin(), importedModels.end(), hash) != importedModels.end();
}

const Aabb& LoadedSection::FocusBounds() const {
    if (terrainBounds.Valid()) return terrainBounds;
    if (trackBounds.Valid())   return trackBounds;
    return bounds;
}

std::vector<std::wstring> World::AvailableRegions(const std::wstring& tracksDirectory) {
    std::vector<std::wstring> regions;

    WIN32_FIND_DATAW find{};
    const std::wstring pattern = tracksDirectory + L"\\STREAM*.BUN";
    HANDLE handle = ::FindFirstFileW(pattern.c_str(), &find);
    if (handle == INVALID_HANDLE_VALUE) return regions;

    do {
        std::wstring name = find.cFileName;               // STREAML4RA.BUN
        if (name.size() <= 10) continue;
        // Underground 1's per-track parts, STREAML1RA_1001.BUN, are not regions.
        // NFS World's files are: it has no stream of its own, only one file a
        // section (below).
        if (name.rfind(L"STREAML5R", 0) != 0 && name.find(L'_') != std::wstring::npos) continue;
        name = name.substr(6, name.size() - 6 - 4);       // L4RA
        // NFS World: one file per section, STREAML5RA_<number or 0xhash>.BUN.
        const size_t underscore = name.find(L'_');
        if (underscore != std::wstring::npos) {
            name.resize(underscore);
            if (std::find(regions.begin(), regions.end(), name) != regions.end()) continue;
        }
        regions.push_back(name);
    } while (::FindNextFileW(handle, &find));

    ::FindClose(handle);
    // The regions the editor made (nfs/CustomRegions.h), read through their source's files.
    const size_t slash = tracksDirectory.find_last_of(L"\\/");
    if (slash != std::wstring::npos)
        for (const CustomRegion& custom : ReadCustomRegions(tracksDirectory.substr(0, slash)))
            if (std::find(regions.begin(), regions.end(), custom.name) == regions.end() &&
                std::find(regions.begin(), regions.end(), custom.source) != regions.end())
                regions.push_back(custom.name);
    std::sort(regions.begin(), regions.end());
    return regions;
}

bool World::Open(const std::wstring& tracksDirectory, const std::wstring& region) {
    Close();

    // A region the editor made reads the files of the region it copies; its
    // edits keep its own name (nfs/CustomRegions.h).
    const size_t slash = tracksDirectory.find_last_of(L"\\/");
    const std::wstring files =
        slash == std::wstring::npos ? region : SourceRegion(tracksDirectory.substr(0, slash), region);
    emptyCity_ = false;
    if (files != region) {
        CustomRegion custom;
        emptyCity_ = FindCustomRegion(tracksDirectory.substr(0, slash), region, custom) && custom.empty;
        LOG_INFO("region %S: a copy of %S%s", region.c_str(), files.c_str(), emptyCity_ ? ", empty" : "");
    }

    // No layout file named after the region: Underground 1.
    if (::GetFileAttributesW((tracksDirectory + L"\\" + files + L".BUN").c_str()) == INVALID_FILE_ATTRIBUTES)
        return OpenUnderground1(tracksDirectory, region);

    if (!layout_.Open(tracksDirectory + L"\\" + files + L".BUN"))
        return false;

    // NFS World's regions are L5Rx; everything else is read as Underground 2.
    ActiveGame() = files.rfind(L"L5R", 0) == 0 ? Game::World : Game::Underground2;
    tracksDirectory_ = tracksDirectory;

    if (IsWorld()) {
        if (!OpenWorldStream(tracksDirectory, region)) {
            Close();
            return false;
        }
        OpenGlobalPacks(tracksDirectory);
        LoadWorldSky(tracksDirectory);
        // No race barrier groups to name: NFS World has no 0x3412F (none in
        // the 5790 TRACKS files, no loader for it in nfsw.exe), and its
        // collision polygons carry no group byte (WorldCollision.cpp).
        region_ = region;
        sectionToLoaded_.assign(table_.Sections().size(), -1);
        return true;
    }

    if (!stream_.Open(tracksDirectory + L"\\STREAM" + files + L".BUN")) {
        layout_.Close();
        return false;
    }

    // Visibility is optional: a region without it simply falls back to
    // drawing by distance, which is worse but not fatal.
    preculler_.Parse(layout_.Span());

    // The edits live beside the game, not in it, so they are gathered here and
    // applied to each mesh as its section is parsed. That way the viewport and
    // the running game are looking at the same geometry.
    overlays_.clear();
    if (!overlayFolder_.empty()) {
        const size_t loaded = ReadMeshOverlays(overlayFolder_, region, overlays_);
        if (loaded) LOG_INFO("%zu edited meshes loaded from the models folder", loaded);

        spans_.clear();
        const size_t spans = ReadSpanOverlays(overlayFolder_, region, spans_);
        LOG_INFO("models folder %S: %zu edited meshes, %zu rebuilt spans",
                 overlayFolder_.c_str(), loaded, spans);
    }

    // Whole regions brought into this one (io/RegionImport.h) live only in the
    // region file the game is served: its table carries their entries and its
    // visibility their zones, beside everything the shipped file has at the
    // same offsets.
    std::vector<u8> served;
    if (!overlayFolder_.empty() &&
        ::GetFileAttributesW((overlayFolder_ + L"\\imports-" + region + L".txt").c_str()) != INVALID_FILE_ATTRIBUTES) {
        FileMap copy;
        if (copy.Open(overlayFolder_ + L"\\files\\TRACKS\\" + region + L".BUN")) {
            const ByteSpan bytes = copy.Span();
            served.assign(bytes.data, bytes.data + bytes.size);
            LOG_INFO("region %S: its table and zones read from the served copy (imports)", region.c_str());
        }
    }
    if (!table_.Parse(served.empty() ? layout_.Span() : ByteSpan{ served.data(), served.size() })) {
        Close();
        return false;
    }
    if (!served.empty()) preculler_.Parse(ByteSpan{ served.data(), served.size() });

    // The table corrections come after it is parsed, so a section that moved is
    // looked for where its bytes now are.
    bool layoutEdited = false;
    for (const SpanOverlay& span : spans_)
        if (span.layoutFile) {
            table_.ApplyBytes(span.fileOffset, span.bytes.data(), span.bytes.size());
            layoutEdited = true;
        }
    // Edited visibility lists (ui/ZoneTools.h) are spans of the layout file
    // too: the zones are read again from the file as the game will see it.
    if (layoutEdited && served.empty()) {   // the served copy has the lists folded in
        const ByteSpan original = layout_.Span();
        std::vector<u8> patched(original.data, original.data + original.size);
        for (const SpanOverlay& span : spans_)
            if (span.layoutFile && span.fileOffset + span.bytes.size() <= patched.size())
                std::copy(span.bytes.begin(), span.bytes.end(), patched.begin() + (ptrdiff_t)span.fileOffset);
        preculler_.Parse(ByteSpan{ patched.data(), patched.size() });
    }

    OpenGlobalPacks(tracksDirectory);
    ReadRaceBarrierGroups();
    // An empty region has none of its source's moving objects (planes, trams,
    // doors): the region file the game reads has them taken out too
    // (io/RegionCreate.h, ApplyEmptyRegionLists).
    if (!emptyCity_) objectAnimations_.Parse(layout_.Span());

    region_ = region;
    sectionToLoaded_.assign(table_.Sections().size(), -1);
    return true;
}

std::vector<size_t> World::SectionsLoadedThroughout(const std::vector<Vec3>& points) const {
    std::vector<size_t> out;
    if (preculler_.IsEmpty() || points.empty()) return out;
    std::vector<u32> common;
    bool first = true;
    const auto narrow = [&](std::vector<u32> listed) {
        std::sort(listed.begin(), listed.end());
        if (first) { common = std::move(listed); first = false; return; }
        std::vector<u32> both;
        std::set_intersection(common.begin(), common.end(), listed.begin(), listed.end(), std::back_inserter(both));
        common.swap(both);
    };
    for (const Vec3& point : points) {
        std::vector<u32> listed;
        if (preculler_.VisibleAt(point, listed)) { narrow(std::move(listed)); continue; }
        // No zone claims it: the car keeps the zone it came from, any of the
        // ones around -- every one of them must load it.
        std::vector<std::vector<u32>> around;
        preculler_.ListsNear(point, 80.0f, around);
        for (auto& list : around) narrow(std::move(list));
    }
    for (size_t s = 0; s < loaded_.size(); ++s)
        if (loaded_[s]->info && !IsLibrary(*loaded_[s]) &&
            std::binary_search(common.begin(), common.end(), loaded_[s]->info->number))
            out.push_back(s);
    return out;
}

// 32-byte records {u32 hash, u32 polygon count, u8 1, char name[23]}, the
// first one "NONE". Read from the top level of the layout file.
void World::ReadRaceBarrierGroups() {
    raceGroups_.clear();
    ChunkReader reader(LayoutSpan(), 0);
    Chunk chunk;
    while (reader.Next(chunk)) {
        if (chunk.id != kChunkRaceBarrierGroups) continue;
        for (size_t at = 0; at + 32 <= chunk.data.size; at += 32) {
            const char* name = reinterpret_cast<const char*>(chunk.data.data + at + 9);
            raceGroups_.emplace_back(name, strnlen(name, 23));
        }
        break;
    }
    LOG_INFO("%zu race barrier groups", raceGroups_.size());
}

// 0x30300102: 52-byte entries {name[24], first frame hash, frame count, fps,
// 16 bytes unknown}; 0x30300103: every entry's frames in turn, 16 bytes each
// with the texture hash first.
void World::ReadTextureAnimations(const Chunk& container) {
    ByteSpan entries, frames;
    ChunkReader reader = ChunkReader::Children(container);
    Chunk child;
    while (reader.Next(child)) {
        if (child.id == 0x30300102u) entries = child.data;
        else if (child.id == 0x30300103u) frames = child.data;
    }
    size_t next = 0;
    for (size_t at = 0; at + 52 <= entries.size; at += 52) {
        TextureAnimation animation;
        const char* name = reinterpret_cast<const char*>(entries.data + at);
        animation.name.assign(name, strnlen(name, 24));
        const u32 hash = entries.read<u32>(at + 24);
        const u32 count = entries.read<u32>(at + 28);
        animation.fps = (float)entries.read<u32>(at + 32);
        for (u32 k = 0; k < count && (next + 1) * 16 <= frames.size; ++k, ++next)
            animation.frames.push_back(frames.read<u32>(next * 16));
        if (animation.frames.size() > 1 && animation.fps > 0.0f) {
            LOG_INFO("texture animation %s: %zu frames at %.0f/s", animation.name.c_str(), animation.frames.size(),
                     animation.fps);
            textureAnimations_[hash] = std::move(animation);
        }
    }
}

std::string World::RaceBarrierGroupName(u8 group) const {
    if (group == 0) return "NONE";
    if (group < raceGroups_.size()) return raceGroups_[group];
    return "GROUP_" + std::to_string(group);
}

// NFS World: lays every section's file end to end in a reserved range, so the
// rest of the editor keeps addressing sections by an offset into one stream.
// Nothing is read here; LoadSection reads a section's file when it is opened.
bool World::OpenWorldStream(const std::wstring& tracksDirectory, const std::wstring& region) {
    if (!table_.Parse(layout_.Span())) return false;

    std::unordered_map<std::wstring, u64> sizes;
    WIN32_FIND_DATAW find{};
    HANDLE handle = ::FindFirstFileW((tracksDirectory + L"\\STREAM" + region + L"_*.BUN").c_str(), &find);
    if (handle != INVALID_HANDLE_VALUE) {
        do {
            std::wstring name = find.cFileName;
            for (wchar_t& c : name) c = (wchar_t)::towupper(c);
            sizes[name] = ((u64)find.nFileSizeHigh << 32) | find.nFileSizeLow;
        } while (::FindNextFileW(handle, &find));
        ::FindClose(handle);
    }

    // A section the editor has written to is read from the copy kept of the
    // shipped file, so what is shown is always the game as it came plus the
    // edit list -- never an edit applied on top of itself.
    const std::wstring kept = tracksDirectory + L"\\frs-original";
    std::unordered_map<std::wstring, u64> keptSizes;
    handle = ::FindFirstFileW((kept + L"\\STREAM" + region + L"_*.BUN").c_str(), &find);
    if (handle != INVALID_HANDLE_VALUE) {
        do {
            std::wstring name = find.cFileName;
            for (wchar_t& c : name) c = (wchar_t)::towupper(c);
            keptSizes[name] = ((u64)find.nFileSizeHigh << 32) | find.nFileSizeLow;
        } while (::FindNextFileW(handle, &find));
        ::FindClose(handle);
    }

    // 64 KiB steps keep every payload's alignment what it is inside its own
    // file, which is what the chunk reader derives the filler from.
    constexpr u64 kStep = 0x10000;
    u64 offset = 0;
    size_t missing = 0;
    for (Section& section : table_.MutableSections()) {
        std::wstring key = section.fileName;
        for (wchar_t& c : key) c = (wchar_t)::towupper(c);
        const auto found = sizes.find(key);
        const auto keptCopy = keptSizes.find(key);
        if (keptCopy != keptSizes.end()) {
            section.streamSize = (u32)keptCopy->second;
            section.sourcePath = kept + L"\\" + section.fileName;
        } else if (found != sizes.end()) {
            section.streamSize = (u32)found->second;
            section.sourcePath = tracksDirectory + L"\\" + section.fileName;
        } else {
            ++missing;
            section.streamSize = 0;
        }

        section.streamOffset = offset;
        section.geometrySize = section.streamSize;
        section.originalStreamOffset = (u32)section.streamOffset;
        section.originalStreamSize   = section.streamSize;
        section.originalOtherSize    = section.streamSize;
        offset += (section.streamSize + kStep - 1) & ~(kStep - 1);
    }
    if (missing) LOG_WARN("%zu sections of the table have no file in TRACKS", missing);
    LOG_INFO("NFS World stream: %zu section files, %llu MiB of address space",
             sizes.size(), (unsigned long long)(offset >> 20));

    // Read before the file is let go: zones and lists are copied out, only
    // their record offsets point back into it.
    preculler_.Parse(layout_.Span());

    // The table is parsed; the file is let go so that saving can write the
    // section sizes back into it (io/WorldWrite.cpp).
    layout_.Close();

    return stream_.Reserve(offset ? offset : kStep);
}

void World::Close() {
    region_.clear();
    raceGroups_.clear();
    textureAnimations_.clear();
    objectAnimations_.Clear();
    layout_.Close();
    layoutUnpacked_.clear();
    layoutUnpacked_.shrink_to_fit();
    skyBundle_.clear();
    skyBundle_.shrink_to_fit();
    trackCollision_.clear();
    game_ = Game::Underground2;
    trackNumber_ = 0;
    stream_.Close();
    globalPacks_.clear();
    globalFiles_.clear();
    loaded_.clear();
    sectionToLoaded_.clear();
    modelsByHash_.clear();
    modelHome_.clear();
    instancedHashes_.clear();
    placements_.clear();
    instanceCount_ = 0;
    textureUsage_.clear();
    bounds_ = {};
}

// Whether a section's object lists are broken the way the editor up to 0.2.1
// wrote them: copying a model into a list whose first hash starts with the
// byte 0x11 (0x0E3EB411 in E25) dropped that byte as filler, so the hash
// table no longer holds one (hash, 0) pair per object. The game reads the
// table from the first byte of the chunk, takes hash bytes for model
// pointers, and crashes the next time a texture pack loads (SPEED2 0x493832).
// Read raw, without the reader's 0x11 filler removal that caused it.
static bool BrokenObjectTable(ByteSpan section) {
    const auto read = [](const u8* at) { u32 v; std::memcpy(&v, at, 4); return v; };
    const auto walk = [&](const u8* data, size_t size, auto&& each) {
        for (size_t at = 0; at + 8 <= size;) {
            const u32 id = read(data + at), length = read(data + at + 4);
            if (at + 8 + (size_t)length > size) return;
            each(id, data + at + 8, (size_t)length);
            at += 8 + (size_t)length;
        }
    };
    bool broken = false;
    walk(section.data, section.size, [&](u32 id, const u8* pack, size_t packSize) {
        if (id != kChunkSolidListPack) return;
        walk(pack, packSize, [&](u32 childId, const u8* header, size_t headerSize) {
            if (childId != 0x80134001u) return;
            u32 count = 0;
            bool haveCount = false, haveTable = false;
            size_t tableSize = 0;
            const u8* table = nullptr;
            walk(header, headerSize, [&](u32 partId, const u8* part, size_t partSize) {
                if (partId == 0x134002u && partSize >= 0x10) { count = read(part + 0x0C); haveCount = true; }
                if (partId == 0x134003u) { table = part; tableSize = partSize; haveTable = true; }
            });
            if (!haveCount || !haveTable) return;
            if (tableSize % 8 != 0 || tableSize / 8 != count) { broken = true; return; }
            for (size_t k = 0; k < count; ++k)
                if (read(table + k * 8 + 4) != 0) { broken = true; return; }
        });
    });
    return broken;
}

const LoadedSection* World::LoadSection(const Section& section) {
    const auto& sections = table_.Sections();
    const size_t index = (size_t)(&section - sections.data());
    if (index >= sections.size()) return nullptr;

    if (sectionToLoaded_[index] >= 0)
        return loaded_[(size_t)sectionToLoaded_[index]].get();

    // Mark as visited up front so a section without geometry is not re-parsed.
    sectionToLoaded_[index] = -2;

    // A section that moved past the end of the file is not an error: its bytes
    // live in a span, and the check below would reject it for pointing at
    // nothing.
    bool relocated = false;
    for (const SpanOverlay& span : spans_)
        if (!span.layoutFile && span.fileOffset == section.streamOffset) relocated = true;

    if (!relocated && section.streamOffset + section.streamSize > stream_.Size()) {
        LOG_WARN("section %s points outside the stream file", section.name.c_str());
        return nullptr;
    }

    if (IsWorld()) {
        if (section.streamSize == 0 ||
            !stream_.Load(section.streamOffset, section.sourcePath, section.streamSize))
            return nullptr;
    }

    const ByteSpan buffer = SectionBytes(section);

    auto loadedSection = std::make_unique<LoadedSection>();
    loadedSection->info = &section;

    // When the section comes from an earlier save, the shipped scenery counts
    // say which records are the editor's own.
    std::vector<size_t> shippedInstances;
    const bool fromSpan = buffer.data < stream_.Span().data ||
                          buffer.data >= stream_.Span().data + stream_.Size();
    if (fromSpan &&
        section.originalStreamOffset + (u64)section.originalStreamSize <= stream_.Size()) {
        const ByteSpan original =
            stream_.Span().sub(section.originalStreamOffset, section.originalStreamSize);
        ChunkReader shipped(original, section.originalStreamOffset);
        Chunk chunk;
        while (shipped.Next(chunk)) {
            if (chunk.id != kChunkTrackScenery) continue;
            SceneryChunkInfo info;
            ReadSceneryChunk(ByteSpan{ chunk.raw.data - 8, chunk.raw.size + 8 },
                             chunk.headerOffset, info);
            shippedInstances.push_back(info.instanceCount);
        }
    }
    size_t sceneryIndex = 0;

    ChunkReader reader(buffer, section.streamOffset);
    Chunk chunk;
    std::vector<Chunk> animationChunks;   // NFS World: its moving scenery (WorldAnimation.h)
    while (reader.Next(chunk)) {
        if (IsWorld() && (chunk.id & 0xFFFFFF0Fu) == 0x37200u && chunk.id >= 0x37220u && chunk.id <= 0x37270u)
            animationChunks.push_back(chunk);
        if (chunk.id == kChunkSolidListPack) {
            SolidList list;
            if (ParseSolidList(chunk, list)) {
                // An edited mesh replaces its positions here, before anything
                // measures bounds or uploads it, so every consumer sees the
                // shape the game will be handed and not the shipped one.
                if (!overlays_.empty()) {
                    for (SolidObject& object : list.objects) {
                        const auto found = overlays_.find(object.hash);
                        if (found == overlays_.end()) continue;
                        if (found->second.size() != object.vertices.size()) {
                            LOG_WARN("edited mesh '%s' has %zu vertices against the "
                                     "game's %zu, so it was ignored",
                                     object.name.c_str(), found->second.size(),
                                     object.vertices.size());
                            continue;
                        }

                        for (size_t i = 0; i < object.vertices.size(); ++i)
                            object.vertices[i].position = found->second[i];

                        object.bounds = {};
                        for (const SolidVertex& vertex : object.vertices)
                            object.bounds.Add(vertex.position);
                    }
                }

                loadedSection->solidLists.push_back(std::move(list));
            }
        } else if (chunk.id == kChunkTrackScenery) {
            const ByteSpan whole{ chunk.raw.data - 8, chunk.raw.size + 8 };
            SceneryChunkInfo info;
            ReadSceneryChunk(whole, chunk.headerOffset, info);
            loadedSection->sceneryNumbers.push_back(info.number);

            // Opt-in check of the scenery writer: FRS_SCENERY_SELFTEST=1.
            static const bool selfTest = std::getenv("FRS_SCENERY_SELFTEST") != nullptr;
            if (selfTest) {
                std::vector<u8> same, grown;
                std::wstring error;
                std::string why, whyGrown;
                const bool identical = AppendScenery(whole, chunk.headerOffset, {}, same, error) &&
                                       same.size() == whole.size &&
                                       std::memcmp(same.data(), whole.data, whole.size) == 0;
                const bool valid = ValidateScenery(whole, chunk.headerOffset, why);

                // And with three made-up placements appended: the result has
                // to be a valid tree holding every record once.
                bool grownValid = false;
                std::vector<PlacedInstance> probe(3);
                for (size_t p = 0; p < probe.size(); ++p) {
                    probe[p].modelHash = 0x12345678u + (u32)p;
                    probe[p].modelName = "SELFTEST";
                    probe[p].transform = Mat4::Translation(
                        { section.position.x + p * 7.0f, section.position.y, section.position.z });
                    probe[p].modelBounds.Add({ -1, -1, 0 });
                    probe[p].modelBounds.Add({ 1, 1, 2 });
                }
                if (AppendScenery(whole, chunk.headerOffset, probe, grown, error)) {
                    SceneryChunkInfo grownInfo;
                    grownValid = ValidateScenery(ByteSpan{ grown.data(), grown.size() },
                                                 chunk.headerOffset, whyGrown) &&
                                 ReadSceneryChunk(ByteSpan{ grown.data(), grown.size() },
                                                  chunk.headerOffset, grownInfo) &&
                                 grownInfo.instanceCount == info.instanceCount + 3;
                }
                LOG_INFO("scenery self-test %s #%u: re-encode %s, shipped tree %s, grown %s",
                         section.name.c_str(), info.number, identical ? "identical" : "DIFFERENT",
                         valid ? "valid" : why.c_str(),
                         grownValid ? "valid" : (whyGrown.empty() ? "FAILED" : whyGrown.c_str()));
            }

            Scenery& scenery = loadedSection->scenery;
            const size_t before = scenery.instances.size();
            ParseScenery(chunk, scenery, IsUnderground1());
            for (size_t i = before; i < scenery.instances.size(); ++i)
                scenery.instances[i].sceneryNumber = info.number;

            const size_t k = sceneryIndex++;
            if (k < shippedInstances.size() && info.instanceCount > shippedInstances[k] &&
                scenery.instances.size() - before == info.instanceCount) {
                std::vector<PlacedInstance> placed;
                ReadSceneryPlacements(whole, chunk.headerOffset, shippedInstances[k], placed);
                scenery.instances.resize(before + shippedInstances[k]);
                LOG_INFO("section %s: %zu objects placed by the editor read back", section.name.c_str(),
                         placed.size());
                for (PlacedInstance& p : placed)
                    loadedSection->placements.push_back({ info.number, std::move(p) });
            }
        } else if (chunk.id == kChunkCollisionPack) {
            CollisionPack pack;
            if (ParseCollision(chunk, pack)) {
                // Opt-in check that the encoder reproduces the shipped pack:
                // set FRS_COLLISION_SELFTEST=1 before starting the editor.
                static const bool selfTest = std::getenv("FRS_COLLISION_SELFTEST") != nullptr;
                if (selfTest) {
                    std::vector<u8> encoded;
                    std::wstring error;
                    const size_t originalSize = (size_t)chunk.raw.size + 8;
                    const u8* original = chunk.raw.data - 8;
                    const bool same = EncodeCollision(pack, encoded, error) &&
                                      encoded.size() == originalSize &&
                                      std::memcmp(encoded.data(), original, originalSize) == 0;
                    size_t firstDiff = 0;
                    while (!same && firstDiff < std::min(encoded.size(), originalSize) &&
                           encoded[firstDiff] == original[firstDiff]) ++firstDiff;
                    LOG_INFO("collision self-test %s: %s (%zu vs %zu bytes, first difference at %zu)",
                             section.name.c_str(), same ? "identical" : "DIFFERENT",
                             encoded.size(), originalSize, same ? (size_t)0 : firstDiff);
                }
                loadedSection->collision.push_back(std::move(pack));
            }
        } else if (chunk.id == kChunkWorldCollisionPack && IsWorld()) {
            CollisionPack pack;
            if (ParseWorldCollision(chunk, pack)) {
                static const char* selfTest = std::getenv("FRS_WORLD_COLLISION_SELFTEST");
                if (selfTest) WorldCollisionSelfTest(chunk, pack, (u32)std::max(2, std::atoi(selfTest)));
                loadedSection->collision.push_back(std::move(pack));
            }
        } else if (chunk.id == kChunkTexturePack) {
            auto pack = std::make_unique<TexturePack>();
            if (pack->Parse(chunk))
                loadedSection->texturePacks.push_back(std::move(pack));
        } else if (chunk.id == 0xB0300100u) {
            ReadTextureAnimations(chunk);
        } else if (chunk.id == kChunkWorldStreamMips) {
            ByteSpan list, pixels;
            ChunkReader parts = ChunkReader::Children(chunk);
            Chunk part;
            while (parts.Next(part)) {
                if (part.id == kChunkWorldStreamMipList) list = part.data;
                else if (part.id == kChunkTpkPixelData)  pixels = part.data;
            }
            for (size_t at = 0; at + 32 <= list.size; at += 32) {
                Texture level;
                level.hash          = list.read<u32>(at);
                // One level is stored for each texture here, the next texture's
                // right behind it. The size at +8 and the count at +28 are
                // those of a whole chain from this level down, which is not
                // in the file: writing by them ran into the neighbour.
                level.baseLevelSize = list.read<u32>(at + 12);
                level.pixels        = pixels.sub(list.read<u32>(at + 4), level.baseLevelSize);
                level.width         = list.read<u16>(at + 16);
                level.height        = list.read<u16>(at + 18);
                level.mipCount      = 1;
                if (level.width && level.height && !level.pixels.empty())
                    loadedSection->streamMips.push_back(std::move(level));
            }
        }
    }

    if (!animationChunks.empty()) objectAnimations_.AddWorldSection(animationChunks, (int)section.number);

    // Written by an older editor with a broken object list: rebuilt from the
    // shipped bytes and its edits at the next save, which lists the objects
    // again the way the game reads them.
    if (fromSpan && BrokenObjectTable(buffer)) {
        loadedSection->placementsChanged = true;
        loadedSection->brokenObjectList = true;
        LOG_WARN("section %s: its object list was written broken by an older version "
                 "(the game crashes at 0x493832); it is rebuilt at the next save", section.name.c_str());
    }

    if (IsUnderground1()) {
        const auto found = trackCollision_.find(section.number);
        if (found != trackCollision_.end()) loadedSection->collision.push_back(found->second);
    }

    // An empty region (nfs/CustomRegions.h): none of the source's city. Every
    // shipped object of a positioned section counts as removed -- hidden here,
    // and written with its matrix zeroed whenever the section is saved -- and
    // the shipped collision of a section the editor has not written yet starts
    // empty; one it wrote holds its own collision already. The libraries (X, Y,
    // Z: models with no place of their own) stay as they are.
    // In the libraries, what the game hides until a scenery group or an
    // animation shows it (instance flag 0x10: the jumbo planes in Z0, shown
    // only by their animation, which an empty region does not have).
    if (emptyCity_ && section.number >= 2400 && section.number < 2700)
        for (const SceneryInstance& instance : loadedSection->scenery.instances) {
            if (instance.flags & 0x10) {
                loadedSection->removedNatives.push_back({ instance.sceneryNumber, instance.record });
                continue;
            }
            const SolidObject* model = FindModel(loadedSection->scenery.ModelHash(instance));
            LOG_INFO("empty region: %s keeps %s (flags %04X): always loaded", section.name.c_str(),
                     model ? model->name.c_str() : "?", instance.flags);
        }
    if (emptyCity_ && !(section.number >= 2400 && section.number < 2700)) {
        for (const SceneryInstance& instance : loadedSection->scenery.instances) {
            loadedSection->removedNatives.push_back({ instance.sceneryNumber, instance.record });
            // A written section whose shipped objects are still in it (an
            // import rebuilt it from its bytes) still has the city: pending.
            const Mat4& m = instance.transform;
            bool zeroed = true;
            for (int r = 0; r < 3; ++r)
                for (int c = 0; c < 3; ++c) zeroed = zeroed && m.m[r][c] == 0.0f;
            if (fromSpan && !zeroed) loadedSection->emptyPending = true;
        }
        // The shipped collision starts empty. A written section keeps the
        // packs the editor wrote and loses the ones still as shipped.
        std::vector<u32> shippedPacks;
        if (fromSpan && section.originalStreamOffset + (u64)section.originalStreamSize <= stream_.Size()) {
            ChunkReader shipped(stream_.Span().sub(section.originalStreamOffset, section.originalStreamSize),
                                section.originalStreamOffset);
            Chunk chunk;
            while (shipped.Next(chunk)) {
                if (chunk.id != kChunkCollisionPack) continue;
                CollisionPack pack;
                if (ParseCollision(chunk, pack)) shippedPacks.push_back(CollisionFingerprint(pack));
            }
        }
        for (size_t k = 0; k < loadedSection->collision.size(); ++k) {
            CollisionPack& pack = loadedSection->collision[k];
            const bool asShipped = !fromSpan || (k < shippedPacks.size() && CollisionFingerprint(pack) == shippedPacks[k]);
            if (!asShipped || pack.polygons.empty()) continue;
            pack.polygons.clear();
            pack.gridRank.clear();
            if (fromSpan) loadedSection->emptyPending = true;
        }
    }

    if (loadedSection->solidLists.empty() && loadedSection->texturePacks.empty() &&
        loadedSection->streamMips.empty() &&
        loadedSection->scenery.Empty() && loadedSection->collision.empty() &&
        loadedSection->placements.empty() && loadedSection->sceneryNumbers.empty())
        return nullptr;

    for (SolidList& list : loadedSection->solidLists) {
        for (SolidObject& object : list.objects) {
            // Claim this placement. A later copy of the same mesh in the same
            // spot is redundant and must not be drawn again.
            if (!placements_.insert(PlacementKey(object.hash, object.transform)).second)
                object.duplicatePlacement = true;

            const Aabb worldBounds = object.WorldBounds();
            loadedSection->bounds.Add(worldBounds);
            if (object.IsBackdrop()) continue;
            loadedSection->trackBounds.Add(worldBounds);
            if (object.IsTerrain()) loadedSection->terrainBounds.Add(worldBounds);
        }
    }

    // Instances are the bulk of a section's content, so they have to be part of
    // its bounds or frustum culling would discard the props with their block.
    for (const SceneryInstance& instance : loadedSection->scenery.instances) {
        loadedSection->bounds.Add(instance.worldBounds);
        loadedSection->trackBounds.Add(instance.worldBounds);
    }

    // A scenery instance names a mesh by hash, so the index must point at the
    // copy that actually got uploaded -- never at one suppressed as a duplicate.
    for (const SolidList& list : loadedSection->solidLists)
        for (const SolidObject& object : list.objects)
            if (!object.duplicatePlacement) {
                modelsByHash_.emplace(object.hash, &object);
                modelHome_.emplace(object.hash, loaded_.size());
            }

    for (const SceneryInstance& instance : loadedSection->scenery.instances)
        instancedHashes_.insert(loadedSection->scenery.ModelHash(instance));

    for (const SolidList& list : loadedSection->solidLists)
        for (const SolidObject& object : list.objects)
            for (const SolidMaterial& material : object.materials)
                if (material.textureIndex < object.textureHashes.size())
                    textureUsage_[object.textureHashes[material.textureIndex]]++;

    instanceCount_ += loadedSection->scenery.instances.size();

    bounds_.Add(loadedSection->bounds);

    sectionToLoaded_[index] = (int)loaded_.size();
    loaded_.push_back(std::move(loadedSection));
    return loaded_.back().get();
}

ByteSpan World::SectionBytes(const Section& section) const {
    // A rebuilt section is read out of its span instead of out of the file. The
    // span holds the section whole, so it stands in for that range exactly.
    for (const SpanOverlay& span : spans_) {
        if (span.layoutFile || span.fileOffset != section.streamOffset) continue;
        return ByteSpan{ span.bytes.data(), span.bytes.size() };
    }
    return stream_.Span().sub(section.streamOffset, section.streamSize);
}

void World::LoadAllSections(void (*progress)(size_t, size_t, void*), void* userData) {
    const auto& sections = table_.Sections();
    for (size_t i = 0; i < sections.size(); ++i) {
        LoadSection(sections[i]);
        if (progress) progress(i + 1, sections.size(), userData);
    }
    LogWorldCollisionSelfTest();
    if (IsWorld()) {
        size_t baked = 0;
        for (const WorldAnimationTree& tree : objectAnimations_.Trees())
            for (const WorldAnimationPart& part : tree.parts) baked += !part.baked.empty();
        LOG_INFO("NFS World animations: %zu trees, %zu placements, %zu parts with frames; visibility: %zu zones",
                 objectAnimations_.Trees().size(), objectAnimations_.Placements().size(), baked,
                 preculler_.ZoneCount());
    }

    // Underground 1's scenery names no models: name each after the model it
    // places, now that every section's models are known.
    if (IsUnderground1())
        for (auto& section : loaded_) NameUnderground1Scenery(*section);

    size_t objects = 0, triangles = 0;
    for (const auto& section : loaded_) {
        objects   += section->ObjectCount();
        triangles += section->TriangleCount();
    }
    LOG_INFO("region %ls: %zu sections with data, %zu objects, %zu triangles, "
             "%zu scenery instances (%zu distinct models placed)",
             region_.c_str(), loaded_.size(), objects, triangles,
             instanceCount_, instancedHashes_.size());
}

Aabb World::TrackBounds() const {
    // Built from the section table rather than the geometry. Terrain and props
    // routinely reach past the block that stores them -- in L4RB the terrain
    // spans 10 700 units while the section centres span only 3 300 -- so a box
    // grown from the meshes frames the camera several times too far back.
    Aabb bounds = table_.Bounds();
    if (!bounds.Valid()) return bounds_;

    bounds = Pad(bounds, SectionPadding());
    return bounds;
}

Aabb World::SectionBounds(const LoadedSection& section) const {
    if (section.info && (section.info->position.x != 0.0f || section.info->position.y != 0.0f)) {
        Aabb box;
        box.Add(section.info->position);
        return Pad(box, SectionPadding());
    }
    return section.FocusBounds();
}

float World::SectionPadding() const {
    // Half a section reaches from a centre to the edge of its own block. Regions
    // with a single positioned section have no spacing to measure, so fall back
    // to something on the scale of a city block.
    const float spacing = table_.TypicalSectionSpacing();
    return spacing > 0.0f ? spacing * 0.5f : 300.0f;
}

size_t World::ModelHome(u32 hash) const {
    const auto it = modelHome_.find(hash);
    return it == modelHome_.end() ? SIZE_MAX : it->second;
}

bool World::IsLibrary(const LoadedSection& section) const {
    return !section.info ||
           (section.info->position.x == 0.0f && section.info->position.y == 0.0f);
}

// A new model is built by rebuilding one of the section's own objects around
// the new mesh (SelectionImport's BuildAddedModel): a section without one -- the
// sky's, or one that only holds collision or placements -- cannot take it.
static bool HasModelToBuildFrom(const LoadedSection& section) {
    for (const SolidList& list : section.solidLists)
        for (const SolidObject& model : list.objects)
            if (model.vertexStride && !model.vertices.empty() &&
                model.name.find("SKY") == std::string::npos && model.name.find("PAN_") == std::string::npos)
                return true;
    return false;
}

size_t World::SectionForPlacement(const Vec3& point) const {
    size_t best = SIZE_MAX, nearest = SIZE_MAX;
    float bestDistance = 1e30f, nearestDistance = 1e30f;

    for (size_t i = 0; i < loaded_.size(); ++i) {
        const LoadedSection& candidate = *loaded_[i];
        if (!candidate.info || IsLibrary(candidate) || IsUnusedAlternate(*candidate.info)) continue;
        if (!HasModelToBuildFrom(candidate)) continue;
        if (std::find(candidate.sceneryNumbers.begin(), candidate.sceneryNumbers.end(),
                      candidate.info->number) == candidate.sceneryNumbers.end())
            continue;

        const float dx = candidate.info->position.x - point.x;
        const float dy = candidate.info->position.y - point.y;
        const float distance = dx * dx + dy * dy;
        if (distance < nearestDistance) { nearestDistance = distance; nearest = i; }

        const Aabb& ground = candidate.terrainBounds;
        if (ground.Valid() && point.x >= ground.min.x && point.x <= ground.max.x &&
            point.y >= ground.min.y && point.y <= ground.max.y && distance < bestDistance) {
            bestDistance = distance;
            best = i;
        }
    }
    return best != SIZE_MAX ? best : nearest;
}

size_t World::SectionForPlacementAlong(const std::vector<Vec3>& points) const {
    if (points.empty()) return SIZE_MAX;
    Vec3 middle{};
    for (const Vec3& p : points) middle = middle + p;
    middle = middle * (1.0f / (float)points.size());
    if (preculler_.IsEmpty()) return SectionForPlacement(middle);
    const size_t usual = SectionForPlacement(middle);
    size_t best = SIZE_MAX;
    float bestDistance = 1e30f;
    for (size_t i : SectionsLoadedThroughout(points)) {
        const LoadedSection& candidate = *loaded_[i];
        if (!candidate.info || IsLibrary(candidate) || IsUnusedAlternate(*candidate.info)) continue;
        if (!HasModelToBuildFrom(candidate)) continue;
        if (std::find(candidate.sceneryNumbers.begin(), candidate.sceneryNumbers.end(), candidate.info->number) ==
            candidate.sceneryNumbers.end())
            continue;
        if (i == usual) return i;
        const float dx = candidate.info->position.x - middle.x, dy = candidate.info->position.y - middle.y;
        if (dx * dx + dy * dy < bestDistance) { bestDistance = dx * dx + dy * dy; best = i; }
    }
    return best;
}

std::vector<const SolidObject*> World::AllModels() const {
    std::vector<const SolidObject*> out;
    out.reserve(modelsByHash_.size());
    for (const auto& [hash, model] : modelsByHash_) out.push_back(model);
    std::sort(out.begin(), out.end(), [](const SolidObject* a, const SolidObject* b) {
        return a->name < b->name;
    });
    return out;
}

// The list is a SolidList of its own: moving the vector of lists moves each
// list's object vector, so pointers into the other lists stay good.
void World::AddModel(size_t loadedIndex, SolidObject model) {
    RemoveModel(model.hash);
    if (loadedIndex >= loaded_.size()) return;
    LoadedSection& section = *loaded_[loadedIndex];
    SolidList list;
    list.path = "frs-new";
    list.objects.push_back(std::move(model));
    section.solidLists.push_back(std::move(list));
    const SolidObject& added = section.solidLists.back().objects.back();
    modelsByHash_[added.hash] = &added;
    modelHome_[added.hash] = loadedIndex;
}

void World::RemoveModel(u32 hash) {
    for (auto& section : loaded_) {
        auto& lists = section->solidLists;
        const auto gone = std::remove_if(lists.begin(), lists.end(), [&](const SolidList& list) {
            return list.path == "frs-new" && list.objects.size() == 1 && list.objects[0].hash == hash;
        });
        if (gone == lists.end()) continue;
        lists.erase(gone, lists.end());
        modelsByHash_.erase(hash);
        modelHome_.erase(hash);
    }
}

const SolidObject* World::FindModel(u32 hash) const {
    const auto it = modelsByHash_.find(hash);
    if (it != modelsByHash_.end()) return it->second;
    return foreignLookup_ ? FindForeignModel(hash) : nullptr;
}

std::vector<const Texture*> World::AllTextures() const {
    std::vector<const Texture*> result;
    std::unordered_set<u32> seen;

    for (const auto& section : loaded_) {
        for (const auto& pack : section->texturePacks) {
            for (const Texture& texture : pack->Textures()) {
                if (seen.insert(texture.hash).second)
                    result.push_back(&texture);
            }
        }
    }
    return result;
}

u32 World::TextureUsage(u32 hash) const {
    const auto it = textureUsage_.find(hash);
    return it == textureUsage_.end() ? 0u : it->second;
}

const Texture* World::FindTexture(u32 hash) const {
    for (const auto& section : loaded_)
        for (const auto& pack : section->texturePacks)
            if (const Texture* texture = pack->Find(hash)) return texture;
    return foreignLookup_ ? FindForeignTexture(hash) : nullptr;
}

bool World::LargestTexture(u32 hash, Texture& out) const {
    const Texture* base = FindTexture(hash);
    if (!base) return false;
    out = *base;
    for (const auto& section : loaded_)
        for (const Texture& level : section->streamMips) {
            if (level.hash != hash || (u32)level.width * level.height <= (u32)out.width * out.height)
                continue;
            out.width         = level.width;
            out.height        = level.height;
            out.mipCount      = level.mipCount;
            out.baseLevelSize = level.baseLevelSize;
            out.pixels        = level.pixels;
        }
    return true;
}

bool World::FullTexture(u32 hash, Texture& out, std::vector<u8>& bytes) const {
    const Texture* base = FindTexture(hash);
    if (!base || base->pixels.empty()) return false;

    // One of each size, smallest first: the same level is stored in every
    // section that uses the texture.
    std::vector<const Texture*> above;
    for (const auto& section : loaded_)
        for (const Texture& level : section->streamMips) {
            if (level.hash != hash || level.width <= base->width || level.pixels.empty()) continue;
            const auto at = std::lower_bound(above.begin(), above.end(), level.width,
                                             [](const Texture* have, u32 width) { return have->width < width; });
            if (at == above.end() || (*at)->width != level.width) above.insert(at, &level);
        }

    // A chain: each level twice the one under it, from the pack's first up.
    u32 width = base->width, height = base->height;
    size_t levels = 0;
    for (; levels < above.size(); ++levels) {
        if (above[levels]->width != width * 2 || above[levels]->height != height * 2) break;
        width *= 2;
        height *= 2;
    }
    if (!levels) return false;

    bytes.clear();
    for (size_t i = levels; i-- > 0;)
        bytes.insert(bytes.end(), above[i]->pixels.data, above[i]->pixels.data + above[i]->pixels.size);
    bytes.insert(bytes.end(), base->pixels.data, base->pixels.data + base->pixels.size);

    out = *base;
    out.width         = above[levels - 1]->width;
    out.height        = above[levels - 1]->height;
    out.mipCount      = (decltype(out.mipCount))(std::max<u32>(base->mipCount, 1) + levels);
    out.baseLevelSize = above[levels - 1]->baseLevelSize;
    out.pixels        = ByteSpan{ bytes.data(), bytes.size() };
    return true;
}

const Texture* World::FindDisplayTexture(u32 hash) const {
    if (const Texture* texture = FindTexture(hash)) return texture;
    for (const auto& pack : globalPacks_)
        if (const Texture* texture = pack->Find(hash)) return texture;
    return nullptr;
}

// Optional: a missing file only leaves those textures grey, as before.
void World::OpenGlobalPacks(const std::wstring& tracksDirectory) {
    const size_t slash = tracksDirectory.find_last_of(L"\\/");
    const std::wstring game = slash == std::wstring::npos ? L"." : tracksDirectory.substr(0, slash);

    // The global packs, and the city's dynamic textures (TRACKS\LOC4DYNTEX.BIN):
    // the sky dome draws from those, which the game swaps with the weather.
    std::vector<std::wstring> paths;
    for (const wchar_t* name : { L"InGameCommon.bun", L"GLOBALB.BUN" }) paths.push_back(game + L"\\GLOBAL\\" + name);
    // Underground 1: its in-game packs and the track's own (TEX<n>TRACK.BIN).
    if (IsUnderground1()) {
        for (const wchar_t* name : { L"INGAMEB.BUN", L"DYNTEX.BIN" }) paths.push_back(game + L"\\GLOBAL\\" + name);
        paths.push_back(tracksDirectory + L"\\TEX" + std::to_wstring(trackNumber_) + L"TRACK.BIN");
    }
    WIN32_FIND_DATAW found{};
    HANDLE search = ::FindFirstFileW((tracksDirectory + L"\\LOC*DYNTEX.BIN").c_str(), &found);
    if (search != INVALID_HANDLE_VALUE) {
        do paths.push_back(tracksDirectory + L"\\" + found.cFileName);
        while (::FindNextFileW(search, &found));
        ::FindClose(search);
    }
    for (const std::wstring& path : paths) {
        if (::GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES) continue;

        auto file = std::make_unique<FileMap>();
        if (!file->Open(path)) continue;

        size_t packs = 0;
        ChunkReader top(file->Span(), 0);
        Chunk chunk;
        while (top.Next(chunk)) {
            if (chunk.id != kChunkTexturePack) continue;
            auto pack = std::make_unique<TexturePack>();
            if (pack->Parse(chunk)) { globalPacks_.push_back(std::move(pack)); ++packs; }
        }
        LOG_INFO("global textures: %zu packs from %S", packs, path.c_str());
        globalFiles_.push_back(std::move(file));
    }
}

// ---------------------------------------------------------------------------
// Underground 1
// ---------------------------------------------------------------------------

namespace {

bool ReadWholeFile(const std::wstring& path, std::vector<u8>& out) {
    HANDLE file = ::CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                                FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER size{};
    bool ok = ::GetFileSizeEx(file, &size) && size.QuadPart > 0 && size.QuadPart < (1ll << 31);
    if (ok) {
        out.resize((size_t)size.QuadPart);
        DWORD read = 0;
        ok = ::ReadFile(file, out.data(), (DWORD)out.size(), &read, nullptr) && read == out.size();
    }
    ::CloseHandle(file);
    return ok;
}

// TRACKB<n>.LZC unpacked; a bundle left unpacked is taken as it is.
std::vector<u8> UnpackTrackBundle(const std::wstring& path) {
    std::vector<u8> file;
    if (!ReadWholeFile(path, file)) return {};
    if (file.size() >= 4 && std::memcmp(file.data(), "JDLZ", 4) == 0)
        return JdlzDecompress(ByteSpan{ file.data(), file.size() });
    return file;
}

ByteSpan TopLevelChunk(ByteSpan file, u32 id) {
    ChunkReader reader(file, 0);
    Chunk chunk;
    while (reader.Next(chunk))
        if (chunk.id == id) return chunk.data;
    return {};
}

constexpr u32 kChunkStreamSizesU1    = 0x00034108;   // {"--" size, region stream size, track part size}
constexpr u32 kChunkTrackCollisionU1 = 0x00034132;   // the whole track's collision polygons

std::vector<u32> TrackNumbersIn(const std::wstring& pattern, size_t digitsFrom) {
    std::vector<u32> numbers;
    WIN32_FIND_DATAW find{};
    HANDLE handle = ::FindFirstFileW(pattern.c_str(), &find);
    if (handle == INVALID_HANDLE_VALUE) return numbers;
    do {
        const std::wstring name = find.cFileName;
        if (name.size() <= digitsFrom || !iswdigit(name[digitsFrom])) continue;
        numbers.push_back((u32)std::wcstoul(name.c_str() + digitsFrom, nullptr, 10));
    } while (::FindNextFileW(handle, &find));
    ::FindClose(handle);
    std::sort(numbers.begin(), numbers.end());
    numbers.erase(std::unique(numbers.begin(), numbers.end()), numbers.end());
    return numbers;
}

} // namespace

// Every track of a region has its own bundle over the region's one stream.
// The bundle to open is one whose stream sizes (0x00034108) name this stream:
// tried first are the tracks the region lists a part file for
// (STREAM<region>_<n>.BUN), an empty part -- a track with nothing of its own
// -- before the rest; then any other bundle, those whose number starts with
// the region's digit (L4RA -> 4xxx) first.
bool World::OpenUnderground1(const std::wstring& tracksDirectory, const std::wstring& region) {
    if (!stream_.Open(tracksDirectory + L"\\STREAM" + region + L".BUN")) return false;

    const std::vector<u32> parts =
        TrackNumbersIn(tracksDirectory + L"\\STREAM" + region + L"_*.BUN", 6 + region.size() + 1);
    const std::vector<u32> bundles = TrackNumbersIn(tracksDirectory + L"\\TRACKB*.LZC", 6);

    std::vector<u32> empty, full;
    for (u32 number : parts) {
        WIN32_FILE_ATTRIBUTE_DATA data{};
        const std::wstring part =
            tracksDirectory + L"\\STREAM" + region + L"_" + std::to_wstring(number) + L".BUN";
        if (!::GetFileAttributesExW(part.c_str(), GetFileExInfoStandard, &data)) continue;
        (data.nFileSizeLow == 0 && data.nFileSizeHigh == 0 ? empty : full).push_back(number);
    }
    const wchar_t digit = region.size() > 1 ? region[1] : L'?';
    std::vector<u32> sameDigit, otherDigit;
    for (u32 number : bundles) {
        if (std::find(parts.begin(), parts.end(), number) != parts.end()) continue;
        (std::to_wstring(number)[0] == digit ? sameDigit : otherDigit).push_back(number);
    }
    std::vector<u32> order;
    for (const auto* list : { &empty, &full, &sameDigit, &otherDigit })
        order.insert(order.end(), list->begin(), list->end());

    for (u32 number : order) {
        const std::wstring path = tracksDirectory + L"\\TRACKB" + std::to_wstring(number) + L".LZC";
        std::vector<u8> unpacked = UnpackTrackBundle(path);
        if (unpacked.empty()) continue;
        const ByteSpan sizes = TopLevelChunk(ByteSpan{ unpacked.data(), unpacked.size() }, kChunkStreamSizesU1);
        if (sizes.size < 8 || sizes.read<u32>(4) != stream_.Size()) continue;
        layoutUnpacked_ = std::move(unpacked);
        trackNumber_ = number;
        break;
    }
    if (layoutUnpacked_.empty()) {
        LOG_ERROR("Underground 1 region %S: no TRACKB<n>.LZC describes STREAM%S.BUN", region.c_str(),
                  region.c_str());
        stream_.Close();
        return false;
    }
    game_ = Game::Underground1;
    LOG_INFO("Underground 1 region %S opened with track %u (%zu bytes unpacked)", region.c_str(), trackNumber_,
             layoutUnpacked_.size());

    const ByteSpan layout = LayoutSpan();
    if (!table_.Parse(layout)) {
        Close();
        return false;
    }
    preculler_.Parse(layout, true);

    // Positions: the outline of each drivable section's zone (zone id =
    // section number); a detail section (<letter>11..19) sits on its
    // drivable one's. Shared sections (W, X, Y, Z...) stay unpositioned, as
    // the library they are.
    std::unordered_map<u32, Vec3> centres;
    for (const Preculler::Zone& zone : preculler_.Zones()) {
        Vec3 sum{};
        for (const Vec3& p : zone.ring) sum += p;
        centres[zone.id] = sum * (1.0f / (float)zone.ring.size());
    }
    for (size_t i = 0; i < table_.Sections().size(); ++i) {
        const u32 number = table_.Sections()[i].number;
        auto found = centres.find(number);
        if (found == centres.end() && number % 100 >= 11 && number % 100 <= 19) found = centres.find(number - 10);
        if (found != centres.end()) table_.SetPosition(i, found->second);
    }

    SplitTrackCollision();
    OpenGlobalPacks(tracksDirectory);
    raceGroups_.clear();

    region_ = region;
    sectionToLoaded_.assign(table_.Sections().size(), -1);
    return true;
}

// The track's collision is one list of polygons. The editor keeps collision
// per section, so each polygon goes to the drivable section whose zone holds
// its middle, or to the nearest zone's.
void World::SplitTrackCollision() {
    trackCollision_.clear();
    const ByteSpan records = TopLevelChunk(LayoutSpan(), kChunkTrackCollisionU1);
    std::vector<CollisionPolygon> polygons;
    ParseCollisionPolygons(records, polygons);
    if (polygons.empty()) return;

    std::unordered_set<u32> numbers;
    for (const Section& section : table_.Sections()) numbers.insert(section.number);
    const auto& zones = preculler_.Zones();

    size_t unplaced = 0;
    for (const CollisionPolygon& polygon : polygons) {
        float x = 0.0f, y = 0.0f;
        for (const Vec3& c : polygon.corners) { x += c.x * 0.25f; y += c.y * 0.25f; }
        int zone = preculler_.ZoneAt(x, y);
        if (zone < 0 || !numbers.count(zones[(size_t)zone].id)) {
            float best = 1e30f;
            zone = -1;
            for (size_t z = 0; z < zones.size(); ++z) {
                if (!numbers.count(zones[z].id)) continue;
                const float cx = (zones[z].boundsMin.x + zones[z].boundsMax.x) * 0.5f - x;
                const float cy = (zones[z].boundsMin.y + zones[z].boundsMax.y) * 0.5f - y;
                const float d = cx * cx + cy * cy;
                if (d < best) { best = d; zone = (int)z; }
            }
        }
        if (zone < 0) { ++unplaced; continue; }
        const u32 number = zones[(size_t)zone].id;
        CollisionPack& pack = trackCollision_[number];
        pack.sectionNumber = number;
        pack.polygons.push_back(polygon);
    }

    for (auto& [number, pack] : trackCollision_) {
        Aabb ground;
        for (const CollisionPolygon& polygon : pack.polygons)
            if (!polygon.barrier)
                for (const Vec3& c : polygon.corners) ground.Add(c);
        pack.bounds = ground;
        const float low  = ground.Valid() ? ground.min.z - CollisionPack::kBarrierBelow : -1.0f;
        const float high = ground.Valid() ? ground.max.z + CollisionPack::kBarrierAbove : 3.0f;
        for (const CollisionPolygon& polygon : pack.polygons)
            if (polygon.barrier)
                for (const Vec3& c : polygon.corners) {
                    pack.bounds.Add({ c.x, c.y, low });
                    pack.bounds.Add({ c.x, c.y, high });
                }
        pack.gridOrigin = { std::floor(pack.bounds.min.x / 32.0f) * 32.0f,
                            std::floor(pack.bounds.min.y / 32.0f) * 32.0f, 0.0f };
        pack.gridWidth  = (u16)std::ceil((pack.bounds.max.x - pack.gridOrigin.x) / 32.0f);
        pack.gridHeight = (u16)std::ceil((pack.bounds.max.y - pack.gridOrigin.y) / 32.0f);
    }
    LOG_INFO("Underground 1 collision: %zu polygons over %zu sections (%zu with no zone)", polygons.size(),
             trackCollision_.size(), unplaced);
}

void World::NameUnderground1Scenery(LoadedSection& section) const {
    for (SceneryInfo& info : section.scenery.infos)
        if (info.name.empty())
            if (const SolidObject* model = FindModel(info.solidHash)) info.name = model->name;
}

// NFS World's sky is no track model: the domes (SKYDOME_1_DAWN, _NOON,
// _LATEAFTERNOON, _SUNSET, _DUSK, _NIGHT; solid list "eLabSkyDomes.bin") are
// in GLOBAL\GlobalB.lzc and their textures in GLOBAL\SkyDomeTextures.bin
// (tools/nfsw/sky_probe.py). GlobalB.lzc: u32 0x66113388, unpacked size,
// packed size, u32; then blocks {u32 0x55441122, size, packed (with this
// 24-byte header), offset in the output, offset of the block - 16, 0}, each
// a JDLZ or HUFF stream. Its solid lists that hold a SKYDOME go in a loaded
// section of their own, at the origin like the library sections, with no
// scenery: only the Sky option draws them.
void World::LoadWorldSky(const std::wstring& tracksDirectory) {
    const size_t slash = tracksDirectory.find_last_of(L"\\/");
    const std::wstring global = (slash == std::wstring::npos ? L"." : tracksDirectory.substr(0, slash)) + L"\\GLOBAL\\";

    auto textures = std::make_unique<FileMap>();
    if (textures->Open(global + L"SkyDomeTextures.bin")) {
        size_t packs = 0;
        ChunkReader top(textures->Span(), 0);
        Chunk chunk;
        while (top.Next(chunk)) {
            if (chunk.id != kChunkTexturePack) continue;
            auto pack = std::make_unique<TexturePack>();
            if (pack->Parse(chunk)) { globalPacks_.push_back(std::move(pack)); ++packs; }
        }
        LOG_INFO("sky textures: %zu packs from SkyDomeTextures.bin", packs);
        globalFiles_.push_back(std::move(textures));
    }

    std::vector<u8> file;
    if (!ReadWholeFile(global + L"GlobalB.lzc", file) || file.size() < 16 ||
        *reinterpret_cast<const u32*>(file.data()) != 0x66113388u) {
        LOG_WARN("sky: GLOBAL\\GlobalB.lzc missing or not the packed layout");
        return;
    }
    const u32 total = *reinterpret_cast<const u32*>(file.data() + 4);
    skyBundle_.assign(total, 0);
    size_t blocks = 0, failed = 0;
    std::vector<u8> out;
    for (size_t at = 16; at + 24 <= file.size();) {
        const u32* head = reinterpret_cast<const u32*>(file.data() + at);
        if (head[0] != 0x55441122u || head[2] < 24 || at + head[2] > file.size()) break;
        out.clear();
        if (EaDecompress(ByteSpan{ file.data() + at + 24, head[2] - 24 }, out) && out.size() == head[1] &&
            (size_t)head[3] + out.size() <= skyBundle_.size()) {
            std::memcpy(skyBundle_.data() + head[3], out.data(), out.size());
            ++blocks;
        } else {
            ++failed;
        }
        at += head[2];
    }

    auto sky = std::make_unique<LoadedSection>();
    skySection_ = Section{};
    skySection_.name = "SKY";
    skySection_.fileName = L"GlobalB.lzc";
    sky->info = &skySection_;
    // The solid lists sit inside other containers, so every level is searched.
    std::vector<ChunkReader> readers{ ChunkReader(ByteSpan{ skyBundle_.data(), skyBundle_.size() }, 0) };
    std::map<u32, size_t> topIds;
    Chunk chunk;
    while (!readers.empty()) {
        if (!readers.back().Next(chunk)) { readers.pop_back(); continue; }
        if (readers.size() == 1) ++topIds[chunk.id];
        if (chunk.id != kChunkSolidListPack) {
            if (chunk.IsContainer() && readers.size() < 6) readers.push_back(ChunkReader::Children(chunk));
            continue;
        }
        SolidList list;
        if (!ParseSolidList(chunk, list)) continue;
        if (std::none_of(list.objects.begin(), list.objects.end(),
                         [](const SolidObject& o) { return o.name.rfind("SKYDOME", 0) == 0; }))
            continue;
        for (const SolidObject& o : list.objects) LOG_INFO("sky: model %s from GlobalB.lzc", o.name.c_str());
        sky->solidLists.push_back(std::move(list));
    }
    for (const auto& [id, count] : topIds) LOG_INFO("sky: GlobalB.lzc top-level chunk %08X x%zu", id, count);
    LOG_INFO("sky: GlobalB.lzc %u bytes, %zu blocks unpacked, %zu failed, %zu solid lists with a dome", total,
             blocks, failed, sky->solidLists.size());
    if (sky->solidLists.empty()) return;
    for (SolidList& list : sky->solidLists)
        for (SolidObject& object : list.objects) {
            sky->bounds.Add(object.bounds);
            modelsByHash_.emplace(object.hash, &object);
            modelHome_.emplace(object.hash, loaded_.size());
        }
    loaded_.push_back(std::move(sky));
}

} // namespace nfs
