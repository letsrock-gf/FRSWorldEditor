#pragma once

#include "core/Types.h"

#include <string>
#include <vector>

struct NewTexture;

namespace nfs {

struct SolidObject;
struct Texture;

// Models from outside the open region -- other games' (ui/U1Library.h:
// Need for Speed Underground's; Most Wanted's) -- offered in the model library beside the
// region's own.
//
// The region answers for its own models first; only a hash it does not hold
// is asked of this source. Such a model is placed like one from another
// section of the region: shown from the source's copy until the placements
// are written, when the source hands over the model's object chunk already in
// this game's layout and the textures it needs, and the section takes them in
// (io/PlacementWrite.cpp).
class ForeignModels {
public:
    virtual ~ForeignModels() = default;

    // Every model it offers, one per hash.
    virtual void ListModels(std::vector<const SolidObject*>& out) const = 0;
    // The game they come from, as the model library's filter names it.
    virtual std::wstring Game() const = 0;

    virtual const SolidObject* FindModel(u32 hash) const = 0;
    virtual const Texture* FindTexture(u32 hash) const = 0;

    // The model's whole object chunk (0x80134010) as Underground 2 stores
    // one, laid out for a file position of `mod128` modulo 128. The bytes
    // stay valid while the source is open.
    virtual bool ModelChunk(u32 hash, ByteSpan& chunk, u32& mod128, std::wstring& error) const = 0;

    // A texture of a foreign model, ready to be added to a section's pack.
    virtual bool CopyTexture(u32 hash, NewTexture& out) const = 0;

    // Where the model comes from, for the catalogue and the status bar:
    // "Underground 1, L1RA X0".
    virtual std::wstring Origin(u32 hash) const = 0;
};

// The sources the editor consults, one per game (ui/U1Library.h, ...),
// in the order they were added. Changed from the UI thread only.
void AddForeignModels(const ForeignModels* source);
void RemoveForeignModels(const ForeignModels* source);
const std::vector<const ForeignModels*>& ForeignSources();

// The first source that has the model; its model; a texture of any source.
const ForeignModels* ForeignSourceOf(u32 hash);
const SolidObject* FindForeignModel(u32 hash);
const Texture* FindForeignTexture(u32 hash);

} // namespace nfs
