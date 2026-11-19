#include "ui/ImportTools.h"

#include "core/Log.h"
#include "io/DistrictFog.h"
#include "nfs/CustomRegions.h"
#include "nfs/World.h"
#include "render/WorldRenderer.h"

#include "imgui.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cwchar>

namespace {

constexpr float kCentreReach = 350.0f;   // as io/RegionImport: a section's ground past its centre

// The rows of the properties panel.
constexpr int kFieldX = 1, kFieldY = 2, kFieldZ = 3, kFieldTurn = 4;

std::string Narrow(const std::wstring& text) {
    std::string out;
    for (wchar_t c : text) out += (char)c;
    return out;
}

std::wstring Number(float value) {
    wchar_t text[32];
    ::swprintf_s(text, L"%.2f", value);
    return text;
}

bool ParseNumber(std::wstring text, float& value) {
    for (wchar_t& c : text) if (c == L',') c = L'.';
    wchar_t* end = nullptr;
    const float parsed = std::wcstof(text.c_str(), &end);
    if (end == text.c_str() || !std::isfinite(parsed)) return false;
    value = parsed;
    return true;
}

Mat4 TurnZ(float radians) {
    const float c = std::cos(radians), s = std::sin(radians);
    Mat4 m = Mat4::Identity();
    m.m[0][0] = c;  m.m[0][1] = s;
    m.m[1][0] = -s; m.m[1][1] = c;
    return m;
}

Mat4 Move(const Vec3& by) {
    Mat4 m = Mat4::Identity();
    m.m[3][0] = by.x; m.m[3][1] = by.y; m.m[3][2] = by.z;
    return m;
}

float Wrap360(float degrees) {
    degrees = std::fmod(degrees, 360.0f);
    return degrees < 0.0f ? degrees + 360.0f : degrees;
}

bool Same(const io::RegionImport& a, const io::RegionImport& b) {
    return a.source == b.source && a.firstLetter == b.firstLetter && a.offset.x == b.offset.x &&
           a.offset.y == b.offset.y && a.offset.z == b.offset.z && a.yawDegrees == b.yawDegrees;
}

} // namespace

ImportTools::~ImportTools() {
    undo::Unregister(this);
}

// ---------------------------------------------------------------------------
// The window
// ---------------------------------------------------------------------------

void ImportTools::OnCreate() {
    host_.viewport->AddTool(this);
    Renderer().AddExtension(this);
    undo::Register(this);
    if (!host_.menu) return;
    for (int i = 0; i < ::GetMenuItemCount(host_.menu); ++i) {
        wchar_t text[64] = {};
        ::GetMenuStringW(host_.menu, (UINT)i, text, 63, MF_BYPOSITION);
        if (std::wstring(text).find(L"World") == std::wstring::npos) continue;
        HMENU world = ::GetSubMenu(host_.menu, i);
        if (!world) break;
        ::AppendMenuW(world, MF_SEPARATOR, 0, nullptr);
        ::AppendMenuW(world, MF_STRING, kImportCommand, L"&Import Region...");
        break;
    }
}

bool ImportTools::OnCommand(int id) {
    if (id == kBakeCommand) { Bake(Selected()); return true; }
    if (id == kRevertCommand) {
        const int index = Selected();
        if (index < 0 || !entries_[(size_t)index].import.moved) return true;
        Remember();
        entries_[(size_t)index].import.moved = false;
        SaveShown();
        Status(entries_[(size_t)index].import.source + L" is back where the game has it.");
        return true;
    }
    if (id != kImportCommand) return false;
    nfs::World* world = World();
    if (!world) {
        ::MessageBoxW(host_.window, L"Open the region to bring another one into first.", L"Import Region",
                      MB_ICONINFORMATION | MB_OK);
        return true;
    }
    nfs::CustomRegion custom;
    if (world->IsUnderground1() || nfs::FindCustomRegion(Game(), world->Region(), custom)) {
        ::MessageBoxW(host_.window,
                      L"Regions can be brought into one of the game's own Underground 2 regions only, for now.",
                      L"Import Region", MB_ICONINFORMATION | MB_OK);
        return true;
    }
    ChooseSources();
    askMessage_.clear();
    asking_ = true;
    if (sources_.empty()) askMessage_ = L"Every region of the game is already in this one.";
    return true;
}

void ImportTools::ChooseSources() {
    sources_.clear();
    source_ = 0;
    nfs::World* world = World();
    const std::wstring tracks = Game() + L"\\TRACKS";
    for (const std::wstring& region : nfs::World::AvailableRegions(tracks)) {
        if (world && region == world->Region()) continue;
        if (::GetFileAttributesW((tracks + L"\\" + region + L".BUN").c_str()) == INVALID_FILE_ATTRIBUTES) continue;
        nfs::CustomRegion custom;
        if (nfs::FindCustomRegion(Game(), region, custom)) continue;
        bool taken = false;
        for (const Entry& e : entries_) taken |= e.import.source == region;
        if (!taken) sources_.push_back(region);
    }
}

void ImportTools::DrawHud(float, float, float, float) {
    if (!asking_) return;
    ImGui::SetNextWindowSize(ImVec2(470, 0), ImGuiCond_Appearing);
    bool open = true;
    if (!ImGui::Begin("Import Region###importregion", &open, ImGuiWindowFlags_NoCollapse)) {
        ImGui::End();
        if (!open) asking_ = false;
        return;
    }
    ImGui::TextWrapped("Brings a whole region of the game into this one: its scenery, ground and collision, as "
                       "sections of its own. It goes to the free place that leaves the most room to what is "
                       "here; then select it in the Explorer (Imported regions) and move or turn it with the "
                       "gizmo.");
    ImGui::Spacing();
    ImGui::TextWrapped("The game's collision reaches 4096 m from the middle of the map: moved past that, a region "
                       "is still drawn, but the part outside has no collision (its outline turns orange).");
    ImGui::Spacing();
    if (!sources_.empty()) {
        ImGui::SetNextItemWidth(160);
        if (ImGui::BeginCombo("Region", Narrow(sources_[(size_t)source_]).c_str())) {
            for (size_t i = 0; i < sources_.size(); ++i)
                if (ImGui::Selectable(Narrow(sources_[i]).c_str(), (int)i == source_)) source_ = (int)i;
            ImGui::EndCombo();
        }
    }
    if (!askMessage_.empty()) ImGui::TextColored(ImVec4(1.0f, 0.5f, 0.4f, 1.0f), "%s", Narrow(askMessage_).c_str());
    ImGui::Spacing();
    if (!sources_.empty() && ImGui::Button("Import", ImVec2(100, 0))) {
        nfs::World* world = World();
        const std::wstring source = sources_[(size_t)source_];
        ::SetCursor(::LoadCursorW(nullptr, IDC_WAIT));
        const io::RegionImport made = world ? io::SuggestImport(Game(), world->Region(), source) : io::RegionImport{};
        if (!world) {
            asking_ = false;
        } else if (made.firstLetter == 0) {
            askMessage_ = L"Could not read the sections of " + source + L" or " + world->Region() + L" (see the log).";
        } else if (!io::InsideCollisionRange(io::SectionCentres(Game(), source), made)) {
            askMessage_ = source + L" does not fit anywhere inside the collision range.";
        } else {
            std::vector<io::RegionImport> next;
            for (const Entry& e : entries_) next.push_back(e.import);
            next.push_back(made);
            Remember();
            std::wstring why;
            if (Apply(next, why)) {
                asking_ = false;
                selectAfterReopen_ = (int)next.size() - 1;
            } else {
                undo_.pop_back();
                askMessage_ = why;
            }
        }
    }
    ImGui::SameLine();
    if (ImGui::Button("Cancel", ImVec2(100, 0))) asking_ = false;
    ImGui::End();
    if (!open) asking_ = false;
}

// ---------------------------------------------------------------------------
// The region
// ---------------------------------------------------------------------------

ImportTools::Entry ImportTools::Describe(const io::RegionImport& import) const {
    Entry e;
    e.import = import;
    e.centres = io::SectionCentres(Game(), import.source);
    e.letters = (int)io::SourceLetters(Game(), import.source).size();
    Vec2 lo{ 1e30f, 1e30f }, hi{ -1e30f, -1e30f };
    for (const Vec2& c : e.centres) {
        lo = { std::min(lo.x, c.x), std::min(lo.y, c.y) };
        hi = { std::max(hi.x, c.x), std::max(hi.y, c.y) };
    }
    if (e.centres.empty()) lo = hi = { 0.0f, 0.0f };
    e.middle = { (lo.x + hi.x) * 0.5f, (lo.y + hi.y) * 0.5f };
    e.halfSize = { (hi.x - lo.x) * 0.5f + kCentreReach, (hi.y - lo.y) * 0.5f + kCentreReach };
    return e;
}

void ImportTools::Load() {
    entries_.clear();
    nfs::World* world = World();
    if (!world) return;
    std::vector<io::RegionImport> imports;
    io::ReadImports(Models(), world->Region(), imports);
    for (const io::RegionImport& import : imports) entries_.push_back(Describe(import));
}

void ImportTools::OnRegionLoaded() {
    nfs::World* world = World();
    const std::wstring region = world ? world->Region() : L"";
    if (region != undoRegion_) undo_.clear();
    undoRegion_ = region;
    dragging_ = false;
    Load();
    RefreshPreviews();
    ++version_;
}

void ImportTools::OnRegionClosed() {
    entries_.clear();
    dragging_ = false;
    Renderer().ClearSectionPreview();
    ++version_;
}

void ImportTools::Tick(float) {
    if (reopenDue_) {
        reopenDue_ = false;
        nfs::World* world = World();
        if (world && host_.reopenRegion) host_.reopenRegion(world->Region());
        return;
    }
    if (selectAfterReopen_ >= 0 && (size_t)selectAfterReopen_ < entries_.size()) {
        Select(selectAfterReopen_);
        selectAfterReopen_ = -1;
    }
}

bool ImportTools::Apply(const std::vector<io::RegionImport>& next, std::wstring& why) {
    nfs::World* world = World();
    if (!world) { why = L"No region is open."; return false; }
    if (host_.confirmUnsaved && !host_.confirmUnsaved()) { why = L"Left as it was."; return false; }
    const std::wstring game = Game(), region = world->Region();
    std::vector<io::RegionImport> before;
    for (const Entry& e : entries_) before.push_back(e.import);
    const auto find = [](const std::vector<io::RegionImport>& list, const std::wstring& source) {
        for (const io::RegionImport& i : list)
            if (i.source == source) return &i;
        return (const io::RegionImport*)nullptr;
    };

    ::SetCursor(::LoadCursorW(nullptr, IDC_WAIT));
    // The list first: the region file the bakes write is made from it.
    if (!io::WriteImports(Models(), region, next)) {
        why = L"Could not write " + io::ImportsPath(Models(), region) + L".";
        return false;
    }
    std::vector<const io::RegionImport*> baked;
    std::wstring report;
    for (const io::RegionImport& import : next) {
        const io::RegionImport* old = find(before, import.source);
        if (old && Same(*old, import)) continue;
        Status(L"Baking " + import.source + L" into " + region + L"...");
        std::wstring message;
        if (io::BakeImport(*world, game, import, message)) {
            baked.push_back(&import);
            report = message;
            continue;
        }
        // Back as it was: the list, and what this call already baked.
        why = message;
        io::WriteImports(Models(), region, before);
        for (const io::RegionImport* done : baked) {
            std::wstring unused;
            if (const io::RegionImport* was = find(before, done->source)) io::BakeImport(*world, game, *was, unused);
            else io::RemoveImportSpans(game, region, *done);
        }
        std::wstring unused;
        io::WriteFogToGame(game, region, unused);
        LOG_WARN("import: %S not changed: %S", region.c_str(), why.c_str());
        return false;
    }
    for (const io::RegionImport& old : before)
        if (!find(next, old.source)) io::RemoveImportSpans(game, region, old);
    std::wstring message;
    if (!io::WriteFogToGame(game, region, message)) {
        why = message;
        return false;
    }
    Status((report.empty() ? L"Imported regions written." : report) + L" " + region + L" opens again.");
    reopenDue_ = true;
    return true;
}

std::vector<io::RegionImport> ImportTools::List() const {
    std::vector<io::RegionImport> list;
    for (const Entry& e : entries_) list.push_back(e.import);
    return list;
}

void ImportTools::RefreshPreviews() {
    std::vector<WorldRenderer::SectionPreview> previews;
    for (const Entry& e : entries_) {
        if (!e.import.moved) continue;
        // From where it is baked to where it is shown: the baked placement
        // undone, then the shown one.
        const io::RegionImport shown = e.import.Shown();
        WorldRenderer::SectionPreview p;
        p.first = (u32)e.import.firstLetter * 100;
        p.last = (u32)(e.import.firstLetter + e.letters) * 100;
        p.delta = Move(e.import.offset * -1.0f) * TurnZ((shown.yawDegrees - e.import.yawDegrees) * kPi / 180.0f) *
                  Move(shown.offset);
        previews.push_back(p);
    }
    Renderer().SetSectionPreviews(std::move(previews));
    ++version_;
    if (host_.updateProperties) host_.updateProperties();
}

void ImportTools::SaveShown() {
    nfs::World* world = World();
    if (world && !io::WriteImports(Models(), world->Region(), List()))
        Status(L"Could not write " + io::ImportsPath(Models(), world->Region()) + L".");
    RefreshPreviews();
}

void ImportTools::Bake(int index) {
    if (index < 0 || (size_t)index >= entries_.size() || reopenDue_) return;
    const io::RegionImport& import = entries_[(size_t)index].import;
    if (!import.moved) {
        Status(import.source + L" is in the game where it is shown: nothing to bake.");
        return;
    }
    std::vector<io::RegionImport> next = List();
    next[(size_t)index] = import.Shown();
    Remember();
    std::wstring why;
    if (Apply(next, why)) {
        selectAfterReopen_ = index;
    } else {
        undo_.pop_back();
        Status(L"Not baked: " + why);
    }
}

void ImportTools::Remember() {
    UndoStep step;
    step.stamp = undo::NextStamp();
    for (const Entry& e : entries_) step.imports.push_back(e.import);
    undo_.push_back(std::move(step));
    if (undo_.size() > 50) undo_.erase(undo_.begin());
}

bool ImportTools::UndoLatest() {
    if (undo_.empty()) return false;
    UndoStep step = std::move(undo_.back());
    undo_.pop_back();
    dragging_ = false;
    const int selected = Selected();
    // Only where they are shown changed: nothing to bake.
    bool sameBake = step.imports.size() == entries_.size();
    for (size_t i = 0; sameBake && i < entries_.size(); ++i)
        sameBake = Same(step.imports[i], entries_[i].import);
    if (sameBake) {
        for (size_t i = 0; i < entries_.size(); ++i) entries_[i].import = step.imports[i];
        SaveShown();
        Status(L"Imported region moved back.");
        return true;
    }
    std::wstring why;
    if (Apply(step.imports, why)) {
        selectAfterReopen_ = selected >= 0 && (size_t)selected < step.imports.size() ? selected : -1;
        Status(L"Imported region change undone; the region opens again.");
    } else {
        Status(L"Could not undo the imported region change: " + why);
    }
    return true;
}

// ---------------------------------------------------------------------------
// Selection and moving
// ---------------------------------------------------------------------------

int ImportTools::Selected() const {
    const int index = Renderer().ExtensionSelection(this);
    return index >= 0 && (size_t)index < entries_.size() ? index : -1;
}

void ImportTools::Select(int index) {
    if (index >= 0) Renderer().SelectExtension(this, index);
    else if (Renderer().ExtensionSelection(this) >= 0) Renderer().SelectExtension(this, -1);
    if (host_.updateProperties) host_.updateProperties();
}

Vec3 ImportTools::Centre(const io::RegionImport& import, const Entry& entry) const {
    return import.ShownTransform().TransformPoint({ entry.middle.x, entry.middle.y, 0.0f });
}

void ImportTools::Outline(const Mat4& transform, const Entry& e, float z, Vec3 out[4]) const {
    const Vec2 m = e.middle, h = e.halfSize;
    const Vec2 corners[4] = { { m.x - h.x, m.y - h.y }, { m.x + h.x, m.y - h.y },
                              { m.x + h.x, m.y + h.y }, { m.x - h.x, m.y + h.y } };
    for (int i = 0; i < 4; ++i) {
        const Vec3 p = transform.TransformPoint({ corners[i].x, corners[i].y, 0.0f });
        out[i] = { p.x, p.y, z };
    }
}

bool ImportTools::GizmoPivot(Vec3& pivot) {
    const int index = Selected();
    if (index < 0 || reopenDue_) return false;
    const Entry& e = entries_[(size_t)index];
    pivot = dragging_ ? dragPivot_ : Centre(e.import, e);
    return true;
}

void ImportTools::GizmoChanged(const Mat4& transform, bool done, bool keep) {
    const int index = Selected();
    if (index < 0) { dragging_ = false; return; }
    Entry& e = entries_[(size_t)index];
    if (!dragging_) {
        Remember();
        dragging_ = true;
        dragStart_ = e.import;
        dragPivot_ = Centre(e.import, e);
    }
    const float factor = std::max(1e-4f, std::sqrt(transform.m[0][0] * transform.m[0][0] +
                                                   transform.m[0][1] * transform.m[0][1] +
                                                   transform.m[0][2] * transform.m[0][2]));
    // Only the turn about Z: a tilt or a scale has no meaning for a region.
    const float turn = std::atan2(transform.m[0][1] / factor, transform.m[0][0] / factor);
    const Vec3 to{ transform.m[3][0], transform.m[3][1], transform.m[3][2] };
    const float startYaw = dragStart_.Shown().yawDegrees;
    SetPoint(to, startYaw + turn * 180.0f / kPi, false);
    if (!done) return;
    if (!keep) {
        e.import = dragStart_;
        dragging_ = false;
        undo_.pop_back();
        RefreshPreviews();
        return;
    }
    SetPoint(to, startYaw + turn * 180.0f / kPi, true);
}

bool ImportTools::GetPoint(Vec3& position, float& heading) {
    const int index = Selected();
    if (index < 0 || reopenDue_) return false;
    const Entry& e = entries_[(size_t)index];
    position = Centre(e.import, e);
    heading = e.import.Shown().yawDegrees;
    return true;
}

bool ImportTools::SetPoint(const Vec3& position, float heading, bool commit) {
    const int index = Selected();
    if (index < 0 || reopenDue_) return false;
    Entry& e = entries_[(size_t)index];
    if (!dragging_) {
        Remember();
        dragging_ = true;
        dragStart_ = e.import;
        dragPivot_ = Centre(e.import, e);
    }
    // Where it was shown as the drag began: from there, about the centre, by
    // the turn, to the new centre. Only the view follows; the game keeps the
    // baked place until Bake.
    const io::RegionImport start = dragStart_.Shown();
    const float turn = (heading - start.yawDegrees) * kPi / 180.0f;
    const Mat4 placed = start.Transform() * Move(dragPivot_ * -1.0f) * TurnZ(turn) * Move(position);
    e.import.shownYaw = Wrap360(heading);
    e.import.shownOffset = { placed.m[3][0], placed.m[3][1], placed.m[3][2] };
    const auto closeTo = [](float a, float b) { return std::fabs(a - b) < 0.01f; };
    const float turnLeft = std::fmod(std::fabs(e.import.shownYaw - e.import.yawDegrees), 360.0f);
    e.import.moved = !(closeTo(e.import.shownOffset.x, e.import.offset.x) && closeTo(e.import.shownOffset.y, e.import.offset.y) &&
                       closeTo(e.import.shownOffset.z, e.import.offset.z) && (turnLeft < 0.01f || turnLeft > 359.99f));
    RefreshPreviews();
    if (!commit) return true;

    dragging_ = false;
    const io::RegionImport now = e.import.Shown(), was = dragStart_.Shown();
    if (Same(now, was)) {
        undo_.pop_back();   // nothing moved
        return true;
    }
    SaveShown();
    Status(e.import.moved ? e.import.source + L" moved in the view only: Bake in its properties puts it in the game there."
                          : e.import.source + L" is back where the game has it.");
    return true;
}

bool ImportTools::DeleteSelection() {
    const int index = Selected();
    if (index < 0) return false;
    if (reopenDue_) return true;
    std::vector<io::RegionImport> next;
    for (size_t i = 0; i < entries_.size(); ++i)
        if ((int)i != index) next.push_back(entries_[i].import);
    const std::wstring source = entries_[(size_t)index].import.source;
    Remember();
    std::wstring why;
    if (Apply(next, why)) {
        Select(-1);
        Status(source + L" taken out of the region (Ctrl+Z brings it back); the region opens again.");
    } else {
        undo_.pop_back();
        Status(L"Could not take " + source + L" out: " + why);
    }
    return true;
}

// ---------------------------------------------------------------------------
// The Explorer and the properties
// ---------------------------------------------------------------------------

size_t ImportTools::SceneSignature() const {
    return entries_.empty() ? 0 : entries_.size() * 7907 + version_ * 37;
}

void ImportTools::PopulateTree(SceneTreeBuilder& tree) {
    if (entries_.empty()) return;
    HTREEITEM root = tree.insert(TVI_ROOT, L"Imported regions (" + std::to_wstring(entries_.size()) + L")", -1);
    size_t shown = 0;
    for (size_t i = 0; i < entries_.size(); ++i) {
        const Entry& e = entries_[i];
        wchar_t label[128];
        ::swprintf_s(label, L"%s  (sections %d-%d)%s", e.import.source.c_str(), e.import.firstLetter * 100,
                     (e.import.firstLetter + e.letters) * 100 - 1, e.import.moved ? L"  - moved, not baked" : L"");
        if (!tree.matches(label)) continue;
        tree.insert(root, label, (intptr_t)i);
        ++shown;
    }
    tree.finish(root, shown, true);
}

void ImportTools::OnTreeItem(intptr_t code) {
    if (code < 0 || (size_t)code >= entries_.size()) return;
    Select((int)code);
    const Entry& e = entries_[(size_t)code];
    Vec3 corners[4];
    const float z = e.import.Shown().offset.z;
    Outline(e.import.ShownTransform(), e, z, corners);
    Aabb box;
    for (const Vec3& c : corners) {
        box.Add({ c.x, c.y, z - 50.0f });
        box.Add({ c.x, c.y, z + 150.0f });
    }
    host_.viewport->FrameBounds(box);
}

bool ImportTools::StatusText(std::wstring& text) const {
    const int index = Selected();
    if (index < 0) return false;
    const Entry& e = entries_[(size_t)index];
    const Vec3 c = Centre(e.import, e);
    wchar_t buffer[256];
    ::swprintf_s(buffer, L"Imported region %s: centre (%.0f, %.0f), turned %.1f degrees%s -- the gizmo moves and turns "
                         L"it in the view, Bake (properties) puts it in the game, Delete takes it out",
                 e.import.source.c_str(), c.x, c.y, e.import.Shown().yawDegrees,
                 e.import.moved ? L" (moved, not baked)" : L"");
    text = buffer;
    return true;
}

bool ImportTools::FillProperties(PropertiesPanel::View& view) {
    const int index = Selected();
    if (index < 0) return false;
    const Entry& e = entries_[(size_t)index];
    view.title = L"Imported region " + e.import.source;
    view.moveLabel.clear();
    view.removeLabel = L"Remove import";
    const auto add = [&view](const wchar_t* group, const wchar_t* name, std::wstring value, const wchar_t* help,
                             int field, bool editable) {
        PropertiesPanel::Row row;
        row.group = group;
        row.name = name;
        row.value = std::move(value);
        row.description = help;
        row.field = field;
        row.editable = editable;
        view.rows.push_back(std::move(row));
    };
    const Vec3 c = Centre(e.import, e);
    const io::RegionImport shown = e.import.Shown();
    const wchar_t* place = L"Where the middle of the region stands, in metres. In the view only, until Bake.";
    add(L"Placement", L"Centre X", Number(c.x), place, kFieldX, true);
    add(L"Placement", L"Centre Y", Number(c.y), place, kFieldY, true);
    add(L"Placement", L"Height", Number(shown.offset.z), L"Raised or lowered as a whole, in metres.", kFieldZ, true);
    add(L"Placement", L"Turn", Number(shown.yawDegrees), L"About Z, in degrees.", kFieldTurn, true);
    add(L"Placement", L"In the game", e.import.moved ? L"No: moved since the last bake" : L"Yes, where it is shown",
        L"The gizmo and these fields move it in the view only. Bake writes it into the game where it is shown "
        L"(scenery, ground and collision), and the region opens again.", 0, false);
    view.actions.push_back({ e.import.moved ? L"Bake" : L"Bake (nothing moved)", kBakeCommand, L"Game",
                             L"Writes the region into the game where it is shown; the region opens again." });
    if (e.import.moved)
        view.actions.push_back({ L"Back to the baked place", kRevertCommand, L"Game",
                                 L"Puts it back in the view where the game has it." });
    wchar_t range[64];
    ::swprintf_s(range, L"%d-%d", e.import.firstLetter * 100, (e.import.firstLetter + e.letters) * 100 - 1);
    add(L"Region", L"Source", e.import.source, L"The game's region it is a copy of.", 0, false);
    add(L"Region", L"Sections", range, L"The numbers its sections have here: letters past Z, which the loader "
                                       L"opens (loader/src/Limits.cpp).", 0, false);
    add(L"Region", L"Section count", std::to_wstring(e.centres.size()), L"Sections with ground of their own.", 0,
        false);
    return true;
}

bool ImportTools::ApplyProperties() {
    const int index = Selected();
    if (index < 0) return false;
    PropertiesPanel* properties = host_.properties;
    if (!properties || reopenDue_) return true;
    Entry& e = entries_[(size_t)index];
    const Vec3 was = Centre(e.import, e);
    Vec3 centre = was;
    float turn = e.import.Shown().yawDegrees;
    const auto number = [&](int field, float& into) {
        float value = 0.0f;
        std::wstring text;
        if (properties->ReadText(field, text) && ParseNumber(text, value)) into = value;
    };
    number(kFieldX, centre.x);
    number(kFieldY, centre.y);
    number(kFieldZ, centre.z);
    number(kFieldTurn, turn);
    // The same as dragging it there: about its centre, to the new centre.
    SetPoint(centre, turn, false);
    SetPoint(centre, turn, true);
    return true;
}

// ---------------------------------------------------------------------------
// Drawing
// ---------------------------------------------------------------------------

void ImportTools::DrawOverlay(IDirect3DDevice9* device, const Camera& camera, WorldRenderer& renderer) {
    (void)camera;
    if (entries_.empty()) return;
    struct LineVertex { float x, y, z; D3DCOLOR colour; };
    constexpr DWORD kLineFvf = D3DFVF_XYZ | D3DFVF_DIFFUSE;
    renderer.BeginOverlay(device);
    device->SetTexture(0, nullptr);
    device->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
    device->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_DIFFUSE);
    device->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1);
    device->SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_DIFFUSE);
    device->SetRenderState(D3DRS_ZENABLE, FALSE);
    device->SetFVF(kLineFvf);
    const int selected = Selected();
    for (size_t i = 0; i < entries_.size(); ++i) {
        // The selected one, and those moved but not baked: their outlines,
        // where they are shown (the regions themselves are in the view).
        const Entry& e = entries_[i];
        if ((int)i != selected && !e.import.moved) continue;
        Vec3 c[4];
        Outline(e.import.ShownTransform(), e, Centre(e.import, e).z + 2.0f, c);
        // Selected: yellow, orange while part of it stands past the collision
        // range (drawn there, but not driven on). Not selected and not baked: cyan.
        const D3DCOLOR colour = (int)i != selected                                     ? 0xFF40D0E0
                                : io::InsideCollisionRange(e.centres, e.import.Shown()) ? 0xFFFFC832
                                                                                        : 0xFFFF8020;
        const LineVertex outline[5] = {
            { c[0].x, c[0].y, c[0].z, colour }, { c[1].x, c[1].y, c[1].z, colour },
            { c[2].x, c[2].y, c[2].z, colour }, { c[3].x, c[3].y, c[3].z, colour },
            { c[0].x, c[0].y, c[0].z, colour },
        };
        device->DrawPrimitiveUP(D3DPT_LINESTRIP, 4, outline, sizeof(LineVertex));
    }
    renderer.EndOverlay(device);
}
