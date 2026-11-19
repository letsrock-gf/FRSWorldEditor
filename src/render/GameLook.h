#pragma once

// The pieces of Need for Speed Underground 2's own world rendering that the
// viewport's "Game look" view reuses: the WORLD / WORLDREFLECT / WORLDNOFOG
// effects' vs_1_1 / ps_1_1 bytecode taken from SPEED2.EXE (no D3DX needed),
// and the per-track fog of the region's parameter maps with the constants
// eDisplayFrame derives from it. See NFSU2Unity docs/re/world.md and
// city-drive.md for the reverse engineering this follows.

#include "core/Math.h"
#include "core/Types.h"

#include <d3d9.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace gamelook {

// One effect's shaders: pass 0 of technique 0 ("world").
struct EffectCode {
    std::vector<uint32_t> vertexShader;
    std::vector<uint32_t> pixelShader;
};

// The three effects the world is drawn with (gEffects[] ids 0, 2 and 7):
//  WORLD        IDI_WORLD_FX:        vs c0..c3 WorldViewProj, c4..c6 WorldView, c7 TextureOffset, c8 FogValue;
//                                    ps out.rgb = 2 * t0 * v0, out.a = t0.a * v0.a
//  WORLDREFLECT IDI_WORLDREFLECT_FX: vs c0..c3, c4..c6, c7 FogValue (no TextureOffset), oT1 = the screen
//                                    position as a projective coordinate; ps out.rgb = 2 * t0 * v0 + t1 *
//                                    (t0.a * v0.a), out.a = 1, s1 = ReflectedTex
//  WORLDNOFOG   IDI_WORLDNOFOG_FX:   vs c0..c3, c4 TextureOffset; ps as WORLD
// Each shader is found by its version token, the byte count the effect stores in front of it and its
// constant table. False with a reason when the exe lacks one of them.
// filter: IDI_FILTER_FX pass p3 (4 taps: vs copies the clip position and four texture coordinates, ps
// out = t0*c0 + t1*c1 + t2*c2 + t3*c3), which sub_5C7E40 blurs the reflection with; empty when absent.
struct WorldEffects {
    EffectCode world, reflect, noFog, filter;
};
bool ExtractWorldEffects(const std::wstring& exePath, WorldEffects& out, std::string& error);

// NFS World (nfsw.exe): its effect table (0xC83018, 47 entries of 0x90 bytes: name, vertex layout, .fx
// name, resource) and the material effect id table 0xC84B5C pick each mesh's IDI_*_FX effect; every
// resource is a binary fx_2_0 whose vs_2_0 / ps_2_0 are stored after their byte count. The renderer takes
// them with ExtractGameEffect (below) and feeds them by name.

// The world_B / world_A constants of one daytime frame, as the game's own world draws received them
// (Data2015\mesh-deform.log, "Pedestrian world light", the most frequent sample with
// cvTimeOfDayParams1 = (1, 0): 99 of 4698 lines, world-space draws so the light is in world space).
// ambient is cvAmbientColour itself (xyz, w = 4): the preshader multiplies (logged value / 4).
struct NfswLight {
    float lightVec[4]  = { 0.063101f, -0.775425f, 0.628443f, 0.0f };
    float diffuse[4]   = { 1.985050f, 1.855151f, 1.651232f, 2.0f };
    float ambient[4]   = { 1.059071f / 4.0f, 1.093091f / 4.0f, 1.147111f / 4.0f, 4.0f };   // cvAmbientColour (w = 4)
    float timeOfDay1[4] = { 1.0f, 0.0f, 0.0f, 0.0f };
    float timeOfDay0[4] = { 1.0f, 0.0f, 0.0f, 0.0f };
    // rea-specular-reflection-shadow.md: L+0x40 SpecularColour, L+0x60 SpecularColour2, L+0x70 field
    // 0x707F1B6C, L+0x80 SpecularPower, L+0x84 SpecularPower2 (FUN_007A2EF0, 0x7AE8D0, 0x7ADF10).
    float specular[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
    float specular2[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
    float light2Dir[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
    float specularPower = 0.0f, specularPower2 = 0.0f;
    // Weather W (view +0x5C): +0x36A0 settles at 0.75 dry, +0x290 is 0 dry. Dry weather is drawn.
    float wetness = 0.75f, rain = 0.0f;
    // The shadow map's light: the second sunandsky vector L+0x120 (A = 0xACB78DC4, Z = 0xA480C321).
    float shadowVec[4] = { 0.063101f, -0.775425f, 0.628443f, 0.0f };
    // cvTimeOfDayParams0.x by material selector (worldFlags >> 28): 1 for 0..6, field 0x507AFAFC for 7,
    // ChristmasLights for 8.
    float todSelector7 = 1.0f, todSelector8 = 0.0f;
    float fogBrPlusBm[4] = { 0.000189f, 0.000206f, 0.000226f, 0.0f };
    float fogConst3[4] = { 1.0f, 1.0f, 0.0f, 3.006382f };
    float fogValueMW[4] = { 1.786778f, 0.0f, 0.0f, 0.0f };
    // Not in the capture: fog stays off until they are computed from the lighting data.
    float fogEnable[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
    float fogOneOverBrBm[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
    float fogConst1[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
    float fogConst2[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
    // timeofdaylighting "default" ShadowStrength (attributes.bin); world_A InvShadowStrength = 1 - it.
    float shadowStrength = 0.85f;
};

// One shader of a game effect with what it needs to be fed: its constant table (name -> first register
// and register count, float4 and sampler sets) and its preshader (the PRES comment: a CTAB of inputs, a
// CLIT of double literals and FXLC code that D3DX runs on the CPU to fill derived registers).
struct ShaderProgram {
    struct Slot { int reg = 0, count = 1; bool sampler = false; };
    std::vector<uint32_t> code;
    std::unordered_map<std::string, Slot> table;
    struct Arg { uint32_t table = 0, offset = 0; };
    struct Op { uint32_t opcode = 0, comps = 0; bool scalar = false; std::vector<Arg> in; Arg out; };
    std::vector<std::pair<std::string, Slot>> presInputs;   // preshader inputs, registers in its own table
    std::vector<double> presLiterals;
    std::vector<Op> presOps;
    bool presValid = true;   // false: the preshader uses an op or addressing this reader does not run
    // Runs the preshader. `value(name)` gives a parameter's floats (rows of 4, nullptr when unknown);
    // `write(register, float4)` receives every output register.
    template <typename Value, typename Write> void RunPreshader(Value value, Write write) const;
};
bool LoadShaderProgram(const std::vector<uint32_t>& code, ShaderProgram& out);

// A technique's pass p0 of an IDI_*_FX resource, found by its vs / ps sizes in dwords (fxdump of the
// installed exe, work/toon-research/fx-dump) and checked by its constant table.
struct GameEffect {
    std::string name;
    ShaderProgram vs, ps;
    // The effect's float parameters as stored in the fx_2_0 (their initial values: what D3DX holds until
    // the game sets one), by name, rows of 4 for matrices.
    std::unordered_map<std::string, std::vector<float>> defaults;
    bool Valid() const { return !vs.code.empty() && !ps.code.empty(); }
};
bool ExtractGameEffect(const std::wstring& exePath, const wchar_t* resource, size_t vsDwords, size_t psDwords,
                       const char* name, GameEffect& out, std::string& error);

struct LightingStage;
struct SunAndSkyStage;
// The light of a time-of-day position: stage `from` blended into `to` by t (0..1), linear as 0x6E30E0.
// Sun: sunandsky loader 0x6E3D50, dir = (cos az sin ze, sin az sin ze, cos ze) with az = field 0xD4496F5F
// and ze = field 0x78E76168 in degrees. `source` says which values are computed and which captured.
NfswLight ComputeNfswLight(const LightingStage& from, const LightingStage& to, float t, const SunAndSkyStage* sunFrom,
                           const SunAndSkyStage* sunTo, std::string& source);

// The instructions of a vs_1_1 / ps_1_1 token stream as text ("dp4 oPos.x, v0, c0"), one per entry,
// to check an extraction against the published disassembly.
std::vector<std::string> Disassemble(const std::vector<uint32_t>& tokens);

// bStringHash: h = 0xFFFFFFFF, h = h * 33 + c.
u32 GameHash(const char* text);

// The fog struct sub_614300 (0x614300) fills, the part eDisplayFrame uses.
struct FogState {
    float start  = 212.0f;   // +0x0C
    float factor = 0.74f;    // +0x10
    float r = 110.0f, g = 105.0f, b = 75.0f;   // +0x14 (bytes)
};

// The constants eDisplayFrame (0x5CB032..0x5CB189) sets from it for a player view:
//   FOGVALUE = (250, 1 / (250 - start), min(1 - factor, 0.6))   flt_7A27A0 = 250, dword_784564 = 0.6
//   FOGCOLOR = each colour byte * 0.75 (flt_785510), truncated
// and the WORLD vertex shader turns them into oFog = clamp((FogValue.x - viewZ) * FogValue.y, FogValue.z, 1).
struct FogConstants {
    float value[4] = { 250.0f, 1.0f / 38.0f, 0.26f, 0.0f };
    D3DCOLOR colour = 0xFF524F38;
};
FogConstants MakeFogConstants(const FogState& fog);

// A region's fog parameter maps: chunk 0x8003B600 of TRACKS\<region>.BUN, one 0x8003B601 layer per
// named map (header 0x3B602, field types 0x3B603, field offsets 0x3B604, parameter sets 0x3B605, an
// 8-bit or 16-bit quad tree 0x3B607 / 0x3B608). LoaderParameterMaps 0x60B810, ParameterMapLayer::
// GetParameterData 0x5FA4E0, GetParameterSetIndexFromQuadData8/16 0x5F0930 / 0x5F0A30.
class FogMap {
public:
    // Reads the "Normal Fog" layer: in free roam without the time-of-day option (dword_870D14 != 1)
    // sub_614300 blends it with weight 1 - flt_8A1C88 = 1, the night layer with 0, and skips the rain
    // layer. False (and the exe's fallback struct 0x800E18 in Sample) when the file has no such layer.
    bool Load(const std::wstring& bunPath);
    bool Loaded() const { return !sets_.empty(); }

    // The game's value at (x, y) game space, eased the way ParameterAccessorBlendByDistance does it
    // (sub_5FA660 with radius 50: blend = min(distance moved since the last sample / 50, 1)).
    FogState Sample(float x, float y);
    void ResetSmoothing() { haveLast_ = false; }

    // The set index the quad tree gives at (x, y), for the log.
    int SetIndexAt(float x, float y) const;

private:
    float minX_ = 0, minY_ = 0, maxX_ = 0, maxY_ = 0;
    std::vector<std::vector<float>> sets_;   // 8 fields each
    std::vector<u16> tree_;                  // 4 children per node (8-bit trees widened)
    bool haveLast_ = false;
    float lastX_ = 0, lastY_ = 0;
    std::vector<float> smoothed_;
};

template <typename Value, typename Write>
void ShaderProgram::RunPreshader(Value value, Write write) const {
    if (presOps.empty() || !presValid) return;
    // Tables (Wine d3dx9 preshader numbering): 1 literals, 2 inputs, 4 output constants, 7 temporaries.
    std::vector<float> inputs, temps(256, 0.0f);
    for (const auto& [name, slot] : presInputs) {
        const size_t at = (size_t)slot.reg * 4, n = (size_t)slot.count * 4;
        if (inputs.size() < at + n) inputs.resize(at + n, 0.0f);
        if (const float* v = value(name))
            for (size_t k = 0; k < n; ++k) inputs[at + k] = v[k];
    }
    std::unordered_map<int, std::array<float, 4>> outputs;
    auto read = [&](const Arg& a, uint32_t k) -> float {
        const size_t i = a.offset + k;
        switch (a.table) {
            case 1: return i < presLiterals.size() ? (float)presLiterals[i] : 0.0f;
            case 2: return i < inputs.size() ? inputs[i] : 0.0f;
            case 4: {
                const auto it = outputs.find((int)(i / 4));
                return it != outputs.end() ? it->second[i % 4] : 0.0f;
            }
            case 7: return i < temps.size() ? temps[i] : 0.0f;
            default: return 0.0f;
        }
    };
    auto store = [&](const Arg& a, uint32_t k, float v) {
        const size_t i = a.offset + k;
        if (a.table == 7 && i < temps.size()) temps[i] = v;
        else if (a.table == 4) {
            auto [it, fresh] = outputs.try_emplace((int)(i / 4));
            if (fresh) it->second = { 0.0f, 0.0f, 0.0f, 0.0f };
            it->second[i % 4] = v;
        }
    };
    for (const Op& op : presOps) {
        auto in = [&](size_t n, uint32_t k) { return read(op.in[n], op.scalar && n == 0 ? 0 : k); };
        if (op.opcode == 0x500) {   // dot: one output from comps-wide inputs
            float sum = 0.0f;
            for (uint32_t k = 0; k < op.comps; ++k) sum += in(0, k) * in(1, k);
            store(op.out, 0, sum);
            continue;
        }
        for (uint32_t k = 0; k < op.comps; ++k) {
            float v = 0.0f;
            switch (op.opcode) {
                case 0x100: v = in(0, k); break;                                   // mov
                case 0x101: v = -in(0, k); break;                                  // neg
                case 0x103: v = in(0, k) != 0.0f ? 1.0f / in(0, k) : INFINITY; break;   // rcp
                case 0x107: v = in(0, k) > 0.0f ? 1.0f / std::sqrt(in(0, k)) : INFINITY; break;   // rsq
                case 0x200: v = (std::min)(in(0, k), in(1, k)); break;            // min
                case 0x201: v = (std::max)(in(0, k), in(1, k)); break;            // max
                case 0x204: v = in(0, k) + in(1, k); break;                       // add
                case 0x205: v = in(0, k) * in(1, k); break;                       // mul
                default: break;
            }
            store(op.out, k, v);
        }
    }
    for (const auto& [reg, v] : outputs) write(reg, v.data());
}

} // namespace gamelook
