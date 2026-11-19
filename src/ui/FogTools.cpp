#include "ui/FogTools.h"
#include "nfs/CustomRegions.h"

#include "core/Log.h"
#include "nfs/World.h"
#include "ui/Viewport.h"

#include "imgui.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace {

std::string Narrow(const std::wstring& text) {
    std::string out;
    if (text.empty()) return out;
    const int size = ::WideCharToMultiByte(CP_UTF8, 0, text.c_str(), (int)text.size(), nullptr, 0, nullptr, nullptr);
    out.resize((size_t)size);
    ::WideCharToMultiByte(CP_UTF8, 0, text.c_str(), (int)text.size(), out.data(), size, nullptr, nullptr);
    return out;
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

// A point the game counts as inside zone `index` (the smallest zone there is
// that one): its box centre when that works, else the first such point of a
// grid over its box. A zone's box centre can lie outside a bent outline.
bool InsidePoint(const std::vector<nfs::CityZone>& zones, size_t index, float& x, float& y) {
    const nfs::CityZone& z = zones[index];
    x = (z.boundsMin.x + z.boundsMax.x) * 0.5f;
    y = (z.boundsMin.y + z.boundsMax.y) * 0.5f;
    if (nfs::ZoneAt(zones, x, y) == (int)index) return true;
    for (int j = 1; j < 32; ++j)
        for (int i = 1; i < 32; ++i) {
            x = z.boundsMin.x + (z.boundsMax.x - z.boundsMin.x) * (float)i / 32.0f;
            y = z.boundsMin.y + (z.boundsMax.y - z.boundsMin.y) * (float)j / 32.0f;
            if (nfs::ZoneAt(zones, x, y) == (int)index) return true;
        }
    return false;
}

} // namespace

const std::vector<nfs::CityZone>& FogTools::Zones() const {
    static const std::vector<nfs::CityZone> none;
    return host_.zones ? host_.zones() : none;
}

void FogTools::OnCreate() {
    undo::Register(this);
    if (std::getenv("FRS_FOG_SELFTEST")) selfTest_ = 1;
    if (!host_.menu) return;
    for (int i = 0; i < ::GetMenuItemCount(host_.menu); ++i) {
        wchar_t text[64] = {};
        ::GetMenuStringW(host_.menu, (UINT)i, text, 63, MF_BYPOSITION);
        if (std::wstring(text).find(L"View") == std::wstring::npos) continue;
        if (HMENU view = ::GetSubMenu(host_.menu, i)) {
            ::AppendMenuW(view, MF_SEPARATOR, 0, nullptr);
            ::AppendMenuW(view, MF_STRING, kShowCommand, L"&Fog by Neighbourhood...");
        }
        break;
    }
}

bool FogTools::OnCommand(int id) {
    if (id != kShowCommand) return false;
    shown_ = !shown_;
    if (host_.menuChecks) host_.menuChecks();
    return true;
}

void FogTools::UpdateMenuChecks(HMENU menu) {
    ::CheckMenuItem(menu, kShowCommand, MF_BYCOMMAND | (shown_ ? MF_CHECKED : MF_UNCHECKED));
}

void FogTools::OnRegionLoaded() {
    undo_.clear();
    plan_ = {};
    nfs::World* world = host_.world ? host_.world() : nullptr;
    if (!world || !host_.gameFolder) return;
    region_ = world->Region();
    const std::wstring game = host_.gameFolder();
    shipped_ = ReadWhole(nfs::ShippedTrackFile(game, region_, region_ + L".BUN"));
    io::ReadFogPlan(game + L"\\models", region_, plan_);
    // A row for every neighbourhood, starting from what the game shows there.
    const auto& zones = Zones();
    for (const nfs::CityZone& zone : zones) EntryFor(zone);
    LOG_INFO("fog: %zu neighbourhoods, %zu with a fog of their own", zones.size(),
             (size_t)std::count_if(plan_.entries.begin(), plan_.entries.end(),
                                   [](const io::DistrictFogEntry& e) { return e.custom; }));
}

io::DistrictFogEntry& FogTools::EntryFor(const nfs::CityZone& zone) {
    if (io::DistrictFogEntry* e = plan_.Find(zone.hash)) return *e;
    io::DistrictFogEntry e;
    e.zoneHash = zone.hash;
    const auto& zones = Zones();
    const size_t index = (size_t)(&zone - zones.data());
    float x = 0, y = 0;
    if (index < zones.size() && InsidePoint(zones, index, x, y))
        io::FogAt(shipped_, x, y, e.day, e.night, e.rain);
    plan_.entries.push_back(e);
    return plan_.entries.back();
}

bool FogTools::Commit() {
    if (!host_.gameFolder || region_.empty()) return false;
    const std::wstring game = host_.gameFolder();
    if (!io::WriteFogPlan(game + L"\\models", region_, plan_)) {
        lastMessage_ = L"Could not write " + io::FogPlanPath(game + L"\\models", region_);
        Status(lastMessage_);
        return false;
    }
    const bool ok = io::WriteFogToGame(game, region_, lastMessage_);
    if (host_.viewport) host_.viewport->GetRenderer().ReloadFogMaps();
    Status(lastMessage_);
    return ok;
}

void FogTools::Changed(const io::FogPlan& before) {
    if (before == plan_) return;
    undo_.push_back({ undo::NextStamp(), before });
    if (undo_.size() > 100) undo_.erase(undo_.begin());
    Commit();
}

bool FogTools::UndoLatest() {
    if (undo_.empty()) return false;
    plan_ = undo_.back().before;
    undo_.pop_back();
    Commit();
    Status(L"Fog change taken back.");
    return true;
}

// Instant widgets (a tick, a button) change the plan in the frame they are
// used; a slider changes it every frame of the drag. Either way one undo step,
// from the plan as it was when the widget was taken hold of.
void FogTools::Track(const io::FogPlan& frameStart) {
    if (ImGui::IsItemActivated()) editStart_ = frameStart;
    if (ImGui::IsItemDeactivatedAfterEdit()) Changed(editStart_);
}

bool FogTools::LookEditor(const char* label, io::FogLook& look) {
    ImGui::PushID(label);
    ImGui::TextUnformatted(label);
    const io::FogPlan frameStart = plan_;
    bool edited = false;
    ImGui::SetNextItemWidth(160);
    edited |= ImGui::SliderFloat("Amount", &look.strength, 0.0f, 1.0f, "%.2f");
    Track(frameStart);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("How much fog there is 250 m away and beyond. The PC never shows less than 0.4 there:\n"
                          "to hide the fog, give it the colour of the sky.");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(160);
    edited |= ImGui::SliderFloat("Starts at", &look.start, 0.0f, 249.0f, "%.0f m");
    Track(frameStart);
    float colour[3] = { look.r / 255.0f, look.g / 255.0f, look.b / 255.0f };
    ImGui::SameLine();
    if (ImGui::ColorEdit3("Colour", colour, ImGuiColorEditFlags_NoInputs)) {
        look.r = colour[0] * 255.0f;
        look.g = colour[1] * 255.0f;
        look.b = colour[2] * 255.0f;
        edited = true;
    }
    Track(frameStart);
    if (std::max({ look.r, look.g, look.b }) > 191.5f) {
        ImGui::SameLine();
        ImGui::TextDisabled("(brighter than 191 shows as 191)");
    }
    ImGui::PopID();
    return edited;
}

void FogTools::DrawHud(float x, float y, float width, float height) {
    (void)x; (void)y; (void)width; (void)height;
    if (!shown_) return;
    ImGui::SetNextWindowSize(ImVec2(620, 640), ImGuiCond_FirstUseEver);
    bool open = true;
    if (!ImGui::Begin("Fog by Neighbourhood###districtfog", &open)) {
        ImGui::End();
        if (!open) { shown_ = false; if (host_.menuChecks) host_.menuChecks(); }
        return;
    }
    const auto& zones = Zones();
    if (zones.empty()) {
        ImGui::TextWrapped("This region has no neighbourhoods (zones of type 0x11 in PathsFreeRoam.bin).");
        ImGui::End();
        if (!open) shown_ = false;
        return;
    }
    ImGui::TextWrapped("Give a neighbourhood a fog of its own for day, night and rain. The game eases from one "
                       "to the next over 50 m. A neighbourhood left alone keeps the game's own fog. Each change "
                       "is written to the game at once; turn on View > Game Look to see the day fog here.");
    ImGui::Separator();

    std::vector<std::wstring> districts = nfs::DistrictOrder();
    for (const nfs::CityZone& z : zones)
        if (std::find(districts.begin(), districts.end(), z.district) == districts.end()) districts.push_back(z.district);

    for (const std::wstring& district : districts) {
        std::vector<size_t> members;
        for (size_t i = 0; i < zones.size(); ++i)
            if (zones[i].district == district) members.push_back(i);
        if (members.empty()) continue;
        const std::string title = (district.empty() ? std::string("Other") : Narrow(district)) + "##d";
        if (!ImGui::CollapsingHeader(title.c_str(), ImGuiTreeNodeFlags_DefaultOpen)) continue;
        for (size_t index : members) {
            const nfs::CityZone& zone = zones[index];
            io::DistrictFogEntry& entry = EntryFor(zone);
            ImGui::PushID((int)index);
            const io::FogPlan frameStart = plan_;
            ImGui::Checkbox("##own", &entry.custom);
            Track(frameStart);
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("A fog of its own (off: the game's)");
            ImGui::SameLine();
            const std::string name = zone.name.empty() ? std::string("Unnamed zone") : Narrow(zone.name);
            const bool expanded = ImGui::TreeNodeEx(name.c_str(), entry.custom ? ImGuiTreeNodeFlags_DefaultOpen : 0);
            ImGui::SameLine(ImGui::GetContentRegionAvail().x + ImGui::GetCursorPosX() - 190);
            if (ImGui::SmallButton("Go there") && host_.travelToZone) host_.travelToZone(index);
            ImGui::SameLine();
            if (ImGui::SmallButton("Same for district")) {
                const io::FogPlan before = plan_;
                const io::DistrictFogEntry copy = entry;
                for (size_t other : members) {
                    io::DistrictFogEntry& e = EntryFor(zones[other]);
                    const u32 hash = e.zoneHash;
                    e = copy;
                    e.zoneHash = hash;
                }
                Changed(before);
            }
            if (expanded) {
                ImGui::BeginDisabled(!entry.custom);
                LookEditor("Day", entry.day);
                LookEditor("Night", entry.night);
                const io::FogPlan beforeTick = plan_;
                ImGui::Checkbox("Rain: the day's, lighter", &entry.rainFollowsDay);
                Track(beforeTick);
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("Rain darkens the sky on top of its fog: a dark rain fog gives a nearly "
                                      "black picture, so the rain look is kept lighter than the day's.");
                if (!entry.rainFollowsDay) LookEditor("Rain", entry.rain);
                ImGui::EndDisabled();
                ImGui::TreePop();
            }
            ImGui::PopID();
        }
    }
    ImGui::Separator();
    if (ImGui::Button("Back to the game's fog everywhere")) {
        const io::FogPlan before = plan_;
        for (io::DistrictFogEntry& e : plan_.entries) e.custom = false;
        Changed(before);
    }
    ImGui::SameLine();
    if (ImGui::Button("Write to the game again")) Commit();
    if (!lastMessage_.empty()) ImGui::TextDisabled("%s", Narrow(lastMessage_).c_str());
    ImGui::End();
    if (!open) { shown_ = false; if (host_.menuChecks) host_.menuChecks(); }
}

// FRS_FOG_SELFTEST=1: the rebuild checked in memory on the region that opens.
// Nothing is written.
void FogTools::Tick(float) {
    if (selfTest_ != 1 || shipped_.empty() || Zones().empty()) return;
    selfTest_ = 2;
    int failures = 0;
    const auto check = [&](bool ok, const char* what) {
        if (!ok) ++failures;
        LOG_INFO("fog selftest: %s  %s", ok ? "ok  " : "FAIL", what);
    };
    const auto& zones = Zones();
    size_t target = 0;
    float tx = 0, ty = 0;
    while (target < zones.size() && (zones[target].hash == 0 || !InsidePoint(zones, target, tx, ty))) ++target;
    check(target < zones.size(), "a named neighbourhood with a point inside it");
    if (target >= zones.size()) return;

    io::FogPlan plan;
    io::DistrictFogEntry e;
    e.zoneHash = zones[target].hash;
    e.custom = true;
    e.day = { 0.8f, 30.0f, 150.0f, 30.0f, 30.0f };
    e.night = { 0.7f, 60.0f, 20.0f, 20.0f, 120.0f };
    e.rainFollowsDay = true;
    plan.entries.push_back(e);

    std::vector<u8> layout = shipped_;
    std::wstring error;
    check(io::ApplyFogPlan(layout, plan, zones, error), "the plan applies");
    io::FogLook day, night, rain, sDay, sNight, sRain;
    check(io::FogAt(layout, tx, ty, day, night, rain), "fog read back");
    const auto closeTo = [](const io::FogLook& a, const io::FogLook& b) {
        return std::fabs(a.strength - b.strength) < 0.01f && std::fabs(a.start - b.start) < 0.6f &&
               std::fabs(a.r - b.r) < 0.6f && std::fabs(a.g - b.g) < 0.6f && std::fabs(a.b - b.b) < 0.6f;
    };
    check(closeTo(day, e.day) && closeTo(night, e.night) && closeTo(rain, io::LighterRain(e.day)),
          "inside the neighbourhood: its own day, night and rain");
    // Somewhere no zone is: what the game had.
    bool compared = false, same = true;
    for (float px = -6000; px < 2000 && !compared; px += 97)
        for (float py = -6000; py < 3000 && !compared; py += 89) {
            if (nfs::ZoneAt(zones, px, py) >= 0) continue;
            io::FogAt(layout, px, py, day, night, rain);
            io::FogAt(shipped_, px, py, sDay, sNight, sRain);
            same = closeTo(day, sDay) && closeTo(night, sNight);
            compared = true;
        }
    check(compared && same, "outside every neighbourhood: the shipped fog");
    // The shipped bytes all where they were: only the old chunk's id differs
    // (it is padding now), the rebuilt maps come after the end.
    size_t differing = 0;
    for (size_t i = 0; i < shipped_.size() && i < layout.size(); ++i) differing += shipped_[i] != layout[i];
    check(layout.size() > shipped_.size() && differing >= 1 && differing <= 4, "nothing in the shipped file moved");
    io::FogPlan none;
    check(io::ApplyFogPlan(layout, none, zones, error) && layout == shipped_, "an empty plan gives back the shipped file");
    LOG_INFO("fog selftest: %s (%d failures)", failures ? "FAILED" : "PASSED", failures);
}
