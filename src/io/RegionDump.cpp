#include "io/RegionDump.h"

#include "core/Log.h"
#include "nfs/TextureDecode.h"
#include "nfs/World.h"

#include <windows.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <set>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace io {

namespace {

constexpr size_t kBuffer = 1 << 20;

// Creates `path` and every missing folder above it.
void CreatePath(const std::wstring& path) {
    for (size_t at = 0; at != std::wstring::npos;) {
        at = path.find_first_of(L"\\/", at + 1);
        const std::wstring part = path.substr(0, at);
        if (!part.empty() && part.back() != L':') ::CreateDirectoryW(part.c_str(), nullptr);
    }
}

FILE* OpenText(const std::wstring& path, std::vector<char>& buffer) {
    FILE* file = nullptr;
    if (::_wfopen_s(&file, path.c_str(), L"wb") != 0 || !file) return nullptr;
    buffer.resize(kBuffer);
    std::setvbuf(file, buffer.data(), _IOFBF, buffer.size());
    return file;
}

// A name that stays one token on a line.
std::string Token(const std::string& name) {
    std::string out = name.empty() ? std::string("?") : name;
    for (char& c : out)
        if ((unsigned char)c <= ' ') c = '_';
    return out;
}

// Area-weighted vertex normals from the triangles, for layouts without any.
std::vector<Vec3> BuildNormals(const nfs::SolidObject& model) {
    std::vector<Vec3> normals(model.vertices.size(), Vec3{});
    for (size_t k = 0; k + 2 < model.indices.size(); k += 3) {
        const u16 ia = model.indices[k], ib = model.indices[k + 1], ic = model.indices[k + 2];
        if (ia >= model.vertices.size() || ib >= model.vertices.size() || ic >= model.vertices.size())
            continue;
        const Vec3& a = model.vertices[ia].position;
        const Vec3 face = Cross(model.vertices[ib].position - a, model.vertices[ic].position - a);
        normals[ia] = normals[ia] + face;
        normals[ib] = normals[ib] + face;
        normals[ic] = normals[ic] + face;
    }
    for (Vec3& n : normals) n = Length(n) > 1e-6f ? Normalize(n) : Vec3{ 0.0f, 0.0f, 1.0f };
    return normals;
}

u32 DiffuseHash(const nfs::SolidObject& model, const nfs::SolidMaterial& material) {
    return material.textureIndex < model.textureHashes.size() ? model.textureHashes[material.textureIndex] : 0;
}

// Writes models\<HASH8>.obj; returns the triangles written.
size_t WriteModel(const nfs::SolidObject& model, const std::wstring& path, std::vector<char>& buffer) {
    FILE* obj = OpenText(path, buffer);
    if (!obj) return 0;
    std::fprintf(obj, "# name %s\no %s\n", model.name.c_str(), Token(model.name).c_str());
    for (const nfs::SolidVertex& v : model.vertices)
        std::fprintf(obj, "v %.6f %.6f %.6f %u %u %u %u\n", v.position.x, v.position.y, v.position.z,
                     (v.color >> 16) & 0xFF, (v.color >> 8) & 0xFF, v.color & 0xFF, v.color >> 24);
    for (const nfs::SolidVertex& v : model.vertices) std::fprintf(obj, "vt %.6f %.6f\n", v.uv.x, v.uv.y);
    const std::vector<Vec3> rebuilt = model.hasNormals ? std::vector<Vec3>{} : BuildNormals(model);
    for (size_t i = 0; i < model.vertices.size(); ++i) {
        const Vec3& n = model.hasNormals ? model.vertices[i].normal : rebuilt[i];
        std::fprintf(obj, "vn %.5f %.5f %.5f\n", n.x, n.y, n.z);
    }
    size_t triangles = 0;
    for (const nfs::SolidMaterial& material : model.materials) {
        if (material.indexCount < 3) continue;   // the renderer draws nothing for it either
        const u32 texture = DiffuseHash(model, material);
        if (texture) std::fprintf(obj, "usemtl tex_%08X\n", texture);
        else         std::fprintf(obj, "usemtl untextured\n");
        const size_t last = (size_t)material.firstIndex + material.indexCount;
        for (size_t k = material.firstIndex; k + 2 < last && k + 2 < model.indices.size(); k += 3) {
            const unsigned a = model.indices[k] + 1u, b = model.indices[k + 1] + 1u, c = model.indices[k + 2] + 1u;
            std::fprintf(obj, "f %u/%u/%u %u/%u/%u %u/%u/%u\n", a, a, a, b, b, b, c, c, c);
            ++triangles;
        }
    }
    std::fclose(obj);
    return triangles;
}

// Uncompressed 32-bit TGA, bottom-up rows, eight alpha bits.
bool WriteTga(const std::wstring& path, const std::vector<u32>& bgra, u32 width, u32 height) {
    if (bgra.size() < (size_t)width * height) return false;
    FILE* file = nullptr;
    if (::_wfopen_s(&file, path.c_str(), L"wb") != 0 || !file) return false;
    u8 header[18] = {};
    header[2]  = 2;
    header[12] = (u8)(width & 0xFF);
    header[13] = (u8)(width >> 8);
    header[14] = (u8)(height & 0xFF);
    header[15] = (u8)(height >> 8);
    header[16] = 32;
    header[17] = 8;
    std::fwrite(header, sizeof(header), 1, file);
    for (u32 y = 0; y < height; ++y)
        std::fwrite(bgra.data() + (size_t)(height - 1 - y) * width, 4, width, file);
    std::fclose(file);
    return true;
}

struct Placement {
    u32 section = 0;
    const nfs::SolidObject* model = nullptr;
    Mat4 transform;
};

} // namespace

bool DumpRegion(const nfs::World& world, const std::wstring& folder) {
    const DWORD start = ::GetTickCount();
    CreatePath(folder + L"\\models");
    CreatePath(folder + L"\\textures");
    const auto& loaded = world.Loaded();

    // What the renderer can draw: the first non-duplicate copy of each mesh, in
    // section order (WorldRenderer::BuildInstances' modelIndex over GpuSection).
    std::unordered_map<u32, const nfs::SolidObject*> modelIndex;
    for (const auto& section : loaded) {
        size_t vertices = 0, indices = 0;
        for (const nfs::SolidList& list : section->solidLists)
            for (const nfs::SolidObject& object : list.objects)
                if (!object.duplicatePlacement) { vertices += object.vertices.size(); indices += object.indices.size(); }
        if (!vertices || !indices) continue;   // GpuSection::Upload uploads nothing
        for (const nfs::SolidList& list : section->solidLists)
            for (const nfs::SolidObject& object : list.objects)
                if (!object.duplicatePlacement) modelIndex.emplace(object.hash, &object);
    }

    // The instances, deduplicated across every section in the renderer's order;
    // the hidden second set and the backdrop are left out afterwards.
    std::vector<Placement> placements;
    std::vector<size_t> perSection(loaded.size(), 0);
    std::vector<Aabb> sectionBounds(loaded.size());
    std::unordered_set<u64> seen;
    size_t unresolved = 0, redundant = 0, alternate = 0, backdrop = 0, distantLod = 0;
    for (size_t i = 0; i < loaded.size(); ++i) {
        const nfs::LoadedSection& section = *loaded[i];
        const bool hidden = section.info && world.IsUnusedAlternate(*section.info);
        const u32 number = section.info ? section.info->number : 0;
        auto add = [&](const nfs::SolidObject* model, const Mat4& transform) {
            if (hidden) { ++alternate; return; }
            // BackdropHidden in the default view. FRS_DUMP_KEEP=<name>: that backdrop is kept (UG2's sea is PAN_OCEAN).
            static const char* keep = std::getenv("FRS_DUMP_KEEP");
            if (model->IsBackdrop() && !(keep && *keep && model->name == keep)) { ++backdrop; return; }
            std::string upper = model->name;
            for (char& c : upper) c = (char)std::toupper((unsigned char)c);
            if (upper.rfind("LOD_", 0) == 0 || upper.find("_1Z_") != std::string::npos) ++distantLod;
            placements.push_back({ number, model, transform });
            ++perSection[i];
        };
        const auto& removed = section.removedNatives;
        for (const nfs::SceneryInstance& instance : section.scenery.instances) {
            if (std::find(removed.begin(), removed.end(),
                          std::make_pair(instance.sceneryNumber, instance.record)) != removed.end())
                continue;
            const u32 hash = section.scenery.ModelHash(instance);
            const auto it = modelIndex.find(hash);
            if (it == modelIndex.end()) { ++unresolved; continue; }
            if (!seen.insert(nfs::PlacementKey(hash, instance.transform)).second) { ++redundant; continue; }
            add(it->second, instance.transform);
        }
        for (const auto& user : section.placements) {
            const auto it = modelIndex.find(user.instance.modelHash);
            if (it == modelIndex.end()) { ++unresolved; continue; }   // a foreign model: not in the region
            add(it->second, user.instance.transform);
        }
    }

    // Models.
    std::vector<char> buffer;
    std::map<u32, const nfs::SolidObject*> models;
    for (const Placement& p : placements) models.emplace(p.model->hash, p.model);
    std::unordered_map<u32, size_t> modelTriangles;
    std::set<u32> textures;
    for (const auto& [hash, model] : models) {
        wchar_t name[32];
        ::swprintf_s(name, L"\\models\\%08X.obj", hash);
        modelTriangles[hash] = WriteModel(*model, folder + name, buffer);
        for (const nfs::SolidMaterial& material : model->materials)
            if (material.indexCount >= 3)
                if (const u32 texture = DiffuseHash(*model, material)) textures.insert(texture);
    }

    // Placements, and the world bounds of what each section places.
    size_t worldTriangles = 0;
    std::unordered_map<u32, size_t> sectionIndex;
    for (size_t i = 0; i < loaded.size(); ++i)
        if (loaded[i]->info) sectionIndex.emplace(loaded[i]->info->number, i);
    if (FILE* out = OpenText(folder + L"\\placements.txt", buffer)) {
        for (const Placement& p : placements) {
            std::fprintf(out, "%u %08X", p.section, p.model->hash);
            for (int k = 0; k < 16; ++k) std::fprintf(out, " %.9g", p.transform.m[k / 4][k % 4]);
            std::fprintf(out, "\n");
            worldTriangles += modelTriangles[p.model->hash];
            const auto home = sectionIndex.find(p.section);
            if (home != sectionIndex.end() && p.model->bounds.Valid())
                sectionBounds[home->second].Add(Transform(p.model->bounds, p.transform));
        }
        std::fclose(out);
    } else {
        LOG_ERROR("region dump: could not write placements.txt");
        return false;
    }

    // Textures: the largest stored copy, top level, as decoded.
    size_t missing = 0;
    if (FILE* out = OpenText(folder + L"\\textures.txt", buffer)) {
        for (u32 hash : textures) {
            const nfs::Texture* texture = world.FindTexture(hash);
            if (!texture) texture = world.FindDisplayTexture(hash);   // the global packs the viewport also draws from
            std::vector<u32> pixels;
            u32 width = 0, height = 0;
            bool decoded = false;
            if (texture) {
                nfs::Texture largest;
                const nfs::Texture& source = world.LargestTexture(hash, largest) ? largest : *texture;
                decoded = nfs::DecodeTexture(source, 0, pixels, width, height) ||
                          nfs::DecodeTexture(*texture, 0, pixels, width, height);
            }
            wchar_t name[40];
            ::swprintf_s(name, L"\\textures\\%08X.tga", hash);
            if (!decoded || !WriteTga(folder + name, pixels, width, height)) {
                ++missing;
                LOG_WARN("region dump: texture %08X %s", hash, texture ? "could not be decoded" : "is not loaded");
                std::fprintf(out, "%08X %s 0 0 0\n", hash, texture ? Token(texture->name).c_str() : "?");
                continue;
            }
            bool alpha = false;
            for (size_t k = 0; k < (size_t)width * height && !alpha; ++k) alpha = (pixels[k] >> 24) < 250;
            std::fprintf(out, "%08X %s %u %u %d\n", hash, Token(texture->name).c_str(), width, height, alpha ? 1 : 0);
        }
        std::fclose(out);
    }

    // Collision, every pack of every section, corners as stored (game axes).
    size_t polygons = 0;
    if (FILE* out = OpenText(folder + L"\\collision.txt", buffer)) {
        for (const auto& section : loaded)
            for (const nfs::CollisionPack& pack : section->collision)
                for (const nfs::CollisionPolygon& p : pack.polygons) {
                    std::fprintf(out, "%u %d %d %d %u %u %u %u %u", pack.sectionNumber, p.barrier ? 1 : 0,
                                 p.quad ? 1 : 0, p.wall ? 1 : 0, p.attributes[0], p.attributes[1],
                                 p.attributes[2], p.attributes[3], p.flags);
                    for (const Vec3& c : p.corners) std::fprintf(out, " %.9g %.9g %.9g", c.x, c.y, c.z);
                    std::fprintf(out, "\n");
                    ++polygons;
                }
        std::fclose(out);
    }

    // Sections.
    if (FILE* out = OpenText(folder + L"\\sections.txt", buffer)) {
        for (size_t i = 0; i < loaded.size(); ++i) {
            const nfs::Section* info = loaded[i]->info;
            if (!info) continue;
            const Aabb& b = sectionBounds[i];
            const Vec3 lo = b.Valid() ? b.min : Vec3{}, hi = b.Valid() ? b.max : Vec3{};
            std::fprintf(out, "%u %s %.3f %.3f %.3f %.3f %.3f %.3f %.3f %.3f %.3f %zu\n", info->number,
                         Token(info->name).c_str(), info->position.x, info->position.y, info->position.z,
                         lo.x, lo.y, lo.z, hi.x, hi.y, hi.z, perSection[i]);
        }
        std::fclose(out);
    }

    LOG_INFO("region dump: skipped %zu unresolved, %zu redundant, %zu in the hidden second set, %zu backdrop; "
             "%zu placements are distant LOD copies (LOD_/_1Z_); %lu ms",
             unresolved, redundant, alternate, backdrop, distantLod, ::GetTickCount() - start);
    LOG_INFO("region dump: %zu models, %zu placements, %zu triangles, %zu textures (%zu missing), "
             "%zu collision polygons -> %S",
             models.size(), placements.size(), worldTriangles, textures.size(), missing, polygons, folder.c_str());
    return true;
}

} // namespace io
