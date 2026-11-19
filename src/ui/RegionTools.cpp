#include "ui/RegionTools.h"

#include "core/Log.h"
#include "io/RegionCreate.h"
#include "io/RegionImport.h"
#include "terrain/Procedural.h"
#include "ui/Resource.h"
#include "nfs/World.h"

#include "imgui.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace {

std::string Narrow(const std::wstring& text) {
    std::string out;
    if (text.empty()) return out;
    const int size = ::WideCharToMultiByte(CP_UTF8, 0, text.c_str(), (int)text.size(), nullptr, 0, nullptr, nullptr);
    out.resize((size_t)size);
    ::WideCharToMultiByte(CP_UTF8, 0, text.c_str(), (int)text.size(), out.data(), size, nullptr, nullptr);
    return out;
}

std::wstring Widen(const std::string& text) {
    std::wstring out;
    if (text.empty()) return out;
    const int size = ::MultiByteToWideChar(CP_UTF8, 0, text.c_str(), (int)text.size(), nullptr, 0);
    out.resize((size_t)size);
    ::MultiByteToWideChar(CP_UTF8, 0, text.c_str(), (int)text.size(), out.data(), size);
    return out;
}

constexpr DWORD kFastTestRestoreMs = 30000;   // the game reads it as it starts

} // namespace

void RegionTools::OnCreate() {
    if (!host_.menu) return;
    for (int i = 0; i < ::GetMenuItemCount(host_.menu); ++i) {
        wchar_t text[64] = {};
        ::GetMenuStringW(host_.menu, (UINT)i, text, 63, MF_BYPOSITION);
        if (std::wstring(text).find(L"File") == std::wstring::npos) continue;
        if (HMENU file = ::GetSubMenu(host_.menu, i)) {
            // Before the last separator (Exit), as a block of its own.
            const int count = ::GetMenuItemCount(file);
            int at = count;
            for (int k = count - 1; k >= 0; --k)
                if (::GetMenuState(file, (UINT)k, MF_BYPOSITION) & MF_SEPARATOR) { at = k; break; }
            ::InsertMenuW(file, (UINT)at, MF_BYPOSITION | MF_SEPARATOR, 0, nullptr);
            ::InsertMenuW(file, (UINT)at + 1, MF_BYPOSITION | MF_STRING, kNewCommand, L"New &Region...");
            ::InsertMenuW(file, (UINT)at + 2, MF_BYPOSITION | MF_STRING, kRemoveCommand, L"Remove This Region...");
        }
        break;
    }
}

bool RegionTools::OnCommand(int id) {
    if (id == kNewCommand) {
        // The regions the game ships, to copy from: L4RA first.
        sources_.clear();
        const std::wstring game = Game();
        for (const std::wstring& r : nfs::World::AvailableRegions(game + L"\\TRACKS"))
            if (nfs::SourceRegion(game, r) == r && r.rfind(L"L4R", 0) == 0) sources_.push_back(r);
        source_ = 0;
        for (size_t i = 0; i < sources_.size(); ++i)
            if (sources_[i] == L"L4RA") source_ = (int)i;
        std::memset(title_, 0, sizeof title_);
        createMessage_.clear();
        asking_ = !sources_.empty();
        if (!asking_) Status(L"No region of the game to copy was found.");
        return true;
    }
    if (id == kRemoveCommand) { Remove(); return true; }
    return false;
}

void RegionTools::UpdateMenuChecks(HMENU menu) {
    ::EnableMenuItem(menu, kRemoveCommand, MF_BYCOMMAND | (isCustom_ ? MF_ENABLED : MF_GRAYED));
}

void RegionTools::OnRegionLoaded() {
    nfs::World* world = host_.world ? host_.world() : nullptr;
    isCustom_ = world && nfs::FindCustomRegion(Game(), world->Region(), open_);
    if (isCustom_)
        Status(L"Region " + open_.name + (open_.empty ? L" (empty" : L" (a copy of " + open_.source) + L"): track " +
               std::to_wstring(open_.track) + L", \"" + Widen(open_.title) + L"\" in Quick Race > Free Run.");
    // FRS_IMPORT_SELFTEST=<SOURCE>, and only with FRS_GAME_FOLDER: brings the
    // whole source region into the open one at the suggested place, as World >
    // Import Region will, and opens the region again to show it.
    if (!importTested_ && world && std::getenv("FRS_IMPORT_SELFTEST") && std::getenv("FRS_GAME_FOLDER") && !isCustom_) {
        importTested_ = true;
        const std::string name = std::getenv("FRS_IMPORT_SELFTEST");
        const std::wstring source(name.begin(), name.end());
        std::vector<io::RegionImport> imports;
        io::ReadImports(Game() + L"\\models", world->Region(), imports);
        bool present = false;
        for (const io::RegionImport& i : imports) present |= i.source == source;
        if (!present && source != world->Region()) {
            const io::RegionImport made = io::SuggestImport(Game(), world->Region(), source);
            if (made.firstLetter == 0) {
                LOG_WARN("import selftest: FAILED: nothing read for %S", source.c_str());
                return;
            }
            imports.push_back(made);
            io::WriteImports(Game() + L"\\models", world->Region(), imports);
            std::wstring message;
            const bool ok = io::BakeImport(*world, Game(), made, message);
            LOG_INFO("import selftest: %s: %S", ok ? "baked" : "FAILED", message.c_str());
            Status(message);
            importReopen_ = ok;
            return;
        }
    }
    // FRS_REGION_SELFTEST=1, and only with FRS_GAME_FOLDER (a scratch game
    // folder, never the player's): makes an empty region as File > New Region
    // would, and lets it run through generation and the bake.
    if (selfTest_ == 0 && std::getenv("FRS_REGION_SELFTEST") && std::getenv("FRS_GAME_FOLDER") && !isCustom_) {
        selfTest_ = 1;
        sources_ = { world ? world->Region() : std::wstring(L"L4RA") };
        source_ = 0;
        std::strcpy(title_, "Selftest Ground");
        empty_ = true;
        preset_ = 1;
        sizeChoice_ = std::atoi(std::getenv("FRS_REGION_SELFTEST")) == 4 ? 2 : 0;   // "4": the 4 km ground
        seed_ = 7;
        LOG_INFO("region selftest: making an empty region from %S", sources_[0].c_str());
        Create();
        return;
    }
    // An empty region: a section an import (the bake, a quick edit) rebuilt
    // from its bytes has the city back in it on disk -- written again without.
    if (world && world->EmptyCity()) {
        size_t pending = 0;
        for (const auto& section : world->Loaded()) pending += section->emptyPending;
        if (!pending) cleanupTries_ = 0;
        else if (cleanupTries_ < 2) cleanupDue_ = true;
        else LOG_WARN("regions: %zu sections of %S still carry the city after two saves", pending, open_.name.c_str());
    }
    if (!isCustom_ || open_.name != pendingName_) return;
    if (pendingStage_ == 1) {
        pendingStage_ = GenerateTerrain() ? 2 : 0;
    } else if (pendingStage_ == 3) {
        pendingStage_ = 0;
        // Roads > Bake (ui/RoadsExtension.cpp: ID_ROADS_FIRST + 6 = ID_EXTENSION_FIRST + 106):
        // the ground's meshes and collision into the region's sections.
        if (host_.window) ::PostMessageW(host_.window, WM_COMMAND, ID_EXTENSION_FIRST + 106, 0);
        if (selfTest_ == 1) { selfTest_ = 2; selfTestAt_ = ::GetTickCount() + 45000; }
        Status(L"Region " + open_.name + L": its ground is being baked into the game.");
    }
}

bool RegionTools::GenerateTerrain() {
    nfs::World* world = host_.world ? host_.world() : nullptr;
    if (!world) return false;
    terrain::ProceduralSettings settings;
    settings.preset = open_.terrainPreset;
    settings.size = open_.terrainSize;
    settings.seed = open_.terrainSeed;
    if (!io::RegionStart(Game(), open_, settings.start)) {
        Status(L"Region " + open_.name + L": no start grid to build the ground round.");
        return false;
    }
    // Only where the game streams something: outside every visibility zone
    // the whole region unloads and the car falls.
    const nfs::Preculler& zones = world->Visibility();
    const auto inside = [&](float x, float y) { return zones.IsEmpty() || zones.ZoneAt(x, y) >= 0; };
    terrain::Terrain ground;
    size_t patches = 0, cells = 0;
    if (!terrain::GenerateGround(settings, inside, ground, patches, cells)) {
        Status(L"Region " + open_.name + L": no ground could be laid round the start.");
        return false;
    }
    const std::wstring path = Game() + L"\\models\\terrain-" + open_.name + L".txt";
    if (!ground.Save(path)) {
        Status(L"Could not write " + path);
        return false;
    }
    // The bake gives terrain patches collision only when the region's roads
    // file asks for it (Network::TerrainCollision): here there is no other
    // ground to drive on.
    {
        const std::wstring roads = Game() + L"\\models\\roads-" + open_.name + L".txt";
        std::string text;
        if (FILE* in = nullptr; ::_wfopen_s(&in, roads.c_str(), L"rb") == 0 && in) {
            char buffer[4096];
            size_t got = 0;
            while ((got = std::fread(buffer, 1, sizeof buffer, in)) > 0) text.append(buffer, got);
            std::fclose(in);
        }
        if (text.find("terraincollision 1") == std::string::npos) {
            if (!text.empty() && text.back() != '\n') text += "\n";
            text += "terraincollision 1\n";
            if (FILE* out = nullptr; ::_wfopen_s(&out, roads.c_str(), L"wb") == 0 && out) {
                std::fwrite(text.data(), 1, text.size(), out);
                std::fclose(out);
            }
        }
    }
    LOG_INFO("regions: %S ground: %zu patches, %zu cells (preset %d, %.0f m, seed %u) round (%.1f, %.1f, %.1f)",
             open_.name.c_str(), patches, cells, settings.preset, settings.size, settings.seed, settings.start.x,
             settings.start.y, settings.start.z);
    return true;
}

void RegionTools::Sync() {
    if (!isCustom_) return;
    std::wstring message;
    if (!io::SyncRegionFiles(Game(), open_, message)) Status(L"Region files not written: " + message);
}

std::wstring RegionTools::FastTestIni() const {
    const std::wstring game = Game();
    return game.empty() ? L"" : game + L"\\scripts\\FRSFastTest.ini";
}

void RegionTools::OnBeforeLaunch() {
    if (!isCustom_) return;
    Sync();
    const std::wstring ini = FastTestIni();
    if (ini.empty() || ::GetFileAttributesW(ini.c_str()) == INVALID_FILE_ATTRIBUTES) {
        Status(L"FRSFastTest is not installed: start the region from Quick Race > Free Run.");
        return;
    }
    // FRSFastTest's dev start on the region's own track, free roam (race type 0).
    const std::vector<std::pair<std::wstring, std::wstring>> values = {
        { L"Mode", L"dev" },
        { L"DevTrack", std::to_wstring(open_.track) },
        { L"DevRaceType", L"0" },
    };
    if (!restorePending_) {
        savedKeys_.clear();
        absentKeys_.clear();
        for (const auto& [key, value] : values) {
            (void)value;
            wchar_t text[64] = {};
            ::GetPrivateProfileStringW(L"FastTest", key.c_str(), L"\x01", text, 64, ini.c_str());
            if (std::wstring(text) == L"\x01") absentKeys_.push_back(key);
            else savedKeys_.push_back({ key, text });
        }
    }
    for (const auto& [key, value] : values) ::WritePrivateProfileStringW(L"FastTest", key.c_str(), value.c_str(), ini.c_str());
    restorePending_ = true;
    restoreAt_ = ::GetTickCount() + kFastTestRestoreMs;
    LOG_INFO("regions: launch goes straight into %S (track %u, free roam)", open_.name.c_str(), open_.track);
}

void RegionTools::RestoreFastTest() {
    if (!restorePending_) return;
    restorePending_ = false;
    const std::wstring ini = FastTestIni();
    if (ini.empty()) return;
    for (const auto& [key, value] : savedKeys_) ::WritePrivateProfileStringW(L"FastTest", key.c_str(), value.c_str(), ini.c_str());
    for (const auto& key : absentKeys_) ::WritePrivateProfileStringW(L"FastTest", key.c_str(), nullptr, ini.c_str());
}

void RegionTools::Tick(float) {
    if (importReopen_) {
        importReopen_ = false;
        nfs::World* world = host_.world ? host_.world() : nullptr;
        if (world && host_.reopenRegions) host_.reopenRegions(world->Region());
    }
    if (restorePending_ && (int)(::GetTickCount() - restoreAt_) >= 0) RestoreFastTest();
    if (pendingStage_ == 2) {
        pendingStage_ = 3;
        if (host_.reopenRegions) host_.reopenRegions(pendingName_);
    }
    if (cleanupDue_ && pendingStage_ == 0) {
        cleanupDue_ = false;
        ++cleanupTries_;
        nfs::World* world = host_.world ? host_.world() : nullptr;
        size_t sections = 0;
        if (world)
            for (size_t i = 0; i < world->Loaded().size(); ++i) {
                nfs::LoadedSection* section = world->MutableLoaded(i);
                if (!section || !section->emptyPending) continue;
                section->placementsChanged = true;
                for (u32 p = 0; p < (u32)section->collision.size(); ++p) section->MarkCollisionChanged(p);
                ++sections;
            }
        LOG_INFO("regions: %S: %zu written sections still had the city in them; written again without it",
                 open_.name.c_str(), sections);
        if (sections && host_.saveEdits) host_.saveEdits();
    }
    if (selfTest_ == 2 && (int)(::GetTickCount() - selfTestAt_) >= 0) {
        selfTest_ = 3;
        nfs::World* world = host_.world ? host_.world() : nullptr;
        size_t hidden = 0, instances = 0, polygons = 0;
        if (world)
            for (const auto& section : world->Loaded()) {
                hidden += section->removedNatives.size();
                instances += section->scenery.instances.size();
                for (const auto& pack : section->collision) polygons += pack.polygons.size();
            }
        LOG_INFO("region selftest: %S empty=%d, %zu of %zu shipped objects hidden, %zu collision polygons in memory",
                 open_.name.c_str(), world && world->EmptyCity() ? 1 : 0, hidden, instances, polygons);
        Sync();
        LOG_INFO("region selftest: done");
    }
}

void RegionTools::Create() {
    if (source_ < 0 || (size_t)source_ >= sources_.size()) return;
    io::NewRegion request;
    request.source = sources_[(size_t)source_];
    request.title = title_;
    request.empty = empty_;
    request.preset = preset_;
    request.size = sizeChoice_ == 0 ? 1000.0f : sizeChoice_ == 2 ? 4000.0f : 2000.0f;
    request.seed = (u32)std::max(1, seed_);
    nfs::CustomRegion made;
    std::wstring message;
    if (!io::CreateRegion(Game(), request, made, message)) {
        createMessage_ = message;
        return;
    }
    asking_ = false;
    Status(message);
    if (made.empty) {
        pendingName_ = made.name;
        pendingStage_ = 1;
    }
    if (host_.reopenRegions) host_.reopenRegions(made.name);
}

void RegionTools::Remove() {
    if (!isCustom_) {
        Status(L"Only a region made in the editor can be removed.");
        return;
    }
    const std::wstring question =
        L"Remove region " + open_.name + L" (\"" + Widen(open_.title) + L"\", track " + std::to_wstring(open_.track) +
        L")?\n\nEvery edit made in it goes with it: its objects, meshes, collision, roads, fog and its own "
        L"route files. The region it was copied from (" + open_.source + L") is not touched.\n\nThis cannot be undone.";
    if (::MessageBoxW(host_.window, question.c_str(), L"Remove Region", MB_ICONWARNING | MB_YESNO | MB_DEFBUTTON2) != IDYES)
        return;
    const nfs::CustomRegion gone = open_;
    std::wstring message;
    io::RemoveRegion(Game(), gone, message);
    isCustom_ = false;
    Status(message);
    if (host_.reopenRegions) host_.reopenRegions(gone.source);
}

void RegionTools::DrawHud(float, float, float, float) {
    if (!asking_) return;
    ImGui::SetNextWindowSize(ImVec2(460, 0), ImGuiCond_Appearing);
    bool open = true;
    if (!ImGui::Begin("New Region###newregion", &open, ImGuiWindowFlags_NoCollapse)) {
        ImGui::End();
        if (!open) asking_ = false;
        return;
    }
    ImGui::TextWrapped("A new region starts as a copy of one of the game's. Everything you then change in it "
                       "stays in it: the region it was copied from is left as it is. It gets a track number of "
                       "its own and shows in Quick Race > Free Run; Launch Game starts straight in it.");
    ImGui::Spacing();
    const std::string preview = sources_.empty() ? std::string() : Narrow(sources_[(size_t)source_]);
    ImGui::SetNextItemWidth(160);
    if (ImGui::BeginCombo("Copy of", preview.c_str())) {
        for (size_t i = 0; i < sources_.size(); ++i) {
            const std::string label = Narrow(sources_[i]) + (sources_[i] == L"L4RA" ? "  (the city)" : "");
            if (ImGui::Selectable(label.c_str(), (int)i == source_)) source_ = (int)i;
        }
        ImGui::EndCombo();
    }
    ImGui::SetNextItemWidth(300);
    ImGui::InputTextWithHint("Name", "shown in Quick Race", title_, sizeof title_);
    ImGui::Spacing();
    if (ImGui::RadioButton("The whole city, to change", !empty_)) empty_ = false;
    if (ImGui::RadioButton("Empty, with ground made for it", empty_)) empty_ = true;
    if (empty_) {
        ImGui::Indent();
        ImGui::TextWrapped("None of the city: a square of new ground round the start, hills, grass and rock, with "
                           "a steep rim at its edge. Build on it with the roads and terrain tools. No traffic "
                           "until there are roads.");
        const char* presets[] = { "Gentle", "Hills", "Mountains" };
        ImGui::SetNextItemWidth(160);
        ImGui::Combo("Ground", &preset_, presets, 3);
        const char* sizes[] = { "1 x 1 km", "2 x 2 km", "4 x 4 km (heavy)" };
        ImGui::SetNextItemWidth(160);
        ImGui::Combo("Size", &sizeChoice_, sizes, 3);
        ImGui::SetNextItemWidth(160);
        ImGui::InputInt("Seed", &seed_);
        ImGui::Unindent();
    }
    if (!createMessage_.empty()) ImGui::TextColored(ImVec4(1.0f, 0.5f, 0.4f, 1.0f), "%s", Narrow(createMessage_).c_str());
    ImGui::Spacing();
    if (ImGui::Button("Create", ImVec2(100, 0))) Create();
    ImGui::SameLine();
    if (ImGui::Button("Cancel", ImVec2(100, 0))) asking_ = false;
    ImGui::End();
    if (!open) asking_ = false;
}
