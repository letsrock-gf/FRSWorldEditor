#pragma once

#include "core/FileMap.h"
#include "core/Math.h"
#include "io/MeshOverlay.h"
#include "nfs/WorldAnimation.h"
#include "nfs/Collision.h"
#include "nfs/GameFormat.h"
#include "nfs/Preculler.h"
#include "nfs/SectionTable.h"
#include "nfs/Scenery.h"
#include "nfs/SceneryEdit.h"
#include "nfs/SolidList.h"
#include "nfs/TexturePack.h"

#include <deque>
#include <memory>
#include <unordered_map>
#include <unordered_set>
#include <string>
#include <vector>

namespace nfs {

// Fingerprint of "this mesh, drawn here". Used to make sure a model that is
// stored twice, or placed both by its own header and by a scenery instance,
// still ends up on screen exactly once.
u64 PlacementKey(u32 modelHash, const Mat4& transform);

// The parsed contents of one streaming section.
struct LoadedSection {
    const Section*          info = nullptr;
    std::vector<SolidList>  solidLists;
    std::vector<std::unique_ptr<TexturePack>> texturePacks;
    Scenery                 scenery;
    std::vector<CollisionPack> collision;

    // Scenery the editor placed, kept apart from the shipped instances so
    // those stay exactly as they were and a save knows what to append. A
    // section read back from an earlier save has its extra records moved here.
    struct UserPlacement {
        u32            sceneryNumber = 0;   // which of the section's scenery chunks
        PlacedInstance instance;
    };
    std::vector<UserPlacement> placements;
    std::vector<u32> sceneryNumbers;        // each scenery chunk's number, in file order
    bool placementsChanged = false;         // differs from what is on disk

    // Shipped scenery records marked for removal and not written yet:
    // (scenery number, record index). Written ones come back as records with
    // their matrix zeroed.
    std::vector<std::pair<u32, u32>> removedNatives;

    // An empty region (World::EmptyCity): a section written earlier that still
    // carries some of the source's city on disk -- shipped objects not zeroed,
    // a collision pack as shipped. Saving it again takes the city out.
    bool emptyPending = false;

    // Written by an older version with a broken object list (World.cpp,
    // BrokenObjectTable): the game crashes on it until it is saved again.
    bool brokenObjectList = false;

    // Collision edited in the editor and not written yet: the packs (indices
    // into `collision`) whose polygons differ from what is on disk.
    bool collisionChanged = false;
    std::vector<u32> changedCollisionPacks;
    // NFS World: the polygons the edit list took out of a pack when the region
    // was read (pack index, polygons) -- what the Edits tab lists and can put back.
    std::vector<std::pair<u32, std::vector<CollisionPolygon>>> savedCollisionCuts;
    std::vector<std::pair<u32, std::vector<CollisionPolygon>>> savedCollisionAdds;   // and put into one
    void MarkCollisionChanged(u32 pack) {
        collisionChanged = true;
        for (u32 p : changedCollisionPacks) if (p == pack) return;
        changedCollisionPacks.push_back(pack);
    }
    // Shipped scenery records the editor moved: where each stood and where
    // it stands now. The instance itself already carries the new transform.
    struct MovedNative {
        u32  sceneryNumber = 0;
        u32  record = 0;
        Mat4 original;
        Mat4 moved;
    };
    std::vector<MovedNative> movedNatives;

    // NFS World keeps a texture's larger levels apart from its pack, in packs
    // of their own (0xB3300011). Format is left Unknown here: it is the pack
    // texture's, looked up by hash when the pixels are used.
    std::vector<Texture> streamMips;

    // Bytes that stand in for a range of the section's file when it is
    // written: the pixels of a replaced texture.
    struct BytePatch {
        u32 hash = 0;
        size_t offset = 0;        // into the section's file, as shipped
        std::vector<u8> bytes;

        // The stored copy as shipped and as it is with the replacement in.
        // They differ when the image came in at another size, and then the
        // pack is laid out again around it when the section is written.
        size_t shippedSize = 0;
        u16 shippedWidth = 0, shippedHeight = 0;
        u8  shippedMips = 0;
        u32 shippedBase = 0;
        u16 width = 0, height = 0;
        u8  mipCount = 0;
        u32 base = 0;             // bytes of the largest level
        u8  blockBytes = 16;      // of one 4x4 block in this format
        bool Resized() const { return width != shippedWidth || height != shippedHeight; }
    };
    std::vector<std::unique_ptr<BytePatch>> texturePatches;

    // Models from other positioned sections to be copied into this one when
    // the placements are written, so they can be placed here. Hashes.
    std::vector<u32> importedModels;

    bool HoldsModel(u32 hash) const;
    Aabb                    bounds;        // everything, backdrop included
    Aabb                    trackBounds;   // drivable world only
    // Bounds of the terrain alone. Props are frequently shared between
    // neighbouring sections and can sit well outside the footprint of the
    // section that stores them, so the terrain is what marks where it lies.
    Aabb                    terrainBounds;

    // The box the camera should frame when this section is selected.
    const Aabb& FocusBounds() const;

    size_t ObjectCount() const;
    size_t TriangleCount() const;
};

// A texture that cycles through frames: water, flashing signs, blinking
// lights. Materials name the first frame's hash; the game swaps in the frame
// of the moment (chunk 0xB0300100, beside a section's texture pack).
struct TextureAnimation {
    std::string      name;
    std::vector<u32> frames;     // texture hashes, the first is the one materials use
    float            fps = 0.0f;
};

// The games whose worlds the editor reads: nfs::Game, in nfs/GameFormat.h.

// One track region: the L4Rx.BUN layout file plus its STREAML4Rx.BUN payload.
//
// Underground 1 has no layout file per region: each track has a bundle,
// TRACKS\TRACKB<n>.LZC (JDLZ), holding the section table, the visibility
// zones and the whole track's collision, over a stream shared by every track
// of the region. The region opens with one of those bundles in the layout's
// place (see OpenUnderground1).
//
// Sections are parsed on demand. Because the stream stays memory-mapped, a
// section that is never visited costs nothing beyond its table entry.
class World {
public:
    // Region identifiers as they appear in the file names: "L4RA", "L4RB", ...
    static std::vector<std::wstring> AvailableRegions(const std::wstring& tracksDirectory);

    // Where the edited meshes live, usually <game>\models. Set it before
    // opening: overlays are applied as each section is parsed, so the editor
    // shows the same geometry the game will be handed.
    void SetOverlayFolder(const std::wstring& folder) { overlayFolder_ = folder; }
    const std::wstring& OverlayFolder() const { return overlayFolder_; }

    size_t OverlayCount() const { return overlays_.size(); }

    // Opens <tracksDirectory>\<region>.BUN and STREAM<region>.BUN.
    bool Open(const std::wstring& tracksDirectory, const std::wstring& region);
    void Close();

    bool IsOpen() const { return stream_.IsOpen(); }

    Game GameKind() const { return game_; }
    bool IsUnderground1() const { return game_ == Game::Underground1; }
    // An empty region the editor made (nfs/CustomRegions.h): its source's city
    // hidden, and taken out of every section it saves.
    bool EmptyCity() const { return emptyCity_; }
    // Underground 1: the track whose bundle the region was opened with.
    u32  TrackNumber() const { return trackNumber_; }

    const std::wstring& Region() const { return region_; }
    const SectionTable& Table()  const { return table_; }

    // The mapped stream, for anything that has to work in file bytes rather
    // than in parsed geometry -- rebuilding a section, for one.
    ByteSpan StreamBytes() const { return stream_.Span(); }

    // A section's bytes as the editor reads them: the rebuilt span when an
    // earlier import replaced the section, the stream file otherwise. Building
    // a new edit on these keeps the edits already in force.
    ByteSpan SectionBytes(const Section& section) const;
    const Preculler&    Visibility() const { return preculler_; }
    // The layout file (L4Rx.BUN) as shipped, and the spans the models folder
    // lays over it and the stream.
    ByteSpan LayoutBytes() const { return LayoutSpan(); }
    const std::vector<SpanOverlay>& Spans() const { return spans_; }

    // Parses a section if it has not been parsed yet. Returns nullptr when the
    // section holds no geometry.
    const LoadedSection* LoadSection(const Section& section);

    // Parses every section. Reports progress through the optional callback,
    // which receives (sectionsDone, sectionsTotal).
    void LoadAllSections(void (*progress)(size_t, size_t, void*) = nullptr,
                         void* userData = nullptr);

    const std::vector<std::unique_ptr<LoadedSection>>& Loaded() const { return loaded_; }

    // Bounds of everything parsed so far, including the distant backdrop.
    Aabb Bounds() const { return bounds_; }

    // Bounds of the drivable world only.
    //
    // The first sections of a region hold shared assets and the painted
    // backdrop, which stretch tens of thousands of units in every direction and
    // would swamp any camera framed on them. Those sections carry no world
    // position in the table, so positioned sections alone give the real extent.
    Aabb TrackBounds() const;

    // The box the camera should frame for one section.
    Aabb SectionBounds(const LoadedSection& section) const;

    // Looks a texture up across every pack loaded so far.
    const Texture* FindTexture(u32 hash) const;

    // The same, falling back to the packs the game keeps loaded for every
    // track (GLOBAL\InGameCommon.bun, GLOBAL\GLOBALB.BUN). The race barriers,
    // their tape and neon, and a few effect sprites draw from those. Only for
    // looking at: a texture found there is not the region's to copy or export.
    const Texture* FindDisplayTexture(u32 hash) const;

    // The largest stored version of a texture. In NFS World that is usually
    // one of the levels kept apart; `out` then has the pack texture's name and
    // format with the larger pixels.
    bool LargestTexture(u32 hash, Texture& out) const;
    // NFS World: the texture as the game has it once everything is streamed
    // in -- the levels kept apart, largest first, then the pack's own. `bytes`
    // holds the levels in that order and `out.pixels` points into it. False
    // when the pack's copy is all there is.
    bool FullTexture(u32 hash, Texture& out, std::vector<u8>& bytes) const;

    // NFS World: images that stand in for textures, by hash, and the file
    // each was kept as (io/WorldWrite.h).
    struct TextureReplacement {
        u32 hash = 0;
        std::wstring file;
    };
    std::vector<TextureReplacement> textureReplacements;

    // NFS World: models whose mesh was edited (in Blender), by hash, and the
    // model-space OBJ each was kept as (io/WorldMesh.h).
    struct MeshReplacement {
        u32 hash = 0;
        std::wstring file;
    };
    std::vector<MeshReplacement> meshReplacements;

    // NFS World: models the editor made (io/WorldMesh.h), each written into
    // the file of one section, from the model-space OBJ it was kept as.
    struct NewModel {
        u32 hash = 0;
        u32 section = 0;          // the section's number
        std::wstring file;
    };
    std::vector<NewModel> newModels;

    // NFS World: textures the editor made for new models (io/WorldTexture.h),
    // written into every texture section whose pack holds `like`, the game
    // texture whose header and format they copy. A deque: the in-memory pack
    // entry points into `pixels`.
    struct NewTexture {
        u32 hash = 0, like = 0;
        std::string name;              // upper case, what `hash` is the hash of
        std::wstring file;             // the kept image, by name in frs-original\textures
        u32 width = 0, height = 0, mips = 0, fourCc = 0, baseBytes = 0;
        std::vector<u8> pixels;        // every level, largest first
        // A game texture copied for a model placed where it is not loaded: the
        // header (0x90 bytes) it ships with, and more textures of the sections
        // that need it whose packs take it too. Made again at every Save.
        std::vector<u8> header;
        std::vector<u32> moreLikes;
    };
    std::deque<NewTexture> newTextures;

    // A model the editor made, put into a loaded section (a solid list of its
    // own) and into the index FindModel reads. Replaces one of the same hash.
    void AddModel(size_t loadedIndex, SolidObject model);
    // Takes such a model out again.
    void RemoveModel(u32 hash);

    // Every distinct texture in the region, deduplicated by hash. A region
    // repeats textures across its packs, so the raw packs are not a useful list.
    std::vector<const Texture*> AllTextures() const;

    // How many material slots draw with this texture.
    u32 TextureUsage(u32 hash) const;

    // Looks a model up by hash across every section loaded so far. Scenery
    // instances reference models that live in a different section, usually the
    // shared library the region loads first. A hash the region does not
    // hold is asked of the models from outside it (nfs/ForeignModels.h).
    const SolidObject* FindModel(u32 hash) const;

    // Whether lookups fall back to the models from outside the region. Off
    // for the worlds that outside source opens itself.
    void SetForeignLookup(bool on) { foreignLookup_ = on; }

    size_t InstanceCount() const { return instanceCount_; }

    // Every texture animation of the sections loaded, by the hash materials use.
    const std::unordered_map<u32, TextureAnimation>& TextureAnimations() const { return textureAnimations_; }

    // The moving scenery of the layout file: doors, planes, cranes, trams.
    const WorldAnimations& ObjectAnimations() const { return objectAnimations_; }
    // Moving scenery shown but not part of the region (WorldAnimations::SetExtraPlacements).
    void SetPreviewAnimations(std::vector<WorldAnimationPlacement> extra) {
        objectAnimations_.SetExtraPlacements(std::move(extra));
    }

    // The race barrier groups the layout file names (chunk 0x3412F): byte 0
    // of a collision polygon indexes this list, entry 0 being "NONE". A group
    // exists for the game only during the races that name it -- in free roam
    // its polygons are not there at all.
    const std::vector<std::string>& RaceBarrierGroups() const { return raceGroups_; }
    std::string RaceBarrierGroupName(u8 group) const;

    // --- placing scenery -----------------------------------------------------

    LoadedSection* MutableLoaded(size_t index) {
        return index < loaded_.size() ? loaded_[index].get() : nullptr;
    }

    // The loaded section a model is stored in, or SIZE_MAX.
    size_t ModelHome(u32 hash) const;

    // Sections without a world position hold the region's shared library,
    // which the game keeps loaded everywhere. Only a model stored there can be
    // placed anywhere: no shipped instance refers to a model that lives in a
    // different positioned section, which would only exist while that section
    // happened to be streamed in.
    bool IsLibrary(const LoadedSection& section) const;

    // A section of the second set (Section::IsAlternate) that no visibility
    // list of the game loads: a true alternative, hidden unless asked for.
    // Without visibility lists, the second set as a whole.
    bool IsUnusedAlternate(const Section& section) const {
        return section.IsAlternate() && (preculler_.IsEmpty() || !preculler_.Listed(section.number));
    }

    // Loaded sections (indices into Loaded()) that every point's visibility
    // zone lists: what the game has loaded wherever along these points the
    // car is. A model or collision placed in one of them never vanishes on
    // the way. Empty when the points cross zones that share no section, or
    // when the region has no visibility lists.
    std::vector<size_t> SectionsLoadedThroughout(const std::vector<Vec3>& points) const;

    // The positioned section a placement at `point` belongs in: the one whose
    // ground covers it, else the nearest. SIZE_MAX when none can take scenery.
    size_t SectionForPlacement(const Vec3& point) const;

    // The section for a model or placement spanning `points`: one that can
    // take scenery and that every point's visibility zone loads, so it never
    // vanishes along them -- SectionForPlacement's choice for their middle
    // when it is one of those, else the nearest of them. Without visibility
    // lists, SectionForPlacement. SIZE_MAX when no section qualifies.
    size_t SectionForPlacementAlong(const std::vector<Vec3>& points) const;

    // Every model in the region, one per hash.
    std::vector<const SolidObject*> AllModels() const;

private:
    std::wstring region_;
    bool emptyCity_ = false;
    bool foreignLookup_ = true;
    FileMap      layout_;   // L4Rx.BUN
    std::vector<u8> layoutUnpacked_;   // Underground 1: the unpacked TRACKB<n>.LZC
    ByteSpan LayoutSpan() const {
        return layoutUnpacked_.empty() ? layout_.Span() : ByteSpan{ layoutUnpacked_.data(), layoutUnpacked_.size() };
    }
    Game         game_ = Game::Underground2;
    // NFS World: the unpacked GLOBAL\GlobalB.lzc and the stand-in section
    // its sky domes are loaded under (World::LoadWorldSky).
    std::vector<u8> skyBundle_;
    Section      skySection_;
    void LoadWorldSky(const std::wstring& tracksDirectory);
    u32          trackNumber_ = 0;
    bool OpenUnderground1(const std::wstring& tracksDirectory, const std::wstring& region);
    // Underground 1's collision, one pack per drivable section (by zone).
    std::unordered_map<u32, CollisionPack> trackCollision_;
    void SplitTrackCollision();
    void NameUnderground1Scenery(LoadedSection& section) const;
    FileMap      stream_;   // STREAML4Rx.BUN
    std::vector<std::unique_ptr<FileMap>>     globalFiles_;
    std::vector<std::unique_ptr<TexturePack>> globalPacks_;
    void OpenGlobalPacks(const std::wstring& tracksDirectory);
    bool OpenWorldStream(const std::wstring& tracksDirectory, const std::wstring& region);
    std::wstring tracksDirectory_;
    SectionTable table_;
    std::vector<std::string> raceGroups_;
    std::unordered_map<u32, TextureAnimation> textureAnimations_;
    WorldAnimations objectAnimations_;
    void ReadTextureAnimations(const Chunk& chunk);
    void ReadRaceBarrierGroups();

    std::wstring overlayFolder_;
    std::unordered_map<u32, std::vector<Vec3>> overlays_;   // mesh hash -> positions

    // Rebuilt sections and the table corrections that go with them. A mesh that
    // gained or lost vertices cannot be patched a vertex at a time, so its whole
    // section arrives as one span -- and the editor has to read it the same way
    // the game will, or the viewport shows the shipped geometry while the game
    // shows the edited one.
    std::vector<SpanOverlay> spans_;
    Preculler    preculler_;

    std::vector<std::unique_ptr<LoadedSection>> loaded_;
    std::unordered_map<u32, const SolidObject*>  modelsByHash_;
    std::unordered_map<u32, size_t>              modelHome_;
    std::unordered_set<u32>                      instancedHashes_;
    std::unordered_set<u64>                      placements_;
    size_t                                       instanceCount_ = 0;
    std::unordered_map<u32, u32>                 textureUsage_;
    std::vector<int>                            sectionToLoaded_;  // -1 when not parsed
    Aabb                                        bounds_;

    // Half the typical distance between neighbouring section centres.
    float SectionPadding() const;
};

} // namespace nfs
