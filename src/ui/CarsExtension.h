#pragma once

#include "ui/EditorExtension.h"

#include <memory>

namespace nfs { class World; }

// Cars (ui/CarsExtension.cpp): the game's cars as a Library shelf, placed as
// models of their own (io/WorldMesh.h StageCarModel), painted in Properties.
std::unique_ptr<EditorExtension> CreateCarsExtension(MainWindow& window);

// The paint rows for a placed car model (none for any other model).
void AddCarPaintRows(nfs::World& world, u32 hash, PropertiesPanel::View& view);
