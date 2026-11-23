#include "nfs/SolidList.h"

#include "core/Log.h"
#include "nfs/GameFormat.h"

#include <cstring>

namespace nfs {

namespace {

// --- Solid object header, chunk 0x00134011 ---------------------------------
constexpr size_t kHdrVersion   = 0x0C;
constexpr size_t kHdrHash      = 0x10;
constexpr size_t kHdrNumTris   = 0x14;
constexpr size_t kHdrBoundsMin = 0x20;
constexpr size_t kHdrBoundsMax = 0x30;
constexpr size_t kHdrTransform = 0x40;  // 4x4 floats, row major, translation in row 3
constexpr size_t kHdrName      = 0xA4;  // zero-terminated ASCII
// Underground 1 (header version 0x13 in the low word -- the crowd's skinned
// people carry 0x40 in the high one -- Underground 2's is 0x16) has the same
// header sixteen bytes shorter before the name.
constexpr u32    kHdrVersionU1 = 0x13;
constexpr size_t kHdrNameU1    = 0x94;

// --- Mesh descriptor, chunk 0x00134900 -------------------------------------
constexpr size_t kDescNumMaterials = 0x10;
constexpr size_t kDescNumTriangles = 0x24;
constexpr size_t kDescNumVertices  = 0x34;

// --- Material entry, chunk 0x00134B02 --------------------------------------
constexpr size_t kMaterialEntrySize = 60;
constexpr size_t kMatBoundsMin      = 0x00;
constexpr size_t kMatIndexCount     = 0x0C;
constexpr size_t kMatBoundsMax      = 0x10;
constexpr size_t kMatTextureIndex   = 0x1C;
constexpr size_t kMatFlags          = 0x20;
constexpr size_t kMatFirstIndex     = 0x34;
constexpr size_t kMatStripFlags     = 0x38;

// --- Vertex layouts --------------------------------------------------------
// Two strides occur across every shipped Underground 2 track stream.
constexpr u32 kStridePositionColorUv       = 24;  // pos, color, uv
constexpr u32 kStridePositionNormalColorUv = 36;  // pos, normal, color, uv
// Underground 1's crowd: the 36-byte layout, then bone weights and indices.
constexpr u32 kStrideSkinned               = 60;

// --- NFS World ---------------------------------------------------------------
// Measured over all 28 324 objects of TRACKS (L5RA). The header keeps the
// Underground 2 fields and moves the name; a material is 116 bytes; and the
// mesh holds one vertex buffer per run of materials that share an effect, each
// with the layout of that effect. Indices count from the start of their own
// buffer. No object reaches 65 536 vertices in total, so the buffers are
// joined into the one the editor draws from.
constexpr size_t kWorldHdrName = 0xA0;

constexpr size_t kWorldMaterialEntrySize = 116;
constexpr size_t kWorldMatEffect       = 0x08;
constexpr size_t kWorldMatBoundsMin    = 0x10;
constexpr size_t kWorldMatBoundsMax    = 0x1C;
constexpr size_t kWorldMatTextureIndex = 0x28;   // u8: the diffuse slot of five
constexpr size_t kWorldMatVertexCount  = 0x40;
constexpr size_t kWorldMatIndexCount   = 0x44;
constexpr size_t kWorldMatFirstIndex   = 0x4C;

// Where a layout keeps what the editor draws with; -1 when it has none.
struct WorldLayout {
    u32 stride;
    int normal;   // i16[4], 32767 = 1.0
    int color;    // D3DCOLOR
    int uv;       // float[2]
    int color1 = -1;   // D3DCOLOR, the second colour (nfsw.exe effect table 0xC83018 vertex layouts)
    int tangent = -1;  // i16[4]
    // CarShader / CARNORMALMAP (effects 8, 9): i16 position[4] (x / 32767 * 8
    // metres, w 4096), i16 uv[2] (/ 4096), colour, i16 normal[4], i16 tangent[4]
    // -- the car GEOMETRY.BIN layout (work/edge-car-import/solidio.py).
    bool packed = false;
};

bool WorldLayoutFor(u32 effect, u32 stride, WorldLayout& out) {
    switch (effect) {
        case 0x8: case 0x9:
            out = { 32, 16, 12, 8, -1, 24, true }; break;
        // WorldShader: position, SHORT4 normal, uv, colour, colour 1.
        case 0x0: case 0x1: case 0xA: case 0xC: case 0xF:
            out = { 36, 12, 28, 20, 32 }; break;
        // The same followed by a SHORT4 tangent (WorldRoad / WorldNormalMap).
        case 0x2: case 0x3: case 0x7: case 0xB:
            out = { 44, 12, 28, 20, 32, 36 }; break;
        // WorldPrelitShader: position, colour, colour 1, uv.
        case 0x4:
            out = { 28, -1, 12, 20, 16 }; break;
        case 0x5:
            out = { 36, 12, 20, 28 }; break;
        // skyshader: position, SHORT4 normal, uv, uv 1, colour (the SKYDOME_1_* models of GlobalB).
        case 0xE:
            out = { 40, 12, 36, 20 }; break;
        default:
            return false;
    }
    return out.stride == stride;
}

Vec3 ReadVec3(ByteSpan span, size_t offset) {
    return { span.read<float>(offset),
             span.read<float>(offset + 4),
             span.read<float>(offset + 8) };
}

Mat4 ReadMat4(ByteSpan span, size_t offset) {
    Mat4 m;
    for (int row = 0; row < 4; ++row)
        for (int col = 0; col < 4; ++col)
            m.m[row][col] = span.read<float>(offset + (size_t)(row * 4 + col) * 4);
    return m;
}

std::string ReadName(ByteSpan span, size_t offset) {
    if (offset >= span.size) return {};
    const char* text = (const char*)span.data + offset;
    const size_t available = span.size - offset;
    const size_t length = ::strnlen(text, available);
    return std::string(text, length);
}

void ParseHeader(ByteSpan data, SolidObject& out) {
    out.version    = data.read<u32>(kHdrVersion);
    out.hash       = data.read<u32>(kHdrHash);
    out.bounds.min = ReadVec3(data, kHdrBoundsMin);
    out.bounds.max = ReadVec3(data, kHdrBoundsMax);
    out.transform  = ReadMat4(data, kHdrTransform);
    out.name       = ReadName(data, IsWorld() ? kWorldHdrName
                                    : (out.version & 0xFFFFu) <= kHdrVersionU1 ? kHdrNameU1 : kHdrName);
    out.kind       = ClassifyObject(out.name);
}

void ParseTextureRefs(ByteSpan data, SolidObject& out) {
    // Eight bytes per entry: a texture hash followed by four reserved bytes.
    const size_t count = data.size / 8;
    out.textureHashes.reserve(count);
    for (size_t i = 0; i < count; ++i)
        out.textureHashes.push_back(data.read<u32>(i * 8));
}

void ParseMaterials(ByteSpan data, SolidObject& out) {
    const size_t count = data.size / kMaterialEntrySize;
    out.materials.reserve(count);

    for (size_t i = 0; i < count; ++i) {
        const ByteSpan entry = data.sub(i * kMaterialEntrySize, kMaterialEntrySize);

        SolidMaterial material;
        material.indexCount   = entry.read<u32>(kMatIndexCount);
        material.firstIndex   = entry.read<u32>(kMatFirstIndex);
        material.textureIndex = entry.read<u32>(kMatTextureIndex);
        material.flags        = entry.read<u32>(kMatFlags);
        material.stripFlags   = entry.read<u32>(kMatStripFlags);
        material.bounds.min   = ReadVec3(entry, kMatBoundsMin);
        material.bounds.max   = ReadVec3(entry, kMatBoundsMax);

        out.materials.push_back(material);
    }
}

// Widens the packed source vertices into SolidVertex.
bool ParseVertices(ByteSpan data, u32 vertexCount, SolidObject& out) {
    if (vertexCount == 0) return false;
    if (data.size % vertexCount != 0) {
        LOG_WARN("object '%s': vertex buffer %zu bytes is not a multiple of %u vertices",
                 out.name.c_str(), data.size, vertexCount);
        return false;
    }

    const u32 stride = (u32)(data.size / vertexCount);
    size_t colorOffset, uvOffset;
    bool hasNormal;

    switch (stride) {
        case kStridePositionColorUv:
            hasNormal = false; colorOffset = 12; uvOffset = 16;
            break;
        case kStridePositionNormalColorUv:
        case kStrideSkinned:
            hasNormal = true;  colorOffset = 24; uvOffset = 28;
            break;
        default:
            LOG_WARN("object '%s': unsupported vertex stride %u", out.name.c_str(), stride);
            return false;
    }

    out.hasNormals   = hasNormal;
    out.vertexStride = stride;
    out.vertices.resize(vertexCount);
    for (u32 i = 0; i < vertexCount; ++i) {
        const ByteSpan source = data.sub((size_t)i * stride, stride);
        SolidVertex& vertex = out.vertices[i];

        vertex.position = ReadVec3(source, 0);
        vertex.normal   = hasNormal ? ReadVec3(source, 12) : Vec3{ 0.0f, 0.0f, 1.0f };
        vertex.color    = source.read<u32>(colorOffset);
        vertex.uv       = { source.read<float>(uvOffset), source.read<float>(uvOffset + 4) };
    }
    return true;
}

void ParseIndices(ByteSpan data, u32 triangleCount, SolidObject& out) {
    // The buffer is padded out to an alignment boundary, so trust the triangle
    // count from the descriptor over the raw byte size.
    size_t count = (size_t)triangleCount * 3;
    if (count * 2 > data.size) count = data.size / 2;

    out.indices.resize(count);
    if (count) std::memcpy(out.indices.data(), data.data, count * 2);
}

} // namespace

namespace {

bool ParseWorldObject(const Chunk& objectChunk, SolidObject& out) {
    ChunkReader reader = ChunkReader::Children(objectChunk);
    Chunk child;

    std::vector<Chunk> buffers;
    ByteSpan indexData, materialData;
    ByteSpan placeRanges, placeMatrices;   // 0x134C05 / 0x134C06, see below

    while (reader.Next(child)) {
        if (child.id == kChunkSolidObjectHeader)       ParseHeader(child.data, out);
        else if (child.id == kChunkSolidObjectTexRefs) ParseTextureRefs(child.data, out);
        else if (child.id == kChunkMeshContainer) {
            ChunkReader mesh = ChunkReader::Children(child);
            Chunk part;
            while (mesh.Next(part)) {
                if (part.id == kChunkMeshVertices)       buffers.push_back(part);
                else if (part.id == kChunkMeshIndices)   indexData    = part.data;
                else if (part.id == kChunkMeshMaterials) materialData = part.data;
                else if (part.id == 0x00134C05u)         placeRanges   = part.data;
                else if (part.id == 0x00134C06u)         placeMatrices = part.data;
            }
        }
    }

    const size_t materialCount = materialData.size / kWorldMaterialEntrySize;
    const size_t indexTotal    = indexData.size / 2;
    auto entryAt = [&](size_t i) {
        return materialData.sub(i * kWorldMaterialEntrySize, kWorldMaterialEntrySize);
    };

    size_t bufferIndex = 0;
    for (size_t first = 0; first < materialCount; ++bufferIndex) {
        // The run: every following material with the same effect.
        const u32 effect = entryAt(first).read<u32>(kWorldMatEffect);
        size_t end = first;
        u32 runVertices = 0;
        while (end < materialCount && entryAt(end).read<u32>(kWorldMatEffect) == effect)
            runVertices += entryAt(end++).read<u32>(kWorldMatVertexCount);

        const size_t runFirst = first;
        first = end;
        if (bufferIndex >= buffers.size() || runVertices == 0) continue;

        const ByteSpan data = buffers[bufferIndex].data;
        WorldLayout layout{};
        if (data.size % runVertices != 0 ||
            !WorldLayoutFor(effect, (u32)(data.size / runVertices), layout)) {
            LOG_WARN("object '%s': effect %u with %zu bytes for %u vertices is not read yet",
                     out.name.c_str(), effect, data.size, runVertices);
            continue;
        }

        const size_t base = out.vertices.size();
        if (base + runVertices > 65536) {
            LOG_WARN("object '%s': more than 65536 vertices, the rest is left out", out.name.c_str());
            break;
        }

        if (base == 0) {
            // Packed positions cannot be written back as floats: no in-place edit.
            out.vertexFileOffset = layout.packed ? 0 : buffers[bufferIndex].dataOffset;
            out.vertexStride     = layout.packed ? 0 : layout.stride;
            out.hasNormals       = layout.normal >= 0;
        }

        out.vertices.resize(base + runVertices);
        for (u32 i = 0; i < runVertices; ++i) {
            const ByteSpan source = data.sub((size_t)i * layout.stride, layout.stride);
            SolidVertex& vertex = out.vertices[base + i];

            vertex.position = ReadVec3(source, 0);
            vertex.normal   = { 0.0f, 0.0f, 1.0f };
            if (layout.normal >= 0) {
                const float scale = 1.0f / 32767.0f;
                vertex.normal = { (float)source.read<i16>((size_t)layout.normal) * scale,
                                  (float)source.read<i16>((size_t)layout.normal + 2) * scale,
                                  (float)source.read<i16>((size_t)layout.normal + 4) * scale };
            }
            vertex.color = source.read<u32>((size_t)layout.color);
            if (layout.packed) {
                const float p = 8.0f / 32767.0f, t = 1.0f / 4096.0f;
                vertex.position = { source.read<i16>(0) * p, source.read<i16>(2) * p, source.read<i16>(4) * p };
                vertex.uv = { source.read<i16>((size_t)layout.uv) * t, source.read<i16>((size_t)layout.uv + 2) * t };
            } else {
                vertex.uv = { source.read<float>((size_t)layout.uv), source.read<float>((size_t)layout.uv + 4) };
            }
        }
        if (layout.color1 >= 0) {
            out.colors1.resize(out.vertices.size(), 0u);
            for (u32 i = 0; i < runVertices; ++i)
                out.colors1[base + i] = data.sub((size_t)i * layout.stride, layout.stride).read<u32>((size_t)layout.color1);
        } else if (!out.colors1.empty()) {
            out.colors1.resize(out.vertices.size(), 0u);
        }
        if (layout.tangent >= 0) {
            out.tangents.resize(out.vertices.size(), std::array<i16, 4>{});
            for (u32 i = 0; i < runVertices; ++i) {
                const ByteSpan source = data.sub((size_t)i * layout.stride, layout.stride);
                for (int k = 0; k < 4; ++k) out.tangents[base + i][k] = source.read<i16>((size_t)layout.tangent + 2 * k);
            }
        } else if (!out.tangents.empty()) {
            out.tangents.resize(out.vertices.size(), std::array<i16, 4>{});
        }

        // Several copies merged into one mesh (the "_DEINST" models: planters,
        // bollards, hedges): each copy's vertices are stored at its own origin.
        // nfsw.exe 0x64D5E0 / 0x645440 place them at load, in the first vertex
        // buffer only: 0x134C05 holds 16-byte ranges {first vertex, last vertex
        // (inclusive), matrix index, ?}, 0x134C06 64-byte row-major matrices
        // (p' = x*row0 + y*row1 + z*row2 + row3); normal and tangent turn with them.
        if (bufferIndex == 0 && !placeRanges.empty() && !placeMatrices.empty()) {
            const size_t matrices = placeMatrices.size / 64;
            for (size_t r = 0; r + 16 <= placeRanges.size; r += 16) {
                const u32 firstVertex = placeRanges.read<u32>(r), lastVertex = placeRanges.read<u32>(r + 4);
                const u32 matrix = placeRanges.read<u32>(r + 8);
                if (matrix >= matrices) continue;
                float m[16];
                for (int k = 0; k < 16; ++k) m[k] = placeMatrices.read<float>((size_t)matrix * 64 + (size_t)k * 4);
                const auto turn = [&](float x, float y, float z, int i) { return x * m[i] + y * m[4 + i] + z * m[8 + i]; };
                for (u32 v = firstVertex; v <= lastVertex && v < runVertices; ++v) {
                    SolidVertex& vertex = out.vertices[base + v];
                    const Vec3 p = vertex.position, n = vertex.normal;
                    vertex.position = { turn(p.x, p.y, p.z, 0) + m[12], turn(p.x, p.y, p.z, 1) + m[13], turn(p.x, p.y, p.z, 2) + m[14] };
                    vertex.normal   = { turn(n.x, n.y, n.z, 0), turn(n.x, n.y, n.z, 1), turn(n.x, n.y, n.z, 2) };
                    if (base + v < out.tangents.size()) {
                        std::array<i16, 4>& tg = out.tangents[base + v];
                        const float s = 1.0f / 32767.0f, x = tg[0] * s, y = tg[1] * s, z = tg[2] * s;
                        for (int k = 0; k < 3; ++k) tg[k] = (i16)(turn(x, y, z, k) * 32767.0f);
                    }
                }
            }
            // The file keeps the unplaced copies: no in-place vertex edit.
            out.vertexFileOffset = 0;
            out.vertexStride     = 0;
        }

        for (size_t m = runFirst; m < end; ++m) {
            const ByteSpan entry = entryAt(m);
            const u32 firstIndex = entry.read<u32>(kWorldMatFirstIndex);
            u32 indexCount       = entry.read<u32>(kWorldMatIndexCount);
            if (firstIndex >= indexTotal) continue;
            if (firstIndex + indexCount > indexTotal) indexCount = (u32)(indexTotal - firstIndex);

            SolidMaterial material;
            material.firstIndex   = (u32)out.indices.size();
            material.indexCount   = indexCount - indexCount % 3;
            material.textureIndex = entry.read<u8>(kWorldMatTextureIndex);
            for (int k = 0; k < 5; ++k) material.slots[k] = entry.read<u8>(kWorldMatTextureIndex + k);
            material.worldFlags   = entry.read<u32>(0);
            material.flags        = effect;
            material.bounds.min   = ReadVec3(entry, kWorldMatBoundsMin);
            material.bounds.max   = ReadVec3(entry, kWorldMatBoundsMax);

            // Indices count from the start of their own buffer.
            for (u32 i = 0; i < material.indexCount; ++i) {
                const u32 index = indexData.read<u16>(((size_t)firstIndex + i) * 2);
                out.indices.push_back((u16)(index < runVertices ? base + index : base));
            }
            out.materials.push_back(material);
        }
    }

    return !out.vertices.empty() && !out.indices.empty();
}

} // namespace

ObjectKind ClassifyObject(const std::string& name) {
    if (name.rfind("PAN_", 0) == 0)    return ObjectKind::Backdrop;  // panorama pieces
    if (name.rfind("SKYDOME", 0) == 0) return ObjectKind::Backdrop;
    if (name.rfind("TRN_", 0) == 0)    return ObjectKind::Terrain;
    return ObjectKind::Prop;
}

bool ParseSolidObject(const Chunk& objectChunk, SolidObject& out) {
    if (IsWorld()) return ParseWorldObject(objectChunk, out);

    ChunkReader reader = ChunkReader::Children(objectChunk);
    Chunk child;

    u32 vertexCount = 0;
    u32 triangleCount = 0;
    ByteSpan vertexData, indexData, materialData;

    while (reader.Next(child)) {
        switch (child.id) {
            case kChunkSolidObjectHeader:
                ParseHeader(child.data, out);
                triangleCount = child.data.read<u32>(kHdrNumTris);
                break;

            case kChunkSolidObjectTexRefs:
                ParseTextureRefs(child.data, out);
                break;

            case kChunkMeshContainer: {
                ChunkReader mesh = ChunkReader::Children(child);
                Chunk part;
                while (mesh.Next(part)) {
                    switch (part.id) {
                        case kChunkMeshDescriptor:
                            vertexCount = part.data.read<u32>(kDescNumVertices);
                            if (u32 tris = part.data.read<u32>(kDescNumTriangles))
                                triangleCount = tris;
                            break;
                        case kChunkMeshVertices:
                            vertexData = part.data;
                            out.vertexFileOffset = part.dataOffset;
                            break;
                        case kChunkMeshIndices:   indexData    = part.data; break;
                        case kChunkMeshMaterials: materialData = part.data; break;
                        default: break;
                    }
                }
                break;
            }

            default:
                break;
        }
    }

    if (!ParseVertices(vertexData, vertexCount, out)) return false;
    ParseIndices(indexData, triangleCount, out);
    ParseMaterials(materialData, out);

    // Guard the draw calls: a corrupt range would otherwise read past the buffer.
    for (SolidMaterial& material : out.materials) {
        if (material.firstIndex > out.indices.size()) {
            material.firstIndex = 0;
            material.indexCount = 0;
        } else if (material.firstIndex + material.indexCount > out.indices.size()) {
            material.indexCount = (u32)(out.indices.size() - material.firstIndex);
        }
    }

    return !out.vertices.empty() && !out.indices.empty();
}

bool ParseSolidList(const Chunk& pack, SolidList& out) {
    ChunkReader reader = ChunkReader::Children(pack);
    Chunk child;

    while (reader.Next(child)) {
        if (child.id == kChunkSolidListHeader) {
            // The pack path sits in the padding chunk right after the header.
            const Chunk info = FindChunk(child, kChunkSolidListPadding);
            if (info.data.size > 0x10)
                out.path = ReadName(info.data, 0x10);
        } else if (child.id == kChunkSolidObject) {
            SolidObject object;
            if (ParseSolidObject(child, object))
                out.objects.push_back(std::move(object));
        }
    }

    return !out.objects.empty();
}

void ParseAllSolidLists(ByteSpan buffer, u64 fileOffset, std::vector<SolidList>& out) {
    ChunkReader reader(buffer, fileOffset);
    Chunk chunk;

    while (reader.Next(chunk)) {
        if (chunk.id != kChunkSolidListPack) continue;

        SolidList list;
        if (ParseSolidList(chunk, list))
            out.push_back(std::move(list));
    }
}

} // namespace nfs
