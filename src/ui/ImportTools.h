#pragma once

#include "io/RegionImport.h"
#include "render/RenderExtension.h"
#include "ui/EditorExtension.h"
#include "ui/UndoRegistry.h"
#include "ui/Viewport.h"

#include <windows.h>

#include <functional>
#include <string>
#include <vector>

namespace nfs { class World; }

struct ImportToolsHost {
    HWND      window = nullptr;
    HMENU     menu = nullptr;
    Viewport* viewport = nullptr;
    PropertiesPanel* properties = nullptr;
    std::function<nfs::World*()> world;
    std::function<std::wstring()> gameFolder;
    std::function<void(const std::wstring&)> status;
    std::function<void()> updateProperties;
    std::function<bool()> confirmUnsaved;                       // the edits not yet written: asked first
    std::function<void(const std::wstring& region)> reopenRegion;
};

// Whole regions of the game brought into the open one (io/RegionImport.h).
//
// World > Import Region... picks a region of the game and bakes all of it into
// the open one at a free place inside the collision range. Each import is a row
// of the Explorer's "Imported regions" branch: selected, the gizmo moves it and
// turns it about Z -- in the view only: the region is drawn where it is put,
// and the game keeps it where it was baked. The properties card holds its
// centre and turn, and Bake, which writes it into the game where it is shown
// (the region then opens again), and Back to the Baked Place. Delete takes it
// out. Every change is a Ctrl+Z step.
class ImportTools : public EditorExtension, public RenderExtension, public ViewportTool, public UndoParticipant {
public:
    explicit ImportTools(ImportToolsHost host) : host_(std::move(host)) {}
    ~ImportTools() override;

    static constexpr int kImportCommand = 45401, kBakeCommand = 45402, kRevertCommand = 45403;

    // ---- EditorExtension
    void OnCreate() override;
    bool OnCommand(int id) override;
    void OnRegionLoaded() override;
    void OnRegionClosed() override;
    void Tick(float deltaSeconds) override;
    void DrawHud(float x, float y, float width, float height) override;
    size_t SceneSignature() const override;
    void PopulateTree(SceneTreeBuilder& tree) override;
    void OnTreeItem(intptr_t code) override;
    bool StatusText(std::wstring& text) const override;
    bool FillProperties(PropertiesPanel::View& view) override;
    bool ApplyProperties() override;
    bool DeleteSelection() override;
    bool GetPoint(Vec3& position, float& heading) override;
    bool SetPoint(const Vec3& position, float heading, bool commit) override;

    // ---- RenderExtension
    void DrawOverlay(IDirect3DDevice9* device, const Camera& camera, WorldRenderer& renderer) override;

    // ---- ViewportTool: the gizmo on the selected import
    bool GizmoPivot(Vec3& pivot) override;
    void GizmoChanged(const Mat4& transform, bool done, bool keep) override;

    // ---- UndoParticipant
    u64 LatestUndoStamp() const override { return undo_.empty() ? 0 : undo_.back().stamp; }
    bool UndoLatest() override;

private:
    struct Entry {
        io::RegionImport import;
        std::vector<Vec2> centres;   // the source's sections, where the source has them
        Vec2 middle;                 // of their box
        Vec2 halfSize;               // half the box, plus what the ground reaches past a centre
        int  letters = 0;
    };

    nfs::World* World() const { return host_.world ? host_.world() : nullptr; }
    std::wstring Game() const { return host_.gameFolder ? host_.gameFolder() : L""; }
    std::wstring Models() const { return Game() + L"\\models"; }
    WorldRenderer& Renderer() const { return host_.viewport->GetRenderer(); }
    void Status(const std::wstring& text) const { if (host_.status) host_.status(text); }

    Entry Describe(const io::RegionImport& import) const;
    void Load();
    int  Selected() const;
    void Select(int index);
    Vec3 Centre(const io::RegionImport& import, const Entry& entry) const;   // the middle, where it is put
    void Outline(const Mat4& transform, const Entry& entry, float z, Vec3 out[4]) const;
    // Makes `next` the list: what changed baked, what went taken out, the
    // region file written, and the region opened again. False (nothing
    // changed) when it could not be done; `why` then says so.
    bool Apply(const std::vector<io::RegionImport>& next, std::wstring& why);
    std::vector<io::RegionImport> List() const;
    // Each import moved since its bake drawn where it is shown (WorldRenderer::SetSectionPreviews).
    void RefreshPreviews();
    // The list written as it is, nothing baked: where the imports are shown.
    void SaveShown();
    void Bake(int index);           // the import written into the game where it is shown
    void Remember();
    void ChooseSources();

    ImportToolsHost host_;
    std::vector<Entry> entries_;
    size_t version_ = 0;

    struct UndoStep { u64 stamp = 0; std::vector<io::RegionImport> imports; };
    std::vector<UndoStep> undo_;
    std::wstring undoRegion_;         // the steps are kept across the reopen a change does

    bool dragging_ = false;
    io::RegionImport dragStart_;
    io::RegionImport dragNow_;
    Vec3 dragPivot_;
    bool reopenDue_ = false;
    int  selectAfterReopen_ = -1;

    bool asking_ = false;             // the Import Region window
    std::vector<std::wstring> sources_;
    int source_ = 0;
    std::wstring askMessage_;
};
