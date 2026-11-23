#include "ui/CarsExtension.h"

#include "core/Log.h"
#include "io/WorldMesh.h"
#include "io/WorldTexture.h"
#include "nfs/CarGeometry.h"
#include "nfs/GameFormat.h"
#include "render/ModelThumbnails.h"
#include "ui/MainWindow.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cwchar>

struct CarsAccess {
    explicit CarsAccess(MainWindow& window) : w(window) {}
    MainWindow& w;

    nfs::World* World() const { return w.world_.get(); }
    const std::wstring& GameFolder() const { return w.gameFolder_; }
    Viewport& View() const { return w.viewport_; }
    PropertiesPanel& Properties() const { return w.properties_; }
    void Status(const std::wstring& text) const { w.SetStatusText(2, text); }
    bool ViewCentre(Vec3& point) const { return w.ViewCentrePoint(point); }
    void Place(u32 hash, size_t target, const Vec3& point) const { w.PlaceModel(hash, false, target, point); }
    void UpdateProperties() const { w.UpdateProperties(); }
    ModelCatalog& Catalog() const { return w.catalog_; }
    void ShowLibrary() const { w.ShowSideTab(1); }
    void Message(const std::wstring& text) const {
        ::MessageBoxW(w.window_, text.c_str(), L"FRS World Editor - Cars", MB_ICONWARNING | MB_OK);
    }
};

namespace {

constexpr const wchar_t* kShelf = L"Cars";
// Properties fields of the paint rows (1..7 are the transform's).
enum : int { kHue = 41, kLightness, kSaturation, kFinish };

std::wstring Widen(const std::string& s) { return std::wstring(s.begin(), s.end()); }
u32 NameHash(const std::string& name) {   // bStringHash, as io::StringHash
    u32 hash = 0xFFFFFFFFu;
    for (unsigned char c : name) hash = hash * 33u + c;
    return hash;
}
// ponytail: one drawn side view for every car; the car's own mesh would mean reading its GEOMETRY.BIN.
bool CarPicture(std::vector<u32>& bgra) {
    const int n = (int)ModelThumbnails::kSize;
    bgra.assign((size_t)n * n, 0xFF2A2D33u);
    const auto inside = [n](int x, int y) {
        const float u = (float)x / n, v = (float)y / n;   // v down
        const bool body = u > 0.08f && u < 0.92f && v > 0.50f && v < 0.68f;
        const bool cabin = v > 0.36f && v <= 0.50f && u > 0.28f + (0.50f - v) * 0.9f && u < 0.70f - (0.50f - v) * 0.6f;
        const auto wheel = [&](float cx) { return (u - cx) * (u - cx) + (v - 0.68f) * (v - 0.68f) < 0.0085f; };
        return body || cabin ? 1 : wheel(0.27f) || wheel(0.74f) ? 2 : 0;
    };
    for (int y = 0; y < n; ++y)
        for (int x = 0; x < n; ++x)
            if (const int part = inside(x, y)) bgra[(size_t)y * n + x] = part == 1 ? 0xFFC8CDD6u : 0xFF15171Au;
    return true;
}

std::wstring Number(float value) {
    wchar_t text[32];
    std::swprintf(text, 32, L"%.4g", value);
    return text;
}

class CarsExtension final : public EditorExtension {
public:
    explicit CarsExtension(MainWindow& window) : app_(window) {}

    void CatalogRows(std::vector<ModelCatalog::Row>& rows) override {
        if (!app_.World() || !nfs::IsWorld()) return;
        if (cars_.empty()) {
            cars_ = nfs::ListCars(app_.GameFolder());
            LOG_INFO("cars: %zu in %S\\CARS for the Library", cars_.size(), app_.GameFolder().c_str());
        }
        for (const std::string& car : cars_) {
            ModelCatalog::Row row;
            row.name = Widen(car);
            row.home = L"CARS\\" + row.name;
            row.group = kShelf;
            row.owner = static_cast<EditorExtension*>(this);
            row.key = car;
            row.picture = CarPicture;   // the catalogue lists no row without a model or a picture
            rows.push_back(std::move(row));
        }
    }

    // A scripted run (tools/nfsw/test_cars.ps1): FRS_TEST_CAR="<car> <x> <y> <z>" places that car as a
    // double click would, then shows the Cars shelf (the Library has its rows by now).
    void OnRegionLoaded() override {
        char* test = nullptr;
        size_t length = 0;
        if (_dupenv_s(&test, &length, "FRS_TEST_CAR") || !test) return;
        char car[64] = {};
        Vec3 at{};
        if (std::sscanf(test, "%63s %f %f %f", car, &at.x, &at.y, &at.z) == 4) {
            Place(car, at);
            app_.ShowLibrary();
            app_.Catalog().ShowShelf(kShelf);
        }
        std::free(test);
    }

    bool DescribeRow(const ModelCatalog::Row& row, std::wstring& text) override {
        text = row.name + L": the car's stock body at LOD B, painted in Properties -- drag onto the map or "
                          L"double-click to place it at the view centre";
        return true;
    }

    void ActivateRow(const ModelCatalog::Row& row) override {
        Vec3 point;
        if (!app_.World() || !app_.ViewCentre(point)) {
            app_.Status(L"Point the middle of the view at the street where the car should stand.");
            return;
        }
        Place(row.key, point);
    }

    bool BeginDrag(const ModelCatalog::Row& row) override {
        dragging_ = row.key;
        return true;
    }
    void EndDrag(const Vec3* ground) override {
        const std::string car = dragging_;
        dragging_.clear();
        if (ground && !car.empty()) Place(car, *ground);
    }

    bool ApplyProperties() override {
        const int field = app_.Properties().LastField();
        if (field < kHue || field > kFinish) return false;
        nfs::World* world = app_.World();
        u32 section = 0;
        int placement = -1;
        if (!world || !app_.View().SelectedPlacement(section, placement) || section >= world->Loaded().size())
            return true;
        const auto& placements = world->Loaded()[section]->placements;
        if ((size_t)placement >= placements.size()) return true;
        const u32 hash = placements[(size_t)placement].instance.modelHash;
        nfs::World::NewModel* car = io::CarModelOf(*world, hash);
        if (!car) return true;
        float value = 0.0f;
        std::wstring text;
        if (app_.Properties().ReadNumber(kHue, value)) car->hue = std::clamp(value, 0.0f, 360.0f);
        if (app_.Properties().ReadNumber(kLightness, value)) car->lightness = std::clamp(value, 0.0f, 1.0f);
        if (app_.Properties().ReadNumber(kSaturation, value)) car->saturation = std::clamp(value, 0.0f, 1.0f);
        if (app_.Properties().ReadText(kFinish, text)) {
            car->metallic = text == L"Metallic";
            car->gloss = true;   // both REGPAINT and METPAINT reflect the environment map
        }
        io::ShowCarPaint(*world, hash);
        // Only the vertex colours change: written into the GPU buffer, not the section uploaded again.
        if (const nfs::SolidObject* model = world->FindModel(hash)) {
            std::vector<u32> colours(model->vertices.size());
            for (size_t i = 0; i < colours.size(); ++i) colours[i] = model->vertices[i].color;
            app_.View().GetRenderer().UpdateModelColours(hash, colours);
        }
        for (size_t i = 0; i < world->Loaded().size(); ++i)
            if (world->Loaded()[i]->info && world->Loaded()[i]->info->number == car->section)
                world->MutableLoaded(i)->placementsChanged = true;
        app_.UpdateProperties();
        return true;
    }

private:
    void Place(const std::string& car, const Vec3& point) {
        nfs::World* world = app_.World();
        if (!world) return;
        const size_t target = world->SectionForPlacement(point);
        if (target == SIZE_MAX || !world->Loaded()[target]->info) {
            app_.Status(L"No section here can take scenery.");
            return;
        }
        nfs::World::NewModel entry;
        entry.car = car;
        entry.section = world->Loaded()[target]->info->number;
        entry.lightness = 0.85f;   // light grey until painted
        for (int n = 1;; ++n) {
            entry.name = "FRS_" + car + "_" + std::to_string(n);
            entry.hash = NameHash(entry.name);
            if (!world->FindModel(entry.hash) && !io::CarModelOf(*world, entry.hash)) break;
        }
        std::wstring why;
        if (!io::StageCarModel(*world, app_.GameFolder(), entry, why)) {
            app_.Message(L"The car " + Widen(car) + L" could not be made into a model:\n\n" + why);
            return;
        }
        Reupload(*world, entry.hash);
        app_.Place(entry.hash, target, point);
        app_.Status(Widen(car) + L" placed as " + Widen(entry.name) + L" -- paint it in Properties; Ctrl+Shift+S writes it");
    }

    // The section holding the model and the texture sections taking its textures.
    void Reupload(nfs::World& world, u32 hash) {
        std::vector<size_t> touched;
        const nfs::SolidObject* model = world.FindModel(hash);
        for (size_t i = 0; i < world.Loaded().size(); ++i) {
            const nfs::LoadedSection* section = world.Loaded()[i].get();
            bool take = section->HoldsModel(hash);
            if (model && !take)
                for (u32 t : model->textureHashes)
                    if (const nfs::World::NewTexture* texture = io::NewTextureOf(world, t)) {
                        const auto hosts = io::NewTextureHosts(world, *texture);
                        if (std::find(hosts.begin(), hosts.end(), section) != hosts.end()) { take = true; break; }
                    }
            if (take) touched.push_back(i);
        }
        app_.View().GetRenderer().ReuploadSections(world, touched);
    }

    CarsAccess app_;
    std::vector<std::string> cars_;
    std::string dragging_;
};

} // namespace

std::unique_ptr<EditorExtension> CreateCarsExtension(MainWindow& window) {
    return std::make_unique<CarsExtension>(window);
}

void AddCarPaintRows(nfs::World& world, u32 hash, PropertiesPanel::View& view) {
    const nfs::World::NewModel* car = io::CarModelOf(world, hash);
    if (!car) return;
    // Above the object's other rows: the paint is what a car placement is for.
    std::vector<PropertiesPanel::Row> rest = std::move(view.rows);
    view.rows.clear();
    const auto add = [&](const wchar_t* name, std::wstring value, int field, const wchar_t* description,
                         float minimum, float maximum, bool choice) {
        PropertiesPanel::Row row;
        row.group = L"Paint";
        row.name = name;
        row.value = std::move(value);
        row.description = description;
        row.editable = true;
        row.field = field;
        row.minimum = minimum;
        row.maximum = maximum;
        if (choice) row.choices = { L"Yes", L"No" };
        view.rows.push_back(std::move(row));
    };
    add(L"Car", Widen(car->car), 0, L"The car of CARS this model is: its stock body at LOD B.", 0, 0, false);
    view.rows.back().editable = false;
    add(L"Hue", Number(car->hue), kHue, L"HLS hue of the paint, 0..360 degrees (0 red, 120 green, 240 blue).", 0, 360, false);
    add(L"Lightness", Number(car->lightness), kLightness, L"HLS lightness: 0 black, 0.5 the pure colour, 1 white.", 0, 1, false);
    add(L"Saturation", Number(car->saturation), kSaturation, L"HLS saturation: 0 grey, 1 the full colour.", 0, 1, false);
    // ponytail: no Finish row. Gloss / metallic would be the light material, which a world model does not
    // apply (21/11 in game); the colour goes into the car's skin texture instead (StageCarPaintTextures).
    view.rows.insert(view.rows.end(), rest.begin(), rest.end());
}
