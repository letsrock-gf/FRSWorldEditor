#include "GameLookAttributes.h"

#include <algorithm>
#include <cstdio>
#include <fstream>
#include <functional>
#include <iterator>

namespace gamelook {
namespace {

// ---------------------------------------------------------------- hashing

void Mix(u32& a, u32& b, u32& c) {
    a -= b; a -= c; a ^= (c >> 13);
    b -= c; b -= a; b ^= (a << 8);
    c -= a; c -= b; c ^= (b >> 13);
    a -= b; a -= c; a ^= (c >> 12);
    b -= c; b -= a; b ^= (a << 16);
    c -= a; c -= b; c ^= (b >> 5);
    a -= b; a -= c; a ^= (c >> 3);
    b -= c; b -= a; b ^= (a << 10);
    c -= a; c -= b; c ^= (b >> 15);
}

// ---------------------------------------------------------------- layout

constexpr u32 FourCC(char a, char b, char c, char d) {
    return u32(u8(a)) | (u32(u8(b)) << 8) | (u32(u8(c)) << 16) | (u32(u8(d)) << 24);
}

// Chunk ids are stored as little-endian u32 of the reversed tag ("Vers" reads "sreV").
constexpr u32 kChunkExpN = FourCC('N', 'p', 'x', 'E');
constexpr u32 kChunkPtrN = FourCC('N', 'r', 't', 'P');
constexpr u32 kPackMagic = FourCC('V', 'P', 'A', 'K');

constexpr u32 kPtrDepend  = 2;  // switches which part (DepN index) the next fixups live in
constexpr u32 kPtrPointer = 3;  // fixup offset -> destination offset in the .bin part

constexpr u8 kNodeInPlace = 0x40;  // collection entry holds the value in its data word

constexpr u8 kFieldArray    = 0x01;
constexpr u8 kFieldInLayout = 0x02;
constexpr u8 kFieldStatic   = 0x10;

struct FieldDef {
    u32 key = 0;
    u32 type = 0;
    u16 offset = 0;
    u16 size = 0;
    u16 maxCount = 0;
    u8  flags = 0;
};

struct ClassDef {
    u32 key = 0;
    std::vector<FieldDef> fields;
    size_t staticBin = 0;   // offset of the static block in the .bin part (0 = none)
    u32 staticSize = 0;
    u32 layoutSize = 0;
};

struct CollectionDef {
    u32 key = 0;
    u32 parent = 0;
    size_t vltOffset = 0;   // offset of the CollectionLoadData in the .vlt part
    u32 numEntries = 0;
    u32 typesCap = 0;
};

struct Vault {
    ByteSpan vlt;
    ByteSpan bin;
    std::unordered_map<u32, u32> vltPtr;  // .vlt fixup offset -> .bin offset
    std::unordered_map<u32, ClassDef> classes;
    std::vector<CollectionDef> collections;  // export order
};

using Values = std::unordered_map<u32, std::vector<float>>;

std::vector<float> WordsFrom(ByteSpan span, size_t offset, u32 size) {
    std::vector<float> out((size + 3) / 4, 0.f);
    if (offset + size > span.size) return out;
    std::memcpy(out.data(), span.data + offset, size);
    return out;
}

u32 Bits(float f) { u32 u; std::memcpy(&u, &f, 4); return u; }

bool ParseVault(Vault& v, std::string& error) {
    ByteSpan expN, ptrN;
    for (size_t p = 0; p + 8 <= v.vlt.size;) {
        u32 id = v.vlt.read<u32>(p), size = v.vlt.read<u32>(p + 4);
        if (size < 8 || p + size > v.vlt.size) break;
        if (id == kChunkExpN) expN = v.vlt.sub(p + 8, size - 8);
        if (id == kChunkPtrN) ptrN = v.vlt.sub(p + 8, size - 8);
        p += size;
    }
    if (expN.empty() || ptrN.empty()) { error = "vault without ExpN/PtrN chunk"; return false; }

    int part = -1;
    for (size_t p = 0; p + 12 <= ptrN.size; p += 12) {
        u32 fixup = ptrN.read<u32>(p);
        u16 type = ptrN.read<u16>(p + 4);
        u16 index = ptrN.read<u16>(p + 6);
        u32 dest = ptrN.read<u32>(p + 8);
        if (type == kPtrDepend) part = index;
        else if (type == kPtrPointer && part == 0) v.vltPtr[fixup] = dest;
    }

    const u32 kClassLoad = VltHash("Attrib::ClassLoadData");
    const u32 kCollectionLoad = VltHash("Attrib::CollectionLoadData");
    u32 count = expN.read<u32>(0);
    if (4 + size_t(count) * 16 > expN.size) { error = "ExpN table truncated"; return false; }
    auto binPtr = [&](size_t vltOff) -> size_t {
        auto it = v.vltPtr.find(u32(vltOff));
        return it == v.vltPtr.end() ? 0 : it->second;
    };
    for (u32 i = 0; i < count; ++i) {
        size_t e = 4 + size_t(i) * 16;
        u32 type = expN.read<u32>(e + 4), size = expN.read<u32>(e + 8), off = expN.read<u32>(e + 12);
        if (size_t(off) + size > v.vlt.size) continue;
        if (type == kClassLoad && size >= 0x20) {
            ClassDef c;
            c.key = v.vlt.read<u32>(off);
            u32 numFields = v.vlt.read<u32>(off + 8);
            size_t fieldsBin = binPtr(off + 12);
            c.staticSize = v.vlt.read<u32>(off + 16);
            c.staticBin = binPtr(off + 20);
            c.layoutSize = v.vlt.read<u32>(off + 24);
            for (u32 f = 0; f < numFields && fieldsBin; ++f) {
                size_t o = fieldsBin + size_t(f) * 16;
                if (o + 16 > v.bin.size) break;
                FieldDef d;
                d.key = v.bin.read<u32>(o);
                d.type = v.bin.read<u32>(o + 4);
                d.offset = v.bin.read<u16>(o + 8);
                d.size = v.bin.read<u16>(o + 10);
                d.maxCount = v.bin.read<u16>(o + 12);
                d.flags = v.bin.read<u8>(o + 14);
                c.fields.push_back(d);
            }
            v.classes[c.key] = std::move(c);
        } else if (type == kCollectionLoad && size >= 0x20) {
            CollectionDef c;
            c.key = v.vlt.read<u32>(off);
            c.parent = v.vlt.read<u32>(off + 8);
            c.numEntries = v.vlt.read<u32>(off + 20);
            c.typesCap = v.vlt.read<u32>(off + 24) >> 16;
            c.vltOffset = off;
            if (0x20 + 4 * size_t(c.typesCap) + 12 * size_t(c.numEntries) != size) continue;
            v.collections.push_back(c);
        }
    }
    return true;
}

// Own values of one collection: layout block + entry table (no inheritance).
Values OwnValues(const Vault& v, const ClassDef& cls, const CollectionDef& col) {
    Values out;
    std::unordered_map<u32, const FieldDef*> byKey;
    for (const FieldDef& f : cls.fields) byKey[f.key] = &f;

    auto ptrIt = v.vltPtr.find(u32(col.vltOffset + 28));
    if (cls.layoutSize && ptrIt != v.vltPtr.end()) {
        size_t layout = ptrIt->second;
        for (const FieldDef& f : cls.fields)
            if ((f.flags & kFieldInLayout) && !(f.flags & kFieldStatic) && !(f.flags & kFieldArray))
                out[f.key] = WordsFrom(v.bin, layout + f.offset, f.size);
    }

    size_t entries = col.vltOffset + 0x20 + 4 * size_t(col.typesCap);
    for (u32 i = 0; i < col.numEntries; ++i) {
        size_t e = entries + size_t(i) * 12;
        u32 key = v.vlt.read<u32>(e);
        u8 nodeFlags = v.vlt.read<u8>(e + 10);
        auto fit = byKey.find(key);
        if (fit == byKey.end() || (fit->second->flags & kFieldArray)) continue;
        const FieldDef& f = *fit->second;
        if (nodeFlags & kNodeInPlace) {
            u32 word = v.vlt.read<u32>(e + 4);
            if (f.size == 1) word &= 0xFF;
            else if (f.size == 2) word &= 0xFFFF;
            float fl; std::memcpy(&fl, &word, 4);
            out[key] = { fl };
        } else {
            auto pit = v.vltPtr.find(u32(e + 4));
            if (pit == v.vltPtr.end()) continue;
            out[key] = WordsFrom(v.bin, pit->second, f.size);
        }
    }
    return out;
}

// Resolves inheritance for every collection of one class, in export order.
struct Resolved { const CollectionDef* col; Values values; };

std::vector<Resolved> ResolveClass(const Vault& v, const ClassDef& cls) {
    std::vector<const CollectionDef*> cols;
    std::unordered_map<u32, size_t> idx;
    for (const CollectionDef& c : v.collections)
        if (v.vlt.read<u32>(c.vltOffset + 4) == cls.key) { idx[c.key] = cols.size(); cols.push_back(&c); }

    std::vector<Values> done(cols.size());
    std::vector<int> state(cols.size(), 0);  // 0 todo, 1 busy, 2 done
    std::function<void(size_t)> resolve = [&](size_t i) {
        if (state[i] != 0) return;
        state[i] = 1;
        Values merged;
        auto pit = idx.find(cols[i]->parent);
        if (cols[i]->parent && pit != idx.end() && pit->second != i) {
            resolve(pit->second);
            if (state[pit->second] == 2) merged = done[pit->second];
        }
        for (auto& kv : OwnValues(v, cls, *cols[i])) merged[kv.first] = std::move(kv.second);
        done[i] = std::move(merged);
        state[i] = 2;
    };
    std::vector<Resolved> out;
    for (size_t i = 0; i < cols.size(); ++i) resolve(i);
    for (size_t i = 0; i < cols.size(); ++i) out.push_back({ cols[i], std::move(done[i]) });
    return out;
}

// ---------------------------------------------------------------- names

const std::unordered_map<u32, std::string>& KnownNames() {
    static const std::unordered_map<u32, std::string> names = [] {
        const char* list[] = {
            "default", "day1_1", "day1_1a", "day1_2", "day1_3", "day1_4", "day1_5", "day1_5a",
            "day1_6", "carbon", "mostwanted", "night", "fe_main", "timeofdaylighting", "sunandsky",
        };
        std::unordered_map<u32, std::string> m;
        for (const char* n : list) m[VltHash(n)] = n;
        return m;
    }();
    return names;
}

std::string NameOf(u32 key) {
    if (key == 0) return {};
    auto it = KnownNames().find(key);
    if (it != KnownNames().end()) return it->second;
    char buf[16];
    std::snprintf(buf, sizeof(buf), "0x%08X", key);
    return buf;
}

void Copy4(const Values& values, const char* field, float out[4]) {
    auto it = values.find(VltHash(field));
    if (it == values.end()) return;
    for (size_t i = 0; i < 4 && i < it->second.size(); ++i) out[i] = it->second[i];
}

float Get1(const Values& values, const char* field) {
    auto it = values.find(VltHash(field));
    return it == values.end() || it->second.empty() ? 0.f : it->second[0];
}

} // namespace

u32 VltHash(const std::string& text) {
    const u8* k = reinterpret_cast<const u8*>(text.data());
    u32 len = u32(text.size());
    u32 a = 0x9E3779B9u, b = 0x9E3779B9u, c = 0xABCDEF00u;
    u32 rem = len;
    while (rem >= 12) {
        a += k[0] | (u32(k[1]) << 8) | (u32(k[2]) << 16) | (u32(k[3]) << 24);
        b += k[4] | (u32(k[5]) << 8) | (u32(k[6]) << 16) | (u32(k[7]) << 24);
        c += k[8] | (u32(k[9]) << 8) | (u32(k[10]) << 16) | (u32(k[11]) << 24);
        Mix(a, b, c);
        k += 12; rem -= 12;
    }
    c += len;
    switch (rem) {
    case 11: c += u32(k[10]) << 24; [[fallthrough]];
    case 10: c += u32(k[9]) << 16;  [[fallthrough]];
    case 9:  c += u32(k[8]) << 8;   [[fallthrough]];
    case 8:  b += u32(k[7]) << 24;  [[fallthrough]];
    case 7:  b += u32(k[6]) << 16;  [[fallthrough]];
    case 6:  b += u32(k[5]) << 8;   [[fallthrough]];
    case 5:  b += k[4];             [[fallthrough]];
    case 4:  a += u32(k[3]) << 24;  [[fallthrough]];
    case 3:  a += u32(k[2]) << 16;  [[fallthrough]];
    case 2:  a += u32(k[1]) << 8;   [[fallthrough]];
    case 1:  a += k[0];             break;
    default: break;
    }
    Mix(a, b, c);
    return c;
}

bool LoadLighting(const std::wstring& globalFolder,
                  std::vector<LightingStage>& lighting,
                  std::vector<SunAndSkyStage>& sunAndSky,
                  std::string& error,
                  LightingStatics* statics) {
    lighting.clear();
    sunAndSky.clear();
    std::wstring path = globalFolder;
    if (!path.empty() && path.back() != L'\\' && path.back() != L'/') path += L'\\';
    path += L"attributes.bin";

    std::ifstream file(path, std::ios::binary);
    if (!file) { error = "cannot open attributes.bin"; return false; }
    std::vector<u8> bytes((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    ByteSpan all{ bytes.data(), bytes.size() };
    if (all.read<u32>(0) != kPackMagic) { error = "attributes.bin is not a VPAK pack"; return false; }

    const u32 kTod = VltHash("timeofdaylighting");
    const u32 kSun = VltHash("sunandsky");
    bool foundTod = false, foundSun = false;
    u32 numVaults = all.read<u32>(4);
    for (u32 vi = 0; vi < numVaults; ++vi) {
        size_t h = 0x10 + size_t(vi) * 20;
        u32 binSize = all.read<u32>(h + 4), vltSize = all.read<u32>(h + 8);
        u32 binOff = all.read<u32>(h + 12), vltOff = all.read<u32>(h + 16);
        if (size_t(binOff) + binSize > all.size || size_t(vltOff) + vltSize > all.size) {
            error = "vault entry out of range"; return false;
        }
        Vault v;
        v.bin = all.sub(binOff, binSize);
        v.vlt = all.sub(vltOff, vltSize);
        if (!ParseVault(v, error)) return false;

        auto tod = v.classes.find(kTod);
        if (tod != v.classes.end()) {
            foundTod = true;
            const ClassDef& cls = tod->second;
            for (Resolved& r : ResolveClass(v, cls)) {
                LightingStage s;
                s.key = r.col->key;
                s.parentKey = r.col->parent;
                s.name = NameOf(s.key);
                s.parentName = NameOf(s.parentKey);
                const Values& x = r.values;
                Copy4(x, "DiffuseColour", s.diffuse);
                Copy4(x, "AmbientColour", s.ambient);
                Copy4(x, "SpecularColour", s.specular);
                Copy4(x, "FogSkyColour", s.fogSky);
                Copy4(x, "FogHazeColour", s.fogHaze);
                s.fogSkyScale = Get1(x, "FogSkyColourScale");
                s.fogHazeScale = Get1(x, "FogHazeColourScale");
                s.fogDistanceScale = Get1(x, "FogDistanceScale");
                s.fogInLightScatter = Get1(x, "FogInLightScatter");
                s.fogSunFalloff = Get1(x, "FogSunFalloff");
                s.envSkyBrightness = Get1(x, "EnvSkyBrightness");
                s.shadowStrength = Get1(x, "ShadowStrength");
                auto ref = x.find(VltHash("SunAndSky"));  // RefSpec {ClassKey, CollectionKey, ptr}
                if (ref != x.end() && ref->second.size() >= 2) {
                    s.sunAndSkyKey = Bits(ref->second[1]);
                    s.sunAndSky = NameOf(s.sunAndSkyKey);
                }
                s.fields = r.values;
                lighting.push_back(std::move(s));
            }
            if (statics) {
                *statics = {};
                if (cls.staticBin) {
                    for (const FieldDef& f : cls.fields) {
                        if (!(f.flags & kFieldStatic)) continue;
                        std::vector<float> w = WordsFrom(v.bin, cls.staticBin + f.offset, f.size);
                        if (w.empty()) continue;
                        if (f.key == VltHash("DebugMode")) statics->debugMode = (Bits(w[0]) & 0xFF) != 0;
                        else if (f.key == 0x98E9A723u) statics->rate = w[0];
                        else if (f.key == 0x36C9AF8Du) statics->unknown10 = w[0];
                        else if (f.key == 0xB916AF2Du) statics->refA = NameOf(Bits(w[0]));
                        else if (f.key == 0x97A4DE50u) statics->refB = NameOf(Bits(w[0]));
                    }
                    statics->valid = true;
                }
            }
        }

        auto sun = v.classes.find(kSun);
        if (sun != v.classes.end()) {
            foundSun = true;
            const u32 kRot = VltHash("SkyDomeRotation"), kModel = VltHash("SkyDomeModel");
            const u32 kStyle = VltHash("SkyDomeStyle"), kFlare = VltHash("LensFlareIndex");
            for (Resolved& r : ResolveClass(v, sun->second)) {
                SunAndSkyStage s;
                s.key = r.col->key;
                s.parentKey = r.col->parent;
                s.name = NameOf(s.key);
                s.parentName = NameOf(s.parentKey);
                for (auto& kv : r.values) {
                    if (kv.second.empty()) continue;
                    float f = kv.second[0];
                    if (kv.first == kRot) s.skyDomeRotation = f;
                    else if (kv.first == kModel) s.skyDomeModel = Bits(f);
                    else if (kv.first == kStyle) s.skyDomeStyle = int(Bits(f));
                    else if (kv.first == kFlare) s.lensFlareIndex = int(Bits(f));
                    else s.angles[kv.first] = f;
                }
                sunAndSky.push_back(std::move(s));
            }
        }
    }
    if (!foundTod) { error = "class timeofdaylighting not found"; return false; }
    if (!foundSun) { error = "class sunandsky not found"; return false; }
    return true;
}

bool ReadVltArray(const std::wstring& globalFolder, u32 classKey, u32 collectionKey, u32 fieldKey,
                  std::vector<float>& out, std::string& error) {
    out.clear();
    std::wstring path = globalFolder;
    if (!path.empty() && path.back() != L'\\' && path.back() != L'/') path += L'\\';
    path += L"attributes.bin";
    std::ifstream file(path, std::ios::binary);
    if (!file) { error = "cannot open attributes.bin"; return false; }
    std::vector<u8> bytes((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    ByteSpan all{ bytes.data(), bytes.size() };
    if (all.read<u32>(0) != kPackMagic) { error = "attributes.bin is not a VPAK pack"; return false; }
    const u32 numVaults = all.read<u32>(4);
    for (u32 vi = 0; vi < numVaults; ++vi) {
        const size_t h = 0x10 + size_t(vi) * 20;
        const u32 binSize = all.read<u32>(h + 4), vltSize = all.read<u32>(h + 8);
        const u32 binOff = all.read<u32>(h + 12), vltOff = all.read<u32>(h + 16);
        if (size_t(binOff) + binSize > all.size || size_t(vltOff) + vltSize > all.size) continue;
        Vault v;
        v.bin = all.sub(binOff, binSize);
        v.vlt = all.sub(vltOff, vltSize);
        std::string ignored;
        if (!ParseVault(v, ignored)) continue;
        const auto cls = v.classes.find(classKey);
        if (cls == v.classes.end()) continue;
        const FieldDef* field = nullptr;
        for (const FieldDef& f : cls->second.fields)
            if (f.key == fieldKey) field = &f;
        if (!field || !(field->flags & kFieldArray)) {
            error = "the field is not an array";
            return false;
        }
        if (!(field->flags & kFieldInLayout)) {
            // In the collection's entry table: a pointer to {u16 capacity, u16 count, u16 element size, u16 data
            // offset} then the elements (ecar.FECompressions: 2, 2, 4, 8, two floats; read 21/11).
            for (u32 key = collectionKey, guard = 0; key && guard < 16; ++guard) {
                const auto col = std::find_if(v.collections.begin(), v.collections.end(), [&](const CollectionDef& c) {
                    return c.key == key && v.vlt.read<u32>(c.vltOffset + 4) == classKey;
                });
                if (col == v.collections.end()) break;
                const size_t entries = col->vltOffset + 0x20 + 4 * size_t(col->typesCap);
                for (u32 i = 0; i < col->numEntries; ++i) {
                    const size_t e = entries + size_t(i) * 12;
                    if (v.vlt.read<u32>(e) != fieldKey) continue;
                    const auto at = v.vltPtr.find(u32(e + 4));
                    if (at == v.vltPtr.end()) break;
                    const u16 count = v.bin.read<u16>(at->second + 2), size = v.bin.read<u16>(at->second + 4);
                    const u16 data = v.bin.read<u16>(at->second + 6);
                    out = WordsFrom(v.bin, at->second + data, u32(count) * size);
                    return true;
                }
                key = col->parent;
            }
            error = "no entry for the field";
            return false;
        }
        for (u32 key = collectionKey, guard = 0; key && guard < 16; ++guard) {
            const auto col = std::find_if(v.collections.begin(), v.collections.end(), [&](const CollectionDef& c) {
                return c.key == key && v.vlt.read<u32>(c.vltOffset + 4) == classKey;
            });
            if (col == v.collections.end()) break;
            const auto layout = v.vltPtr.find(u32(col->vltOffset + 28));
            if (layout != v.vltPtr.end()) {
                out = WordsFrom(v.bin, layout->second + field->offset + 16, u32(field->maxCount) * field->size);
                return true;
            }
            key = col->parent;
        }
        error = "the collection has no layout for the field";
        return false;
    }
    error = "class not found";
    return false;
}

} // namespace gamelook
