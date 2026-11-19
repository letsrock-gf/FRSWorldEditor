// FRS World Loader -- the engine's limits, raised.
//
// The goal is one world joined from regions of several games, so the limits
// the shipped regions never reach are lifted here, one at a time, each read
// out of SPEED2.EXE first (docs/research/region-import.md).
//
// 1. Collision slots. TheTopologyManager (0x88F768) keeps the loaded
//    collision packs ("trees") in a fixed array of 520 slots at +8, found by
//    Slot(n) (0x5D3C10) = (letter - 'A') * 20 + (n % 100, with 20..39 folded
//    onto 0..19). So only letters A..Z and digits 0..19 have a slot of their
//    own: 40..99 alias the next letter's (C41 is E1), and a number past Z
//    writes over the barrier groups at +0x82C. Every reader and writer of the
//    array takes its index from Slot() -- GetIndex 0x591380, the pack loader
//    0x5E7951, the unloader 0x5E8271 / 0x5E827F, and 0x5AA2F1 which reads
//    [index * 4 + 0x88F770] -- so Slot() alone is replaced: it returns the
//    index that lands `manager + 8 + index * 4` on this plugin's own array,
//    one entry for every section number (0..65535), with the same fold
//    (a zone n reads the pack of n + base). The game's array is left unused.

//
// 2. Section letters. A section number is letter * 100 + digits, and eleven
//    places in the streamer (each `cmp al, 'A'; jl no; cmp al, 'T'; jg no`
//    after `n / 100 + 0x40`) take only A..T as positioned / drivable
//    sections; U, V, W are "other", X and Y the libraries, Z the race
//    sections. L4RA leaves four of A..T free. Each test now also takes the
//    letters after Z, '[' to 0x7F -- numbers 2700 to 6399, 37 letters --
//    and U..Z stay what they were. (Past 6399 the byte the game compares
//    turns negative and is refused as before.)
//
// 3. Sections per zone list. The 0x34153 records are walked at a fixed
//    stride of 0xA4 (LoaderVisibleSections's reader 0x5E9C10: the count is
//    size / 164, the next record is +0xA4), which leaves room for 72
//    sections. A record's u16 at +0x0A -- after the zone id, zero in every
//    shipped file -- now gives the stride when it is not zero: the editor
//    writes a larger one where a list needs it (0x2A4: 330 sections, under
//    the 384 the streamer's wanted list holds, sub_5E5110).

//
// 4. The track streaming pool. TrackStreamingPoolSizes (0x79DC54, .rdata) =
//    {60000, 43000, 36000} KiB, read once per region by
//    RegionLoader::FinishedLoading (0x57A61C: one player, split screen,
//    L4RG). [Limits] StreamingPoolKiB sets the first; 120000 by default. The
//    process is not large-address-aware, so a few hundred MiB is the ceiling.
//
// 5. Slot pools. Every fixed-capacity pool the game makes goes through
//    bNewSlotPool(size, count, name, flags) at 0x440DF0 (eModelSlotPool 5000,
//    eStreamingPackSlotPool 64, TexturePackSlotPool 128, g_textureHeaderPool
//    and g_vertexBufferHeaderPool 1024, eMeshRender 4096, TopologyTreeSlotPool
//    48 / 96, WorldModelSlotPool 160...). Its start is hooked: a pool named
//    in [Limits] (e.g. eModelSlotPool=20000) gets that many slots at least.
//    None is raised by default -- some may have a fixed array of the same
//    size elsewhere -- and each pool made is written to the log once, with
//    the count it got.

#include "Loader.h"

#include <cstring>
#include <initializer_list>
#include <string>
#include <unordered_map>

namespace frs {

namespace {

constexpr uintptr_t kSlot = 0x5D3C10;
constexpr uintptr_t kSlotArray = 0x88F770;        // TheTopologyManager 0x88F768 + 8
constexpr uintptr_t kManager = 0x88F768;
constexpr uintptr_t kCollisionBase = 0x800DFC;    // 20 in every shipped region
constexpr size_t kKeys = 65536;

unsigned* g_slots = nullptr;
bool g_slotsOn = false;

// The key of a section number: the number itself, a collision section
// (n % 100 in base..2 base) folded onto its zone.
unsigned SlotKey(unsigned number) {
    const int base = Peek<int>(kCollisionBase);
    const unsigned rem = number % 100;
    if (base > 0 && (int)rem >= base && (int)rem < 2 * base) return number - (unsigned)base;
    return number;
}

// Slot() as the game calls it: stdcall, the number pushed (a short, sign
// extended), the index in eax. ebx, esi, edi and ebp are kept, as the
// original keeps them; ecx and edx are free, as they were.
int __stdcall SlotIndex(int number) {
    const unsigned n = (unsigned)number & 0xFFFF;
    if (!n) return 0;   // as the game: its own slot 0, never used
    const intptr_t entry = (intptr_t)&g_slots[SlotKey(n)];
    return (int)((entry - (intptr_t)kSlotArray) / 4);   // negative when the array is lower: the game's i*4 wraps there
}

bool Expect(uintptr_t at, const unsigned char* bytes, size_t size, const char* what) {
    if (std::memcmp((const void*)at, bytes, size) == 0) return true;
    Log("limits: %s at 0x%06X is not the expected code (another mod?); left as it is", what, (unsigned)at);
    return false;
}

// TopologyManager::Close (0x5E1180, on leaving a region): it disconnects the
// trees and deletes their slot pool whole, without unloading them one by one.
// The game's own array would then hold freed trees too, but a small streaming
// pool makes it unload nearly every section first; with a large pool many
// trees are still in place, and a slot left pointing at one sent the next
// region's lookups into freed memory (the heap check at 0x440039 caught it
// as a race in another region started, 2026-11-15). So the array is cleared
// as the manager closes.
constexpr uintptr_t kTopologyClose = 0x5E1180;     // push ebx; mov ebx, [883D90h] (7 bytes)

void __cdecl ClearSlots() {
    if (g_slots) std::memset(g_slots, 0, kKeys * sizeof(unsigned));
}

__declspec(naked) void TopologyCloseHook() {
    __asm {
        pushad
        call ClearSlots
        popad
        push ebx
        mov ebx, dword ptr ds:[0x883D90]
        push 0x5E1187
        ret
    }
}

// Once per region: every tree the manager holds (its list at +0 / +4: next at
// +0, number u16 at +0x30) is found again through its own slot.
size_t g_lastTrees = (size_t)-1;
void CheckSlots() {
    const uintptr_t head = kManager;
    size_t trees = 0, found = 0;
    unsigned lowest = 0xFFFF, highest = 0;
    for (uintptr_t node = Peek<uintptr_t>(head); node && node != head && trees < 100000; node = Peek<uintptr_t>(node)) {
        const unsigned number = Peek<unsigned short>(node + 0x30);
        ++trees;
        if (g_slots[SlotKey(number)] == (unsigned)node) ++found;
        if (number < lowest) lowest = number;
        if (number > highest) highest = number;
    }
    if (trees == g_lastTrees) return;
    g_lastTrees = trees;
    if (trees)
        Log("limits: collision slots: %zu packs loaded (sections %u..%u), %zu found in their slot%s", trees, lowest,
            highest, found, found == trees ? "" : " -- SOME ARE NOT");
}

// ---- 2. the letters after Z as positioned sections

// The eleven tests, all `3C 41 7C nn 3C 54 7F nn` but one with near jumps.
constexpr uintptr_t kLetterTests[] = { 0x5D5EFA, 0x5D5F39, 0x5D5F4D, 0x5D61C6, 0x5DAB8E, 0x5DF767,
                                       0x5E0064, 0x5E62B9, 0x5E8421, 0x5E8501, 0x5E9C7E };

void Emit(unsigned char*& at, std::initializer_list<unsigned char> bytes) {
    for (unsigned char b : bytes) *at++ = b;
}
void EmitJump(unsigned char*& at, unsigned char op1, unsigned char op2, uintptr_t to) {   // 0F 8x rel32 / E9 rel32
    if (op1) *at++ = op1;
    *at++ = op2;
    const LONG rel = (LONG)(to - ((uintptr_t)at + 4));
    std::memcpy(at, &rel, 4);
    at += 4;
}

bool PatchLetterTest(uintptr_t site) {
    const unsigned char* p = (const unsigned char*)site;
    uintptr_t no = 0, after = 0;
    if (p[0] == 0x3C && p[1] == 0x41 && p[2] == 0x7C && p[4] == 0x3C && p[5] == 0x54 && p[6] == 0x7F &&
        site + 4 + (signed char)p[3] == site + 8 + (signed char)p[7]) {
        no = site + 4 + (signed char)p[3];
        after = site + 8;
    } else if (p[0] == 0x3C && p[1] == 0x41 && p[2] == 0x0F && p[3] == 0x8C && p[8] == 0x3C && p[9] == 0x54 &&
               p[10] == 0x0F && p[11] == 0x8F) {
        LONG a, b;
        std::memcpy(&a, p + 4, 4);
        std::memcpy(&b, p + 12, 4);
        if (site + 8 + a != site + 16 + b) return false;
        no = site + 8 + a;
        after = site + 16;
    } else {
        return false;
    }
    unsigned char* stub = NewCode(32);
    if (!stub) return false;
    unsigned char* at = stub;
    Emit(at, { 0x3C, 0x41 });            // cmp al, 'A'
    EmitJump(at, 0x0F, 0x8C, no);        // jl no
    Emit(at, { 0x3C, 0x54 });            // cmp al, 'T'
    EmitJump(at, 0x0F, 0x8E, after);     // jle positioned
    Emit(at, { 0x3C, 0x5B });            // cmp al, '['
    EmitJump(at, 0x0F, 0x8C, no);        // jl no: U..Z as before
    EmitJump(at, 0, 0xE9, after);        // '['..0x7F: positioned
    PatchJump((unsigned char*)site, after - site, stub);
    return true;
}

// ---- 3. the zone lists' stride

constexpr uintptr_t kListCount = 0x5E9CCE;     // mov eax, 0C7CE0C7Dh; mul [ebx+4]; mov esi, edx; shr esi, 7
constexpr uintptr_t kListCountDone = 0x5E9CDB;
constexpr uintptr_t kListNext = 0x5E9D4F;      // add ecx, 0A4h
unsigned g_listStride = 0xA4;

// ebx = the 0x34153 chunk; out: esi = the record count, g_listStride set.
__declspec(naked) void ListCount() {
    __asm {
        push ecx
        movzx ecx, word ptr [ebx + 0x12]      // the first record's u16 at +0x0A (chunk data at ebx + 8)
        cmp ecx, 0x20
        jae stride_set
        mov ecx, 0xA4
    stride_set:
        mov dword ptr [g_listStride], ecx
        mov eax, dword ptr [ebx + 4]
        xor edx, edx
        div ecx
        mov esi, eax
        pop ecx
        push 0x5E9CDB                         // kListCountDone
        ret
    }
}

// ---- 4. the streaming pool

constexpr uintptr_t kPoolSizes = 0x79DC54;

// ---- 5. slot pools

constexpr uintptr_t kNewSlotPool = 0x440DF0;      // mov eax, [esp+10h]; mov ecx, [esp+0Ch] (8 bytes)
std::unordered_map<std::string, int> g_seenPools;  // name -> count given (logged once)

// The arguments as the caller pushed them: size, count, name, flags.
void __cdecl AdjustPool(int* args) {
    const char* name = (const char*)args[2];
    if (::IsBadStringPtrA(name, 64)) return;
    std::string key(name);
    std::wstring wide(key.begin(), key.end());
    for (wchar_t& c : wide) if (c == L' ' || c == L':') c = L'_';
    const int wanted = Setting(L"Limits", wide.c_str(), 0);
    const int shipped = args[1];
    if (wanted > shipped) args[1] = wanted;
    if (g_seenPools.emplace(key, args[1]).second)
        Log("limits: pool %s: %d slots of %d bytes%s", name, args[1], args[0],
            args[1] != shipped ? (" (the game's " + std::to_string(shipped) + ")").c_str() : "");
}

__declspec(naked) void NewSlotPoolHook() {
    __asm {
        pushad
        lea eax, [esp + 0x24]           // past pushad (0x20) and the return address
        push eax
        call AdjustPool
        add esp, 4
        popad
        mov eax, dword ptr [esp + 0x10]
        mov ecx, dword ptr [esp + 0x0C]
        push 0x440DF8
        ret
    }
}

} // namespace

void InstallLimits() {
    // 4. The streaming pool.
    {
        unsigned sizes[3];
        std::memcpy(sizes, (const void*)kPoolSizes, sizeof(sizes));
        // Large-address-aware (the 4 GB flag on SPEED2.EXE): up to 4 GiB on a
        // 64-bit Windows instead of 2, so twice the default pool.
        const auto* dos = (const IMAGE_DOS_HEADER*)::GetModuleHandleW(nullptr);
        const auto* nt = (const IMAGE_NT_HEADERS*)((const unsigned char*)dos + dos->e_lfanew);
        const bool large = (nt->FileHeader.Characteristics & IMAGE_FILE_LARGE_ADDRESS_AWARE) != 0;
        Log("limits: SPEED2.EXE is %s", large ? "large-address-aware (the 4 GB flag): up to 4 GiB of memory"
                                               : "not large-address-aware: 2 GiB of memory at most");
        if (sizes[0] == 60000 && sizes[1] == 43000 && sizes[2] == 36000) {
            const int wanted = Setting(L"Limits", L"StreamingPoolKiB", large ? 240000 : 120000);
            if (wanted > 0 && wanted != 60000) {
                const unsigned value = (unsigned)wanted;
                Patch((unsigned char*)kPoolSizes, &value, 4);
            }
            Log("limits: track streaming pool: %d KiB (the game's 60000)", wanted > 0 ? wanted : 60000);
        } else {
            Log("limits: the streaming pool sizes are not the shipped ones (%u, %u, %u); left as they are", sizes[0],
                sizes[1], sizes[2]);
        }
    }
    // 5. Slot pools.
    {
        static const unsigned char start[] = { 0x8B, 0x44, 0x24, 0x10, 0x8B, 0x4C, 0x24, 0x0C };
        if (!Setting(L"Limits", L"SlotPoolHook", 1)) Log("limits: slot pools: off ([Limits] SlotPoolHook=0)");
        else if (Expect(kNewSlotPool, start, sizeof(start), "bNewSlotPool")) {
            PatchJump((unsigned char*)kNewSlotPool, sizeof(start), (const void*)&NewSlotPoolHook);
            Log("limits: slot pools: [Limits] in FRSWorldLoader.ini raises any of them by name");
        }
    }

    static const unsigned char slot[] = { 0x8B, 0x4C, 0x24, 0x04, 0x66, 0x85, 0xC9, 0x75, 0x05 };
    if (!Setting(L"Limits", L"CollisionSlots", 1)) Log("limits: collision slots: off ([Limits] CollisionSlots=0)");
    else if (Expect(kSlot, slot, sizeof(slot), "the collision slot function")) {
        g_slots = (unsigned*)::VirtualAlloc(nullptr, kKeys * sizeof(unsigned), MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
        static const unsigned char close[] = { 0x53, 0x8B, 0x1D, 0x90, 0x3D, 0x88, 0x00 };
        if (g_slots && Expect(kTopologyClose, close, sizeof(close), "TopologyManager::Close")) {
            PatchJump((unsigned char*)kTopologyClose, sizeof(close), (const void*)&TopologyCloseHook);
            PatchJump((unsigned char*)kSlot, 5, (const void*)&SlotIndex);
            g_slotsOn = true;
            Log("limits: collision slots: %zu (one per section number) instead of 520, cleared as a region closes", kKeys);
        } else {
            Log("limits: no memory for the collision slots; left as they are");
        }
    }

    int letters = 0;
    if (Setting(L"Limits", L"SectionLetters", 1))
    for (uintptr_t site : kLetterTests) {
        if (PatchLetterTest(site)) ++letters;
        else Log("limits: the section letter test at 0x%06X is not the expected code; left as it is", (unsigned)site);
    }
    Log("limits: section letters: %d of %zu tests take the letters after Z (numbers 2700..6399) too", letters,
        sizeof(kLetterTests) / sizeof(kLetterTests[0]));

    static const unsigned char count[] = { 0xB8, 0x7D, 0x0C, 0xCE, 0xC7, 0xF7, 0x63, 0x04, 0x8B, 0xF2, 0xC1, 0xEE, 0x07 };
    static const unsigned char next[] = { 0x81, 0xC1, 0xA4, 0x00, 0x00, 0x00 };
    if (!Setting(L"Limits", L"ZoneListStride", 1)) Log("limits: zone lists: off ([Limits] ZoneListStride=0)");
    else if (Expect(kListCount, count, sizeof(count), "the zone list count") &&
        Expect(kListNext, next, sizeof(next), "the zone list step")) {
        PatchJump((unsigned char*)kListCount, sizeof(count), (const void*)&ListCount);
        unsigned char step[6] = { 0x03, 0x0D };   // add ecx, [g_listStride]
        const unsigned address = (unsigned)(uintptr_t)&g_listStride;
        std::memcpy(step + 2, &address, 4);
        Patch((unsigned char*)kListNext, step, sizeof(step));
        Log("limits: zone lists: the stride is read from each file (0xA4 = 72 sections when it gives none)");
    }
}

void LimitsFrame() {
    // Only in gameplay: between regions the manager's list still names the
    // trees its closing freed.
    if (g_slotsOn && InGameplay()) CheckSlots();
}

} // namespace frs
