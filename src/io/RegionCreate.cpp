#include "io/RegionCreate.h"

#include "core/Log.h"
#include "io/DistrictFog.h"

#include <windows.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>

namespace io {

namespace {

constexpr u32 kTrackInfoChunk = 0x00034201;
constexpr size_t kTrackInfoSize = 0x128;
constexpr u32 kMarkerChunk = 0x00034146;
constexpr size_t kMarkerSize = 0x30;
constexpr u16 kFirstTrack = 4950;

// TrackInfo fields (docs/NFSU2-FORMATS.md, "New tracks and regions").
constexpr size_t kInfoDescription = 0x00, kInfoTrackDirectory = 0x20, kInfoRegionName = 0x40,
                 kInfoRegionDirectory = 0x48, kInfoValid = 0x84, kInfoReverse = 0x86, kInfoNumber = 0x8A,
                 kInfoSameAs = 0x8C, kInfoMode = 0x94;
constexpr u32 kModeFreeRun = 0x10;
constexpr size_t kInfoMaxTraffic = 0xEC;    // u8[8]

constexpr u32 kVisibilityContainer = 0x80034150, kVisibilityLists = 0x00034153;
constexpr u8 kListMarker[8] = { 0x0B, 0, 0, 0, 0x0B, 0, 0, 0 };

// Section numbers are the letter's place in the alphabet times 100 plus the
// digits (A1 = 101, X1 = 2401); X, Y and Z are the libraries every zone shares.
bool LibrarySection(u32 number) { return number >= 2400 && number < 2700; }

u32 SectionNumber(const std::wstring& name) {
    if (name.size() < 2 || name[0] < L'A' || name[0] > L'Z') return 0;
    for (size_t i = 1; i < name.size(); ++i)
        if (name[i] < L'0' || name[i] > L'9') return 0;
    return (u32)(name[0] - L'A' + 1) * 100 + (u32)_wtoi(name.c_str() + 1);
}

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

bool WriteWhole(const std::wstring& path, const std::vector<u8>& bytes) {
    FILE* file = nullptr;
    if (::_wfopen_s(&file, path.c_str(), L"wb") != 0 || !file) return false;
    const bool ok = std::fwrite(bytes.data(), 1, bytes.size(), file) == bytes.size();
    return std::fclose(file) == 0 && ok;
}

bool Exists(const std::wstring& path) { return ::GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES; }

u32 Get32(const u8* p) { u32 v; std::memcpy(&v, p, 4); return v; }
u16 Get16(const u8* p) { u16 v; std::memcpy(&v, p, 2); return v; }

u32 BinHash(const std::string& text) {
    u32 h = 0xFFFFFFFFu;
    for (unsigned char c : text) h = h * 33u + c;
    return h;
}

std::string Narrow(const std::wstring& text) {
    std::string out(text.size(), '\0');
    for (size_t i = 0; i < text.size(); ++i) out[i] = (char)text[i];
    return out;
}

void PutString(std::vector<u8>& record, size_t at, size_t room, const std::string& text) {
    std::memset(record.data() + at, 0, room);
    std::memcpy(record.data() + at, text.data(), std::min(text.size(), room - 1));
}

std::string GetString(const std::vector<u8>& record, size_t at, size_t room) {
    std::string out;
    for (size_t i = 0; i < room && record[at + i]; ++i) out += (char)record[at + i];
    return out;
}

// Every TrackInfo entry of the shipped GLOBALB, wherever 0x34201 sits.
void FindTrackInfo(const u8* data, size_t size, std::vector<std::vector<u8>>& out) {
    size_t at = 0;
    while (at + 8 <= size) {
        const u32 id = Get32(data + at), length = Get32(data + at + 4);
        if (at + 8 + (size_t)length > size) break;
        if (id == kTrackInfoChunk)
            for (size_t r = 0; r + kTrackInfoSize <= length; r += kTrackInfoSize)
                out.emplace_back(data + at + 8 + r, data + at + 8 + r + kTrackInfoSize);
        else if (id & 0x80000000u)
            FindTrackInfo(data + at + 8, length, out);
        at += 8 + (size_t)length;
    }
}

std::vector<std::vector<u8>> ShippedTrackInfo(const std::wstring& gameFolder) {
    std::vector<std::vector<u8>> out;
    const std::vector<u8> global = ReadWhole(gameFolder + L"\\GLOBAL\\GLOBALB.BUN");
    if (!global.empty()) FindTrackInfo(global.data(), global.size(), out);
    return out;
}

// The source region's free-roam track: its lowest-numbered entry (4000 for
// L4RA, 4200 for L4RB...), whose map shows the whole region.
bool TemplateTrack(const std::vector<std::vector<u8>>& infos, const std::wstring& source, std::vector<u8>& out) {
    u16 best = 0xFFFF;
    for (const auto& info : infos) {
        if (GetString(info, kInfoRegionName, 8) != Narrow(source)) continue;
        const u16 number = Get16(info.data() + kInfoNumber);
        if (number < best) { best = number; out = info; }
    }
    return best != 0xFFFF;
}

// The served copy of a region's route file when the editor wrote one, else the shipped (source's) file.
std::wstring Effective(const std::wstring& gameFolder, const std::wstring& region, const std::wstring& file) {
    const std::wstring served = gameFolder + L"\\models\\files\\TRACKS\\ROUTES" + region + L"\\" + file;
    if (Exists(served)) return served;
    return nfs::ShippedTrackFile(gameFolder, region, L"ROUTES" + region + L"\\" + file);
}

bool IsStartMarker(u32 hash) {
    static const char* const names[] = { "ForwardStartingPosition", "ForwardStartLine", "BackwardStartingPosition",
                                         "BackwardStartLine", "ForwardFinishLine", "BackwardFinishLine" };
    for (const char* n : names)
        if (BinHash(n) == hash) return true;
    return false;
}

// 0x34146 records of a TrackPosMarkers file: the chunk header, 0x11 up to a
// 16-byte boundary, then 0x30-byte records.
bool ReadMarkers(const std::vector<u8>& bytes, std::vector<std::vector<u8>>& records) {
    records.clear();
    if (bytes.size() < 8 || Get32(bytes.data()) != kMarkerChunk) return false;
    const size_t end = std::min(bytes.size(), 8 + (size_t)Get32(bytes.data() + 4));
    size_t p = 8;
    while (p < end && bytes[p] == 0x11) ++p;
    for (; p + kMarkerSize <= end; p += kMarkerSize) records.emplace_back(bytes.begin() + (ptrdiff_t)p, bytes.begin() + (ptrdiff_t)(p + kMarkerSize));
    return true;
}

std::vector<u8> WriteMarkers(const std::vector<std::vector<u8>>& records) {
    std::vector<u8> out(16, 0x11);
    const u32 id = kMarkerChunk, size = (u32)(8 + records.size() * kMarkerSize);
    std::memcpy(out.data(), &id, 4);
    std::memcpy(out.data() + 4, &size, 4);
    for (const auto& r : records) out.insert(out.end(), r.begin(), r.end());
    return out;
}

std::vector<u16> RaceNumbers(const std::wstring& gameFolder) {
    std::vector<u16> out;
    const std::wstring folder = gameFolder + L"\\models\\races\\";
    WIN32_FIND_DATAW found{};
    HANDLE find = ::FindFirstFileW((folder + L"*.frsrace").c_str(), &found);
    if (find == INVALID_HANDLE_VALUE) return out;
    do {
        std::ifstream in(folder + found.cFileName);
        std::string line;
        while (std::getline(in, line)) {
            std::istringstream words(line);
            std::string key;
            unsigned number = 0;
            if (words >> key && key == "number" && words >> number) out.push_back((u16)number);
        }
    } while (::FindNextFileW(find, &found));
    ::FindClose(find);
    return out;
}

} // namespace

std::vector<u16> UsedTrackNumbers(const std::wstring& gameFolder) {
    std::vector<u16> used;
    for (const auto& info : ShippedTrackInfo(gameFolder)) used.push_back(Get16(info.data() + kInfoNumber));
    for (u16 n : RaceNumbers(gameFolder)) used.push_back(n);
    for (const nfs::CustomRegion& r : nfs::ReadCustomRegions(gameFolder)) used.push_back(r.track);
    return used;
}

bool CreateRegion(const std::wstring& gameFolder, const NewRegion& request, nfs::CustomRegion& made,
                  std::wstring& message) {
    if (!Exists(gameFolder + L"\\TRACKS\\STREAM" + request.source + L".BUN")) {
        message = L"Only a region the game ships can be copied (" + request.source + L" is not one).";
        return false;
    }
    const auto infos = ShippedTrackInfo(gameFolder);
    std::vector<u8> info;
    if (!TemplateTrack(infos, request.source, info)) {
        message = L"GLOBALB has no track in " + request.source + L" to start the new one from.";
        return false;
    }
    const u16 templateNumber = Get16(info.data() + kInfoNumber);

    // A name: L4R + a letter no region uses. Never G: the game gives L4RG a
    // smaller streaming pool (RegionLoader::FinishedLoading 0x57A580).
    const auto customs = nfs::ReadCustomRegions(gameFolder);
    std::wstring name;
    for (wchar_t letter : std::wstring(L"EIJKLMNOPQSTUVWXYZ")) {
        const std::wstring candidate = std::wstring(L"L4R") + letter;
        if (Exists(gameFolder + L"\\TRACKS\\STREAM" + candidate + L".BUN")) continue;
        if (std::any_of(customs.begin(), customs.end(), [&](const nfs::CustomRegion& c) { return c.name == candidate; }))
            continue;
        name = candidate;
        break;
    }
    if (name.empty()) {
        message = L"Every region name is taken.";
        return false;
    }
    const std::vector<u16> used = UsedTrackNumbers(gameFolder);
    u16 number = kFirstTrack;
    while (std::find(used.begin(), used.end(), number) != used.end()) ++number;
    if (number > 9999) {
        message = L"No free track number is left.";
        return false;
    }

    std::string title = request.title.empty() ? "FRS " + Narrow(name) : request.title;
    const std::string digits = std::to_string(number);
    PutString(info, kInfoDescription, 32, title);
    PutString(info, kInfoTrackDirectory, 32, "Location4\\Track_" + digits);
    PutString(info, kInfoRegionName, 8, Narrow(name));
    std::string directory = GetString(info, kInfoRegionDirectory, 32);   // Location4\RegionA
    if (!directory.empty()) directory.back() = (char)name.back();
    else directory = "Location4\\Region" + std::string(1, (char)name.back());
    PutString(info, kInfoRegionDirectory, 32, directory);
    info[kInfoValid] = 1;
    info[kInfoReverse] = 0;
    std::memcpy(info.data() + kInfoNumber, &number, 2);
    std::memcpy(info.data() + kInfoSameAs, &number, 2);
    const u32 mode = kModeFreeRun;
    std::memcpy(info.data() + kInfoMode, &mode, 4);
    // An empty region keeps the source's road graph only for resets: no
    // traffic on roads that are not there.
    if (request.empty) std::memset(info.data() + kInfoMaxTraffic, 0, 8);

    made = nfs::CustomRegion{};
    made.empty = request.empty;
    made.terrainPreset = request.preset;
    made.terrainSize = request.size;
    made.terrainSeed = request.seed;
    made.name = name;
    made.source = request.source;
    made.track = number;
    made.startFrom = templateNumber;
    made.title = title;
    made.trackInfo = info;
    if (!nfs::WriteCustomRegion(gameFolder, made)) {
        message = L"Could not write " + nfs::CustomRegionFolder(gameFolder) + L"\\" + name + L".frsregion";
        return false;
    }
    if (!SyncRegionFiles(gameFolder, made, message)) return false;
    LOG_INFO("regions: %S made from %S, track %u (\"%s\", start from %u)", name.c_str(), request.source.c_str(),
             number, title.c_str(), templateNumber);
    message = L"Region " + name + L" made from " + request.source + L": track " + std::to_wstring(number) +
              L" in Quick Race > Free Run.";
    return true;
}

bool SyncRegionFiles(const std::wstring& gameFolder, const nfs::CustomRegion& region, std::wstring& message) {
    const std::wstring tracks = gameFolder + L"\\models\\files\\TRACKS";
    const std::wstring routes = tracks + L"\\ROUTES" + region.name;
    for (const std::wstring& folder : { gameFolder + L"\\models", gameFolder + L"\\models\\files", tracks, routes })
        ::CreateDirectoryW(folder.c_str(), nullptr);
    const std::wstring n = std::to_wstring(region.track);

    const std::vector<u8> graph = ReadWhole(Effective(gameFolder, region.name, L"RoutesFreeRoam.bin"));
    const std::vector<u8> zones = ReadWhole(Effective(gameFolder, region.name, L"PathsFreeRoam.bin"));
    if (graph.empty() || zones.empty()) {
        message = L"Could not read the free-roam routes of " + region.source + L".";
        return false;
    }
    bool ok = WriteWhole(routes + L"\\Routes" + n + L"F.bin", graph) &&
              WriteWhole(routes + L"\\Routes" + n + L"B.bin", graph) &&
              WriteWhole(routes + L"\\Paths" + n + L".bin", zones);

    // The start grid: the template track's, renumbered, over whatever marker
    // file the region has now.
    const u16 from = region.startFrom ? region.startFrom : 4000;
    std::vector<std::vector<u8>> shipped, records;
    ReadMarkers(ReadWhole(nfs::ShippedTrackFile(gameFolder, region.name, L"ROUTES" + region.name + L"\\TrackPosMarkersAll.bin")),
                shipped);
    ReadMarkers(ReadWhole(Effective(gameFolder, region.name, L"TrackPosMarkersAll.bin")), records);
    records.erase(std::remove_if(records.begin(), records.end(),
                                 [&](const std::vector<u8>& r) { return Get32(r.data() + 0x24) == region.track; }),
                  records.end());
    size_t grid = 0;
    for (const auto& r : shipped) {
        if (Get32(r.data() + 0x24) != from || !IsStartMarker(Get32(r.data() + 8))) continue;
        std::vector<u8> copy = r;
        const u32 track = region.track;
        std::memcpy(copy.data() + 0x24, &track, 4);
        records.push_back(copy);
        ++grid;
    }
    if (!grid) {
        message = L"Track " + std::to_wstring(from) + L" has no start grid to copy.";
        return false;
    }
    ok = ok && WriteWhole(routes + L"\\TrackPosMarkersAll.bin", WriteMarkers(records));

    // The map: the template's, its texture renamed (same-length digits).
    std::vector<u8> map = ReadWhole(gameFolder + L"\\TRACKS\\TRACKMAP" + std::to_wstring(from) + L".BIN");
    const std::string oldDigits = std::to_string(from), newDigits = std::to_string(region.track);
    if (map.size() > 0x200 && oldDigits.size() == newDigits.size()) {
        const u32 oldHash = BinHash("TRACKMAP" + oldDigits), newHash = BinHash("TRACKMAP" + newDigits);
        const std::string oldName = "TRACKMAP" + oldDigits, newName = "TRACKMAP" + newDigits;
        for (size_t i = 0; i + oldName.size() <= 0x200; ++i)
            if (std::memcmp(map.data() + i, oldName.data(), oldName.size()) == 0)
                std::memcpy(map.data() + i, newName.data(), newName.size());
        for (size_t i = 0; i + 4 <= 0x200; ++i)
            if (std::memcmp(map.data() + i, &oldHash, 4) == 0) std::memcpy(map.data() + i, &newHash, 4);
        ok = ok && WriteWhole(tracks + L"\\TRACKMAP" + n + L".BIN", map);
    }
    if (!ok) {
        message = L"Could not write the files of track " + n + L" in " + routes;
        return false;
    }
    // An empty region's own region file: its zone lists cut (with the fog,
    // through the one writer every edit of that file goes through).
    if (region.empty) {
        std::wstring written;
        if (!WriteFogToGame(gameFolder, region.name, written)) {
            message = written;
            return false;
        }
    }
    LOG_INFO("regions: %S track %u files written (start grid of %u, %zu markers)", region.name.c_str(), region.track,
             from, grid);
    return true;
}

bool RegionStart(const std::wstring& gameFolder, const nfs::CustomRegion& region, Vec3& at) {
    std::vector<std::vector<u8>> records;
    ReadMarkers(ReadWhole(Effective(gameFolder, region.name, L"TrackPosMarkersAll.bin")), records);
    const u16 from = region.startFrom ? region.startFrom : 4000;
    const u32 grid = BinHash("ForwardStartingPosition"), line = BinHash("ForwardStartLine");
    bool found = false;
    for (const auto& r : records) {
        const u32 track = Get32(r.data() + 0x24);
        if (track != region.track && track != from) continue;
        const u32 hash = Get32(r.data() + 8);
        if (hash != grid && !(hash == line && !found)) continue;
        std::memcpy(&at.x, r.data() + 0x10, 4);
        std::memcpy(&at.y, r.data() + 0x14, 4);
        std::memcpy(&at.z, r.data() + 0x18, 4);
        found = true;
        if (hash == grid && Get32(r.data() + 0x0C) == 0) return true;
    }
    return found;
}

bool ApplyEmptyRegionLists(std::vector<u8>& layout, const std::wstring& gameFolder, const std::wstring& region,
                           std::wstring& error) {
    nfs::CustomRegion custom;
    if (!nfs::FindCustomRegion(gameFolder, region, custom) || !custom.empty) return true;

    // The sections the editor wrote for the region.
    std::vector<u32> kept;
    const std::wstring models = gameFolder + L"\\models\\";
    WIN32_FIND_DATAW found{};
    HANDLE find = ::FindFirstFileW((models + region + L"-*-stream.frssection").c_str(), &found);
    if (find != INVALID_HANDLE_VALUE) {
        do {
            std::wstring name = found.cFileName;                           // L4RE-M12-stream.frssection
            name = name.substr(region.size() + 1);
            name = name.substr(0, name.find(L'-'));
            if (const u32 number = SectionNumber(name)) kept.push_back(number);
        } while (::FindNextFileW(find, &found));
        ::FindClose(find);
    }

    // The shipped lists, rewritten in place over the same records of `layout`
    // (nothing in front of the parameter maps ever moves).
    const std::vector<u8> shipped = ReadWhole(nfs::ShippedTrackFile(gameFolder, region, region + L".BUN"));
    size_t at = 0;
    while (at + 8 <= shipped.size() && Get32(shipped.data() + at) != kVisibilityContainer)
        at += 8 + (size_t)Get32(shipped.data() + at + 4);
    if (at + 8 > shipped.size() || at + 8 > layout.size()) {
        error = L"The region file has no visibility lists.";
        return false;
    }
    const size_t containerEnd = at + 8 + (size_t)Get32(shipped.data() + at + 4);
    size_t child = at + 8;
    size_t zones = 0, entries = 0;
    while (child + 8 <= containerEnd) {
        const u32 id = Get32(shipped.data() + child), length = Get32(shipped.data() + child + 4);
        const size_t body = child + 8, end = body + length;
        if (id == kVisibilityLists && end <= layout.size()) {
            std::vector<size_t> starts;
            for (size_t p = body; p + 8 <= end; p += 4)
                if (std::memcmp(shipped.data() + p, kListMarker, 8) == 0) starts.push_back(p + 8);
            for (size_t k = 0; k < starts.size(); ++k) {
                const size_t record = starts[k];
                const size_t recordEnd = k + 1 < starts.size() ? starts[k + 1] - 8 : end;
                if (record + 12 > recordEnd) continue;
                const u32 count = Get32(shipped.data() + record + 8);
                if (record + 12 + (size_t)count * 2 > recordEnd) continue;
                std::vector<u16> list;
                for (u32 i = 0; i < count; ++i) {
                    const u16 section = Get16(shipped.data() + record + 12 + i * 2);
                    if (LibrarySection(section) || std::find(kept.begin(), kept.end(), section) != kept.end())
                        list.push_back(section);
                }
                const u32 newCount = (u32)list.size();
                std::memcpy(layout.data() + record + 8, &newCount, 4);
                std::memset(layout.data() + record + 12, 0, (size_t)count * 2);
                if (!list.empty()) std::memcpy(layout.data() + record + 12, list.data(), list.size() * 2);
                ++zones;
                entries += list.size();
            }
        }
        child = end;
    }
    LOG_INFO("regions: %S is empty: %zu zone lists cut to the libraries and %zu written sections (%zu entries)",
             region.c_str(), zones, kept.size(), entries);

    // The city's moving objects -- the jumbo planes, trams, doors, spotlights
    // (world animations: placements 0x37090 and 0x37150, scene nodes 0x37080,
    // controllers 0x37110, animation banks 0xE34010) -- turned into padding of
    // their size. A shop the editor placed (io/MarkerWrite) adds placements of
    // its own, which differ from every shipped one: those stay, and then the
    // scenes their doors play do too. The game makes its animation pools on the
    // first placement it meets, and a controller before any placement crashes
    // it (0x440D43), so the scenes go only when no placement is left.
    const auto fingerprint = [](const u8* data, size_t size) {
        u64 h = 1469598103934665603ull;
        for (size_t i = 0; i < size; ++i) { h ^= data[i]; h *= 1099511628211ull; }
        return h ^ ((u64)size << 40);
    };
    const auto animation = [](u32 id) {
        return id == 0x37090 || id == 0x37150 || id == 0x37080 || id == 0x37110 || id == 0x00E34010;
    };
    std::vector<u64> shippedChunks;
    for (size_t p = 0; p + 8 <= shipped.size();) {
        const u32 id = Get32(shipped.data() + p), size = Get32(shipped.data() + p + 4);
        if (p + 8 + (size_t)size > shipped.size()) break;
        if (animation(id)) shippedChunks.push_back(fingerprint(shipped.data() + p, 8 + (size_t)size));
        p += 8 + (size_t)size;
    }
    std::vector<size_t> placements, scenes;
    bool ownPlacements = false;
    for (size_t p = 0; p + 8 <= layout.size();) {
        const u32 id = Get32(layout.data() + p), size = Get32(layout.data() + p + 4);
        if (p + 8 + (size_t)size > layout.size()) break;
        if (animation(id)) {
            const bool fromCity = std::find(shippedChunks.begin(), shippedChunks.end(),
                                            fingerprint(layout.data() + p, 8 + (size_t)size)) != shippedChunks.end();
            const bool placement = id == 0x37090 || id == 0x37150;
            if (placement && !fromCity) ownPlacements = true;
            if (fromCity) (placement ? placements : scenes).push_back(p);
        }
        p += 8 + (size_t)size;
    }
    const u32 padding = 0;
    for (size_t p : placements) std::memcpy(layout.data() + p, &padding, 4);
    if (!ownPlacements)
        for (size_t p : scenes) std::memcpy(layout.data() + p, &padding, 4);
    LOG_INFO("regions: %S is empty: %zu of the city's moving-object placements and %zu scene chunks taken out",
             region.c_str(), placements.size(), ownPlacements ? (size_t)0 : scenes.size());
    return true;
}

bool RemoveRegion(const std::wstring& gameFolder, const nfs::CustomRegion& region, std::wstring& message) {
    const std::wstring tracks = gameFolder + L"\\models\\files\\TRACKS";
    const std::wstring routes = tracks + L"\\ROUTES" + region.name;
    WIN32_FIND_DATAW found{};
    HANDLE find = ::FindFirstFileW((routes + L"\\*").c_str(), &found);
    if (find != INVALID_HANDLE_VALUE) {
        do {
            if (!(found.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) ::DeleteFileW((routes + L"\\" + found.cFileName).c_str());
        } while (::FindNextFileW(find, &found));
        ::FindClose(find);
    }
    ::RemoveDirectoryW(routes.c_str());
    ::DeleteFileW((tracks + L"\\" + region.name + L".BUN").c_str());
    ::DeleteFileW((tracks + L"\\TRACKMAP" + std::to_wstring(region.track) + L".BIN").c_str());
    // The region's own edits in models: <R>-... overlays and ...-<R>.txt sources.
    const std::wstring models = gameFolder + L"\\models";
    find = ::FindFirstFileW((models + L"\\*").c_str(), &found);
    size_t removed = 0;
    if (find != INVALID_HANDLE_VALUE) {
        do {
            if (found.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
            std::wstring upper = found.cFileName;
            for (wchar_t& c : upper) c = (wchar_t)::towupper(c);
            const bool owned = upper.rfind(region.name + L"-", 0) == 0 || upper.find(L"-" + region.name + L".") != std::wstring::npos;
            if (owned && ::DeleteFileW((models + L"\\" + found.cFileName).c_str())) ++removed;
        } while (::FindNextFileW(find, &found));
        ::FindClose(find);
    }
    nfs::DeleteCustomRegion(gameFolder, region.name);
    message = L"Region " + region.name + L" removed (" + std::to_wstring(removed) + L" edit files with it).";
    LOG_INFO("regions: %S removed, %zu edit files", region.name.c_str(), removed);
    return true;
}

} // namespace io
