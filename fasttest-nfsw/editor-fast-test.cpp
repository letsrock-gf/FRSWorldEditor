// editor-fast-test.asi: puts the local car where the FRS World Editor's camera
// was when Launch Game was pressed. The editor writes
//   pose <x> <y> <ground z> <heading degrees>
// to <game>\scripts\editor-fast-test.txt (render coordinates, Z up, heading 0
// = +X, counter-clockwise), or the one word
//   menus
// when it found no ground under the camera. This reads it once at start,
// renames it .used, passes the title and the driver select by code, and (with
// a pose) moves the car once it is in free roam.
//
// nfsw.exe addresses (static, base 0x400000; sha256 b2c616c6...c819), REA 14/11:
//   0x706340  Sim GetPosition, __thiscall(sim) -> const V* (physics). Hooked: it
//             hands over the local Sim (same hook as liberty-spawn).
//   0x7D1210 / 0x4A9BE0 / 0x77BA20  Sim->player, player IsLocal, player->Sim
//             (local Sim check, as driving-controls / living-traffic).
//   sim+0x84  IRigidBody*, vtable 0xBDAFC8, every setter __thiscall(body, const T*):
//     +0x5C 0x842E10 SetPosition(V)      record +0x10
//     +0x60 0x842E40 SetLinearVelocity(V) record +0x20
//     +0x64 0x842E70 SetAngularVelocity(V) record +0x30
//     +0x74 0x842C30 SetOrientation(Quat x,y,z,w) record +0x00, rows +0x70 (FUN_00751C60)
//     +0x34 0x6FC610 GetForwardVector = row 2 (+0x90); +0x30 0x6FC560 = |v.xz| (Y up)
//   record = [0xDCF1D8] + body[+0x30] * 0xB0
//   [0xCFC730] == 6  in gameplay (liberty-spawn's ready()).
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <bcrypt.h>
#include <atomic>
#include <cstdarg>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <initializer_list>
#ifndef EDITOR_FAST_TEST_TEST
#include <MinHook.h>
#endif

namespace {
struct V { float x, y, z; };
struct Quat { float x, y, z, w; };
struct Pose { float x, y, z, heading; bool valid; };   // valid: the request carried a pose

// Physics is Y up; render = (p.z, -p.x, p.y) (liberty-spawn physicsToRender).
V renderToPhysics(V r) { return { -r.y, r.z, r.x }; }
V physicsToRender(V p) { return { p.z, -p.x, p.y }; }
// Turn about physics Y that sends the forward row to render (cos h, sin h, 0).
Quat headingQuat(float degrees) {
    const float t = -degrees * 3.14159265f / 180.0f;
    return { 0, std::sin(t / 2), 0, std::cos(t / 2) };
}
// Row 2 of FUN_00751C60's matrix: the body's forward vector.
V forwardRow(Quat q) {
    return { 2 * (q.x * q.z + q.w * q.y), 2 * (q.y * q.z - q.w * q.x), 1 - 2 * (q.x * q.x + q.y * q.y) };
}

// Reads the editor's request and renames the file .used, so it counts once. A
// file older than maxAgeSeconds is from a launch that never reached the game.
// True for a fresh request; pose.valid says whether it carried a pose. Without
// one (the editor found no ground under its camera and wrote "menus") the
// title and the driver select are still skipped, and the car is left alone.
bool takePose(const char* path, Pose& pose, double maxAgeSeconds) {
    WIN32_FILE_ATTRIBUTE_DATA info{};
    if (!GetFileAttributesExA(path, GetFileExInfoStandard, &info)) return false;
    FILETIME nowFile{};
    GetSystemTimeAsFileTime(&nowFile);
    ULARGE_INTEGER now, written;
    now.LowPart = nowFile.dwLowDateTime, now.HighPart = nowFile.dwHighDateTime;
    written.LowPart = info.ftLastWriteTime.dwLowDateTime, written.HighPart = info.ftLastWriteTime.dwHighDateTime;
    const double age = (double(now.QuadPart) - double(written.QuadPart)) / 1e7;
    bool ok = false;
    if (FILE* file = std::fopen(path, "r")) {
        ok = std::fscanf(file, "pose %f %f %f %f", &pose.x, &pose.y, &pose.z, &pose.heading) == 4 &&
             std::isfinite(pose.x) && std::isfinite(pose.y) && std::isfinite(pose.z) && std::isfinite(pose.heading);
        std::fclose(file);
    }
    char used[MAX_PATH + 8];
    std::snprintf(used, sizeof used, "%s.used", path);
    MoveFileExA(path, used, MOVEFILE_REPLACE_EXISTING);
    pose.valid = ok;
    return age <= maxAgeSeconds;
}
} // namespace

#ifdef EDITOR_FAST_TEST_TEST
// Test: editor-fast-test-test.exe <pose file written by the editor>
// Reads it the way the ASI does, then checks the axis and heading maths.
int main(int argc, char** argv) {
    int failures = 0;
    auto check = [&](bool ok, const char* what) { std::printf("%s %s\n", ok ? "ok  " : "FAIL", what); failures += !ok; };
    auto same = [](V a, V b) { return std::fabs(a.x - b.x) < 1e-4f && std::fabs(a.y - b.y) < 1e-4f && std::fabs(a.z - b.z) < 1e-4f; };
    for (float h : { 0.0f, 37.0f, 90.0f, 180.0f, 270.0f, 359.0f }) {
        const float r = h * 3.14159265f / 180.0f;
        char what[64];
        std::snprintf(what, sizeof what, "heading %.0f -> forward (cos, sin, 0)", h);
        check(same(physicsToRender(forwardRow(headingQuat(h))), { std::cos(r), std::sin(r), 0 }), what);
    }
    check(same(physicsToRender(renderToPhysics({ 1, 2, 3 })), { 1, 2, 3 }), "render -> physics -> render");
    check(renderToPhysics({ 0, 0, 5 }).y == 5, "render Z is physics Y (up)");
    if (argc > 1) {
        Pose pose{};
        const bool read = takePose(argv[1], pose, 600);
        std::printf("pose %.3f %.3f %.3f %.2f\n", pose.x, pose.y, pose.z, pose.heading);
        check(read && pose.valid, "editor's file read");
        check(GetFileAttributesA(argv[1]) == INVALID_FILE_ATTRIBUTES, "file renamed .used");
        check(!takePose(argv[1], pose, 600), "second read finds nothing");
        char used[MAX_PATH + 8];
        std::snprintf(used, sizeof used, "%s.used", argv[1]);
        MoveFileExA(used, argv[1], MOVEFILE_REPLACE_EXISTING);
        check(!takePose(argv[1], pose, -1), "stale file refused");
        MoveFileExA(used, argv[1], MOVEFILE_REPLACE_EXISTING);   // leave it as the editor wrote it
    }
    {   // a request without a pose still counts: the menus are skipped, the car is not moved
        char temp[MAX_PATH + 48];
        GetTempPathA(MAX_PATH, temp);
        std::strcat(temp, "editor-fast-test-selftest.txt");
        if (FILE* file = std::fopen(temp, "w")) { std::fputs("menus\n", file); std::fclose(file); }
        Pose none{};
        check(takePose(temp, none, 600) && !none.valid, "request without a pose: fresh, no pose");
        check(!takePose(temp, none, 600), "request without a pose: taken once");
        std::strcat(temp, ".used");
        DeleteFileA(temp);
    }
    std::printf("%s\n", failures ? "FAILED" : "PASSED");
    return failures != 0;
}
#else
namespace {
using Position = const V*(__thiscall*)(void*);
using Pointer = void*(__thiscall*)(void*);
using Bool = bool(__thiscall*)(void*);
using SetVector = void(__thiscall*)(void*, const V*);
using SetQuat = void(__thiscall*)(void*, const Quat*);

constexpr ULONGLONG kSettleMs = 3000;   // in free roam this long before the move
constexpr ULONGLONG kHoldMs = 2000;     // pinned there while the streamer loads it
constexpr float kLift = 1.0f;           // body centre above the editor's ground

uintptr_t image{};
Position originalPosition{};
HANDLE logFile = INVALID_HANDLE_VALUE;
Pose pose{};
std::atomic<bool> pending{}, busy{};
ULONGLONG inGameSince{}, placedAt{}, lastPin{};
thread_local bool inside{};

uintptr_t va(uintptr_t a) { return image + a - 0x400000; }
void log(const char* format, ...) {
    if (logFile == INVALID_HANDLE_VALUE) return;
    char line[400];
    va_list args;
    va_start(args, format);
    const int n = std::vsnprintf(line, sizeof line - 2, format, args);
    va_end(args);
    if (n <= 0) return;
    DWORD length = DWORD(std::strlen(line)), written;
    std::memcpy(line + length, "\r\n", 3);
    WriteFile(logFile, line, length + 2, &written, nullptr);
}
bool readable(const void* p, size_t n) { return p && !IsBadReadPtr(p, n); }
template <class T> T raw(const void* p, size_t o = 0) { T v; std::memcpy(&v, (const char*)p + o, sizeof v); return v; }
// vtable slot `offset` of p is `expected`; vtables live in the exe's .rdata.
bool method(const void* p, unsigned offset, uintptr_t expected) {
    if (!readable(p, 4)) return false;
    const auto table = raw<uintptr_t>(p);
    return table >= va(0xB00000) && table + offset + 4 <= va(0xD00000) && raw<uintptr_t>((void*)table, offset) == va(expected);
}
bool localSim(void* sim) {
    if (!readable(sim, 0x94) || !method(sim, 0x20, 0x7D1210) || !method(sim, 0x60, 0x706340) || !method(sim, 0x64, 0x6F8010))
        return false;
    void* player = reinterpret_cast<Pointer>(va(0x7D1210))(sim);
    return readable(player, 12) && method(player, 8, 0x4A9BE0) && method(player, 4, 0x77BA20) &&
           reinterpret_cast<Bool>(va(0x4A9BE0))(player) && reinterpret_cast<Pointer>(va(0x77BA20))(player) == sim;
}
void* rigidBody(void* sim) {
    void* body = raw<void*>(sim, 0x84);
    return method(body, 0x20, 0x6FC4A0) && method(body, 0x5C, 0x842E10) && method(body, 0x60, 0x842E40) &&
           method(body, 0x64, 0x842E70) && method(body, 0x74, 0x842C30) && method(body, 0x34, 0x6FC610) ? body : nullptr;
}
void place(void* body) {
    const V position = renderToPhysics({ pose.x, pose.y, pose.z + kLift }), zero{};
    const Quat turn = headingQuat(pose.heading);
    reinterpret_cast<SetQuat>(va(0x842C30))(body, &turn);
    reinterpret_cast<SetVector>(va(0x842E10))(body, &position);
    reinterpret_cast<SetVector>(va(0x842E40))(body, &zero);
    reinterpret_cast<SetVector>(va(0x842E70))(body, &zero);
}

void step(void* sim) {
    const ULONGLONG now = GetTickCount64();
    if (raw<uint32_t>((void*)va(0xCFC730)) != 6) { inGameSince = 0; return; }
    if (!inGameSince) {
        inGameSince = now;
        log("free roam reached; moving the car in %llu ms (sim thread %lu)", kSettleMs, GetCurrentThreadId());
    }
    if (now - inGameSince < kSettleMs) return;
    void* body = rigidBody(sim);
    if (!body) { log("local rigid body has an unknown vtable: nothing moved"); pending = false; return; }
    if (!placedAt) {
        const V before = physicsToRender(*originalPosition(sim));
        placedAt = now;
        log("car at (%.2f, %.2f, %.2f) -> (%.2f, %.2f, %.2f) heading %.1f", before.x, before.y, before.z,
            pose.x, pose.y, pose.z + kLift, pose.heading);
    }
    if (now - placedAt < kHoldMs) {
        if (now - lastPin >= 50) { place(body); lastPin = now; }
        return;
    }
    V forward{};
    reinterpret_cast<void(__thiscall*)(void*, V*)>(va(0x6FC610))(body, &forward);
    const V at = physicsToRender(*originalPosition(sim)), f = physicsToRender(forward);
    const float r = pose.heading * 3.14159265f / 180.0f;
    log("released at (%.2f, %.2f, %.2f), forward (%.3f, %.3f, %.3f), wanted (%.3f, %.3f, 0)", at.x, at.y, at.z,
        f.x, f.y, f.z, std::cos(r), std::sin(r));
    pending = false;
}

// ---- Menus by code (the editor's Fast test / Launch Game boots straight into the world) ----
// Same calls as LibertySpawn's bridge (REA 26/10), checked by their bytes before each call:
//   0x52DA90  services(), cdecl, guard [0xCECDF8]; title "PRESS ANY KEY" = services->vf[0x64/4]
//             (services, 0x429, 0, 0), stdcall (what TitleScreen's OnButtonPressed runs, 0x4867E0).
//   0x46EA00  PersonaSelectByIndex, cdecl(&{int index; u8 leaveScreen; u8 leaveToWorld}), guard [0xCE35FC];
//             persona list ready when [0xCE38B8] bit 0, records [0xCE3638+0x14, +0x18) of 0x78 bytes.
//   [0xCFC730] GameFlow: 3 frontend, 6 world.
// Run from a PeekMessageW hook: the game's own message pump, so on the window's thread.
using PeekMessageFn = BOOL(WINAPI*)(MSG*, HWND, UINT, UINT, UINT);
PeekMessageFn originalPeek{};
bool menusDone{};
ULONGLONG menusStart{}, frontendSince{}, lastPress{}, lastEnter{}, lastMenuStep{};
int presses{};
DWORD pumpThread{};

bool pressStart() {
    static const unsigned char prefix[] = { 0xb8, 1, 0, 0, 0, 0x84, 0x05 };
    const auto entry = (const unsigned char*)va(0x52DA90);
    if (!readable(entry, 11) || std::memcmp(entry, prefix, sizeof prefix) || raw<uint32_t>(entry, 7) != va(0xCECDF8)) {
        log("press start: 0x52DA90 bytes differ");
        return false;
    }
    void* services = reinterpret_cast<void*(__cdecl*)()>((void*)entry)();
    void** table = services ? raw<void**>(services) : nullptr;
    if (!readable(table, 26 * 4)) { log("press start: no services object yet"); return false; }
    reinterpret_cast<void(__stdcall*)(void*, int, int, int)>(table[25])(services, 0x429, 0, 0);
    return true;
}

unsigned personaCount() {
    if (!(raw<uint32_t>((void*)va(0xCE38B8)) & 1)) return 0;
    const auto first = raw<uintptr_t>((void*)va(0xCE3638 + 0x14)), last = raw<uintptr_t>((void*)va(0xCE3638 + 0x18));
    return first && last > first ? unsigned((last - first) / 0x78) : 0;
}

bool enterWorld() {
    static const unsigned char prefix[] = { 0x55, 0x8b, 0xec, 0xb8, 1, 0, 0, 0, 0x84, 0x05 };
    const auto entry = (const unsigned char*)va(0x46EA00);
    if (!readable(entry, 14) || std::memcmp(entry, prefix, sizeof prefix) || raw<uint32_t>(entry, 10) != va(0xCE35FC)) {
        log("enter world: 0x46EA00 bytes differ");
        return false;
    }
    struct { int index; uint8_t leaveScreen, leaveToWorld; } request{ 0, 1, 1 };
    const int hr = reinterpret_cast<int(__cdecl*)(void*)>((void*)entry)(&request);
    log("enter world: persona 0, hr %08x", unsigned(hr));
    return hr == 0;
}

// ponytail: fixed waits and retry counts (title 4 s, persona 2 s); a game state hook if they ever miss.
void menuStep() {
    const ULONGLONG now = GetTickCount64();
    if (now - lastMenuStep < 100) return;
    lastMenuStep = now;
    if (!menusStart) menusStart = now;
    const uint32_t flow = raw<uint32_t>((void*)va(0xCFC730));
    if (flow == 6 && frontendSince) {   // flow 6 also flickers during boot, before the frontend
        menusDone = true;
        log("world reached %.1f s after the first frame (pump thread %lu)", (now - menusStart) / 1000.0, pumpThread);
        return;
    }
    if (now - menusStart > 180000) { menusDone = true; log("menus: not in the world after 180 s, flow %u: stopped", flow); return; }
    if (flow != 3) return;
    if (!frontendSince) { frontendSince = now; log("frontend (flow 3), %u personas loaded", personaCount()); }
    // The persona list is loaded before the title goes, so it says nothing about the title. Cycle as
    // LibertySpawn's relaunch: press start, enter world 10 s later; again every 25 s (4 cycles).
    if (now - frontendSince >= 4000 && presses < 4 && (!lastPress || now - lastPress >= 25000)) {
        lastPress = now;
        lastEnter = 0;
        ++presses;
        log("title: press start (cycle %d): %s", presses, pressStart() ? "sent" : "not sent");
    }
    if (lastPress && !lastEnter && now - lastPress >= 10000) {
        lastEnter = now;
        if (personaCount()) enterWorld();
        else log("driver select: persona list not loaded");
    }
}

BOOL WINAPI peekHook(MSG* message, HWND window, UINT first, UINT last, UINT remove) {
    const BOOL result = originalPeek(message, window, first, last, remove);
    if (!menusDone && !inside) {
        inside = true;
        if (!pumpThread) pumpThread = GetCurrentThreadId();
        if (GetCurrentThreadId() == pumpThread) menuStep();
        inside = false;
    }
    return result;
}

const V* __fastcall positionHook(void* self, void*) {
    const V* p = originalPosition(self);
    if (pending && !inside && !busy.exchange(true)) {
        inside = true;
        if (localSim(self)) step(self);
        inside = false;
        busy = false;
    }
    return p;
}

bool exeMatches() {
    static constexpr uint8_t expected[32] = { 0xb2,0xc6,0x16,0xc6,0x41,0xd3,0x51,0x2b,0x4d,0x1e,0xde,0xc9,0xaa,0xb7,0x50,0xf3,
                                              0x30,0x82,0x81,0x75,0x26,0x28,0xab,0xc8,0x45,0x14,0x2d,0x1c,0x43,0x4e,0xc8,0x19 };
    char path[MAX_PATH];
    GetModuleFileNameA(nullptr, path, MAX_PATH);
    HANDLE f = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    BCRYPT_ALG_HANDLE alg{};
    BCRYPT_HASH_HANDLE hash{};
    bool ok = BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0) >= 0 &&
              BCryptCreateHash(alg, &hash, nullptr, 0, nullptr, 0, 0) >= 0;
    static unsigned char buffer[65536];
    unsigned char digest[32]{};
    for (DWORD n; ok;) {
        if (!ReadFile(f, buffer, sizeof buffer, &n, nullptr)) ok = false;
        else if (!n) break;
        else ok = BCryptHashData(hash, buffer, n, 0) >= 0;
    }
    if (ok) ok = BCryptFinishHash(hash, digest, 32, 0) >= 0;
    if (hash) BCryptDestroyHash(hash);
    if (alg) BCryptCloseAlgorithmProvider(alg, 0);
    CloseHandle(f);
    return ok && !std::memcmp(digest, expected, 32);
}
} // namespace

BOOL WINAPI DllMain(HMODULE module, DWORD reason, LPVOID) {
    if (reason != DLL_PROCESS_ATTACH) return TRUE;
    DisableThreadLibraryCalls(module);
    char path[MAX_PATH];
    GetModuleFileNameA(module, path, MAX_PATH);
    char* tail = std::strrchr(path, '\\');
    if (!tail) return TRUE;
    std::strcpy(tail + 1, "editor-fast-test.txt");
    Pose read{};
    if (!takePose(path, read, 600)) return TRUE;   // no fresh request: a normal start, nothing hooked
    // The log keeps every launch (appended, each under its time): a launch that
    // went wrong is still there after the next one.
    std::strcpy(tail + 1, "editor-fast-test.log");
    logFile = CreateFileA(path, FILE_APPEND_DATA, FILE_SHARE_READ, nullptr, OPEN_ALWAYS, 0, nullptr);
    SYSTEMTIME time{};
    GetLocalTime(&time);
    log("--- launch %04u-%02u-%02u %02u:%02u:%02u", time.wYear, time.wMonth, time.wDay, time.wHour, time.wMinute,
        time.wSecond);
    if (read.valid) log("pose (%.2f, %.2f, %.2f) heading %.1f from the editor", read.x, read.y, read.z, read.heading);
    else log("no pose from the editor (no ground under its camera): menus skipped, the car stays where the game puts it");
    if (!exeMatches()) { log("nfsw.exe sha256 differs: disabled"); return TRUE; }
    image = uintptr_t(GetModuleHandleW(nullptr));
    const MH_STATUS init = MH_Initialize();
    if (init != MH_OK && init != MH_ERROR_ALREADY_INITIALIZED) { log("MinHook did not start: disabled"); return TRUE; }
    if (read.valid) {
        pose = read;
        pending = true;
        if (MH_CreateHook((void*)va(0x706340), (void*)positionHook, (void**)&originalPosition) != MH_OK ||
            MH_EnableHook((void*)va(0x706340)) != MH_OK) {
            pending = false;
            log("hook at 0x706340 failed: the car is not moved");
        } else {
            log("waiting for free roam (hook 0x706340)");
        }
    }
    void* peek = (void*)GetProcAddress(GetModuleHandleW(L"user32.dll"), "PeekMessageW");
    if (!peek || MH_CreateHook(peek, (void*)peekHook, (void**)&originalPeek) != MH_OK || MH_EnableHook(peek) != MH_OK)
        log("PeekMessageW hook failed: title and driver select left to the player");
    else
        log("title and driver select by code (PeekMessageW hook)");
    return TRUE;
}
#endif
