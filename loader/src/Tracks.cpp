// New track numbers: the race editor's races and the editor's new regions.
//
// The editor keeps each race it made in <game>\models\races\<n>.frsrace and
// each region it made in <game>\models\regions\<R>.frsregion; one with a
// track number the game does not have carries its whole TrackInfo entry there
// ("trackinfo <hex>"). Their files -- Paths<n>.bin, Routes<n>F/B.bin, the
// rebuilt TrackPosMarkersAll.bin, TRACKMAP<n>.BIN -- reach the game through
// models\files like any other replaced file, and a region's own name is an
// alias of the region it copies (FileEdits.cpp). What the files cannot do is
// done here (docs/research/new-races.md):
//
//   0x5D3EDE  the end of the TrackInfo chunk loader (0x34201): the table
//             is copied into a larger one with the new entries, and the
//             pointer 0x883DA0 / count 0x883DA4 set to it.
//   0x5143C0  PlayerCareerState::IsTrackUnlockedForQuickRace: a new track is
//             unlocked (no career event owns it, so the game would padlock it).
//   0x4FF9D0  SearchForString: TRACK_NAME_<n>, REVERSE_TRACK_NAME_<n> and
//             QR_TRACK_<n>_COMMENT answered with the track's own name.

#include "Loader.h"

#include <cstring>
#include <fstream>
#include <sstream>
#include <unordered_map>
#include <vector>

namespace frs {

namespace {

constexpr size_t kTrackInfoSize = 0x128;
constexpr uintptr_t kTrackInfoTable = 0x883DA0, kTrackInfoCount = 0x883DA4;
constexpr uintptr_t kLoaderEnd = 0x5D3EDE;
constexpr uintptr_t kUnlockTest = 0x5143C0;   // resumes at 0x5143C6
constexpr uintptr_t kSearchString = 0x4FF9D0;  // resumes at 0x4FF9D7

struct Race {
    int number = 0;
    std::vector<unsigned char> info;   // 0x128 bytes
    std::string name;                  // cp1252
};

std::vector<Race> g_races;
std::unordered_map<unsigned, std::string> g_strings;   // bin hash -> cp1252 text

unsigned BinHash(const std::string& text) {
    unsigned h = 0xFFFFFFFFu;
    for (unsigned char c : text) h = h * 33 + c;
    return h;
}

std::string Cp1252(const std::string& utf8) {
    const int wide = ::MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), (int)utf8.size(), nullptr, 0);
    std::wstring w((size_t)wide, L'\0');
    ::MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), (int)utf8.size(), w.data(), wide);
    const int narrow = ::WideCharToMultiByte(1252, 0, w.c_str(), (int)w.size(), nullptr, 0, "?", nullptr);
    std::string out((size_t)narrow, '\0');
    ::WideCharToMultiByte(1252, 0, w.c_str(), (int)w.size(), out.data(), narrow, "?", nullptr);
    return out;
}

void ReadTracks(const wchar_t* subfolder, const wchar_t* pattern, const char* magic, const char* numberKey,
                const char* comment) {
    Inside inside;
    const std::wstring folder = GameFolder() + L"\\models\\" + subfolder + L"\\";
    WIN32_FIND_DATAW found{};
    HANDLE find = ::FindFirstFileW((folder + pattern).c_str(), &found);
    if (find == INVALID_HANDLE_VALUE) return;
    do {
        std::ifstream in(folder + found.cFileName);
        std::string line;
        if (!std::getline(in, line) || line.rfind(magic, 0) != 0) continue;
        Race race;
        while (std::getline(in, line)) {
            std::istringstream words(line);
            std::string key;
            words >> key;
            if (key == numberKey) words >> race.number;
            else if (key == "name") std::getline(words >> std::ws, race.name);
            else if (key == "trackinfo") {
                std::string hex;
                words >> hex;
                for (size_t i = 0; i + 1 < hex.size(); i += 2)
                    race.info.push_back((unsigned char)std::stoi(hex.substr(i, 2), nullptr, 16));
            }
        }
        // Only new numbers carry an entry; an edited shipped race needs none.
        if (race.number <= 0 || race.info.size() != kTrackInfoSize) continue;
        race.name = Cp1252(race.name);
        const std::string n = std::to_string(race.number);
        g_strings[BinHash("TRACK_NAME_" + n)] = race.name;
        g_strings[BinHash("REVERSE_TRACK_NAME_" + n)] = race.name + " (reverse)";
        g_strings[BinHash("QR_TRACK_" + n + "_COMMENT")] = comment;
        Log("tracks: %d \"%s\" from %S", race.number, race.name.c_str(), found.cFileName);
        g_races.push_back(std::move(race));
    } while (::FindNextFileW(find, &found));
    ::FindClose(find);
}

void ReadRaces() {
    ReadTracks(L"races", L"*.frsrace", "FRSRACE", "number", "A race made with the FRS World Editor.");
    ReadTracks(L"regions", L"*.frsregion", "FRSREGION", "track", "A region made with the FRS World Editor.");
}

// ---- 0x5D3EDE: the TrackInfo table, with the new entries

void __cdecl AppendTrackInfo() {
    const unsigned char* table = Peek<const unsigned char*>(kTrackInfoTable);
    const unsigned count = Peek<unsigned>(kTrackInfoCount);
    if (!table || !count || g_races.empty()) return;
    // Kept for the whole session: the game reads the table until it quits.
    unsigned char* grown = (unsigned char*)::HeapAlloc(::GetProcessHeap(), 0, (count + g_races.size()) * kTrackInfoSize);
    if (!grown) return;
    std::memcpy(grown, table, count * kTrackInfoSize);
    unsigned total = count;
    for (const Race& race : g_races) {
        unsigned at = total;
        for (unsigned i = 0; i < count; ++i)   // a shipped number: replaced in place
            if (*(const short*)(grown + i * kTrackInfoSize + 0x8A) == (short)race.number) { at = i; break; }
        std::memcpy(grown + at * kTrackInfoSize, race.info.data(), kTrackInfoSize);
        if (at == total) ++total;
    }
    *(unsigned char**)kTrackInfoTable = grown;
    *(unsigned*)kTrackInfoCount = total;
    Log("tracks: TrackInfo table %u -> %u entries", count, total);
}

__declspec(naked) void LoaderEndHook() {
    __asm {
        pushad
        call AppendTrackInfo
        popad
        mov eax, 1
        ret
    }
}

// ---- 0x5143C0: unlocked in Quick Race

int __cdecl IsCustomRace(int number) {
    for (const Race& race : g_races)
        if (race.number == (short)number) return 1;
    return 0;
}

__declspec(naked) void UnlockHook() {
    __asm {
        push ecx
        push edx
        push dword ptr [esp + 0xC]   // the track (the first argument, past the two pushes)
        call IsCustomRace
        add esp, 4
        pop edx
        pop ecx
        test eax, eax
        jz original
        mov al, 1
        ret 8
    original:
        push esi
        push edi
        mov edi, dword ptr [esp + 0xC]
        push 0x5143C6
        ret
    }
}

// ---- 0x4FF9D0: the race's strings (edx = hash, eax = the text or 0)

const char* __cdecl FindRaceString(unsigned hash) {
    auto it = g_strings.find(hash);
    return it == g_strings.end() ? nullptr : it->second.c_str();
}

__declspec(naked) void SearchStringHook() {
    __asm {
        push ecx
        push edx
        push edx
        call FindRaceString
        add esp, 4
        pop edx
        pop ecx
        test eax, eax
        jz original
        ret
    original:
        push ebx
        mov ebx, dword ptr ds:[0x8383D8]
        push 0x4FF9D7
        ret
    }
}

bool Expect(uintptr_t at, const unsigned char* bytes, size_t size, const char* what) {
    if (std::memcmp((const void*)at, bytes, size) == 0) return true;
    Log("tracks: %s at 0x%06X is not the expected code (another mod?); not hooked", what, (unsigned)at);
    return false;
}

} // namespace

void StartTracks() {
    ReadRaces();
    if (g_races.empty()) return;
    static const unsigned char loaderEnd[] = { 0xB8, 0x01, 0x00, 0x00, 0x00, 0xC3 };
    static const unsigned char unlock[] = { 0x56, 0x57, 0x8B, 0x7C, 0x24, 0x0C };
    static const unsigned char search[] = { 0x53, 0x8B, 0x1D, 0xD8, 0x83, 0x83, 0x00 };
    int hooks = 0;
    if (Expect(kLoaderEnd, loaderEnd, sizeof(loaderEnd), "the TrackInfo loader")) {
        PatchJump((unsigned char*)kLoaderEnd, sizeof(loaderEnd), (const void*)&LoaderEndHook);
        ++hooks;
    }
    if (Expect(kUnlockTest, unlock, sizeof(unlock), "the Quick Race unlock test")) {
        PatchJump((unsigned char*)kUnlockTest, sizeof(unlock), (const void*)&UnlockHook);
        ++hooks;
    }
    if (Expect(kSearchString, search, sizeof(search), "the string lookup")) {
        PatchJump((unsigned char*)kSearchString, sizeof(search), (const void*)&SearchStringHook);
        ++hooks;
    }
    Log("tracks: %zu new tracks, %d of 3 hooks in", g_races.size(), hooks);
}

} // namespace frs
