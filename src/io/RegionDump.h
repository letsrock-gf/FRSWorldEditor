#pragma once

#include <string>

namespace nfs { class World; }

namespace io {

// Writes the whole loaded region to `folder` as plain files, for tools outside
// the editor: every model the viewport draws (model space, game axes, OBJ with
// vertex colours), one line per drawn placement with its world matrix, the
// textures those models use (TGA), the collision polygons and a section list.
// Started by the environment variable FRS_DUMP_REGION=<folder> once a region
// has loaded (ui/MainWindow.cpp). Logs one summary line; false when the folder
// cannot be written.
bool DumpRegion(const nfs::World& world, const std::wstring& folder);

} // namespace io
