#include "io/U1Convert.h"

#include <cmath>
#include <cstring>
#include <limits>

namespace {

constexpr u32 kObject       = 0x80134010;
constexpr u32 kHeader       = 0x00134011;
constexpr u32 kMesh         = 0x80134100;
constexpr u32 kMeshHeader   = 0x00134900;
constexpr u32 kVertices     = 0x00134B01;
constexpr u32 kMaterials    = 0x00134B02;
constexpr u32 kIndices      = 0x00134B03;

constexpr size_t kHeaderU1 = 176, kHeaderU2 = 192;
constexpr size_t kHeaderNameU1 = 0x94;
constexpr size_t kMeshHeaderU1 = 64, kMeshHeaderU2 = 68;
constexpr u32 kHeaderConstantU2 = 0x000EE580;   // +0x88 and +0x8C of every Underground 2 header
constexpr u32 kSkinnedStride = 60, kPlainStride = 24;

u32 AlignOf(u32 id) {
    switch (id) {
        case kHeader: case kMeshHeader: case kMaterials: case kIndices: return 16;
        case kVertices: return 0x80;
        default: return 0;
    }
}

u32 Get32(const u8* p) { u32 v; std::memcpy(&v, p, 4); return v; }
float GetF(const u8* p) { float v; std::memcpy(&v, p, 4); return v; }
void Set32(std::vector<u8>& b, size_t at, u32 v) { std::memcpy(b.data() + at, &v, 4); }
void SetF(std::vector<u8>& b, size_t at, float v) { std::memcpy(b.data() + at, &v, 4); }

struct Node {
    u32 id = 0;
    u64 payloadOffset = 0;      // in the source file
    std::vector<u8> data;       // a leaf's payload, alignment filler taken off
    std::vector<Node> kids;
};

// The payload of an aligned leaf starts after a run of 0x11 bytes that brings
// it to its boundary. Only as much of the run as does that is filler: a
// payload may itself begin with 0x11.
size_t FillerOf(u32 id, const u8* data, u32 size, u64 payloadOffset) {
    const u32 align = AlignOf(id);
    if (!align) return 0;
    size_t run = 0;
    while (run < size && run < align && data[run] == 0x11) ++run;
    for (size_t p = 0; p <= run; ++p)
        if ((payloadOffset + p) % align == 0) return p;
    return 0;
}

bool Parse(const u8* data, size_t size, u64 offset, std::vector<Node>& out, std::wstring& error) {
    for (size_t at = 0; at + 8 <= size;) {
        const u32 id = Get32(data + at), length = Get32(data + at + 4);
        if (at + 8 + (size_t)length > size) { error = L"A chunk of the model runs past its end."; return false; }
        const u8* payload = data + at + 8;
        const u64 payloadOffset = offset + at + 8;
        if (id != 0) {
            Node node;
            node.id = id;
            node.payloadOffset = payloadOffset;
            if (id & 0x80000000u) {
                if (!Parse(payload, length, payloadOffset, node.kids, error)) return false;
            } else {
                const size_t filler = FillerOf(id, payload, length, payloadOffset);
                node.data.assign(payload + filler, payload + length);
            }
            out.push_back(std::move(node));
        }
        at += 8 + (size_t)length;
    }
    return true;
}

class Writer {
public:
    explicit Writer(std::vector<u8>& out) : b_(out) {}
    size_t Begin(u32 id) {
        const size_t at = b_.size();
        b_.resize(at + 8);
        Set32(b_, at, id);
        return at;
    }
    void End(size_t at) { Set32(b_, at + 4, (u32)(b_.size() - at - 8)); }
    void Leaf(u32 id, const std::vector<u8>& data) {
        const size_t at = Begin(id);
        const u32 align = AlignOf(id);
        if (align) b_.insert(b_.end(), (align - b_.size() % align) % align, 0x11);
        b_.insert(b_.end(), data.begin(), data.end());
        End(at);
    }
    void Write(const Node& node) {
        if (node.id & 0x80000000u) {
            const size_t at = Begin(node.id);
            for (const Node& kid : node.kids) Write(kid);
            End(at);
        } else {
            Leaf(node.id, node.data);
        }
    }
private:
    std::vector<u8>& b_;
};

Node* Find(std::vector<Node>& nodes, u32 id) {
    for (Node& n : nodes)
        if (n.id == id) return &n;
    return nullptr;
}

// A mesh in Underground 2's layouts. True when it keeps mesh flags.
bool ConvertMesh(Node& mesh, std::wstring& error) {
    Node* header = Find(mesh.kids, kMeshHeader);
    if (!header || header->data.size() < kMeshHeaderU1) { error = L"A mesh has no header."; return false; }
    std::vector<u8>& h = header->data;
    if (h.size() == kMeshHeaderU1) h.resize(kMeshHeaderU2, 0);
    u32 flags = Get32(h.data() + 0x0C);
    const u32 vertexCount = Get32(h.data() + 0x34);

    Node* vertices = Find(mesh.kids, kVertices);
    if (vertices && vertexCount && vertices->data.size() >= (size_t)vertexCount * kSkinnedStride &&
        vertices->data.size() / vertexCount == kSkinnedStride) {
        // The crowd: position, normal, colour, uv, then bone weights and
        // indices. Underground 2's world draws {position, colour, uv}.
        std::vector<u8> plain((size_t)vertexCount * kPlainStride);
        for (u32 v = 0; v < vertexCount; ++v) {
            const u8* s = vertices->data.data() + (size_t)v * kSkinnedStride;
            u8* d = plain.data() + (size_t)v * kPlainStride;
            std::memcpy(d, s, 12);            // position
            std::memcpy(d + 12, s + 24, 12);  // colour, uv
        }
        vertices->data = std::move(plain);
        flags = 0;
    }
    // Flags Underground 2's world does not use (0x8000 on one mesh of L1RA).
    if (flags & ~0x1040u) flags = 0;
    Set32(h, 0x0C, flags);
    return flags != 0;
}

void ConvertHeader(std::vector<u8>& h, bool meshFlags) {
    if (h.size() != kHeaderU1) return;
    std::vector<u8> out(kHeaderU2, 0);
    std::memcpy(out.data(), h.data(), kHeaderNameU1);
    std::memcpy(out.data() + kHeaderNameU1 + 16, h.data() + kHeaderNameU1, kHeaderU1 - kHeaderNameU1);
    out[0x0C] = 0x16;
    // Header flags: 0x40 only with a mesh that keeps flags.
    u16 objectFlags;
    std::memcpy(&objectFlags, out.data() + 0x0E, 2);
    if (!meshFlags) objectFlags = (u16)(objectFlags & ~0x40);
    std::memcpy(out.data() + 0x0E, &objectFlags, 2);
    Set32(out, 0x88, kHeaderConstantU2);
    Set32(out, 0x8C, kHeaderConstantU2);
    // The box's volume and the triangles per unit of it, as Underground 2's
    // files have them -- with the empty-box sentinel too (-inf, -0).
    const float dx = GetF(h.data() + 0x30) - GetF(h.data() + 0x20);
    const float dy = GetF(h.data() + 0x34) - GetF(h.data() + 0x24);
    const float dz = GetF(h.data() + 0x38) - GetF(h.data() + 0x28);
    const float volume = dx * dy * dz;
    SetF(out, kHeaderNameU1, volume);
    SetF(out, kHeaderNameU1 + 4, (float)Get32(h.data() + 0x14) / volume);
    h = std::move(out);
}

} // namespace

bool ConvertU1Object(ByteSpan object, u64 fileOffset, std::vector<u8>& out, std::wstring& error) {
    out.clear();
    if (object.size < 8 || Get32(object.data) != kObject) { error = L"That is not a model chunk."; return false; }
    std::vector<Node> top;
    if (!Parse(object.data, object.size, fileOffset, top, error)) return false;
    if (top.size() != 1) { error = L"That is not one model chunk."; return false; }
    Node& root = top[0];

    bool meshFlags = false;
    for (Node& kid : root.kids)
        if (kid.id == kMesh && ConvertMesh(kid, error)) meshFlags = true;
    if (!error.empty()) return false;

    Node* header = Find(root.kids, kHeader);
    if (!header || header->data.size() != kHeaderU1 || (header->data[0x0C] != 0x13)) {
        error = L"The model's header is not Need for Speed Underground's.";
        return false;
    }
    ConvertHeader(header->data, meshFlags);

    Writer writer(out);
    writer.Write(root);
    return true;
}
