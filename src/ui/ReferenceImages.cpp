#include "ui/ReferenceImages.h"

#include "core/Log.h"
#include "nfs/SceneryEdit.h"
#include "nfs/World.h"
#include "render/WorldRenderer.h"

#include <commdlg.h>
#include <wincodec.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <sstream>

namespace {

// The long side of a sheet at scale 1, in metres.
constexpr float kBaseSize = 100.0f;
// Laid this far over the ground found under the view, so it does not
// flicker against the road it was put on.
constexpr float kAboveGround = 0.3f;

// The rows of the properties panel (1..6 are the transform's own).
constexpr int kFieldScaleX = 61, kFieldScaleY = 62, kFieldScaleZ = 63, kFieldOpacity = 64,
              kFieldVisible = 65, kFieldLocked = 66, kFieldOnTop = 67, kFieldName = 68;

std::string Utf8(const std::wstring& text) {
    if (text.empty()) return {};
    const int size = ::WideCharToMultiByte(CP_UTF8, 0, text.c_str(), (int)text.size(), nullptr, 0, nullptr, nullptr);
    std::string out((size_t)size, '\0');
    ::WideCharToMultiByte(CP_UTF8, 0, text.c_str(), (int)text.size(), out.data(), size, nullptr, nullptr);
    return out;
}

std::wstring FromUtf8(const std::string& text) {
    if (text.empty()) return {};
    const int size = ::MultiByteToWideChar(CP_UTF8, 0, text.c_str(), (int)text.size(), nullptr, 0);
    std::wstring out((size_t)size, L'\0');
    ::MultiByteToWideChar(CP_UTF8, 0, text.c_str(), (int)text.size(), out.data(), size);
    return out;
}

std::wstring Number(float value) {
    wchar_t text[32];
    ::swprintf_s(text, L"%.3f", value);
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

bool SameImage(const ReferenceImages::Image& a, const ReferenceImages::Image& b) {
    const auto same = [](const Vec3& x, const Vec3& y) { return x.x == y.x && x.y == y.y && x.z == y.z; };
    return a.file == b.file && a.name == b.name && same(a.position, b.position) && same(a.rotation, b.rotation) &&
           same(a.scale, b.scale) && a.opacity == b.opacity && a.visible == b.visible && a.locked == b.locked &&
           a.onTop == b.onTop;
}

bool SameList(const std::vector<ReferenceImages::Image>& a, const std::vector<ReferenceImages::Image>& b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i)
        if (!SameImage(a[i], b[i])) return false;
    return true;
}

// A picture file as 32-bit BGRA through the system's WIC: PNG, JPEG, BMP,
// GIF, TIFF. `sizeOnly` reads the dimensions alone.
bool DecodeFile(const std::wstring& path, std::vector<u32>& bgra, UINT& width, UINT& height, bool sizeOnly) {
    IWICImagingFactory* factory = nullptr;
    HRESULT hr = ::CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory));
    if (hr == CO_E_NOTINITIALIZED) {
        ::CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        hr = ::CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory));
    }
    if (FAILED(hr) || !factory) return false;
    bool ok = false;
    IWICBitmapDecoder* decoder = nullptr;
    IWICBitmapFrameDecode* frame = nullptr;
    IWICFormatConverter* converter = nullptr;
    if (SUCCEEDED(factory->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ,
                                                     WICDecodeMetadataCacheOnDemand, &decoder)) &&
        SUCCEEDED(decoder->GetFrame(0, &frame)) && SUCCEEDED(frame->GetSize(&width, &height)) && width && height) {
        if (sizeOnly) {
            ok = true;
        } else if (SUCCEEDED(factory->CreateFormatConverter(&converter)) &&
                   SUCCEEDED(converter->Initialize(frame, GUID_WICPixelFormat32bppBGRA, WICBitmapDitherTypeNone,
                                                   nullptr, 0.0, WICBitmapPaletteTypeCustom))) {
            bgra.assign((size_t)width * height, 0);
            ok = SUCCEEDED(converter->CopyPixels(nullptr, width * 4, width * height * 4, (BYTE*)bgra.data()));
        }
    }
    if (converter) converter->Release();
    if (frame) frame->Release();
    if (decoder) decoder->Release();
    factory->Release();
    return ok;
}

// Half the size, each pixel the mean of four.
void Halve(std::vector<u32>& pixels, UINT& width, UINT& height) {
    const UINT w = std::max(1u, width / 2), h = std::max(1u, height / 2);
    std::vector<u32> out((size_t)w * h);
    for (UINT y = 0; y < h; ++y)
        for (UINT x = 0; x < w; ++x) {
            u32 sum[4] = {};
            for (UINT dy = 0; dy < 2; ++dy)
                for (UINT dx = 0; dx < 2; ++dx) {
                    const UINT sx = std::min(width - 1, x * 2 + dx), sy = std::min(height - 1, y * 2 + dy);
                    const u32 p = pixels[(size_t)sy * width + sx];
                    for (int c = 0; c < 4; ++c) sum[c] += (p >> (c * 8)) & 0xFF;
                }
            u32 v = 0;
            for (int c = 0; c < 4; ++c) v |= ((sum[c] + 2) / 4) << (c * 8);
            out[(size_t)y * w + x] = v;
        }
    pixels.swap(out);
    width = w;
    height = h;
}

std::wstring FileName(const std::wstring& path) {
    const size_t slash = path.find_last_of(L"\\/");
    return slash == std::wstring::npos ? path : path.substr(slash + 1);
}

std::wstring StemOf(const std::wstring& name) {
    const size_t dot = name.find_last_of(L'.');
    return dot == std::wstring::npos ? name : name.substr(0, dot);
}

bool IsFullPath(const std::wstring& path) {
    return path.size() > 2 && (path[1] == L':' || (path[0] == L'\\' && path[1] == L'\\'));
}

} // namespace

ReferenceImages::~ReferenceImages() {
    undo::Unregister(this);
    ReleaseGpu();
}

// ---------------------------------------------------------------------------
// The window
// ---------------------------------------------------------------------------

void ReferenceImages::OnCreate() {
    host_.viewport->AddTool(this);
    Renderer().AddExtension(this);
    undo::Register(this);
    if (!host_.menu) return;
    for (int i = 0; i < ::GetMenuItemCount(host_.menu); ++i) {
        wchar_t text[64] = {};
        ::GetMenuStringW(host_.menu, (UINT)i, text, 63, MF_BYPOSITION);
        if (std::wstring(text).find(L"View") == std::wstring::npos) continue;
        HMENU view = ::GetSubMenu(host_.menu, i);
        if (!view) break;
        ::AppendMenuW(view, MF_SEPARATOR, 0, nullptr);
        ::AppendMenuW(view, MF_STRING, kImportCommand, L"Import &Reference Image (blueprint)...");
        ::AppendMenuW(view, MF_STRING, kShowCommand, L"Show Reference &Images");
        break;
    }
}

bool ReferenceImages::OnCommand(int id) {
    if (id == kImportCommand) { Import(); return true; }
    if (id == kShowCommand) {
        shown_ = !shown_;
        if (!shown_ && Selected() >= 0) Select(-1);
        Status(shown_ ? L"Reference images shown." : L"Reference images hidden (View > Show Reference Images).");
        if (host_.menuChecks) host_.menuChecks();
        return true;
    }
    if (id < kLayFlatCommand || id > kNaturalSizeCommand) return false;
    Image* image = SelectedImage();
    if (!image) return true;
    Remember();
    switch (id) {
        case kLayFlatCommand:     image->rotation.y = 0.0f; image->rotation.z = 0.0f; break;
        case kStandUpCommand:     image->rotation.y = 0.0f; image->rotation.z = 90.0f; break;
        case kLockCommand:        image->locked = !image->locked; break;
        case kOnTopCommand:       image->onTop = !image->onTop; break;
        case kNaturalSizeCommand: image->scale = { 1.0f, 1.0f, 1.0f }; break;
        default: break;
    }
    Changed();
    if (id == kLockCommand)
        Status(image->locked ? L"Locked: clicks now go through it to the world. Unlock it from the Explorer's "
                               L"\"Reference images\" branch."
                             : L"Unlocked: a click picks it again.");
    return true;
}

void ReferenceImages::UpdateMenuChecks(HMENU menu) {
    ::CheckMenuItem(menu, kShowCommand, MF_BYCOMMAND | (shown_ ? MF_CHECKED : MF_UNCHECKED));
}

// ---------------------------------------------------------------------------
// The region
// ---------------------------------------------------------------------------

void ReferenceImages::OnRegionLoaded() {
    images_.clear();
    undo_.clear();
    dragging_ = false;
    Load();
    ++version_;
}

void ReferenceImages::OnRegionClosed() {
    images_.clear();
    undo_.clear();
    dragging_ = false;
    ReleaseGpu();
    ++version_;
}

std::wstring ReferenceImages::ListPath() const {
    nfs::World* world = World();
    const std::wstring folder = host_.modelsFolder ? host_.modelsFolder() : L"";
    if (!world || folder.empty()) return L"";
    return folder + L"\\editor-references-" + world->Region() + L".txt";
}

std::wstring ReferenceImages::FullPath(const Image& image) const {
    if (IsFullPath(image.file)) return image.file;
    const std::wstring folder = host_.modelsFolder ? host_.modelsFolder() : L"";
    return folder + L"\\" + image.file;
}

// One line per sheet, tab separated: file, name, pixels wide and high,
// position, heading/pitch/roll, scale, opacity, visible, locked, on top.
void ReferenceImages::Save() const {
    const std::wstring path = ListPath();
    if (path.empty()) return;
    if (images_.empty()) {
        ::DeleteFileW(path.c_str());
        return;
    }
    FILE* file = nullptr;
    if (::_wfopen_s(&file, path.c_str(), L"wb") != 0 || !file) {
        LOG_WARN("reference images: could not write %S", path.c_str());
        return;
    }
    std::fputs("# FRS World Editor reference images (editor only)\n", file);
    for (const Image& image : images_) {
        std::wstring name = image.name;
        for (wchar_t& c : name) if (c == L'\t' || c == L'\n' || c == L'\r') c = L' ';
        std::fprintf(file, "%s\t%s\t%d\t%d\t%.4f\t%.4f\t%.4f\t%.4f\t%.4f\t%.4f\t%.5f\t%.5f\t%.5f\t%.3f\t%d\t%d\t%d\n",
                     Utf8(image.file).c_str(), Utf8(name).c_str(), image.pixelsWide, image.pixelsHigh,
                     image.position.x, image.position.y, image.position.z, image.rotation.x, image.rotation.y,
                     image.rotation.z, image.scale.x, image.scale.y, image.scale.z, image.opacity,
                     image.visible ? 1 : 0, image.locked ? 1 : 0, image.onTop ? 1 : 0);
    }
    std::fclose(file);
}

void ReferenceImages::Load() {
    const std::wstring path = ListPath();
    if (path.empty()) return;
    FILE* file = nullptr;
    if (::_wfopen_s(&file, path.c_str(), L"rb") != 0 || !file) return;
    char line[4096];
    while (std::fgets(line, sizeof(line), file)) {
        std::string text = line;
        while (!text.empty() && (text.back() == '\n' || text.back() == '\r')) text.pop_back();
        if (text.empty() || text[0] == '#') continue;
        std::vector<std::string> parts;
        std::stringstream stream(text);
        std::string part;
        while (std::getline(stream, part, '\t')) parts.push_back(part);
        if (parts.size() < 17) continue;
        Image image;
        image.file = FromUtf8(parts[0]);
        image.name = FromUtf8(parts[1]);
        const auto f = [&](size_t i) { return (float)std::atof(parts[i].c_str()); };
        image.pixelsWide = std::atoi(parts[2].c_str());
        image.pixelsHigh = std::atoi(parts[3].c_str());
        image.position = { f(4), f(5), f(6) };
        image.rotation = { f(7), f(8), f(9) };
        image.scale = { f(10), f(11), f(12) };
        image.opacity = std::clamp(f(13), 0.0f, 1.0f);
        image.visible = parts[14] != "0";
        image.locked = parts[15] != "0";
        image.onTop = parts[16] != "0";
        images_.push_back(std::move(image));
    }
    std::fclose(file);
    if (!images_.empty()) LOG_INFO("reference images: %zu read from %S", images_.size(), path.c_str());
}

// ---------------------------------------------------------------------------
// Editing
// ---------------------------------------------------------------------------

int ReferenceImages::Selected() const {
    const int index = Renderer().ExtensionSelection(this);
    return index >= 0 && (size_t)index < images_.size() ? index : -1;
}

ReferenceImages::Image* ReferenceImages::SelectedImage() {
    const int index = Selected();
    return index >= 0 ? &images_[(size_t)index] : nullptr;
}

void ReferenceImages::Select(int index) {
    if (index >= 0) Renderer().SelectExtension(this, index);
    else if (Renderer().ExtensionSelection(this) >= 0) Renderer().SelectExtension(this, -1);
    if (host_.updateProperties) host_.updateProperties();
}

void ReferenceImages::Remember() {
    UndoStep step;
    step.stamp = undo::NextStamp();
    step.images = images_;
    step.selected = Selected();
    undo_.push_back(std::move(step));
    if (undo_.size() > 200) undo_.erase(undo_.begin());
}

bool ReferenceImages::UndoLatest() {
    if (undo_.empty()) return false;
    UndoStep step = std::move(undo_.back());
    undo_.pop_back();
    dragging_ = false;
    images_ = std::move(step.images);
    Select(step.selected >= 0 && (size_t)step.selected < images_.size() ? step.selected : -1);
    ++version_;
    Save();
    Status(L"Reference image change undone.");
    return true;
}

void ReferenceImages::Changed() {
    // A step that changed nothing is not one to undo.
    if (!undo_.empty() && !dragging_ && SameList(undo_.back().images, images_)) undo_.pop_back();
    ++version_;
    Save();
    if (host_.updateProperties) host_.updateProperties();
}

void ReferenceImages::Import() {
    nfs::World* world = World();
    const std::wstring folder = host_.modelsFolder ? host_.modelsFolder() : L"";
    if (!world || folder.empty()) {
        ::MessageBoxW(host_.window, L"Open a region first: reference images are kept per region.",
                      L"Reference image", MB_ICONINFORMATION | MB_OK);
        return;
    }
    wchar_t chosen[MAX_PATH] = {};
    OPENFILENAMEW dialog{};
    dialog.lStructSize = sizeof(dialog);
    dialog.hwndOwner = host_.window;
    dialog.lpstrFilter = L"Images (*.png;*.jpg;*.jpeg;*.bmp;*.gif;*.tif;*.tiff)\0*.png;*.jpg;*.jpeg;*.bmp;*.gif;*.tif;*.tiff\0"
                         L"All files (*.*)\0*.*\0";
    dialog.lpstrFile = chosen;
    dialog.nMaxFile = MAX_PATH;
    dialog.lpstrTitle = L"Import reference image (blueprint)";
    dialog.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    if (!::GetOpenFileNameW(&dialog)) return;
    const std::wstring source = chosen;

    std::vector<u32> unused;
    UINT width = 0, height = 0;
    if (!DecodeFile(source, unused, width, height, true)) {
        ::MessageBoxW(host_.window, (L"Windows could not read this picture:\n" + source).c_str(), L"Reference image",
                      MB_ICONWARNING | MB_OK);
        return;
    }

    // Copied into <game>\models, so it travels with the world.
    const std::wstring store = folder + L"\\editor-references";
    ::CreateDirectoryW(store.c_str(), nullptr);
    std::wstring name = FileName(source);
    std::wstring target = store + L"\\" + name;
    if (::_wcsicmp(target.c_str(), source.c_str()) != 0) {
        const size_t dot = name.find_last_of(L'.');
        const std::wstring stem = StemOf(name), extension = dot == std::wstring::npos ? L"" : name.substr(dot);
        for (int n = 2; ::GetFileAttributesW(target.c_str()) != INVALID_FILE_ATTRIBUTES && n < 1000; ++n) {
            name = stem + L" (" + std::to_wstring(n) + L")" + extension;
            target = store + L"\\" + name;
        }
        if (!::CopyFileW(source.c_str(), target.c_str(), TRUE)) {
            ::MessageBoxW(host_.window, (L"Could not copy the picture to\n" + target).c_str(), L"Reference image",
                          MB_ICONWARNING | MB_OK);
            return;
        }
    }

    Image image;
    image.file = L"editor-references\\" + name;
    image.name = StemOf(name);
    image.pixelsWide = (int)width;
    image.pixelsHigh = (int)height;
    Vec3 centre;
    if (host_.viewCentre && host_.viewCentre(centre)) {
        image.position = centre;
    } else {
        const Vec3 eye = host_.viewport->GetCamera().Position();
        image.position = { eye.x, eye.y, eye.z - 20.0f };
    }
    image.position.z += kAboveGround;
    // Turned with the view, so its top is what lies ahead.
    // (Heading h takes the picture's top, local +Y, to (-sin h, cos h).)
    const Vec3 forward = host_.viewport->GetCamera().Forward();
    if (std::fabs(forward.x) + std::fabs(forward.y) > 1e-3f)
        image.rotation.x = std::fmod(Degrees(std::atan2(-forward.x, forward.y)) + 360.0f, 360.0f);

    Remember();
    images_.push_back(std::move(image));
    shown_ = true;
    ++version_;
    Save();
    Select((int)images_.size() - 1);
    Status(L"Reference image \"" + images_.back().name + L"\" laid on the ground: move, turn and scale it with the "
           L"gizmo or in the properties; Lock it to click the world through it.");
    if (host_.menuChecks) host_.menuChecks();
}

bool ReferenceImages::DeleteSelection() {
    const int index = Selected();
    if (index < 0) return false;
    Remember();
    const std::wstring name = images_[(size_t)index].name;
    images_.erase(images_.begin() + index);
    Select(-1);
    ++version_;
    Save();
    Status(L"Reference image \"" + name + L"\" removed (Ctrl+Z brings it back).");
    return true;
}

bool ReferenceImages::MoveSelectionToView() {
    Image* image = SelectedImage();
    if (!image) return false;
    Vec3 centre;
    if (!host_.viewCentre || !host_.viewCentre(centre)) return true;
    Remember();
    image->position = { centre.x, centre.y, centre.z + kAboveGround };
    Changed();
    return true;
}

bool ReferenceImages::GetPoint(Vec3& position, float& heading) {
    const Image* image = SelectedImage();
    if (!image || image->locked) return false;
    position = image->position;
    heading = image->rotation.x;
    return true;
}

bool ReferenceImages::SetPoint(const Vec3& position, float heading, bool commit) {
    Image* image = SelectedImage();
    if (!image || image->locked) return false;
    if (!dragging_) {
        Remember();
        dragStart_ = *image;
        dragging_ = true;
    }
    image->position = position;
    image->rotation.x = heading;
    if (commit) {
        dragging_ = false;
        if (SameImage(*image, dragStart_)) undo_.pop_back();
        Save();
    }
    if (host_.updateProperties) host_.updateProperties();
    return true;
}

bool ReferenceImages::GizmoPivot(Vec3& pivot) {
    const Image* image = SelectedImage();
    if (!image || image->locked || !shown_) return false;
    pivot = image->position;
    return true;
}

void ReferenceImages::GizmoChanged(const Mat4& transform, bool done, bool keep) {
    Image* image = SelectedImage();
    if (!image) { dragging_ = false; return; }
    if (!dragging_) {
        Remember();
        dragStart_ = *image;
        dragging_ = true;
    }
    if (done && !keep) {
        *image = dragStart_;
        dragging_ = false;
        undo_.pop_back();
        if (host_.updateProperties) host_.updateProperties();
        return;
    }
    // `transform` takes the pivot's frame (no turn, unit scale) to where it
    // is now: a move, a turn about a world axis, or one scale factor.
    const Vec3 row0{ transform.m[0][0], transform.m[0][1], transform.m[0][2] };
    const float factor = std::max(1e-4f, Length(row0));
    Mat4 turn = Mat4::Identity();
    for (int r = 0; r < 3; ++r)
        for (int c = 0; c < 3; ++c) turn.m[r][c] = transform.m[r][c] / factor;
    const Mat4 before = nfs::ComposePlacement({}, dragStart_.rotation.x, dragStart_.rotation.y,
                                              dragStart_.rotation.z, 1.0f);
    Vec3 unusedPosition;
    float heading = 0.0f, pitch = 0.0f, roll = 0.0f, unusedScale = 1.0f;
    nfs::DecomposePlacement(before * turn, unusedPosition, heading, pitch, roll, unusedScale);
    image->position = { transform.m[3][0], transform.m[3][1], transform.m[3][2] };
    image->rotation = { heading, pitch, roll };
    image->scale = dragStart_.scale * factor;
    if (done) {
        dragging_ = false;
        if (SameImage(*image, dragStart_)) undo_.pop_back();
        Save();
    }
    if (host_.updateProperties) host_.updateProperties();
}

// ---------------------------------------------------------------------------
// The Explorer and the properties
// ---------------------------------------------------------------------------

size_t ReferenceImages::SceneSignature() const {
    return images_.empty() ? 0 : images_.size() * 7919 + version_ * 31 + (shown_ ? 1 : 0);
}

void ReferenceImages::PopulateTree(SceneTreeBuilder& tree) {
    if (images_.empty()) return;
    HTREEITEM root = tree.insert(TVI_ROOT, L"Reference images (" + std::to_wstring(images_.size()) + L")", -1);
    size_t shown = 0;
    for (size_t i = 0; i < images_.size(); ++i) {
        std::wstring label = images_[i].name;
        if (images_[i].locked) label += L"  [locked]";
        if (!images_[i].visible) label += L"  [hidden]";
        if (!tree.matches(label)) continue;
        tree.insert(root, label, (intptr_t)i);
        ++shown;
    }
    tree.finish(root, shown, true);
}

void ReferenceImages::OnTreeItem(intptr_t code) {
    if (code < 0 || (size_t)code >= images_.size()) return;
    shown_ = true;
    Select((int)code);
    Vec3 corners[4];
    Corners(images_[(size_t)code], corners);
    Aabb box;
    for (const Vec3& corner : corners) box.Add(corner);
    host_.viewport->FrameBounds(box);
    if (host_.menuChecks) host_.menuChecks();
}

bool ReferenceImages::StatusText(std::wstring& text) const {
    const int index = Selected();
    if (index < 0) return false;
    const Image& image = images_[(size_t)index];
    const float aspect = image.pixelsWide > 0 && image.pixelsHigh > 0
                             ? (float)image.pixelsWide / (float)image.pixelsHigh : 1.0f;
    const float wide = kBaseSize * (aspect >= 1.0f ? 1.0f : aspect) * image.scale.x;
    const float high = kBaseSize * (aspect >= 1.0f ? 1.0f / aspect : 1.0f) * image.scale.y;
    wchar_t buffer[256];
    ::swprintf_s(buffer, L"Reference image \"%s\": %.1f x %.1f m%s", image.name.c_str(), wide, high,
                 image.locked ? L" (locked)" : L"");
    text = buffer;
    return true;
}

bool ReferenceImages::FillProperties(PropertiesPanel::View& view) {
    const int index = Selected();
    if (index < 0) return false;
    const Image& image = images_[(size_t)index];
    view.title = image.name;
    view.moveLabel = L"Move to view centre";
    view.removeLabel = L"Delete image";

    const auto add = [&view](const wchar_t* group, const wchar_t* name, std::wstring value,
                             const wchar_t* description, int field, bool editable,
                             std::vector<std::wstring> choices = {}) {
        PropertiesPanel::Row row;
        row.group = group;
        row.name = name;
        row.value = std::move(value);
        row.description = description;
        row.field = field;
        row.editable = editable;
        row.choices = std::move(choices);
        view.rows.push_back(std::move(row));
    };
    const std::vector<std::wstring> yesNo{ L"Yes", L"No" };
    const wchar_t* place = L"Where the middle of the picture stands, in metres.";
    const wchar_t* turn = L"Heading turns it on the ground; pitch and roll tilt it (roll 90 stands it upright, as "
                          L"a wall would).";
    const wchar_t* size = L"Scale on each of its axes: X along the picture's width, Y along its height (its long "
                          L"side is 100 m at 1), Z across the sheet (no visible effect on a flat picture).";

    add(L"Reference image", L"Name", image.name, L"What the Explorer lists it as.", kFieldName, true);
    add(L"Transform", L"X", Number(image.position.x), place, 1, true);
    add(L"Transform", L"Y", Number(image.position.y), place, 2, true);
    add(L"Transform", L"Z (height)", Number(image.position.z), place, 3, true);
    add(L"Transform", L"Heading", Number(image.rotation.x), turn, 4, true);
    add(L"Transform", L"Pitch", Number(image.rotation.y), turn, 5, true);
    add(L"Transform", L"Roll", Number(image.rotation.z), turn, 6, true);
    add(L"Transform", L"Scale X", Number(image.scale.x), size, kFieldScaleX, true);
    add(L"Transform", L"Scale Y", Number(image.scale.y), size, kFieldScaleY, true);
    add(L"Transform", L"Scale Z", Number(image.scale.z), size, kFieldScaleZ, true);
    add(L"Look", L"Opacity", Number(image.opacity), L"0 is invisible, 1 hides what is behind it.", kFieldOpacity,
        true);
    add(L"Look", L"Visible", image.visible ? L"Yes" : L"No", L"Hidden ones stay listed in the Explorer.",
        kFieldVisible, true, yesNo);
    add(L"Look", L"Over the world", image.onTop ? L"Yes" : L"No",
        L"Yes: drawn over everything, never hidden by the ground or buildings. No: the world in front of it "
        L"hides it.", kFieldOnTop, true, yesNo);
    add(L"Look", L"Locked", image.locked ? L"Yes" : L"No",
        L"A locked picture cannot be clicked or moved: clicks reach the world through it. The Explorer's "
        L"\"Reference images\" branch selects it again.", kFieldLocked, true, yesNo);

    std::wstring statusLine;
    StatusText(statusLine);
    const size_t colon = statusLine.find(L": ");
    add(L"File", L"Size", colon == std::wstring::npos ? L"" : statusLine.substr(colon + 2),
        L"Width by height on the ground.", 0, false);
    add(L"File", L"Pixels", std::to_wstring(image.pixelsWide) + L" x " + std::to_wstring(image.pixelsHigh),
        L"The picture's own size.", 0, false);
    add(L"File", L"File", FullPath(image), L"The copy kept with the world, in <game>\\models\\editor-references.",
        0, false);

    view.actions.push_back({ L"Lay flat", kLayFlatCommand, L"Orientation",
                             L"Pitch and roll back to 0: flat on the ground, as a map." });
    view.actions.push_back({ L"Stand upright", kStandUpCommand, L"Orientation",
                             L"Roll 90: standing like a wall, for an elevation or a facade." });
    view.actions.push_back({ L"Natural size", kNaturalSizeCommand, L"Orientation",
                             L"Scale back to 1 on every axis: the long side 100 m." });
    view.actions.push_back({ image.onTop ? L"Draw in the world" : L"Draw over the world", kOnTopCommand,
                             L"Orientation", L"Whether the ground and buildings in front of it hide it." });
    view.actions.push_back({ image.locked ? L"Unlock" : L"Lock", kLockCommand, L"Orientation",
                             L"A locked picture lets clicks through to the world." });
    return true;
}

bool ReferenceImages::ApplyProperties() {
    Image* image = SelectedImage();
    if (!image) return false;
    PropertiesPanel* properties = host_.properties;
    if (!properties) return true;
    Remember();
    const auto number = [&](int field, float& into) {
        float value = 0.0f;
        std::wstring text;
        if (properties->ReadText(field, text) && ParseNumber(text, value)) into = value;
    };
    const auto flag = [&](int field, bool& into) {
        std::wstring text;
        if (properties->ReadText(field, text)) into = text == L"Yes";
    };
    number(1, image->position.x);
    number(2, image->position.y);
    number(3, image->position.z);
    number(4, image->rotation.x);
    number(5, image->rotation.y);
    number(6, image->rotation.z);
    Vec3 scale = image->scale;
    number(kFieldScaleX, scale.x);
    number(kFieldScaleY, scale.y);
    number(kFieldScaleZ, scale.z);
    // A sheet of no size cannot be seen or picked again.
    if (std::fabs(scale.x) >= 0.001f && std::fabs(scale.y) >= 0.001f && std::fabs(scale.z) >= 0.001f)
        image->scale = scale;
    number(kFieldOpacity, image->opacity);
    image->opacity = std::clamp(image->opacity, 0.0f, 1.0f);
    flag(kFieldVisible, image->visible);
    flag(kFieldOnTop, image->onTop);
    flag(kFieldLocked, image->locked);
    std::wstring name;
    if (properties->ReadText(kFieldName, name) && !name.empty()) image->name = name;
    Changed();
    return true;
}

// ---------------------------------------------------------------------------
// Drawing and picking
// ---------------------------------------------------------------------------

Mat4 ReferenceImages::Transform(const Image& image) const {
    const float aspect = image.pixelsWide > 0 && image.pixelsHigh > 0
                             ? (float)image.pixelsWide / (float)image.pixelsHigh : 1.0f;
    const float halfWide = 0.5f * kBaseSize * (aspect >= 1.0f ? 1.0f : aspect);
    const float halfHigh = 0.5f * kBaseSize * (aspect >= 1.0f ? 1.0f / aspect : 1.0f);
    const Mat4 size = Mat4::Scale({ halfWide * image.scale.x, halfHigh * image.scale.y, image.scale.z });
    return size * nfs::ComposePlacement(image.position, image.rotation.x, image.rotation.y, image.rotation.z, 1.0f);
}

void ReferenceImages::Corners(const Image& image, Vec3 out[4]) const {
    const Mat4 world = Transform(image);
    out[0] = world.TransformPoint({ -1.0f, -1.0f, 0.0f });
    out[1] = world.TransformPoint({ 1.0f, -1.0f, 0.0f });
    out[2] = world.TransformPoint({ 1.0f, 1.0f, 0.0f });
    out[3] = world.TransformPoint({ -1.0f, 1.0f, 0.0f });
}

bool ReferenceImages::Pick(const Ray& ray, int& index, float& distance) const {
    index = -1;
    if (!shown_) return false;
    for (size_t i = 0; i < images_.size(); ++i) {
        const Image& image = images_[i];
        if (!image.visible || image.locked) continue;
        Vec3 c[4];
        Corners(image, c);
        float hit = IntersectRayTriangle(ray, c[0], c[1], c[2]);
        if (hit < 0.0f) hit = IntersectRayTriangle(ray, c[0], c[2], c[3]);
        if (hit < 0.0f || (index >= 0 && hit >= distance)) continue;
        index = (int)i;
        distance = hit;
    }
    return index >= 0;
}

IDirect3DTexture9* ReferenceImages::Texture(IDirect3DDevice9* device, const Image& image) {
    const std::wstring path = FullPath(image);
    CachedTexture& cached = textures_[path];
    if (cached.texture || cached.failed) return cached.texture;
    cached.failed = true;   // until it works

    std::vector<u32> pixels;
    UINT width = 0, height = 0;
    if (!DecodeFile(path, pixels, width, height, false)) {
        LOG_WARN("reference images: could not read %S", path.c_str());
        return nullptr;
    }
    D3DCAPS9 caps{};
    device->GetDeviceCaps(&caps);
    const UINT most = std::max<UINT>(256, std::min<UINT>(caps.MaxTextureWidth, caps.MaxTextureHeight));
    while (width > most || height > most) Halve(pixels, width, height);

    IDirect3D9* d3d = nullptr;
    bool autoMips = false;
    if (SUCCEEDED(device->GetDirect3D(&d3d)) && d3d) {
        D3DDEVICE_CREATION_PARAMETERS creation{};
        D3DDISPLAYMODE mode{};
        device->GetCreationParameters(&creation);
        d3d->GetAdapterDisplayMode(creation.AdapterOrdinal, &mode);
        autoMips = (caps.Caps2 & D3DCAPS2_CANAUTOGENMIPMAP) &&
                   d3d->CheckDeviceFormat(creation.AdapterOrdinal, creation.DeviceType, mode.Format,
                                          D3DUSAGE_AUTOGENMIPMAP, D3DRTYPE_TEXTURE, D3DFMT_A8R8G8B8) == D3D_OK;
        d3d->Release();
    }
    IDirect3DTexture9* texture = nullptr;
    if (FAILED(device->CreateTexture(width, height, autoMips ? 0 : 1, autoMips ? D3DUSAGE_AUTOGENMIPMAP : 0,
                                     D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &texture, nullptr)) || !texture) {
        LOG_WARN("reference images: no texture of %ux%u for %S", width, height, path.c_str());
        return nullptr;
    }
    D3DLOCKED_RECT locked{};
    if (FAILED(texture->LockRect(0, &locked, nullptr, 0))) {
        texture->Release();
        return nullptr;
    }
    for (UINT y = 0; y < height; ++y)
        std::memcpy((u8*)locked.pBits + (size_t)y * locked.Pitch, pixels.data() + (size_t)y * width, width * 4);
    texture->UnlockRect(0);
    if (autoMips) texture->GenerateMipSubLevels();
    cached.texture = texture;
    cached.failed = false;
    return texture;
}

void ReferenceImages::ReleaseGpu() {
    for (auto& [path, cached] : textures_)
        if (cached.texture) cached.texture->Release();
    textures_.clear();
}

void ReferenceImages::DrawOverlay(IDirect3DDevice9* device, const Camera& camera, WorldRenderer& renderer) {
    (void)camera;
    if (!shown_ || images_.empty()) return;

    struct Vertex { float x, y, z; D3DCOLOR colour; float u, v; };
    constexpr DWORD kFvf = D3DFVF_XYZ | D3DFVF_DIFFUSE | D3DFVF_TEX1;
    struct LineVertex { float x, y, z; D3DCOLOR colour; };
    constexpr DWORD kLineFvf = D3DFVF_XYZ | D3DFVF_DIFFUSE;

    renderer.BeginOverlay(device);
    DWORD saved[5] = {};
    const D3DSAMPLERSTATETYPE kept[5] = { D3DSAMP_ADDRESSU, D3DSAMP_ADDRESSV, D3DSAMP_MINFILTER,
                                          D3DSAMP_MAGFILTER, D3DSAMP_MIPFILTER };
    for (int i = 0; i < 5; ++i) device->GetSamplerState(0, kept[i], &saved[i]);
    device->SetSamplerState(0, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
    device->SetSamplerState(0, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
    device->SetSamplerState(0, D3DSAMP_MINFILTER, D3DTEXF_LINEAR);
    device->SetSamplerState(0, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR);
    device->SetSamplerState(0, D3DSAMP_MIPFILTER, D3DTEXF_LINEAR);
    device->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
    device->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_SRCALPHA);
    device->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA);
    device->SetRenderState(D3DRS_ZWRITEENABLE, FALSE);
    device->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
    device->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
    device->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
    device->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_MODULATE);
    device->SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_TEXTURE);
    device->SetTextureStageState(0, D3DTSS_ALPHAARG2, D3DTA_DIFFUSE);
    device->SetFVF(kFvf);

    const int selected = Selected();
    for (size_t i = 0; i < images_.size(); ++i) {
        const Image& image = images_[i];
        if (!image.visible) continue;
        IDirect3DTexture9* texture = Texture(device, image);
        Vec3 c[4];
        Corners(image, c);
        const D3DCOLOR tint = ((D3DCOLOR)(std::clamp(image.opacity, 0.0f, 1.0f) * 255.0f + 0.5f) << 24) |
                              (texture ? 0x00FFFFFFu : 0x00FF00FFu);
        const Vertex quad[4] = {
            { c[0].x, c[0].y, c[0].z, tint, 0.0f, 1.0f },
            { c[1].x, c[1].y, c[1].z, tint, 1.0f, 1.0f },
            { c[2].x, c[2].y, c[2].z, tint, 1.0f, 0.0f },
            { c[3].x, c[3].y, c[3].z, tint, 0.0f, 0.0f },
        };
        device->SetTexture(0, texture);
        if (!texture) {   // a file that would not read: a magenta sheet says so
            device->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_DIFFUSE);
            device->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1);
            device->SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_DIFFUSE);
        }
        device->SetRenderState(D3DRS_ZENABLE, image.onTop ? FALSE : TRUE);
        device->DrawPrimitiveUP(D3DPT_TRIANGLEFAN, 2, quad, sizeof(Vertex));
        if (!texture) {
            device->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
            device->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_MODULATE);
            device->SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_TEXTURE);
        }
    }

    // The selected sheet's outline, through everything.
    if (selected >= 0 && images_[(size_t)selected].visible) {
        Vec3 c[4];
        Corners(images_[(size_t)selected], c);
        const D3DCOLOR colour = images_[(size_t)selected].locked ? 0xFF8090A0 : 0xFFFFC832;
        const LineVertex outline[5] = {
            { c[0].x, c[0].y, c[0].z, colour }, { c[1].x, c[1].y, c[1].z, colour },
            { c[2].x, c[2].y, c[2].z, colour }, { c[3].x, c[3].y, c[3].z, colour },
            { c[0].x, c[0].y, c[0].z, colour },
        };
        device->SetTexture(0, nullptr);
        device->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_DIFFUSE);
        device->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1);
        device->SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_DIFFUSE);
        device->SetRenderState(D3DRS_ZENABLE, FALSE);
        device->SetFVF(kLineFvf);
        device->DrawPrimitiveUP(D3DPT_LINESTRIP, 4, outline, sizeof(LineVertex));
    }

    // Back to the overlay's own states, then the world's.
    device->SetTexture(0, nullptr);
    device->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
    device->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_DIFFUSE);
    device->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1);
    device->SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_DIFFUSE);
    device->SetTextureStageState(0, D3DTSS_ALPHAARG2, D3DTA_CURRENT);
    for (int i = 0; i < 5; ++i) device->SetSamplerState(0, kept[i], saved[i]);
    renderer.EndOverlay(device);
}
