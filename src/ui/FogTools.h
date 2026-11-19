#pragma once

#include "io/DistrictFog.h"
#include "nfs/CityZones.h"
#include "ui/EditorExtension.h"
#include "ui/UndoRegistry.h"

#include <windows.h>

#include <functional>
#include <string>
#include <vector>

namespace nfs { class World; }
class Viewport;

struct FogToolsHost {
    HMENU     menu = nullptr;
    Viewport* viewport = nullptr;
    std::function<nfs::World*()> world;
    std::function<std::wstring()> gameFolder;
    std::function<const std::vector<nfs::CityZone>&()> zones;   // the region's neighbourhoods
    std::function<void(size_t)> travelToZone;
    std::function<void(const std::wstring&)> status;
    std::function<void()> menuChecks;
};

// View > Fog by Neighbourhood: a fog of its own for each of the city's
// neighbourhoods -- day, night and rain -- written into the region file's
// parameter maps (io/DistrictFog). A neighbourhood left alone keeps the game's
// own fog. Every finished change is written at once, so the view (with View >
// Game Look on) and the next launch show it; Ctrl+Z takes it back.
class FogTools : public EditorExtension, public UndoParticipant {
public:
    explicit FogTools(FogToolsHost host) : host_(std::move(host)) {}
    ~FogTools() override { undo::Unregister(this); }

    static constexpr int kShowCommand = 45302;

    // ---- EditorExtension
    void OnCreate() override;
    bool OnCommand(int id) override;
    void UpdateMenuChecks(HMENU menu) override;
    void OnRegionLoaded() override;
    void OnRegionClosed() override { plan_ = {}; undo_.clear(); }
    void DrawHud(float x, float y, float width, float height) override;
    void Tick(float deltaSeconds) override;   // FRS_FOG_SELFTEST only

    // ---- UndoParticipant
    u64 LatestUndoStamp() const override { return undo_.empty() ? 0 : undo_.back().stamp; }
    bool UndoLatest() override;

private:
    struct Step { u64 stamp = 0; io::FogPlan before; };

    bool Commit();                                    // the plan to disk and to the game
    void Changed(const io::FogPlan& before);          // one finished edit: an undo step, then Commit
    void Track(const io::FogPlan& frameStart);        // after each widget
    bool LookEditor(const char* label, io::FogLook& look);
    io::DistrictFogEntry& EntryFor(const nfs::CityZone& zone);
    void Status(const std::wstring& text) const { if (host_.status) host_.status(text); }
    const std::vector<nfs::CityZone>& Zones() const;

    FogToolsHost host_;
    bool shown_ = false;
    std::wstring region_;
    io::FogPlan plan_;
    io::FogPlan editStart_;            // the plan when the widget being dragged was taken hold of
    std::vector<u8> shipped_;          // the region file as shipped: the looks a neighbourhood starts from
    std::vector<Step> undo_;
    std::wstring lastMessage_;
    int selfTest_ = 0;
};
