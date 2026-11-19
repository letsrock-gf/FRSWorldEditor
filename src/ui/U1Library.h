#pragma once

#include "ui/EditorExtension.h"

#include <memory>
#include <string>

class MainWindow;

// Need for Speed Underground's models in the model library.
//
// Objects > Models from Need for Speed Underground points the editor at that
// game's folder. Its regions (L1RA, the city, then the drag, drift and test
// track areas) are read in the background, read only, and every model they
// hold that the open region does not is listed in the Library beside the
// region's own -- the "Game" filter there tells the two apart -- with its
// picture, and can be placed like any other. The editor shows it from the
// first game's copy; saving converts it to this game's layout
// (io/U1Convert.h) and puts it, with its textures, in the region's library
// (io/PlacementWrite.cpp), from where every section can place it.
//
// The folder and the switch are kept in the editor's settings and the
// models read again at every start.
std::unique_ptr<EditorExtension> CreateU1Library(MainWindow& window);

// Converts every model of the game in `folder` and reads it back as this
// game's (FRSWorldEditor.exe --u1-selftest <folder>): the result goes to the
// log. 0 when every model came back the same.
int RunU1SelfTest(const std::wstring& folder);
