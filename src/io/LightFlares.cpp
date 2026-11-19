#include "io/LightFlares.h"

#include "core/Log.h"
#include "io/MeshOverlay.h"
#include "io/SectionEdits.h"
#include "nfs/ChunkIds.h"
#include "nfs/FreeRoam.h"
#include "nfs/World.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace flares {

namespace {

constexpr size_t kHeaderBytes = 0x60;
constexpr size_t kFlareBytes = 0x30;

float GetF(const std::vector<u8>& b, size_t at) { float v; std::memcpy(&v, b.data() + at, 4); return v; }
void SetF(std::vector<u8>& b, size_t at, float v) { std::memcpy(b.data() + at, &v, 4); }
u32 Get32(const std::vector<u8>& b, size_t at) { u32 v; std::memcpy(&v, b.data() + at, 4); return v; }
void Set32(std::vector<u8>& b, size_t at, u32 v) { std::memcpy(b.data() + at, &v, 4); }

// A pack's payload (the container's children) as header + flares, fillers left out.
bool SplitPack(const std::vector<u8>& payload, std::vector<u8>& header, std::vector<u8>& records) {
    header.clear();
    records.clear();
    size_t at = 0;
    while (at + 8 <= payload.size()) {
        const u32 id = Get32(payload, at), size = Get32(payload, at + 4);
        if (at + 8 + size > payload.size()) break;
        size_t from = at + 8;
        const size_t end = at + 8 + size;
        while (from < end && payload[from] == 0x11) ++from;
        if (id == nfs::kChunkFlareHeader) header.assign(payload.begin() + (ptrdiff_t)from, payload.begin() + (ptrdiff_t)end);
        else if (id == nfs::kChunkFlares) records.assign(payload.begin() + (ptrdiff_t)from, payload.begin() + (ptrdiff_t)end);
        at = end;
    }
    return header.size() >= kHeaderBytes;
}

struct Shipped {
    std::vector<u8> header, records;
};

// Every section's shipped pack, by loaded index.
std::map<size_t, Shipped> ShippedPacks(const nfs::World& world) {
    std::map<size_t, Shipped> out;
    const auto& loaded = world.Loaded();
    for (size_t i = 0; i < loaded.size(); ++i) {
        if (!loaded[i] || !loaded[i]->info) continue;
        std::vector<u8> payload;
        if (!ShippedChunkData(world, *loaded[i]->info, nfs::kChunkFlarePack, payload)) continue;
        Shipped pack;
        if (SplitPack(payload, pack.header, pack.records)) out[i] = std::move(pack);
    }
    return out;
}

Vec3 FlareAt(const std::vector<u8>& records, size_t k) {
    const size_t o = k * kFlareBytes;
    return { GetF(records, o + 0x10), GetF(records, o + 0x14), GetF(records, o + 0x18) };
}

} // namespace

std::map<u32, std::vector<Vec3>> LampOffsets(const nfs::World& world) {
    std::map<u32, std::vector<Vec3>> out;
    const std::map<size_t, Shipped> packs = ShippedPacks(world);
    std::vector<Vec3> all;
    for (const auto& [index, pack] : packs)
        for (size_t k = 0; (k + 1) * kFlareBytes <= pack.records.size(); ++k) all.push_back(FlareAt(pack.records, k));
    if (all.empty()) return out;
    for (const auto& loaded : world.Loaded()) {
        if (!loaded) continue;
        const nfs::Scenery& scenery = loaded->scenery;
        for (const nfs::SceneryInstance& instance : scenery.instances) {
            const u32 hash = scenery.ModelHash(instance);
            if (out.count(hash)) continue;
            const nfs::SolidObject* model = world.FindModel(hash);
            if (!model || !model->bounds.Valid()) continue;
            const std::string& n = model->name;
            if (n.find("LIGHT") == std::string::npos && n.find("LAMP") == std::string::npos) continue;
            // The flares within the model's own box (a little grown), in its space.
            const Mat4 toLocal = InverseAffine(instance.transform);
            const Aabb& b = model->bounds;
            std::vector<Vec3> lamps;
            for (const Vec3& f : all) {
                const Vec3 local = toLocal.TransformPoint(f);
                if (local.x < b.min.x - 1.5f || local.x > b.max.x + 1.5f || local.y < b.min.y - 1.5f ||
                    local.y > b.max.y + 1.5f || local.z < b.min.z - 1.0f || local.z > b.max.z + 1.5f)
                    continue;
                lamps.push_back(local);
            }
            if (!lamps.empty()) out[hash] = std::move(lamps);
        }
    }
    return out;
}

bool PlanFlares(const nfs::World& world, const std::wstring& modelsFolder, const std::vector<Vec3>& positions,
                Plan& plan, std::wstring& error) {
    const std::map<size_t, Shipped> shipped = ShippedPacks(world);
    if (shipped.empty()) {
        plan.report = L"The region ships no light flares to copy.\n";
        return true;
    }
    const Shipped& model = shipped.begin()->second;    // a header and a flare to copy

    std::map<size_t, std::vector<Vec3>> wanted;
    for (const Vec3& p : positions) {
        const size_t index = world.SectionForPlacement(p);
        if (index < world.Loaded().size() && world.Loaded()[index]->info) wanted[index].push_back(p);
    }

    std::vector<SpanOverlayInfo> existing;
    ListSpanOverlays(modelsFolder, world.Region(), existing);

    const auto& loaded = world.Loaded();
    for (size_t index = 0; index < loaded.size(); ++index) {
        if (!loaded[index]->info) continue;
        const nfs::Section& section = *loaded[index]->info;
        std::vector<SectionEdit> edits;
        std::wstring why;
        if (!DiffSection(world, section, edits, why)) {
            plan.report += std::wstring(section.name.begin(), section.name.end()) + L": " + why + L"\n";
            continue;
        }
        bool had = false;
        std::vector<SectionEdit> kept;
        for (SectionEdit& edit : edits) {
            if (edit.kind == SectionEdit::Kind::Flares) had = true;
            else kept.push_back(std::move(edit));
        }
        const auto want = wanted.find(index);
        if (!had && want == wanted.end()) continue;

        if (want != wanted.end()) {
            // The shipped pack, or one made from another section's, then ours.
            const auto own = shipped.find(index);
            std::vector<u8> header = own != shipped.end() ? own->second.header : model.header;
            std::vector<u8> records = own != shipped.end() ? own->second.records : std::vector<u8>{};
            header.resize(kHeaderBytes, 0);
            std::vector<u8> sample(model.records.begin(), model.records.begin() + (ptrdiff_t)kFlareBytes);
            for (const Vec3& p : want->second) {
                std::vector<u8> r = sample;
                SetF(r, 0x10, p.x);
                SetF(r, 0x14, p.y);
                SetF(r, 0x18, p.z);
                // The reflection on the road (eRenderLightFlare 0x490920): 999 =
                // the game mirrors it about the floor under it (2 * floor - z),
                // 9999 = none. Retail flares hang 5-15 m over the road; one
                // higher mirrors far off the road, and one with no floor under
                // it, or sitting on it, has nothing to mirror in.
                float floor = 0.0f;
                const bool overFloor = nfs::GroundBelow(world, p.x, p.y, p.z + 0.5f, floor);
                const float hang = p.z - floor;
                SetF(r, 0x1C, overFloor && hang > 0.3f && hang <= 15.0f ? 999.0f : 9999.0f);
                SetF(r, 0x20, 0.0f);
                SetF(r, 0x24, 0.0f);
                SetF(r, 0x28, -1.0f);
                r[0x2C] = 9;                                 // a street light
                r[0x2D] = 2;                                 // no falloff
                r[0x2E] = (u8)(section.number & 0xFF);
                records.insert(records.end(), r.begin(), r.end());
            }
            // The header: count, box and section.
            const size_t count = records.size() / kFlareBytes;
            Vec3 lo{ 1e30f, 1e30f, 1e30f }, hi{ -1e30f, -1e30f, -1e30f };
            for (size_t k = 0; k < count; ++k) {
                const Vec3 f = FlareAt(records, k);
                lo = { std::min(lo.x, f.x), std::min(lo.y, f.y), std::min(lo.z, f.z) };
                hi = { std::max(hi.x, f.x), std::max(hi.y, f.y), std::max(hi.z, f.z) };
            }
            SetF(header, 0x30, lo.x); SetF(header, 0x34, lo.y); SetF(header, 0x38, lo.z); SetF(header, 0x3C, 0.0f);
            SetF(header, 0x40, hi.x); SetF(header, 0x44, hi.y); SetF(header, 0x48, hi.z); SetF(header, 0x4C, 0.0f);
            const u16 count16 = (u16)std::min<size_t>(count, 0xFFFF);
            std::memcpy(header.data() + 0x50, &count16, 2);
            header[0x52] = 0;
            Set32(header, 0x54, section.number);

            SectionEdit pack;
            pack.kind = SectionEdit::Kind::Flares;
            pack.label = "light flares";
            pack.record = header;
            pack.record.insert(pack.record.end(), records.begin(), records.end());
            kept.push_back(std::move(pack));
            plan.flares += want->second.size();
        }

        SectionSpanPlan spans;
        if (!PlanSectionFromEdits(world, section, kept, existing, spans, error)) {
            error = std::wstring(section.name.begin(), section.name.end()) + L": " + error;
            return false;
        }
        for (const SectionSpan& span : spans.spans) {
            SpanOverlayInfo info;
            info.fileOffset = span.fileOffset;
            info.byteCount = (u32)span.bytes.size();
            info.layoutFile = span.layoutFile;
            existing.push_back(info);
        }
        for (SectionSpan& span : spans.spans) plan.spans.push_back(std::move(span));
        for (std::wstring& path : spans.superseded) plan.superseded.push_back(std::move(path));
        ++plan.sections;
        plan.report += std::wstring(section.name.begin(), section.name.end()) +
                       (want != wanted.end() ? L": light flares written\n" : L": light flares removed\n");
        LOG_INFO("light flares %s: %zu of the editor's", section.name.c_str(),
                 want != wanted.end() ? want->second.size() : (size_t)0);
    }
    return true;
}

} // namespace flares
