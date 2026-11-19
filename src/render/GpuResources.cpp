#include "render/GpuResources.h"

#include "core/Log.h"

#include <algorithm>
#include <cctype>
#include <cstring>

namespace {

// The surface format a texture is uploaded as. Palettised textures are expanded
// on the way in: D3DFMT_P8 needs palette support that modern drivers no longer
// offer, and the expansion costs a few megabytes across a whole region.
D3DFORMAT ToD3DFormat(nfs::TextureFormat format) {
    switch (format) {
        case nfs::TextureFormat::Dxt1:        return D3DFMT_DXT1;
        case nfs::TextureFormat::Dxt3:        return D3DFMT_DXT3;
        case nfs::TextureFormat::Dxt5:        return D3DFMT_DXT5;
        case nfs::TextureFormat::Ati1:        return (D3DFORMAT)MAKEFOURCC('A', 'T', 'I', '1');
        case nfs::TextureFormat::Ati2:        return (D3DFORMAT)MAKEFOURCC('A', 'T', 'I', '2');
        case nfs::TextureFormat::Palettised8: return D3DFMT_A8R8G8B8;
        case nfs::TextureFormat::Argb8888:    return D3DFMT_A8R8G8B8;
        default:                              return D3DFMT_UNKNOWN;
    }
}

// Shape of one mip level as it is stored in the file. DXT works on 4x4 blocks,
// so its dimensions are rounded up to whole blocks; a palettised level is one
// byte per texel.
struct LevelLayout {
    u32 rowBytes = 0;   // bytes in one source row
    u32 rowCount = 0;   // number of source rows
    u32 Size() const { return rowBytes * rowCount; }
};

LevelLayout SourceLayout(nfs::TextureFormat format, u32 width, u32 height) {
    LevelLayout layout;
    if (format == nfs::TextureFormat::Palettised8) {
        layout.rowBytes = width;
        layout.rowCount = height;
        return layout;
    }
    if (format == nfs::TextureFormat::Argb8888) {
        layout.rowBytes = width * 4;
        layout.rowCount = height;
        return layout;
    }

    const u32 blockBytes = (format == nfs::TextureFormat::Dxt1 || format == nfs::TextureFormat::Ati1) ? 8u : 16u;
    layout.rowBytes = std::max(1u, (width  + 3) / 4) * blockBytes;
    layout.rowCount = std::max(1u, (height + 3) / 4);
    return layout;
}

// Expands one row of palette indices into D3DFMT_A8R8G8B8.
//
// The palette holds D3DCOLOR values, so the low three bytes go straight across.
// The fourth byte is not opacity -- it carries a noisy gloss pattern the game
// uses for wet road reflections, and taking it as alpha would leave every
// street nearly invisible. Road surfaces are opaque: their gloss is kept in
// alpha (`keepAlpha`), which the editor's views ignore for opaque materials and
// the "Game look" view's WORLDREFLECT effect scales the reflection by. Other
// palettised textures keep the solid alpha they always had.
void ExpandPaletteRow(u8* destination, const u8* indices, u32 count, const u8* palette, bool keepAlpha) {
    for (u32 x = 0; x < count; ++x) {
        const u8* entry = palette + (size_t)indices[x] * 4;
        destination[x * 4 + 0] = entry[0];
        destination[x * 4 + 1] = entry[1];
        destination[x * 4 + 2] = entry[2];
        destination[x * 4 + 3] = keepAlpha ? entry[3] : 0xFF;
    }
}

} // namespace

// ---------------------------------------------------------------------------
// GpuSection
// ---------------------------------------------------------------------------

GpuSection::~GpuSection() {
    Release();
}

void GpuSection::Release() {
    if (vertexBuffer_) { vertexBuffer_->Release(); vertexBuffer_ = nullptr; }
    if (indexBuffer_)  { indexBuffer_->Release();  indexBuffer_  = nullptr; }
    if (colour1Buffer_) { colour1Buffer_->Release(); colour1Buffer_ = nullptr; }
    if (tangentBuffer_) { tangentBuffer_->Release(); tangentBuffer_ = nullptr; }
    objects_.clear();
    bounds_ = {};
    vertexCount_ = triangleCount_ = 0;
}

bool GpuSection::Upload(IDirect3DDevice9* device, const nfs::LoadedSection& section) {
    Release();
    if (!device) return false;

    // Meshes the region stores more than once at the same spot are skipped
    // outright: uploading them would waste memory and drawing them would make
    // the shared surfaces z-fight.
    size_t totalVertices = 0;
    size_t totalIndices  = 0;
    for (const nfs::SolidList& list : section.solidLists) {
        for (const nfs::SolidObject& object : list.objects) {
            if (object.duplicatePlacement) continue;
            totalVertices += object.vertices.size();
            totalIndices  += object.indices.size();
        }
    }

    if (totalVertices == 0 || totalIndices == 0) return false;

    // A 16-bit index buffer addresses at most 65536 vertices, but the base
    // vertex offset of each draw shifts that window, so a section only has to
    // keep every single object below that limit -- which the game guarantees.
    if (totalVertices > 0xFFFFFFFFull / sizeof(nfs::SolidVertex)) {
        LOG_ERROR("section %s is too large to upload", section.info->name.c_str());
        return false;
    }

    if (totalVertices > 0xFFFF)
        LOG_INFO("section %s merges %zu vertices into one buffer",
                 section.info ? section.info->name.c_str() : "?", totalVertices);

    const UINT vertexBytes = (UINT)(totalVertices * sizeof(nfs::SolidVertex));
    const UINT indexBytes  = (UINT)(totalIndices * sizeof(u16));

    // D3DPOOL_MANAGED survives a device reset, so resizing the viewport does not
    // force the whole world to be re-uploaded.
    if (FAILED(device->CreateVertexBuffer(vertexBytes, D3DUSAGE_WRITEONLY, kWorldVertexFvf,
                                          D3DPOOL_MANAGED, &vertexBuffer_, nullptr))) {
        LOG_ERROR("failed to create a %u byte vertex buffer", vertexBytes);
        return false;
    }

    if (FAILED(device->CreateIndexBuffer(indexBytes, D3DUSAGE_WRITEONLY, D3DFMT_INDEX16,
                                         D3DPOOL_MANAGED, &indexBuffer_, nullptr))) {
        LOG_ERROR("failed to create a %u byte index buffer", indexBytes);
        Release();
        return false;
    }

    void* vertexData = nullptr;
    void* indexData  = nullptr;
    if (FAILED(vertexBuffer_->Lock(0, 0, &vertexData, 0)) ||
        FAILED(indexBuffer_->Lock(0, 0, &indexData, 0))) {
        LOG_ERROR("failed to lock the section buffers");
        Release();
        return false;
    }

    auto* vertices = (nfs::SolidVertex*)vertexData;
    auto* indices  = (u16*)indexData;

    u32 vertexCursor = 0;
    u32 indexCursor  = 0;

    for (const nfs::SolidList& list : section.solidLists) {
        for (const nfs::SolidObject& object : list.objects) {
            if (object.duplicatePlacement) continue;

            GpuObject gpuObject;
            gpuObject.source      = &object;
            gpuObject.transform   = object.transform;
            gpuObject.worldBounds = object.WorldBounds();
            gpuObject.isBackdrop  = object.IsBackdrop();
            gpuObject.raceProp    = object.name.rfind("ZPM_", 0) == 0 ||
                                    object.name.find("TRACKBARRIER") != std::string::npos;
            gpuObject.baseVertex  = vertexCursor;
            {
                std::string upper = object.name;
                for (char& c : upper) c = (char)std::toupper((unsigned char)c);
                const bool pan = upper.rfind("PAN_", 0) == 0;
                const bool farCopy = (upper.size() >= 2 && upper.compare(upper.size() - 2, 2, "_Z") == 0) ||
                                     upper.find("_Z_") != std::string::npos;
                if (upper.rfind("LOD_", 0) == 0 || upper.find("_1Z_") != std::string::npos || (pan && farCopy))
                    gpuObject.backdropKind = kBackdropDistantLod;
                else if (pan)
                    gpuObject.backdropKind = kBackdropPanorama;
            }
            {
                u32 stripFlags = 0;
                for (const nfs::SolidMaterial& material : object.materials) stripFlags |= material.stripFlags;
                gpuObject.effect = (stripFlags & 0x01) ? kEffectWorldBone
                                 : (stripFlags & 0x40) ? kEffectWorldReflect
                                 : (stripFlags & 0x80) ? kEffectCar
                                                       : kEffectWorld;
            }

            std::memcpy(vertices + vertexCursor, object.vertices.data(),
                        object.vertices.size() * sizeof(nfs::SolidVertex));
            std::memcpy(indices + indexCursor, object.indices.data(),
                        object.indices.size() * sizeof(u16));

            for (const nfs::SolidMaterial& material : object.materials) {
                if (material.indexCount < 3) continue;

                GpuDraw draw;
                draw.startIndex     = indexCursor + material.firstIndex;
                draw.primitiveCount = material.indexCount / 3;
                draw.minVertex      = 0;
                draw.vertexCount    = (u32)object.vertices.size();
                draw.textureHash    = material.textureIndex < object.textureHashes.size()
                                          ? object.textureHashes[material.textureIndex]
                                          : 0;
                draw.effectId       = (u8)(material.flags & 0xFF);
                auto slotHash = [&](int k) {
                    return material.slots[k] < object.textureHashes.size() ? object.textureHashes[material.slots[k]] : 0u;
                };
                draw.normalHash     = slotHash(1);
                draw.specularHash   = slotHash(3);
                draw.materialFlags  = material.worldFlags;
                gpuObject.draws.push_back(draw);
            }

            vertexCursor  += (u32)object.vertices.size();
            indexCursor   += (u32)object.indices.size();
            triangleCount_ += object.TriangleCount();

            bounds_.Add(gpuObject.worldBounds);
            objects_.push_back(std::move(gpuObject));
        }
    }

    vertexBuffer_->Unlock();
    indexBuffer_->Unlock();

    vertexCount_ = vertexCursor;

    // The second colours, when any mesh of the section has them (NFS World).
    bool anyColour1 = false;
    for (const GpuObject& object : objects_)
        anyColour1 |= object.source && !object.source->colors1.empty() &&
                      object.source->colors1.size() == object.source->vertices.size();
    void* colourData = nullptr;
    if (anyColour1 &&
        SUCCEEDED(device->CreateVertexBuffer(vertexCursor * 4, D3DUSAGE_WRITEONLY, 0, D3DPOOL_MANAGED,
                                             &colour1Buffer_, nullptr)) &&
        SUCCEEDED(colour1Buffer_->Lock(0, 0, &colourData, 0))) {
        auto* colours = (u32*)colourData;
        for (GpuObject& object : objects_) {
            const auto& source = object.source->colors1;
            const size_t count = object.source->vertices.size();
            if (source.size() == count) {
                std::memcpy(colours + object.baseVertex, source.data(), count * 4);
                object.colour1 = colour1Buffer_;
            } else {
                std::memset(colours + object.baseVertex, 0, count * 4);
            }
        }
        colour1Buffer_->Unlock();
    } else if (colour1Buffer_) {
        colour1Buffer_->Release();
        colour1Buffer_ = nullptr;
    }

    // The tangents of the normal-map meshes (NFS World), 8 bytes per vertex.
    bool anyTangent = false;
    for (const GpuObject& object : objects_)
        anyTangent |= object.source && !object.source->tangents.empty() &&
                      object.source->tangents.size() == object.source->vertices.size();
    void* tangentData = nullptr;
    if (anyTangent &&
        SUCCEEDED(device->CreateVertexBuffer(vertexCursor * 8, D3DUSAGE_WRITEONLY, 0, D3DPOOL_MANAGED,
                                             &tangentBuffer_, nullptr)) &&
        SUCCEEDED(tangentBuffer_->Lock(0, 0, &tangentData, 0))) {
        auto* tangents = (u8*)tangentData;
        for (GpuObject& object : objects_) {
            const auto& source = object.source->tangents;
            const size_t count = object.source->vertices.size();
            if (source.size() == count) {
                std::memcpy(tangents + (size_t)object.baseVertex * 8, source.data(), count * 8);
                object.tangent = tangentBuffer_;
            } else {
                std::memset(tangents + (size_t)object.baseVertex * 8, 0, count * 8);
            }
        }
        tangentBuffer_->Unlock();
    } else if (tangentBuffer_) {
        tangentBuffer_->Release();
        tangentBuffer_ = nullptr;
    }
    return true;
}

// ---------------------------------------------------------------------------
// TextureCache
// ---------------------------------------------------------------------------

TextureCache::~TextureCache() {
    Clear();
}

void TextureCache::Clear() {
    for (auto& entry : textures_)
        if (entry.second.texture) entry.second.texture->Release();
    textures_.clear();
}


// What the game does with a texture, from the two alpha fields of its header.
// Alpha is only opacity when the texture says so: the road textures carry gloss
// there (usage 2 or 0, blend "copy") and are drawn solid.
SurfaceBlend BlendOf(const nfs::Texture& texture) {
    switch (texture.alphaBlend) {
        case nfs::AlphaBlend::Blend:    return SurfaceBlend::Blend;
        case nfs::AlphaBlend::Additive: return SurfaceBlend::Additive;
        default: break;
    }
    return texture.alphaUsage == nfs::AlphaUsage::PunchThru ? SurfaceBlend::CutOut
                                                             : SurfaceBlend::Opaque;
}

const CachedTexture& TextureCache::Get(IDirect3DDevice9* device, u32 hash) {
    static const CachedTexture kNone;
    if (!device || !world_ || hash == 0) return kNone;

    const auto it = textures_.find(hash);
    if (it != textures_.end()) return it->second;

    CachedTexture created;
    const nfs::Texture* source = world_->FindDisplayTexture(hash);
    // NFS World keeps a texture's larger levels apart from its pack, whose
    // copy is small (128 pixels as a rule): the view draws the whole chain,
    // so a texture looks as it does in the game from close by -- and a
    // replaced one at the size of its image.
    nfs::Texture full;
    std::vector<u8> chain;
    if (source && world_->FullTexture(hash, full, chain)) source = &full;
    if (!source && extra_) source = extra_(hash);
    if (source) {
        created.texture = Create(device, *source);
        created.blend   = BlendOf(*source);
        created.punchThru = source->alphaUsage == nfs::AlphaUsage::PunchThru;
        created.scrollType = source->scrollType;
        created.scrollStep = source->scrollStep;
        created.scrollS    = source->scrollS;
        created.scrollT    = source->scrollT;
    }
    return textures_.emplace(hash, created).first->second;
}

IDirect3DTexture9* TextureCache::Create(IDirect3DDevice9* device, const nfs::Texture& texture) {
    const D3DFORMAT format = ToD3DFormat(texture.format);
    if (format == D3DFMT_UNKNOWN || texture.width == 0 || texture.height == 0) {
        LOG_WARN("texture '%s' uses unsupported format %s",
                 texture.name.c_str(), nfs::ToString(texture.format));
        return nullptr;
    }

    const bool palettised = texture.format == nfs::TextureFormat::Palettised8;
    if (palettised && texture.palette.size < 256 * 4) {
        LOG_WARN("texture '%s' is palettised but carries no palette", texture.name.c_str());
        return nullptr;
    }

    IDirect3DTexture9* result = nullptr;
    if (FAILED(device->CreateTexture(texture.width, texture.height, texture.mipCount, 0,
                                     format, D3DPOOL_MANAGED, &result, nullptr)))
        return nullptr;

    // Mip levels follow each other in the pack, largest first.
    size_t offset = 0;
    const u32 levels = std::min<u32>(texture.mipCount, result->GetLevelCount());

    for (u32 level = 0; level < levels; ++level) {
        const u32 width  = std::max(1u, (u32)texture.width  >> level);
        const u32 height = std::max(1u, (u32)texture.height >> level);
        const LevelLayout layout = SourceLayout(texture.format, width, height);

        if (offset + layout.Size() > texture.pixels.size) break;

        D3DLOCKED_RECT locked{};
        if (SUCCEEDED(result->LockRect(level, &locked, nullptr, 0))) {
            // The driver may pad each row, so honour the returned pitch instead
            // of writing the level in one go.
            for (u32 row = 0; row < layout.rowCount; ++row) {
                u8* destination = (u8*)locked.pBits + (size_t)row * locked.Pitch;
                const u8* source = texture.pixels.data + offset +
                                   (size_t)row * layout.rowBytes;

                if (palettised) {
                    ExpandPaletteRow(destination, source, width, texture.palette.data,
                                     BlendOf(texture) == SurfaceBlend::Opaque);
                } else {
                    std::memcpy(destination, source, layout.rowBytes);
                }
            }
            result->UnlockRect(level);
        }
        offset += layout.Size();
    }

    return result;
}
