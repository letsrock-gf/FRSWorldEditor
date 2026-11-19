#include "nfs/CustomRegions.h"

#include <windows.h>

#include <cstdio>
#include <fstream>
#include <sstream>

namespace nfs {

namespace {

std::wstring Widen(const std::string& text) {
    std::wstring out(text.size(), L'\0');
    for (size_t i = 0; i < text.size(); ++i) out[i] = (wchar_t)(unsigned char)text[i];
    return out;
}

std::string Narrow(const std::wstring& text) {
    std::string out(text.size(), '\0');
    for (size_t i = 0; i < text.size(); ++i) out[i] = (char)text[i];
    return out;
}

std::wstring Upper(std::wstring text) {
    for (wchar_t& c : text) c = (wchar_t)::towupper(c);
    return text;
}

bool ReadOne(const std::wstring& path, CustomRegion& out) {
    std::ifstream in(path);
    std::string line;
    if (!std::getline(in, line) || line.rfind("FRSREGION", 0) != 0) return false;
    out = CustomRegion{};
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        std::istringstream words(line);
        std::string key;
        words >> key;
        if (key == "region") { std::string v; words >> v; out.name = Upper(Widen(v)); }
        else if (key == "source") { std::string v; words >> v; out.source = Upper(Widen(v)); }
        else if (key == "track") { unsigned v = 0; words >> v; out.track = (u16)v; }
        else if (key == "start") { unsigned v = 0; words >> v; out.startFrom = (u16)v; }
        else if (key == "name") std::getline(words >> std::ws, out.title);
        else if (key == "kind") { std::string v; words >> v; out.empty = v == "empty"; }
        else if (key == "terrain") words >> out.terrainPreset >> out.terrainSize >> out.terrainSeed;
        else if (key == "trackinfo") {
            std::string hex;
            words >> hex;
            out.trackInfo.clear();
            for (size_t i = 0; i + 1 < hex.size(); i += 2)
                out.trackInfo.push_back((u8)std::stoi(hex.substr(i, 2), nullptr, 16));
        }
    }
    return !out.name.empty() && !out.source.empty() && out.name != out.source;
}

} // namespace

std::wstring CustomRegionFolder(const std::wstring& gameFolder) { return gameFolder + L"\\models\\regions"; }

std::vector<CustomRegion> ReadCustomRegions(const std::wstring& gameFolder) {
    std::vector<CustomRegion> out;
    const std::wstring folder = CustomRegionFolder(gameFolder);
    WIN32_FIND_DATAW found{};
    HANDLE find = ::FindFirstFileW((folder + L"\\*.frsregion").c_str(), &found);
    if (find == INVALID_HANDLE_VALUE) return out;
    do {
        CustomRegion region;
        if (ReadOne(folder + L"\\" + found.cFileName, region)) out.push_back(std::move(region));
    } while (::FindNextFileW(find, &found));
    ::FindClose(find);
    return out;
}

bool FindCustomRegion(const std::wstring& gameFolder, const std::wstring& region, CustomRegion& out) {
    return ReadOne(CustomRegionFolder(gameFolder) + L"\\" + Upper(region) + L".frsregion", out);
}

bool WriteCustomRegion(const std::wstring& gameFolder, const CustomRegion& region) {
    ::CreateDirectoryW((gameFolder + L"\\models").c_str(), nullptr);
    ::CreateDirectoryW(CustomRegionFolder(gameFolder).c_str(), nullptr);
    std::ofstream out(CustomRegionFolder(gameFolder) + L"\\" + Upper(region.name) + L".frsregion", std::ios::trunc);
    if (!out) return false;
    out << "FRSREGION 1\n";
    out << "region " << Narrow(region.name) << "\n";
    out << "source " << Narrow(region.source) << "\n";
    out << "track " << region.track << "\n";
    if (region.startFrom) out << "start " << region.startFrom << "\n";
    out << "name " << region.title << "\n";
    if (region.empty) {
        out << "kind empty\n";
        out << "terrain " << region.terrainPreset << " " << region.terrainSize << " " << region.terrainSeed << "\n";
    }
    out << "trackinfo ";
    char hex[3];
    for (u8 b : region.trackInfo) {
        std::snprintf(hex, sizeof hex, "%02X", b);
        out << hex;
    }
    out << "\n";
    return (bool)out;
}

bool DeleteCustomRegion(const std::wstring& gameFolder, const std::wstring& region) {
    return ::DeleteFileW((CustomRegionFolder(gameFolder) + L"\\" + Upper(region) + L".frsregion").c_str()) != FALSE;
}

std::wstring SourceRegion(const std::wstring& gameFolder, const std::wstring& region) {
    if (::GetFileAttributesW((gameFolder + L"\\TRACKS\\STREAM" + region + L".BUN").c_str()) != INVALID_FILE_ATTRIBUTES)
        return region;
    CustomRegion custom;
    if (FindCustomRegion(gameFolder, region, custom)) return custom.source;
    return region;
}

std::wstring ShippedTrackFile(const std::wstring& gameFolder, const std::wstring& region,
                              const std::wstring& relative) {
    const std::wstring source = SourceRegion(gameFolder, region);
    std::wstring path = relative;
    if (source != region && !region.empty()) {
        const std::wstring upperRegion = Upper(region);
        std::wstring upper = Upper(path);
        for (size_t at = upper.find(upperRegion); at != std::wstring::npos; at = upper.find(upperRegion, at + source.size())) {
            path.replace(at, region.size(), source);
            upper.replace(at, region.size(), source);
        }
    }
    return gameFolder + L"\\TRACKS\\" + path;
}

} // namespace nfs
