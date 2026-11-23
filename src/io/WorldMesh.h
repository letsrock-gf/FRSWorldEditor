#pragma once

#include "nfs/World.h"

#include <string>
#include <vector>

// Edited meshes written into NFS World's own section files.
//
// A model is one 0x80134010 chunk in a solid list pack of the file that
// stores it: a header (0x134011), the texture list (0x134012) and a mesh
// container whose materials (0x134B02, 116 bytes each) are grouped in runs of
// one effect, each run with a vertex buffer (0x134B01) in that effect's layout,
// and one index buffer (0x134B03) whose ranges count from their run's start.
//
// An edit comes from Blender as the OBJ the editor exported (world space, Y up,
// with its manifest). It is turned into the model's own space and kept as an
// OBJ of its own, TRACKS\frs-original\meshes\<HASH>.obj, named in the edit list
// as `O <HASH> <file>`: the shipped file plus the list always gives the result.
//
// Writing rebuilds that model's materials, buffers and counts. Every vertex is
// written over the bytes of a shipped one -- itself when the mesh kept its
// vertices and triangles (moved, recoloured, re-mapped), else the nearest one of
// the same material -- so the fields the OBJ cannot carry (the second colour,
// the normal's fourth word, the tangent's handedness) keep the game's values;
// normals come from the OBJ and tangents are turned to stay square to them.
// When nothing changed size the object is written over itself; otherwise its
// solid list is laid out again and the padding after it takes up the change,
// growing the file by whole 128-byte steps when it cannot.
//
// Only the edited model changes. The other levels of detail the scenery names
// for it (an info holds four) stay as shipped: from far away the game may still
// show the old shape. Collision is a separate pack and is not touched.
namespace io {

struct WorldMeshImport {
    size_t objectsRead = 0;
    size_t staged      = 0;
    size_t created     = 0;     // of those, new models placed where they stand
    std::vector<u32> hashes;    // the models staged
    std::vector<size_t> sections;   // loaded sections that took a new model
    std::wstring done;          // one line per model staged
    std::wstring refused;       // one line per object left alone, and why
};

// A save in Blender: every object of `objPath` the manifest names becomes a
// kept model-space OBJ and is staged. An object the manifest does not name --
// or every object, when there is no manifest and `newOrigin` says where the
// OBJ's origin stands -- becomes a NEW model (see below) placed where it
// stands. Nothing in the game's files changes until the next Save.
bool ImportWorldMeshes(nfs::World& world, const std::wstring& tracksFolder, const std::wstring& objPath,
                       WorldMeshImport& out, const Vec3* newOrigin = nullptr);

// --- models the editor makes -----------------------------------------------------
// A new model is written into the file of the section it stands in: its object
// is put into that file's last solid list, in hash order (nfsw.exe 0x747700
// finds a model by a binary search of the list's 0x134003 rows, count at
// 0x134002 +0x0C), and the section's scenery gets an info and a record for it
// (io/WorldPlace.h). Each OBJ material names a texture the game has
// (tex_<hash>, or the texture's own name) that a model of that section
// already draws with as its first texture: that material is taken whole --
// effect, flags, the other four texture slots, shader name (0x134C02) -- and
// only the geometry is new. A material that names none gets a new texture
// (io/WorldTexture.h) from its image (MTL map_Kd, or <obj>\<material>.png
// beside the OBJ) or its Kd colour, named FRS_<image or material>; its
// material is copied from a texture of the section picked by PickLikeTexture
// in WorldMesh.cpp, and slot 0 then names the new texture.
//
// A placement whose model the section neither stores nor names in its own
// scenery may not be loaded where it stands (shipped: every info a numbered
// section has names a model in its own file or a shared library file, and no
// model is stored twice). Such a model is copied into the section's file under
// a hash of its own, `<name>_FRS<section>`, when every texture it draws with is
// one the section's own models draw with; otherwise it is written as before (the
// record names the model where it is stored) and the log says the game may not
// show it there.

// Takes the kept model-space OBJ of a new model into section `sectionNumber`
// (what the viewport draws, and listed for the next Save). The edit list does
// this on opening, before the placements that use it.
bool StageNewWorldModel(nfs::World& world, u32 sectionNumber, u32 hash, const std::wstring& keptPath,
                        std::wstring& error);

// Whether a placement anywhere uses this model.
bool ModelPlaced(const nfs::World& world, u32 hash);

// The new textures (World::newTextures) a placed new model draws with: what
// Save writes and lists.
std::vector<u32> NewTexturesInUse(const nfs::World& world);

// For every placement of a model its section does not load, copies the game
// textures it draws with that the section's models do not into a texture
// section the section loads (io/WorldTexture.h StageCopiedWorldTexture), so
// SectionModelCopies can copy the model. Save calls it first; returns the
// textures those placements need, to write.
std::vector<u32> StageCopiedTextures(nfs::World& world);

struct WorldModelCopy {
    u32 source = 0, copy = 0;
    std::string name;
};
// The models `loaded`'s placements need copied into its file. False, with why,
// when one cannot be.
bool SectionModelCopies(const nfs::World& world, const nfs::LoadedSection& loaded,
                        std::vector<WorldModelCopy>& out, std::wstring& error);

// Takes the kept OBJ of `hash` into the loaded model (what the viewport draws)
// and lists it for the next Save. The edit list does this on opening.
bool StageWorldMesh(nfs::World& world, u32 hash, const std::wstring& keptPath, std::wstring& error);

// Whether this section's file stores or places an edited model.
bool SectionHasMeshEdits(const nfs::World& world, const nfs::LoadedSection& loaded);

// Rebuilds the edited models this section's file stores. `bytes` is the
// shipped file and may change size.
bool WriteWorldMeshes(const nfs::World& world, const nfs::LoadedSection& loaded, std::vector<u8>& bytes,
                      size_t& written, std::wstring& error);

// The furthest any vertex of the loaded model is from its origin: the radius a
// scenery info keeps for it.
float ModelRadius(const nfs::SolidObject& model);

// --- cars (nfs/CarGeometry.h) ----------------------------------------------------
// A placed car is a new model of its own (entry.car names it): the car's stock
// parts at LOD B merged into one object, every material as the car ships it
// (CarShader), every texture it draws with that the region does not have
// copied from the car's TEXTURES.BIN beside a texture the section draws with.
// Its paint strips (light material kCarPaintLightMaterial) name the light
// material CarPaintName(hash) instead, which Save writes into the section's
// file before the solid list (chunk 0x135200, CarPaintMaterial) from the
// entry's paint. Saved as `K <section> <HASH> <car> <hue> <lightness>
// <saturation> <metallic> <gloss>` in the edit list.
bool StageCarModel(nfs::World& world, const std::wstring& gameFolder, const nfs::World::NewModel& entry,
                   std::wstring& error);
// The entry's paint shown in the view: the painted materials' vertex colour.
void ShowCarPaint(nfs::World& world, u32 hash);
std::string CarPaintName(u32 hash);
// The car textures placed cars draw with: what Save writes.
std::vector<u32> CarTexturesInUse(const nfs::World& world);
// Before Save: each placed car's FRS_SKIN_<hash> (its <CAR>_SKIN1, which only the paint strips draw with) tinted
// to the entry's paint, as a new texture beside the section's. On a world model this is the paint the game shows:
// the light material is not applied there (21/11 in game).
void StageCarPaintTextures(nfs::World& world);
// The new-model entry of a car model, or null.
nfs::World::NewModel* CarModelOf(nfs::World& world, u32 hash);
// No window, no region (FRSWorldEditor --cars-selftest <game folder> <car> <out file>): the car's object as
// StageCarModel builds it, read back, and a paint light material, written to `outFile` (the paint chunk, then
// the object chunk); a line of counts to the log. 0 when it reads back.
int CarModelSelfTest(const std::wstring& gameFolder, const std::string& car, const std::wstring& outFile);

} // namespace io
