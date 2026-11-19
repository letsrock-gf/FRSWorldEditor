#pragma once

#include "render/RenderExtension.h"
#include "ui/EditorExtension.h"
#include "ui/UndoRegistry.h"
#include "ui/Viewport.h"

#include <windows.h>

#include <functional>
#include <map>
#include <string>
#include <vector>

namespace nfs { class World; }

struct ReferenceImagesHost {
    HWND      window = nullptr;
    HMENU     menu = nullptr;
    Viewport* viewport = nullptr;
    PropertiesPanel* properties = nullptr;
    std::function<nfs::World*()> world;
    std::function<std::wstring()> modelsFolder;
    std::function<void(const std::wstring&)> status;
    std::function<void()> menuChecks;
    std::function<void()> updateProperties;
    std::function<bool(Vec3&)> viewCentre;   // the ground in the middle of the view
};

// Pictures to build over -- a blueprint, a map, a photo from above -- shown
// in the view as flat, see-through sheets. Editor only: the game never sees
// them.
//
// View > Import Reference Image... copies the picture into
// <game>\models\editor-references (so it goes with the world and its
// .frsworld file, but never with a published one) and lays it flat on the
// ground in the middle of the view, its long side 100 m at scale 1. A click
// picks it; the properties then hold its position, rotation and scale on
// all three axes, opacity, whether it is drawn over the world and whether it
// is locked (a locked sheet cannot be clicked, so the world under it can).
// The gizmo moves, turns and scales it as it does an object; every change is
// a Ctrl+Z step. The list is kept per region in
// <game>\models\editor-references-<REGION>.txt.
class ReferenceImages : public EditorExtension, public RenderExtension, public ViewportTool,
                        public UndoParticipant {
public:
    explicit ReferenceImages(ReferenceImagesHost host) : host_(std::move(host)) {}
    ~ReferenceImages() override;

    static constexpr int kImportCommand = 44951, kShowCommand = 44952, kLayFlatCommand = 44953,
                         kStandUpCommand = 44954, kLockCommand = 44955, kOnTopCommand = 44956,
                         kNaturalSizeCommand = 44957;

    // ---- EditorExtension
    void OnCreate() override;
    bool OnCommand(int id) override;
    void UpdateMenuChecks(HMENU menu) override;
    void OnRegionLoaded() override;
    void OnRegionClosed() override;
    size_t SceneSignature() const override;
    void PopulateTree(SceneTreeBuilder& tree) override;
    void OnTreeItem(intptr_t code) override;
    bool StatusText(std::wstring& text) const override;
    bool FillProperties(PropertiesPanel::View& view) override;
    bool ApplyProperties() override;
    bool DeleteSelection() override;
    bool MoveSelectionToView() override;
    bool GetPoint(Vec3& position, float& heading) override;
    bool SetPoint(const Vec3& position, float heading, bool commit) override;

    // ---- RenderExtension
    void DrawOverlay(IDirect3DDevice9* device, const Camera& camera, WorldRenderer& renderer) override;
    bool Pick(const Ray& ray, int& index, float& distance) const override;
    float PickTolerance() const override { return 0.0f; }
    void ReleaseGpu() override;

    // ---- ViewportTool: the gizmo on the selected sheet
    bool GizmoPivot(Vec3& pivot) override;
    void GizmoChanged(const Mat4& transform, bool done, bool keep) override;

    // ---- UndoParticipant
    u64 LatestUndoStamp() const override { return undo_.empty() ? 0 : undo_.back().stamp; }
    bool UndoLatest() override;

    struct Image {
        std::wstring file;          // relative to <game>\models, or a full path
        std::wstring name;
        int  pixelsWide = 0, pixelsHigh = 0;
        Vec3 position;
        Vec3 rotation;              // heading, pitch, roll in degrees
        Vec3 scale{ 1.0f, 1.0f, 1.0f };
        float opacity = 0.6f;
        bool visible = true;
        bool locked = false;
        bool onTop = false;         // drawn over the world, not hidden by it
    };

private:
    void Import();
    void Select(int index);
    int  Selected() const;
    Image* SelectedImage();
    void Remember();                // an undo step of the list as it is now
    void Changed();                 // written, and the view told
    void Save() const;
    void Load();
    std::wstring ListPath() const;
    std::wstring FullPath(const Image& image) const;
    Mat4 Transform(const Image& image) const;   // the unit square (-1..1) to the world
    void Corners(const Image& image, Vec3 out[4]) const;
    IDirect3DTexture9* Texture(IDirect3DDevice9* device, const Image& image);
    WorldRenderer& Renderer() const { return host_.viewport->GetRenderer(); }
    nfs::World* World() const { return host_.world ? host_.world() : nullptr; }
    void Status(const std::wstring& text) const { if (host_.status) host_.status(text); }

    ReferenceImagesHost host_;
    std::vector<Image> images_;
    bool shown_ = true;
    size_t version_ = 0;

    struct CachedTexture { IDirect3DTexture9* texture = nullptr; bool failed = false; };
    std::map<std::wstring, CachedTexture> textures_;   // by full path

    struct UndoStep {
        u64 stamp = 0;
        std::vector<Image> images;
        int selected = -1;
    };
    std::vector<UndoStep> undo_;
    bool dragging_ = false;         // the gizmo or G/R has the selected sheet
    Image dragStart_;
};
