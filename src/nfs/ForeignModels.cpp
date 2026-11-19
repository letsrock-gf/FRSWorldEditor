#include "nfs/ForeignModels.h"

#include <algorithm>

namespace nfs {

namespace {
std::vector<const ForeignModels*>& Sources() {
    static std::vector<const ForeignModels*> sources;
    return sources;
}
}

void AddForeignModels(const ForeignModels* source) {
    auto& sources = Sources();
    if (source && std::find(sources.begin(), sources.end(), source) == sources.end()) sources.push_back(source);
}

void RemoveForeignModels(const ForeignModels* source) {
    auto& sources = Sources();
    sources.erase(std::remove(sources.begin(), sources.end(), source), sources.end());
}

const std::vector<const ForeignModels*>& ForeignSources() { return Sources(); }

const ForeignModels* ForeignSourceOf(u32 hash) {
    for (const ForeignModels* source : Sources())
        if (source->FindModel(hash)) return source;
    return nullptr;
}

const SolidObject* FindForeignModel(u32 hash) {
    for (const ForeignModels* source : Sources())
        if (const SolidObject* model = source->FindModel(hash)) return model;
    return nullptr;
}

const Texture* FindForeignTexture(u32 hash) {
    for (const ForeignModels* source : Sources())
        if (const Texture* texture = source->FindTexture(hash)) return texture;
    return nullptr;
}

} // namespace nfs
