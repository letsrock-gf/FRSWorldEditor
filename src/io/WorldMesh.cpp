#include "io/WorldMesh.h"

#include "core/Log.h"
#include "io/ObjMesh.h"
#include "io/SelectionImport.h"
#include "io/TextureImport.h"
#include "io/WorldTexture.h"
#include "io/WorldWrite.h"
#include "nfs/ChunkReader.h"
#include "nfs/SolidList.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <functional>
#include <iterator>
#include <map>
#include <unordered_map>
#include <unordered_set>

#include <windows.h>

namespace io {

namespace {

// --- the object's fields, as measured over 3153 objects (tools/nfsw/mesh_stats.py) -----
constexpr size_t kHdrHash      = 0x10;
constexpr size_t kHdrTriangles = 0x14;
constexpr size_t kHdrMin       = 0x20;
constexpr size_t kHdrMax       = 0x30;
constexpr size_t kHdrArea      = 0x98;   // the triangles' area
constexpr size_t kHdrDensity   = 0x9C;   // triangles per unit of that area
constexpr size_t kHdrSize      = 0xA0;

constexpr size_t kDescMaterials = 0x10;
constexpr size_t kDescBuffers   = 0x18;  // one per run of materials with one effect
constexpr size_t kDescIndices   = 0x2C;
constexpr size_t kDescSize      = 0x30;

constexpr size_t kMatSize      = 116;
constexpr size_t kMatEffect    = 0x08;
constexpr size_t kMatMin       = 0x10;
constexpr size_t kMatMax       = 0x1C;
constexpr size_t kMatVertices  = 0x40;
constexpr size_t kMatIndices   = 0x44;
constexpr size_t kMatTriangles = 0x48;
constexpr size_t kMatFirst     = 0x4C;   // into the index buffer; ranges count from their run's start

template <class T>
T Read(const u8* at) { T value; std::memcpy(&value, at, sizeof(T)); return value; }
template <class T>
void Write(u8* at, T value) { std::memcpy(at, &value, sizeof(T)); }
Vec3 ReadVec3(const u8* at) { return { Read<float>(at), Read<float>(at + 4), Read<float>(at + 8) }; }
void WriteVec3(u8* at, const Vec3& v) { Write(at, v.x); Write(at + 4, v.y); Write(at + 8, v.z); }

std::wstring Widen(const std::string& text) { return std::wstring(text.begin(), text.end()); }

// Where each effect's vertex keeps what an OBJ carries; -1 when it has none.
// The bytes not named here (a second colour, the normal's fourth word, the
// tangent's handedness) are the shipped vertex's.
struct Layout {
    u32 stride;
    int normal;    // i16[4], 32767 = 1
    int color;     // D3DCOLOR
    int uv;        // float[2]
    int tangent;   // i16[4]
};

bool LayoutFor(u32 effect, Layout& out) {
    switch (effect) {
        case 0x0: case 0x1: case 0xA: case 0xC: case 0xF: out = { 36, 12, 28, 20, -1 }; return true;
        case 0x2: case 0x3: case 0x7: case 0xB:           out = { 44, 12, 28, 20, 36 }; return true;
        case 0x4:                                         out = { 28, -1, 12, 20, -1 }; return true;
        case 0x5:                                         out = { 36, 12, 20, 28, -1 }; return true;
        default:                                          return false;
    }
}

// --- chunks as a tree, written back the way they were read ------------------------------
struct Node {
    u32 id = 0;
    bool aligned = false;          // its payload starts on PayloadAlignment(id)
    std::vector<u8> payload;       // a leaf's, without the 0x11 filler
    std::vector<Node> children;    // a container's
};

// The chunks the game's files always align to 16 (measured over 390 files:
// never seen unaligned); anything else only when it had filler. The texture
// list (0x134012), 0x134013 and the names (0x134C02) often start anywhere.
bool AlwaysAligned(u32 id) {
    switch (id) {
        case nfs::kChunkSolidObjectHeader: case nfs::kChunkMeshDescriptor: case nfs::kChunkMeshVertices:
        case nfs::kChunkMeshMaterials: case nfs::kChunkMeshIndices: case 0x0013401A: case 0x00134C05:
        case 0x00134C06:
            return true;
        default:
            return false;
    }
}

std::vector<Node> ParseNodes(ByteSpan span, u64 fileOffset) {
    std::vector<Node> out;
    nfs::ChunkReader reader(span, fileOffset);
    reader.SetSkipZeroPadding(false);
    nfs::Chunk chunk;
    while (reader.Next(chunk)) {
        Node node;
        node.id = chunk.id;
        if (chunk.IsContainer()) {
            node.children = ParseNodes(chunk.raw, chunk.headerOffset + 8);
        } else {
            node.payload.assign(chunk.data.data, chunk.data.data + chunk.data.size);
            node.aligned = chunk.FillerBytes() > 0 || AlwaysAligned(chunk.id);
        }
        out.push_back(std::move(node));
    }
    return out;
}

// Appends `nodes` to `out`, whose first byte is at `base` in the file.
void Emit(std::vector<u8>& out, const std::vector<Node>& nodes, u64 base) {
    for (const Node& node : nodes) {
        const size_t header = out.size();
        out.resize(header + 8);
        if (nfs::IsContainer(node.id)) {
            Emit(out, node.children, base);
        } else {
            if (node.aligned) {
                const u64 alignment = nfs::PayloadAlignment(node.id);
                const u64 at = base + out.size();
                out.insert(out.end(), (size_t)((alignment - at % alignment) % alignment), (u8)0x11);
            }
            out.insert(out.end(), node.payload.begin(), node.payload.end());
        }
        Write<u32>(out.data() + header, node.id);
        Write<u32>(out.data() + header + 4, (u32)(out.size() - header - 8));
    }
}

Node* ChildOf(Node& parent, u32 id) {
    for (Node& child : parent.children)
        if (child.id == id) return &child;
    return nullptr;
}

u32 HashOf(Node& object) {
    const Node* header = ChildOf(object, nfs::kChunkSolidObjectHeader);
    return header && header->payload.size() >= kHdrSize ? Read<u32>(header->payload.data() + kHdrHash) : 0;
}

// --- the edit --------------------------------------------------------------------------
struct EditVertex {
    Vec3 position;
    Vec3 normal{ 0.0f, 0.0f, 1.0f };
    Vec2 uv;
    u32  color = 0xFFFFFFFFu;
    bool hasTangent = false;   // a new model's, from its own UVs
    Vec3 tangent;
};

struct MeshEdit {
    // Same vertices and triangles as shipped: one per shipped vertex, in the
    // order the object numbers them (its runs one after the other).
    bool same = false;
    std::vector<EditVertex> byOriginal;
    // Otherwise, per material: its vertices and its triangles into them.
    std::vector<std::vector<EditVertex>> vertices;
    std::vector<std::vector<u16>> indices;
};

// The materials and vertices the shipped object has, from its chunks.
bool CountsOf(Node& object, size_t& materials, size_t& vertices) {
    Node* mesh = ChildOf(object, nfs::kChunkMeshContainer);
    Node* entries = mesh ? ChildOf(*mesh, nfs::kChunkMeshMaterials) : nullptr;
    if (!entries || entries->payload.size() % kMatSize) return false;
    materials = entries->payload.size() / kMatSize;
    vertices = 0;
    for (size_t m = 0; m < materials; ++m) vertices += Read<u32>(entries->payload.data() + m * kMatSize + kMatVertices);
    return true;
}

bool ReadKeptMesh(const std::wstring& path, size_t materialCount, size_t vertexCount, MeshEdit& edit,
                  std::wstring& why) {
    std::vector<ObjMesh> meshes;
    if (!ReadObjMeshes(path, meshes, why) || meshes.empty()) {
        if (why.empty()) why = L"the kept mesh " + path + L" holds no faces";
        return false;
    }
    const ObjMesh& mesh = meshes.front();
    {
        std::ifstream file(path);
        std::string line;
        while (std::getline(file, line) && !line.empty() && line[0] == '#')
            if (line.find("frs-topology same") != std::string::npos) edit.same = true;
    }
    const auto vertexAt = [&](size_t k) {
        EditVertex v;
        v.position = mesh.positions[k];
        v.normal   = mesh.normals[k];
        v.uv       = mesh.uvs[k];
        v.color    = mesh.colors[k];
        return v;
    };

    if (edit.same) {
        if (mesh.positions.size() != vertexCount) {
            why = L"the kept mesh has " + std::to_wstring(mesh.positions.size()) + L" vertices, the model " +
                  std::to_wstring(vertexCount);
            return false;
        }
        edit.byOriginal.resize(vertexCount);
        std::vector<char> seen(vertexCount, 0);
        for (size_t k = 0; k < mesh.positions.size(); ++k) {
            const u32 source = mesh.sourcePositions[k];
            if (source >= vertexCount || seen[source]) { why = L"the kept mesh repeats a vertex"; return false; }
            seen[source] = 1;
            edit.byOriginal[source] = vertexAt(k);
        }
        return true;
    }

    edit.vertices.assign(materialCount, {});
    edit.indices.assign(materialCount, {});
    std::vector<std::unordered_map<u16, u16>> local(materialCount);
    for (const ObjFaceGroup& group : mesh.groups) {
        unsigned material = 0;
        if (std::sscanf(group.material.c_str(), "m%u", &material) != 1 || material >= materialCount) {
            why = L"the kept mesh names a material (" + Widen(group.material) + L") the model does not have";
            return false;
        }
        for (u32 i = group.firstIndex; i < group.firstIndex + group.indexCount && i < mesh.indices.size(); ++i) {
            const u16 k = mesh.indices[i];
            const auto [at, fresh] = local[material].emplace(k, (u16)edit.vertices[material].size());
            if (fresh) edit.vertices[material].push_back(vertexAt(k));
            edit.indices[material].push_back(at->second);
        }
    }
    return true;
}

i16 PackUnit(float value) { return (i16)std::lround(std::clamp(value, -1.0f, 1.0f) * 32767.0f); }

// One vertex in `layout`, over the bytes of the shipped vertex `shipped`.
void Encode(u8* out, const u8* shipped, const Layout& layout, const EditVertex& v) {
    u8 bytes[64];
    std::memcpy(bytes, shipped, layout.stride);
    WriteVec3(bytes, v.position);
    const Vec3 n = Length(v.normal) > 1e-6f ? Normalize(v.normal) : Vec3{ 0.0f, 0.0f, 1.0f };
    if (layout.normal >= 0) {
        Write<i16>(bytes + layout.normal, PackUnit(n.x));
        Write<i16>(bytes + layout.normal + 2, PackUnit(n.y));
        Write<i16>(bytes + layout.normal + 4, PackUnit(n.z));
    }
    if (layout.tangent >= 0) {
        // ponytail: the shipped tangent turned square to the new normal, handedness
        // kept; a fresh UV layout would want tangents from its own UV gradients.
        const u8* t = bytes + layout.tangent;
        Vec3 tangent{ Read<i16>(t) / 32767.0f, Read<i16>(t + 2) / 32767.0f, Read<i16>(t + 4) / 32767.0f };
        if (v.hasTangent) tangent = v.tangent;
        tangent = tangent - n * Dot(n, tangent);
        if (Length(tangent) > 1e-4f) {
            tangent = Normalize(tangent);
            Write<i16>(bytes + layout.tangent, PackUnit(tangent.x));
            Write<i16>(bytes + layout.tangent + 2, PackUnit(tangent.y));
            Write<i16>(bytes + layout.tangent + 4, PackUnit(tangent.z));
        }
    }
    Write<float>(bytes + layout.uv, v.uv.x);
    Write<float>(bytes + layout.uv + 4, v.uv.y);
    const u32 old = Read<u32>(bytes + layout.color);
    Write<u32>(bytes + layout.color, (old & 0xFF000000u) | (v.color & 0x00FFFFFFu));
    std::memcpy(out, bytes, layout.stride);
}

// The shipped vertex nearest a point, among a material's: sorted by x, and
// the search stops once x alone is further than the best so far.
class Nearest {
public:
    explicit Nearest(std::vector<Vec3> points) : points_(std::move(points)) {
        order_.resize(points_.size());
        for (u32 i = 0; i < order_.size(); ++i) order_[i] = i;
        std::sort(order_.begin(), order_.end(), [&](u32 a, u32 b) { return points_[a].x < points_[b].x; });
    }
    u32 Find(const Vec3& p) const {
        const auto start = std::lower_bound(order_.begin(), order_.end(), p.x,
                                            [&](u32 i, float x) { return points_[i].x < x; }) - order_.begin();
        float best = 1e30f;
        u32 found = order_.empty() ? 0 : order_.front();
        const auto look = [&](std::ptrdiff_t at) {
            const Vec3 d = points_[order_[(size_t)at]] - p;
            const float distance = Dot(d, d);
            if (distance < best) { best = distance; found = order_[(size_t)at]; }
            return d.x * d.x <= best;
        };
        for (std::ptrdiff_t i = start; i < (std::ptrdiff_t)order_.size() && look(i); ++i) {}
        for (std::ptrdiff_t i = start - 1; i >= 0 && look(i); --i) {}
        return found;
    }
private:
    std::vector<Vec3> points_;
    std::vector<u32> order_;
};

// Rebuilds `object` (a 0x80134010 node as shipped) with `edit`.
bool BuildObject(Node& object, const MeshEdit& edit, std::wstring& why) {
    Node* header = ChildOf(object, nfs::kChunkSolidObjectHeader);
    Node* mesh   = ChildOf(object, nfs::kChunkMeshContainer);
    Node* desc   = mesh ? ChildOf(*mesh, nfs::kChunkMeshDescriptor) : nullptr;
    Node* mats   = mesh ? ChildOf(*mesh, nfs::kChunkMeshMaterials) : nullptr;
    Node* index  = mesh ? ChildOf(*mesh, nfs::kChunkMeshIndices) : nullptr;
    if (!header || !desc || !mats || !index || header->payload.size() < kHdrSize ||
        desc->payload.size() < kDescSize || mats->payload.size() % kMatSize) {
        why = L"the model's chunks are not the layout the editor writes";
        return false;
    }
    std::vector<Node*> buffers;
    for (Node& child : mesh->children)
        if (child.id == nfs::kChunkMeshVertices) buffers.push_back(&child);

    // --- the shipped runs --------------------------------------------------------------
    const size_t materialCount = mats->payload.size() / kMatSize;
    const auto entry = [&](size_t m) { return mats->payload.data() + m * kMatSize; };
    struct Run { u32 effect; Layout layout; size_t first, end; u32 vertices; size_t globalBase; };
    std::vector<Run> runs;
    std::vector<size_t> runOf(materialCount);
    std::vector<u32> baseOf(materialCount);   // the material's first vertex in its run
    size_t global = 0;
    for (size_t m = 0; m < materialCount; ++m) {
        const u32 effect = Read<u32>(entry(m) + kMatEffect);
        if (runs.empty() || runs.back().effect != effect) runs.push_back({ effect, {}, m, m, 0, global });
        Run& run = runs.back();
        runOf[m] = runs.size() - 1;
        baseOf[m] = run.vertices;
        run.vertices += Read<u32>(entry(m) + kMatVertices);
        run.end = m + 1;
        global += Read<u32>(entry(m) + kMatVertices);
    }
    if (runs.size() != buffers.size()) {
        why = L"the model has " + std::to_wstring(buffers.size()) + L" vertex buffers for " +
              std::to_wstring(runs.size()) + L" runs of materials";
        return false;
    }
    for (size_t r = 0; r < runs.size(); ++r) {
        if (!LayoutFor(runs[r].effect, runs[r].layout)) {
            why = L"it uses effect " + std::to_wstring(runs[r].effect) +
                  L", whose vertices (integer-packed in effects 8 and 9) the editor does not write yet";
            return false;
        }
        if (buffers[r]->payload.size() != (size_t)runs[r].vertices * runs[r].layout.stride) {
            why = L"a vertex buffer is not the size its materials say";
            return false;
        }
    }
    const auto shippedVertex = [&](size_t m, u32 local) {
        const Run& run = runs[runOf[m]];
        return buffers[runOf[m]]->payload.data() + (size_t)(baseOf[m] + local) * run.layout.stride;
    };

    Aabb bounds;
    double area = 0.0;
    u32 indexTotal = 0;
    const auto addTriangle = [&](const Vec3& a, const Vec3& b, const Vec3& c) {
        area += 0.5 * (double)Length(Cross(b - a, c - a));
    };

    if (edit.same) {
        // --- moved, not reshaped: every vertex over itself, indices as shipped ----------
        if (edit.byOriginal.size() != global) { why = L"the kept mesh does not match the model's vertices"; return false; }
        for (size_t m = 0; m < materialCount; ++m) {
            const Layout& layout = runs[runOf[m]].layout;
            const u32 count = Read<u32>(entry(m) + kMatVertices);
            const size_t first = runs[runOf[m]].globalBase + baseOf[m];
            Aabb box;
            for (u32 v = 0; v < count; ++v) {
                u8* at = shippedVertex(m, v);
                Encode(at, at, layout, edit.byOriginal[first + v]);
                box.Add(edit.byOriginal[first + v].position);
            }
            if (box.Valid()) {
                WriteVec3(entry(m) + kMatMin, box.min);
                WriteVec3(entry(m) + kMatMax, box.max);
                bounds.Add(box.min);
                bounds.Add(box.max);
            }
            const u32 firstIndex = Read<u32>(entry(m) + kMatFirst), indices = Read<u32>(entry(m) + kMatIndices);
            const size_t runBase = runs[runOf[m]].globalBase;
            for (u32 i = 0; i + 2 < indices && (size_t)(firstIndex + i + 2) * 2 < index->payload.size(); i += 3) {
                const u8* at = index->payload.data() + (size_t)(firstIndex + i) * 2;
                const size_t a = runBase + Read<u16>(at), b = runBase + Read<u16>(at + 2), c = runBase + Read<u16>(at + 4);
                if (a < global && b < global && c < global)
                    addTriangle(edit.byOriginal[a].position, edit.byOriginal[b].position, edit.byOriginal[c].position);
            }
            indexTotal += indices;
        }
    } else {
        // --- reshaped: each material's vertices and triangles laid out again --------------
        if (edit.vertices.size() != materialCount) { why = L"the kept mesh does not match the model's materials"; return false; }
        std::vector<u8> newMaterials;
        std::vector<u16> newIndices;
        std::vector<std::vector<u8>> newBuffers;
        std::vector<char> keptMaterial(materialCount, 0);
        for (const Run& run : runs) {
            std::vector<u8> buffer;
            u32 runVertices = 0;
            for (size_t m = run.first; m < run.end; ++m) {
                const auto& vertices = edit.vertices[m];
                const auto& triangles = edit.indices[m];
                if (triangles.size() < 3) continue;   // a material left with no faces goes
                if (runVertices + vertices.size() > 0xFFFF) {
                    why = L"one run of its materials would need more than 65535 vertices";
                    return false;
                }
                // The shipped vertices to write over: the material's own, or its
                // run's when it shipped with none.
                size_t source = m;
                u32 sourceCount = Read<u32>(entry(m) + kMatVertices);
                for (size_t other = run.first; other < run.end && sourceCount == 0; ++other)
                    if (Read<u32>(entry(other) + kMatVertices)) { source = other; sourceCount = Read<u32>(entry(other) + kMatVertices); }
                if (sourceCount == 0) { why = L"a run of its materials shipped with no vertices"; return false; }
                std::vector<Vec3> points(sourceCount);
                for (u32 v = 0; v < sourceCount; ++v) points[v] = ReadVec3(shippedVertex(source, v));
                const Nearest nearest(std::move(points));

                const size_t at = buffer.size();
                buffer.resize(at + vertices.size() * run.layout.stride);
                Aabb box;
                for (size_t v = 0; v < vertices.size(); ++v) {
                    Encode(buffer.data() + at + v * run.layout.stride,
                           shippedVertex(source, nearest.Find(vertices[v].position)), run.layout, vertices[v]);
                    box.Add(vertices[v].position);
                }
                const size_t whole = triangles.size() / 3 * 3;
                std::vector<u8> material(entry(m), entry(m) + kMatSize);
                WriteVec3(material.data() + kMatMin, box.min);
                WriteVec3(material.data() + kMatMax, box.max);
                Write<u32>(material.data() + kMatVertices, (u32)vertices.size());
                Write<u32>(material.data() + kMatIndices, (u32)whole);
                Write<u32>(material.data() + kMatTriangles, (u32)(whole / 3));
                Write<u32>(material.data() + kMatFirst, (u32)newIndices.size());
                newMaterials.insert(newMaterials.end(), material.begin(), material.end());
                keptMaterial[m] = 1;
                for (size_t i = 0; i < whole; i += 3)
                    addTriangle(vertices[triangles[i]].position, vertices[triangles[i + 1]].position,
                                vertices[triangles[i + 2]].position);
                for (size_t i = 0; i < whole; ++i) newIndices.push_back((u16)(runVertices + triangles[i]));
                bounds.Add(box.min);
                bounds.Add(box.max);
                runVertices += (u32)vertices.size();
            }
            if (runVertices) newBuffers.push_back(std::move(buffer));
        }
        if (newIndices.empty()) { why = L"no triangles are left"; return false; }

        mats->payload = std::move(newMaterials);
        index->payload.assign((const u8*)newIndices.data(), (const u8*)newIndices.data() + newIndices.size() * 2);
        if (newIndices.size() % 2) index->payload.resize(index->payload.size() + 2, 0);   // to 4 bytes, as shipped
        std::vector<Node> children;
        bool placed = false;
        size_t name = 0;
        for (Node& child : mesh->children) {
            // One shader name per material, in order (nfsw.exe 0x64D5E0 writes
            // the n-th into material n): a material that went takes its name along.
            if (child.id == 0x00134C02) {
                if (name < materialCount && keptMaterial[name++]) children.push_back(std::move(child));
                continue;
            }
            if (child.id != nfs::kChunkMeshVertices) { children.push_back(std::move(child)); continue; }
            if (placed) continue;
            placed = true;
            for (std::vector<u8>& buffer : newBuffers) {
                Node node;
                node.id = nfs::kChunkMeshVertices;
                node.aligned = true;
                node.payload = std::move(buffer);
                children.push_back(std::move(node));
            }
        }
        mesh->children = std::move(children);
        desc = ChildOf(*mesh, nfs::kChunkMeshDescriptor);
        header = ChildOf(object, nfs::kChunkSolidObjectHeader);
        Write<u32>(desc->payload.data() + kDescMaterials, (u32)(ChildOf(*mesh, nfs::kChunkMeshMaterials)->payload.size() / kMatSize));
        Write<u32>(desc->payload.data() + kDescBuffers, (u32)newBuffers.size());
        indexTotal = (u32)newIndices.size();
    }

    Write<u32>(desc->payload.data() + kDescIndices, indexTotal);
    u8* h = header->payload.data();
    Write<u32>(h + kHdrTriangles, indexTotal / 3);
    if (bounds.Valid()) {
        WriteVec3(h + kHdrMin, bounds.min);
        WriteVec3(h + kHdrMax, bounds.max);
    }
    Write<float>(h + kHdrArea, (float)area);
    Write<float>(h + kHdrDensity, area > 0.0 ? (float)((indexTotal / 3) / area) : 0.0f);
    return true;
}

// --- the model in its file ----------------------------------------------------------------
bool FindObject(ByteSpan file, u32 hash, Node& node, u64& offset, ByteSpan& raw) {
    nfs::ChunkReader top(file, 0);
    nfs::Chunk pack;
    while (top.Next(pack)) {
        if (pack.id != nfs::kChunkSolidListPack) continue;
        nfs::ChunkReader inside = nfs::ChunkReader::Children(pack);
        nfs::Chunk object;
        while (inside.Next(object)) {
            if (object.id != nfs::kChunkSolidObject) continue;
            const nfs::Chunk header = nfs::FindChunk(object, nfs::kChunkSolidObjectHeader);
            if (header.data.size < kHdrSize || header.data.read<u32>(kHdrHash) != hash) continue;
            node = Node{};
            node.id = object.id;
            node.children = ParseNodes(object.raw, object.headerOffset + 8);
            offset = object.headerOffset;
            raw = file.sub((size_t)object.headerOffset, object.raw.size + 8);
            return true;
        }
    }
    return false;
}

// `bytes` (an object chunk at `offset` of its file) read as the game's model.
bool ParseModel(const std::vector<u8>& bytes, u64 offset, nfs::SolidObject& out) {
    nfs::ChunkReader reader(ByteSpan{ bytes.data(), bytes.size() }, offset);
    nfs::Chunk chunk;
    return reader.Next(chunk) && nfs::ParseSolidObject(chunk, out);
}

struct Shipped {
    Node node;                 // the object chunk as shipped
    u64 offset = 0;
    size_t materials = 0, vertices = 0;
    nfs::SolidObject model;    // and as the editor reads it
};

bool FindShipped(const nfs::World& world, u32 hash, Shipped& out, std::wstring& why) {
    for (const auto& loaded : world.Loaded()) {
        if (!loaded->info || !loaded->HoldsModel(hash)) continue;
        ByteSpan raw;
        if (!FindObject(world.SectionBytes(*loaded->info), hash, out.node, out.offset, raw)) continue;
        std::vector<u8> again;
        Emit(again, { out.node }, out.offset);
        if (again.size() != raw.size || std::memcmp(again.data(), raw.data, raw.size) != 0) {
            why = L"its file lays the model out in a way the editor does not write yet";
            return false;
        }
        if (!CountsOf(out.node, out.materials, out.vertices) || !ParseModel(again, out.offset, out.model)) {
            why = L"the model could not be read from its file";
            return false;
        }
        return true;
    }
    why = L"the file that stores the model was not found";
    return false;
}

Vec3 FromBlender(const Vec3& v) { return { v.x, -v.z, v.y }; }

// The OBJ's material, as one of the model's: tex_<hash> is the first with that
// texture, tex_<hash>_m<n> material n, untextured the first with none.
int MaterialFor(std::string name, const nfs::SolidObject& model) {
    const size_t dot = name.find_last_of('.');
    if (dot != std::string::npos && dot + 1 < name.size() &&
        name.find_first_not_of("0123456789", dot + 1) == std::string::npos)
        name.resize(dot);   // Blender's .001
    const auto textureOf = [&](size_t m) {
        const u32 slot = model.materials[m].textureIndex;
        return slot < model.textureHashes.size() ? model.textureHashes[slot] : 0u;
    };
    if (name == "untextured") {
        for (size_t m = 0; m < model.materials.size(); ++m)
            if (!textureOf(m)) return (int)m;
        return -1;
    }
    unsigned hash = 0, material = 0;
    char tail = 0;
    if (std::sscanf(name.c_str(), "tex_%8x_m%u%c", &hash, &material, &tail) == 2)
        return material < model.materials.size() && textureOf(material) == hash ? (int)material : -1;
    if (std::sscanf(name.c_str(), "tex_%8x%c", &hash, &tail) == 1)
        for (size_t m = 0; m < model.materials.size(); ++m)
            if (textureOf(m) == hash) return (int)m;
    return -1;
}

// Corners Blender wrote apart for their normal alone are one vertex again:
// a game vertex has one normal, and a mesh that was only moved keeps its own
// vertices. Same `v` line, UV and colour make one vertex; normals averaged.
ObjMesh MergeCorners(const ObjMesh& mesh) {
    ObjMesh out;
    out.name = mesh.name;
    out.groups = mesh.groups;
    std::map<std::array<u32, 4>, u16> seen;
    std::vector<u16> mergedOf(mesh.positions.size());
    for (size_t k = 0; k < mesh.positions.size(); ++k) {
        std::array<u32, 4> key{ mesh.sourcePositions[k], 0, 0, mesh.colors[k] };
        std::memcpy(&key[1], &mesh.uvs[k].x, 4);
        std::memcpy(&key[2], &mesh.uvs[k].y, 4);
        const auto [at, fresh] = seen.emplace(key, (u16)out.positions.size());
        if (fresh) {
            out.positions.push_back(mesh.positions[k]);
            out.normals.push_back({ 0.0f, 0.0f, 0.0f });
            out.uvs.push_back(mesh.uvs[k]);
            out.colors.push_back(mesh.colors[k]);
            out.sourcePositions.push_back(mesh.sourcePositions[k]);
        }
        out.normals[at->second] = out.normals[at->second] + mesh.normals[k];
        mergedOf[k] = at->second;
    }
    for (Vec3& n : out.normals) n = Length(n) > 1e-6f ? Normalize(n) : Vec3{ 0.0f, 0.0f, 1.0f };
    out.indices.reserve(mesh.indices.size());
    for (u16 k : mesh.indices) out.indices.push_back(mergedOf[k]);
    return out;
}

using Triangle = std::array<u32, 3>;
Triangle Rotated(u32 a, u32 b, u32 c) {   // same winding, smallest first
    if (b < a && b < c) return { b, c, a };
    if (c < a && c < b) return { c, a, b };
    return { a, b, c };
}

// Whether the OBJ is the model's own vertices and triangles, maybe moved.
bool SameTopology(const ObjMesh& mesh, const std::vector<int>& groupMaterial, const nfs::SolidObject& model) {
    const size_t count = model.vertices.size();
    if (mesh.positions.size() != count || mesh.sourcePositions.size() != count) return false;
    std::vector<char> seen(count, 0);
    for (u32 source : mesh.sourcePositions) {
        if (source >= count || seen[source]) return false;
        seen[source] = 1;
    }
    std::vector<std::vector<Triangle>> want(model.materials.size()), got(model.materials.size());
    for (size_t m = 0; m < model.materials.size(); ++m) {
        const nfs::SolidMaterial& material = model.materials[m];
        for (u32 i = material.firstIndex; i + 2 < material.firstIndex + material.indexCount; i += 3)
            want[m].push_back(Rotated(model.indices[i], model.indices[i + 1], model.indices[i + 2]));
    }
    for (size_t g = 0; g < mesh.groups.size(); ++g) {
        const ObjFaceGroup& group = mesh.groups[g];
        for (u32 i = group.firstIndex; i + 2 < group.firstIndex + group.indexCount; i += 3)
            got[(size_t)groupMaterial[g]].push_back(Rotated(mesh.sourcePositions[mesh.indices[i]],
                                                            mesh.sourcePositions[mesh.indices[i + 1]],
                                                            mesh.sourcePositions[mesh.indices[i + 2]]));
    }
    for (size_t m = 0; m < want.size(); ++m) {
        std::sort(want[m].begin(), want[m].end());
        std::sort(got[m].begin(), got[m].end());
        if (want[m] != got[m]) return false;
    }
    return true;
}

// --- models the editor makes ---------------------------------------------------------------
// Measured on the shipped files (tools/nfsw/newmodel_fields.py, newmodel_rel.py) and read
// in nfsw.exe (0x74D550 object, 0x64D5E0 mesh, 0x749C80 / 0x747700 solid list):
constexpr size_t kMatSlots   = 0x28;  // five u8 texture slots (indices into 0x134012), 0xFF after
constexpr size_t kHdrCounts  = 0x18;  // u8 ?, u8 textures, u8 0x134013 entries, u8 0x13401A rows
constexpr size_t kHdrMatrix  = 0x40;  // float 4x4
constexpr size_t kDescFlags  = 0x0C;  // the OR of the materials' +0x00, in all 3691 objects measured
constexpr u32 kChunkShaderName = 0x00134C02;   // one per material, in order
constexpr const char* kNewList = "frs-new";     // World::AddModel's solid lists

// What nfsw.exe names a solid by: its hash is this of its name (23 of 23 checked).
u32 StringHash(const std::string& text) {
    u32 hash = 0xFFFFFFFFu;
    for (unsigned char c : text) hash = hash * 33u + c;
    return hash;
}

// The header with another name: 0xA0 bytes, then the name, its NUL, up to 4.
std::vector<u8> HeaderWithName(const std::vector<u8>& header, const std::string& name) {
    std::vector<u8> out(header.begin(), header.begin() + (std::ptrdiff_t)kHdrSize);
    out.insert(out.end(), name.begin(), name.end());
    out.resize((out.size() + 1 + 3) / 4 * 4, 0);
    Write<u32>(out.data() + kHdrHash, StringHash(name));
    return out;
}

// The models the game has loaded whenever this section draws: what its file
// stores and what its own scenery names (every level of every info).
std::vector<u32> LoadedWith(const nfs::LoadedSection& loaded) {
    std::vector<u32> out;
    for (const nfs::SolidList& list : loaded.solidLists)
        for (const nfs::SolidObject& object : list.objects) out.push_back(object.hash);
    for (const nfs::SceneryInfo& info : loaded.scenery.infos)
        for (size_t level = 0; level < 4 && info.raw.size() >= 0x28; ++level)
            if (const u32 hash = Read<u32>(info.raw.data() + 0x18 + level * 4)) out.push_back(hash);
    std::sort(out.begin(), out.end());
    out.erase(std::unique(out.begin(), out.end()), out.end());
    return out;
}

const nfs::LoadedSection* SectionNumbered(const nfs::World& world, u32 number, size_t* index = nullptr) {
    for (size_t i = 0; i < world.Loaded().size(); ++i)
        if (world.Loaded()[i]->info && world.Loaded()[i]->info->number == number) {
            if (index) *index = i;
            return world.Loaded()[i].get();
        }
    return nullptr;
}

std::wstring Hex(u32 value) {
    wchar_t text[16];
    ::_snwprintf_s(text, _TRUNCATE, L"%08X", value);
    return text;
}

// A shipped material drawing with a texture as its first: taken whole for a
// new model's material that draws with that texture.
struct Donor {
    u32 model = 0;
    std::vector<u8> header, descriptor, material, shaderName, vertex;
    std::array<u32, 5> slots{};   // the texture each slot names; 0 where 0xFF
    Layout layout{};
    u32 effect = 0;
};

bool FindDonor(const nfs::World& world, const nfs::LoadedSection& target, u32 texture, Donor& out) {
    for (const u32 hash : LoadedWith(target)) {
        const nfs::SolidObject* model = world.FindModel(hash);
        if (!model || std::find(model->textureHashes.begin(), model->textureHashes.end(), texture) ==
                          model->textureHashes.end())
            continue;
        Shipped shipped;
        std::wstring ignored;
        if (!FindShipped(world, hash, shipped, ignored)) continue;
        Node& object = shipped.node;
        Node* header = ChildOf(object, nfs::kChunkSolidObjectHeader);
        Node* textures = ChildOf(object, nfs::kChunkSolidObjectTexRefs);
        Node* mesh = ChildOf(object, nfs::kChunkMeshContainer);
        Node* desc = mesh ? ChildOf(*mesh, nfs::kChunkMeshDescriptor) : nullptr;
        Node* mats = mesh ? ChildOf(*mesh, nfs::kChunkMeshMaterials) : nullptr;
        if (!header || !textures || !desc || !mats || header->payload.size() < kHdrSize ||
            desc->payload.size() < kDescSize || mats->payload.size() % kMatSize)
            continue;
        std::vector<Node*> buffers, names;
        for (Node& child : mesh->children) {
            if (child.id == nfs::kChunkMeshVertices) buffers.push_back(&child);
            if (child.id == kChunkShaderName) names.push_back(&child);
        }
        const size_t count = mats->payload.size() / kMatSize;
        if (names.size() != count) continue;
        const auto list = [&](u8 slot) {
            return (size_t)slot * 8 + 4 <= textures->payload.size() ? Read<u32>(textures->payload.data() + slot * 8) : 0u;
        };
        // The runs, as BuildObject reads them, to find the material's first vertex.
        size_t run = 0, inRun = 0;
        for (size_t m = 0; m < count; ++m) {
            const u8* entry = mats->payload.data() + m * kMatSize;
            const u32 effect = Read<u32>(entry + kMatEffect);
            if (m > 0 && effect != Read<u32>(entry - kMatSize + kMatEffect)) { ++run; inRun = 0; }
            const u32 vertices = Read<u32>(entry + kMatVertices);
            Layout layout;
            const bool usable = entry[kMatSlots] != 0xFF && list(entry[kMatSlots]) == texture && vertices > 0 &&
                                LayoutFor(effect, layout) && run < buffers.size() &&
                                (inRun + 1) * layout.stride <= buffers[run]->payload.size();
            if (usable) {
                out.model = hash;
                out.header.assign(header->payload.begin(), header->payload.begin() + (std::ptrdiff_t)kHdrSize);
                out.descriptor = desc->payload;
                out.material.assign(entry, entry + kMatSize);
                out.shaderName = names[m]->payload;
                for (size_t s = 0; s < 5; ++s) out.slots[s] = entry[kMatSlots + s] == 0xFF ? 0u : list(entry[kMatSlots + s]);
                const u8* first = buffers[run]->payload.data() + inRun * layout.stride;
                out.vertex.assign(first, first + layout.stride);
                out.layout = layout;
                out.effect = effect;
                return true;
            }
            inRun += vertices;
        }
    }
    return false;
}

// The texture an OBJ material names: tex_<hash> (the editor's own), else the
// name of one of the game's textures. 0 when it names none the game has.
u32 MaterialTexture(const nfs::World& world, std::string name) {
    const size_t dot = name.find_last_of('.');
    if (dot != std::string::npos && dot + 1 < name.size() &&
        name.find_first_not_of("0123456789", dot + 1) == std::string::npos)
        name.resize(dot);   // Blender's .001
    unsigned hash = 0;
    if (std::sscanf(name.c_str(), "tex_%8x", &hash) == 1 && world.FindTexture(hash)) return hash;
    for (char& c : name) c = (char)::toupper((unsigned char)c);
    const u32 named = StringHash(name);
    return world.FindTexture(named) ? named : 0u;
}

// A flat colour as a picture, for a material that has no image: 8 x 8, a
// 24-bit BMP (every reader takes one).
bool WriteColourBmp(const std::wstring& path, u32 argb) {
    u8 file[54 + 8 * 8 * 3] = {};
    const auto put32 = [&](size_t at, u32 value) { std::memcpy(file + at, &value, 4); };
    file[0] = 'B';
    file[1] = 'M';
    put32(2, sizeof(file));
    put32(10, 54);
    put32(14, 40);
    put32(18, 8);
    put32(22, 8);
    file[26] = 1;
    file[28] = 24;
    put32(34, 8 * 8 * 3);
    for (size_t i = 0; i < 64; ++i) {
        file[54 + i * 3]     = (u8)(argb & 0xFF);
        file[54 + i * 3 + 1] = (u8)((argb >> 8) & 0xFF);
        file[54 + i * 3 + 2] = (u8)((argb >> 16) & 0xFF);
    }
    FILE* f = nullptr;
    if (::_wfopen_s(&f, path.c_str(), L"wb") != 0 || !f) return false;
    const bool ok = std::fwrite(file, 1, sizeof(file), f) == sizeof(file);
    std::fclose(f);
    return ok;
}

// The game texture a new texture copies (io/WorldTexture.h): one a model the
// section loads draws with as the first of a material the editor writes,
// stored in a texture section, DXT1/3/5. Its alpha is opacity when the image
// has some and not otherwise where there is such a one, and of those the
// material using the fewest texture slots (a normal map of the copied
// material stays on the new one).
u32 PickLikeTexture(const nfs::World& world, const nfs::LoadedSection& target, bool alpha) {
    u32 best = 0;
    size_t bestSlots = 99;
    bool bestFits = false;
    std::vector<u32> tried;
    for (const u32 hash : LoadedWith(target)) {
        const nfs::SolidObject* model = world.FindModel(hash);
        if (!model) continue;
        for (const u32 texture : model->textureHashes) {
            if (std::find(tried.begin(), tried.end(), texture) != tried.end()) continue;
            tried.push_back(texture);
            const nfs::Texture* game = world.FindTexture(texture);
            if (!game || NewTextureOf(world, texture) ||
                (game->format != nfs::TextureFormat::Dxt1 && game->format != nfs::TextureFormat::Dxt3 &&
                 game->format != nfs::TextureFormat::Dxt5))
                continue;
            const bool fits = nfs::AlphaIsOpacity(*game) == alpha;
            if (bestFits && !fits) continue;
            if (TextureHosts(world, texture).empty()) continue;
            Donor donor;
            if (!FindDonor(world, target, texture, donor)) continue;
            const size_t slots = (size_t)std::count_if(donor.slots.begin(), donor.slots.end(), [](u32 s) { return s != 0; });
            if ((fits && !bestFits) || slots < bestSlots) {
                best = texture;
                bestSlots = slots;
                bestFits = fits;
            }
        }
    }
    return best;
}


// A new model's object, from its kept OBJ: every material over a donor (see
// WorldMesh.h), the vertices and triangles the OBJ's own.
bool BuildNewNode(const nfs::World& world, const nfs::World::NewModel& entry, const std::string& name, Node& out,
                  std::wstring& why) {
    const nfs::LoadedSection* target = SectionNumbered(world, entry.section);
    if (!target) { why = L"its section " + std::to_wstring(entry.section) + L" is not loaded"; return false; }
    std::vector<ObjMesh> meshes;
    if (!ReadObjMeshes(entry.file, meshes, why) || meshes.empty() || meshes.front().indices.size() < 3) {
        if (why.empty()) why = L"the kept mesh " + entry.file + L" holds no faces";
        return false;
    }
    const ObjMesh& mesh = meshes.front();

    struct Part {
        const Donor* donor = nullptr;
        std::vector<EditVertex> vertices;
        std::vector<u16> indices;
    };
    std::map<u32, Donor> donors;
    std::vector<Part> parts;
    for (const ObjFaceGroup& group : mesh.groups) {
        if (group.indexCount < 3) continue;
        unsigned texture = 0;
        if (std::sscanf(group.material.c_str(), "tex_%8x", &texture) != 1) {
            why = L"the kept mesh names material " + Widen(group.material) + L", not tex_<hash>";
            return false;
        }
        auto found = donors.find(texture);
        if (found == donors.end()) {
            // A new texture's material is the one of the game texture it copies.
            const nfs::World::NewTexture* made = NewTextureOf(world, texture);
            Donor donor;
            if (!FindDonor(world, *target, made ? made->like : texture, donor)) {
                why = L"no model section " + Widen(target->info->name) + L" draws with texture " +
                      Hex(made ? made->like : texture) + L" as its first, in a layout the editor writes";
                return false;
            }
            if (made) donor.slots[0] = texture;
            found = donors.emplace(texture, std::move(donor)).first;
        }
        Part part;
        part.donor = &found->second;
        std::unordered_map<u16, u16> local;
        for (u32 i = group.firstIndex; i < group.firstIndex + group.indexCount && i < mesh.indices.size(); ++i) {
            const u16 k = mesh.indices[i];
            const auto [at, fresh] = local.emplace(k, (u16)part.vertices.size());
            if (fresh) {
                EditVertex v;
                v.position = mesh.positions[k];
                v.normal   = mesh.normals[k];
                v.uv       = mesh.uvs[k];
                v.color    = mesh.colors[k];
                part.vertices.push_back(v);
            }
            part.indices.push_back(at->second);
        }
        part.indices.resize(part.indices.size() / 3 * 3);

        // Normals the OBJ left out come from the faces; tangents always do,
        // from the UVs (u's direction across each triangle).
        std::vector<Vec3> faceNormal(part.vertices.size()), tangent(part.vertices.size());
        for (size_t i = 0; i + 2 < part.indices.size(); i += 3) {
            EditVertex& a = part.vertices[part.indices[i]];
            EditVertex& b = part.vertices[part.indices[i + 1]];
            EditVertex& c = part.vertices[part.indices[i + 2]];
            const Vec3 e1 = b.position - a.position, e2 = c.position - a.position;
            const Vec3 n = Cross(e1, e2);
            const float du1 = b.uv.x - a.uv.x, dv1 = b.uv.y - a.uv.y, du2 = c.uv.x - a.uv.x, dv2 = c.uv.y - a.uv.y;
            const float det = du1 * dv2 - du2 * dv1;
            const Vec3 t = std::fabs(det) > 1e-12f ? (e1 * dv2 - e2 * dv1) * (1.0f / det) : Vec3{};
            for (u16 k : { part.indices[i], part.indices[i + 1], part.indices[i + 2] }) {
                faceNormal[k] = faceNormal[k] + n;
                tangent[k] = tangent[k] + t;
            }
        }
        for (size_t v = 0; v < part.vertices.size(); ++v) {
            EditVertex& vertex = part.vertices[v];
            if (Length(vertex.normal) < 1e-6f && Length(faceNormal[v]) > 1e-12f) vertex.normal = Normalize(faceNormal[v]);
            if (Length(tangent[v]) > 1e-12f) { vertex.hasTangent = true; vertex.tangent = Normalize(tangent[v]); }
        }
        if (!part.indices.empty()) parts.push_back(std::move(part));
    }
    if (parts.empty()) { why = L"the kept mesh has no triangles"; return false; }
    // Runs of one effect share a vertex buffer (0x64D5E0), so materials of one
    // effect go side by side.
    std::stable_sort(parts.begin(), parts.end(), [](const Part& a, const Part& b) { return a.donor->effect < b.donor->effect; });

    std::vector<u32> textures;
    const auto textureSlot = [&](u32 hash) {
        const auto at = std::find(textures.begin(), textures.end(), hash);
        if (at != textures.end()) return (u8)(at - textures.begin());
        textures.push_back(hash);
        return (u8)(textures.size() - 1);
    };
    std::vector<u8> materials;
    std::vector<u16> indices;
    std::vector<std::vector<u8>> buffers;
    std::vector<Node> shaderNames;
    u32 flags = 0, runVertices = 0, runEffect = 0;
    Aabb bounds;
    double area = 0.0;
    for (const Part& part : parts) {
        const Donor& donor = *part.donor;
        if (buffers.empty() || donor.effect != runEffect) {
            buffers.emplace_back();
            runVertices = 0;
            runEffect = donor.effect;
        }
        if (runVertices + part.vertices.size() > 0xFFFF) { why = L"a run of its materials needs more than 65535 vertices"; return false; }
        std::vector<u8>& buffer = buffers.back();
        const size_t at = buffer.size();
        buffer.resize(at + part.vertices.size() * donor.layout.stride);
        Aabb box;
        for (size_t v = 0; v < part.vertices.size(); ++v) {
            Encode(buffer.data() + at + v * donor.layout.stride, donor.vertex.data(), donor.layout, part.vertices[v]);
            box.Add(part.vertices[v].position);
        }
        std::vector<u8> material = donor.material;
        for (size_t s = 0; s < 5; ++s)
            if (material[kMatSlots + s] != 0xFF) material[kMatSlots + s] = textureSlot(donor.slots[s]);
        if (textures.size() > 255) { why = L"it draws with more than 255 textures"; return false; }
        Write<u32>(material.data() + 0x0C, 0);   // the game puts its shader here
        WriteVec3(material.data() + kMatMin, box.min);
        WriteVec3(material.data() + kMatMax, box.max);
        Write<u32>(material.data() + kMatVertices, (u32)part.vertices.size());
        Write<u32>(material.data() + kMatIndices, (u32)part.indices.size());
        Write<u32>(material.data() + kMatTriangles, (u32)(part.indices.size() / 3));
        Write<u32>(material.data() + kMatFirst, (u32)indices.size());
        materials.insert(materials.end(), material.begin(), material.end());
        flags |= Read<u32>(material.data());
        for (size_t i = 0; i + 2 < part.indices.size(); i += 3)
            area += 0.5 * (double)Length(Cross(part.vertices[part.indices[i + 1]].position - part.vertices[part.indices[i]].position,
                                               part.vertices[part.indices[i + 2]].position - part.vertices[part.indices[i]].position));
        for (u16 k : part.indices) indices.push_back((u16)(runVertices + k));
        runVertices += (u32)part.vertices.size();
        bounds.Add(box.min);
        bounds.Add(box.max);
        Node shader;
        shader.id = kChunkShaderName;
        shader.payload = donor.shaderName;
        shaderNames.push_back(std::move(shader));
    }

    const Donor& first = *parts.front().donor;
    const u32 triangles = (u32)(indices.size() / 3);
    Node header;
    header.id = nfs::kChunkSolidObjectHeader;
    header.aligned = true;
    header.payload = HeaderWithName(first.header, name);
    u8* h = header.payload.data();
    Write<u32>(h + kHdrHash, entry.hash);
    Write<u32>(h + kHdrTriangles, triangles);
    h[kHdrCounts + 1] = (u8)textures.size();
    h[kHdrCounts + 2] = 0;   // no 0x134013: the loader walks this many entries of it
    h[kHdrCounts + 3] = 0;   // no 0x13401A
    WriteVec3(h + kHdrMin, bounds.min);
    Write<u32>(h + kHdrMin + 12, 0);
    WriteVec3(h + kHdrMax, bounds.max);
    Write<u32>(h + kHdrMax + 12, 0);
    for (int i = 0; i < 16; ++i) Write<float>(h + kHdrMatrix + (size_t)i * 4, i % 5 == 0 ? 1.0f : 0.0f);
    Write<float>(h + kHdrArea, (float)area);
    Write<float>(h + kHdrDensity, area > 0.0 ? (float)(triangles / area) : 0.0f);

    Node list;
    list.id = nfs::kChunkSolidObjectTexRefs;   // read where it stands: never filler
    for (u32 hash : textures) {
        const size_t at = list.payload.size();
        list.payload.resize(at + 8, 0);
        Write<u32>(list.payload.data() + at, hash);
    }

    Node meshNode;
    meshNode.id = nfs::kChunkMeshContainer;
    Node desc;
    desc.id = nfs::kChunkMeshDescriptor;
    desc.aligned = true;
    desc.payload = first.descriptor;
    Write<u32>(desc.payload.data() + kDescFlags, flags);
    Write<u32>(desc.payload.data() + kDescMaterials, (u32)parts.size());
    Write<u32>(desc.payload.data() + kDescBuffers, (u32)buffers.size());
    Write<u32>(desc.payload.data() + kDescIndices, (u32)indices.size());
    Node mats;
    mats.id = nfs::kChunkMeshMaterials;
    mats.aligned = true;
    mats.payload = std::move(materials);
    Node index;
    index.id = nfs::kChunkMeshIndices;
    index.aligned = true;
    index.payload.assign((const u8*)indices.data(), (const u8*)indices.data() + indices.size() * 2);
    if (indices.size() % 2) index.payload.resize(index.payload.size() + 2, 0);   // to 4 bytes, as shipped
    meshNode.children.push_back(std::move(desc));
    meshNode.children.push_back(std::move(mats));
    meshNode.children.push_back(std::move(index));
    for (std::vector<u8>& buffer : buffers) {
        Node vertices;
        vertices.id = nfs::kChunkMeshVertices;
        vertices.aligned = true;
        vertices.payload = std::move(buffer);
        meshNode.children.push_back(std::move(vertices));
    }
    for (Node& shader : shaderNames) meshNode.children.push_back(std::move(shader));

    out = Node{};
    out.id = nfs::kChunkSolidObject;
    out.children.push_back(std::move(header));
    out.children.push_back(std::move(list));
    out.children.push_back(std::move(meshNode));
    return true;
}

const nfs::World::NewModel* NewModelOf(const nfs::World& world, u32 hash) {
    for (const auto& entry : world.newModels)
        if (entry.hash == hash) return &entry;
    return nullptr;
}

// The object of a model as the game would read it now: a new one built from
// its kept OBJ, a shipped one with its mesh edit applied.
bool SourceNode(const nfs::World& world, u32 hash, Node& out, std::wstring& why) {
    if (const nfs::World::NewModel* entry = NewModelOf(world, hash)) {
        const nfs::SolidObject* model = world.FindModel(hash);
        if (!model) { why = L"the new model " + Hex(hash) + L" is not staged"; return false; }
        return BuildNewNode(world, *entry, model->name, out, why);
    }
    Shipped shipped;
    if (!FindShipped(world, hash, shipped, why)) return false;
    out = std::move(shipped.node);
    for (const auto& replacement : world.meshReplacements) {
        if (replacement.hash != hash) continue;
        MeshEdit edit;
        if (!ReadKeptMesh(replacement.file, shipped.materials, shipped.vertices, edit, why) || !BuildObject(out, edit, why))
            return false;
    }
    return true;
}

// The pack's object count and hash rows, checked against its objects: what a
// model is added to. Null when the pack is not laid out that way.
struct PackIndex {
    Node* count = nullptr;   // 0x134002, count at +0x0C
    Node* rows = nullptr;    // 0x134003, (hash, 0) per object in object order, sorted
};

bool IndexOf(std::vector<Node>& nodes, PackIndex& out) {
    std::vector<u32> hashes;
    for (Node& node : nodes) {
        if (node.id == nfs::kChunkSolidListHeader) {
            out.count = ChildOf(node, nfs::kChunkSolidListPadding);
            out.rows = ChildOf(node, nfs::kChunkSolidListHashes);
        } else if (node.id == nfs::kChunkSolidObject) {
            hashes.push_back(HashOf(node));
        }
    }
    if (!out.count || !out.rows || out.count->payload.size() < 0x10 || out.rows->payload.size() != hashes.size() * 8 ||
        Read<u32>(out.count->payload.data() + 0x0C) != hashes.size())
        return false;
    for (size_t i = 0; i < hashes.size(); ++i)
        if (Read<u32>(out.rows->payload.data() + i * 8) != hashes[i] || (i && hashes[i - 1] >= hashes[i])) return false;
    return true;
}

// Puts `object` into the pack at its place in hash order.
bool AddToPack(std::vector<Node>& nodes, Node object, std::wstring& why) {
    PackIndex index;
    if (!IndexOf(nodes, index)) { why = L"its solid list is not laid out as the editor adds to"; return false; }
    const u32 hash = HashOf(object);
    size_t row = 0;
    auto at = nodes.end();
    for (auto it = nodes.begin(); it != nodes.end(); ++it) {
        if (it->id != nfs::kChunkSolidObject) continue;
        const u32 other = HashOf(*it);
        if (other == hash) { why = L"the section already stores a model of hash " + Hex(hash); return false; }
        if (other > hash) { at = it; break; }
        ++row;
    }
    if (at == nodes.end()) {   // after the last object, before any padding that ends the pack
        at = nodes.begin();
        for (auto it = nodes.begin(); it != nodes.end(); ++it)
            if (it->id == nfs::kChunkSolidObject || it->id == nfs::kChunkSolidListHeader) at = it + 1;
    }
    u8 entry[8] = {};
    Write<u32>(entry, hash);
    index.rows->payload.insert(index.rows->payload.begin() + (std::ptrdiff_t)(row * 8), entry, entry + 8);
    Write<u32>(index.count->payload.data() + 0x0C, Read<u32>(index.count->payload.data() + 0x0C) + 1);
    nodes.insert(at, std::move(object));
    return true;
}

} // namespace

float ModelRadius(const nfs::SolidObject& model) {
    float radius = 0.0f;
    for (const nfs::SolidVertex& vertex : model.vertices) radius = std::max(radius, Length(vertex.position));
    return radius;
}

bool StageWorldMesh(nfs::World& world, u32 hash, const std::wstring& keptPath, std::wstring& error) {
    Shipped shipped;
    if (!FindShipped(world, hash, shipped, error)) return false;
    MeshEdit edit;
    if (!ReadKeptMesh(keptPath, shipped.materials, shipped.vertices, edit, error)) return false;
    Node rebuilt = shipped.node;
    if (!BuildObject(rebuilt, edit, error)) return false;
    std::vector<u8> bytes;
    Emit(bytes, { rebuilt }, shipped.offset);
    nfs::SolidObject fresh;
    if (!ParseModel(bytes, shipped.offset, fresh)) {
        error = L"the rebuilt model does not read back";
        return false;
    }

    // What the viewport draws: every loaded copy of the model, and the boxes
    // of what places it (never smaller than they were: other levels of detail
    // keep their shape).
    const float radius = ModelRadius(fresh);
    for (size_t i = 0; i < world.Loaded().size(); ++i) {
        nfs::LoadedSection* loaded = world.MutableLoaded(i);
        if (!loaded) continue;
        for (nfs::SolidList& list : loaded->solidLists)
            for (nfs::SolidObject& model : list.objects) {
                if (model.hash != hash) continue;
                model.vertices     = fresh.vertices;
                model.indices      = fresh.indices;
                model.materials    = fresh.materials;
                model.bounds       = fresh.bounds;
                model.hasNormals   = fresh.hasNormals;
                model.vertexStride = fresh.vertexStride;
                loaded->placementsChanged = true;
            }
        for (nfs::SceneryInstance& instance : loaded->scenery.instances)
            if (loaded->scenery.ModelHash(instance) == hash && fresh.bounds.Valid()) {
                const Aabb box = Transform(fresh.bounds, instance.transform);
                instance.worldBounds.Add(box.min);
                instance.worldBounds.Add(box.max);
            }
        for (nfs::SceneryInfo& info : loaded->scenery.infos)
            if (info.solidHash == hash) info.radius = std::max(info.radius, radius);
    }

    auto& list = world.meshReplacements;
    list.erase(std::remove_if(list.begin(), list.end(), [&](const auto& r) { return r.hash == hash; }), list.end());
    list.push_back({ hash, keptPath });
    LOG_INFO("mesh edit %08X staged from %S: %zu vertices, %zu triangles (%s)", hash, keptPath.c_str(),
             fresh.vertices.size(), fresh.indices.size() / 3, edit.same ? "same layout" : "rebuilt");
    return true;
}

bool ModelPlaced(const nfs::World& world, u32 hash) {
    for (const auto& loaded : world.Loaded())
        for (const auto& placement : loaded->placements)
            if (placement.instance.modelHash == hash) return true;
    return false;
}

std::vector<u32> NewTexturesInUse(const nfs::World& world) {
    std::vector<u32> used;
    if (world.newTextures.empty()) return used;
    for (const auto& model : world.newModels) {
        if (!ModelPlaced(world, model.hash)) continue;
        std::vector<ObjMesh> meshes;
        std::wstring ignored;
        if (!ReadObjMeshes(model.file, meshes, ignored)) continue;
        for (const ObjMesh& mesh : meshes)
            for (const ObjFaceGroup& group : mesh.groups) {
                unsigned hash = 0;
                if (group.indexCount && std::sscanf(group.material.c_str(), "tex_%8x", &hash) == 1 &&
                    NewTextureOf(world, hash))
                    used.push_back(hash);
            }
    }
    std::sort(used.begin(), used.end());
    used.erase(std::unique(used.begin(), used.end()), used.end());
    return used;
}

bool StageNewWorldModel(nfs::World& world, u32 sectionNumber, u32 hash, const std::wstring& keptPath,
                        std::wstring& error) {
    size_t index = SIZE_MAX;
    if (!SectionNumbered(world, sectionNumber, &index)) {
        error = L"section " + std::to_wstring(sectionNumber) + L" is not loaded";
        return false;
    }
    // The name is the kept OBJ's object name (o line).
    std::vector<ObjMesh> meshes;
    if (!ReadObjMeshes(keptPath, meshes, error) || meshes.empty()) {
        if (error.empty()) error = L"the kept mesh " + keptPath + L" holds no faces";
        return false;
    }
    const nfs::World::NewModel entry{ hash, sectionNumber, keptPath };
    Node node;
    if (!BuildNewNode(world, entry, meshes.front().name, node, error)) return false;
    std::vector<u8> bytes;
    Emit(bytes, { node }, 0);
    nfs::SolidObject model;
    if (!ParseModel(bytes, 0, model) || model.hash != hash || model.vertices.empty()) {
        error = L"the new model does not read back";
        return false;
    }
    const size_t vertices = model.vertices.size(), triangles = model.indices.size() / 3;
    world.AddModel(index, std::move(model));
    auto& list = world.newModels;
    list.erase(std::remove_if(list.begin(), list.end(), [&](const auto& m) { return m.hash == hash; }), list.end());
    list.push_back(entry);
    world.MutableLoaded(index)->placementsChanged = true;
    LOG_INFO("new model %08X staged in section %u from %S: %zu vertices, %zu triangles", hash, sectionNumber,
             keptPath.c_str(), vertices, triangles);
    return true;
}

namespace {

// Every texture the models a section loads draw with, sorted.
std::vector<u32> SectionTextures(const nfs::World& world, const std::vector<u32>& here) {
    std::vector<u32> textures;
    for (u32 h : here)
        if (const nfs::SolidObject* m = world.FindModel(h))
            textures.insert(textures.end(), m->textureHashes.begin(), m->textureHashes.end());
    std::sort(textures.begin(), textures.end());
    textures.erase(std::unique(textures.begin(), textures.end()), textures.end());
    return textures;
}

// Whether a staged texture (copied or made by the editor) goes into a
// pack this section has loaded: one of the textures it is put beside is the
// section's.
bool CopyCovers(const nfs::World& world, u32 texture, const std::vector<u32>& sectionTextures) {
    const nfs::World::NewTexture* copy = NewTextureOf(world, texture);
    if (!copy) return false;
    if (std::binary_search(sectionTextures.begin(), sectionTextures.end(), copy->like)) return true;
    for (const u32 like : copy->moreLikes)
        if (std::binary_search(sectionTextures.begin(), sectionTextures.end(), like)) return true;
    return false;
}

} // namespace

std::vector<u32> StageCopiedTextures(nfs::World& world) {
    std::vector<u32> needed;
    for (size_t i = 0; i < world.Loaded().size(); ++i) {
        const nfs::LoadedSection& loaded = *world.Loaded()[i];
        if (!loaded.info || loaded.placements.empty()) continue;
        const std::vector<u32> here = LoadedWith(loaded);
        std::vector<u32> textures;
        bool read = false;
        u32 host = 0;   // a texture of the section whose pack takes the copies
        for (const auto& placement : loaded.placements) {
            const u32 hash = placement.instance.modelHash;
            if (std::binary_search(here.begin(), here.end(), hash)) continue;
            const nfs::SolidObject* model = world.FindModel(hash);
            if (!model) continue;
            if (!read) {
                textures = SectionTextures(world, here);
                read = true;
                for (const u32 t : textures)
                    if (!NewTextureOf(world, t) && !TextureHosts(world, t).empty()) { host = t; break; }
            }
            for (const u32 t : model->textureHashes) {
                if (std::binary_search(textures.begin(), textures.end(), t)) continue;
                std::wstring why;
                if (!host || !StageCopiedWorldTexture(world, t, host, why)) {
                    LOG_WARN("texture %08X of %s placed in %s not copied: %S", t, model->name.c_str(),
                             loaded.info->name.c_str(), host ? why.c_str() : L"the section draws with no texture of a texture section");
                    continue;
                }
                needed.push_back(t);
            }
        }
    }
    std::sort(needed.begin(), needed.end());
    needed.erase(std::unique(needed.begin(), needed.end()), needed.end());
    return needed;
}

bool SectionModelCopies(const nfs::World& world, const nfs::LoadedSection& loaded,
                        std::vector<WorldModelCopy>& out, std::wstring& error) {
    if (!loaded.info || loaded.placements.empty()) return true;
    const std::vector<u32> here = LoadedWith(loaded);
    std::vector<u32> textures;
    bool texturesRead = false;
    for (const auto& placement : loaded.placements) {
        const u32 hash = placement.instance.modelHash;
        if (std::binary_search(here.begin(), here.end(), hash)) continue;
        if (std::any_of(out.begin(), out.end(), [&](const WorldModelCopy& c) { return c.source == hash; })) continue;
        const nfs::SolidObject* model = world.FindModel(hash);
        if (!model) { error = L"The model " + Hex(hash) + L" placed in " + Widen(loaded.info->name) + L" is not loaded."; return false; }
        if (!texturesRead) {
            textures = SectionTextures(world, here);
            texturesRead = true;
        }
        // Textures the section does not load come with it (StageCopiedTextures).
        std::wstring missing;
        for (u32 t : model->textureHashes)
            if (!std::binary_search(textures.begin(), textures.end(), t) && !CopyCovers(world, t, textures))
                missing += L" " + Hex(t);
        if (!missing.empty()) {
            // ponytail: written as before -- the record names the model where it is
            // stored, which the game shows only if it has that file loaded there.
            LOG_WARN("placement of %s in section %s: stored elsewhere, not among what that section loads, and "
                     "draws with textures no model of it draws with (%S): not copied, the game may not show it",
                     model->name.c_str(), loaded.info->name.c_str(), missing.c_str());
            continue;
        }
        WorldModelCopy copy;
        copy.source = hash;
        copy.name = model->name + "_FRS" + std::to_string(loaded.info->number);
        copy.copy = StringHash(copy.name);
        if (world.FindModel(copy.copy)) {
            error = L"The copy of " + Widen(model->name) + L" for section " + Widen(loaded.info->name) +
                    L" would take hash " + Hex(copy.copy) + L", which a model already has. Nothing was written.";
            return false;
        }
        out.push_back(std::move(copy));
    }
    return true;
}

bool ImportWorldMeshes(nfs::World& world, const std::wstring& tracksFolder, const std::wstring& objPath,
                       WorldMeshImport& out, const Vec3* newOrigin) {
    std::vector<ObjMesh> meshes;
    std::wstring why;
    if (!ReadObjMeshes(objPath, meshes, why)) {
        out.refused = L"  " + why + L"\n";
        return false;
    }
    std::unordered_map<std::string, ManifestObject> manifest;
    Vec3 origin{};
    const bool manifested = ReadExportManifest(objPath, manifest, origin);
    if (!manifested && !newOrigin) {
        out.refused = L"  No manifest beside the OBJ names its objects: keep the .json the editor wrote.\n";
        return false;
    }
    if (!manifested) origin = *newOrigin;
    const std::wstring folder = KeptMeshFolder(tracksFolder);
    std::string manifestEntries;   // new objects, for the next save in Blender to edit

    // A new model kept in its own space: Z up, vt as the game stores it, each
    // group as the game texture it draws with.
    // A material that names no game texture gets a new one (io/WorldTexture.h)
    // from its image -- the MTL's map_Kd, or <obj name>\<material>.png|jpg|tga|bmp
    // beside the OBJ -- or, with none, its Kd colour.
    std::unordered_map<std::string, std::string> materialImages;
    std::unordered_set<std::string> tgaMaterials;
    std::unordered_map<std::string, std::wstring> imageFiles;
    std::unordered_map<std::string, u32> diffuse;
    ReadObjMaterials(objPath, materialImages, &tgaMaterials, &imageFiles, &diffuse);
    std::map<std::string, u32> madeHere;   // material -> new texture, once per import
    const auto exists = [](const std::wstring& path) {
        return !path.empty() && ::GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES;
    };
    const auto newTexture = [&](const std::string& material, const nfs::LoadedSection& section, std::wstring& error) -> u32 {
        if (const auto made = madeHere.find(material); made != madeHere.end()) return made->second;
        std::string wanted = material;
        const size_t dot = wanted.find_last_of('.');
        if (dot != std::string::npos && dot + 1 < wanted.size() &&
            wanted.find_first_not_of("0123456789", dot + 1) == std::string::npos)
            wanted.resize(dot);   // Blender's .001
        std::wstring image;
        if (const auto file = imageFiles.find(material); file != imageFiles.end() && exists(file->second)) {
            image = file->second;
            if (const auto stem = materialImages.find(material); stem != materialImages.end() && !stem->second.empty())
                wanted = stem->second;
        } else {
            const size_t cut = objPath.find_last_of(L'.');
            const std::wstring beside = (cut == std::wstring::npos ? objPath : objPath.substr(0, cut)) + L"\\" + Widen(wanted);
            for (const wchar_t* ext : { L".png", L".jpg", L".jpeg", L".tga", L".bmp" })
                if (exists(beside + ext)) { image = beside + ext; break; }
        }
        const u32 like = PickLikeTexture(world, section, !image.empty() && ImageHasAlpha(image));
        if (!like) {
            error = L"no model of section " + Widen(section.info->name) +
                    L" has a material a new texture can be copied from";
            return 0;
        }
        const std::string name = NewTextureName(world, wanted);
        const u32 hash = StringHash(name);
        wchar_t file[32];
        ::_snwprintf_s(file, _TRUNCATE, L"NEW_%08X", hash);
        const size_t extAt = image.find_last_of(L'.');
        const std::wstring kept = KeptTextureFolder(tracksFolder) + L"\\" + file +
                                  (image.empty() ? L".bmp" : extAt == std::wstring::npos ? L".png" : image.substr(extAt));
        if (image.empty()) {
            const auto colour = diffuse.find(material);
            if (!WriteColourBmp(kept, colour == diffuse.end() ? 0xFFFFFFFFu : colour->second)) {
                error = L"could not write " + kept;
                return 0;
            }
        } else if (!::CopyFileW(image.c_str(), kept.c_str(), FALSE)) {
            error = L"could not keep a copy of " + image;
            return 0;
        }
        if (!StageNewWorldTexture(world, name, like, kept, error)) return 0;
        LOG_INFO("import: material '%s' -> new texture %s %08X from %S", material.c_str(), name.c_str(), hash,
                 image.empty() ? L"its colour" : image.c_str());
        madeHere[material] = hash;
        return hash;
    };

    const auto keepNew = [&](const ObjMesh& mesh, const nfs::LoadedSection& section, const std::string& name, u32 hash,
                             const Mat4& toModel, std::wstring& kept, std::wstring& error) {
        std::vector<u32> textures(mesh.groups.size(), 0);
        for (size_t g = 0; g < mesh.groups.size(); ++g)
            if (mesh.groups[g].indexCount && !(textures[g] = MaterialTexture(world, mesh.groups[g].material)) &&
                !(textures[g] = newTexture(mesh.groups[g].material, section, error))) {
                error = L"material " + Widen(mesh.groups[g].material) + L": " + error;
                return false;
            }
        wchar_t file[32];
        ::_snwprintf_s(file, _TRUNCATE, L"NEW_%08X.obj", hash);
        kept = folder + L"\\" + file;
        FILE* f = nullptr;
        if (::_wfopen_s(&f, kept.c_str(), L"w") != 0 || !f) { error = L"could not write " + kept; return false; }
        std::fprintf(f, "# FRS World Editor: NFS World new model %s (%08X)\n", name.c_str(), hash);
        std::fprintf(f, "# model space, game axes (Z up); vt as the game stores it (v down); usemtl tex_<hash> = game texture\n");
        std::fprintf(f, "o %s\n", name.c_str());
        for (size_t k = 0; k < mesh.positions.size(); ++k) {
            const Vec3 p = toModel.TransformPoint(FromBlender(mesh.positions[k]) + origin);
            const u32 c = mesh.colors[k];
            std::fprintf(f, "v %.9g %.9g %.9g %.6f %.6f %.6f\n", p.x, p.y, p.z, ((c >> 16) & 0xFF) / 255.0f,
                         ((c >> 8) & 0xFF) / 255.0f, (c & 0xFF) / 255.0f);
        }
        for (size_t k = 0; k < mesh.normals.size(); ++k) {
            const Vec3 n = toModel.TransformDirection(FromBlender(mesh.normals[k]));
            std::fprintf(f, "vn %.6f %.6f %.6f\n", n.x, n.y, n.z);
        }
        for (const Vec2& uv : mesh.uvs) std::fprintf(f, "vt %.7g %.7g\n", uv.x, 1.0f - uv.y);
        for (size_t g = 0; g < mesh.groups.size(); ++g) {
            const ObjFaceGroup& group = mesh.groups[g];
            if (!group.indexCount) continue;
            std::fprintf(f, "usemtl tex_%08X\n", textures[g]);
            for (u32 i = group.firstIndex; i + 2 < group.firstIndex + group.indexCount; i += 3) {
                const size_t a = mesh.indices[i] + 1u, b = mesh.indices[i + 1] + 1u, c = mesh.indices[i + 2] + 1u;
                std::fprintf(f, "f %zu/%zu/%zu %zu/%zu/%zu %zu/%zu/%zu\n", a, a, a, b, b, b, c, c, c);
            }
        }
        std::fclose(f);
        return true;
    };

    for (const ObjMesh& corners : meshes) {
        const ObjMesh mesh = MergeCorners(corners);
        ++out.objectsRead;
        const std::wstring name = Widen(mesh.name);
        auto entry = manifest.find(mesh.name);
        if (entry == manifest.end() && mesh.name.find_last_of('.') != std::string::npos)
            entry = manifest.find(mesh.name.substr(0, mesh.name.find_last_of('.')));

        if (entry == manifest.end() || NewModelOf(world, entry->second.hash)) {
            // --- a new model, or a new model saved again ---------------------------------
            std::wstring kept;
            if (entry != manifest.end()) {
                const nfs::World::NewModel known = *NewModelOf(world, entry->second.hash);
                const nfs::SolidObject* model = world.FindModel(known.hash);
                const nfs::LoadedSection* home = SectionNumbered(world, known.section);
                if (!model || !home ||
                    !keepNew(mesh, *home, model->name, known.hash, InverseAffine(entry->second.transform), kept, why) ||
                    !StageNewWorldModel(world, known.section, known.hash, kept, why)) {
                    out.refused += L"  " + name + L": " + why + L"\n";
                    continue;
                }
                size_t index = SIZE_MAX;
                SectionNumbered(world, known.section, &index);
                ++out.staged;
                out.hashes.push_back(known.hash);
                out.sections.push_back(index);
                out.done += L"  " + name + L": new model saved again, " + std::to_wstring(mesh.TriangleCount()) +
                            L" triangles\n";
                continue;
            }
            if (mesh.indices.size() < 3) { out.refused += L"  " + name + L": it has no faces\n"; continue; }

            // Pivot in the middle of its footprint, on its lowest point, as the
            // original editor makes new objects.
            Aabb box;
            std::vector<Vec3> along;
            const size_t step = std::max<size_t>(1, mesh.positions.size() / 48);
            for (size_t k = 0; k < mesh.positions.size(); ++k) {
                const Vec3 p = FromBlender(mesh.positions[k]) + origin;
                box.Add(p);
                if (k % step == 0) along.push_back(p);
            }
            const Vec3 pivot{ (box.min.x + box.max.x) * 0.5f, (box.min.y + box.max.y) * 0.5f, box.min.z };
            size_t target = world.SectionForPlacementAlong(along);
            if (target == SIZE_MAX) target = world.SectionForPlacement(pivot);
            nfs::LoadedSection* section = world.MutableLoaded(target);
            if (!section || !section->info) { out.refused += L"  " + name + L": no section where it stands can take scenery\n"; continue; }

            std::string model = "FRS_";
            for (char c : mesh.name) {
                const bool plain = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9');
                model.push_back(plain ? (char)::toupper((unsigned char)c) : '_');
            }
            const std::string base = model;
            for (int n = 2; world.FindModel(StringHash(model)) || NewModelOf(world, StringHash(model)); ++n)
                model = base + "_" + std::to_string(n);
            const u32 hash = StringHash(model);
            const Mat4 placement = Mat4::Translation(pivot);
            if (!keepNew(mesh, *section, model, hash, InverseAffine(placement), kept, why) ||
                !StageNewWorldModel(world, section->info->number, hash, kept, why)) {
                out.refused += L"  " + name + L": " + why + L"\n";
                continue;
            }
            const nfs::SolidObject* made = world.FindModel(hash);
            nfs::PlacedInstance placed;
            placed.modelHash = hash;
            placed.modelName = model;
            placed.modelBounds = made->bounds;
            placed.triangles = made->TriangleCount();
            placed.radius = ModelRadius(*made);
            placed.transform = placement;
            section->placements.push_back({ section->info->number, placed });
            section->placementsChanged = true;

            char text[1024];
            const Mat4& m = placement;
            ::_snprintf_s(text, _TRUNCATE,
                          "    {\n      \"name\": \"%s\",\n      \"model\": \"%s\",\n      \"hash\": \"%08X\",\n"
                          "      \"section\": \"%s\",\n      \"placedBy\": \"instance\",\n"
                          "      \"vertices\": %zu,\n      \"triangles\": %zu,\n"   // ReadManifest wants vertices
                          "      \"transform\": [%.6f, %.6f, %.6f, %.6f, %.6f, %.6f, %.6f, %.6f, %.6f, %.6f, %.6f, %.6f]\n    }",
                          mesh.name.c_str(), model.c_str(), hash, section->info->name.c_str(), mesh.positions.size(),
                          mesh.TriangleCount(), m.m[0][0], m.m[0][1],
                          m.m[0][2], m.m[1][0], m.m[1][1], m.m[1][2], m.m[2][0], m.m[2][1], m.m[2][2], m.m[3][0],
                          m.m[3][1], m.m[3][2]);
            manifestEntries += (manifestEntries.empty() ? "" : ",\n") + std::string(text);

            ++out.staged;
            ++out.created;
            out.hashes.push_back(hash);
            out.sections.push_back(target);
            out.done += L"  " + name + L": new model " + Widen(model) + L" in section " + Widen(section->info->name) +
                        L", " + std::to_wstring(mesh.positions.size()) + L" vertices, " +
                        std::to_wstring(mesh.TriangleCount()) + L" triangles, placed where it stands\n";
            continue;
        }
        const u32 hash = entry->second.hash;
        Shipped shipped;
        if (!FindShipped(world, hash, shipped, why)) {
            out.refused += L"  " + name + L": " + why + L"\n";
            continue;
        }
        const nfs::SolidObject& model = shipped.model;
        if (model.materials.size() != shipped.materials || model.vertices.size() != shipped.vertices) {
            // A run the editor cannot read (effects 8 and 9) left out of the model.
            Node probe = shipped.node;
            MeshEdit nothing;
            nothing.same = true;
            BuildObject(probe, nothing, why);
            out.refused += L"  " + name + L": " + why + L"\n";
            continue;
        }

        std::vector<int> groupMaterial(mesh.groups.size(), -1);
        std::wstring unknown;
        for (size_t g = 0; g < mesh.groups.size(); ++g)
            if ((groupMaterial[g] = MaterialFor(mesh.groups[g].material, model)) < 0) unknown = Widen(mesh.groups[g].material);
        if (!unknown.empty()) {
            out.refused += L"  " + name + L": material " + unknown + L" is not one of the model's; a new "
                           L"material or texture is not written yet\n";
            continue;
        }

        const Mat4 toModel = InverseAffine(entry->second.transform);
        const bool same = SameTopology(mesh, groupMaterial, model);
        const std::vector<u32>& source = mesh.sourcePositions;

        // Kept in the model's own space, so the list alone rebuilds it.
        wchar_t file[32];
        ::_snwprintf_s(file, _TRUNCATE, L"%08X.obj", hash);
        const std::wstring kept = folder + L"\\" + file;
        FILE* f = nullptr;
        if (::_wfopen_s(&f, kept.c_str(), L"w") != 0 || !f) {
            out.refused += L"  " + name + L": could not write " + kept + L"\n";
            continue;
        }
        std::fprintf(f, "# FRS World Editor: NFS World mesh %s (%08X)\n", model.name.c_str(), hash);
        std::fprintf(f, "# model space, game axes (Z up); vt as the game stores it (v down); usemtl m<n> = material n\n");
        std::fprintf(f, "# frs-topology %s\n", same ? "same" : "rebuilt");
        std::fprintf(f, "o %s\n", model.name.c_str());
        std::vector<size_t> order(mesh.positions.size());
        for (size_t k = 0; k < order.size(); ++k) order[same ? source[k] : k] = k;
        for (size_t k : order) {
            const Vec3 p = toModel.TransformPoint(FromBlender(mesh.positions[k]) + origin);
            const u32 c = mesh.colors[k];
            std::fprintf(f, "v %.9g %.9g %.9g %.6f %.6f %.6f\n", p.x, p.y, p.z, ((c >> 16) & 0xFF) / 255.0f,
                         ((c >> 8) & 0xFF) / 255.0f, (c & 0xFF) / 255.0f);
        }
        for (size_t k : order) {
            Vec3 n = toModel.TransformDirection(FromBlender(mesh.normals[k]));
            n = Length(n) > 1e-6f ? Normalize(n) : Vec3{ 0.0f, 0.0f, 1.0f };
            std::fprintf(f, "vn %.6f %.6f %.6f\n", n.x, n.y, n.z);
        }
        for (size_t k : order) std::fprintf(f, "vt %.7g %.7g\n", mesh.uvs[k].x, 1.0f - mesh.uvs[k].y);
        const auto line = [&](u16 k) { return (size_t)(same ? source[k] : k) + 1; };
        for (size_t g = 0; g < mesh.groups.size(); ++g) {
            const ObjFaceGroup& group = mesh.groups[g];
            std::fprintf(f, "usemtl m%d\n", groupMaterial[g]);
            for (u32 i = group.firstIndex; i + 2 < group.firstIndex + group.indexCount; i += 3) {
                const size_t a = line(mesh.indices[i]), b = line(mesh.indices[i + 1]), c = line(mesh.indices[i + 2]);
                std::fprintf(f, "f %zu/%zu/%zu %zu/%zu/%zu %zu/%zu/%zu\n", a, a, a, b, b, b, c, c, c);
            }
        }
        std::fclose(f);

        if (!StageWorldMesh(world, hash, kept, why)) {
            out.refused += L"  " + name + L": " + why + L"\n";
            continue;
        }
        ++out.staged;
        out.hashes.push_back(hash);
        out.done += L"  " + name + L": " + std::to_wstring(mesh.positions.size()) + L" vertices, " +
                    std::to_wstring(mesh.TriangleCount()) + L" triangles (shipped " +
                    std::to_wstring(model.vertices.size()) + L", " + std::to_wstring(model.indices.size() / 3) +
                    (same ? L"), same layout, written over itself\n" : L"), rebuilt\n");
    }

    // The new objects join the manifest (the one beside the OBJ, as a Blender
    // session has), so its next save edits them instead of making them again.
    if (manifested && !manifestEntries.empty()) {
        const std::wstring path = objPath.substr(0, objPath.find_last_of(L'.')) + L".json";
        std::string text;
        {
            std::ifstream in(path, std::ios::binary);
            if (in) text.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
        }
        const size_t list = text.find("\"objects\"");
        const size_t open = list == std::string::npos ? std::string::npos : text.find('[', list);
        const size_t close = text.rfind(']');
        if (open != std::string::npos && close != std::string::npos && close > open) {
            const bool hasEntries = text.find('{', open) < close;
            size_t end = close;
            while (end > open && (text[end - 1] == ' ' || text[end - 1] == '\n' || text[end - 1] == '\r')) --end;
            text = text.substr(0, end) + (hasEntries ? ",\n" : "\n") + manifestEntries + "\n  " + text.substr(close);
            FILE* file = nullptr;
            if (::_wfopen_s(&file, path.c_str(), L"wb") == 0 && file) {
                std::fwrite(text.data(), 1, text.size(), file);
                std::fclose(file);
            } else {
                LOG_WARN("new models: could not record them in %S", path.c_str());
            }
        }
    }
    return out.staged > 0;
}

bool SectionHasMeshEdits(const nfs::World& world, const nfs::LoadedSection& loaded) {
    if (loaded.info)
        for (const auto& entry : world.newModels)
            if (entry.section == loaded.info->number && ModelPlaced(world, entry.hash)) return true;
    if (!loaded.placements.empty()) {
        const std::vector<u32> here = LoadedWith(loaded);
        for (const auto& placement : loaded.placements)
            if (!std::binary_search(here.begin(), here.end(), placement.instance.modelHash)) return true;
    }
    for (const auto& replacement : world.meshReplacements) {
        if (loaded.HoldsModel(replacement.hash)) return true;
        for (const nfs::SceneryInfo& info : loaded.scenery.infos)
            for (size_t level = 0; level < 4 && info.raw.size() >= 0x28; ++level)
                if (Read<u32>(info.raw.data() + 0x18 + level * 4) == replacement.hash) return true;
    }
    return false;
}

bool WriteWorldMeshes(const nfs::World& world, const nfs::LoadedSection& loaded, std::vector<u8>& bytes,
                      size_t& written, std::wstring& error) {
    std::unordered_map<u32, std::wstring> kept;
    for (const auto& replacement : world.meshReplacements)
        if (loaded.HoldsModel(replacement.hash)) kept[replacement.hash] = replacement.file;

    // New models of this section that something places, and the models its
    // placements need copied in (WorldMesh.h): objects to add to its file.
    std::vector<Node> added;
    if (loaded.info) {
        for (const auto& entry : world.newModels) {
            if (entry.section != loaded.info->number || !ModelPlaced(world, entry.hash)) continue;
            Node node;
            std::wstring why;
            if (!SourceNode(world, entry.hash, node, why)) {
                error = L"The new model " + Hex(entry.hash) + L" could not be built: " + why;
                return false;
            }
            added.push_back(std::move(node));
        }
        std::vector<WorldModelCopy> copies;
        if (!SectionModelCopies(world, loaded, copies, error)) return false;
        for (const WorldModelCopy& copy : copies) {
            Node node;
            std::wstring why;
            if (!SourceNode(world, copy.source, node, why)) {
                error = L"The model " + Hex(copy.source) + L" could not be copied into " + Widen(loaded.info->name) + L": " + why;
                return false;
            }
            Node* header = ChildOf(node, nfs::kChunkSolidObjectHeader);
            header->payload = HeaderWithName(header->payload, copy.name);
            added.push_back(std::move(node));
        }
    }
    if (kept.empty() && added.empty()) return true;

    // They go into the last solid list laid out the way one is added to.
    size_t addTo = SIZE_MAX;
    if (!added.empty()) {
        for (size_t at = 0; at + 8 <= bytes.size();) {
            const u32 id = Read<u32>(bytes.data() + at), size = Read<u32>(bytes.data() + at + 4);
            if (at + 8 + size > bytes.size()) break;
            if (id == nfs::kChunkSolidListPack) {
                std::vector<Node> nodes = ParseNodes(ByteSpan{ bytes.data() + at + 8, size }, at + 8);
                PackIndex index;
                if (IndexOf(nodes, index)) addTo = at;
            }
            at += 8 + size;
        }
        if (addTo == SIZE_MAX) {
            error = L"Section " + Widen(loaded.info->name) + L" has no solid list the editor can add a model to.";
            return false;
        }
    }

    size_t position = 0;
    while (position + 8 <= bytes.size()) {
        const u32 id = Read<u32>(bytes.data() + position), size = Read<u32>(bytes.data() + position + 4);
        if (position + 8 + size > bytes.size()) break;
        if (id != nfs::kChunkSolidListPack) { position += 8 + size; continue; }

        std::vector<Node> nodes = ParseNodes(ByteSpan{ bytes.data() + position + 8, size }, position + 8);
        bool wanted = position == addTo;
        for (Node& node : nodes)
            if (node.id == nfs::kChunkSolidObject && kept.count(HashOf(node))) wanted = true;
        if (!wanted) { position += 8 + size; continue; }

        std::vector<u8> payload;
        Emit(payload, nodes, position + 8);
        if (payload.size() != size || std::memcmp(payload.data(), bytes.data() + position + 8, size) != 0) {
            error = L"A solid list in " + loaded.info->fileName + L" is laid out in a way the editor does not write yet.";
            return false;
        }
        for (Node& node : nodes) {
            if (node.id != nfs::kChunkSolidObject) continue;
            const auto found = kept.find(HashOf(node));
            if (found == kept.end()) continue;
            size_t materials = 0, vertices = 0;
            MeshEdit edit;
            std::wstring why;
            if (!CountsOf(node, materials, vertices) || !ReadKeptMesh(found->second, materials, vertices, edit, why) ||
                !BuildObject(node, edit, why)) {
                wchar_t hash[16];
                ::_snwprintf_s(hash, _TRUNCATE, L"%08X", found->first);
                error = L"The edited mesh " + std::wstring(hash) + L" could not be written: " + why;
                return false;
            }
            ++written;
        }
        if (position == addTo) {
            for (Node& node : added) {
                std::wstring why;
                if (!AddToPack(nodes, std::move(node), why)) {
                    error = L"A model could not be added to " + loaded.info->fileName + L": " + why;
                    return false;
                }
                ++written;
            }
            added.clear();
        }
        payload.clear();
        Emit(payload, nodes, position + 8);

        // The pack in its new size, and the padding after it taking up the
        // change: what follows stays where it was modulo 128.
        const std::ptrdiff_t delta = (std::ptrdiff_t)payload.size() - (std::ptrdiff_t)size;
        bytes.erase(bytes.begin() + (std::ptrdiff_t)(position + 8), bytes.begin() + (std::ptrdiff_t)(position + 8 + size));
        bytes.insert(bytes.begin() + (std::ptrdiff_t)(position + 8), payload.begin(), payload.end());
        Write<u32>(bytes.data() + position + 4, (u32)payload.size());
        const size_t next = position + 8 + payload.size();
        if (delta != 0) {
            if (next + 8 <= bytes.size() && Read<u32>(bytes.data() + next) == 0) {
                const std::ptrdiff_t pad = Read<u32>(bytes.data() + next + 4);
                std::ptrdiff_t want = pad - delta;
                while (want < 0) want += 128;
                const auto at = bytes.begin() + (std::ptrdiff_t)(next + 8);
                if (want > pad) bytes.insert(at + pad, (size_t)(want - pad), (u8)0);
                else bytes.erase(at + want, at + pad);
                Write<u32>(bytes.data() + next + 4, (u32)want);
            } else if (delta % 128 != 0) {
                const std::ptrdiff_t want = ((-delta - 8) % 128 + 128) % 128;
                std::vector<u8> filler(8 + (size_t)want, 0);
                Write<u32>(filler.data() + 4, (u32)want);
                bytes.insert(bytes.begin() + (std::ptrdiff_t)next, filler.begin(), filler.end());
            }
        }
        position = next;
    }
    return true;
}

} // namespace io
