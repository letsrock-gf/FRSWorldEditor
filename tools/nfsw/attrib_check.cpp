// Console check for gamelook::LoadLighting against the real attributes.bin and
// the Attribulator YAML export.
//
//   attrib_check [globalFolder] [yamlDbFolder]
//
// Exit code 0 = every hard assert passed (the YAML sweep is reported, not fatal).

#include "../../src/render/GameLookAttributes.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <map>
#include <string>
#include <vector>

using namespace gamelook;

static int g_failed = 0;

static void Check(bool ok, const char* what) {
    std::printf("%s  %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok) ++g_failed;
}

static bool Eq(float a, float b) { return a == b || std::fabs(a - b) <= 1e-6f * (1.f + std::fabs(b)); }

static bool Eq4(const float* v, float x, float y, float z, float w) {
    return Eq(v[0], x) && Eq(v[1], y) && Eq(v[2], z) && Eq(v[3], w);
}

static u32 Bits(float f) { u32 u; std::memcpy(&u, &f, 4); return u; }

// ---- minimal reader for Attribulator's collection YAML ---------------------

struct YamlValue { std::vector<std::string> parts; };          // scalar(s) in file order
struct YamlCollection { std::string name, parent; std::vector<std::pair<std::string, YamlValue>> data; };

static std::string Trim(const std::string& s) {
    size_t a = s.find_first_not_of(" \t\r"), b = s.find_last_not_of(" \t\r");
    return a == std::string::npos ? std::string() : s.substr(a, b - a + 1);
}

static std::vector<YamlCollection> ReadYaml(const std::string& path) {
    std::vector<YamlCollection> out;
    std::ifstream in(path);
    std::string line;
    while (std::getline(in, line)) {
        if (line.rfind("- ParentName:", 0) == 0) { out.push_back({}); out.back().parent = Trim(line.substr(13)); continue; }
        if (out.empty()) continue;
        size_t indent = line.find_first_not_of(' ');
        if (indent == std::string::npos) continue;
        std::string body = Trim(line);
        size_t colon = body.find(':');
        if (colon == std::string::npos) continue;
        std::string key = body.substr(0, colon), value = Trim(body.substr(colon + 1));
        if (indent == 2 && key == "Name") out.back().name = value;
        else if (indent == 4) { out.back().data.push_back({ key, {} }); if (!value.empty()) out.back().data.back().second.parts.push_back(value); }
        else if (indent == 6 && !out.back().data.empty()) out.back().data.back().second.parts.push_back(value);
    }
    return out;
}

static u32 FieldHash(const std::string& name) {
    if (name.rfind("0x", 0) == 0) return u32(std::strtoul(name.c_str(), nullptr, 16));
    return VltHash(name);
}

static u32 CollectionHash(const std::string& name) { return FieldHash(name); }

// Compares one YAML scalar to one loaded 32-bit word.
static bool ScalarMatches(const std::string& text, float word) {
    if (text == "true") return (Bits(word) & 0xFF) == 1;
    if (text == "false") return (Bits(word) & 0xFF) == 0;
    char* end = nullptr;
    double d = std::strtod(text.c_str(), &end);
    if (end == text.c_str()) return CollectionHash(text) == Bits(word);  // a key name
    bool integral = text.find_first_of(".eE") == std::string::npos;
    if (Eq(float(d), word)) return true;
    if (integral) {  // UInt32 / Int32 / hash stored as bits
        long long n = std::strtoll(text.c_str(), nullptr, 10);
        return u32(n) == Bits(word);
    }
    return false;
}

static void SweepLighting(const std::vector<LightingStage>& stages, const std::string& yaml, int& compared, int& bad) {
    for (const YamlCollection& yc : ReadYaml(yaml)) {
        const LightingStage* s = nullptr;
        for (const LightingStage& t : stages) if (t.key == CollectionHash(yc.name)) s = &t;
        if (!s) { std::printf("  missing collection %s\n", yc.name.c_str()); ++bad; continue; }
        if (!yc.parent.empty() && CollectionHash(yc.parent) != s->parentKey) {
            std::printf("  %s parent mismatch\n", yc.name.c_str()); ++bad;
        }
        for (const auto& kv : yc.data) {
            auto it = s->fields.find(FieldHash(kv.first));
            ++compared;
            if (it == s->fields.end()) { std::printf("  %s.%s not loaded\n", yc.name.c_str(), kv.first.c_str()); ++bad; continue; }
            std::vector<std::string> parts = kv.second.parts;
            std::vector<float> words = it->second;
            // RefSpec: YAML gives ClassKey, CollectionKey; words are {class, collection, ptr}.
            bool ok = parts.size() <= words.size();
            for (size_t i = 0; ok && i < parts.size(); ++i) ok = ScalarMatches(parts[i], words[i]);
            if (!ok) {
                ++bad;
                std::printf("  %s.%s yaml=", yc.name.c_str(), kv.first.c_str());
                for (auto& p : parts) std::printf("%s ", p.c_str());
                std::printf("loaded=");
                for (float w : words) std::printf("%.9g(0x%08X) ", w, Bits(w));
                std::printf("\n");
            }
        }
    }
}

static void SweepSun(const std::vector<SunAndSkyStage>& stages, const std::string& yaml, int& compared, int& bad) {
    for (const YamlCollection& yc : ReadYaml(yaml)) {
        const SunAndSkyStage* s = nullptr;
        for (const SunAndSkyStage& t : stages) if (t.key == CollectionHash(yc.name)) s = &t;
        if (!s) { std::printf("  missing sunandsky %s\n", yc.name.c_str()); ++bad; continue; }
        for (const auto& kv : yc.data) {
            ++compared;
            if (kv.second.parts.empty()) { ++bad; continue; }
            const std::string& p = kv.second.parts[0];
            u32 h = FieldHash(kv.first);
            bool ok;
            if (kv.first == "SkyDomeRotation") ok = Eq(float(std::strtod(p.c_str(), nullptr)), s->skyDomeRotation);
            else if (kv.first == "SkyDomeModel") ok = u32(std::strtoull(p.c_str(), nullptr, 10)) == s->skyDomeModel;
            else if (kv.first == "SkyDomeStyle") ok = std::atoi(p.c_str()) == s->skyDomeStyle;
            else if (kv.first == "LensFlareIndex") ok = std::atoi(p.c_str()) == s->lensFlareIndex;
            else { auto it = s->angles.find(h); ok = it != s->angles.end() && Eq(float(std::strtod(p.c_str(), nullptr)), it->second); }
            if (!ok) { ++bad; std::printf("  sunandsky %s.%s yaml=%s mismatch\n", yc.name.c_str(), kv.first.c_str(), p.c_str()); }
        }
    }
}

int main(int argc, char** argv) {
    std::wstring global = L"D:\\NFS World\\Data2015\\GLOBAL";
    std::string yamlDb = "D:\\NFS World\\work\\lighting-research\\current-installed-audit\\main\\attributes\\db";
    if (argc > 1) { std::string a = argv[1]; global.assign(a.begin(), a.end()); }
    if (argc > 2) yamlDb = argv[2];

    std::vector<LightingStage> lighting;
    std::vector<SunAndSkyStage> sun;
    LightingStatics statics;
    std::string error;
    bool loaded = LoadLighting(global, lighting, sun, error, &statics);
    Check(loaded, ("LoadLighting " + error).c_str());
    if (!loaded) return 1;
    std::printf("timeofdaylighting: %zu collections, sunandsky: %zu collections\n", lighting.size(), sun.size());
    for (const LightingStage& s : lighting)
        std::printf("  tod %-12s parent=%-8s sun=%-10s diffuse=(%g %g %g %g) fogDist=%g fields=%zu\n",
                    s.name.c_str(), s.parentName.c_str(), s.sunAndSky.c_str(),
                    s.diffuse[0], s.diffuse[1], s.diffuse[2], s.diffuse[3], s.fogDistanceScale, s.fields.size());
    for (const SunAndSkyStage& s : sun)
        std::printf("  sun %-12s parent=%-8s rot=%g model=%u style=%d flare=%d angles=%zu\n",
                    s.name.c_str(), s.parentName.c_str(), s.skyDomeRotation, s.skyDomeModel,
                    s.skyDomeStyle, s.lensFlareIndex, s.angles.size());

    auto tod = [&](const char* n) -> const LightingStage* {
        for (const LightingStage& s : lighting) if (s.name == n) return &s;
        return nullptr;
    };
    auto sky = [&](const char* n) -> const SunAndSkyStage* {
        for (const SunAndSkyStage& s : sun) if (s.name == n) return &s;
        return nullptr;
    };

    const LightingStage* d = tod("default");
    const LightingStage* d2 = tod("day1_2");
    const LightingStage* d6 = tod("day1_6");
    Check(d && d2 && d6, "default, day1_2, day1_6 present");
    if (d) {
        Check(Eq4(d->diffuse, 1.f, 0.94f, 0.84f, 1.f), "default DiffuseColour = 1 0.94 0.84 1");
        Check(Eq4(d->ambient, 0.26f, 0.27f, 0.28f, 1.f), "default AmbientColour = 0.26 0.27 0.28 1");
        Check(Eq(d->fogDistanceScale, 5.3f), "default FogDistanceScale = 5.3");
        Check(d->sunAndSky == "default", "default SunAndSky = sunandsky/default");
        Check(d->parentKey == 0, "default has no parent");
    }
    if (d2) {
        Check(Eq4(d2->diffuse, 1.f, 0.96f, 0.88f, 1.f), "day1_2 DiffuseColour = 1 0.96 0.88 1");
        Check(Eq4(d2->ambient, 0.255f, 0.265f, 0.28f, 1.f), "day1_2 AmbientColour = 0.255 0.265 0.28 1");
        Check(Eq(d2->fogDistanceScale, 4.5f), "day1_2 FogDistanceScale = 4.5");
        Check(Eq(d2->shadowStrength, 0.7f), "day1_2 ShadowStrength = 0.7");
        Check(d2->sunAndSky == "day1_2", "day1_2 SunAndSky = sunandsky/day1_2");
        Check(d2->parentName == "default", "day1_2 parent = default");
    }
    if (d6) {
        Check(Eq4(d6->diffuse, 0.08f, 0.095f, 0.125f, 1.f), "day1_6 DiffuseColour = 0.08 0.095 0.125 1");
        Check(Eq4(d6->ambient, 0.09f, 0.095f, 0.11f, 1.f), "day1_6 AmbientColour = 0.09 0.095 0.11 1");
        Check(Eq(d6->fogDistanceScale, 2.3f), "day1_6 FogDistanceScale = 2.3");
        Check(d6->sunAndSky == "day1_6", "day1_6 SunAndSky = sunandsky/day1_6");
    }
    // Inheritance: "night" sets no SunAndSky/FogInLightScatter in the YAML, so both come from default.
    const LightingStage* night = tod("night");
    Check(night && d && night->sunAndSky == "default", "night inherits SunAndSky = default");
    Check(night && d && Eq(night->fogInLightScatter, 3.965f) && Eq(d->fogInLightScatter, 3.965f),
          "night inherits FogInLightScatter = 3.965 from default");
    Check(night && Eq(night->shadowStrength, 0.5f), "night own ShadowStrength = 0.5");
    bool allInherit = d != nullptr;
    for (const LightingStage& s : lighting)
        for (const auto& kv : d ? d->fields : decltype(s.fields){})
            if (!s.fields.count(kv.first)) allInherit = false;
    Check(allInherit, "every timeofdaylighting stage has every field of default");

    const SunAndSkyStage* s2 = sky("day1_2");
    Check(s2 != nullptr, "sunandsky day1_2 present");
    if (s2) {
        Check(Eq(s2->skyDomeRotation, 310.f), "sunandsky day1_2 SkyDomeRotation = 310");
        auto it = s2->angles.find(0x036F58D1u);
        Check(it != s2->angles.end() && Eq(it->second, 34.f), "sunandsky day1_2 0x036F58D1 = 34");
        Check(s2->skyDomeModel == 1159354308u, "sunandsky day1_2 SkyDomeModel = 1159354308");
        Check(s2->skyDomeStyle == 1 && s2->lensFlareIndex == 0, "sunandsky day1_2 style 1, flare 0");
    }

    std::printf("statics: valid=%d rate(0x98E9A723)=%g 0x36C9AF8D=%g DebugMode=%d 0xB916AF2D=%s 0x97A4DE50=%s\n",
                statics.valid, statics.rate, statics.unknown10, statics.debugMode,
                statics.refA.c_str(), statics.refB.c_str());
    Check(statics.valid && Eq(statics.rate, 720.f), "static 0x98E9A723 (offset 0x14) = 720");
    Check(statics.valid && !statics.debugMode, "static DebugMode = false");

    int compared = 0, bad = 0;
    SweepLighting(lighting, yamlDb + "\\timeofdaylighting.yml", compared, bad);
    SweepSun(sun, yamlDb + "\\sunandsky.yml", compared, bad);
    std::printf("YAML sweep: %d field values compared, %d differ\n", compared, bad);

    std::printf(g_failed ? "RESULT: %d FAILED\n" : "RESULT: all hard asserts passed\n", g_failed);
    return g_failed ? 1 : 0;
}
