#pragma once

#include "core/Types.h"
#include "nfs/ChunkReader.h"

#include <string>
#include <unordered_map>
#include <vector>

namespace nfs {

// Pixel formats that appear in the shipped track packs.
enum class TextureFormat {
    Unknown,
    Dxt1,
    Dxt3,
    Dxt5,
    // NFS World: one-channel (ATI1, specular maps) and two-channel (ATI2, normal maps) block formats, 4x4
    // blocks of 8 and 16 bytes, uploaded with their FOURCC.
    Ati1,
    Ati2,
    // D3DFMT_P8: one byte per texel into a 256-entry palette. Only 23 textures
    // in a region use it, but they are the road lanes, intersections, parking
    // bays and pavements -- 3986 of the 27 987 material slots, which is every
    // street surface in the city.
    Palettised8,
    // D3DFMT_A8R8G8B8, uncompressed: Underground 1's sky (TEX<n>TRACK.BIN).
    Argb8888,
};

const char* ToString(TextureFormat format);

// How a texture's alpha is used, from +0x55 of its header.
enum class AlphaUsage : u8 {
    None       = 0,   // alpha is not opacity (the road textures keep gloss there)
    PunchThru  = 1,   // cut-out: trees, chain-link fences, railings
    Modulated  = 2,   // opacity that takes part in blending
};

// How the texture combines with what is behind it, from +0x56.
enum class AlphaBlend : u8 {
    SrcCopy  = 0,     // replaces what is behind it
    Blend    = 1,     // src * alpha + dst * (1 - alpha): smoke, glass, barrier tape
    Additive = 2,     // src * alpha + dst: neon, flares, light beams on black
};

// One texture inside a pack.
//
// Materials reference textures by hash, so the pack keeps a hash lookup. The
// pixel data is not copied: `pixels` points into the mapped stream file and
// stays valid as long as the mapping does. Textures of a compressed pack point
// into the pack's own decompressed copy instead.
struct Texture {
    std::string   name;
    u32           hash   = 0;
    u16           width  = 0;
    u16           height = 0;
    u8            mipCount = 1;
    TextureFormat format = TextureFormat::Unknown;
    u32           fourCc = 0;   // as the pack's format table has it, known to the editor or not
    AlphaUsage    alphaUsage = AlphaUsage::None;
    AlphaBlend    alphaBlend = AlphaBlend::SrcCopy;

    // Scrolling, from +0x52 (type: 0 none, 1 smooth, 2 in steps) and the
    // 8.8 fixed-point values at +0x58: seconds a step, texture lengths a
    // second along S and T. The fountains' and waterfalls' water flows by
    // it, rolling neon signs turn by it.
    u8    scrollType = 0;
    float scrollStep = 1.0f;
    float scrollS = 0.0f, scrollT = 0.0f;

    u32 baseLevelSize = 0;   // bytes of mip 0
    ByteSpan pixels;         // all mip levels, tightly packed
    ByteSpan palette;        // 256 entries, empty unless the format is palettised
};

// Whether the alpha is what shows through: a cut-out, or a texture that
// blends. Anything else is drawn solid whatever its alpha holds -- NFS World's
// ground and walls are marked Modulated with SrcCopy and keep gloss there.
inline bool AlphaIsOpacity(const Texture& texture) {
    return texture.alphaUsage == AlphaUsage::PunchThru || texture.alphaBlend != AlphaBlend::SrcCopy;
}

// A 0xB3300000 texture pack.
class TexturePack {
public:
    // Parses one pack chunk. `out` textures point into the buffer the chunk
    // came from, so keep the file mapped for as long as the pack is used.
    bool Parse(const Chunk& packChunk);

    const std::vector<Texture>& Textures() const { return textures_; }
    // For pointing a replaced texture at its new pixels (io/WorldWrite.h).
    std::vector<Texture>& MutableTextures() { return textures_; }

    const Texture* Find(u32 hash) const;

    // NFS World: a texture the editor made (io/WorldTexture.h), its pixels
    // owned by the caller. Replaces one of the same hash.
    void Add(const Texture& texture) {
        Remove(texture.hash);
        byHash_[texture.hash] = textures_.size();
        textures_.push_back(texture);
    }
    void Remove(u32 hash) {
        const auto it = byHash_.find(hash);
        if (it == byHash_.end()) return;
        textures_.erase(textures_.begin() + (std::ptrdiff_t)it->second);
        byHash_.clear();
        for (size_t i = 0; i < textures_.size(); ++i) byHash_[textures_[i].hash] = i;
    }

    const std::string& Name() const { return name_; }

private:
    // A compressed pack (TRACKS\LOC4DYNTEX.BIN: the sky, the weather and the
    // other textures the game swaps) keeps each texture as an EA-compressed
    // block holding its pixels, then its 124-byte header and 32-byte format.
    bool ParseCompressed(const Chunk& packChunk, ByteSpan index);

    std::string          name_;
    std::vector<Texture> textures_;
    std::vector<std::vector<u8>> owned_;   // decompressed blocks
    std::unordered_map<u32, size_t> byHash_;
};

} // namespace nfs
