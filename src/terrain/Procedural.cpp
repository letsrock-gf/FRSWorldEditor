#include "terrain/Procedural.h"

#include <algorithm>
#include <cmath>
#include <vector>

namespace terrain {

namespace {

constexpr int kPatchCells = 64;
constexpr int kRimCells = 3;
constexpr float kRimHeight = 45.0f;
constexpr float kFlatRadius = 80.0f, kEaseRadius = 200.0f;

u32 Hash(u32 seed, int x, int y) {
    u32 h = seed * 0x9E3779B1u ^ (u32)x * 0x85EBCA77u ^ (u32)y * 0xC2B2AE3Du;
    h ^= h >> 15; h *= 0x2C1B3C6Du; h ^= h >> 12; h *= 0x297A2D39u; h ^= h >> 15;
    return h;
}

float Lattice(u32 seed, int x, int y) { return (float)(Hash(seed, x, y) & 0xFFFFFF) / (float)0x7FFFFF - 1.0f; }

float Smooth(float t) { return t * t * t * (t * (t * 6.0f - 15.0f) + 10.0f); }

// Value noise in -1..1, one unit per lattice step.
float Value(u32 seed, float x, float y) {
    const float fx = std::floor(x), fy = std::floor(y);
    const int ix = (int)fx, iy = (int)fy;
    const float tx = Smooth(x - fx), ty = Smooth(y - fy);
    const float a = Lattice(seed, ix, iy), b = Lattice(seed, ix + 1, iy);
    const float c = Lattice(seed, ix, iy + 1), d = Lattice(seed, ix + 1, iy + 1);
    return (a + (b - a) * tx) + ((c + (d - c) * tx) - (a + (b - a) * tx)) * ty;
}

// Fractal sum, about -1..1.
float Fractal(u32 seed, float x, float y, int octaves) {
    float sum = 0.0f, amplitude = 1.0f, total = 0.0f, frequency = 1.0f;
    for (int o = 0; o < octaves; ++o) {
        sum += Value(seed + (u32)o * 1013u, x * frequency, y * frequency) * amplitude;
        total += amplitude;
        amplitude *= 0.5f;
        frequency *= 2.03f;
    }
    return sum / total;
}

} // namespace

bool GenerateGround(const ProceduralSettings& s, const std::function<bool(float, float)>& inside, Terrain& out,
                    size_t& patchCount, size_t& cellCount) {
    out.Clear();
    patchCount = cellCount = 0;
    const float size = std::clamp(s.size, 256.0f, 8192.0f);
    const float cell = std::max(8.0f, std::round(size / 256.0f));
    const int n = (int)std::ceil(size / cell);
    const Vec2 origin{ std::floor((s.start.x - size * 0.5f) / cell) * cell,
                       std::floor((s.start.y - size * 0.5f) / cell) * cell };

    // The relief and the size of its features, by preset.
    float relief = 30.0f, scale = 900.0f;
    if (s.preset == 0) { relief = 4.0f; scale = 600.0f; }
    else if (s.preset == 2) { relief = 110.0f; scale = 1400.0f; }

    // Which cells have ground, and how far each is from the edge of it.
    std::vector<u8> in((size_t)n * n, 0);
    for (int j = 0; j < n; ++j)
        for (int i = 0; i < n; ++i)
            in[(size_t)j * n + i] = inside(origin.x + (i + 0.5f) * cell, origin.y + (j + 0.5f) * cell) ? 1 : 0;
    std::vector<int> edge((size_t)n * n, kRimCells);
    for (int j = 0; j < n; ++j)
        for (int i = 0; i < n; ++i) {
            if (!in[(size_t)j * n + i]) { edge[(size_t)j * n + i] = 0; continue; }
            int best = kRimCells;
            for (int dj = -kRimCells; dj <= kRimCells; ++dj)
                for (int di = -kRimCells; di <= kRimCells; ++di) {
                    const int x = i + di, y = j + dj;
                    const bool out_ = x < 0 || y < 0 || x >= n || y >= n || !in[(size_t)y * n + x];
                    if (out_) best = std::min(best, std::max(std::abs(di), std::abs(dj)) - 1);
                }
            edge[(size_t)j * n + i] = std::max(0, best);
        }

    // Corner heights: noise, the flat start, the rim.
    std::vector<float> heights((size_t)(n + 1) * (n + 1));
    const float base = s.start.z - 0.05f;
    for (int j = 0; j <= n; ++j)
        for (int i = 0; i <= n; ++i) {
            const float x = origin.x + i * cell, y = origin.y + j * cell;
            const float d = std::hypot(x - s.start.x, y - s.start.y);
            const float ease = std::clamp((d - kFlatRadius) / (kEaseRadius - kFlatRadius), 0.0f, 1.0f);
            // Valleys round the start rather than a pit: the noise is taken
            // from its value at the start, so the start sits at its own height.
            const float hill = Fractal(s.seed, x / scale, y / scale, 5) - Fractal(s.seed, s.start.x / scale, s.start.y / scale, 5);
            float h = base + hill * relief * Smooth(ease);
            int rim = kRimCells;
            for (int dj = -1; dj <= 0; ++dj)
                for (int di = -1; di <= 0; ++di) {
                    const int ci = i + di, cj = j + dj;
                    if (ci < 0 || cj < 0 || ci >= n || cj >= n) { rim = 0; continue; }
                    rim = std::min(rim, edge[(size_t)cj * n + ci]);
                }
            if (rim < kRimCells) h += kRimHeight * (1.0f - (float)rim / kRimCells);
            heights[(size_t)j * (n + 1) + i] = h;
        }

    const auto heightAt = [&](float x, float y) {
        const int i = std::clamp((int)std::lround((x - origin.x) / cell), 0, n);
        const int j = std::clamp((int)std::lround((y - origin.y) / cell), 0, n);
        return heights[(size_t)j * (n + 1) + i];
    };
    const auto cellIn = [&](float x, float y) {
        const int i = (int)std::floor((x - origin.x) / cell), j = (int)std::floor((y - origin.y) / cell);
        return i >= 0 && j >= 0 && i < n && j < n && in[(size_t)j * n + i];
    };

    for (int pj = 0; pj * kPatchCells < n; ++pj)
        for (int pi = 0; pi * kPatchCells < n; ++pi) {
            const int columns = std::min(kPatchCells, n - pi * kPatchCells), rows = std::min(kPatchCells, n - pj * kPatchCells);
            const Vec2 min{ origin.x + pi * kPatchCells * cell, origin.y + pj * kPatchCells * cell };
            const Vec2 max{ min.x + columns * cell, min.y + rows * cell };
            const u32 id = out.AddWhere(min, max, cell, cellIn, heightAt);
            Patch* patch = id ? out.Find(id) : nullptr;
            if (!patch) continue;
            Layer grass;
            grass.texture = "TRN_GRASSA_KT";   // the city's grass (L4RA); TRN_OLDTOWNGRASS is not in it
            grass.tile = 8.0f;
            grass.surface = 0x0F;
            patch->layers[0] = grass;
            patch->autoRock = true;
            patch->rockAngle = 30.0f;
            patch->rockTexture = "TRN_ROCKCLIFFA_KT";
            patch->mixTexture = "TRN_GRASSACLIFFA";
            patch->conform = true;
            // No city light to take in an empty region: a neutral grey, which
            // the world shader (2 x texture x vertex colour) shows near as painted.
            patch->ambient = 0x707070;
            for (u8 p : patch->paint) cellCount += p != kEmpty;
            ++patchCount;
        }
    out.Touch();
    return patchCount > 0;
}

} // namespace terrain
