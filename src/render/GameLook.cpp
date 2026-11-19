#include "render/GameLook.h"
#include "render/GameLookAttributes.h"

#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <initializer_list>

namespace gamelook {

namespace {

constexpr uint32_t kVs11 = 0xFFFE0101u;
constexpr uint32_t kPs11 = 0xFFFF0101u;
constexpr uint32_t kEndToken = 0x0000FFFFu;
constexpr uint32_t kOpTex = 0x42, kOpDef = 0x51, kOpDcl = 0x1F;

bool Contains(const uint32_t* tokens, size_t count, const char* text) {
    const uint8_t* p = reinterpret_cast<const uint8_t*>(tokens);
    const size_t n = count * 4, k = std::strlen(text);
    for (size_t i = 0; i + k <= n; ++i)
        if (std::memcmp(p + i, text, k) == 0) return true;
    return false;
}

// Length in dwords of the token stream starting at `at` (a version token), 0 when it runs off.
size_t ShaderLength(const uint32_t* t, size_t count, size_t at) {
    size_t i = at + 1;
    while (i < count) {
        const uint32_t token = t[i];
        if ((token & 0xFFFFu) == 0xFFFEu) {   // comment
            i += 1 + (token >> 16);
            continue;
        }
        if (token == kEndToken) return i + 1 - at;
        ++i;
    }
    return 0;
}

// Calls `visit(opcode, firstParameter, parameterCount, instructionToken)` for every instruction.
template <typename Visit>
void ForEachInstruction(const std::vector<uint32_t>& tokens, Visit visit) {
    size_t i = 1;
    while (i < tokens.size()) {
        const uint32_t token = tokens[i];
        if (token == kEndToken) break;
        if ((token & 0xFFFFu) == 0xFFFEu) { i += 1 + (token >> 16); continue; }
        const uint32_t op = token & 0xFFFFu;
        size_t n = 0;
        if (op == kOpDef) n = 5;
        else
            while (i + 1 + n < tokens.size() && (tokens[i + 1 + n] & 0x80000000u)) ++n;
        visit(op, i + 1, n, token);
        i += 1 + n;
    }
}

bool HasInstruction(const std::vector<uint32_t>& tokens, uint32_t opcode) {
    bool found = false;
    ForEachInstruction(tokens, [&](uint32_t op, size_t, size_t, uint32_t) { found |= op == opcode; });
    return found;
}

// Every vs_1_1 / ps_1_1 the effect resource holds (each stored after a dword with its byte count).
bool ShadersOf(HMODULE module, const wchar_t* resource, std::vector<std::vector<uint32_t>>& vertex,
               std::vector<std::vector<uint32_t>>& pixel, uint32_t vsVersion = kVs11, uint32_t psVersion = kPs11) {
    HRSRC info = ::FindResourceW(module, resource, MAKEINTRESOURCEW(10) /* RT_RCDATA */);
    if (!info) return false;
    const DWORD size = ::SizeofResource(module, info);
    HGLOBAL handle = ::LoadResource(module, info);
    const uint8_t* data = handle ? static_cast<const uint8_t*>(::LockResource(handle)) : nullptr;
    if (!data || size < 8) return false;
    std::vector<uint32_t> tokens(size / 4);
    std::memcpy(tokens.data(), data, tokens.size() * 4);
    for (size_t i = 1; i < tokens.size(); ++i) {
        if (tokens[i] != vsVersion && tokens[i] != psVersion) continue;
        const size_t n = ShaderLength(tokens.data(), tokens.size(), i);
        if (n == 0 || tokens[i - 1] != n * 4) continue;
        (tokens[i] == vsVersion ? vertex : pixel)
            .emplace_back(tokens.begin() + (std::ptrdiff_t)i, tokens.begin() + (std::ptrdiff_t)(i + n));
        i += n - 1;
    }
    return true;
}

template <typename Pred>
bool Pick(const std::vector<std::vector<uint32_t>>& list, Pred pred, std::vector<uint32_t>& out) {
    for (const auto& shader : list)
        if (pred(shader)) {
            out = shader;
            return true;
        }
    return false;
}

std::string RegisterName(uint32_t t, bool pixel, bool destination) {
    const uint32_t type = ((t >> 28) & 0x7u) | ((t >> 8) & 0x18u);
    const uint32_t number = t & 0x7FFu;
    char text[32];
    switch (type) {
        case 0: std::snprintf(text, sizeof text, "r%u", number); break;
        case 1: std::snprintf(text, sizeof text, "v%u", number); break;
        case 2: std::snprintf(text, sizeof text, "c%u", number); break;
        case 3: std::snprintf(text, sizeof text, pixel ? "t%u" : "a%u", number); break;
        case 4: std::snprintf(text, sizeof text, "%s", number == 0 ? "oPos" : number == 1 ? "oFog" : "oPts"); break;
        case 5: std::snprintf(text, sizeof text, "oD%u", number); break;
        case 6: std::snprintf(text, sizeof text, "oT%u", number); break;
        default: std::snprintf(text, sizeof text, "?%u_%u", type, number); break;
    }
    std::string name = text;
    if (destination) {
        const uint32_t mask = (t >> 16) & 0xFu;
        if (mask != 0xFu) {
            name += '.';
            for (int k = 0; k < 4; ++k)
                if (mask & (1u << k)) name += "xyzw"[k];
        }
    } else {
        const uint32_t swizzle = (t >> 16) & 0xFFu;
        if (swizzle != 0xE4u) {
            const int c[4] = { (int)(swizzle & 3), (int)((swizzle >> 2) & 3), (int)((swizzle >> 4) & 3),
                               (int)((swizzle >> 6) & 3) };
            name += '.';
            if (c[0] == c[1] && c[1] == c[2] && c[2] == c[3]) name += "xyzw"[c[0]];
            else
                for (int k = 0; k < 4; ++k) name += "xyzw"[c[k]];
        }
        const uint32_t modifier = (t >> 24) & 0xFu;
        if (modifier == 1) name = "-" + name;
        else if (modifier != 0) name = "(mod" + std::to_string(modifier) + ")" + name;
    }
    return name;
}

const char* OpName(uint32_t op) {
    switch (op) {
        case 0x00: return "nop";
        case 0x01: return "mov";
        case 0x02: return "add";
        case 0x03: return "sub";
        case 0x04: return "mad";
        case 0x05: return "mul";
        case 0x06: return "rcp";
        case 0x07: return "rsq";
        case 0x08: return "dp3";
        case 0x09: return "dp4";
        case 0x0A: return "min";
        case 0x0B: return "max";
        case 0x0C: return "slt";
        case 0x0D: return "sge";
        case 0x0E: return "exp";
        case 0x0F: return "log";
        case 0x10: return "lit";
        case 0x11: return "dst";
        case 0x12: return "lrp";
        case 0x13: return "frc";
        case kOpDcl: return "dcl";
        case kOpTex: return "tex";
        case kOpDef: return "def";
        default: return "op?";
    }
}

} // namespace

bool ExtractWorldEffects(const std::wstring& exePath, WorldEffects& out, std::string& error) {
    out = WorldEffects{};
    HMODULE module = ::LoadLibraryExW(exePath.c_str(), nullptr, LOAD_LIBRARY_AS_DATAFILE | LOAD_LIBRARY_AS_IMAGE_RESOURCE);
    if (!module) {
        error = "cannot open SPEED2.EXE";
        return false;
    }
    bool ok = true;
    std::vector<std::vector<uint32_t>> vs, ps;
    auto has = [](const char* text) {
        return [text](const std::vector<uint32_t>& s) { return Contains(s.data(), s.size(), text); };
    };
    auto withTex = [](const std::vector<uint32_t>& s) { return HasInstruction(s, kOpTex); };

    // IDI_WORLD_FX (rva 0x4BFD70, 13100 bytes): pass p0's VS reads FogValue and TextureOffset (pass p1's
    // is the depth-of-field mask with FocalRange); p0's PS samples t0 (p1's only copies v0).
    if (!ShadersOf(module, L"IDI_WORLD_FX", vs, ps) ||
        !Pick(vs, [&](const std::vector<uint32_t>& s) { return has("FogValue")(s) && has("TextureOffset")(s); },
              out.world.vertexShader) ||
        !Pick(ps, withTex, out.world.pixelShader)) {
        error = "IDI_WORLD_FX: no technique world pass p0 shaders";
        ok = false;
    }
    vs.clear();
    ps.clear();
    // IDI_WORLDREFLECT_FX (rva 0x4CEC68, 14808 bytes): one VS (FogValue) and one PS (reflected_sampler).
    if (ok && (!ShadersOf(module, L"IDI_WORLDREFLECT_FX", vs, ps) || !Pick(vs, has("FogValue"), out.reflect.vertexShader) ||
               !Pick(ps, has("reflected_sampler"), out.reflect.pixelShader))) {
        error = "IDI_WORLDREFLECT_FX: no technique world pass p0 shaders";
        ok = false;
    }
    vs.clear();
    ps.clear();
    // IDI_WORLDNOFOG_FX (rva 0x4C3400, 10812 bytes): VS with TextureOffset only.
    if (ok && (!ShadersOf(module, L"IDI_WORLDNOFOG_FX", vs, ps) ||
               !Pick(vs,
                     [&](const std::vector<uint32_t>& s) {
                         return has("TextureOffset")(s) && !has("FogValue")(s) && !has("FocalRange")(s);
                     },
                     out.noFog.vertexShader) ||
               !Pick(ps, withTex, out.noFog.pixelShader))) {
        error = "IDI_WORLDNOFOG_FX: no technique world pass p0 shaders";
        ok = false;
    }
    vs.clear();
    ps.clear();
    // IDI_FILTER_FX (rva 0x4DB2D0): pass p3's PS samples four textures (filter_sampler3), its VS declares a
    // position and four texture coordinates.
    if (ok && ShadersOf(module, L"IDI_FILTER_FX", vs, ps)) {
        auto dclCount = [](const std::vector<uint32_t>& s) {
            int n = 0;
            ForEachInstruction(s, [&](uint32_t op, size_t, size_t, uint32_t) { n += op == kOpDcl; });
            return n;
        };
        if (!Pick(vs, [&](const std::vector<uint32_t>& s) { return dclCount(s) == 5; }, out.filter.vertexShader) ||
            !Pick(ps, has("filter_sampler3"), out.filter.pixelShader))
            out.filter = EffectCode{};
    }
    ::FreeLibrary(module);
    return ok;
}

namespace {

constexpr uint32_t kVs20 = 0xFFFE0200u;
constexpr uint32_t kPs20 = 0xFFFF0200u;

// Float and sampler registers by name from the shader's top-level CTAB comment (D3DXSHADER_CONSTANTTABLE,
// D3DXSHADER_CONSTANTINFO entries of 20 bytes, offsets from the table start). The preshader's own CTAB
// sits inside its PRES comment and is not read.
std::unordered_map<std::string, int> ConstantTable(const std::vector<uint32_t>& tokens) {
    std::unordered_map<std::string, int> out;
    size_t i = 1;
    while (i < tokens.size()) {
        const uint32_t token = tokens[i];
        if ((token & 0xFFFFu) != 0xFFFEu) break;   // comments come before the first instruction
        const size_t words = token >> 16;
        if (words >= 7 && i + 1 + words <= tokens.size() && tokens[i + 1] == 0x42415443u /* CTAB */) {
            const uint8_t* table = reinterpret_cast<const uint8_t*>(&tokens[i + 2]);
            const size_t size = (words - 1) * 4;
            auto u32 = [&](size_t o) { uint32_t v = 0; if (o + 4 <= size) std::memcpy(&v, table + o, 4); return v; };
            auto u16 = [&](size_t o) { uint16_t v = 0; if (o + 2 <= size) std::memcpy(&v, table + o, 2); return v; };
            const uint32_t count = u32(12), info = u32(16);
            for (uint32_t k = 0; k < count && k < 1024; ++k) {
                const size_t entry = (size_t)info + k * 20;
                if (entry + 20 > size) break;
                const size_t name = u32(entry);
                if (name >= size) continue;
                const size_t end = ::strnlen(reinterpret_cast<const char*>(table + name), size - name);
                const uint16_t set = u16(entry + 4);
                if (set == 2 || set == 3)   // D3DXRS_FLOAT4, D3DXRS_SAMPLER
                    out.emplace(std::string(reinterpret_cast<const char*>(table + name), end), (int)u16(entry + 6));
            }
            break;
        }
        i += 1 + words;
    }
    return out;
}

bool HasAll(const std::unordered_map<std::string, int>& table, std::initializer_list<const char*> names) {
    for (const char* name : names)
        if (!table.count(name)) return false;
    return true;
}

} // namespace

namespace {

// A D3DXSHADER_CONSTANTTABLE body (after its fourcc): name -> register, count; float4 and sampler sets.
void ReadConstantTable(const uint8_t* table, size_t size, std::unordered_map<std::string, ShaderProgram::Slot>& out) {
    auto u32 = [&](size_t o) { uint32_t v = 0; if (o + 4 <= size) std::memcpy(&v, table + o, 4); return v; };
    auto u16 = [&](size_t o) { uint16_t v = 0; if (o + 2 <= size) std::memcpy(&v, table + o, 2); return v; };
    const uint32_t count = u32(12), info = u32(16);
    for (uint32_t k = 0; k < count && k < 1024; ++k) {
        const size_t entry = (size_t)info + k * 20;
        if (entry + 20 > size) break;
        const size_t name = u32(entry);
        if (name >= size) continue;
        const size_t end = ::strnlen(reinterpret_cast<const char*>(table + name), size - name);
        const uint16_t set = u16(entry + 4);
        if (set != 2 && set != 3) continue;   // D3DXRS_FLOAT4, D3DXRS_SAMPLER
        ShaderProgram::Slot slot;
        slot.reg = u16(entry + 6);
        slot.count = (std::max)(1, (int)u16(entry + 8));
        slot.sampler = set == 3;
        out.emplace(std::string(reinterpret_cast<const char*>(table + name), end), slot);
    }
}

} // namespace

bool LoadShaderProgram(const std::vector<uint32_t>& code, ShaderProgram& out) {
    out = ShaderProgram{};
    out.code = code;
    size_t i = 1;
    while (i < code.size() && (code[i] & 0xFFFFu) == 0xFFFEu) {
        const size_t words = code[i] >> 16;
        if (i + 1 + words > code.size()) break;
        const uint32_t* body = &code[i + 1];
        if (words >= 1 && body[0] == 0x42415443u /* CTAB */) {
            ReadConstantTable(reinterpret_cast<const uint8_t*>(body + 1), (words - 1) * 4, out.table);
        } else if (words >= 2 && body[0] == 0x53455250u /* PRES */) {
            size_t m = 2;
            while (m < words) {
                if ((body[m] & 0xFFFFu) != 0xFFFEu) { ++m; continue; }
                const size_t n = body[m] >> 16;
                if (m + 1 + n > words || n == 0) break;
                const uint32_t* inner = body + m + 1;
                const uint32_t tag = inner[0];
                if (tag == 0x42415443u) {   // CTAB: the preshader's inputs
                    std::unordered_map<std::string, ShaderProgram::Slot> inputs;
                    ReadConstantTable(reinterpret_cast<const uint8_t*>(inner + 1), (n - 1) * 4, inputs);
                    out.presInputs.assign(inputs.begin(), inputs.end());
                } else if (tag == 0x54494C43u && n >= 2) {   // CLIT: count, doubles
                    const size_t count = (std::min)((size_t)inner[1], (n - 2) / 2);
                    out.presLiterals.resize(count);
                    std::memcpy(out.presLiterals.data(), inner + 2, count * 8);
                } else if (tag == 0x434C5846u && n >= 2) {   // FXLC: count, then instructions
                    const uint32_t* p = inner + 2;
                    const uint32_t* end = inner + n;
                    for (uint32_t k = 0; k < inner[1] && p + 2 <= end; ++k) {
                        ShaderProgram::Op op;
                        op.opcode = (p[0] >> 20) & 0x7FFu;
                        op.comps = p[0] & 0xFFFFu;
                        op.scalar = (p[0] & 0x80000000u) != 0;
                        const uint32_t args = p[1];
                        p += 2;
                        for (uint32_t a = 0; a <= args && p + 3 <= end; ++a) {
                            if (p[0] != 0) { out.presValid = false; break; }   // relative addressing
                            ShaderProgram::Arg arg{ p[1], p[2] };
                            p += 3;
                            (a < args ? op.in.push_back(arg), op.in.back() : op.out) = arg;
                        }
                        switch (op.opcode) {
                            case 0x100: case 0x101: case 0x103: case 0x107: case 0x200: case 0x201: case 0x204:
                            case 0x205: case 0x500: break;
                            default: out.presValid = false; break;
                        }
                        out.presOps.push_back(std::move(op));
                    }
                }
                m += 1 + n;
            }
        }
        i += 1 + words;
    }
    return !out.code.empty();
}

namespace {

// fx_2_0 (Wine d3dx9 effect.c): tag 0xFEFF0901, offset; the parameter section at 8 + offset holds
// parameter count, technique count, a dword, object count, then per parameter {typedef offset, value
// offset, flags, annotation count} and annotation pairs; offsets count from byte 8. A typedef starts
// {type, class, name offset, semantic offset, elements} and, for scalar / vector / matrix classes,
// {columns, rows}. Strings are {length, characters}.
void ReadEffectDefaults(const uint8_t* data, size_t size, std::unordered_map<std::string, std::vector<float>>& out) {
    auto u32 = [&](size_t o) { uint32_t v = 0; if (o + 4 <= size) std::memcpy(&v, data + o, 4); return v; };
    if (size < 24 || u32(0) != 0xFEFF0901u) return;
    const size_t base = 8;
    size_t p = base + u32(4);
    const uint32_t count = u32(p);
    p += 16;
    auto text = [&](uint32_t at) -> std::string {
        const size_t o = base + at;
        const uint32_t n = u32(o);
        if (!at || o + 4 + n > size) return {};
        return std::string(reinterpret_cast<const char*>(data + o + 4), ::strnlen(reinterpret_cast<const char*>(data + o + 4), n));
    };
    for (uint32_t i = 0; i < count && i < 1024 && p + 16 <= size; ++i) {
        const uint32_t typedefAt = u32(p), valueAt = u32(p + 4), annotations = u32(p + 12);
        p += 16 + 8ull * annotations;
        const size_t t = base + typedefAt;
        const uint32_t type = u32(t), cls = u32(t + 4), elements = u32(t + 16);
        if (type != 3 || cls > 3) continue;   // floats: scalar, vector, matrix rows / columns
        const uint32_t n = u32(t + 20) * u32(t + 24) * (std::max)(1u, elements);
        if (n == 0 || n > 1024 || base + valueAt + 4ull * n > size) continue;
        std::vector<float> values(n);
        std::memcpy(values.data(), data + base + valueAt, 4ull * n);
        out[text(u32(t + 8))] = std::move(values);
    }
}

} // namespace

bool ExtractGameEffect(const std::wstring& exePath, const wchar_t* resource, size_t vsDwords, size_t psDwords,
                       const char* name, GameEffect& out, std::string& error) {
    out = GameEffect{};
    out.name = name;
    HMODULE module = ::LoadLibraryExW(exePath.c_str(), nullptr, LOAD_LIBRARY_AS_DATAFILE | LOAD_LIBRARY_AS_IMAGE_RESOURCE);
    if (!module) { error = "cannot open nfsw.exe"; return false; }
    std::vector<std::vector<uint32_t>> vs, ps;
    bool found = ShadersOf(module, resource, vs, ps, kVs20, kPs20);
    found |= ShadersOf(module, resource, vs, ps, 0xFFFE0300u, 0xFFFF0300u);   // vs_3_0 / ps_3_0 (road_3_0_A)
    if (HRSRC info = ::FindResourceW(module, resource, MAKEINTRESOURCEW(10))) {
        HGLOBAL handle = ::LoadResource(module, info);
        if (const void* bytes = handle ? ::LockResource(handle) : nullptr)
            ReadEffectDefaults(static_cast<const uint8_t*>(bytes), ::SizeofResource(module, info), out.defaults);
    }
    ::FreeLibrary(module);
    std::vector<uint32_t> v, p;
    if (!found || !Pick(vs, [&](const std::vector<uint32_t>& s) { return s.size() == vsDwords; }, v) ||
        !Pick(ps, [&](const std::vector<uint32_t>& s) { return s.size() == psDwords; }, p)) {
        error = std::string(name) + ": no vs " + std::to_string(vsDwords) + " / ps " + std::to_string(psDwords) +
                " dwords in its resource";
        return false;
    }
    LoadShaderProgram(v, out.vs);
    LoadShaderProgram(p, out.ps);
    if (!out.vs.presValid || !out.ps.presValid) {
        error = std::string(name) + ": preshader with an op this reader does not run";
        out = GameEffect{};
        return false;
    }
    return true;
}

NfswLight ComputeNfswLight(const LightingStage& from, const LightingStage& to, float t, const SunAndSkyStage* sunFrom,
                           const SunAndSkyStage* sunTo, std::string& source) {
    // rea-lighting-fog.md: FUN_006D40A0 loads timeofdaylighting, FUN_006E3D50 sunandsky, FUN_006E30E0 lerps
    // every field linearly, FUN_007A2EF0 / FUN_0063AD40 / FUN_007AF370 set the world constants.
    NfswLight light;
    auto lerp = [t](float a, float b) { return a + (b - a) * t; };
    auto field = [&](const LightingStage& stage, u32 hash, size_t i = 0) {
        const auto it = stage.fields.find(hash);
        return it != stage.fields.end() && i < it->second.size() ? it->second[i] : 0.0f;
    };
    auto both = [&](u32 hash, size_t i = 0) { return lerp(field(from, hash, i), field(to, hash, i)); };
    for (int k = 0; k < 4; ++k) {
        light.diffuse[k] = 2.0f * lerp(from.diffuse[k], to.diffuse[k]);   // L+0x00 = 2 x DiffuseColour
        light.ambient[k] = k < 3 ? lerp(from.ambient[k], to.ambient[k]) : 4.0f;   // L+0x10, w = 4
        light.specular[k] = lerp(from.specular[k], to.specular[k]);
        light.specular2[k] = both(VltHash("SpecularColour2"), (size_t)k);
        light.light2Dir[k] = both(0x707F1B6Cu, (size_t)k);
    }
    light.specularPower = both(VltHash("SpecularPower"));
    light.specularPower2 = both(VltHash("SpecularPower2"));
    light.todSelector7 = both(0x507AFAFCu);
    light.todSelector8 = both(VltHash("ChristmasLights"));
    // Sun: (cos A sin Z, sin A sin Z, cos Z), A = 0xD4496F5F, Z = 0x78E76168 degrees; lerped, normalised.
    auto sunDirection = [](const SunAndSkyStage* sun, float out[3]) {
        if (!sun) return false;
        const auto az = sun->angles.find(0xD4496F5Fu), ze = sun->angles.find(0x78E76168u);
        if (az == sun->angles.end() || ze == sun->angles.end()) return false;
        const float d2r = 3.14159265f / 180.0f;
        out[0] = std::cos(az->second * d2r) * std::sin(ze->second * d2r);
        out[1] = std::sin(az->second * d2r) * std::sin(ze->second * d2r);
        out[2] = std::cos(ze->second * d2r);
        return true;
    };
    // The shadow map's light, the second vector (L+0x120): same formula with 0xACB78DC4 / 0xA480C321.
    auto vectorOf = [](const SunAndSkyStage* stage, u32 azimuth, u32 zenith, float out[3]) {
        if (!stage) return false;
        const auto az = stage->angles.find(azimuth), ze = stage->angles.find(zenith);
        if (az == stage->angles.end() || ze == stage->angles.end()) return false;
        const float d2r = 3.14159265f / 180.0f;
        out[0] = std::cos(az->second * d2r) * std::sin(ze->second * d2r);
        out[1] = std::sin(az->second * d2r) * std::sin(ze->second * d2r);
        out[2] = std::cos(ze->second * d2r);
        return true;
    };
    float sa[3], sb[3];
    if (vectorOf(sunFrom, 0xACB78DC4u, 0xA480C321u, sa) && vectorOf(sunTo, 0xACB78DC4u, 0xA480C321u, sb)) {
        float n = 0.0f;
        for (int k = 0; k < 3; ++k) { light.shadowVec[k] = lerp(sa[k], sb[k]); n += light.shadowVec[k] * light.shadowVec[k]; }
        n = std::sqrt(n);
        if (n > 1e-6f)
            for (int k = 0; k < 3; ++k) light.shadowVec[k] /= n;
    }
    float a[3], b[3];
    const bool sun = sunDirection(sunFrom, a) && sunDirection(sunTo, b);
    if (sun) {
        for (int k = 0; k < 3; ++k) light.lightVec[k] = lerp(a[k], b[k]);
        const float length = std::sqrt(light.lightVec[0] * light.lightVec[0] + light.lightVec[1] * light.lightVec[1] +
                                       light.lightVec[2] * light.lightVec[2]);
        if (length > 1e-6f)
            for (int k = 0; k < 3; ++k) light.lightVec[k] /= length;
        light.lightVec[3] = 0.0f;
    }
    // cvTimeOfDayParams0 = (1 for the world selectors 0..6, 0x76E6BE76, 0xCCF24433 headlights, 0x13208780);
    // cvTimeOfDayParams1 = (0xDD4C2632, 2 x 0x4454FC3B) with the ramp k ([0xC84F48]) at 0.
    light.timeOfDay0[0] = 1.0f;
    light.timeOfDay0[1] = both(0x76E6BE76u);
    light.timeOfDay0[2] = both(0xCCF24433u);
    light.timeOfDay0[3] = both(0x13208780u);
    light.timeOfDay1[0] = both(0xDD4C2632u);
    light.timeOfDay1[1] = 2.0f * both(0x4454FC3Bu);
    light.timeOfDay1[2] = light.timeOfDay1[3] = 0.0f;
    // Scattering fog (FUN_007AF370).
    const float skyScale = lerp(from.fogSkyScale, to.fogSkyScale), hazeScale = lerp(from.fogHazeScale, to.fogHazeScale);
    const float sR = skyScale * 0.01f * 0.0099f + 1e-4f;
    const float sM = hazeScale > 0.0f ? hazeScale * 0.01f * 0.0099f + 1e-4f : 0.0f;
    const float pi = 3.14159265f;
    for (int k = 0; k < 3; ++k) {
        const float br = lerp(from.fogSky[k], to.fogSky[k]) * sR;
        const float bm = (1.0f - lerp(from.fogHaze[k], to.fogHaze[k])) * sM;
        light.fogBrPlusBm[k] = br + bm;
        light.fogOneOverBrBm[k] = br + bm > 0.0f ? 1.0f / (br + bm) : 0.0f;
        light.fogConst1[k] = br * 3.0f / (16.0f * pi);
        light.fogConst2[k] = bm / (4.0f * pi);
    }
    const float g = lerp(from.fogSunFalloff, to.fogSunFalloff);
    light.fogConst3[0] = (1.0f - g) * (1.0f - g);
    light.fogConst3[1] = 1.0f + g * g;
    light.fogConst3[2] = -2.0f * g;
    light.fogConst3[3] = lerp(from.fogInLightScatter, to.fogInLightScatter);
    light.fogValueMW[0] = lerp(from.fogDistanceScale, to.fogDistanceScale);
    light.fogEnable[0] = 1.0f;   // the captured frame's cfFogEnable; its writer is not found
    light.shadowStrength = lerp(from.shadowStrength, to.shadowStrength);
    source = std::string("attributes.bin ") + from.name + " -> " + to.name + (sun ? "" : " (no sunandsky angles)");
    return light;
}

std::vector<std::string> Disassemble(const std::vector<uint32_t>& tokens) {
    std::vector<std::string> lines;
    if (tokens.empty()) return lines;
    const bool pixel = tokens[0] == kPs11;
    lines.push_back(pixel ? "ps_1_1" : "vs_1_1");
    ForEachInstruction(tokens, [&](uint32_t op, size_t first, size_t count, uint32_t instruction) {
        std::string line;
        if (instruction & 0x40000000u) line += '+';
        if (op == kOpDef && count == 5) {
            char text[128];
            float v[4];
            std::memcpy(v, &tokens[first + 1], sizeof v);
            std::snprintf(text, sizeof text, "def %s, %g, %g, %g, %g", RegisterName(tokens[first], pixel, true).c_str(),
                          v[0], v[1], v[2], v[3]);
            lines.push_back(line + text);
            return;
        }
        if (op == kOpDcl && count == 2) {
            const uint32_t usage = tokens[first] & 0x1Fu;
            const char* name = usage == 0 ? "position" : usage == 3 ? "normal" : usage == 5 ? "texcoord"
                             : usage == 10 ? "color" : "usage?";
            lines.push_back(line + "dcl_" + name + " " + RegisterName(tokens[first + 1], pixel, true));
            return;
        }
        line += OpName(op);
        if (count > 0) {
            const uint32_t shift = (tokens[first] >> 24) & 0xFu;
            if (shift == 1) line += "_x2";
            else if (shift == 2) line += "_x4";
            else if (shift == 0xF) line += "_d2";
            if ((tokens[first] >> 20) & 1u) line += "_sat";
            line += ' ';
            for (size_t k = 0; k < count; ++k) {
                if (k) line += ", ";
                line += RegisterName(tokens[first + k], pixel, k == 0);
            }
        }
        lines.push_back(line);
    });
    return lines;
}

u32 GameHash(const char* text) {
    u32 h = 0xFFFFFFFFu;
    for (const unsigned char* p = reinterpret_cast<const unsigned char*>(text); *p; ++p) h = h * 33u + *p;
    return h;
}

FogConstants MakeFogConstants(const FogState& fog) {
    FogConstants c;
    c.value[0] = 250.0f;
    c.value[1] = 1.0f / (250.0f - fog.start);
    const float density = 1.0f - fog.factor;
    c.value[2] = density <= 0.6f ? density : 0.6f;
    c.value[3] = 0.0f;
    // sub_614300 truncates each field into a byte (__ftol2), eDisplayFrame scales each byte by 0.75.
    auto channel = [](float v) {
        const int byte = (int)v & 0xFF;
        return (D3DCOLOR)((int)((float)byte * 0.75f) & 0xFF);
    };
    c.colour = 0xFF000000u | (channel(fog.r) << 16) | (channel(fog.g) << 8) | channel(fog.b);
    return c;
}

// ---------------------------------------------------------------------------
// Fog parameter maps
// ---------------------------------------------------------------------------

namespace {

struct Span {
    const uint8_t* data = nullptr;
    size_t size = 0;
};

template <typename Visit>
void ForEachChunk(Span span, Visit visit) {
    size_t at = 0;
    while (at + 8 <= span.size) {
        uint32_t id = 0, size = 0;
        std::memcpy(&id, span.data + at, 4);
        std::memcpy(&size, span.data + at + 4, 4);
        if (at + 8 + size > span.size) break;
        visit(id, Span{ span.data + at + 8, size });
        at += 8 + (size_t)size;
    }
}

} // namespace

bool FogMap::Load(const std::wstring& bunPath) {
    sets_.clear();
    tree_.clear();
    haveLast_ = false;
    HANDLE file = ::CreateFileW(bunPath.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    // Walk the top-level chunks, reading only the parameter maps.
    std::vector<uint8_t> chunk;
    LARGE_INTEGER position{};
    for (;;) {
        uint32_t header[2] = {};
        DWORD got = 0;
        if (!::ReadFile(file, header, 8, &got, nullptr) || got != 8) break;
        if (header[0] == 0x8003B600u) {
            chunk.resize(header[1]);
            if (!::ReadFile(file, chunk.data(), header[1], &got, nullptr) || got != header[1]) chunk.clear();
            break;
        }
        LARGE_INTEGER skip{};
        skip.QuadPart = header[1];
        if (!::SetFilePointerEx(file, skip, &position, FILE_CURRENT)) break;
    }
    ::CloseHandle(file);
    if (chunk.empty()) return false;

    const u32 wanted = GameHash("Normal Fog");
    ForEachChunk(Span{ chunk.data(), chunk.size() }, [&](uint32_t id, Span layer) {
        if (id != 0x8003B601u || !sets_.empty()) return;
        Span header, types, offsets, data, q8, q16;
        ForEachChunk(layer, [&](uint32_t sub, Span s) {
            switch (sub) {
                case 0x3B602: header = s; break;
                case 0x3B603: types = s; break;
                case 0x3B604: offsets = s; break;
                case 0x3B605: data = s; break;
                case 0x3B607: q8 = s; break;
                case 0x3B608: q16 = s; break;
                default: break;
            }
        });
        if (header.size < 0x24) return;
        uint32_t h[9];
        std::memcpy(h, header.data, sizeof h);
        if (h[0] != wanted) return;
        float bounds[4];
        std::memcpy(bounds, header.data + 4, sizeof bounds);
        const uint32_t nodes = h[5], count = h[6], stride = h[7], fields = h[8];
        if (fields < 5 || types.size != 4 * fields || offsets.size != 4 * fields || data.size != (size_t)count * stride)
            return;
        std::vector<uint16_t> tree;
        if (q8.size == 4ull * nodes && nodes) {
            for (size_t i = 0; i < q8.size; ++i) tree.push_back(q8.data[i]);
        } else if (q16.size == 8ull * nodes && nodes) {
            tree.resize(nodes * 4ull);
            std::memcpy(tree.data(), q16.data, q16.size);
        }
        std::vector<std::vector<float>> sets(count, std::vector<float>(8, 0.0f));
        for (uint32_t s = 0; s < count; ++s)
            for (uint32_t f = 0; f < fields && f < 8; ++f) {
                int32_t type = 0, offset = 0;
                std::memcpy(&type, types.data + 4 * f, 4);
                std::memcpy(&offset, offsets.data + 4 * f, 4);
                if (offset < 0 || (size_t)offset + 4 > stride) continue;
                const uint8_t* p = data.data + (size_t)s * stride + offset;
                if (type == 0) std::memcpy(&sets[s][f], p, 4);
                else {
                    int32_t v = 0;
                    std::memcpy(&v, p, 4);
                    sets[s][f] = (float)v;
                }
            }
        minX_ = bounds[0];
        minY_ = bounds[1];
        maxX_ = bounds[2];
        maxY_ = bounds[3];
        tree_ = std::move(tree);
        sets_ = std::move(sets);
    });
    return !sets_.empty();
}

int FogMap::SetIndexAt(float x, float y) const {
    if (sets_.empty()) return -1;
    if (tree_.empty()) return 0;
    // GetParameterSetIndexFromQuadData16 (0x5F0A30): its out-of-bounds test can never pass, so a point
    // outside the bounds walks into the edge cells.
    float minX = minX_, minY = minY_, maxX = maxX_, maxY = maxY_;
    size_t n = 0;
    for (int guard = 0; guard < 64; ++guard) {
        if (n * 4 + 3 >= tree_.size()) return 0;
        const uint16_t* c = &tree_[n * 4];
        if (c[0] == 0) {
            const int set = c[1];
            return set < (int)sets_.size() ? set : 0;
        }
        const float mx = (minX + maxX) * 0.5f, my = (minY + maxY) * 0.5f;
        if (x < mx) {
            maxX = mx;
            if (y < my) { n = c[0]; maxY = my; }
            else { n = c[1]; minY = my; }
        } else {
            minX = mx;
            if (y < my) { n = c[2]; maxY = my; }
            else { n = c[3]; minY = my; }
        }
    }
    return 0;
}

FogState FogMap::Sample(float x, float y) {
    FogState fog;   // the exe's fallback struct 0x800E18 when there is no map
    if (sets_.empty()) return fog;
    const std::vector<float>& set = sets_[(size_t)(std::max)(0, SetIndexAt(x, y))];
    if (!haveLast_ || smoothed_.size() != set.size()) {
        smoothed_ = set;
    } else {
        const float dx = x - lastX_, dy = y - lastY_;
        const float t = (std::min)(std::sqrt(dx * dx + dy * dy) / 50.0f, 1.0f);
        for (size_t i = 0; i < set.size(); ++i) smoothed_[i] = smoothed_[i] * (1.0f - t) + set[i] * t;
    }
    haveLast_ = true;
    lastX_ = x;
    lastY_ = y;
    // Fields: 0 factor, 1 start, 2 blue, 3 green, 4 red (sub_614300 0x614437..0x6145B2).
    fog.factor = smoothed_[0];
    fog.start = smoothed_[1];
    fog.b = smoothed_[2];
    fog.g = smoothed_[3];
    fog.r = smoothed_[4];
    return fog;
}

} // namespace gamelook
