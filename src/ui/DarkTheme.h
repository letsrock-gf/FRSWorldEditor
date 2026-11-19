#pragma once

#include <windows.h>

// A dark look for the whole editor.
//
// Win32 has no switch for it, so it is put together from what the system
// does offer and what has to be drawn by hand:
//   - the dark title bar and the "DarkMode_Explorer" control themes Windows
//     ships for its own programs (trees, lists, buttons, scroll bars);
//   - dark popup menus, through the app-mode call uxtheme exports by ordinal;
//   - the menu bar, the status bar and the tab strip painted here, since
//     nothing themes those;
//   - the system colours as this program sees them, so that code drawing with
//     GetSysColor -- the property grid among it -- comes out dark as well.
//
// FRS_LIGHT=1 in the environment leaves the editor as Windows draws it.
namespace dark {

// Once, before the first window is made.
void Init();

// Themes a top-level window and every control in it. Windows and dialogs are
// adopted by themselves the first time they are activated; this is for one
// that has to be right before it shows.
void Adopt(HWND window);

bool Enabled();

} // namespace dark
