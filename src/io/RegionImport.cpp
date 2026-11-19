#include "io/RegionImport.h"

#include "core/Log.h"
#include "io/DistrictFog.h"
#include "io/MeshOverlay.h"
#include "io/SectionRebuild.h"
#include "io/SectionSplice.h"
#include "nfs/ChunkIds.h"
#include "nfs/ChunkReader.h"
#include "nfs/Collision.h"
#include "nfs/SceneryEdit.h"
#include "nfs/World.h"

#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <cwctype>
#include <fstream>
#include <map>
#include <sstream>

namespace io {

namespace {

constexpr u32 kTable = 0x00034110;
constexpr size_t kEntry = 80;
constexpr u32 kVisibility = 0x80034150, kVisHeader = 0x00034151, kOutlines = 0x00034152, kLists = 0x00034153;
constexpr u8 kMarker[8] = { 0x0B, 0, 0, 0, 0x0B, 0, 0, 0 };
constexpr char kImportMark[8] = { 'F', 'R', 'S', 'I', 'M', 'P', '1', 0 };
constexpr int kFirstImportLetter = 27;   // '[': the letters the loader opens past Z
constexpr int kLastLetter = 63;          // 6399: past it the game's letter byte turns negative
constexpr size_t kShiftUnit = 0x8000;
constexpr u32 kSpawners = 0x00034027;
constexpr float kPlanLimit = 4096.0f;    // nfs/Collision: i16 eighths of a metre
constexpr float kCentreReach = 350.0f;   // how far a section's ground reaches past its centre

u32 Get32(const u8* p) { u32 v; std::memcpy(&v, p, 4); return v; }
u16 Get16(const u8* p) { u16 v; std::memcpy(&v, p, 2); return v; }
float GetF(const u8* p) { float v; std::memcpy(&v, p, 4); return v; }
void Set32(std::vector<u8>& b, size_t at, u32 v) { std::memcpy(b.data() + at, &v, 4); }
void Set16(std::vector<u8>& b, size_t at, u16 v) { std::memcpy(b.data() + at, &v, 2); }
void SetF(std::vector<u8>& b, size_t at, float v) { std::memcpy(b.data() + at, &v, 4); }
void Put32(std::vector<u8>& b, u32 v) { const size_t at = b.size(); b.resize(at + 4); Set32(b, at, v); }

std::vector<u8> ReadWhole(const std::wstring& path) {
    std::vector<u8> bytes;
    FILE* file = nullptr;
    if (::_wfopen_s(&file, path.c_str(), L"rb") != 0 || !file) return bytes;
    std::fseek(file, 0, SEEK_END);
    const long size = std::ftell(file);
    std::fseek(file, 0, SEEK_SET);
    if (size > 0) {
        bytes.resize((size_t)size);
        if (std::fread(bytes.data(), 1, bytes.size(), file) != bytes.size()) bytes.clear();
    }
    std::fclose(file);
    return bytes;
}

std::string Narrow(const std::wstring& t) {
    std::string out;
    for (wchar_t c : t) out += (char)c;
    return out;
}

std::string SectionName(u32 number) {
    const int letter = (int)(number / 100);
    return std::string(1, (char)('A' + letter - 1)) + std::to_string(number % 100);
}

// The top-level chunks of a file or a section: (offset, id, size).
struct Top { size_t at; u32 id; u32 size; };
std::vector<Top> TopChunks(const u8* data, size_t size) {
    std::vector<Top> out;
    for (size_t at = 0; at + 8 <= size;) {
        const u32 id = Get32(data + at), length = Get32(data + at + 4);
        if (at + 8 + (size_t)length > size) break;
        out.push_back({ at, id, length });
        at += 8 + (size_t)length;
    }
    return out;
}

// Replaces [start, end) of `bytes` -- whole chunks -- and remakes the padding
// chunk that follows so the next real chunk moves by a whole number of
// kShiftUnit: whatever alignment the later chunks had survives
// (io/MarkerWrite's SpliceRealigned).
void SpliceRealigned(std::vector<u8>& bytes, size_t start, size_t end, const std::vector<u8>& replacement) {
    size_t next = end;
    if (next + 8 <= bytes.size() && Get32(bytes.data() + next) == 0) next += 8 + Get32(bytes.data() + next + 4);
    const size_t padAt = start + replacement.size();
    size_t newNext = next;
    while (newNext < padAt + 8) newNext += kShiftUnit;
    std::vector<u8> pad(newNext - padAt, 0);
    Set32(pad, 4, (u32)(pad.size() - 8));
    std::vector<u8> middle(replacement);
    middle.insert(middle.end(), pad.begin(), pad.end());
    bytes.erase(bytes.begin() + (std::ptrdiff_t)start, bytes.begin() + (std::ptrdiff_t)next);
    bytes.insert(bytes.begin() + (std::ptrdiff_t)start, middle.begin(), middle.end());
}

// A chunk of the city's light flares, placed effects or breakable props,
// switched off in place: its count zeroed (phase 1 brings none of them).
bool SwitchedOff(const u8* whole, size_t size, u32 id, std::vector<u8>& out) {
    if (id != nfs::kChunkFlarePack && id != nfs::kChunkEmitters && id != kSpawners) return false;
    out.assign(whole, whole + size);
    const auto payload = [&](size_t header, size_t end) {
        size_t p = header + 8;
        while (p < end && out[p] == 0x11) ++p;
        return p;
    };
    const auto zero = [&](size_t at, size_t bytes) { if (at + bytes <= out.size()) std::memset(out.data() + at, 0, bytes); };
    if (id == nfs::kChunkFlarePack) {
        for (size_t at = 8; at + 8 <= out.size();) {
            const u32 child = Get32(out.data() + at), length = Get32(out.data() + at + 4);
            const size_t end = std::min(out.size(), at + 8 + (size_t)length);
            if (child == nfs::kChunkFlareHeader) zero(payload(at, end) + 0x50, 2);
            at = end;
        }
    } else if (id == nfs::kChunkEmitters) {
        zero(payload(0, out.size()) + 12, 4);
    } else {
        zero(payload(0, out.size()) + 16, 4);
    }
    return true;
}

// ---- transforming one section

struct SectionMap {
    const RegionImport* import;
    std::vector<int> letters;
    u32 Map(u32 number) const { return import->MapNumber(number, letters); }
};

constexpr size_t kInstBoundsMin = 0x00, kInstBoundsMax = 0x0C, kInstInfo = 0x18, kInstFlags = 0x1A,
                 kInstTranslation = 0x20, kInstRotation = 0x2C;

std::vector<u8> TransformRecord(const std::vector<u8>& record, const Mat4& M, const nfs::SolidObject* worldSpaceModel) {
    std::vector<u8> out = record;
    Mat4 inst = Mat4::Identity();
    for (int r = 0; r < 3; ++r)
        for (int c = 0; c < 3; ++c) {
            i16 v;
            std::memcpy(&v, record.data() + kInstRotation + (size_t)(r * 3 + c) * 2, 2);
            inst.m[r][c] = v / 8192.0f;
        }
    inst.m[3][0] = GetF(record.data() + kInstTranslation);
    inst.m[3][1] = GetF(record.data() + kInstTranslation + 4);
    inst.m[3][2] = GetF(record.data() + kInstTranslation + 8);
    u16 flags = Get16(record.data() + kInstFlags);
    Mat4 placed;
    if (flags & 0x0100) {
        // A world-space mesh the game draws without the record's matrix: the
        // flag goes and the record gets the model's own matrix, moved.
        placed = (worldSpaceModel ? worldSpaceModel->transform : Mat4::Identity()) * M;
        flags = (u16)(flags & ~0x0100);
    } else {
        placed = inst * M;
    }
    for (int r = 0; r < 3; ++r)
        for (int c = 0; c < 3; ++c) {
            const long v = std::lround(placed.m[r][c] * 8192.0f);
            const i16 q = (i16)std::clamp(v, -32768L, 32767L);
            std::memcpy(out.data() + kInstRotation + (size_t)(r * 3 + c) * 2, &q, 2);
        }
    SetF(out, kInstTranslation, placed.m[3][0]);
    SetF(out, kInstTranslation + 4, placed.m[3][1]);
    SetF(out, kInstTranslation + 8, placed.m[3][2]);
    Set16(out, kInstFlags, flags);
    Aabb box;
    box.min = { GetF(record.data() + kInstBoundsMin), GetF(record.data() + kInstBoundsMin + 4), GetF(record.data() + kInstBoundsMin + 8) };
    box.max = { GetF(record.data() + kInstBoundsMax), GetF(record.data() + kInstBoundsMax + 4), GetF(record.data() + kInstBoundsMax + 8) };
    const Aabb moved = Transform(box, M);
    if (moved.Valid()) {
        SetF(out, kInstBoundsMin, moved.min.x); SetF(out, kInstBoundsMin + 4, moved.min.y); SetF(out, kInstBoundsMin + 8, moved.min.z);
        SetF(out, kInstBoundsMax, moved.max.x); SetF(out, kInstBoundsMax + 4, moved.max.y); SetF(out, kInstBoundsMax + 8, moved.max.z);
    }
    return out;
}

bool TransformSection(const nfs::World& source, const nfs::Section& section, const SectionMap& map, const Mat4& M,
                      std::vector<u8>& out, std::wstring& error, size_t& instances, size_t& polygons,
                      size_t& dropped) {
    const ByteSpan bytes = source.SectionBytes(section);
    if (bytes.empty()) { error = L"no bytes"; return false; }
    const nfs::LoadedSection* loaded = nullptr;
    for (const auto& l : source.Loaded())
        if (l->info == &section) loaded = l.get();

    std::vector<ChunkEdit> edits;
    nfs::ChunkReader reader(bytes, section.streamOffset);
    nfs::Chunk chunk;
    while (reader.Next(chunk)) {
        const size_t start = (size_t)(chunk.headerOffset - section.streamOffset);
        const size_t end = start + 8 + chunk.raw.size;
        const ByteSpan whole{ chunk.raw.data - 8, chunk.raw.size + 8 };
        ChunkEdit edit;
        edit.start = start;
        edit.end = end;
        if (chunk.id == nfs::kChunkTrackScenery) {
            nfs::SceneryChunkInfo info;
            nfs::ReadSceneryChunk(whole, chunk.headerOffset, info);
            std::vector<std::vector<u8>> records;
            nfs::ReadSceneryRecords(whole, chunk.headerOffset, records);
            std::map<u32, u32> modelOf;   // record -> model hash
            if (loaded)
                for (const nfs::SceneryInstance& inst : loaded->scenery.instances)
                    if (inst.sceneryNumber == info.number) modelOf[inst.record] = loaded->scenery.ModelHash(inst);
            std::vector<nfs::ReplacedRecord> replaced;
            for (size_t r = 0; r < records.size(); ++r) {
                const auto found = modelOf.find((u32)r);
                const nfs::SolidObject* model = found != modelOf.end() ? source.FindModel(found->second) : nullptr;
                replaced.push_back({ r, TransformRecord(records[r], M, model) });
            }
            instances += records.size();
            if (!nfs::AppendScenery(whole, chunk.headerOffset, {}, edit.bytes, error, replaced)) return false;
            // Its header names the section (0x34101 +0x0C).
            for (const Top& c : TopChunks(edit.bytes.data() + 8, edit.bytes.size() - 8))
                if (c.id == 0x00034101 && c.size >= 0x10) Set32(edit.bytes, 8 + c.at + 8 + 0x0C, map.Map(info.number));
        } else if (chunk.id == nfs::kChunkCollisionPack) {
            nfs::CollisionPack pack;
            if (!nfs::ParseCollision(chunk, pack)) continue;
            // The file holds plan positions in i16 eighths of a metre: what is
            // moved past +/-4096 cannot be written, and is left out -- the
            // region is drawn there, but not driven on.
            std::vector<nfs::CollisionPolygon> kept;
            for (nfs::CollisionPolygon& p : pack.polygons) {
                bool inside = true;
                for (Vec3& c : p.corners) {
                    const Vec3 moved = M.TransformPoint(c);
                    c = { moved.x, moved.y, p.barrier ? 0.0f : moved.z };
                    inside &= std::fabs(c.x) < kPlanLimit - 1.0f && std::fabs(c.y) < kPlanLimit - 1.0f;
                }
                if (inside) kept.push_back(std::move(p));
                else ++dropped;
            }
            pack.polygons = std::move(kept);
            pack.sectionNumber = map.Map(pack.sectionNumber);
            pack.gridRank.clear();
            pack.edited = true;
            polygons += pack.polygons.size();
            if (!nfs::EncodeCollision(pack, edit.bytes, error)) return false;
        } else if (!SwitchedOff(whole.data, whole.size, chunk.id, edit.bytes)) {
            continue;
        }
        edits.push_back(std::move(edit));
    }
    out = SpliceSection(bytes, std::move(edits));
    return true;
}

// ---- the source's files

struct SourceTable { std::vector<std::vector<u8>> entries; };

bool ReadTable(const std::vector<u8>& file, std::vector<std::vector<u8>>& entries) {
    for (const Top& t : TopChunks(file.data(), file.size()))
        if (t.id == kTable) {
            for (size_t k = 0; k + kEntry <= t.size; k += kEntry)
                entries.emplace_back(file.begin() + (std::ptrdiff_t)(t.at + 8 + k), file.begin() + (std::ptrdiff_t)(t.at + 8 + k + kEntry));
            return true;
        }
    return false;
}

struct Record { std::vector<u8> body; };   // after the marker

std::vector<Record> Records(const u8* data, size_t size) {
    std::vector<size_t> starts;
    for (size_t at = 0; at + 8 <= size; at += 4)
        if (std::memcmp(data + at, kMarker, 8) == 0) starts.push_back(at);
    std::vector<Record> out;
    for (size_t k = 0; k < starts.size(); ++k) {
        const size_t begin = starts[k] + 8, end = k + 1 < starts.size() ? starts[k + 1] : size;
        out.push_back({ std::vector<u8>(data + begin, data + end) });
    }
    return out;
}

struct Visibility {
    std::vector<u8> header;                 // 0x34151
    std::vector<Record> outlines, lists;    // 0x34152, 0x34153
    std::vector<std::pair<u32, std::vector<u8>>> others;   // the other children, as they are
};

bool ReadVisibility(const u8* container, size_t size, Visibility& out) {
    for (const Top& c : TopChunks(container, size)) {
        const u8* d = container + c.at + 8;
        if (c.id == kVisHeader) out.header.assign(d, d + c.size);
        else if (c.id == kOutlines) out.outlines = Records(d, c.size);
        else if (c.id == kLists) out.lists = Records(d, c.size);
        else out.others.push_back({ c.id, std::vector<u8>(d, d + c.size) });
    }
    return !out.outlines.empty() && !out.lists.empty();
}

std::wstring ModelsOf(const std::wstring& game) { return game + L"\\models"; }

std::wstring Widen(const std::string& t) { return std::wstring(t.begin(), t.end()); }

// The span file of an imported section is named by its number: past Z the
// letters are '[', '\', ']', ... and 'a'..'z', which a file name cannot carry
// (or, on Windows, tell apart from 'A'..'Z').
std::wstring SpanKey(const std::wstring& target, u32 number) {
    return target + L"-" + std::to_wstring(number) + L"-stream";
}

} // namespace

// ---- the description

Mat4 RegionImport::Transform() const {
    const float a = yawDegrees * 3.14159265358979f / 180.0f;
    const float c = std::cos(a), s = std::sin(a);
    Mat4 m = Mat4::Identity();
    m.m[0][0] = c;  m.m[0][1] = s;
    m.m[1][0] = -s; m.m[1][1] = c;
    m.m[3][0] = offset.x; m.m[3][1] = offset.y; m.m[3][2] = offset.z;
    return m;
}

Mat4 RegionImport::ShownTransform() const { return Shown().Transform(); }

RegionImport RegionImport::Shown() const {
    RegionImport r = *this;
    if (moved) {
        r.offset = shownOffset;
        r.yawDegrees = shownYaw;
    }
    r.moved = false;
    return r;
}

u32 RegionImport::MapNumber(u32 number, const std::vector<int>& letters) const {
    const int letter = (int)(number / 100);
    const auto it = std::find(letters.begin(), letters.end(), letter);
    if (it == letters.end()) return 0;
    const int target = firstLetter + (int)(it - letters.begin());
    if (target > kLastLetter) return 0;
    return (u32)target * 100 + number % 100;
}

std::wstring ImportsPath(const std::wstring& modelsFolder, const std::wstring& target) {
    return modelsFolder + L"\\imports-" + target + L".txt";
}

bool ReadImports(const std::wstring& modelsFolder, const std::wstring& target, std::vector<RegionImport>& out) {
    out.clear();
    std::ifstream in(ImportsPath(modelsFolder, target));
    if (!in) return false;
    std::string line;
    while (std::getline(in, line)) {
        std::istringstream words(line);
        std::string key, source;
        RegionImport r;
        if (!(words >> key) || key != "import") continue;
        if (words >> source >> r.firstLetter >> r.offset.x >> r.offset.y >> r.offset.z >> r.yawDegrees) {
            r.source = std::wstring(source.begin(), source.end());
            r.moved = (bool)(words >> r.shownOffset.x >> r.shownOffset.y >> r.shownOffset.z >> r.shownYaw);
            out.push_back(r);
        }
    }
    return true;
}

bool WriteImports(const std::wstring& modelsFolder, const std::wstring& target, const std::vector<RegionImport>& list) {
    const std::wstring path = ImportsPath(modelsFolder, target);
    if (list.empty()) { ::DeleteFileW(path.c_str()); return true; }
    std::ofstream out(path, std::ios::trunc);
    if (!out) return false;
    out << "# FRS World Editor: whole regions brought into this one (docs/research/region-import.md)\n"
           "# import <source> <first letter> <x> <y> <z> <yaw degrees> [<shown x> <y> <z> <yaw>, not baked yet]\n";
    char text[320];
    for (const RegionImport& r : list) {
        std::snprintf(text, sizeof text, "import %s %d %.3f %.3f %.3f %.4f", Narrow(r.source).c_str(), r.firstLetter,
                      r.offset.x, r.offset.y, r.offset.z, r.yawDegrees);
        out << text;
        if (r.moved) {
            std::snprintf(text, sizeof text, " %.3f %.3f %.3f %.4f", r.shownOffset.x, r.shownOffset.y, r.shownOffset.z,
                          r.shownYaw);
            out << text;
        }
        out << "\n";
    }
    return (bool)out;
}

std::vector<int> SourceLetters(const std::wstring& gameFolder, const std::wstring& source) {
    std::vector<std::vector<u8>> entries;
    ReadTable(ReadWhole(gameFolder + L"\\TRACKS\\" + source + L".BUN"), entries);
    std::vector<int> letters;
    for (const auto& e : entries) {
        const u32 number = Get32(e.data() + 8);
        if (number >= 100) letters.push_back((int)(number / 100));
    }
    std::sort(letters.begin(), letters.end());
    letters.erase(std::unique(letters.begin(), letters.end()), letters.end());
    return letters;
}

std::vector<Vec2> SectionCentres(const std::wstring& gameFolder, const std::wstring& region) {
    std::vector<std::vector<u8>> entries;
    ReadTable(ReadWhole(gameFolder + L"\\TRACKS\\" + region + L".BUN"), entries);
    std::vector<Vec2> out;
    for (const auto& e : entries) {
        const u32 number = Get32(e.data() + 8);
        const float x = GetF(e.data() + 0x24), y = GetF(e.data() + 0x28);
        if (number >= 100 && number / 100 < 24 && (x != 0.0f || y != 0.0f)) out.push_back({ x, y });
    }
    return out;
}

bool InsideCollisionRange(const std::vector<Vec2>& sourceCentres, const RegionImport& import) {
    if (sourceCentres.empty()) return false;   // nothing read: nothing known to fit
    const Mat4 M = import.Transform();
    const float limit = kPlanLimit - kCentreReach;
    for (const Vec2& c : sourceCentres) {
        const Vec3 p = M.TransformPoint({ c.x, c.y, 0.0f });
        if (std::fabs(p.x) > limit || std::fabs(p.y) > limit) return false;
    }
    return true;
}

RegionImport SuggestImport(const std::wstring& gameFolder, const std::wstring& target, const std::wstring& source) {
    const auto centres = [&](const std::wstring& region) { return SectionCentres(gameFolder, region); };
    // What is already there: the target's sections and every earlier import's, where it put them.
    std::vector<Vec2> taken = centres(target);
    std::vector<RegionImport> existing;
    ReadImports(ModelsOf(gameFolder), target, existing);
    int next = kFirstImportLetter;
    for (const RegionImport& e : existing) {
        next = std::max(next, e.firstLetter + (int)SourceLetters(gameFolder, e.source).size());
        const Mat4 M = e.Transform();
        for (const Vec2& c : centres(e.source)) {
            const Vec3 p = M.TransformPoint({ c.x, c.y, 0.0f });
            taken.push_back({ p.x, p.y });
        }
    }
    const std::vector<Vec2> own = centres(source);
    RegionImport r;
    r.source = source;
    r.firstLetter = next;
    // Nothing read (no such file, no table): no place to look for -- and the
    // search below would have no bounds to stop at.
    if (own.empty() || taken.empty()) {
        LOG_WARN("import: no section centres read for %S (%zu) or %S (%zu)", source.c_str(), own.size(),
                 target.c_str(), taken.size());
        r.firstLetter = 0;
        return r;
    }

    // The game's collision holds plan positions in eighths of a metre in an
    // i16: everything drivable stays inside +/-4096 (less what a section's
    // ground reaches past its centre). Inside that, the place and the turn (15
    // degree steps) that leave the most room to what is already there.
    const float limit = kPlanLimit - kCentreReach;
    float best = -1.0f;
    for (int yaw = 0; yaw < 360; yaw += 15) {
        const float a = yaw * 3.14159265358979f / 180.0f, c = std::cos(a), s = std::sin(a);
        std::vector<Vec2> turned;
        Vec2 lo{ 1e30f, 1e30f }, hi{ -1e30f, -1e30f };
        for (const Vec2& p : own) {
            const Vec2 t{ p.x * c - p.y * s, p.x * s + p.y * c };
            turned.push_back(t);
            lo = { std::min(lo.x, t.x), std::min(lo.y, t.y) };
            hi = { std::max(hi.x, t.x), std::max(hi.y, t.y) };
        }
        for (float ox = -limit - lo.x; ox <= limit - hi.x; ox += 50.0f)
            for (float oy = -limit - lo.y; oy <= limit - hi.y; oy += 50.0f) {
                float gap = 1e30f;
                for (const Vec2& t : turned) {
                    for (const Vec2& q : taken) {
                        const float dx = t.x + ox - q.x, dy = t.y + oy - q.y;
                        gap = std::min(gap, dx * dx + dy * dy);
                    }
                    if (gap <= best) break;
                }
                if (gap > best) {
                    best = gap;
                    r.offset = { ox, oy, 0.0f };
                    r.yawDegrees = (float)yaw;
                }
            }
    }
    if (best < 0.0f) LOG_WARN("import: %S does not fit inside the collision range at any turn", source.c_str());
    else LOG_INFO("import: %S suggested at (%.0f, %.0f) turned %.0f, %.0f m from the nearest section",
                  source.c_str(), r.offset.x, r.offset.y, r.yawDegrees, std::sqrt(best));
    return r;
}

// ---- baking the sections

bool RemoveImportSpans(const std::wstring& gameFolder, const std::wstring& target, const RegionImport& import) {
    const std::vector<int> letters = SourceLetters(gameFolder, import.source);
    std::vector<SpanOverlayInfo> existing;
    ListSpanOverlays(ModelsOf(gameFolder), target, existing);
    size_t removed = 0;
    for (const SpanOverlayInfo& span : existing) {
        if (span.layoutFile) continue;
        // <TARGET>-<number>-stream (SpanKey): a number in one of the import's letters.
        const std::wstring file = span.path.substr(span.path.find_last_of(L"\\/") + 1);
        const size_t dash = file.find(L'-');
        if (dash == std::wstring::npos || dash + 1 >= file.size() || !iswdigit(file[dash + 1])) continue;
        const int letter = (int)(std::wcstoul(file.c_str() + dash + 1, nullptr, 10) / 100);
        if (letter >= import.firstLetter && letter < import.firstLetter + (int)letters.size())
            if (::DeleteFileW(span.path.c_str())) ++removed;
    }
    LOG_INFO("import: %zu spans of %S taken out", removed, import.source.c_str());
    return true;
}

bool BakeImport(const nfs::World& target, const std::wstring& gameFolder, const RegionImport& import,
                std::wstring& message) {
    const std::wstring tracks = gameFolder + L"\\TRACKS";
    nfs::World source;
    if (!source.Open(tracks, import.source)) {
        message = L"Could not open " + import.source + L".";
        return false;
    }
    source.LoadAllSections();
    SectionMap map{ &import, SourceLetters(gameFolder, import.source) };
    if (import.firstLetter + (int)map.letters.size() - 1 > kLastLetter) {
        message = L"Not enough section letters are left for " + import.source + L".";
        return false;
    }
    const Mat4 M = import.Transform();
    const std::wstring targetName = target.Region();

    // This import's spans from an earlier bake stay until the new ones are all
    // made: a bake that fails leaves the region as it was.
    std::vector<SpanOverlayInfo> existing;
    ListSpanOverlays(ModelsOf(gameFolder), targetName, existing);
    const auto mine = [&](const SpanOverlayInfo& span) {
        const std::wstring file = span.path.substr(span.path.find_last_of(L"\\/") + 1);
        const size_t dash = file.find(L'-');
        if (dash == std::wstring::npos || dash + 1 >= file.size() || !iswdigit(file[dash + 1])) return false;
        const int letter = (int)(std::wcstoul(file.c_str() + dash + 1, nullptr, 10) / 100);
        return letter >= import.firstLetter && letter < import.firstLetter + (int)map.letters.size();
    };
    u64 highest = target.StreamBytes().size;
    for (const nfs::Section& s : target.Table().Sections())
        if (s.number / 100 < (u32)kFirstImportLetter) highest = std::max<u64>(highest, s.streamOffset + s.streamSize);
    for (const SpanOverlayInfo& span : existing)
        if (!span.layoutFile && !mine(span)) highest = std::max<u64>(highest, span.fileOffset + span.byteCount);

    std::vector<SectionSpan> spans;
    size_t sections = 0, instances = 0, polygons = 0, skipped = 0, failures = 0, dropped = 0;
    std::wstring firstFailure;
    for (const nfs::Section& section : source.Table().Sections()) {
        const u32 number = map.Map(section.number);
        if (section.number < 100 || !number || section.streamSize == 0) { ++skipped; continue; }
        std::vector<u8> bytes;
        std::wstring error;
        if (!TransformSection(source, section, map, M, bytes, error, instances, polygons, dropped)) {
            LOG_WARN("import: %s of %S not brought: %S", section.name.c_str(), import.source.c_str(), error.c_str());
            if (failures++ == 0) firstFailure = Widen(section.name) + L": " + error;
            continue;
        }
        const std::string name = SectionName(number);
        const u64 offset = (highest + 4096 + 2047) / 2048 * 2048;
        highest = offset + bytes.size();
        SectionSpan span;
        span.name = Narrow(import.source) + " " + section.name + " as " + name;
        span.key = SpanKey(targetName, number);
        span.fileOffset = offset;
        span.bytes = std::move(bytes);
        spans.push_back(std::move(span));
        ++sections;
    }
    // All or nothing: a region with holes in it is no use to drive.
    if (failures) {
        wchar_t text[96];
        ::swprintf_s(text, L"%zu sections of %s could not be moved there. ", failures, import.source.c_str());
        message = text + firstFailure;
        return false;
    }
    RemoveImportSpans(gameFolder, targetName, import);
    const size_t written = WriteSectionSpans(spans, {}, ModelsOf(gameFolder), targetName);
    if (written != spans.size()) {
        message = L"Some sections could not be written; the log has why.";
        return false;
    }
    std::wstring regionMessage;
    if (!WriteFogToGame(gameFolder, targetName, regionMessage)) {
        message = L"The sections are written but the region file is not: " + regionMessage;
        return false;
    }
    wchar_t text[256];
    ::swprintf_s(text, L"%s brought in: %zu sections (%zu objects, %zu collision polygons), letters from %c.",
                 import.source.c_str(), sections, instances, polygons, (wchar_t)(L'A' + import.firstLetter - 1));
    message = text;
    if (dropped) {
        ::swprintf_s(text, L" %zu collision polygons lie past 4096 m from the middle of the map, where the game's "
                           L"collision cannot reach: that part is drawn but has no collision.", dropped);
        message += text;
        LOG_WARN("import: %zu collision polygons of %S past the collision range left out", dropped, import.source.c_str());
    }
    LOG_INFO("import: %S into %S: %zu sections, %zu instances, %zu polygons, %zu skipped; offset (%.1f, %.1f, %.1f) yaw %.2f",
             import.source.c_str(), targetName.c_str(), sections, instances, polygons, skipped, import.offset.x,
             import.offset.y, import.offset.z, import.yawDegrees);
    return true;
}

// ---- the region file

bool ApplyImports(std::vector<u8>& layout, const std::wstring& gameFolder, const std::wstring& target,
                  std::wstring& error) {
    const std::vector<u8> shipped = ReadWhole(gameFolder + L"\\TRACKS\\" + target + L".BUN");
    if (shipped.empty()) return true;   // a region the editor made: not supported yet

    // What an earlier call added comes out first. The container it appended
    // went last (after the fog's own), so cutting at its mark takes out only it.
    for (const Top& t : TopChunks(layout.data(), layout.size()))
        if (t.id == 0 && t.size >= 8 && std::memcmp(layout.data() + t.at + 8, kImportMark, 8) == 0) {
            layout.resize(t.at);
            break;
        }
    size_t shippedVis = SIZE_MAX, shippedVisSize = 0;
    for (const Top& t : TopChunks(shipped.data(), shipped.size()))
        if (t.id == kVisibility) { shippedVis = t.at; shippedVisSize = t.size; break; }
    if (shippedVis == SIZE_MAX || shippedVis + 8 > layout.size()) return true;
    Set32(layout, shippedVis, kVisibility);   // restored (it stays in place when nothing is imported)
    // Table: the entries of letters past Z are the imports'.
    const auto tableAt = [&]() {
        for (const Top& t : TopChunks(layout.data(), layout.size()))
            if (t.id == kTable) return t;
        return Top{ SIZE_MAX, 0, 0 };
    };
    Top table = tableAt();
    if (table.at == SIZE_MAX) return true;
    std::vector<u8> kept;
    bool hadImported = false;
    for (size_t k = 0; k + kEntry <= table.size; k += kEntry) {
        const u8* e = layout.data() + table.at + 8 + k;
        if (Get32(e + 8) / 100 >= (u32)kFirstImportLetter) { hadImported = true; continue; }
        kept.insert(kept.end(), e, e + kEntry);
    }

    std::vector<RegionImport> imports;
    ReadImports(ModelsOf(gameFolder), target, imports);
    if (imports.empty()) {
        if (hadImported) {
            std::vector<u8> chunk;
            Put32(chunk, kTable);
            Put32(chunk, (u32)kept.size());
            chunk.insert(chunk.end(), kept.begin(), kept.end());
            SpliceRealigned(layout, table.at, table.at + 8 + table.size, chunk);
        }
        return true;
    }

    // The baked sections, by the name their span file carries.
    std::vector<SpanOverlayInfo> spans;
    ListSpanOverlays(ModelsOf(gameFolder), target, spans);
    const auto spanOf = [&](u32 number) -> const SpanOverlayInfo* {
        const std::wstring key = SpanKey(target, number);
        for (const SpanOverlayInfo& s : spans)
            if (!s.layoutFile && s.path.find(key + L".") != std::wstring::npos) return &s;
        return nullptr;
    };

    // The shipped visibility, with the zone-list spans (ui/ZoneTools) folded in.
    std::vector<u8> container(shipped.begin() + (std::ptrdiff_t)shippedVis + 8,
                              shipped.begin() + (std::ptrdiff_t)(shippedVis + 8 + shippedVisSize));
    {
        std::vector<SpanOverlay> layoutSpans;
        ReadSpanOverlays(ModelsOf(gameFolder), target, layoutSpans);
        const u64 from = shippedVis + 8, to = from + shippedVisSize;
        for (const SpanOverlay& s : layoutSpans)
            if (s.layoutFile && s.fileOffset >= from && s.fileOffset + s.bytes.size() <= to)
                std::memcpy(container.data() + (s.fileOffset - from), s.bytes.data(), s.bytes.size());
    }
    Visibility vis;
    if (!ReadVisibility(container.data(), container.size(), vis)) {
        error = L"The region's visibility could not be read.";
        return false;
    }
    std::vector<u16> drivable;
    if (vis.header.size() >= 8) {
        const u32 count = Get32(vis.header.data() + 4);
        for (u32 i = 0; i < count && 8 + (size_t)i * 2 + 2 <= vis.header.size(); ++i)
            drivable.push_back(Get16(vis.header.data() + 8 + i * 2));
    }

    size_t newSections = 0, newZones = 0;
    for (const RegionImport& import : imports) {
        const std::vector<u8> sourceFile = ReadWhole(gameFolder + L"\\TRACKS\\" + import.source + L".BUN");
        std::vector<std::vector<u8>> entries;
        ReadTable(sourceFile, entries);
        const std::vector<int> letters = SourceLetters(gameFolder, import.source);
        const Mat4 M = import.Transform();
        // Table entries.
        for (const auto& e : entries) {
            const u32 number = import.MapNumber(Get32(e.data() + 8), letters);
            if (!number || Get32(e.data() + 8) < 100) continue;
            const std::string name = SectionName(number);
            const SpanOverlayInfo* span = spanOf(number);
            if (!span) continue;
            std::vector<u8> entry = e;
            std::memset(entry.data(), 0, 8);
            std::memcpy(entry.data(), name.data(), std::min<size_t>(name.size(), 4));
            Set32(entry, 0x08, number);
            Set32(entry, 0x14, (u32)span->fileOffset);
            Set32(entry, 0x18, span->byteCount);
            std::vector<u8> body = ReadWhole(span->path);
            u32 geometry = span->byteCount;
            if (body.size() >= sizeof(SpanOverlayHeader) + span->byteCount) {
                std::vector<u8> sectionBytes(body.begin() + sizeof(SpanOverlayHeader), body.end());
                geometry = SectionGeometrySize(sectionBytes);
            }
            Set32(entry, 0x1C, geometry);
            const float x = GetF(e.data() + 0x24), y = GetF(e.data() + 0x28);
            if (x != 0.0f || y != 0.0f) {
                const Vec3 p = M.TransformPoint({ x, y, 0.0f });
                SetF(entry, 0x24, p.x);
                SetF(entry, 0x28, p.y);
            }
            kept.insert(kept.end(), entry.begin(), entry.end());
            ++newSections;
        }
        // Visibility.
        Visibility source;
        for (const Top& t : TopChunks(sourceFile.data(), sourceFile.size()))
            if (t.id == kVisibility) ReadVisibility(sourceFile.data() + t.at + 8, t.size, source);
        if (source.header.size() >= 8) {
            const u32 count = Get32(source.header.data() + 4);
            for (u32 i = 0; i < count && 8 + (size_t)i * 2 + 2 <= source.header.size(); ++i)
                if (const u32 n = import.MapNumber(Get16(source.header.data() + 8 + i * 2), letters)) drivable.push_back((u16)n);
        }
        for (const Record& r : source.outlines) {
            if (r.body.size() < 28) continue;
            Record o = r;
            const u32 id = import.MapNumber(Get16(o.body.data()), letters);
            if (!id) continue;
            Set16(o.body, 0, (u16)id);
            const u16 points = Get16(o.body.data() + 2);
            if (28 + (size_t)points * 8 > o.body.size()) continue;
            Vec2 lo{ 1e30f, 1e30f }, hi{ -1e30f, -1e30f };
            for (u16 k = 0; k < points; ++k) {
                const size_t at = 28 + (size_t)k * 8;
                const Vec3 p = M.TransformPoint({ GetF(o.body.data() + at), GetF(o.body.data() + at + 4), 0.0f });
                SetF(o.body, at, p.x);
                SetF(o.body, at + 4, p.y);
                lo = { std::min(lo.x, p.x), std::min(lo.y, p.y) };
                hi = { std::max(hi.x, p.x), std::max(hi.y, p.y) };
            }
            SetF(o.body, 4, lo.x); SetF(o.body, 8, lo.y); SetF(o.body, 12, hi.x); SetF(o.body, 16, hi.y);
            SetF(o.body, 20, (lo.x + hi.x) * 0.5f); SetF(o.body, 24, (lo.y + hi.y) * 0.5f);
            vis.outlines.push_back(o);
            ++newZones;
        }
        // The source's libraries, which its own game loaded everywhere by number.
        std::vector<u16> libraries;
        for (u32 lib : { 2400u, 2500u, 2600u })
            if (const u32 n = import.MapNumber(lib, letters)) libraries.push_back((u16)n);
        for (const Record& r : source.lists) {
            if (r.body.size() < 12) continue;
            const u32 id = import.MapNumber(Get16(r.body.data()), letters);
            if (!id) continue;
            const u32 count = Get32(r.body.data() + 8);
            std::vector<u16> list;
            for (u32 k = 0; k < count && 12 + (size_t)k * 2 + 2 <= r.body.size(); ++k)
                if (const u32 n = import.MapNumber(Get16(r.body.data() + 12 + k * 2), letters)) list.push_back((u16)n);
            for (u16 lib : libraries)
                if (std::find(list.begin(), list.end(), lib) == list.end()) list.push_back(lib);
            Record l;
            l.body.resize(12 + list.size() * 2, 0);
            Set16(l.body, 0, (u16)id);
            Set32(l.body, 8, (u32)list.size());
            std::memcpy(l.body.data() + 12, list.data(), list.size() * 2);
            vis.lists.push_back(l);
        }
    }

    // The table, grown in place.
    {
        std::vector<u8> chunk;
        Put32(chunk, kTable);
        Put32(chunk, (u32)kept.size());
        chunk.insert(chunk.end(), kept.begin(), kept.end());
        table = tableAt();
        SpliceRealigned(layout, table.at, table.at + 8 + table.size, chunk);
    }

    // The visibility container, rebuilt: lists at one stride for all (the
    // loader reads it from the first record's +0x0A, 0xA4 = 72 when absent).
    size_t longest = 0;
    for (const Record& l : vis.lists) longest = std::max<size_t>(longest, Get32(l.body.data() + 8));
    const size_t stride = longest <= 72 ? 0xA4 : ((0x14 + longest * 2 + 3) / 4 * 4);
    std::vector<u8> header = vis.header;
    const size_t needed = 8 + drivable.size() * 2;
    if (header.size() < needed) header.resize((needed + 15) / 16 * 16, 0);
    Set32(header, 4, (u32)drivable.size());
    std::memset(header.data() + 8, 0, header.size() - 8);
    std::memcpy(header.data() + 8, drivable.data(), drivable.size() * 2);
    std::vector<u8> outlines, lists;
    for (const Record& o : vis.outlines) {
        outlines.insert(outlines.end(), kMarker, kMarker + 8);
        outlines.insert(outlines.end(), o.body.begin(), o.body.end());
    }
    for (const Record& l : vis.lists) {
        std::vector<u8> rec(stride, 0);
        std::memcpy(rec.data(), kMarker, 8);
        const u32 count = std::min<u32>(Get32(l.body.data() + 8), (u32)((stride - 0x14) / 2));
        Set16(rec, 8, Get16(l.body.data()));
        Set16(rec, 10, stride == 0xA4 ? 0 : (u16)stride);
        Set32(rec, 16, count);
        std::memcpy(rec.data() + 20, l.body.data() + 12, std::min<size_t>((size_t)count * 2, l.body.size() - 12));
        lists.insert(lists.end(), rec.begin(), rec.end());
    }
    std::vector<u8> body;
    const auto child = [&](u32 id, const std::vector<u8>& data) {
        Put32(body, id);
        Put32(body, (u32)data.size());
        body.insert(body.end(), data.begin(), data.end());
    };
    // In the shipped order: header, outlines, lists, then the rest.
    child(kVisHeader, header);
    child(kOutlines, outlines);
    child(kLists, lists);
    for (const auto& [id, data] : vis.others) child(id, data);

    Set32(layout, shippedVis, 0);   // the shipped container: padding of its size
    std::vector<u8> pad(8 + 16, 0);
    std::memcpy(pad.data() + 8, kImportMark, 8);
    const size_t start = layout.size() + pad.size();
    const size_t aligned = (start + 8 + 0x7F) / 0x80 * 0x80 - 8;
    pad.resize(aligned - layout.size(), 0);
    Set32(pad, 4, (u32)(pad.size() - 8));
    layout.insert(layout.end(), pad.begin(), pad.end());
    Put32(layout, kVisibility);
    Put32(layout, (u32)body.size());
    layout.insert(layout.end(), body.begin(), body.end());
    LOG_INFO("import: %S's region file: %zu imported sections in the table, %zu zones, lists at stride 0x%zX (longest %zu)",
             target.c_str(), newSections, newZones, stride, longest);
    return true;
}

} // namespace io
