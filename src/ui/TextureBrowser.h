#pragma once

#include "nfs/World.h"

#include <windows.h>

#include <functional>
#include <vector>

// Lists the textures of a region and previews the selected one.
//
// The preview is decoded on the CPU rather than shown through the 3D device, so
// it reflects what the parser read out of the file. That is the point: when the
// world looks wrong, this separates "the texture is being read incorrectly" from
// "the texture is being applied incorrectly".
class TextureBrowser {
public:
    void Show(HINSTANCE instance, HWND parent, const nfs::World* world);
    void SetWorld(const nfs::World* world);

    // With hashes given, only those textures are listed: the selection's.
    // Empty lists the whole region.
    void SetFilter(std::vector<u32> hashes);
    void Close();

    HWND Handle() const { return dialog_; }

    // Asked to put an image in a texture's place (false) or take it back out
    // (true). The main window does it: it owns the world and the game folder.
    std::function<void(u32 hash, bool restore)> onReplace;

private:
    static INT_PTR CALLBACK DialogProc(HWND dialog, UINT message,
                                       WPARAM wParam, LPARAM lParam);

    void OnInit(HWND dialog);
    void Populate();
    void OnSelectionChanged();
    void DrawPreview(const DRAWITEMSTRUCT& item);
    void SaveSelected();
    void Layout();

    const nfs::Texture* Selected() const;

    HWND dialog_ = nullptr;
    HWND list_   = nullptr;
    const nfs::World* world_ = nullptr;

    std::vector<const nfs::Texture*> textures_;
    std::vector<u32> filter_;   // sorted

    // Decoded pixels of the current selection, top-down BGRA.
    nfs::Texture shown_;        // the largest stored version of the selected texture
    std::vector<u32> pixels_;
    u32 pixelWidth_  = 0;
    u32 pixelHeight_ = 0;
};
