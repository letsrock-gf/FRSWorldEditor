#pragma once

#include "nfs/World.h"

#include <string>
#include <vector>

namespace io {

// Lays out again the texture packs of one NFS World section file -- `bytes`,
// as shipped apart from patches made in place -- around the replaced textures
// that are not the size they shipped at (BytePatch::Resized). Their headers
// take the new size, the textures behind them move, and the file grows by a
// multiple of 128 bytes so that whatever follows stays aligned as it was.
//
// Nothing is done, and true returned, for a section with no such texture.
bool ResizeTextures(const nfs::LoadedSection& loaded, std::vector<u8>& bytes, std::wstring& error);

// --- New textures (World::newTextures), for the models the editor makes ------
//
// NFS World keeps the map's textures in texture sections (table tags U..Y,
// files STREAML5RA_0x<hash>.BUN), one pack each, which the game loads beside
// the map sections that draw with them. A new texture goes into every texture
// section whose pack holds `like` -- the game texture of the material the new
// model's material is copied from (io/WorldMesh.h), which a model of the same
// section draws with, so that pack is loaded wherever the new model is.
//
// Read from nfsw.exe (REA): 0x74C890 counts a pack's textures as the hash
// table's size / 8 and refuses it when the format table (32 bytes each) holds
// another count; 0x74AF40 pairs header i (0x91 bytes + the length byte at
// +0x90) with hash entry i, 0x749F70 stores the header's address at entry +4;
// 0x747700 finds textures by binary search of that table. Measured on every
// texture section: the table is sorted (3034/3034), entry +4 is 0, hash =
// bStringHash of the upper-case name (5032/5102, the rest cut), the length
// byte is the name and its NUL made up so the header ends on 4 (5155/5155),
// format entries hold only the FourCC at +0x0C (5155/5155). 1452 shipped
// textures have no larger level in any 0xB3300011 pack, up to 2048 px, and no
// header field tells them apart: a new texture is stored whole in the pack,
// at most 512 px a side, its levels down to 4 px.

// A name the game can take for an image or material brought in: FRS_ and the
// name in upper case, A-Z 0-9 _ only, at most 31 characters, and not the name
// of a texture the game has (a number is added).
std::string NewTextureName(const nfs::World& world, const std::string& wanted);

// The texture sections whose pack holds `like`.
std::vector<const nfs::LoadedSection*> TextureHosts(const nfs::World& world, u32 like);

// Encodes the image `keptFile` (its full path) for a new texture `name` that
// copies `like`, puts it into the first host's pack in memory (so the view
// draws it) and records it. Again with the same name replaces it.
bool StageNewWorldTexture(nfs::World& world, const std::string& name, u32 like, const std::wstring& keptFile,
                          std::wstring& error);

const nfs::World::NewTexture* NewTextureOf(const nfs::World& world, u32 hash);

// The texture sections that take a staged texture.
std::vector<const nfs::LoadedSection*> NewTextureHosts(const nfs::World& world, const nfs::World::NewTexture& texture);

// A game texture copied, same hash, name and header, for a model placed in a
// section that does not load it (io/WorldMesh.h SectionModelCopies): into the
// texture sections whose pack holds `like`, a texture that section draws with.
// Its levels as the editor has them loaded (pack and larger levels), at most
// 512 a side. Again with another `like` adds that one's sections.
bool StageCopiedWorldTexture(nfs::World& world, u32 hash, u32 like, std::wstring& error);

// Whether a staged new texture names this section as a host.
bool HostsNewTextures(const nfs::World& world, const nfs::LoadedSection& loaded);

// Writes into the pack of `bytes` (the section's file) the new textures in
// `inUse` that this section hosts; the file grows by a multiple of 128.
bool WriteNewTextures(const nfs::World& world, const nfs::LoadedSection& loaded, const std::vector<u32>& inUse,
                      std::vector<u8>& bytes, size_t& added, std::wstring& error);

} // namespace io
