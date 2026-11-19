#pragma once

#include "nfs/CustomRegions.h"
#include "ui/EditorExtension.h"

#include <windows.h>

#include <functional>
#include <string>
#include <vector>

namespace nfs { class World; }

struct RegionToolsHost {
    HWND  window = nullptr;
    HMENU menu = nullptr;
    std::function<nfs::World*()> world;
    std::function<std::wstring()> gameFolder;
    std::function<void(const std::wstring&)> status;
    // The region list read again, then `region` opened ("" keeps the first).
    std::function<void(const std::wstring& region)> reopenRegions;
    std::function<bool()> saveEdits;   // Ctrl+Shift+S: the placements and collision written
};

// File > New Region: a new world beside the game's own (nfs/CustomRegions.h,
// io/RegionCreate.h), started as a copy of a shipped region and reached as a
// track of its own in Quick Race > Free Run. File > Remove This Region takes a
// region the editor made away again, with every edit made in it.
//
// While one is open, its per-track files are brought up to date whenever the
// editor saves or starts the game, and the game starts straight in it
// (FRSFastTest dev start: its track, free roam).
class RegionTools : public EditorExtension {
public:
    explicit RegionTools(RegionToolsHost host) : host_(std::move(host)) {}

    static constexpr int kNewCommand = 45311;
    static constexpr int kRemoveCommand = 45312;

    void OnCreate() override;
    bool OnCommand(int id) override;
    void UpdateMenuChecks(HMENU menu) override;
    void OnRegionLoaded() override;
    void OnRegionClosed() override { open_ = {}; isCustom_ = false; }
    void OnSaved() override { Sync(); }
    void OnBeforeLaunch() override;
    void Tick(float deltaSeconds) override;
    void DrawHud(float x, float y, float width, float height) override;

private:
    void Sync();
    bool GenerateTerrain();   // an empty region's ground, from its settings
    void Create();
    void Remove();
    void RestoreFastTest();
    std::wstring FastTestIni() const;
    std::wstring Game() const { return host_.gameFolder ? host_.gameFolder() : L""; }
    void Status(const std::wstring& text) const { if (host_.status) host_.status(text); }

    RegionToolsHost host_;
    nfs::CustomRegion open_;
    bool isCustom_ = false;

    // The New Region window.
    bool asking_ = false;
    std::vector<std::wstring> sources_;
    int source_ = 0;
    char title_[32] = {};
    bool empty_ = false;
    int preset_ = 1;
    int sizeChoice_ = 1;               // 1, 2 or 4 km
    int seed_ = 1;
    std::wstring createMessage_;

    // An empty region being made: 1 its ground is generated when it opens,
    // 2 it opens again (the roads tools read the new ground as it opens),
    // 3 the bake runs once it has.
    int pendingStage_ = 0;
    std::wstring pendingName_;

    // FRSFastTest keys set for one launch, put back after.
    bool restorePending_ = false;
    DWORD restoreAt_ = 0;
    std::vector<std::pair<std::wstring, std::wstring>> savedKeys_;
    std::vector<std::wstring> absentKeys_;

    // An empty region: sections written with some of the city still in them
    // (an import rebuilds a section from its bytes) are written again without it.
    bool cleanupDue_ = false;
    int cleanupTries_ = 0;

    int selfTest_ = 0;                 // FRS_REGION_SELFTEST
    bool importTested_ = false;        // FRS_IMPORT_SELFTEST
    bool importReopen_ = false;
    DWORD selfTestAt_ = 0;
};
