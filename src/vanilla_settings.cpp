// Vanilla Settings: runtime toggles for Elden Ring's own behaviour.
//
// Patch sites and replacement bytes follow techiew's EldenRingMods (MIT,
// https://github.com/techiew/EldenRingMods), which credits uberhalit's
// EldenRingFpsUnlockAndMore for the frame-rate sites. Every site was
// re-verified against eldenring.exe 2.7.1.0 (one unique match, expected bytes
// present). A site that is missing or ambiguous in another build is disabled
// instead of being patched.
//
// Live toggling writes code that other threads may be executing, so writes
// are atomic per 16-byte block (cmpxchg16b). A patch that straddles a block
// boundary is written in three steps behind a short jump over the region.

#include "vanilla_settings.hpp"

#include "game_memory.hpp"
#include "hotkeys.hpp"
#include "live_text.hpp"
#include "perf_log.hpp"
#include "spirit_ashes.hpp"
#include "traditional_chinese.hpp"

#include <Windows.h>
#include <intrin.h>

#pragma comment(lib, "synchronization.lib")  // WaitOnAddress

#include <algorithm>
#include <atomic>
#include <cmath>
#include <map>
#include <set>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

namespace vanilla {
namespace {

LogFn g_log{};
std::wstring g_config_path{};
// The log file is on ([Logging] EnableLog or [Diagnostics] PerfLog): the
// per-event diagnostic lines (downed sequences, animation speeds) and the reads
// they need are skipped otherwise.
bool g_diagnostics{};
const erui::Registration* g_registration{};
SRWLOCK g_patch_lock = SRWLOCK_INIT;
HANDLE g_save_event{};

void log(const wchar_t* message) noexcept {
    if (g_log) g_log(message);
}

// Bumped by every option change. Workers whose option is off sleep on it
// instead of polling, and wake at once when the player turns one on.
volatile LONG g_option_generation{};
constexpr DWORD kIdleWaitMs = 500;

void wake_workers() noexcept {
    InterlockedIncrement(&g_option_generation);
    WakeByAddressAll(const_cast<LONG*>(&g_option_generation));
}

void idle_wait() noexcept {
    LONG seen = g_option_generation;
    WaitOnAddress(&g_option_generation, &seen, sizeof(seen), kIdleWaitMs);
}

void idle_wait(perf::Loop& loop) noexcept {
    loop.pause();
    idle_wait();
    loop.resume();
}

// Takes a lock shared with a hook on the game thread; PerfLog records the wait.
void lock_timed(SRWLOCK* lock, perf::Timer* wait) noexcept {
    const std::uint64_t start = wait ? perf::now() : 0;
    AcquireSRWLockExclusive(lock);
    if (wait) perf::record(wait, perf::now() - start);
}

// ---------------------------------------------------------------------------
// Pattern scanning

std::vector<int> parse_pattern(const char* text) {
    std::vector<int> bytes;
    for (const char* p = text; *p;) {
        while (*p == ' ') ++p;
        if (!*p) break;
        if (*p == '?') {
            bytes.push_back(-1);
            while (*p == '?') ++p;
        } else {
            bytes.push_back(static_cast<int>(std::strtoul(p, nullptr, 16)));
            while (*p && *p != ' ') ++p;
        }
    }
    return bytes;
}

bool matches(const std::uint8_t* at, const std::vector<int>& pattern) noexcept {
    for (std::size_t i = 0; i < pattern.size(); ++i)
        if (pattern[i] >= 0 && at[i] != pattern[i]) return false;
    return true;
}

// Returns the single match in the game's executable sections, or nullptr when
// the pattern is absent or ambiguous.
std::uint8_t* find_unique(const std::vector<int>& pattern) noexcept {
    auto* base = reinterpret_cast<std::uint8_t*>(game::image());
    if (!base || pattern.empty() || pattern[0] < 0) return nullptr;
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
    const auto* section = IMAGE_FIRST_SECTION(nt);
    std::uint8_t* found{};
    for (unsigned i = 0; i < nt->FileHeader.NumberOfSections; ++i, ++section) {
        if (!(section->Characteristics & IMAGE_SCN_MEM_EXECUTE)) continue;
        std::uint8_t* begin = base + section->VirtualAddress;
        const std::size_t size = section->Misc.VirtualSize;
        if (size < pattern.size()) continue;
        std::uint8_t* const end = begin + size - pattern.size();
        const auto first = static_cast<std::uint8_t>(pattern[0]);
        for (std::uint8_t* p = begin; p <= end; ++p) {
            p = static_cast<std::uint8_t*>(std::memchr(p, first, static_cast<std::size_t>(end - p) + 1));
            if (!p) break;
            if (!matches(p, pattern)) continue;
            if (found) return nullptr;
            found = p;
        }
    }
    return found;
}

// ---------------------------------------------------------------------------
// Atomic code writes

bool cas_block(std::uint8_t* address, const std::uint8_t* bytes, std::size_t count) noexcept {
    const auto block_address = reinterpret_cast<std::uintptr_t>(address) & ~std::uintptr_t{15};
    const std::size_t offset = reinterpret_cast<std::uintptr_t>(address) - block_address;
    if (offset + count > 16) return false;
    auto* block = reinterpret_cast<volatile __int64*>(block_address);
    alignas(16) __int64 expected[2]{block[0], block[1]};
    for (;;) {
        alignas(16) __int64 desired[2]{expected[0], expected[1]};
        std::memcpy(reinterpret_cast<std::uint8_t*>(desired) + offset, bytes, count);
        if (_InterlockedCompareExchange128(block, desired[1], desired[0], expected)) return true;
    }
}

bool write_code(std::uint8_t* address, const std::uint8_t* bytes, std::size_t count) noexcept {
    if (!address || !count) return false;
    DWORD old_protect{};
    if (!VirtualProtect(address, count, PAGE_EXECUTE_READWRITE, &old_protect)) return false;

    bool ok{};
    const std::size_t offset = reinterpret_cast<std::uintptr_t>(address) & 15;
    if (offset + count <= 16) {
        ok = cas_block(address, bytes, count);
    } else if (count >= 3 && count <= 129 && offset <= 14) {
        // Divert execution past the region, rewrite its tail, then publish
        // the new first two bytes in one atomic store.
        const std::uint8_t skip[2]{0xEB, static_cast<std::uint8_t>(count - 2)};
        ok = cas_block(address, skip, 2);
        if (ok) {
            FlushInstructionCache(GetCurrentProcess(), address, count);
            std::memcpy(address + 2, bytes + 2, count - 2);
            FlushInstructionCache(GetCurrentProcess(), address, count);
            ok = cas_block(address, bytes, 2);
        }
    }

    DWORD ignored{};
    VirtualProtect(address, count, old_protect, &ignored);
    FlushInstructionCache(GetCurrentProcess(), address, count);
    return ok;
}

// ---------------------------------------------------------------------------
// Patch sites

struct Site {
    const wchar_t* name;
    const char* aob;
    std::ptrdiff_t offset;
    const char* expected;
    std::vector<std::uint8_t> replacement;
    std::uint8_t* address{};
    std::vector<std::uint8_t> original{};
    bool ready{};
};

Site g_camera_rotate{L"camera auto-rotate",
    "0f 29 ? ? ? ? ? ? 0f 28 ? ? 8b ? e8 ? ? ? ? ? 0f b6 ? ? ? ? 0f 28 ? ? 8b ? e8 ? ? ? ? ? 8b ? ? 0f 28 ? ? 8b ? e8 ? ? ? ? ? 8d ? ? ? 8b ? ? 8d",
    0, "0f 29 ? ? ? ? ?", {0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90}};
Site g_camera_reset{L"camera reset",
    "80 ? ? ? ? ? 00 74 ? ? 8b ? e8 ? ? ? ? eb ? 0f 28 ? ? ? ? ? ? 8d",
    7, "74", {0xEB}};
Site g_vignette{L"vignette",
    "f3 0f 10 ? 50 f3 0f 59 ? ? ? ? ? e8 ? ? ? ? f3 ? 0f 5c ? f3 ? 0f 59 ? ? 8d ? ? a0 00 00 00",
    0x17, "f3 ? 0f 59 ?", {0xF3, 0x0F, 0x5C, 0xC0, 0x90}};
Site g_chromatic{L"chromatic aberration",
    "0f 11 ? 60 ? 8d ? 80 00 00 00 0f 10 ? a0 00 00 00 0f 11 ? f0 ? 8d ? b0 00 00 00 0f 10 ? 0f 11 ? 0f 10 ? 10",
    0x2F, "0f 11 ? ?", {0x66, 0x0F, 0xEF, 0xC9}};
Site g_animation{L"animation distance",
    "c7 ? ? ? 01 00 00 00 f3 ? 0f 10 ? ? ? f3 ? 0f 10 ? ? ? f3 0f 59 ? ? ? ? ? ? 0f 28 ? f3 ? 0f 5c ? ? 58",
    0x48, "f3 ? 0f 5e ? ? ?", {0x0F, 0x57, 0xC9, 0x90, 0x90, 0x90, 0x90}};
Site g_rune_loss{L"rune loss",
    "b0 01 ? 8b ? e8 ? ? ? ? ? 8b ? ? ? 32 c0 ? 83 ? 28 c3",
    5, "e8", {0x90, 0x90, 0x90, 0x90, 0x90}};
Site g_pause{L"pause",
    "0f 84 ? ? ? ? c6 ? ? ? ? ? 00 ? 8d ? ? ? ? ? ? 89 ? ? 89 ? ? ? 8b ? ? ? ? ? ? 85 ? 75",
    1, "84", {0x85}};
Site g_frametime{L"frame time",
    "c7 ? ? 89 88 88 3c eb ? 89 ? 18 eb ? 89 ? 18 c7",
    3, "89 88 88 3c", {0x89, 0x88, 0x88, 0x3C}};
Site g_fullscreen_cap{L"fullscreen 60 FPS cap",
    "c7 ? ef 3c 00 00 00 c7 ? f3 01 00 00 00",
    0, "c7 ? ef 3c 00 00 00", {0xC7, 0x45, 0xEF, 0x00, 0x00, 0x00, 0x00}};
Site g_ultrawide{L"ultrawide",
    "48 c7 45 b8 fe ff ff ff 48 89 58 10 48 89 70 18 48 89 78 20 0f 29 70 c8 48 8b",
    0x94, "74", {0xEB}};
// WorldChrMan's respawn request: "if (state == 0) state = 1" on the field at
// +0x1E524 (2.7.1.0), the only writer of 1. Every world reset (resting or
// passing time at a grace, dying, the warp/fade reset) reaches it through
// eldenring.exe+0x5F4410; the WorldChrMan_Respawn task then revives every
// enemy without an event flag. A ret skips the request and nothing else of
// the reset: estus, HP, objects and event scripts reset as usual.
Site g_enemy_respawn{L"enemy respawn",
    "83 b9 ? ? ? ? 00 75 0a c7 81 ? ? ? ? 01 00 00 00 c3",
    0, "83 b9", {0xC3}};

Site* const kSites[]{&g_camera_rotate, &g_camera_reset, &g_vignette, &g_chromatic,
    &g_animation, &g_rune_loss, &g_pause, &g_frametime, &g_fullscreen_cap, &g_ultrawide, &g_enemy_respawn};

void resolve(Site& site) noexcept {
    try {
        std::uint8_t* hit = find_unique(parse_pattern(site.aob));
        wchar_t message[160]{};
        if (!hit) {
            swprintf_s(message, L"Vanilla site '%ls' not found (or ambiguous); option disabled.", site.name);
            log(message);
            return;
        }
        std::uint8_t* address = hit + site.offset;
        if (!matches(address, parse_pattern(site.expected))) {
            swprintf_s(message, L"Vanilla site '%ls' has unexpected bytes; option disabled.", site.name);
            log(message);
            return;
        }
        site.address = address;
        site.original.assign(address, address + site.replacement.size());
        site.ready = true;
        swprintf_s(message, L"Vanilla site '%ls' found at eldenring.exe+0x%llX.", site.name,
            static_cast<unsigned long long>(address - reinterpret_cast<std::uint8_t*>(game::image())));
        log(message);
    } catch (...) {
        site.ready = false;
    }
}

bool set_bytes(Site& site, const std::vector<std::uint8_t>& bytes) noexcept {
    if (!site.ready || bytes.size() != site.original.size()) return false;
    if (std::memcmp(site.address, bytes.data(), bytes.size()) == 0) return true;
    return write_code(site.address, bytes.data(), bytes.size());
}

bool set_site(Site& site, bool patched) noexcept {
    return set_bytes(site, patched ? site.replacement : site.original);
}

// ---------------------------------------------------------------------------
// Field-of-view hook. The game computes the vertical FOV in degrees, then
// converts it to radians; the trampoline scales the computed value so the
// game's own zoom transitions (aiming, mounts, cutscenes) stay intact.

struct FovData {
    volatile std::uint32_t enabled;
    volatile float factor;
};

FovData* g_fov{};
std::uint8_t* g_fov_site{};

constexpr std::uint8_t kFovExpected[]{0x48, 0x8D, 0x4C, 0x24, 0x20, 0x44, 0x0F, 0x28, 0xC8, 0xE8};

// Only locates the site. The code is left untouched until the option is
// first enabled, so other mods that patch the same FOV code (for example
// Solid Uncapper) still find the original bytes.
void resolve_fov_site() noexcept {
    const auto pattern = parse_pattern(
        "8d ? ? ? ? 0f 28 ? e8 ? ? ? ? 80 ? ? ? ? ? ? ? 0f 28 ? f3 ? 0f 10 ? ? ? ? ? ? 0f 57 ? f3 ? 0f 59");
    std::uint8_t* hit = find_unique(pattern);
    if (!hit) {
        log(L"Vanilla site 'field of view' not found (or ambiguous); option disabled.");
        return;
    }
    std::uint8_t* site = hit - 1;
    if (std::memcmp(site, kFovExpected, sizeof(kFovExpected)) != 0) {
        log(L"Vanilla site 'field of view' has unexpected bytes; option disabled.");
        return;
    }
    g_fov_site = site;
}

void install_fov_hook() noexcept {
    if (g_fov || !g_fov_site) return;
    std::uint8_t* site = g_fov_site;
    if (std::memcmp(site, kFovExpected, sizeof(kFovExpected)) != 0) {
        g_fov_site = nullptr;
        log(L"Field-of-view code was changed by another mod; NativeTweaks FOV disabled.");
        return;
    }
    std::int32_t relative{};
    std::memcpy(&relative, site + 10, sizeof(relative));
    const std::uint64_t target = reinterpret_cast<std::uint64_t>(site + 14) + relative;
    const std::uint64_t resume = reinterpret_cast<std::uint64_t>(site + 14);

    auto* code = static_cast<std::uint8_t*>(VirtualAlloc(
        nullptr, 4096, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
    if (!code) return;
    g_fov = reinterpret_cast<FovData*>(code + 64);
    g_fov->enabled = 0;
    g_fov->factor = 1.0f;

    std::uint8_t* p = code;
    const auto put = [&](std::initializer_list<std::uint8_t> bytes) {
        for (auto b : bytes) *p++ = b;
    };
    const auto put_u64 = [&](std::uint64_t v) { std::memcpy(p, &v, 8); p += 8; };
    const auto put_rel = [&](const volatile void* data, std::size_t instruction_tail) {
        const auto next = reinterpret_cast<std::intptr_t>(p) + 4 + instruction_tail;
        const auto disp = static_cast<std::int32_t>(reinterpret_cast<std::intptr_t>(data) - next);
        std::memcpy(p, &disp, 4);
        p += 4;
    };
    put({0x48, 0x8D, 0x4C, 0x24, 0x20});              // lea rcx,[rsp+20h]  (original)
    put({0x44, 0x0F, 0x28, 0xC8});                    // movaps xmm9,xmm0   (original)
    put({0xFF, 0x15, 0x02, 0x00, 0x00, 0x00});        // call [rip+2]
    put({0xEB, 0x08});                                // jmp over target
    put_u64(target);
    put({0x80, 0x3D}); put_rel(&g_fov->enabled, 1); put({0x00});  // cmp byte [enabled],0
    put({0x74, 0x08});                                // je resume
    put({0xF3, 0x0F, 0x59, 0x05}); put_rel(&g_fov->factor, 0);    // mulss xmm0,[factor]
    put({0xFF, 0x25, 0x00, 0x00, 0x00, 0x00});        // jmp [rip+0]
    put_u64(resume);
    FlushInstructionCache(GetCurrentProcess(), code, 64);

    std::uint8_t jump[14]{0xFF, 0x25, 0x00, 0x00, 0x00, 0x00};
    const auto trampoline = reinterpret_cast<std::uint64_t>(code);
    std::memcpy(jump + 6, &trampoline, 8);
    if (!write_code(site, jump, sizeof(jump))) {
        g_fov = nullptr;
        log(L"Vanilla field-of-view hook could not be written; option disabled.");
        return;
    }
    log(L"Vanilla field-of-view hook installed.");
}

// Fall damage hook. eldenring.exe+0x44E700 (2.7.1.0) applies landing damage
// (max HP x ratio, or all HP for a lethal fall) to the character owning the
// fall module in rcx. Found with the r61 HP write trace. The trampoline
// returns early for the player's and Torrent's fall modules only (each 0 when
// its option is off), so enemies still take it.
// r85: a mounted landing calls it for the player's module and then for
// Torrent's (r84 log), so with Torrent protection on, the call that comes
// right after the player's is blocked as well.
struct FallData {
    volatile std::uint32_t player_on;
    volatile std::uint32_t torrent_on;
    volatile std::uint32_t blocked;
    volatile std::uint32_t pad;
    volatile std::uint64_t player_fall_module;
    volatile std::uint64_t last_caller;  // rcx of the last two calls
    volatile std::uint64_t prev_caller;
};

FallData* g_fall{};

void install_fall_hook() noexcept {
    std::uint8_t* site = find_unique(parse_pattern(
        "48 8b c4 55 53 56 57 41 54 41 56 41 57 48 8d 68 b1 48 81 ec c0 00 00 00 48 c7 45 b7 fe ff ff ff "
        "0f 29 70 b8 0f 29 78 a8 44 0f 29 40 98 44 0f 29 48 88 48 8b 05 ? ? ? ? 48 33 c4 48 89 45 cf "
        "41 0f b6 f9 44 0f 28 ca 44 0f 28 c1"));
    if (!site) {
        log(L"Vanilla site 'fall damage' not found (or ambiguous); hook disabled.");
        return;
    }
    constexpr std::size_t kStolen = 17;  // mov rax,rsp .. lea rbp,[rax-4Fh]
    auto* code = static_cast<std::uint8_t*>(VirtualAlloc(
        nullptr, 4096, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
    if (!code) return;
    g_fall = reinterpret_cast<FallData*>(code + 128);
    g_fall->player_on = 0;
    g_fall->torrent_on = 0;
    g_fall->blocked = 0;
    g_fall->player_fall_module = 0;
    g_fall->last_caller = 0;
    g_fall->prev_caller = 0;

    std::uint8_t* p = code;
    const auto put = [&](std::initializer_list<std::uint8_t> bytes) {
        for (auto b : bytes) *p++ = b;
    };
    const auto put_rel = [&](const volatile void* data, std::size_t instruction_tail) {
        const auto next = reinterpret_cast<std::intptr_t>(p) + 4 + instruction_tail;
        const auto disp = static_cast<std::int32_t>(reinterpret_cast<std::intptr_t>(data) - next);
        std::memcpy(p, &disp, 4);
        p += 4;
    };
    put({0x48, 0x8B, 0x05}); put_rel(&g_fall->last_caller, 0);             // mov rax,[last]
    put({0x48, 0x89, 0x05}); put_rel(&g_fall->prev_caller, 0);             // mov [prev],rax
    put({0x48, 0x89, 0x0D}); put_rel(&g_fall->last_caller, 0);             // mov [last],rcx
    put({0x48, 0x3B, 0x0D}); put_rel(&g_fall->player_fall_module, 0);      // cmp rcx,[player]
    put({0x75, 0x0B});                                                      // jne not_player
    put({0x83, 0x3D}); put_rel(&g_fall->player_on, 1); put({0x00});         // cmp dword [player_on],0
    put({0x75, 0x14});                                                      // jne block
    put({0xEB, 0x1A});                                                      // jmp original
    put({0x83, 0x3D}); put_rel(&g_fall->torrent_on, 1); put({0x00});        // not_player: cmp [torrent_on],0
    put({0x74, 0x11});                                                      // je original
    put({0x48, 0x3B, 0x05}); put_rel(&g_fall->player_fall_module, 0);      // cmp rax,[player] (previous call)
    put({0x75, 0x08});                                                      // jne original
    put({0xF0, 0xFF, 0x05}); put_rel(&g_fall->blocked, 0);                 // lock inc [blocked]
    put({0xC3});                                                            // ret
    std::memcpy(p, site, kStolen); p += kStolen;                            // original prologue
    put({0xFF, 0x25, 0x00, 0x00, 0x00, 0x00});                              // jmp [rip+0]
    const std::uint64_t resume = reinterpret_cast<std::uint64_t>(site + kStolen);
    std::memcpy(p, &resume, 8);
    FlushInstructionCache(GetCurrentProcess(), code, 128);

    std::uint8_t jump[kStolen]{0xFF, 0x25, 0x00, 0x00, 0x00, 0x00, 0, 0, 0, 0, 0, 0, 0, 0, 0x90, 0x90, 0x90};
    const auto trampoline = reinterpret_cast<std::uint64_t>(code);
    std::memcpy(jump + 6, &trampoline, 8);
    if (!write_code(site, jump, sizeof(jump))) {
        g_fall = nullptr;
        log(L"Vanilla fall damage hook could not be written; hook disabled.");
        return;
    }
    wchar_t message[128]{};
    swprintf_s(message, L"Vanilla fall damage hook installed at eldenring.exe+0x%llX.",
        static_cast<unsigned long long>(site - reinterpret_cast<std::uint8_t*>(game::image())));
    log(message);
}

// Status immunity hook. Before adding buildup the resist module
// (eldenring.exe+0x43E0DF in 2.7.2.0, one path per status 0-6) asks
// eldenring.exe+0x4FAE40 (0x4FAD60 in 2.7.0.0/2.7.1.x) whether a SpEffect of
// the character (rcx = ChrIns+0x178) makes it immune to that status, and adds
// nothing when it returns 1. Those eight calls are its only callers. The
// trampoline returns 1 for the player's SpEffect object while the option is
// on. Buildup counts down from the maximum and the status triggers inside the
// same call, so holding the value from a worker missed any hit that took it
// to zero at once (r133 report).
struct StatusImmunityData {
    volatile std::uint32_t on;
    volatile std::uint32_t blocked;
    volatile std::uint64_t player_effects;
};

StatusImmunityData* g_status_immunity{};

constexpr const char* kStatusImmunityPattern =
    "48 89 5c 24 08 57 48 83 ec 20 48 8b 59 08 8b fa 48 85 db 74 ? f7 43 60 03 00 0c 80 75 ? 8b d7 48 8b cb e8";

void install_status_immunity_hook() noexcept {
    std::uint8_t* site = find_unique(parse_pattern(kStatusImmunityPattern));
    if (!site) {
        log(L"Vanilla site 'status immunity' not found (or ambiguous); using the buildup hold instead.");
        return;
    }
    constexpr std::size_t kStolen = 14;  // mov [rsp+8],rbx; push rdi; sub rsp,20h; mov rbx,[rcx+8]
    auto* code = static_cast<std::uint8_t*>(VirtualAlloc(
        nullptr, 4096, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
    if (!code) return;
    g_status_immunity = reinterpret_cast<StatusImmunityData*>(code + 128);
    g_status_immunity->on = 0;
    g_status_immunity->blocked = 0;
    g_status_immunity->player_effects = 0;

    std::uint8_t* p = code;
    const auto put = [&](std::initializer_list<std::uint8_t> bytes) {
        for (auto b : bytes) *p++ = b;
    };
    const auto put_rel = [&](const volatile void* data, std::size_t instruction_tail) {
        const auto next = reinterpret_cast<std::intptr_t>(p) + 4 + instruction_tail;
        const auto disp = static_cast<std::int32_t>(reinterpret_cast<std::intptr_t>(data) - next);
        std::memcpy(p, &disp, 4);
        p += 4;
    };
    put({0x83, 0x3D}); put_rel(&g_status_immunity->on, 1); put({0x00});           // cmp dword [on],0
    put({0x74, 0x13});                                                             // je original
    put({0x48, 0x3B, 0x0D}); put_rel(&g_status_immunity->player_effects, 0);      // cmp rcx,[player]
    put({0x75, 0x0A});                                                             // jne original
    put({0xB0, 0x01});                                                             // mov al,1 (immune)
    put({0xF0, 0xFF, 0x05}); put_rel(&g_status_immunity->blocked, 0);             // lock inc [blocked]
    put({0xC3});                                                                   // ret
    std::memcpy(p, site, kStolen); p += kStolen;                                   // original prologue
    put({0xFF, 0x25, 0x00, 0x00, 0x00, 0x00});                                     // jmp [rip+0]
    const std::uint64_t resume = reinterpret_cast<std::uint64_t>(site + kStolen);
    std::memcpy(p, &resume, 8);
    FlushInstructionCache(GetCurrentProcess(), code, 128);

    std::uint8_t jump[kStolen]{0xFF, 0x25, 0x00, 0x00, 0x00, 0x00};
    const auto trampoline = reinterpret_cast<std::uint64_t>(code);
    std::memcpy(jump + 6, &trampoline, 8);
    if (!write_code(site, jump, sizeof(jump))) {
        g_status_immunity = nullptr;
        log(L"Vanilla status immunity hook could not be written; using the buildup hold instead.");
        return;
    }
    wchar_t message[128]{};
    swprintf_s(message, L"Vanilla status immunity hook installed at eldenring.exe+0x%llX.",
        static_cast<unsigned long long>(site - reinterpret_cast<std::uint8_t*>(game::image())));
    log(message);
}

// No lock-on range hooks. The player's lock-on update (0x717560 in 2.7.2.0,
// 0x7170B0 in 2.7.0.0/2.7.1.x) and target search (0x716F30) compute a target's
// reach as its lock distance plus the lock manager's reach, then compare
// squared distances. Each hook runs a few whole instructions of that sum and
// then, while the option is on, zeroes the register that holds the reach:
// nothing can be locked and a held lock drops. Same bytes in 2.7.0.0, 2.7.1.0,
// 2.7.1.1 JP and 2.7.2.0.
struct LockRangeData {
    volatile std::uint32_t on;
};

LockRangeData* g_lock_range{};

struct LockRangeSite {
    const char* name;
    const char* pattern;
    std::size_t offset;    // first stolen instruction
    std::size_t stolen;    // whole instructions, at least 14 bytes
    std::uint8_t zero[4];  // xorps reg, reg
    std::size_t zero_size;
};

constexpr LockRangeSite kLockRangeSites[]{
    // movzx eax,[r14+74h]; movd xmm9,eax; cvtdq2ps xmm9,xmm9; addss xmm9,[rsi+2924h];
    // stolen: and byte [rbx+90h],6Fh; xor dil,dil; movups xmm15,[r14]
    {"acquire", "41 0f b6 46 74 66 44 0f 6e c8 45 0f 5b c9 f3 44 0f 58 8e 24 29 00 00 "
        "80 a3 90 00 00 00 6f 40 32 ff 45 0f 10 3e", 0x17, 14, {0x45, 0x0F, 0x57, 0xC9}, 4},
    // movups xmm4,[rsi+2930h]; addss xmm4,[rsi+2924h]; ...; stolen: addss xmm4,xmm0;
    // mulss xmm4,xmm4; movups xmm3,[rdi]; movaps xmm0,xmm7 (xmm4 = reach squared)
    {"keep", "0f 10 a6 30 29 00 00 f3 0f 58 a6 24 29 00 00 0f b6 47 74 66 0f 6e c0 0f 5b c0 "
        "f3 0f 58 e0 f3 0f 59 e4 0f 10 1f 0f 28 c7", 0x1A, 14, {0x0F, 0x57, 0xE4}, 3},
    // stolen: cvtdq2ps xmm12,xmm12; addss xmm0,xmm2; addss xmm12,[rdi+2924h]
    {"search", "45 0f 5b e4 f3 0f 58 c2 f3 44 0f 58 a7 24 29 00 00 e8", 0x0, 17,
        {0x45, 0x0F, 0x57, 0xE4}, 4},
};

void install_lock_range_hooks() noexcept {
    std::uint8_t* sites[std::size(kLockRangeSites)]{};
    for (std::size_t i = 0; i < std::size(kLockRangeSites); ++i) {
        std::uint8_t* found = find_unique(parse_pattern(kLockRangeSites[i].pattern));
        if (!found) {
            wchar_t message[160]{};
            swprintf_s(message, L"Vanilla site 'lock-on range %S' not found (or ambiguous); No lock-on keeps the param change only.",
                kLockRangeSites[i].name);
            log(message);
            return;
        }
        sites[i] = found + kLockRangeSites[i].offset;
    }
    auto* code = static_cast<std::uint8_t*>(VirtualAlloc(
        nullptr, 4096, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
    if (!code) return;
    auto* data = reinterpret_cast<LockRangeData*>(code + 2048);
    data->on = 0;
    std::uint8_t* p = code;
    const auto put = [&](std::initializer_list<std::uint8_t> bytes) {
        for (auto b : bytes) *p++ = b;
    };
    std::uint8_t* trampolines[std::size(kLockRangeSites)]{};
    for (std::size_t i = 0; i < std::size(kLockRangeSites); ++i) {
        const LockRangeSite& site = kLockRangeSites[i];
        trampolines[i] = p;
        std::memcpy(p, sites[i], site.stolen); p += site.stolen;     // original instructions
        put({0x9C});                                                  // pushfq
        put({0x83, 0x3D});                                            // cmp dword [on],0
        const auto next = reinterpret_cast<std::intptr_t>(p) + 4 + 1;
        const auto disp = static_cast<std::int32_t>(reinterpret_cast<std::intptr_t>(&data->on) - next);
        std::memcpy(p, &disp, 4); p += 4;
        put({0x00});
        put({0x74, static_cast<std::uint8_t>(site.zero_size)});      // je keep
        std::memcpy(p, site.zero, site.zero_size); p += site.zero_size;  // xorps reach,reach
        put({0x9D});                                                  // keep: popfq
        put({0xFF, 0x25, 0x00, 0x00, 0x00, 0x00});                    // jmp [rip+0]
        const std::uint64_t resume = reinterpret_cast<std::uint64_t>(sites[i] + site.stolen);
        std::memcpy(p, &resume, 8); p += 8;
    }
    FlushInstructionCache(GetCurrentProcess(), code, 2048);
    for (std::size_t i = 0; i < std::size(kLockRangeSites); ++i) {
        std::uint8_t jump[17]{0xFF, 0x25, 0x00, 0x00, 0x00, 0x00};
        const auto target = reinterpret_cast<std::uint64_t>(trampolines[i]);
        std::memcpy(jump + 6, &target, 8);
        for (std::size_t k = 14; k < kLockRangeSites[i].stolen; ++k) jump[k] = 0x90;
        if (!write_code(sites[i], jump, kLockRangeSites[i].stolen)) {
            log(L"Vanilla lock-on range hook could not be written; No lock-on keeps the param change only.");
            return;  // earlier sites only test a flag that stays 0
        }
    }
    g_lock_range = data;
    wchar_t message[160]{};
    swprintf_s(message, L"Vanilla lock-on range hooks installed at eldenring.exe+0x%llX, +0x%llX and +0x%llX.",
        static_cast<unsigned long long>(sites[0] - reinterpret_cast<std::uint8_t*>(game::image())),
        static_cast<unsigned long long>(sites[1] - reinterpret_cast<std::uint8_t*>(game::image())),
        static_cast<unsigned long long>(sites[2] - reinterpret_cast<std::uint8_t*>(game::image())));
    log(message);
}

// Light flinch with Infinite poise. Besides poise (SuperArmor) each character
// has a toughness gauge (CSPlayerToughnessModule, ChrIns modules +0x48, module
// 9). A hit takes damage x [module+0x24] from current toughness (+0x10); at 0
// the game plays the light hit reaction and then hits the poise module
// (0x4873BC in 2.7.2.0, 0x4872DC in 2.7.0.0/2.7.1.x). The multiplier is reset
// to 1 every frame (0x487460), so a worker cannot hold it. While Infinite
// poise is on, the hook skips that subtraction for the player's module only,
// jumping where the game goes when a hit leaves toughness above 0.
struct ToughnessData {
    volatile std::uint32_t on;
    volatile std::uint64_t player_module;
};

ToughnessData* g_toughness{};

// mov r8,[rbx]; mov rdx,rax; mov rcx,rbx; call [r8+38h]; | movss xmm1,[rbx+10h];
// mulss xmm0,[rbx+24h]; mulss xmm0,xmm6 | subss; movss [rbx+10h],xmm1; ...;
// comiss xmm7,[rbx+10h]; jb +52h (the no-reaction exit, 0x90 bytes in)
constexpr const char* kToughnessPattern =
    "4c 8b 03 48 8b d0 48 8b cb 41 ff 50 38 f3 0f 10 4b 10 f3 0f 59 43 24 f3 0f 59 c6 f3 0f 5c c8 "
    "f3 0f 11 4b 10 80 7f 24 00 74 0e 48 8b 03 48 8b cb ff 50 40 f3 0f 11 43 20 0f 2f 7b 10 72 52";
constexpr std::size_t kToughnessSite = 0x0D;
constexpr std::size_t kToughnessStolen = 14;
constexpr std::size_t kToughnessExit = 0x90;

void install_toughness_hook() noexcept {
    std::uint8_t* found = find_unique(parse_pattern(kToughnessPattern));
    if (!found) {
        log(L"Vanilla site 'toughness' not found (or ambiguous); Infinite poise keeps the light flinch.");
        return;
    }
    std::uint8_t* const site = found + kToughnessSite;
    auto* code = static_cast<std::uint8_t*>(VirtualAlloc(
        nullptr, 4096, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
    if (!code) return;
    auto* data = reinterpret_cast<ToughnessData*>(code + 128);
    data->on = 0;
    data->player_module = 0;
    std::uint8_t* p = code;
    const auto put = [&](std::initializer_list<std::uint8_t> bytes) {
        for (auto b : bytes) *p++ = b;
    };
    const auto put_rel = [&](const volatile void* target, std::size_t instruction_tail) {
        const auto next = reinterpret_cast<std::intptr_t>(p) + 4 + instruction_tail;
        const auto disp = static_cast<std::int32_t>(reinterpret_cast<std::intptr_t>(target) - next);
        std::memcpy(p, &disp, 4);
        p += 4;
    };
    const auto put_jump = [&](const std::uint8_t* target) {
        put({0xFF, 0x25, 0x00, 0x00, 0x00, 0x00});                    // jmp [rip+0]
        const auto address = reinterpret_cast<std::uint64_t>(target);
        std::memcpy(p, &address, 8);
        p += 8;
    };
    put({0x9C});                                                      // pushfq
    put({0x83, 0x3D}); put_rel(&data->on, 1); put({0x00});            // cmp dword [on],0
    put({0x74, 0x18});                                                // je original (7+2+1+14)
    put({0x48, 0x3B, 0x1D}); put_rel(&data->player_module, 0);        // cmp rbx,[player]
    put({0x75, 0x0F});                                                // jne original (1+14)
    put({0x9D});                                                      // popfq
    put_jump(found + kToughnessExit);                                 // no toughness damage
    put({0x9D});                                                      // original: popfq
    std::memcpy(p, site, kToughnessStolen); p += kToughnessStolen;
    put_jump(site + kToughnessStolen);
    FlushInstructionCache(GetCurrentProcess(), code, 128);

    std::uint8_t jump[kToughnessStolen]{0xFF, 0x25, 0x00, 0x00, 0x00, 0x00};
    const auto trampoline = reinterpret_cast<std::uint64_t>(code);
    std::memcpy(jump + 6, &trampoline, 8);
    if (!write_code(site, jump, sizeof(jump))) {
        log(L"Vanilla toughness hook could not be written; Infinite poise keeps the light flinch.");
        return;
    }
    g_toughness = data;
    wchar_t message[128]{};
    swprintf_s(message, L"Vanilla toughness hook installed at eldenring.exe+0x%llX.",
        static_cast<unsigned long long>(site - reinterpret_cast<std::uint8_t*>(game::image())));
    log(message);
}

// ---------------------------------------------------------------------------
// Downed enemies (Nightreign style). After a critical hit or a riposte the
// victim's animation (enemy TAE 011000-011070, c0000 070000-070999) holds
// ChrActionFlag 67, "invincible except throw attacks", while it lies on the
// ground and gets up. The TAE handler (eldenring.exe+0x427B30 in 2.7.1.0, a
// switch on the flag type) turns it into bit 3 of the action flag module's
// flags (+0x40); bit 8 is super armour (flag 24), bit 0 perfect invincibility
// (flag 94, boss phases and the like) and bit 1 dodge frames (flag 8), both
// left alone. Nightreign's TAEs swap flag 67 for super armour plus a SpEffect
// with no poise damage. The trampoline swaps it for super armour for every
// character except the local player, so an enemy grab still protects you.

// Dying while downed. The enemy TAEs only allow a death transition at one
// instant of the animation, ChrActionFlag 69 ("throw death transition",
// e.g. 3.33-3.43 s of 011000, when the body lands), so a kill after it, while
// getting up, only plays once the animation ends. The c0000 versions keep
// flag 69 on from the landing to the end. Flag 69 sets bit 1 of the throw
// module's flags (ChrIns modules +0x88, +0x18); once a character has passed
// its flag 69 instant in the current animation, the flag 67 hook runs the
// game's own flag 69 code every frame while its HP is 0, so it dies at once.
// Never before that instant, so a killing critical hit still ends normally.
//
// r130 (in-game log): flag 69 only resolves a death the throw knows about.
// Each throw has an attacker and a defender node (throw module +0x10) with a
// state at +0x68, 3 and 4 while it runs. When throw damage kills, the damage
// code (eldenring.exe+0x449D30) calls throw module helpers that move them to
// 5 and 6 (+0x480D20 -> +0x482FF0) and send the matching notice (+0x481000).
// A normal hit, possible only with this option, never does, so the defender
// stayed at 4 and got up before dying. The flag 67 hook now makes those same
// two calls the first time it sees HP 0 with the defender at 4: from then on
// it is a killing critical hit, dying at flag 69 or at once after it.

using ThrowModuleFn = void (*)(std::uintptr_t throw_module);
ThrowModuleFn g_throw_lethal{};         // +0x480D20: defender 4 -> 6, attacker 3 -> 5
ThrowModuleFn g_throw_lethal_notify{};  // +0x481000

struct DownedData {
    volatile std::uint32_t enabled;
    std::uint32_t pad;
    std::uint64_t handler;  // downed_event
};

DownedData* g_downed{};
std::uint8_t* g_downed_site{};        // flag 67 case
std::uint8_t* g_downed_death_site{};  // flag 69 case

constexpr std::uint8_t kDownedExpected[]{0x48, 0x83, 0x4B, 0x40, 0x08, 0xE9};  // or qword [rbx+40h],8; jmp
constexpr std::uint8_t kDownedDeathExpected[]{
    0x48, 0x8B, 0x86, 0x90, 0x01, 0x00, 0x00,   // mov rax,[rsi+190h]
    0x48, 0x8B, 0x98, 0x88, 0x00, 0x00, 0x00};  // mov rbx,[rax+88h]

// Only locates the sites, like the FOV hook: the code stays untouched until
// the option is first enabled.
void resolve_downed_site() noexcept {
    std::uint8_t* hit = find_unique(parse_pattern(
        "41 83 4e 1c 20 e9 ? ? ? ? 48 83 4b 40 08 e9 ? ? ? ? 48 83 4b 40 04 e9 ? ? ? ? "
        "48 8b 86 90 01 00 00 48 8b 98 88 00 00 00"));
    if (!hit || std::memcmp(hit + 10, kDownedExpected, sizeof(kDownedExpected)) != 0) {
        log(L"Vanilla site 'downed invincibility' not found (or ambiguous); option disabled.");
        return;
    }
    g_downed_site = hit + 10;
    g_downed_death_site = hit + 30;

    // The damage code's "throw damage left HP below 1" calls, both "mov rcx,[rcx+10h]; jmp".
    std::uint8_t* calls = find_unique(parse_pattern(
        "48 8b 88 90 01 00 00 48 8b 89 88 00 00 00 e8 ? ? ? ? 45 84 ed 75 ? 48 8b cd e8 ? ? ? ? "
        "48 8b 88 90 01 00 00 48 8b 89 88 00 00 00 e8"));
    const auto call_target = [](std::uint8_t* call) {
        std::int32_t relative{};
        std::memcpy(&relative, call + 1, sizeof(relative));
        return call + 5 + relative;
    };
    constexpr std::uint8_t kThrowHelper[]{0x48, 0x8B, 0x49, 0x10, 0xE9};
    std::uint8_t* lethal = calls ? call_target(calls + 14) : nullptr;
    std::uint8_t* notify = calls ? call_target(calls + 46) : nullptr;
    if (!calls || std::memcmp(lethal, kThrowHelper, sizeof(kThrowHelper)) != 0 ||
        std::memcmp(notify, kThrowHelper, sizeof(kThrowHelper)) != 0) {
        log(L"Vanilla site 'lethal throw' not found; downed enemies killed by normal hits get up before dying.");
        return;
    }
    g_throw_lethal = reinterpret_cast<ThrowModuleFn>(lethal);
    g_throw_lethal_notify = reinterpret_cast<ThrowModuleFn>(notify);
}

// One downed sequence per character: flag 67 runs every frame from the
// critical hit until the character is up, so a gap with neither flag ends it.
// (r130: the animation ID was the reset key before; a change of ID between
// landing and getting up would have cleared the death point.)
constexpr ULONGLONG kDownedGapMs = 700;

struct DownedTrack {
    std::uintptr_t chr;
    ULONGLONG last_event;
    std::int32_t anim;           // logged only
    bool past_death_point;
    bool logged_armed;
    bool logged_dead_early;
    bool marked_lethal;           // the lethal-throw calls were made
    std::uint32_t forced_frames;  // frames the flag 69 code was run for
};

SRWLOCK g_downed_lock = SRWLOCK_INIT;
perf::Timer* g_perf_downed_hook{};
perf::Timer* g_perf_downed_lock{};
DownedTrack g_downed_tracks[32]{};
std::size_t g_downed_next{};

// Diagnostics for the in-game test, capped so a long session cannot flood the log.
constexpr LONG kDownedLogLines = 300;
volatile LONG g_downed_log_count{};

bool chr_module_field(std::uintptr_t chr, std::ptrdiff_t module, std::ptrdiff_t field, std::int32_t& value) noexcept {
    std::uintptr_t modules{}, address{};
    return game::read(chr + game::kModules, modules) && game::read(modules + module, address) &&
        game::read(address + field, value);
}

// Throw module (modules +0x88) and its node state ([+0x10]+0x68); 0 when missing.
std::uint32_t throw_state(std::uintptr_t chr, std::uintptr_t* throw_module_out = nullptr) noexcept {
    std::uintptr_t modules{}, throw_module{}, node{};
    std::uint32_t state{};
    if (game::read(chr + game::kModules, modules) && game::read(modules + 0x88, throw_module) && throw_module &&
        game::read(throw_module + 0x10, node) && node)
        game::read(node + 0x68, state);
    if (throw_module_out) *throw_module_out = throw_module;
    return state;
}

void downed_log(std::uintptr_t chr, const wchar_t* what, std::int32_t anim, std::int32_t hp, std::uint32_t extra) noexcept {
    if (!g_diagnostics || InterlockedIncrement(&g_downed_log_count) > kDownedLogLines) return;
    // Throw module (modules +0x88): flags at +0x18, throw state at [+0x10]+0x68.
    std::uintptr_t modules{}, throw_module{}, node{};
    std::uint32_t flags{}, state{};
    if (game::read(chr + game::kModules, modules) && game::read(modules + 0x88, throw_module) && throw_module) {
        game::read(throw_module + 0x18, flags);
        if (game::read(throw_module + 0x10, node) && node) game::read(node + 0x68, state);
    }
    wchar_t message[200]{};
    swprintf_s(message, L"Downed: %ls chr=%llX anim=%d hp=%d throw flags=%X state=%u (%u)", what,
        static_cast<unsigned long long>(chr), anim, hp, flags, state, extra);
    log(message);
}

// Called from the trampolines on the thread running the TAE: kind 0 for flag
// 67 (characters other than the player), 1 for flag 69. Returns 1 when the
// flag 67 hook should run the flag 69 code.
std::uint32_t downed_event(std::uintptr_t chr, std::uint32_t kind) noexcept {
    perf::Scope timing(g_perf_downed_hook);
    std::int32_t anim{-1}, hp{1};
    // The animation is only logged (the sequence is not keyed on it).
    if (g_diagnostics)
        if (const auto anim_address = game::chr_anim_id(chr)) game::read(anim_address, anim);
    const bool hp_ok = chr_module_field(chr, 0x0, 0x138, hp);
    const ULONGLONG now = GetTickCount64();
    const wchar_t* start{};
    std::int32_t changed_from{};
    bool changed{};
    const wchar_t* note{};
    std::uint32_t extra{};
    bool force{};
    bool mark_lethal{};
    lock_timed(&g_downed_lock, g_perf_downed_lock);
    DownedTrack* track{};
    for (DownedTrack& entry : g_downed_tracks)
        if (entry.chr == chr) { track = &entry; break; }
    if (!track || now - track->last_event > kDownedGapMs) {
        if (!track) track = &g_downed_tracks[g_downed_next++ % std::size(g_downed_tracks)];
        *track = {chr, now, anim, false, false, false, false, 0};
        start = kind == 1 ? L"sequence starts at flag 69" : L"sequence starts at flag 67";
    } else if (track->anim != anim) {
        changed = true;
        changed_from = track->anim;
        track->anim = anim;
    }
    track->last_event = now;
    if (kind == 1) {
        if (!track->past_death_point) note = L"flag 69 (death point reached)";
        track->past_death_point = true;
    } else if (hp_ok && hp <= 0) {
        if (!track->marked_lethal && g_throw_lethal && throw_state(chr) == 4) {
            track->marked_lethal = true;
            mark_lethal = true;
        }
        if (track->past_death_point) {
            force = true;
            ++track->forced_frames;
            const std::uint32_t frames = track->forced_frames;
            if (frames == 1 || frames == 10 || frames == 60 || frames == 300) {
                note = frames == 1 ? L"HP 0 after the death point: running flag 69 code" :
                    L"still running flag 69 code (death did not start)";
                extra = frames;
            }
        } else if (!track->logged_dead_early) {
            track->logged_dead_early = true;
            note = L"HP 0 before the death point: waiting for flag 69";
        }
    } else if (track->past_death_point && !track->logged_armed) {
        track->logged_armed = true;
        note = L"flag 67 after the death point, alive";
    }
    ReleaseSRWLockExclusive(&g_downed_lock);
    if (start) downed_log(chr, start, anim, hp_ok ? hp : -999999, 0);
    if (changed) downed_log(chr, L"animation changed in the sequence, was", anim, hp_ok ? hp : -999999,
        static_cast<std::uint32_t>(changed_from));
    if (note) downed_log(chr, note, anim, hp_ok ? hp : -999999, extra);
    if (mark_lethal) {
        // Same calls as the damage code after a killing throw hit (not under the lock).
        std::uintptr_t throw_module{};
        throw_state(chr, &throw_module);
        if (throw_module) {
            g_throw_lethal(throw_module);
            g_throw_lethal_notify(throw_module);
        }
        if (g_diagnostics)
            downed_log(chr, L"HP 0 from a normal hit: throw marked as lethal, defender state now", anim, hp,
                throw_state(chr));
    }
    return force ? 1u : 0u;
}

// The patch is a 5-byte relative jump, so the trampoline must be within 2 GB.
std::uint8_t* alloc_near(const std::uint8_t* target) noexcept {
    SYSTEM_INFO info{};
    GetSystemInfo(&info);
    const std::uintptr_t granularity = info.dwAllocationGranularity;
    const auto origin = reinterpret_cast<std::uintptr_t>(target);
    const std::uintptr_t limit = origin + 0x7FF00000;
    for (std::uintptr_t address = origin; address < limit;) {
        MEMORY_BASIC_INFORMATION region{};
        if (!VirtualQuery(reinterpret_cast<void*>(address), &region, sizeof(region))) break;
        const auto base = reinterpret_cast<std::uintptr_t>(region.BaseAddress);
        if (region.State == MEM_FREE) {
            const std::uintptr_t aligned = (base + granularity - 1) & ~(granularity - 1);
            if (aligned + 4096 <= base + region.RegionSize && aligned + 4096 <= limit) {
                if (void* code = VirtualAlloc(reinterpret_cast<void*>(aligned), 4096,
                        MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE))
                    return static_cast<std::uint8_t*>(code);
            }
        }
        address = base + region.RegionSize;
    }
    return nullptr;
}

// Yes/No questions start on Yes. A message box takes its first selected button
// from its builder (+0x10F4, -1 = not set; +0x10F0 = number of buttons; +0x18 =
// movie name, set with the layout at +0x10). Six functions read it when they
// create the box, one per kind of box (0x7B22F6..0x7B2AEE in 2.7.2.0), each
// with "mov reg,[base+10F4h]". Each read jumps to a trampoline that, while the
// option is on, records the box (buttons, first button, movie name) and, for
// a two-button question box (01_010/01_011) not on its first button, gives 0
// (Yes) and counts it; dialog_pointer_worker then
// moves a visible mouse pointer onto Yes. The builder only stores a button
// index for a "No" text that matches GR_Dialogues exactly, which the game's own
// questions (quit, travel) never do: in game (r138) the old one-byte patch at
// that store never ran.
struct DialogYesData {
    volatile std::uint32_t on;
    volatile std::uint32_t built;
    volatile std::uint64_t movie;
    volatile std::uint32_t reads;    // boxes created while the option is on
    volatile std::uint32_t buttons;  // the last one's button count
    volatile std::uint32_t value;    // and first button (-1 = not set)
    std::uint64_t question_box;        // movie names of the question boxes
    std::uint64_t small_question_box;  // (other two-button windows are left alone)
};

// The game's own copy of a movie name (UTF-16 with its terminator), which the
// builders point to; nullptr when absent or ambiguous.
const wchar_t* find_movie_name(const wchar_t* name) noexcept {
    auto* base = reinterpret_cast<std::uint8_t*>(game::image());
    if (!base) return nullptr;
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
    const auto* section = IMAGE_FIRST_SECTION(nt);
    const std::size_t bytes = (std::wcslen(name) + 1) * sizeof(wchar_t);
    const wchar_t* found{};
    for (unsigned i = 0; i < nt->FileHeader.NumberOfSections; ++i, ++section) {
        if (section->Characteristics & (IMAGE_SCN_MEM_EXECUTE | IMAGE_SCN_MEM_WRITE)) continue;
        std::uint8_t* p = base + section->VirtualAddress;
        std::uint8_t* const end = p + section->Misc.VirtualSize - bytes;
        for (; p <= end; p += 2) {
            p = static_cast<std::uint8_t*>(std::memchr(p, name[0], static_cast<std::size_t>(end - p) + 1));
            if (!p) break;
            if ((reinterpret_cast<std::uintptr_t>(p) & 1) || std::memcmp(p, name, bytes) != 0) {
                p -= reinterpret_cast<std::uintptr_t>(p) & 1;
                continue;
            }
            if (found) return nullptr;
            found = reinterpret_cast<const wchar_t*>(p);
        }
    }
    return found;
}

DialogYesData* g_dialog_yes_data{};
std::vector<std::uint8_t*> g_dialog_yes_reads;

// The 32-bit reads of +0x10F4 with a 32-bit displacement and no SIB byte; all
// close together (one group of functions), or none.
void resolve_dialog_yes_reads(bool quiet) noexcept {
    try {
        g_dialog_yes_reads.clear();
        auto* base = reinterpret_cast<std::uint8_t*>(game::image());
        if (!base) return;
        const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
        const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
        const auto* section = IMAGE_FIRST_SECTION(nt);
        const std::uint8_t tail[]{0xF4, 0x10, 0x00, 0x00};
        std::vector<std::uint8_t*> found;
        for (unsigned i = 0; i < nt->FileHeader.NumberOfSections; ++i, ++section) {
            if (!(section->Characteristics & IMAGE_SCN_MEM_EXECUTE)) continue;
            std::uint8_t* const begin = base + section->VirtualAddress;
            std::uint8_t* const end = begin + section->Misc.VirtualSize - 6;
            for (std::uint8_t* p = begin + 1; p <= end; ++p) {
                p = static_cast<std::uint8_t*>(std::memchr(p, 0x8B, static_cast<std::size_t>(end - p) + 1));
                if (!p) break;
                const std::uint8_t modrm = p[1];
                if ((modrm >> 6) != 2 || (modrm & 7) == 4 || std::memcmp(p + 2, tail, 4) != 0) continue;
                const std::uint8_t rex = p[-1];
                if ((rex & 0xF0) == 0x40 && (rex & 0x08)) continue;  // 64-bit read: not this field
                found.push_back(p);
            }
        }
        wchar_t message[200]{};
        if (found.empty() || found.size() > 8 || found.back() - found.front() > 0x10000) {
            swprintf_s(message, L"Vanilla site 'Yes/No dialog cursor' not found (%zu reads); option disabled.",
                found.size());
            log(message);
            g_dialog_yes_reads.clear();
            return;
        }
        g_dialog_yes_reads = std::move(found);
        if (quiet) return;
        swprintf_s(message, L"Vanilla site 'Yes/No dialog cursor' found: %zu reads from eldenring.exe+0x%llX.",
            g_dialog_yes_reads.size(),
            static_cast<unsigned long long>(g_dialog_yes_reads.front() - base));
        log(message);
    } catch (...) {
        g_dialog_yes_reads.clear();
    }
}

void install_dialog_yes_hooks() noexcept {
    resolve_dialog_yes_reads(false);
    if (g_dialog_yes_reads.empty()) return;
    std::uint8_t* code = alloc_near(g_dialog_yes_reads.front());
    if (!code) {
        log(L"Yes/No dialog cursor: no memory near the game code; option disabled.");
        return;
    }
    auto* data = reinterpret_cast<DialogYesData*>(code + 2048);
    data->on = 0;
    data->built = 0;
    data->movie = 0;
    data->reads = 0;
    data->buttons = 0;
    data->value = 0;
    data->question_box = reinterpret_cast<std::uint64_t>(find_movie_name(L"01_010_MessageBox"));
    data->small_question_box = reinterpret_cast<std::uint64_t>(find_movie_name(L"01_011_MessageBox_Small"));
    if (!data->question_box || !data->small_question_box) {
        log(L"Yes/No dialog cursor: a question box name was not found; those boxes keep their first button.");
        if (!data->question_box) data->question_box = 1;  // no movie name lives there
        if (!data->small_question_box) data->small_question_box = 1;
    }

    std::uint8_t* p = code;
    const auto put = [&](std::initializer_list<std::uint8_t> bytes) {
        for (auto b : bytes) *p++ = b;
    };
    const auto put_rel = [&](const volatile void* target, std::size_t instruction_tail) {
        const auto next = reinterpret_cast<std::intptr_t>(p) + 4 + instruction_tail;
        const auto disp = static_cast<std::int32_t>(reinterpret_cast<std::intptr_t>(target) - next);
        std::memcpy(p, &disp, 4);
        p += 4;
    };
    std::size_t installed = 0;
    for (std::uint8_t* read : g_dialog_yes_reads) {
        const std::uint8_t rex = ((read[-1] & 0xF0) == 0x40) ? read[-1] : 0;
        std::uint8_t* const start = rex ? read - 1 : read;
        const std::size_t length = rex ? 7 : 6;
        const unsigned reg = ((read[1] >> 3) & 7) | ((rex & 0x04) ? 8u : 0u);
        const unsigned rm = (read[1] & 7) | ((rex & 0x01) ? 8u : 0u);
        if (p + 160 > code + 2048) break;
        std::uint8_t* const entry = p;

        std::memcpy(p, start, length); p += length;                            // the original read
        put({0x9C});                                                            // pushfq
        put({0x83, 0x3D}); put_rel(&data->on, 1); put({0x00});                  // cmp dword [on],0
        put({0x74, 0x00}); std::uint8_t* const skip_off = p;                    // je done
        if (reg & 8) put({0x44});
        put({0x89, static_cast<std::uint8_t>(0x05 | ((reg & 7) << 3))});
        put_rel(&data->value, 0);                                               // mov [value],reg32
        put({0x50});                                                            // push rax
        if (rm & 8) put({0x41});
        put({0x8B, static_cast<std::uint8_t>(0x80 | (rm & 7)), 0xF0, 0x10, 0x00, 0x00});  // mov eax,[base+10F0h]
        put({0x89, 0x05}); put_rel(&data->buttons, 0);                          // mov [buttons],eax
        put({static_cast<std::uint8_t>((rm & 8) ? 0x49 : 0x48), 0x8B,
            static_cast<std::uint8_t>(0x40 | (rm & 7)), 0x18});                 // mov rax,[base+18h]
        put({0x48, 0x89, 0x05}); put_rel(&data->movie, 0);                      // mov [movie],rax
        put({0xF0, 0xFF, 0x05}); put_rel(&data->reads, 0);                      // lock inc [reads]
        put({0x48, 0x3B, 0x05}); put_rel(&data->question_box, 0);               // cmp rax,[question_box]
        put({0x74, 0x00}); std::uint8_t* const is_box = p;                      // je question
        put({0x48, 0x3B, 0x05}); put_rel(&data->small_question_box, 0);         // cmp rax,[small_question_box]
        put({0x74, 0x00}); std::uint8_t* const is_small_box = p;                // je question
        put({0x58});                                                            // pop rax
        put({0xEB, 0x00}); std::uint8_t* const skip_other = p;                  // jmp done (other window)
        std::uint8_t* const question = p;
        put({0x58});                                                            // question: pop rax
        if (rm & 8) put({0x41});
        put({0x83, static_cast<std::uint8_t>(0xB8 | (rm & 7)), 0xF0, 0x10, 0x00, 0x00, 0x02});  // cmp dword [base+10F0h],2
        put({0x75, 0x00}); std::uint8_t* const skip_count = p;                  // jne done
        if (reg & 8) put({0x45});
        put({0x85, static_cast<std::uint8_t>(0xC0 | ((reg & 7) << 3) | (reg & 7))});  // test reg32,reg32
        put({0x74, 0x00}); std::uint8_t* const skip_set = p;                    // je done (already Yes)
        if (reg & 8) put({0x45});
        put({0x31, static_cast<std::uint8_t>(0xC0 | ((reg & 7) << 3) | (reg & 7))});  // xor reg32,reg32 (Yes)
        put({0xF0, 0xFF, 0x05}); put_rel(&data->built, 0);                      // lock inc [built]
        std::uint8_t* const done = p;
        put({0x9D});                                                            // popfq
        put({0xE9}); put_rel(start + length, 0);                                // jmp back
        for (std::uint8_t* jump_end : {skip_off, skip_other, skip_count, skip_set})
            jump_end[-1] = static_cast<std::uint8_t>(done - jump_end);
        for (std::uint8_t* jump_end : {is_box, is_small_box})
            jump_end[-1] = static_cast<std::uint8_t>(question - jump_end);
        FlushInstructionCache(GetCurrentProcess(), entry, static_cast<SIZE_T>(p - entry));

        std::uint8_t jump[7]{0xE9, 0, 0, 0, 0, 0x90, 0x90};
        const auto disp = static_cast<std::int32_t>(reinterpret_cast<std::intptr_t>(entry) -
            reinterpret_cast<std::intptr_t>(start + 5));
        std::memcpy(jump + 1, &disp, 4);
        if (write_code(start, jump, length)) ++installed;
    }
    wchar_t message[160]{};
    if (installed != g_dialog_yes_reads.size()) {
        // The reads left as they were still work as the game's own.
        swprintf_s(message, L"Yes/No dialog cursor: only %zu of %zu reads hooked.", installed,
            g_dialog_yes_reads.size());
        log(message);
        if (!installed) return;
    }
    g_dialog_yes_data = data;
    swprintf_s(message, L"Yes/No dialog cursor: %zu reads hooked (the mouse pointer is moved onto Yes too).",
        installed);
    log(message);
}

// Dodge invincibility and parry window. Rolls, backsteps and dodge skills mark
// their dodge frames with ChrActionFlag 8 in their TAE (c0000 roll 027100:
// frames 0-13, heavy roll 027120: 0-12, backstep 027000: 0-7; a second 3-frame
// window for the Crucible Feather); parries (skills a692-a699, a02/a03 040392)
// mark theirs with flags 18 (frames 4-11) and 119 (4-10). The TAE flag handler
// (switch at eldenring.exe+0x427C31 in 2.7.2.0, table entry = flag - 1) turns
// them into bits of the action flag module (ChrIns modules +0x8, flags at
// +0x40) every frame while the event runs: flag 8 "or qword [rbx+40h],2"
// (+0x427E40), flag 18 "or qword [rbx+40h],80h" (+0x4280A3), flag 119 bit 37
// (+0x427E20). The module clears the flags at the start of each frame
// (+0x406183, "mov [rcx+40h],r12" with r12 = 0). Hooks: the flag 8 and flag 18
// cases count the frames they run for the player, and the clear puts the bits
// back for the player while window_worker says so (and, for the whole of a
// knockdown or an item animation, bit 1 or flag 24's bit 8, super armour, when
// those options are on). window_worker measures the
// game's own window and holds it on for that length times (percent - 100) %
// more.
struct WindowData {
    volatile std::uint32_t extend;  // dodge
    volatile std::uint32_t seen;    // flag 8 frames for the player
    volatile std::uint64_t player_module;
    volatile std::uint32_t clears;    // frame starts seen for the player (diagnostics)
    volatile std::uint32_t injected;  // of those, with the dodge bit put back
    volatile std::uint32_t parry_extend;
    volatile std::uint32_t parry_seen;      // flag 18 frames for the player
    volatile std::uint32_t parry_injected;  // frame starts with the parry bits put back
    volatile std::uint64_t held_bits;       // put back on every frame (knocked down, using an item)
};

WindowData* g_windows{};

void install_window_hooks() noexcept {
    std::uint8_t* dodge_hit = find_unique(parse_pattern("48 81 4b 40 00 80 00 00 e9 ? ? ? ? 48 83 4b 40 02 e9"));
    std::uint8_t* parry_site = find_unique(parse_pattern(
        "48 81 4b 40 80 00 00 00 e9 ? ? ? ? 48 8d 55 48 48 8b ce e8"));
    std::uint8_t* clear_hit = find_unique(parse_pattern(
        "48 8b 51 40 48 8b c2 48 89 5c 24 ? 48 c1 e8 2d 83 e0 01 48 c1 ea 1e 03 c0 4c 89 64 24 ? 45 33 e4 "
        "4c 89 74 24 ? 4c 89 61 10 83 e2 01 44 89 61 18 0b c2 4c 89 61 40 c1 e0 05"));
    if (!dodge_hit || !parry_site || !clear_hit) {
        log(L"Vanilla site 'dodge and parry frames' not found (or ambiguous); options disabled.");
        return;
    }
    std::uint8_t* const dodge_site = dodge_hit + 13;    // or qword [rbx+40h],2 (5 bytes), then jmp
    std::uint8_t* const clear_site = clear_hit + 0x33;  // mov [rcx+40h],r12; shl eax,5 (7 bytes)
    std::uint8_t* code = alloc_near(dodge_site);
    if (!code) {
        log(L"Vanilla dodge and parry hooks: no memory near the game code; options disabled.");
        return;
    }
    auto* data = reinterpret_cast<WindowData*>(code + 512);
    std::memset(code + 512, 0, sizeof(WindowData));

    std::uint8_t* p = code;
    const auto put = [&](std::initializer_list<std::uint8_t> bytes) {
        for (auto b : bytes) *p++ = b;
    };
    const auto put_rel = [&](const volatile void* target, std::size_t instruction_tail) {
        const auto next = reinterpret_cast<std::intptr_t>(p) + 4 + instruction_tail;
        const auto disp = static_cast<std::int32_t>(reinterpret_cast<std::intptr_t>(target) - next);
        std::memcpy(p, &disp, 4);
        p += 4;
    };
    // A flag case: the original or, then count the player's frames.
    const auto counting_case = [&](std::initializer_list<std::uint8_t> original, std::uint8_t* site,
                                   volatile std::uint32_t* counter) {
        std::uint8_t* const entry = p;
        put(original);
        put({0x9C});                                                        // pushfq
        put({0x48, 0x3B, 0x1D}); put_rel(&data->player_module, 0);         // cmp rbx,[player_module]
        put({0x75, 0x07});                                                  // jne done
        put({0xF0, 0xFF, 0x05}); put_rel(counter, 0);                       // lock inc [counter]
        put({0x9D});                                                        // done: popfq
        put({0xE9}); put_rel(site + original.size(), 0);                    // jmp back (to the case's jmp)
        return entry;
    };
    std::uint8_t* const dodge_code = counting_case({0x48, 0x83, 0x4B, 0x40, 0x02}, dodge_site, &data->seen);
    std::uint8_t* const parry_code =
        counting_case({0x48, 0x81, 0x4B, 0x40, 0x80, 0x00, 0x00, 0x00}, parry_site, &data->parry_seen);

    // Frame start: the original clear, then the bits again while extending.
    std::uint8_t* const clear_code = p;
    put({0x4C, 0x89, 0x61, 0x40});                                          // mov [rcx+40h],r12
    put({0xC1, 0xE0, 0x05});                                                // shl eax,5
    put({0x9C});                                                            // pushfq
    put({0x48, 0x3B, 0x0D}); put_rel(&data->player_module, 0);             // cmp rcx,[player_module]
    put({0x75, 0x00}); std::uint8_t* const skip_other = p;                  // jne done
    put({0xF0, 0xFF, 0x05}); put_rel(&data->clears, 0);                     // lock inc [clears]
    put({0x83, 0x3D}); put_rel(&data->extend, 1); put({0x00});              // cmp dword [extend],0
    put({0x74, 0x00}); std::uint8_t* const skip_dodge = p;                  // je parry
    put({0x48, 0x83, 0x49, 0x40, 0x02});                                   // or qword [rcx+40h],2 (flag 8)
    put({0xF0, 0xFF, 0x05}); put_rel(&data->injected, 0);                   // lock inc [injected]
    std::uint8_t* const parry = p;
    put({0x83, 0x3D}); put_rel(&data->parry_extend, 1); put({0x00});        // parry: cmp dword [parry_extend],0
    put({0x74, 0x00}); std::uint8_t* const skip_parry = p;                  // je held
    put({0x48, 0x81, 0x49, 0x40, 0x80, 0x00, 0x00, 0x00});                 // or qword [rcx+40h],80h (flag 18)
    put({0x48, 0x0F, 0xBA, 0x69, 0x40, 0x25});                             // bts qword [rcx+40h],37 (flag 119)
    put({0xF0, 0xFF, 0x05}); put_rel(&data->parry_injected, 0);             // lock inc [parry_injected]
    std::uint8_t* const held = p;
    put({0x50});                                                            // held: push rax
    put({0x48, 0x8B, 0x05}); put_rel(&data->held_bits, 0);                 // mov rax,[held_bits]
    put({0x48, 0x09, 0x41, 0x40});                                         // or [rcx+40h],rax
    put({0x58});                                                            // pop rax
    std::uint8_t* const done = p;
    put({0x9D});                                                            // done: popfq
    put({0xE9}); put_rel(clear_site + 7, 0);                                // jmp back
    skip_other[-1] = static_cast<std::uint8_t>(done - skip_other);
    skip_dodge[-1] = static_cast<std::uint8_t>(parry - skip_dodge);
    skip_parry[-1] = static_cast<std::uint8_t>(held - skip_parry);
    FlushInstructionCache(GetCurrentProcess(), code, static_cast<SIZE_T>(p - code));

    const auto jump_to = [](std::uint8_t* site, std::uint8_t* target, std::size_t length) {
        std::uint8_t jump[8]{0xE9, 0, 0, 0, 0, 0x90, 0x90, 0x90};
        const auto disp = static_cast<std::int32_t>(reinterpret_cast<std::intptr_t>(target) -
            reinterpret_cast<std::intptr_t>(site + 5));
        std::memcpy(jump + 1, &disp, 4);
        return write_code(site, jump, length);
    };
    // The clear first: without the flag hooks counting, it never adds anything.
    if (!jump_to(clear_site, clear_code, 7) || !jump_to(dodge_site, dodge_code, 5) ||
        !jump_to(parry_site, parry_code, 8)) {
        log(L"Vanilla dodge and parry hooks could not be written; options disabled.");
        return;
    }
    g_windows = data;
    const auto rva = [](std::uint8_t* at) {
        return static_cast<unsigned long long>(at - reinterpret_cast<std::uint8_t*>(game::image()));
    };
    wchar_t message[200]{};
    swprintf_s(message, L"Vanilla dodge and parry hooks installed at eldenring.exe+0x%llX, +0x%llX and +0x%llX.",
        rva(dodge_site), rva(parry_site), rva(clear_site));
    log(message);
}

bool write_jump(std::uint8_t* site, const std::uint8_t* target) noexcept {
    std::uint8_t jump[5]{0xE9};
    const auto disp = static_cast<std::int32_t>(reinterpret_cast<std::intptr_t>(target) -
        reinterpret_cast<std::intptr_t>(site + 5));
    std::memcpy(jump + 1, &disp, 4);
    return write_code(site, jump, sizeof(jump));
}

void install_downed_hook(std::uintptr_t world_chr_man) noexcept {
    if (g_downed || !g_downed_site || !world_chr_man) return;
    std::uint8_t* site = g_downed_site;
    if (std::memcmp(site, kDownedExpected, sizeof(kDownedExpected)) != 0) {
        g_downed_site = nullptr;
        log(L"Downed invincibility code was changed by another mod; option disabled.");
        return;
    }
    std::uint8_t* death_site = g_downed_death_site;
    const bool death_ok = std::memcmp(death_site, kDownedDeathExpected, sizeof(kDownedDeathExpected)) == 0;
    std::int32_t relative{};
    std::memcpy(&relative, site + 6, sizeof(relative));
    const std::uint64_t resume = reinterpret_cast<std::uint64_t>(site + 10) + relative;
    const std::uint64_t death_resume = reinterpret_cast<std::uint64_t>(death_site + sizeof(kDownedDeathExpected));

    std::uint8_t* code = alloc_near(site);
    if (!code) {
        log(L"Vanilla downed invincibility hook: no memory near the game code; option disabled.");
        return;
    }
    g_downed = reinterpret_cast<DownedData*>(code + 256);
    g_downed->enabled = 0;
    g_downed->handler = reinterpret_cast<std::uint64_t>(&downed_event);

    std::uint8_t* p = code;
    const auto put = [&](std::initializer_list<std::uint8_t> bytes) {
        for (auto b : bytes) *p++ = b;
    };
    const auto put_u64 = [&](std::uint64_t v) { std::memcpy(p, &v, 8); p += 8; };
    const auto put_rel = [&](const volatile void* data, std::size_t instruction_tail) {
        const auto next = reinterpret_cast<std::intptr_t>(p) + 4 + instruction_tail;
        const auto disp = static_cast<std::int32_t>(reinterpret_cast<std::intptr_t>(data) - next);
        std::memcpy(p, &disp, 4);
        p += 4;
    };
    // Both cases end in the function epilogue, which restores the non-volatile
    // registers, so rax, rcx, rdx and the other volatile ones are free; rsp is
    // 16-byte aligned there.
    const auto call_handler = [&](std::uint8_t kind) {
        put({0x48, 0x89, 0xF1});                                       // mov rcx,rsi
        put({0xBA, kind, 0x00, 0x00, 0x00});                           // mov edx,kind
        put({0x48, 0x83, 0xEC, 0x20});                                 // sub rsp,20h
        put({0xFF, 0x15}); put_rel(&g_downed->handler, 0);             // call [handler]
        put({0x48, 0x83, 0xC4, 0x20});                                 // add rsp,20h
    };

    // Flag 69 (code + 128): note the death point, then the original code.
    std::uint8_t* const death_code = code + 128;
    p = death_code;
    put({0x83, 0x3D}); put_rel(&g_downed->enabled, 1); put({0x00});    // cmp dword [enabled],0
    put({0x74, 0x00});                                                 // je death_original
    std::uint8_t* const skip_note = p;
    call_handler(1);
    skip_note[-1] = static_cast<std::uint8_t>(p - skip_note);
    std::uint8_t* const death_original = p;
    put({0x48, 0x8B, 0x86, 0x90, 0x01, 0x00, 0x00});                   // mov rax,[rsi+190h]
    put({0x48, 0x8B, 0x98, 0x88, 0x00, 0x00, 0x00});                   // mov rbx,[rax+88h]
    put({0xFF, 0x25, 0x00, 0x00, 0x00, 0x00}); put_u64(death_resume);  // jmp back into case 69

    // Flag 67 (code): rsi = ChrIns, rbx = action flag module.
    p = code;
    std::uint8_t* to_original[3]{};
    put({0x83, 0x3D}); put_rel(&g_downed->enabled, 1); put({0x00});    // cmp dword [enabled],0
    put({0x74, 0x00}); to_original[0] = p;                             // je original
    put({0x48, 0xB8}); put_u64(world_chr_man);                         // mov rax,&WorldChrMan
    put({0x48, 0x8B, 0x00});                                           // mov rax,[rax]
    put({0x48, 0x85, 0xC0});                                           // test rax,rax
    put({0x74, 0x00}); to_original[1] = p;                             // jz original
    put({0x48, 0x3B, 0xB0});                                           // cmp rsi,[rax+PlayerIns]
    const auto player_offset = static_cast<std::int32_t>(game::kPlayerIns);
    std::memcpy(p, &player_offset, 4); p += 4;
    put({0x74, 0x00}); to_original[2] = p;                             // je original
    put({0x48, 0x81, 0x4B, 0x40, 0x00, 0x01, 0x00, 0x00});             // or qword [rbx+40h],100h
    call_handler(0);
    put({0x85, 0xC0});                                                 // test eax,eax
    put({0x0F, 0x85});                                                 // jnz death_original
    const auto to_death = static_cast<std::int32_t>(death_original - (p + 4));
    std::memcpy(p, &to_death, 4); p += 4;
    put({0xFF, 0x25, 0x00, 0x00, 0x00, 0x00}); put_u64(resume);        // jmp resume
    for (std::uint8_t* from : to_original) from[-1] = static_cast<std::uint8_t>(p - from);
    put({0x48, 0x83, 0x4B, 0x40, 0x08});                               // original: or qword [rbx+40h],8
    put({0xFF, 0x25, 0x00, 0x00, 0x00, 0x00}); put_u64(resume);        // jmp resume
    FlushInstructionCache(GetCurrentProcess(), code, 256);

    if (!write_jump(site, code)) {
        g_downed = nullptr;
        log(L"Vanilla downed invincibility hook could not be written; option disabled.");
        return;
    }
    wchar_t message[160]{};
    swprintf_s(message, L"Vanilla downed invincibility hook installed at eldenring.exe+0x%llX.",
        static_cast<unsigned long long>(site - reinterpret_cast<std::uint8_t*>(game::image())));
    log(message);
    if (!death_ok || !write_jump(death_site, death_code)) {
        log(L"Vanilla downed death hook not installed (code changed); downed enemies finish getting up before dying.");
        return;
    }
    swprintf_s(message, L"Vanilla downed death hook installed at eldenring.exe+0x%llX.",
        static_cast<unsigned long long>(death_site - reinterpret_cast<std::uint8_t*>(game::image())));
    log(message);
}

Site g_intro{L"intro logos",
    "c6 ? ? ? ? ? 01 ? 03 00 00 00 ? 8b ? e8 ? ? ? ? e9 ? ? ? ? ? 8d",
    -60, "74", {0x90, 0x90}};

// ---------------------------------------------------------------------------
// Options

enum Id : std::size_t {
    // Camera
    fov_enabled, fov_percent, camera_rotate_off, camera_reset_off, camera_distance,
    // Image
    vignette_off, chromatic_off, animation_distance, aa_off, motion_blur_off, dof_off,
    // Player
    god_mode, no_death, infinite_stamina, infinite_fp, infinite_items, infinite_arrows,
    equip_weight, player_poise, player_poise_lock,
    no_fall_damage, status_immunity,
    // Combat
    damage_dealt, damage_taken, posture_dealt, posture_taken, stamina_cost, spell_fp_cost,
    status_buildup, one_hit_kill, buff_weapon, buff_self, buff_enemy,
    // Speed
    speed_global, speed_attack, speed_spell, speed_roll, speed_move, speed_jump,
    // Enemies and Torrent
    enemy_hp, enemy_poise, enemy_poise_lock, enemy_speed, enemies_no_attack, enemies_no_move, enemies_no_ai,
    torrent_no_death, torrent_speed,
    // Gameplay
    rune_loss_off, runes_gained, loot_drop, ash_cost, upgrade_cost, pause_key, set_time, time_hour, freeze_time,
    set_weather, weather_type, all_graces, reveal_map, fast_travel_anywhere,
    // Spirit ashes
    ash_no_rest, ash_anywhere, ash_multiple, ash_max_active, ash_keep_after_boss, ash_dismiss_all, ash_extra_cost,
    ash_damage_per_ash,
    // Performance & display
    fps_unlock, fps_limit, ultrawide, refresh_rate,
    // System
    skip_intro,
    // Weapon effects
    weapon_bleed, weapon_poison, weapon_rot, weapon_frost, weapon_sleep,
    dmg_physical, dmg_magic, dmg_fire, dmg_lightning, dmg_holy, weapon_scaling, weapon_weight,
    // Defence
    taken_physical, taken_magic, taken_fire, taken_lightning, taken_holy,
    // Status effects
    buildup_bleed, buildup_poison, buildup_rot, buildup_frost, buildup_sleep, buildup_madness, buildup_blight,
    // More weapon and buff options
    buff_weapon_forever, buff_self_forever, perfect_guard,
    // Hits & knockback
    knockback_dealt, knockback_taken,
    // Weapon effects (variants)
    weapon_effects_enemies,
    // Enemies (more)
    enemies_no_immunity,
    // Player (more)
    flask_hp, flask_fp,
    // Resources (more)
    spell_stamina_cost, spell_one_slot, spell_no_requirements, skill_fp_cost, carry_max,
    // Player speed (more)
    speed_item,
    // Shops, crafting and requirements
    shop_price, craft_free, weapon_requirements, upgrade_free,
    // Looks
    hide_helmet,
    // Maximum HP / FP / stamina and flat regeneration (Tarnished Vitals' features)
    max_hp, max_fp, max_stamina, hp_regen_points, fp_regen_points, stamina_regen_points,
    // HUD layout (ERNativeUI patches the HUD movies when the game loads them)
    hud_screen_edges, hud_scale, menu_edges,
    // Enemies (more)
    enemies_no_respawn,
    // Yes/No dialogs open with the cursor on Yes
    dialog_yes_default,
    // Enemies (Nightreign-style downed state)
    enemies_downed_vulnerable,
    // Torrent's jump speed (TorrentSpeedPercent stays the run speed), jump height and distance
    torrent_jump_speed, jump_height, jump_distance, torrent_jump_height, torrent_jump_distance,
    // Enemies: no lock-on (Nexus request)
    enemies_no_lock_on,
    // Damage dealt: every hit breaks poise (user request)
    posture_one_hit,
    // One-hit switches for the other side (user request)
    status_instant, die_one_hit, posture_one_hit_taken,
    // Damage dealt: charged attacks launch enemies (Nexus request)
    charged_launch,
    // Every attack launches and how far (with charged_launch)
    launch_all, launch_distance,
    // Enemies: New Game+ difficulty in any cycle (user request)
    world_journey, journey_keep_runes,
    // Map & travel: the mirror and rebirth screens anywhere (user request)
    open_mirror, open_rebirth,
    // Enemies: boss health on its own (Nexus request)
    boss_hp,
    // Image: no stains on characters, no buff visuals on you (Nexus request)
    clean_characters, hide_buff_effects,
    // Image: weapon buff visuals too (Nexus request)
    hide_weapon_buff_effects,
    // Movement: longer dodge invincibility (Nexus request)
    dodge_frames,
    // Damage taken: longer parry window (Nexus request)
    parry_frames,
    // Movement: invincible while knocked down, super armour while using items
    knockdown_invincible, item_super_armor,
    count
};

enum class Kind : std::uint8_t { toggle, slider };

// Sliders are ERUI API 1.2 numeric sliders: signed, wider than a byte, and
// shown with a unit suffix. Each range must stay within 256 positions.
struct Option {
    const wchar_t* key;
    Kind kind;
    std::int16_t fallback;
    std::int16_t minimum;
    std::int16_t maximum;
    std::int16_t step;
    std::int16_t value;
    const wchar_t* suffix;
    erui::RowHandle row;
};

constexpr Option toggle(const wchar_t* key) { return {key, Kind::toggle, 0, 0, 1, 1, 0, L"", ERUI_INVALID_ROW}; }
constexpr Option toggle_on(const wchar_t* key) { return {key, Kind::toggle, 1, 0, 1, 1, 1, L"", ERUI_INVALID_ROW}; }
constexpr Option slider(const wchar_t* key, std::int16_t fallback, std::int16_t minimum,
    std::int16_t maximum, std::int16_t step, const wchar_t* suffix = L"%") {
    return {key, Kind::slider, fallback, minimum, maximum, step, fallback, suffix, ERUI_INVALID_ROW};
}

Option g_options[count]{
    toggle(L"FovEnabled"), slider(L"FovPercent", 100, 25, 200, 5),
    toggle(L"DisableCameraAutoRotate"), toggle(L"DisableCameraReset"),
    slider(L"CameraDistancePercent", 100, 25, 500, 5),
    toggle(L"RemoveVignette"), toggle(L"RemoveChromaticAberration"), toggle(L"FullAnimationDistance"),
    toggle(L"DisableAntiAliasing"), toggle(L"DisableMotionBlur"), toggle(L"DisableDepthOfField"),
    toggle(L"GodMode"), toggle(L"NoDeath"), toggle(L"InfiniteStamina"), toggle(L"InfiniteFp"),
    toggle(L"InfiniteItems"), toggle(L"InfiniteArrows"),
    slider(L"EquipWeightPercent", 100, 0, 200, 5),
    // Sliders with a scale selector use all 256 positions in steps of 5.
    slider(L"PlayerPoisePercent", 100, 0, 500, 10), toggle(L"PlayerInfinitePoise"),
    toggle(L"NoFallDamage"), toggle(L"StatusImmunity"),
    slider(L"DamageDealtPercent", 100, 10, 1000, 10), slider(L"DamageTakenPercent", 100, 0, 1000, 10),
    slider(L"PostureDamageDealtPercent", 100, 0, 1000, 10), slider(L"PostureDamageTakenPercent", 100, 0, 1000, 10),
    slider(L"StaminaCostPercent", 100, 0, 300, 5), slider(L"SpellFpCostPercent", 100, 0, 300, 5),
    slider(L"StatusBuildupPercent", 100, 0, 1000, 10), toggle(L"OneHitKill"),
    slider(L"BuffDurationWeaponPercent", 100, 25, 500, 25), slider(L"BuffDurationSelfPercent", 100, 25, 500, 25),
    slider(L"BuffDurationEnemyPercent", 100, 25, 500, 25),
    slider(L"SpeedGlobalPercent", 100, 25, 300, 5), slider(L"SpeedAttackPercent", 100, 25, 300, 5),
    slider(L"SpeedSpellPercent", 100, 25, 300, 5), slider(L"SpeedRollPercent", 100, 25, 300, 5),
    slider(L"SpeedMovePercent", 100, 25, 300, 5), slider(L"SpeedJumpPercent", 100, 25, 300, 5),
    slider(L"EnemyHpPercent", 100, 10, 1000, 10), slider(L"EnemyPoisePercent", 100, 0, 500, 10),
    toggle(L"EnemyInfinitePoise"), slider(L"EnemySpeedPercent", 100, 25, 300, 5),
    toggle(L"EnemiesNoAttack"),
    toggle(L"EnemiesNoMove"), toggle(L"EnemiesNoAi"), toggle(L"TorrentNoDeath"),
    slider(L"TorrentSpeedPercent", 100, 25, 300, 5),
    toggle(L"DisableRuneLoss"), slider(L"RunesGainedPercent", 100, 0, 1000, 10),
    slider(L"LootDropPercent", 100, 0, 1000, 10),
    slider(L"SpiritAshCostPercent", 100, 0, 300, 10), slider(L"UpgradeCostPercent", 100, 0, 300, 10),
    toggle(L"PauseKey"),
    toggle(L"SetTimeOfDay"), slider(L"TimeOfDayHour", 12, 0, 23, 1, L":00"), toggle(L"FreezeTime"),
    toggle(L"SetWeather"), slider(L"WeatherType", 4, 0, 15, 1, L""),
    toggle(L"ShowAllGraces"), toggle(L"RevealMap"), toggle(L"FastTravelAnywhere"),
    toggle_on(L"SpiritResummonWithoutRest"), toggle_on(L"SpiritSummonFarFromStones"),
    toggle(L"SpiritMultipleAshes"), slider(L"SpiritMaxActiveAshes", 8, 2, 20, 1, L""),
    toggle_on(L"SpiritKeepAfterBoss"), toggle(L"SpiritDismissAll"),
    slider(L"SpiritExtraAshCostPercent", 100, 0, 200, 10), slider(L"SpiritDamageLossPerExtraAsh", 0, 0, 20, 1),
    toggle(L"UnlockFps"), slider(L"FpsLimit", 120, 30, 360, 10, L" FPS"), toggle(L"AllowUltrawide"),
    slider(L"FullscreenRefreshHz", 0, 0, 240, 1, L" Hz"),
    toggle(L"SkipIntroLogos"),
    toggle(L"WeaponBleed"), toggle(L"WeaponPoison"), toggle(L"WeaponScarletRot"), toggle(L"WeaponFrostbite"),
    toggle(L"WeaponSleep"),
    slider(L"PhysicalDamagePercent", 100, 0, 500, 10), slider(L"MagicDamagePercent", 100, 0, 500, 10),
    slider(L"FireDamagePercent", 100, 0, 500, 10), slider(L"LightningDamagePercent", 100, 0, 500, 10),
    slider(L"HolyDamagePercent", 100, 0, 500, 10),
    slider(L"WeaponScalingPercent", 100, 0, 1000, 10), slider(L"WeaponWeightPercent", 100, 0, 500, 10),
    slider(L"PhysicalDamageTakenPercent", 100, 0, 500, 10), slider(L"MagicDamageTakenPercent", 100, 0, 500, 10),
    slider(L"FireDamageTakenPercent", 100, 0, 500, 10), slider(L"LightningDamageTakenPercent", 100, 0, 500, 10),
    slider(L"HolyDamageTakenPercent", 100, 0, 500, 10),
    slider(L"BleedBuildupPercent", 100, 0, 1000, 10), slider(L"PoisonBuildupPercent", 100, 0, 1000, 10),
    slider(L"ScarletRotBuildupPercent", 100, 0, 1000, 10), slider(L"FrostbiteBuildupPercent", 100, 0, 1000, 10),
    slider(L"SleepBuildupPercent", 100, 0, 1000, 10), slider(L"MadnessBuildupPercent", 100, 0, 1000, 10),
    slider(L"DeathBlightBuildupPercent", 100, 0, 1000, 10),
    toggle(L"PermanentWeaponBuffs"), toggle(L"PermanentSelfBuffs"), toggle(L"PerfectGuard"),
    slider(L"KnockbackDealtPercent", 100, 0, 500, 10), slider(L"KnockbackTakenPercent", 100, 0, 500, 10),
    toggle(L"WeaponEffectsOnEnemies"),
    toggle(L"RemoveEnemyImmunities"),
    slider(L"CrimsonFlaskPercent", 100, 0, 500, 10), slider(L"CeruleanFlaskPercent", 100, 0, 500, 10),
    slider(L"SpellStaminaCostPercent", 100, 0, 500, 10), toggle(L"SpellsUseOneSlot"), toggle(L"IgnoreSpellRequirements"),
    slider(L"SkillFpCostPercent", 100, 0, 500, 10), toggle(L"Carry999Items"),
    slider(L"SpeedItemPercent", 100, 25, 300, 5),
    slider(L"ShopPricePercent", 100, 0, 500, 10), toggle(L"CraftWithoutMaterials"),
    slider(L"WeaponRequirementsPercent", 100, 0, 100, 5), toggle(L"UpgradeWithoutMaterials"),
    toggle(L"HideHelmet"),
    slider(L"MaxHpPercent", 100, 10, 1000, 5), slider(L"MaxFpPercent", 100, 10, 1000, 5),
    slider(L"MaxStaminaPercent", 100, 10, 1000, 5),
    // ERNativeUI sliders hold at most 256 values.
    slider(L"HpRegenPoints", 0, 0, 1000, 5, L"/s"), slider(L"FpRegenPoints", 0, 0, 250, 1, L"/s"),
    slider(L"StaminaRegenPoints", 0, 0, 250, 1, L"/s"),
    toggle(L"HudToScreenEdges"), slider(L"HudScalePercent", 100, 50, 150, 5),
    toggle(L"MenusToScreenEdges"),
    toggle(L"EnemiesDoNotRespawn"),
    toggle(L"DialogsDefaultToYes"),
    toggle(L"HitDownedEnemies"),
    slider(L"TorrentJumpSpeedPercent", 100, 25, 300, 5),
    slider(L"JumpHeightPercent", 100, 25, 500, 5), slider(L"JumpDistancePercent", 100, 25, 500, 5),
    slider(L"TorrentJumpHeightPercent", 100, 25, 500, 5), slider(L"TorrentJumpDistancePercent", 100, 25, 500, 5),
    toggle(L"DisableLockOn"),
    toggle(L"BreakPoiseInOneHit"),
    toggle(L"InstantStatusBuildup"), toggle(L"DieInOneHit"), toggle(L"EnemiesBreakPoiseInOneHit"),
    slider(L"ChargedAttacksLaunch", 0, 0, 3, 1, L""),
    toggle(L"EveryAttackLaunches"),
    slider(L"LaunchDistancePercent", 100, 100, 5000, 25),
    slider(L"WorldDifficulty", 0, 0, 7, 1, L""),
    toggle(L"WorldDifficultyKeepRunes"),
    toggle(L"OpenMirror"), toggle(L"OpenRebirth"),
    slider(L"BossHpPercent", 100, 10, 1000, 10),
    toggle(L"KeepCharactersClean"), toggle(L"HideBuffEffects"),
    toggle(L"HideWeaponBuffEffects"),
    slider(L"DodgeFramesPercent", 100, 100, 500, 10),
    slider(L"ParryWindowPercent", 100, 100, 500, 10),
    toggle(L"KnockdownInvincible"), toggle(L"ItemSuperArmor"),
};

std::int16_t clamp_value(const Option& option, int value) noexcept {
    if (option.kind == Kind::toggle) return value ? 1 : 0;
    value = std::clamp(value, static_cast<int>(option.minimum), static_cast<int>(option.maximum));
    value = option.minimum + (value - option.minimum) / option.step * option.step;
    return static_cast<std::int16_t>(value);
}

int value_of(Id id) noexcept {
    return g_options[id].value;
}

// ---------------------------------------------------------------------------
// Game state: debug flags, params, speed, regeneration

game::Statics g_statics{};
HANDLE g_param_event{};

// ChrDbgFlags byte offsets.
struct FlagBinding { Id id; std::ptrdiff_t offset; };
constexpr FlagBinding kFlags[]{
    {no_death, 0x1}, {torrent_no_death, 0x2}, {one_hit_kill, 0x3}, {infinite_items, 0x4},
    {infinite_stamina, 0x5}, {infinite_fp, 0x6}, {infinite_arrows, 0x7},
    {enemies_no_attack, 0xE}, {enemies_no_move, 0xF}, {enemies_no_ai, 0x10},
    {skill_fp_cost, 0x12},  // "skills without FP": set while Skill FP cost is 0%
    // Also tested in r121 (options removed): +0x8 enemies cannot see you,
    // +0x9 they never target you, +0xA they cannot hear you.
    // +0xB / +0xC are "nobody dies" / "nobody takes damage": until r135 god_mode
    // used +0xC, which also stopped hits on enemies (see sync_god_mode).
};

void apply_flag(Id id) noexcept {
    if (!g_statics.chr_dbg_flags) return;
    // Skill FP cost at 0% also sets the "skills without FP" flag: with an empty
    // FP bar the game otherwise still refuses the skill's effect.
    const bool on = id == skill_fp_cost ? value_of(id) == 0 : value_of(id) != 0;
    for (const auto& flag : kFlags)
        if (flag.id == id)
            game::write<std::uint8_t>(g_statics.chr_dbg_flags + flag.offset, on ? 1 : 0);
}

// Param multipliers. Original values are captured once, so every change is
// computed from the game's own numbers and 100% restores them exactly.
enum class FieldType : std::uint8_t { u8, u16, i16, u32, i32, f32 };

struct ParamField {
    Id id;
    const wchar_t* param;
    std::size_t row_size;
    std::size_t offset;
    FieldType type;
    Id extra{count};  // second slider multiplied in (per element or per status)
    Id lock{count};   // toggle that replaces the value with lock_value
    float lock_value{};
    bool cap{};       // the lock only lowers values above lock_value
    bool stackable{}; // the lock only raises values above 1 (stackable items)
};

constexpr ParamField kParamFields[]{
    {damage_dealt, L"AtkParam_Pc", 0x1C8, 0x3E, FieldType::u16, dmg_physical},   // physical correction
    {damage_dealt, L"AtkParam_Pc", 0x1C8, 0x40, FieldType::u16, dmg_magic},      // magic
    {damage_dealt, L"AtkParam_Pc", 0x1C8, 0x42, FieldType::u16, dmg_fire},       // fire
    {damage_dealt, L"AtkParam_Pc", 0x1C8, 0x44, FieldType::u16, dmg_lightning},  // lightning
    {damage_dealt, L"AtkParam_Pc", 0x1C8, 0x186, FieldType::u16, dmg_holy},      // holy
    // NPC attacks use flat damage; their correction fields (+0x3E..) are empty
    // (18 of 12855 rows in 2.7.1.0). atkPhys / Mag / Fire / Thun, then atkDark.
    {damage_taken, L"AtkParam_Npc", 0x1C8, 0x50, FieldType::u16, taken_physical},
    {damage_taken, L"AtkParam_Npc", 0x1C8, 0x52, FieldType::u16, taken_magic},
    {damage_taken, L"AtkParam_Npc", 0x1C8, 0x54, FieldType::u16, taken_fire},
    {damage_taken, L"AtkParam_Npc", 0x1C8, 0x56, FieldType::u16, taken_lightning},
    {damage_taken, L"AtkParam_Npc", 0x1C8, 0x188, FieldType::u16, taken_holy},
    {stamina_cost, L"BehaviorParam_PC", 0x20, 0x14, FieldType::i32},
    // Magic (2.7.1.0 scan): FP +0x10 and stamina +0x12 per cast, then the
    // charged cast at +0x5C / +0x5E; memory slots u8 +0x21 (1-3);
    // Intelligence / Faith requirements u8 +0x22 / +0x23.
    {spell_fp_cost, L"Magic", 0xA8, 0x10, FieldType::i16},
    {spell_fp_cost, L"Magic", 0xA8, 0x5C, FieldType::i16},
    {spell_stamina_cost, L"Magic", 0xA8, 0x12, FieldType::i16},
    {spell_stamina_cost, L"Magic", 0xA8, 0x5E, FieldType::i16},
    {spell_one_slot, L"Magic", 0xA8, 0x21, FieldType::u8, count, spell_one_slot, 1.0f, true},
    {spell_no_requirements, L"Magic", 0xA8, 0x22, FieldType::u8, count, spell_no_requirements, 0.0f},
    {spell_no_requirements, L"Magic", 0xA8, 0x23, FieldType::u8, count, spell_no_requirements, 0.0f},
    // SwordArtsParam (row 0x20, 278 Ashes of War): FP of the main use +0x12,
    // of the charged / held stage +0x14 and of the follow-up +0x16 (-1 = none).
    {skill_fp_cost, L"SwordArtsParam", 0x20, 0x12, FieldType::i16},
    {skill_fp_cost, L"SwordArtsParam", 0x20, 0x14, FieldType::i16},
    {skill_fp_cost, L"SwordArtsParam", 0x20, 0x16, FieldType::i16},
    // EquipParamGoods (2.7.1.0 scan): carried maximum s16 +0x3A (1, 99, 999...)
    // and stored maximum +0x70 (mostly 600). Unique items (1) are left alone.
    {carry_max, L"EquipParamGoods", 0xB0, 0x3A, FieldType::i16, count, carry_max, 999.0f, false, true},
    {carry_max, L"EquipParamGoods", 0xB0, 0x70, FieldType::i16, count, carry_max, 999.0f, false, true},
    // Player poise damage is the weapon's saWeaponDamage times the attack's
    // atkSuperArmorCorrection, plus atkSuperArmor (spells and fixed hits). The
    // weapon value is cached on equip, so the per-attack fields are scaled instead.
    {posture_dealt, L"AtkParam_Pc", 0x1C8, 0x84, FieldType::f32},    // atkSuperArmor
    {posture_dealt, L"AtkParam_Pc", 0x1C8, 0x1B0, FieldType::f32},   // atkSuperArmorCorrection
    {posture_taken, L"AtkParam_Npc", 0x1C8, 0x84, FieldType::f32},
    {equip_weight, L"EquipParamWeapon", 0x298, 0x10, FieldType::f32, weapon_weight},
    // correctStrength / Agility / Magic / Faith follow sellValue (+0x20).
    {weapon_scaling, L"EquipParamWeapon", 0x298, 0x24, FieldType::f32},
    {weapon_scaling, L"EquipParamWeapon", 0x298, 0x28, FieldType::f32},
    {weapon_scaling, L"EquipParamWeapon", 0x298, 0x2C, FieldType::f32},
    {weapon_scaling, L"EquipParamWeapon", 0x298, 0x30, FieldType::f32},
    // physGuardCutRate / mag / fire / thun follow correctFaith; 100 blocks all.
    {perfect_guard, L"EquipParamWeapon", 0x298, 0x34, FieldType::f32, count, perfect_guard, 100.0f},
    {perfect_guard, L"EquipParamWeapon", 0x298, 0x38, FieldType::f32, count, perfect_guard, 100.0f},
    {perfect_guard, L"EquipParamWeapon", 0x298, 0x3C, FieldType::f32, count, perfect_guard, 100.0f},
    {perfect_guard, L"EquipParamWeapon", 0x298, 0x40, FieldType::f32, count, perfect_guard, 100.0f},
    // knockbackDist (+0x10) precedes hitStopTime and spEffectId0 (+0x18); used only once
    // sync_weapon_effects has confirmed those slots (g_atk_layout_ok).
    {knockback_dealt, L"AtkParam_Pc", 0x1C8, 0x10, FieldType::f32},
    {knockback_taken, L"AtkParam_Npc", 0x1C8, 0x10, FieldType::f32},
    {equip_weight, L"EquipParamProtector", 0x1A0, 0x24, FieldType::f32},
    {player_poise, L"EquipParamProtector", 0x1A0, 0x10, FieldType::f32},  // saDurability (armour poise)
    {status_buildup, L"SpEffectParam", 0x390, 0xCC, FieldType::i32, buildup_poison},
    {status_buildup, L"SpEffectParam", 0x390, 0xD0, FieldType::i32, buildup_rot},
    {status_buildup, L"SpEffectParam", 0x390, 0xD4, FieldType::i32, buildup_bleed},
    {status_buildup, L"SpEffectParam", 0x390, 0xD8, FieldType::i32, buildup_blight},
    {status_buildup, L"SpEffectParam", 0x390, 0x1A8, FieldType::i32, buildup_frost},
    {status_buildup, L"SpEffectParam", 0x390, 0x338, FieldType::i32, buildup_sleep},
    {status_buildup, L"SpEffectParam", 0x390, 0x33C, FieldType::i32, buildup_madness},
    {enemy_hp, L"NpcParam", 0x2E0, 0x24, FieldType::u32},
    {enemy_poise, L"NpcParam", 0x2E0, 0x2A8, FieldType::f32},  // superArmorDurability
    {runes_gained, L"NpcParam", 0x2E0, 0x2C, FieldType::u32},
    {camera_distance, L"LockCamParam", 0x80, 0x0, FieldType::f32},  // camDistTarget
    {ash_cost, L"EquipParamGoods", 0xB0, 0x80, FieldType::i16},     // consumeMP (spirit ashes)
    {ash_cost, L"EquipParamGoods", 0xB0, 0x82, FieldType::i16},     // consumeHP
    {upgrade_cost, L"EquipParamWeapon", 0x298, 0x1C, FieldType::i32},  // reinforcePrice
    // Spirit ash upgrades (Roderika): EquipParamGoods.reinforcePrice s32 +0x8C,
    // runes per level (2.7.1.0: 830 spirit ash rows, 0 for other goods).
    {upgrade_cost, L"EquipParamGoods", 0xB0, 0x8C, FieldType::i32},
    // ShopLineupParam (row 0x34, 1296 rows in 2.7.1.0): price s32 +0x4, in
    // the currency of costType (+0x18: runes for 1137 rows, else items such as
    // Dragon Hearts). Trades that ask for a material set (mtrlId +0x8) keep it.
    {shop_price, L"ShopLineupParam", 0x34, 0x4, FieldType::i32},
    // EquipParamWeapon properStrength / Agility / Magic / Faith u8 +0xF2..+0xF5
    // and properLuck u8 +0x195 (2.7.1.0: Longsword 10 Str, Uchigatana 11 Str).
    {weapon_requirements, L"EquipParamWeapon", 0x298, 0xF2, FieldType::u8},
    {weapon_requirements, L"EquipParamWeapon", 0x298, 0xF3, FieldType::u8},
    {weapon_requirements, L"EquipParamWeapon", 0x298, 0xF4, FieldType::u8},
    {weapon_requirements, L"EquipParamWeapon", 0x298, 0xF5, FieldType::u8},
    {weapon_requirements, L"EquipParamWeapon", 0x298, 0x195, FieldType::u8},
};

struct ParamState {
    bool attempted{};
    bool ready{};
    game::ParamTable table{};
    std::vector<double> original{};
    int applied_percent{100};
    std::vector<bool> boss{};  // enemy_hp: rows that are bosses
    int applied_boss_percent{100};
};

// Boss health (Nexus request): NpcParam rows are bosses when npcType
// (u8 +0x132) is 1 (163 rows in 2.7.2.0: Margit, Rennala, Malenia...) or when
// isSoulGetByBoss (bit 0 of +0x15D) is set (315 rows: every enemy that gives
// the boss-defeated runes, field, dungeon and DLC bosses included). Their
// health is Enemy health times Boss health.
bool npc_row_is_boss(std::uintptr_t row) noexcept {
    std::uint8_t type{}, flags{};
    game::read(row + 0x132, type);
    game::read(row + 0x15D, flags);
    return type == 1 || (flags & 0x1);
}

// Infinite poise overrides the poise sliders with a value no hit can deplete.
constexpr int kLockedPercent = -1;
constexpr double kLockedPoise = 1000000.0;
// Break poise in one hit: player poise damage x30. In game x10 needed 2-3 hits
// and x100 (or a fixed 1e6) seemed to have no effect, as if the hit's poise
// damage wrapped around; x30 stays well below that.
constexpr int kOneHitPosturePercent = 3000;
constexpr double kMaxPoiseDamage = 32767.0;
// Instant status effects: buildup x30, like poise. Die in one hit: enemy attack
// power x1000; the u16 fields stop at 65535.
constexpr int kInstantStatusPercent = 3000;
constexpr int kOneHitDeathPercent = 100000;

// Set once the AtkParam SpEffect slots are confirmed (see sync_weapon_effects).
bool g_atk_layout_ok{};

int target_percent(const ParamField& field) noexcept {
    if (field.id == player_poise && value_of(player_poise_lock)) return kLockedPercent;
    if (field.id == enemy_poise && value_of(enemy_poise_lock)) return kLockedPercent;
    if (field.id == posture_dealt && value_of(posture_one_hit)) return kOneHitPosturePercent;
    if (field.id == posture_taken && value_of(posture_one_hit_taken)) return kOneHitPosturePercent;
    if (field.id == status_buildup && value_of(status_instant)) return kInstantStatusPercent;
    if (field.id == damage_taken && value_of(die_one_hit)) return kOneHitDeathPercent;
    if ((field.id == knockback_dealt || field.id == knockback_taken) &&
        !g_atk_layout_ok) return 100;
    if (field.lock != count) return value_of(field.lock) ? kLockedPercent : 100;
    if (field.extra != count) return value_of(field.id) * value_of(field.extra) / 100;
    return value_of(field.id);
}

ParamState g_param_states[std::size(kParamFields)]{};

double read_field(std::uintptr_t address, FieldType type) noexcept {
    switch (type) {
    case FieldType::f32: { float v{}; game::read(address, v); return v; }
    case FieldType::u8: { std::uint8_t v{}; game::read(address, v); return v; }
    case FieldType::u16: { std::uint16_t v{}; game::read(address, v); return v; }
    case FieldType::i16: { std::int16_t v{}; game::read(address, v); return v; }
    case FieldType::u32: { std::uint32_t v{}; game::read(address, v); return v; }
    default: { std::int32_t v{}; game::read(address, v); return v; }
    }
}

void write_field(std::uintptr_t address, FieldType type, double scaled) noexcept {
    if (type == FieldType::f32) {
        game::write(address, static_cast<float>(scaled));
        return;
    }
    const auto value = static_cast<std::int64_t>(scaled);
    switch (type) {
    case FieldType::u8:
        game::write(address, static_cast<std::uint8_t>(std::clamp<std::int64_t>(value, 0, 0xFF)));
        break;
    case FieldType::u16:
        game::write(address, static_cast<std::uint16_t>(std::clamp<std::int64_t>(value, 0, 0xFFFF)));
        break;
    case FieldType::i16:
        game::write(address, static_cast<std::int16_t>(std::clamp<std::int64_t>(value, -0x8000, 0x7FFF)));
        break;
    case FieldType::u32:
        game::write(address, static_cast<std::uint32_t>(std::clamp<std::int64_t>(value, 0, 0xFFFFFFFFll)));
        break;
    default:
        game::write(address, static_cast<std::int32_t>(std::clamp<std::int64_t>(value, INT32_MIN, INT32_MAX)));
        break;
    }
}

// Runs on the param worker. Tables appear once the regulation file is loaded.
void sync_params() noexcept {
    for (std::size_t i = 0; i < std::size(kParamFields); ++i) {
        const auto& field = kParamFields[i];
        auto& state = g_param_states[i];
        if (state.attempted) continue;  // table found with an unexpected layout
        if (!state.ready) {
            game::ParamTable table;
            // Fields of one param share its table, so it is found and logged once.
            bool shared{};
            for (std::size_t j = 0; j < i && !shared; ++j)
                if (g_param_states[j].ready && !wcscmp(kParamFields[j].param, field.param)) {
                    table = g_param_states[j].table;
                    shared = true;
                }
            // find_param logs only once the table exists, so early retries stay quiet.
            if (!shared && !game::find_param(g_statics, field.param, field.row_size, table, g_log)) {
                if (table.file) state.attempted = true;
                continue;
            }
            state.table = std::move(table);
            state.original.clear();
            state.original.reserve(state.table.rows.size());
            for (const auto row : state.table.rows)
                state.original.push_back(read_field(row + field.offset, field.type));
            if (field.id == enemy_hp) {
                state.boss.clear();
                std::size_t bosses{};
                for (const auto row : state.table.rows) {
                    state.boss.push_back(npc_row_is_boss(row));
                    if (state.boss.back()) ++bosses;
                }
                wchar_t message[120]{};
                swprintf_s(message, L"Boss health: %zu of %zu NpcParam rows are bosses.", bosses,
                    state.table.rows.size());
                log(message);
            }
            state.ready = true;
            state.applied_percent = 100;
            state.applied_boss_percent = 100;
        }
        const int percent = target_percent(field);
        const int boss_percent = field.id == enemy_hp ? value_of(boss_hp) : 100;
        if (percent == state.applied_percent && boss_percent == state.applied_boss_percent) continue;
        for (std::size_t r = 0; r < state.table.rows.size(); ++r) {
            double locked = field.lock != count ? field.lock_value : kLockedPoise;
            if (field.cap) locked = std::min<double>(state.original[r], locked);  // only lowers
            if (field.stackable) locked = state.original[r] > 1 ? std::max<double>(state.original[r], locked)
                                                                : state.original[r];
            // Negative values are markers (-1 = unused), never amounts: keep them.
            double scaled = state.original[r] < 0 ? state.original[r] : state.original[r] * percent / 100.0;
            if (boss_percent != 100 && r < state.boss.size() && state.boss[r] && scaled > 0)
                scaled = scaled * boss_percent / 100.0;
            // The game wraps poise damage past 32767 (x100 broke nothing in game), so
            // large percentages stop at the strongest value that still counts.
            if (field.id == posture_dealt || field.id == posture_taken)
                scaled = std::min(scaled, kMaxPoiseDamage);
            write_field(state.table.rows[r] + field.offset, field.type,
                percent != kLockedPercent ? scaled : locked);
        }
        state.applied_percent = percent;
        state.applied_boss_percent = boss_percent;
        if (field.id == posture_dealt || field.id == posture_taken || field.id == damage_taken ||
            field.id == status_buildup) {
            double original_max{};
            for (const double original : state.original) original_max = std::max(original_max, original);
            wchar_t message[200]{};
            swprintf_s(message, L"Scaled %s +0x%zX: %d%% on %zu rows (largest %.1f -> %.1f before limits).",
                field.param, field.offset, percent, state.table.rows.size(), original_max,
                original_max * percent / 100.0);
            log(message);
        }
        if (field.stackable) {
            std::size_t raised{};
            for (const double original : state.original)
                if (original > 1 && original < field.lock_value) ++raised;
            wchar_t message[160]{};
            swprintf_s(message, L"Carry limits: %s at +0x%zX on %zu stackable items.",
                percent == kLockedPercent ? L"999" : L"restored", field.offset, raised);
            log(message);
        }
        if (field.id == shop_price || (field.id == weapon_requirements && field.offset == 0xF2)) {
            wchar_t message[160]{};
            swprintf_s(message, L"%s: %d%% on %zu %s rows.",
                field.id == shop_price ? L"Shop prices" : L"Weapon requirements", percent,
                state.table.rows.size(), field.param);
            log(message);
        }
    }
}

// Enemy drops: each ItemLotParam row holds 8 weighted slots (lotItemId at
// 0x0 + 4k, lotItemBasePoint at 0x40 + 2k). A slot with item id 0 is the
// "no drop" outcome, so only the weights of real items are scaled.
struct LootState {
    bool attempted{};
    bool ready{};
    game::ParamTable table{};
    std::vector<std::uint16_t> original{};  // 8 weights per row
    int applied_percent{100};
};

LootState g_loot{};

void sync_loot() noexcept {
    if (g_loot.attempted) return;
    if (!g_loot.ready) {
        game::ParamTable table;
        if (!game::find_param(g_statics, L"ItemLotParam_enemy", 0x98, table, g_log)) {
            if (table.file) g_loot.attempted = true;
            return;
        }
        g_loot.table = std::move(table);
        g_loot.original.assign(g_loot.table.rows.size() * 8, 0);
        for (std::size_t r = 0; r < g_loot.table.rows.size(); ++r)
            for (std::size_t k = 0; k < 8; ++k)
                game::read(g_loot.table.rows[r] + 0x40 + k * 2, g_loot.original[r * 8 + k]);
        g_loot.ready = true;
        g_loot.applied_percent = 100;
    }
    const int percent = value_of(loot_drop);
    if (percent == g_loot.applied_percent) return;
    for (std::size_t r = 0; r < g_loot.table.rows.size(); ++r) {
        const auto row = g_loot.table.rows[r];
        for (std::size_t k = 0; k < 8; ++k) {
            std::int32_t item{};
            if (!game::read(row + k * 4, item) || item == 0) continue;
            const auto scaled = static_cast<std::int64_t>(g_loot.original[r * 8 + k]) * percent / 100;
            game::write(row + 0x40 + k * 2, static_cast<std::uint16_t>(std::clamp<std::int64_t>(scaled, 0, 0xFFFF)));
        }
    }
    g_loot.applied_percent = percent;
}

// Effect durations: SpEffectParam.effectEndurance (f32 +0x8, -1 permanent, 0
// instant). Only timed effects of 5-999 s are touched, so short internal
// helpers and permanent passives keep their values. Categories, first match:
// weapon = wepParamChange (+0x158) set; enemy = effectTargetOpposeTarget
// (+0x16C bit 0); self = self or friendly target (+0x16C bits 2/1) that
// applies to the player (+0x15F bit 3).
struct BuffState {
    bool attempted{};
    bool ready{};
    std::vector<std::uintptr_t> rows[3]{};
    std::vector<float> original[3]{};
    int applied_percent[3]{100, 100, 100};
};

BuffState g_buffs{};
constexpr Id kBuffIds[3]{buff_weapon, buff_self, buff_enemy};

void sync_buffs() noexcept {
    if (g_buffs.attempted) return;
    if (!g_buffs.ready) {
        game::ParamTable table;
        if (!game::find_param(g_statics, L"SpEffectParam", 0x390, table, g_log)) {
            if (table.file) g_buffs.attempted = true;
            return;
        }
        for (const auto row : table.rows) {
            float duration{};
            std::uint8_t weapon{}, targets{}, affiliation{};
            if (!game::read(row + 0x8, duration) || duration < 5.0f || duration > 999.0f) continue;
            game::read(row + 0x158, weapon);
            game::read(row + 0x16C, targets);
            game::read(row + 0x15F, affiliation);
            int category = -1;
            if (weapon) category = 0;
            else if (targets & 0x1) category = 2;
            else if ((targets & 0x6) && (affiliation & 0x8)) category = 1;
            if (category < 0) continue;
            g_buffs.rows[category].push_back(row);
            g_buffs.original[category].push_back(duration);
        }
        wchar_t message[160]{};
        swprintf_s(message, L"Effect durations: %zu weapon, %zu self, %zu enemy SpEffectParam rows.",
            g_buffs.rows[0].size(), g_buffs.rows[1].size(), g_buffs.rows[2].size());
        log(message);
        g_buffs.ready = true;
    }
    // Permanent buffs use effectEndurance -1, the game's own "never expires".
    constexpr Id kForeverIds[3]{buff_weapon_forever, buff_self_forever, count};
    for (int c = 0; c < 3; ++c) {
        const bool forever = kForeverIds[c] != count && value_of(kForeverIds[c]);
        const int percent = forever ? kLockedPercent : value_of(kBuffIds[c]);
        if (percent == g_buffs.applied_percent[c]) continue;
        for (std::size_t r = 0; r < g_buffs.rows[c].size(); ++r)
            game::write(g_buffs.rows[c][r] + 0x8, forever ? -1.0f : g_buffs.original[c][r] * percent / 100.0f);
        g_buffs.applied_percent[c] = percent;
    }
}

// Fast travel: MapDefaultInfoParam.enableFastTravelEventFlagId (+0x4) gates
// warping from closed maps such as legacy dungeons; 0 means always allowed.
// Riding Torrent anywhere was investigated in r121 and dropped: byte +0x31 of
// MapDefaultInfoParam is 1 on the 100 dungeon maps and 0 on open-world tiles,
// but clearing it changes nothing; RideParam lists mount types and
// PlayRegionParam multiplayer regions. The no-ride areas (cave mouths, boss
// arenas) are regions in the map files, outside the params.
struct TravelState {
    bool attempted{};
    bool ready{};
    game::ParamTable table{};
    std::vector<std::uint32_t> original{};
    // Warp prohibition SpEffects (TGA "Warp out of uncleared mini-dungeons"):
    // SpEffectParam.stateInfo (+0x156) of row 4270 is the "can't warp" state;
    // every row sharing that state is cleared while the option is on.
    std::vector<std::uintptr_t> effects{};
    std::uint16_t warp_state{};
    bool applied{};
};

TravelState g_travel{};

void find_warp_effects() noexcept {
    game::ParamTable table;
    if (!game::find_param(g_statics, L"SpEffectParam", 0x390, table, g_log)) return;
    for (std::size_t i = 0; i < table.rows.size(); ++i) {
        std::uint32_t id{};
        if (!game::read(table.file + 0x40 + i * 0x18, id) || id != 4270) continue;
        game::read(table.rows[i] + 0x156, g_travel.warp_state);
        break;
    }
    if (g_travel.warp_state) {
        for (const auto row : table.rows) {
            std::uint16_t state{};
            if (game::read(row + 0x156, state) && state == g_travel.warp_state)
                g_travel.effects.push_back(row);
        }
    }
    wchar_t message[160]{};
    swprintf_s(message, L"Fast travel: warp-block state %u on %zu SpEffectParam rows.",
        g_travel.warp_state, g_travel.effects.size());
    log(message);
}

void sync_travel() noexcept {
    if (g_travel.attempted) return;
    if (!g_travel.ready) {
        game::ParamTable table;
        if (!game::find_param(g_statics, L"MapDefaultInfoParam", 0x40, table, g_log)) {
            if (table.file) g_travel.attempted = true;
            return;
        }
        find_warp_effects();
        g_travel.table = std::move(table);
        g_travel.original.assign(g_travel.table.rows.size(), 0);
        std::size_t gated{};
        for (std::size_t r = 0; r < g_travel.table.rows.size(); ++r) {
            game::read(g_travel.table.rows[r] + 0x4, g_travel.original[r]);
            if (g_travel.original[r]) ++gated;
        }
        wchar_t message[160]{};
        swprintf_s(message, L"Fast travel: %zu/%zu MapDefaultInfoParam rows gated by a flag.",
            gated, g_travel.original.size());
        log(message);
        g_travel.ready = true;
        g_travel.applied = false;
    }
    const bool want = value_of(fast_travel_anywhere) != 0;
    if (want == g_travel.applied) return;
    for (std::size_t r = 0; r < g_travel.table.rows.size(); ++r)
        game::write<std::uint32_t>(g_travel.table.rows[r] + 0x4, want ? 0u : g_travel.original[r]);
    for (const auto row : g_travel.effects)
        game::write<std::uint16_t>(row + 0x156, want ? 0 : g_travel.warp_state);
    g_travel.applied = want;
}

// Spirit summoning: BuddyStoneParam (row 0x40). summonedEventFlagId (+0xC)
// is raised after a summon and locks that monument until the flag resets on
// rest, so 0 lets you summon again. activateRange (+0x1C, u16 metres) and
// overwriteActivateRegionEntityId (+0x20) limit where a monument enables
// summoning; the widest range and no region open it across the whole area.
// overwriteReturnRange (+0x1E, s16 metres, -1 = no override) sets how far you
// can walk off before the spirits are dismissed; Summon anywhere resets it to
// -1, as the area rules of Multiple Spirit Ashes 0.1.165 do.
// eliminateTargetEntityId (+0x8) is the boss whose death switches the monument
// off and dismisses its spirits (Church of Elleh 1042360100 -> 1042365100);
// 0 keeps both, like ClearBuddyStoneEliminateTarget in 0.1.165.
struct BuddyState {
    bool attempted{};
    bool ready{};
    game::ParamTable table{};
    std::vector<std::uint32_t> flags{};
    std::vector<std::uint32_t> eliminate_targets{};
    std::vector<std::uint16_t> ranges{};
    std::vector<std::int16_t> return_ranges{};
    std::vector<std::uint32_t> regions{};
    bool applied_no_rest{};
    bool applied_anywhere{};
    bool applied_keep_after_boss{};
};

BuddyState g_buddy{};
constexpr std::uint16_t kBuddyWideRange = 0xFFFF;

void sync_buddy() noexcept {
    if (g_buddy.attempted) return;
    if (!g_buddy.ready) {
        game::ParamTable table;
        if (!game::find_param(g_statics, L"BuddyStoneParam", 0x40, table, g_log)) {
            if (table.file) g_buddy.attempted = true;
            return;
        }
        g_buddy.table = std::move(table);
        const std::size_t rows = g_buddy.table.rows.size();
        g_buddy.flags.assign(rows, 0);
        g_buddy.eliminate_targets.assign(rows, 0);
        g_buddy.ranges.assign(rows, 0);
        g_buddy.return_ranges.assign(rows, 0);
        g_buddy.regions.assign(rows, 0);
        for (std::size_t r = 0; r < rows; ++r) {
            const auto row = g_buddy.table.rows[r];
            game::read(row + 0x8, g_buddy.eliminate_targets[r]);
            game::read(row + 0xC, g_buddy.flags[r]);
            game::read(row + 0x1C, g_buddy.ranges[r]);
            game::read(row + 0x1E, g_buddy.return_ranges[r]);
            game::read(row + 0x20, g_buddy.regions[r]);
        }
        wchar_t message[128]{};
        swprintf_s(message, L"Spirit summoning: %zu BuddyStoneParam rows.", rows);
        log(message);
        g_buddy.ready = true;
    }
    const bool no_rest = value_of(ash_no_rest) != 0;
    if (no_rest != g_buddy.applied_no_rest) {
        for (std::size_t r = 0; r < g_buddy.table.rows.size(); ++r)
            game::write<std::uint32_t>(g_buddy.table.rows[r] + 0xC, no_rest ? 0u : g_buddy.flags[r]);
        g_buddy.applied_no_rest = no_rest;
    }
    const bool anywhere = value_of(ash_anywhere) != 0;
    if (anywhere != g_buddy.applied_anywhere) {
        for (std::size_t r = 0; r < g_buddy.table.rows.size(); ++r) {
            const auto row = g_buddy.table.rows[r];
            game::write<std::uint16_t>(row + 0x1C, anywhere ? kBuddyWideRange : g_buddy.ranges[r]);
            game::write<std::int16_t>(row + 0x1E, anywhere ? std::int16_t{-1} : g_buddy.return_ranges[r]);
            game::write<std::uint32_t>(row + 0x20, anywhere ? 0u : g_buddy.regions[r]);
        }
        g_buddy.applied_anywhere = anywhere;
    }
    const bool keep_after_boss = value_of(ash_keep_after_boss) != 0;
    if (keep_after_boss != g_buddy.applied_keep_after_boss) {
        for (std::size_t r = 0; r < g_buddy.table.rows.size(); ++r)
            game::write<std::uint32_t>(g_buddy.table.rows[r] + 0x8,
                keep_after_boss ? 0u : g_buddy.eliminate_targets[r]);
        g_buddy.applied_keep_after_boss = keep_after_boss;
    }
}

// Several ashes at once: Multiple Spirit Ashes, built in from msa\ (see
// spirit_ashes.hpp). It handles summoning, dismissal and the HUD rows itself;
// the Hub starts it and passes its options and the log switch; the rest
// of its behaviour is always on while it is on.
bool g_spirit_ashes_started{};
// [SpiritAshes] HudRows in NativeTweaks.ini (not in the menu; read at startup):
// 1 = compact ash rows on the HUD (base-game package), 0 = the HUD movie and
// the friendly tags are left alone and the game's own spirit panel stays
// (compatibility package, e.g. Seamless Co-op or other HUD mods).
bool g_spirit_hud_rows{true};

spirit_ashes::Settings spirit_ash_settings() noexcept {
    return {value_of(ash_multiple) != 0, static_cast<std::uint32_t>(value_of(ash_max_active)), g_diagnostics,
        true, true, true, true, true, g_spirit_hud_rows, static_cast<std::uint32_t>(value_of(ash_extra_cost)),
        static_cast<std::uint32_t>(value_of(ash_damage_per_ash))};
}

// First thing in initialize(): its HUD movie hook has to be in before the game
// opens the HUD, so its options are read here, ahead of load_settings().
void start_spirit_ashes() noexcept {
    if (!g_config_path.empty())
        g_spirit_hud_rows = GetPrivateProfileIntW(L"SpiritAshes", L"HudRows", 1, g_config_path.c_str()) != 0;
    for (const Id id : {ash_multiple, ash_max_active, ash_extra_cost, ash_damage_per_ash}) {
        Option& option = g_options[id];
        if (!g_config_path.empty())
            option.value = clamp_value(option, static_cast<int>(GetPrivateProfileIntW(L"Vanilla", option.key,
                option.fallback, g_config_path.c_str())));
    }
    HMODULE self{};
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
        reinterpret_cast<LPCWSTR>(&start_spirit_ashes), &self);
    g_spirit_ashes_started = self && spirit_ashes::start(self, spirit_ash_settings());
    log(g_spirit_ashes_started
            ? L"Several ashes at once (Multiple Spirit Ashes 0.2.0, built in) started."
            : L"Several ashes at once unavailable: unsupported game version or MultipleSpiritAshes.dll is loaded.");
}

// Weapon effects (beta): each toggle adds an on-hit SpEffect to your attacks
// through the free AtkParam_Pc.spEffectId0-4 slots (s32 +0x18..+0x28; -1 or
// 0 is free). Attack params are read on every hit, like the damage sliders.
// The SpEffect for each status is the one the game itself uses most for that
// buildup: first from weapons (EquipParamWeapon.spEffectBehaviorId0-2, s32
// +0x44..+0x4C), else from attacks (AtkParam_Pc / AtkParam_Npc slots). Only
// rows that build up exactly one status count, and both offsets are checked
// against the real SpEffect ids before anything is written.
struct WeaponEffect { Id id; std::size_t buildup; const wchar_t* name; };
constexpr WeaponEffect kWeaponEffects[]{
    {weapon_bleed, 0xD4, L"bleed"}, {weapon_poison, 0xCC, L"poison"}, {weapon_rot, 0xD0, L"scarlet rot"},
    {weapon_frost, 0x1A8, L"frostbite"}, {weapon_sleep, 0x338, L"sleep"},
};
constexpr std::size_t kWeaponEffectCount = std::size(kWeaponEffects);
constexpr std::size_t kAtkSlots = 5;
constexpr std::size_t kAtkSlotOffset = 0x18;
constexpr std::size_t kWeaponSlots = 3;
constexpr std::size_t kWeaponSlotOffset = 0x44;

struct WeaponEffectState {
    bool attempted{};
    bool ready{};
    game::ParamTable attacks{};
    std::vector<std::int32_t> original{};  // kAtkSlots per row
    std::int32_t effect[kWeaponEffectCount]{};  // SpEffect id, 0 when not found
    game::ParamTable npc_attacks{};             // enemies, NPCs and spirit ashes
    std::vector<std::int32_t> npc_original{};
    std::uint32_t applied_mask{};
    std::uint32_t applied_npc_mask{};
    // Weapon slots (r135): with three or more effects on, the first two go to
    // the attack slots (weapons, spells and skills) and the rest to the
    // weapons' own spEffectBehaviorId0-2, so no status is applied twice.
    game::ParamTable weapons{};
    std::vector<std::int32_t> weapon_original{};  // kWeaponSlots per row
    std::uint32_t applied_weapon_mask{};
};

WeaponEffectState g_weapon_effects{};
constexpr int kAttackSlotEffects = 2;  // effects kept in the attack slots when there are more

// Sorted (id, row) pairs of SpEffectParam.
using EffectIndex = std::vector<std::pair<std::int32_t, std::uintptr_t>>;
using EffectCounts = std::vector<std::pair<std::int32_t, int>>;

std::uintptr_t effect_row(const EffectIndex& index, std::int32_t id) noexcept {
    const auto it = std::lower_bound(index.begin(), index.end(), std::make_pair(id, std::uintptr_t{}));
    return it != index.end() && it->first == id ? it->second : 0;
}

// Status built up by this SpEffect, or -1 for none or several.
int effect_status(const EffectIndex& index, std::int32_t id) noexcept {
    const auto row = id > 0 ? effect_row(index, id) : 0;
    if (!row) return -1;
    // Death blight (+0xD8) and madness (+0x33C) are not offered, but a row
    // that also builds them still counts as a mixed effect.
    for (const std::size_t other : {std::size_t{0xD8}, std::size_t{0x33C}}) {
        std::int32_t buildup{};
        if (game::read(row + other, buildup) && buildup > 0) return -1;
    }
    int found = -1;
    for (std::size_t s = 0; s < kWeaponEffectCount; ++s) {
        std::int32_t buildup{};
        if (!game::read(row + kWeaponEffects[s].buildup, buildup) || buildup <= 0) continue;
        if (found >= 0) return -1;
        found = static_cast<int>(s);
    }
    return found;
}

// Counts the slot values of a table that name a single-status SpEffect.
// Returns false when too many filled slots are not SpEffect ids at all, which
// means the offset does not match this game version.
bool count_effect_refs(const game::ParamTable& table, std::size_t offset, std::size_t slots,
    const EffectIndex& index, EffectCounts (&counts)[kWeaponEffectCount]) {
    std::size_t filled{}, valid{};
    for (const auto row : table.rows) {
        for (std::size_t k = 0; k < slots; ++k) {
            std::int32_t id{};
            if (!game::read(row + offset + k * 4, id) || id <= 0) continue;
            ++filled;
            if (!effect_row(index, id)) continue;
            ++valid;
            const int status = effect_status(index, id);
            if (status < 0) continue;
            auto& list = counts[status];
            const auto it = std::find_if(list.begin(), list.end(), [id](const auto& e) { return e.first == id; });
            if (it == list.end()) list.push_back({id, 1});
            else ++it->second;
        }
    }
    return filled > 0 && valid * 10 >= filled * 9;
}

std::int32_t most_used(const EffectCounts& list) noexcept {
    std::int32_t best{};
    int uses{};
    for (const auto& [id, n] : list)
        if (n > uses || (n == uses && id < best)) { best = id; uses = n; }
    return best;
}

std::int32_t effect_buildup(const EffectIndex& index, std::int32_t id, std::size_t s) noexcept {
    std::int32_t buildup{};
    if (const auto row = id > 0 ? effect_row(index, id) : 0)
        game::read(row + kWeaponEffects[s].buildup, buildup);
    return buildup;
}

// Each row starts from its own slots; chosen effects fill the free ones in
// menu order, so a row with few free slots gets the first effects only.
// One SpEffect builds up one status only: the buildup code (0x43E100 in
// 2.7.2.0) switches on the row's stateInfo (+0x156) and reads that status's
// column alone, so a combined row with every buildup applied just its own
// status (r135 test).
void fill_effect_slots(const game::ParamTable& table, const std::vector<std::int32_t>& original,
    std::uint32_t mask, const std::int32_t (&ids)[kWeaponEffectCount],
    std::size_t slot_offset = kAtkSlotOffset, std::size_t slot_count = kAtkSlots) noexcept {
    if (slot_count > kAtkSlots || original.size() != table.rows.size() * slot_count) return;
    for (std::size_t r = 0; r < table.rows.size(); ++r) {
        std::int32_t slots[kAtkSlots]{};
        std::memcpy(slots, &original[r * slot_count], slot_count * sizeof(std::int32_t));
        std::size_t next{};
        for (std::size_t s = 0; s < kWeaponEffectCount; ++s) {
            if (!(mask & (1u << s))) continue;
            while (next < slot_count && slots[next] > 0) ++next;
            if (next == slot_count) break;
            slots[next++] = ids[s];
        }
        for (std::size_t k = 0; k < slot_count; ++k)
            game::write(table.rows[r] + slot_offset + k * 4, slots[k]);
    }
}

void read_effect_slots(const game::ParamTable& table, std::vector<std::int32_t>& original,
    std::size_t slot_offset = kAtkSlotOffset, std::size_t slot_count = kAtkSlots) {
    original.assign(table.rows.size() * slot_count, -1);
    for (std::size_t r = 0; r < table.rows.size(); ++r)
        for (std::size_t k = 0; k < slot_count; ++k)
            game::read(table.rows[r] + slot_offset + k * 4, original[r * slot_count + k]);
}

// Splits the chosen effects: the first kAttackSlotEffects stay in the attack
// slots, the rest go to the weapon slots (only with three or more effects).
void split_effects(std::uint32_t mask, std::uint32_t& attack_mask, std::uint32_t& weapon_mask) noexcept {
    attack_mask = mask;
    weapon_mask = 0;
    int count{};
    for (std::size_t s = 0; s < kWeaponEffectCount; ++s)
        if (mask & (1u << s)) ++count;
    if (count <= kAttackSlotEffects) return;
    int kept{};
    for (std::size_t s = 0; s < kWeaponEffectCount; ++s) {
        if (!(mask & (1u << s))) continue;
        if (kept < kAttackSlotEffects) {
            ++kept;
            continue;
        }
        attack_mask &= ~(1u << s);
        weapon_mask |= 1u << s;
    }
}

// Sorted (row id, row address) pairs of any param table.
EffectIndex index_rows(const game::ParamTable& table) {
    EffectIndex index;
    index.reserve(table.rows.size());
    for (std::size_t i = 0; i < table.rows.size(); ++i) {
        std::int32_t id{};
        if (game::read(table.file + 0x40 + i * 0x18, id)) index.push_back({id, table.rows[i]});
    }
    std::sort(index.begin(), index.end());
    return index;
}


void sync_weapon_effects() noexcept {
    auto& state = g_weapon_effects;
    if (state.attempted) return;
    if (!state.ready) {
        game::ParamTable effects, attacks;
        if (!game::find_param(g_statics, L"SpEffectParam", 0x390, effects, g_log)) {
            if (effects.file) state.attempted = true;
            return;
        }
        if (!game::find_param(g_statics, L"AtkParam_Pc", 0x1C8, attacks, g_log)) {
            if (attacks.file) state.attempted = true;
            return;
        }
        try {
            EffectIndex index;
            index.reserve(effects.rows.size());
            for (std::size_t i = 0; i < effects.rows.size(); ++i) {
                std::int32_t id{};
                if (game::read(effects.file + 0x40 + i * 0x18, id)) index.push_back({id, effects.rows[i]});
            }
            std::sort(index.begin(), index.end());

            EffectCounts weapon_refs[kWeaponEffectCount];
            EffectCounts attack_refs[kWeaponEffectCount];
            if (!count_effect_refs(attacks, kAtkSlotOffset, kAtkSlots, index, attack_refs)) {
                log(L"Weapon effects: AtkParam_Pc SpEffect slots not recognised; options disabled.");
                state.attempted = true;
                return;
            }
            g_atk_layout_ok = true;
            // Enemy attacks come last: their effects are made to land on the
            // player and may not apply to AI characters.
            EffectCounts npc_refs[kWeaponEffectCount];
            game::ParamTable npc_attacks;
            if (game::find_param(g_statics, L"AtkParam_Npc", 0x1C8, npc_attacks, g_log) &&
                count_effect_refs(npc_attacks, kAtkSlotOffset, kAtkSlots, index, npc_refs)) {
                state.npc_attacks = std::move(npc_attacks);
                read_effect_slots(state.npc_attacks, state.npc_original);
            }
            game::ParamTable weapons;
            if (game::find_param(g_statics, L"EquipParamWeapon", 0x298, weapons, g_log)) {
                if (count_effect_refs(weapons, kWeaponSlotOffset, kWeaponSlots, index, weapon_refs)) {
                    state.weapons = weapons;
                    read_effect_slots(state.weapons, state.weapon_original, kWeaponSlotOffset, kWeaponSlots);
                } else {
                    for (auto& list : weapon_refs) list.clear();
                    log(L"Weapon effects: EquipParamWeapon SpEffect slots not recognised; using attacks only.");
                }
            }
            for (std::size_t s = 0; s < kWeaponEffectCount; ++s) {
                // Weapons first, then player attacks (spells, skills), then enemies.
                const EffectCounts* sources[]{&weapon_refs[s], &attack_refs[s], &npc_refs[s]};
                constexpr const wchar_t* kSourceNames[]{L"weapons", L"player attacks", L"enemy attacks"};
                std::size_t source{};
                while (source + 1 < std::size(sources) && sources[source]->empty()) ++source;
                const EffectCounts& refs = *sources[source];
                state.effect[s] = most_used(refs);
                wchar_t message[200]{};
                swprintf_s(message, L"Weapon effects: %s uses SpEffect %d (buildup %d, from %s, %zu candidates).",
                    kWeaponEffects[s].name, state.effect[s], effect_buildup(index, state.effect[s], s),
                    kSourceNames[source], refs.size());
                log(message);
            }
            state.attacks = std::move(attacks);
            read_effect_slots(state.attacks, state.original);
        } catch (...) {
            state.attempted = true;
            return;
        }
        state.ready = true;
        state.applied_mask = 0;
        state.applied_npc_mask = 0;
        state.applied_weapon_mask = 0;
    }
    std::uint32_t mask{};
    for (std::size_t s = 0; s < kWeaponEffectCount; ++s)
        if (value_of(kWeaponEffects[s].id) && state.effect[s]) mask |= 1u << s;
    const std::uint32_t npc_mask = value_of(weapon_effects_enemies) ? mask : 0;
    std::uint32_t attack_mask = mask, weapon_mask = 0;
    if (!state.weapon_original.empty()) split_effects(mask, attack_mask, weapon_mask);
    if (attack_mask != state.applied_mask) fill_effect_slots(state.attacks, state.original, attack_mask, state.effect);
    if (weapon_mask != state.applied_weapon_mask) {
        fill_effect_slots(state.weapons, state.weapon_original, weapon_mask, state.effect, kWeaponSlotOffset,
            kWeaponSlots);
        wchar_t message[160]{};
        swprintf_s(message, L"Weapon effects: attack slots mask %u, weapon slots mask %u.", attack_mask, weapon_mask);
        log(message);
    }
    if (npc_mask != state.applied_npc_mask)
        fill_effect_slots(state.npc_attacks, state.npc_original, npc_mask, state.effect);
    state.applied_mask = attack_mask;
    state.applied_weapon_mask = weapon_mask;
    state.applied_npc_mask = npc_mask;
}

// Enemy immunities (beta): NpcParam stores one u16 resistance per status, and
// 999 means immune. Columns found by scanning 2.7.1.0 data (share of enemies
// immune in brackets): +0x108 poison (29%), +0x10A rot (22%), +0x10C bleed
// (29%), +0x10E death blight (99%), +0x126 sleep (40%), +0x128 madness (91%),
// +0x1E0 frostbite (26%). Each column is checked against the loaded table
// before use. While the option is on, each 999 becomes that column's median,
// so the enemy can be afflicted like a typical one.
constexpr std::size_t kResistColumns[]{0x108, 0x10A, 0x10C, 0x10E, 0x126, 0x128, 0x1E0};
struct ImmunityState {
    bool attempted{};
    bool ready{};
    game::ParamTable table{};
    std::vector<std::size_t> columns{};
    std::vector<std::uint16_t> medians{};
    std::vector<std::pair<std::uintptr_t, std::size_t>> immune_cells{};  // address, column
    bool applied{};
};

ImmunityState g_immunity{};
constexpr std::uint16_t kImmune = 999;

void find_resist_columns() {
    auto& state = g_immunity;
    std::vector<std::uintptr_t> enemies;
    for (const auto row : state.table.rows) {
        std::uint32_t hp{};
        if (game::read(row + 0x24, hp) && hp > 0) enemies.push_back(row);
    }
    if (enemies.size() < 100) return;
    std::vector<std::uint16_t> values;
    for (const std::size_t offset : kResistColumns) {
        std::size_t immune{}, bad{};
        values.clear();
        for (const auto row : enemies) {
            std::uint16_t v{};
            if (!game::read(row + offset, v) || v == 0 || v > kImmune) { ++bad; continue; }
            if (v == kImmune) ++immune;
            else values.push_back(v);
        }
        std::uint16_t median{100};
        if (values.size() >= 5) {
            std::nth_element(values.begin(), values.begin() + values.size() / 2, values.end());
            median = values[values.size() / 2];
        }
        // The column must hold resistances (1-999, some immune) in this game version.
        const bool ok = bad * 5 <= enemies.size() && immune * 100 >= enemies.size() * 3 &&
            median >= 20 && median <= 800;
        wchar_t message[200]{};
        swprintf_s(message, L"Enemy immunities: +0x%zX immune %zu, other %zu, invalid %zu of %zu, median %u%s.",
            offset, immune, values.size(), bad, enemies.size(), median, ok ? L"" : L" -> skipped");
        log(message);
        if (!ok) continue;
        state.columns.push_back(offset);
        state.medians.push_back(median);
    }
}

void sync_immunity() noexcept {
    auto& state = g_immunity;
    if (state.attempted) return;
    if (!state.ready) {
        game::ParamTable table;
        if (!game::find_param(g_statics, L"NpcParam", 0x2E0, table, g_log)) {
            if (table.file) state.attempted = true;
            return;
        }
        state.table = std::move(table);
        try {
            find_resist_columns();
        } catch (...) {
            state.columns.clear();
        }
        std::wstring list;
        for (std::size_t k = 0; k < state.columns.size(); ++k) {
            wchar_t item[32]{};
            swprintf_s(item, L" +0x%zX(median %u)", state.columns[k], state.medians[k]);
            list += item;
        }
        wchar_t message[400]{};
        swprintf_s(message, L"Enemy immunities: %zu NpcParam resistance columns:%s.", state.columns.size(),
            list.c_str());
        log(message);
        // Seven columns are expected; fewer than four means another layout.
        if (state.columns.size() < 4) {
            log(L"Enemy immunities: columns not recognised; option disabled.");
            state.columns.clear();
            state.attempted = true;
            return;
        }
        try {
            for (const auto row : state.table.rows)
                for (std::size_t k = 0; k < state.columns.size(); ++k) {
                    std::uint16_t v{};
                    if (game::read(row + state.columns[k], v) && v == kImmune)
                        state.immune_cells.push_back({row + state.columns[k], k});
                }
        } catch (...) {
            state.attempted = true;
            return;
        }
        wchar_t cells[120]{};
        swprintf_s(cells, L"Enemy immunities: %zu immune values found.", state.immune_cells.size());
        log(cells);
        state.ready = true;
    }
    const bool want = value_of(enemies_no_immunity) != 0;
    if (want == state.applied) return;
    // Only the cells that were 999 when the table loaded are touched.
    for (const auto& [address, column] : state.immune_cells)
        game::write<std::uint16_t>(address, want ? state.medians[column] : kImmune);
    state.applied = want;
}

// No lock-on (Nexus request): NpcParam lockDist (u8 at +0x135) is how far away,
// in metres, a character can be locked on; 0 makes it impossible, as the "No
// Lock On" regulation edit did. +0x135 sits between the resistance columns
// above, whose offsets match the Paramdex layout; the column is still checked
// against the loaded table before use.
// r135 in game: that alone does not stop the player. The game copies it into
// each character's lock points when they are created (0x49D9C8 in 2.7.2.0;
// points on dummy poly 228 read a second u8, +0x145), and the lock-on update
// adds the lock manager's own reach (+0x2924, and +0x2930 = 15 m on one
// path) to it, so close targets stayed lockable. Both columns are still
// cleared, and the range hooks below zero the reach while the option is on.
constexpr std::size_t kLockDistOffset = 0x135;
constexpr std::size_t kPointLockDistOffset = 0x145;
struct LockOnState {
    bool attempted{};
    bool ready{};
    std::vector<std::pair<std::uintptr_t, std::uint8_t>> cells{};  // address, original
    bool applied{};
};

LockOnState g_lock_on{};

void sync_lock_on() noexcept {
    if (g_lock_range) g_lock_range->on = value_of(enemies_no_lock_on) ? 1u : 0u;
    auto& state = g_lock_on;
    if (state.attempted) return;
    if (!state.ready) {
        game::ParamTable table;
        if (!game::find_param(g_statics, L"NpcParam", 0x2E0, table, g_log)) {
            if (table.file) state.attempted = true;
            return;
        }
        std::size_t enemies{};
        std::vector<std::uint8_t> distances;
        try {
            for (const auto row : table.rows) {
                std::uint8_t distance{};
                std::uint8_t point_distance{};
                if (game::read(row + kPointLockDistOffset, point_distance) && point_distance != 0)
                    state.cells.push_back({row + kPointLockDistOffset, point_distance});
                if (!game::read(row + kLockDistOffset, distance) || distance == 0) continue;
                state.cells.push_back({row + kLockDistOffset, distance});
                std::uint32_t hp{};
                if (game::read(row + 0x24, hp) && hp > 0) {
                    ++enemies;
                    distances.push_back(distance);
                }
            }
        } catch (...) {
            state.cells.clear();
            state.attempted = true;
            return;
        }
        std::uint8_t median{};
        if (!distances.empty()) {
            std::nth_element(distances.begin(), distances.begin() + distances.size() / 2, distances.end());
            median = distances[distances.size() / 2];
        }
        // Lock distances are tens of metres; another layout shows up as a
        // different spread.
        const bool ok = enemies >= 100 && median >= 5 && median <= 100;
        wchar_t message[200]{};
        swprintf_s(message, L"No lock-on: NpcParam +0x%zX and +0x145, %zu lock distances (%zu enemies, median %u m)%s.",
            kLockDistOffset, state.cells.size(), enemies, median, ok ? L"" : L" -> option disabled");
        log(message);
        if (!ok) {
            state.cells.clear();
            state.attempted = true;
            return;
        }
        state.ready = true;
    }
    const bool want = value_of(enemies_no_lock_on) != 0;
    if (want == state.applied) return;
    for (const auto& [address, original] : state.cells)
        game::write<std::uint8_t>(address, want ? std::uint8_t{0} : original);
    state.applied = want;
}

// Charged attacks launch enemies (Nexus request): how a hit throws its target
// is AtkParam.dmgLevel (u8 at +0x72, enum ATKPARAM_DMGTYPE_NEW: 0 none, 1-3
// stagger, 4 big launch back, 5 push, 6 fling, 7 launch back, 8 minimum,
// 9 upward, 10 huge launch back, 11 breath); knockbackDist only slides them.
// Weapon attack rows of AtkParam_Pc end in 105/115 (one-handed charged heavy
// #1/#2) or 305/315 (two-handed), as the Smithbox row names show ("Charged
// R2-1", "Heavy #1 Max"). Rows whose level is 0 (no reaction) are left alone.
// r136 in game: on its own a launch only shows when the hit breaks the
// target's poise (a probe found no other copy of the rows: the game reads the
// table). atkBehaviorId 5 ("small stagger guarantee", u8 +0x82) makes every
// hit play it with the poise intact, and blowingCorrection (u16 +0x3C, 0 in
// every vanilla row) sets how far they fly (1000 clearly farther; knockbackDist
// changed nothing). Enemies already on the ground and large enemies and bosses
// have no launch animation. "Every attack launches" extends it to all the
// other rows of AtkParam_Pc that cause a reaction (weapons, skills, spells,
// items).
constexpr std::size_t kDmgLevelOffset = 0x72;
// By option value, 0 = off. 10 (huge launch back) went out after the r136
// test: it threw less than 4, which worked best.
constexpr std::uint8_t kLaunchLevels[]{0, 7, 4, 9};
constexpr std::size_t kBlowingOffset = 0x3C;
constexpr std::size_t kAtkBehaviorOffset = 0x82;
constexpr std::uint8_t kGuaranteedReaction = 5;
// blowingCorrection works like a threshold against NpcParam.weight (humans
// weigh about 130 kg, 95% of enemies under 970): 1000 throws farther than 0
// and 22000 threw no farther (r136 tests). Launch distance (%) lightens the
// enemies instead (see sync_charged_launch).
constexpr std::uint16_t kLaunchBlowing = 1000;
constexpr std::size_t kNpcWeightOffset = 0x1C;
struct LaunchRow {
    std::uintptr_t row;
    std::uint8_t level;      // original dmgLevel
    std::uint16_t blowing;   // original blowingCorrection
    std::uint8_t behavior;   // original atkBehaviorId
};
struct ChargedLaunchState {
    bool attempted{};
    bool ready{};
    std::vector<LaunchRow> cells{};   // charged attacks
    std::vector<LaunchRow> others{};  // every other row with a reaction
    int applied{};
    bool applied_all{};
    // Launch distance (r136): NpcParam.weight (u32 +0x1C), which characters
    // copy into their physics module (+0x300) when they are created.
    bool weights_attempted{};
    std::vector<std::pair<std::uintptr_t, std::uint32_t>> weights{};  // address, original
    int applied_weight_percent{100};
};

ChargedLaunchState g_charged_launch{};

bool charged_attack_row(std::int32_t id) noexcept {
    if (id < 100000) return false;
    const int last = id % 1000;
    return last == 105 || last == 115 || last == 305 || last == 315;
}

void sync_charged_launch() noexcept {
    auto& state = g_charged_launch;
    if (state.attempted) return;
    if (!state.ready) {
        game::ParamTable table;
        if (!game::find_param(g_statics, L"AtkParam_Pc", 0x1C8, table, g_log)) {
            if (table.file) state.attempted = true;
            return;
        }
        std::size_t levels[256]{};
        try {
            const EffectIndex index = index_rows(table);
            for (const auto& [id, row] : index) {
                std::uint8_t level{};
                if (!game::read(row + kDmgLevelOffset, level)) continue;
                std::uint16_t blowing{};
                std::uint8_t behavior{};
                game::read(row + kBlowingOffset, blowing);
                game::read(row + kAtkBehaviorOffset, behavior);
                if (!charged_attack_row(id)) {
                    if (level != 0 && level <= 11) state.others.push_back({row, level, blowing, behavior});
                    continue;
                }
                ++levels[level];
                if (level != 0) state.cells.push_back({row, level, blowing, behavior});
            }
        } catch (...) {
            state.cells.clear();
            state.others.clear();
            state.attempted = true;
            return;
        }
        // Every value must be a damage level, and charged attacks mostly
        // stagger (1-4); another layout shows up as other values.
        std::size_t total{}, valid{}, stagger{};
        for (int v = 0; v < 256; ++v) {
            total += levels[v];
            if (v <= 11) valid += levels[v];
            if (v >= 1 && v <= 4) stagger += levels[v];
        }
        const bool ok = total >= 100 && valid == total && stagger * 2 >= total;
        wchar_t message[220]{};
        swprintf_s(message, L"Charged launch: AtkParam_Pc +0x%zX, %zu charged attack rows (%zu to change; "
            L"levels 0:%zu 1:%zu 2:%zu 3:%zu 4:%zu other:%zu), %zu other rows with a reaction%ls.",
            kDmgLevelOffset, total, state.cells.size(), levels[0], levels[1], levels[2], levels[3], levels[4],
            total - levels[0] - levels[1] - levels[2] - levels[3] - levels[4], state.others.size(),
            ok ? L"" : L" -> option disabled");
        log(message);
        if (!ok) {
            state.cells.clear();
            state.others.clear();
            state.attempted = true;
            return;
        }
        state.ready = true;
    }
    const int want = std::clamp(static_cast<int>(value_of(charged_launch)), 0,
        static_cast<int>(std::size(kLaunchLevels)) - 1);
    const bool all = want != 0 && value_of(launch_all) != 0;
    // Launch distance: the throw is a physics impulse on the enemy's body, and
    // blowingCorrection only decides whether it happens (1000 and 22000 threw
    // alike; scaling positions instead made enemies teleport). Lighter enemies
    // should fly farther from the same impulse, so their weight is divided by
    // the option while launching is on. They take it when they are created:
    // rest at a grace.
    if (!state.weights_attempted) {
        state.weights_attempted = true;
        game::ParamTable npcs;
        if (game::find_param(g_statics, L"NpcParam", 0x2E0, npcs, g_log)) {
            std::vector<std::uint32_t> sample;
            for (const auto row : npcs.rows) {
                std::uint32_t weight{}, hp{};
                if (!game::read(row + kNpcWeightOffset, weight) || weight == 0) continue;
                state.weights.push_back({row + kNpcWeightOffset, weight});
                if (game::read(row + 0x24, hp) && hp > 0) sample.push_back(weight);
            }
            std::uint32_t median{};
            if (!sample.empty()) {
                std::nth_element(sample.begin(), sample.begin() + sample.size() / 2, sample.end());
                median = sample[sample.size() / 2];
            }
            // Humanoids weigh about 130 kg; another layout shows up as another spread.
            const bool ok = sample.size() >= 100 && median >= 30 && median <= 1000;
            wchar_t message[200]{};
            swprintf_s(message, L"Launch distance: NpcParam weight +0x%zX, %zu rows (median %u kg)%ls.",
                kNpcWeightOffset, state.weights.size(), median, ok ? L"" : L" -> option disabled");
            log(message);
            if (!ok) state.weights.clear();
        }
    }
    const int weight_percent = want ? std::clamp(static_cast<int>(value_of(launch_distance)), 100, 5000) : 100;
    if (weight_percent != state.applied_weight_percent) {
        for (const auto& [address, original] : state.weights)
            game::write<std::uint32_t>(address, (std::max)(1u, static_cast<std::uint32_t>(
                static_cast<std::uint64_t>(original) * 100 / weight_percent)));
        state.applied_weight_percent = weight_percent;
    }
    if (want == state.applied && all == state.applied_all) return;
    const auto apply = [&](const LaunchRow& r, bool on) {
        game::write<std::uint8_t>(r.row + kDmgLevelOffset, on ? kLaunchLevels[want] : r.level);
        game::write<std::uint16_t>(r.row + kBlowingOffset, on ? kLaunchBlowing : r.blowing);
        game::write<std::uint8_t>(r.row + kAtkBehaviorOffset, on ? kGuaranteedReaction : r.behavior);
    };
    for (const LaunchRow& r : state.cells) apply(r, want != 0);
    for (const LaunchRow& r : state.others) apply(r, all);
    state.applied = want;
    state.applied_all = all;
}

// Flask healing: the flask levels share one goods row and differ only in the
// SpEffect they apply, one per level (+0 to +12). Found by scanning 2.7.1.0:
// crimson 501000-501012 and 501075-501087 restore HP at +0x2F8 (-250 at +0
// to -810 at +12; negative heals), cerulean 501050-501062 restores FP at
// +0x300 (-80 to -220). A row is only touched while it still holds a heal.
struct FlaskFamily { Id id; std::int32_t first; std::int32_t last; std::size_t offset; };
constexpr FlaskFamily kFlaskFamilies[]{
    {flask_hp, 501000, 501012, 0x2F8}, {flask_hp, 501075, 501087, 0x2F8}, {flask_fp, 501050, 501062, 0x300},
};

struct FlaskState {
    bool attempted{};
    bool ready{};
    std::vector<std::uintptr_t> cells[std::size(kFlaskFamilies)]{};
    std::vector<std::int32_t> original[std::size(kFlaskFamilies)]{};
    int applied_percent[std::size(kFlaskFamilies)]{100, 100, 100};
};

FlaskState g_flasks{};

void sync_flasks() noexcept {
    auto& state = g_flasks;
    if (state.attempted) return;
    if (!state.ready) {
        game::ParamTable table;
        if (!game::find_param(g_statics, L"SpEffectParam", 0x390, table, g_log)) {
            if (table.file) state.attempted = true;
            return;
        }
        try {
            const EffectIndex index = index_rows(table);
            std::size_t found{};
            for (std::size_t f = 0; f < std::size(kFlaskFamilies); ++f) {
                const auto& family = kFlaskFamilies[f];
                for (std::int32_t id = family.first; id <= family.last; ++id) {
                    const auto row = effect_row(index, id);
                    std::int32_t value{};
                    if (!row || !game::read(row + family.offset, value) || value >= 0) continue;
                    state.cells[f].push_back(row + family.offset);
                    state.original[f].push_back(value);
                    ++found;
                }
            }
            wchar_t message[120]{};
            swprintf_s(message, L"Flask healing: %zu of 39 flask levels found.", found);
            log(message);
        } catch (...) {
            state.attempted = true;
            return;
        }
        state.ready = true;
    }
    for (std::size_t f = 0; f < std::size(kFlaskFamilies); ++f) {
        const int percent = value_of(kFlaskFamilies[f].id);
        if (percent == state.applied_percent[f]) continue;
        for (std::size_t k = 0; k < state.cells[f].size(); ++k)
            game::write<std::int32_t>(state.cells[f][k],
                static_cast<std::int32_t>(static_cast<std::int64_t>(state.original[f][k]) * percent / 100));
        state.applied_percent[f] = percent;
    }
}

// Crafting and upgrades without materials. Both name a material set, a row of
// EquipMtrlSetParam (row 0x34): materialId01-06 s32 +0x0..+0x14 and
// itemNum01-06 s8 +0x20..+0x25. The game marks an unused slot with id -1 and
// count -1, so each option empties every slot of the sets it owns.
// - Crafting: ShopLineupParam_Recipe.mtrlId (s32 +0x8). 2.7.1.0: 187 sets,
//   664 of their 1122 slots already empty; one recipe has no set (mtrlId -1).
// - Weapon upgrades: EquipParamWeapon.materialSetId (s32 +0x5C, 0 or 2200
//   somber; -1 cannot be upgraded) plus ReinforceParamWeapon.materialSetId
//   (u8 +0x56) of row reinforceTypeId (s16 +0xDA) + level; the set of level N
//   holds the materials to reach +N (2.7.1.0: 1-25 and 2201-2210).
// - Spirit ash upgrades: EquipParamGoods.reinforceMaterialId (s32 +0x88;
//   2.7.1.0: 10000-10009 and 10100-10109, grave and ghost glovewort).
// A set named by a shop trade, by armour or by the other group is left alone,
// so the two options never write the same row (no overlap in 2.7.1.0).
struct MaterialGroup {
    Id id;
    const wchar_t* name;
    bool ready{};
    std::vector<std::uintptr_t> rows{};
    std::vector<std::int32_t> ids{};    // 6 per row
    std::vector<std::int8_t> counts{};  // 6 per row
    bool applied{};
};

MaterialGroup g_craft{craft_free, L"Crafting"};
MaterialGroup g_upgrade{upgrade_free, L"Upgrades"};
bool g_materials_attempted{};
constexpr std::size_t kMaterialSlots = 6;
constexpr std::int32_t kMaxReinforceLevel = 25;

// Ids found at +offset of every row of a table (set ids above 0 only).
std::vector<std::int32_t> referenced_ids(const game::ParamTable& table, std::size_t offset) {
    std::vector<std::int32_t> ids;
    for (const auto row : table.rows) {
        std::int32_t id{};
        if (game::read(row + offset, id) && id > 0) ids.push_back(id);
    }
    std::sort(ids.begin(), ids.end());
    ids.erase(std::unique(ids.begin(), ids.end()), ids.end());
    return ids;
}

std::vector<std::int32_t> upgrade_sets(const game::ParamTable& weapons, const game::ParamTable& reinforce,
    const game::ParamTable& goods) {
    const EffectIndex levels = index_rows(reinforce);
    std::vector<std::int32_t> sets = referenced_ids(goods, 0x88);
    for (const auto row : weapons.rows) {
        std::int32_t base{};
        std::int16_t type{};
        if (!game::read(row + 0x5C, base) || base < 0 || !game::read(row + 0xDA, type) || type < 0) continue;
        for (std::int32_t level = 1; level <= kMaxReinforceLevel; ++level) {
            const auto level_row = effect_row(levels, type + level);
            std::uint8_t offset{};
            if (level_row && game::read(level_row + 0x56, offset)) sets.push_back(base + offset);
        }
    }
    std::sort(sets.begin(), sets.end());
    sets.erase(std::unique(sets.begin(), sets.end()), sets.end());
    return sets;
}

// Fills a group with the rows of `wanted` that no other user names.
void collect_group(MaterialGroup& group, const std::vector<std::int32_t>& wanted,
    const std::vector<std::int32_t>& excluded, const EffectIndex& sets) {
    std::size_t skipped{}, odd{}, absent{}, filled{};
    for (const std::int32_t id : wanted) {
        if (std::binary_search(excluded.begin(), excluded.end(), id)) { ++skipped; continue; }
        const auto row = effect_row(sets, id);
        if (!row) { ++absent; continue; }  // e.g. the level after the last one
        std::int32_t ids[kMaterialSlots]{};
        std::int8_t counts[kMaterialSlots]{};
        bool plausible = true;
        std::size_t used{};
        for (std::size_t k = 0; k < kMaterialSlots; ++k) {
            game::read(row + k * 4, ids[k]);
            game::read(row + 0x20 + k, counts[k]);
            // A slot is either empty (-1 / -1) or an item with a count of 1-99.
            if (ids[k] == -1 ? counts[k] != -1 : (ids[k] <= 0 || counts[k] < 1 || counts[k] > 99))
                plausible = false;
            if (ids[k] != -1) ++used;
        }
        if (!plausible) { ++odd; continue; }
        if (!used) continue;  // already free
        filled += used;
        group.rows.push_back(row);
        group.ids.insert(group.ids.end(), ids, ids + kMaterialSlots);
        group.counts.insert(group.counts.end(), counts, counts + kMaterialSlots);
    }
    wchar_t message[200]{};
    swprintf_s(message, L"%s: %zu of %zu material sets (%zu materials), %zu shared, %zu absent, %zu unexpected.",
        group.name, group.rows.size(), wanted.size(), filled, skipped, absent, odd);
    log(message);
    // More than a few unexpected sets means another layout.
    if (group.rows.empty() || odd > wanted.size() / 10) {
        swprintf_s(message, L"%s: material sets not recognised; option disabled.", group.name);
        log(message);
        group.rows.clear();
        return;
    }
    group.ready = true;
}

void apply_group(MaterialGroup& group) noexcept {
    if (!group.ready) return;
    const bool want = value_of(group.id) != 0;
    if (want == group.applied) return;
    for (std::size_t r = 0; r < group.rows.size(); ++r)
        for (std::size_t k = 0; k < kMaterialSlots; ++k) {
            game::write<std::int32_t>(group.rows[r] + k * 4, want ? -1 : group.ids[r * kMaterialSlots + k]);
            game::write<std::int8_t>(group.rows[r] + 0x20 + k,
                want ? std::int8_t{-1} : group.counts[r * kMaterialSlots + k]);
        }
    group.applied = want;
    wchar_t message[120]{};
    swprintf_s(message, L"%s: materials %s on %zu sets.", group.name, want ? L"removed" : L"restored",
        group.rows.size());
    log(message);
}

void sync_materials() noexcept {
    if (!g_materials_attempted) {
        constexpr struct { const wchar_t* name; std::size_t size; } kTables[]{
            {L"EquipMtrlSetParam", 0x34}, {L"ShopLineupParam_Recipe", 0x34}, {L"ShopLineupParam", 0x34},
            {L"EquipParamWeapon", 0x298}, {L"ReinforceParamWeapon", 0x80}, {L"EquipParamGoods", 0xB0},
            {L"EquipParamProtector", 0x1A0}};
        game::ParamTable tables[std::size(kTables)];
        for (std::size_t t = 0; t < std::size(kTables); ++t) {
            if (game::find_param(g_statics, kTables[t].name, kTables[t].size, tables[t], g_log)) continue;
            if (!tables[t].file) return;  // not loaded yet: retry
            log(L"Crafting / upgrades: a material table has another layout; options disabled.");
            g_materials_attempted = true;
            return;
        }
        g_materials_attempted = true;
        try {
            const auto& [sets, recipes, shops, weapons, reinforce, goods, armour] = tables;
            const auto craft = referenced_ids(recipes, 0x8);
            const auto upgrade = upgrade_sets(weapons, reinforce, goods);
            auto others = referenced_ids(shops, 0x8);
            const auto armour_sets = referenced_ids(armour, 0x34);
            others.insert(others.end(), armour_sets.begin(), armour_sets.end());
            auto not_craft = others, not_upgrade = others;
            not_craft.insert(not_craft.end(), upgrade.begin(), upgrade.end());
            not_upgrade.insert(not_upgrade.end(), craft.begin(), craft.end());
            std::sort(not_craft.begin(), not_craft.end());
            std::sort(not_upgrade.begin(), not_upgrade.end());
            const EffectIndex index = index_rows(sets);
            collect_group(g_craft, craft, not_craft, index);
            collect_group(g_upgrade, upgrade, not_upgrade, index);
        } catch (...) {
            g_craft = MaterialGroup{craft_free, L"Crafting"};
            g_upgrade = MaterialGroup{upgrade_free, L"Upgrades"};
            return;
        }
    }
    apply_group(g_craft);
    apply_group(g_upgrade);
}

// Hide helmet: head pieces (EquipParamProtector, row 0x1A0, headEquip bit 1
// of +0xDC) take the looks of the bare head, row 10000: equipModelId u16
// +0xA4 (0 = no helmet), equipModelGender u8 +0xD5, useFaceScale (bit 5 of
// +0xDC) and invisibleFlag_SexVer00-95 u8 +0x132..+0x191 (face and hair parts
// a helmet hides). Defence, weight and effects are other fields and keep
// their values. Only pieces with a head model (equipModelCategory u8 +0xD4
// = 5, like the bare head) are changed; the 11 head rows of category 2 / 6
// in 2.7.1.0 are full-body outfits. The game reads these on equip or load.
constexpr std::int32_t kBareHeadId = 10000;
constexpr std::uint8_t kHeadModelCategory = 5;
constexpr std::size_t kInvisibleOffset = 0x132;
constexpr std::size_t kInvisibleCount = 0x60;
constexpr std::size_t kHelmetBytes = kInvisibleCount + 4;  // flags, model (2), gender, invisible flags

struct HelmetState {
    bool attempted{};
    bool ready{};
    std::vector<std::uintptr_t> rows{};
    std::vector<std::uint8_t> original{};  // kHelmetBytes per row
    std::uint8_t bare[kHelmetBytes]{};
    bool applied{};
};

HelmetState g_helmet{};

void read_helmet(std::uintptr_t row, std::uint8_t* out) noexcept {
    std::uint16_t model{};
    game::read(row + 0xDC, out[0]);
    game::read(row + 0xA4, model);
    std::memcpy(out + 1, &model, sizeof(model));
    game::read(row + 0xD5, out[3]);
    for (std::size_t k = 0; k < kInvisibleCount; ++k) game::read(row + kInvisibleOffset + k, out[4 + k]);
}

void write_helmet(std::uintptr_t row, const std::uint8_t* looks, std::uint8_t flags) noexcept {
    std::uint16_t model{};
    std::memcpy(&model, looks + 1, sizeof(model));
    game::write<std::uint8_t>(row + 0xDC, flags);
    game::write<std::uint16_t>(row + 0xA4, model);
    game::write<std::uint8_t>(row + 0xD5, looks[3]);
    for (std::size_t k = 0; k < kInvisibleCount; ++k)
        game::write<std::uint8_t>(row + kInvisibleOffset + k, looks[4 + k]);
}

void sync_helmet() noexcept {
    auto& state = g_helmet;
    if (state.attempted) return;
    if (!state.ready) {
        game::ParamTable table;
        if (!game::find_param(g_statics, L"EquipParamProtector", 0x1A0, table, g_log)) {
            if (table.file) state.attempted = true;
            return;
        }
        try {
            const EffectIndex index = index_rows(table);
            const auto bare = effect_row(index, kBareHeadId);
            std::uint8_t category{};
            if (bare && game::read(bare + 0xD4, category)) read_helmet(bare, state.bare);
            std::uint16_t bare_model{};
            std::memcpy(&bare_model, state.bare + 1, sizeof(bare_model));
            if (category != kHeadModelCategory || !(state.bare[0] & 0x2) || bare_model != 0) {
                log(L"Hide helmet: bare head row not recognised; option disabled.");
                state.attempted = true;
                return;
            }
            std::size_t outfits{};
            for (const auto& entry : index) {
                const auto row = entry.second;
                std::uint8_t flags{}, model_category{};
                if (row == bare || !game::read(row + 0xDC, flags) || !(flags & 0x2)) continue;
                game::read(row + 0xD4, model_category);
                if (model_category != kHeadModelCategory) { ++outfits; continue; }
                state.rows.push_back(row);
                const std::size_t at = state.original.size();
                state.original.resize(at + kHelmetBytes);
                read_helmet(row, state.original.data() + at);
            }
            wchar_t message[160]{};
            swprintf_s(message, L"Hide helmet: %zu head pieces, %zu full-body outfits left alone.",
                state.rows.size(), outfits);
            log(message);
            if (state.rows.empty()) {
                state.attempted = true;
                return;
            }
        } catch (...) {
            state.rows.clear();
            state.attempted = true;
            return;
        }
        state.ready = true;
    }
    const bool want = value_of(hide_helmet) != 0;
    if (want == state.applied) return;
    for (std::size_t r = 0; r < state.rows.size(); ++r) {
        const std::uint8_t* original = state.original.data() + r * kHelmetBytes;
        // Only the useFaceScale bit of the flags byte changes.
        const auto flags = static_cast<std::uint8_t>((original[0] & ~0x20) | (state.bare[0] & 0x20));
        write_helmet(state.rows[r], want ? state.bare : original, want ? flags : original[0]);
    }
    state.applied = want;
    wchar_t message[120]{};
    swprintf_s(message, L"Hide helmet: %s on %zu head pieces.", want ? L"bare head looks" : L"restored",
        state.rows.size());
    log(message);
}

// World difficulty (user request): the enemy scaling of a New Game+ cycle in
// any cycle, without touching the save's real one. New Game+ adds two things
// on top of the area scaling every enemy has (NpcParam.spEffectID3, SpEffect
// 7000-7200):
//  - its own area effect, NpcParam.GameClearSpEffectID (s32 +0x6C; SpEffect
//    7400-7600, 2000744x in the DLC), e.g. Limgrave HP x3.4 and runes x5;
//  - from NG+2 on, ClearCountCorrectParam row <cycle> (row size 0x80, 31 f32),
//    up to NG+7: HP x1.4, attack x1.45, defence x1.3 (2.7.2.0 regulation).
// In a first playthrough the NG+ effect goes into a free resident slot
// (spEffectID4-7, +0x5C..+0x68: 0 in 4259 of the 4343 rows that have one) and
// its rates are multiplied by the ClearCountCorrectParam row. Past NG+ the game
// applies the effect itself, so only the ratio between the two rows is added;
// a target at or below the save's cycle changes nothing. Keep runes holds
// haveSoulRate at what the save's own cycle gives. Characters read their
// params when they appear: rest at a grace.
struct JourneyRate { std::size_t correct; std::size_t effect; };
constexpr JourneyRate kJourneyRates[]{
    {0x0, 0x10},    // MaxHpRate -> maxHpRate
    {0x8, 0x18},    // MaxStaminaRate -> maxStaminaRate
    {0xC, 0x48},    // PhysicsAttackRate -> physicsAttackPowerRate
    {0x20, 0x4C},   // MagicAttackRate
    {0x24, 0x50},   // FireAttackRate
    {0x28, 0x54},   // ThunderAttackRate
    {0x2C, 0x1E0},  // DarkAttackRate
    {0x30, 0x68},   // PhysicsDefenseRate -> physicsDiffenceRate
    {0x34, 0x6C},   // MagicDefenseRate
    {0x38, 0x70},   // FireDefenseRate
    {0x3C, 0x74},   // ThunderDefenseRate
    {0x40, 0x1D4},  // DarkDefenseRate
    {0x44, 0xC8},   // StaminaAttackRate
    {0x48, 0xF4},   // SoulRate -> haveSoulRate
    {0x4C, 0x108},  // PoisionResistRate -> registPoizonChangeRate
    {0x50, 0x10C},  // DiseaseResistRate (scarlet rot)
    {0x54, 0x110},  // BloodResistRate
    {0x58, 0x114},  // CurseResistRate (death blight)
    {0x5C, 0x2B4},  // FreezeResistRate
    {0x6C, 0x340},  // SleepResistRate
    {0x70, 0x344},  // MadnessResistRate
};
constexpr std::size_t kJourneyRateCount = std::size(kJourneyRates);
constexpr std::size_t kJourneySoulRate = 13;  // index of SoulRate above
constexpr std::ptrdiff_t kJourneySlots[]{0x5C, 0x60, 0x64, 0x68};
// SpEffectParam.bGameClearBonus (u8 +0x160, bit 6) is set on every NG+ effect
// and makes the game skip it in a save that has not been cleared yet (first
// in-game test: runes stayed at 64 with the effect in the slot). Cleared while
// the slots are filled.
constexpr std::ptrdiff_t kJourneyFlagsOffset = 0x160;
constexpr std::uint8_t kJourneyClearBonusBit = 0x40;
constexpr int kJourneyMax = 7;

struct JourneyState {
    bool attempted{};
    bool ready{};
    std::vector<std::uintptr_t> npc_slots{};    // free slot of each NpcParam row with an NG+ effect
    std::vector<std::int32_t> npc_effects{};    // that row's GameClearSpEffectID
    std::vector<std::int32_t> npc_original{};   // the slot's own value (0 or -1)
    std::size_t npc_without_slot{};
    std::vector<std::uintptr_t> effect_rows{};  // SpEffectParam rows of those effects
    std::vector<float> original{};              // kJourneyRateCount rates per effect row
    std::vector<std::uint8_t> flags{};          // each effect row's byte +0x160
    float correct[kJourneyMax + 1][kJourneyRateCount]{};  // ClearCountCorrectParam rows 0-7
    int applied_target{};
    int applied_cycle{};
    bool applied_keep{};
    bool slots_filled{};
    int cycle{-1};  // save's own cycle, -1 until a save is loaded
};

JourneyState g_journey{};

std::uintptr_t param_row_by_id(const game::ParamTable& table, std::uint32_t id) noexcept {
    for (std::size_t i = 0; i < table.rows.size(); ++i) {
        std::uint32_t row_id{};
        if (game::read(table.file + 0x40 + i * 0x18, row_id) && row_id == id) return table.rows[i];
    }
    return 0;
}

bool journey_wanted() noexcept { return value_of(world_journey) != 0; }

void sync_journey() noexcept {
    auto& state = g_journey;
    if (state.attempted) return;
    if (!state.ready) {
        game::ParamTable npcs, effects, correct;
        if (!game::find_param(g_statics, L"NpcParam", 0x2E0, npcs, g_log)) {
            if (npcs.file) state.attempted = true;
            return;
        }
        if (!game::find_param(g_statics, L"SpEffectParam", 0x390, effects, g_log) ||
            !game::find_param(g_statics, L"ClearCountCorrectParam", 0x80, correct, g_log)) {
            if (effects.file || correct.file) state.attempted = true;
            return;
        }
        for (int cycle = 0; cycle <= kJourneyMax; ++cycle) {
            const std::uintptr_t row = param_row_by_id(correct, static_cast<std::uint32_t>(cycle));
            for (std::size_t f = 0; f < kJourneyRateCount; ++f) {
                float rate = 1.0f;
                if (row) game::read(row + kJourneyRates[f].correct, rate);
                // Row 0 (no New Game+) is all zeros: it means no correction.
                state.correct[cycle][f] = rate > 0.0f ? rate : 1.0f;
            }
        }
        std::vector<std::int32_t> ids;
        for (const auto row : npcs.rows) {
            std::int32_t effect{};
            if (!game::read(row + 0x6C, effect) || effect <= 0) continue;
            std::uintptr_t slot{};
            for (const auto offset : kJourneySlots) {
                std::int32_t current{};
                if (game::read(row + offset, current) && current <= 0) {
                    slot = row + offset;
                    break;
                }
            }
            if (!slot) {
                ++state.npc_without_slot;
                continue;
            }
            std::int32_t original{};
            game::read(slot, original);
            state.npc_slots.push_back(slot);
            state.npc_effects.push_back(effect);
            state.npc_original.push_back(original);
            ids.push_back(effect);
        }
        std::sort(ids.begin(), ids.end());
        ids.erase(std::unique(ids.begin(), ids.end()), ids.end());
        for (const auto id : ids) {
            const std::uintptr_t row = param_row_by_id(effects, static_cast<std::uint32_t>(id));
            if (!row) continue;
            state.effect_rows.push_back(row);
            std::uint8_t flags{};
            game::read(row + kJourneyFlagsOffset, flags);
            state.flags.push_back(flags);
            for (const auto& rate : kJourneyRates) {
                float value{};
                game::read(row + rate.effect, value);
                state.original.push_back(value);
            }
        }
        wchar_t message[240]{};
        swprintf_s(message,
            L"World difficulty: %zu NpcParam rows with a New Game+ effect (%zu without a free slot), "
            L"%zu effects, NG+7 HP x%.2f.",
            state.npc_slots.size(), state.npc_without_slot, state.effect_rows.size(),
            state.correct[kJourneyMax][0]);
        log(message);
        if (state.npc_slots.empty() || state.effect_rows.empty()) {
            state.attempted = true;
            return;
        }
        state.ready = true;
    }

    // The save's own cycle; kept from the last loaded save on the title screen.
    game::SaveIdentity id{};
    if (game::save_identity(g_statics, id)) state.cycle = static_cast<int>(id.cycle);
    const int target = std::clamp(static_cast<int>(value_of(world_journey)), 0, kJourneyMax);
    const bool keep = value_of(journey_keep_runes) != 0;
    const int cycle = std::clamp(state.cycle, 0, kJourneyMax);
    // Until a save is loaded the first playthrough is assumed; the next pass
    // (every 2 s while the option is on) corrects it.
    const bool active = target > cycle;
    const bool fill = active && cycle == 0;
    if (target == state.applied_target && cycle == state.applied_cycle && keep == state.applied_keep &&
        fill == state.slots_filled) return;

    if (fill != state.slots_filled) {
        for (std::size_t r = 0; r < state.npc_slots.size(); ++r)
            game::write<std::int32_t>(state.npc_slots[r], fill ? state.npc_effects[r] : state.npc_original[r]);
        state.slots_filled = fill;
    }
    for (std::size_t e = 0; e < state.effect_rows.size(); ++e) {
        const std::uint8_t flags = state.flags[e];
        game::write<std::uint8_t>(state.effect_rows[e] + kJourneyFlagsOffset,
            fill ? static_cast<std::uint8_t>(flags & ~kJourneyClearBonusBit) : flags);
        for (std::size_t f = 0; f < kJourneyRateCount; ++f) {
            const float original = state.original[e * kJourneyRateCount + f];
            float factor = active ? state.correct[target][f] / state.correct[cycle][f] : 1.0f;
            float value = original * factor;
            if (active && keep && f == kJourneySoulRate)
                value = cycle == 0 ? 1.0f : original;  // the runes of the save's own cycle
            game::write(state.effect_rows[e] + kJourneyRates[f].effect, value);
        }
    }
    state.applied_target = target;
    state.applied_cycle = cycle;
    state.applied_keep = keep;
    wchar_t message[200]{};
    if (active)
        swprintf_s(message, L"World difficulty: NG+%d on a save in NG+%d (0 = first playthrough)%s, HP x%.2f "
            L"on top of the NG+ effects.", target, cycle, keep ? L", runes kept" : L"",
            state.correct[target][0] / state.correct[cycle][0]);
    else
        swprintf_s(message, L"World difficulty: off (target %d, save in cycle %d); params restored.",
            target, cycle);
    log(message);
}

// Keep characters clean (Nexus request): DecalParam (row 0xF8) entries with
// usePaintDecal (bit 12 of the u32 at +0x24) are painted onto the character
// that is hit: blood, mud and scorch marks that pile up on bodies (154 of 488
// rows in 2.7.2.0, 54 of them marked bloodTypeEnable). Clearing the bit keeps
// the decals on the ground and walls (useDeferredDecal, bit 11) but no longer
// on characters, enemies included.
struct CleanState {
    bool attempted{};
    bool ready{};
    std::vector<std::uintptr_t> rows{};
    bool applied{};
};

CleanState g_clean{};
constexpr std::uint32_t kPaintDecalBit = 1u << 12;

void sync_clean() noexcept {
    auto& state = g_clean;
    if (state.attempted) return;
    if (!state.ready) {
        game::ParamTable table;
        if (!game::find_param(g_statics, L"DecalParam", 0xF8, table, g_log)) {
            if (table.file) state.attempted = true;
            return;
        }
        for (const auto row : table.rows) {
            std::uint32_t bits{};
            if (game::read(row + 0x24, bits) && (bits & kPaintDecalBit)) state.rows.push_back(row);
        }
        wchar_t message[120]{};
        swprintf_s(message, L"Clean characters: %zu of %zu DecalParam rows paint onto characters.",
            state.rows.size(), table.rows.size());
        log(message);
        if (state.rows.empty()) {
            state.attempted = true;
            return;
        }
        state.ready = true;
    }
    const bool want = value_of(clean_characters) != 0;
    if (want == state.applied) return;
    for (const auto row : state.rows) {
        std::uint32_t bits{};
        if (!game::read(row + 0x24, bits)) continue;
        game::write<std::uint32_t>(row + 0x24, want ? bits & ~kPaintDecalBit : bits | kPaintDecalBit);
    }
    state.applied = want;
}

// Hide buff effects (Nexus request): the self buffs that Self buff duration
// already sorts out (g_buffs.rows[1]: 247 SpEffects in 2.7.2.0) draw their
// particles and body glow through SpEffectParam.vfxId..vfxId7 (s32 +0x170,
// +0x18C..+0x1A4) -> SpEffectVfxParam (row 0xA4, 123 rows). Their particle
// effects (midstSfxId +0x0, initSfxId +0x8, finishSfxId +0x10) and material
// glow (materialParamId +0x84) go to -1; sounds, camouflage (Unseen Form) and
// transformations stay, and the buffs keep working. Rows shared with other
// effects lose their visuals too.
struct BuffVfxState {
    bool attempted{};
    bool ready{};
    std::vector<std::uintptr_t> rows{};
    std::vector<std::int32_t> original{};  // 4 per row
    bool applied{};
};

BuffVfxState g_buff_vfx{};         // self buffs (g_buffs.rows[1])
BuffVfxState g_weapon_buff_vfx{};  // weapon buffs (g_buffs.rows[0])
constexpr std::ptrdiff_t kBuffVfxSlots[]{0x170, 0x18C, 0x190, 0x194, 0x198, 0x19C, 0x1A0, 0x1A4};
constexpr std::ptrdiff_t kBuffVfxFields[]{0x0, 0x8, 0x10, 0x84};

void sync_buff_vfx(BuffVfxState& state, int category, Id option, const wchar_t* what) noexcept {
    if (state.attempted) return;
    if (!state.ready) {
        if (g_buffs.attempted) {
            state.attempted = true;
            return;
        }
        if (!g_buffs.ready) return;
        game::ParamTable table;
        if (!game::find_param(g_statics, L"SpEffectVfxParam", 0xA4, table, g_log)) {
            if (table.file) state.attempted = true;
            return;
        }
        std::vector<std::int32_t> ids;
        for (const auto effect : g_buffs.rows[category])
            for (const auto slot : kBuffVfxSlots) {
                std::int32_t id{};
                if (game::read(effect + slot, id) && id > 0) ids.push_back(id);
            }
        std::sort(ids.begin(), ids.end());
        ids.erase(std::unique(ids.begin(), ids.end()), ids.end());
        for (const auto id : ids) {
            const std::uintptr_t row = param_row_by_id(table, static_cast<std::uint32_t>(id));
            if (!row) continue;
            state.rows.push_back(row);
            for (const auto field : kBuffVfxFields) {
                std::int32_t value{-1};
                game::read(row + field, value);
                state.original.push_back(value);
            }
        }
        wchar_t message[140]{};
        swprintf_s(message, L"Hide %s effects: %zu SpEffectVfxParam rows used by %zu of them.", what,
            state.rows.size(), g_buffs.rows[category].size());
        log(message);
        if (state.rows.empty()) {
            state.attempted = true;
            return;
        }
        state.ready = true;
    }
    const bool want = value_of(option) != 0;
    if (want == state.applied) return;
    for (std::size_t r = 0; r < state.rows.size(); ++r)
        for (std::size_t f = 0; f < std::size(kBuffVfxFields); ++f)
            game::write<std::int32_t>(state.rows[r] + kBuffVfxFields[f],
                want ? -1 : state.original[r * std::size(kBuffVfxFields) + f]);
    state.applied = want;
}

bool all_params_ready() noexcept {
    for (const auto& state : g_param_states)
        if (!state.ready && !state.attempted) return false;
    return (g_loot.ready || g_loot.attempted) && (g_travel.ready || g_travel.attempted) &&
        (g_buffs.ready || g_buffs.attempted) && (g_buddy.ready || g_buddy.attempted) &&
        (g_weapon_effects.ready || g_weapon_effects.attempted) && (g_immunity.ready || g_immunity.attempted) &&
        (g_flasks.ready || g_flasks.attempted) && g_materials_attempted &&
        (g_helmet.ready || g_helmet.attempted) && (g_lock_on.ready || g_lock_on.attempted) &&
        (g_charged_launch.ready || g_charged_launch.attempted) && (g_journey.ready || g_journey.attempted) &&
        (g_clean.ready || g_clean.attempted) && (g_buff_vfx.ready || g_buff_vfx.attempted) &&
        (g_weapon_buff_vfx.ready || g_weapon_buff_vfx.attempted);
}

DWORD WINAPI param_worker(void*) noexcept {
    perf::Loop loop(L"param");
    for (;;) {
        loop.pause();
        // World difficulty follows the loaded save's cycle, so it polls while on.
        WaitForSingleObject(g_param_event, all_params_ready() && !journey_wanted() ? INFINITE : 2000);
        loop.resume();
        // First, so it classifies SpEffects before any buildup slider rescales them.
        sync_weapon_effects();
        sync_params();
        sync_loot();
        sync_travel();
        sync_buffs();
        sync_buddy();
        sync_immunity();
        sync_lock_on();
        sync_charged_launch();
        sync_journey();
        sync_clean();
        sync_buff_vfx(g_buff_vfx, 1, hide_buff_effects, L"self buff");
        sync_buff_vfx(g_weapon_buff_vfx, 0, hide_weapon_buff_effects, L"weapon buff");
        sync_flasks();
        sync_materials();
        sync_helmet();
    }
}

// Animation-speed categories, matched on the animation id without its
// weapon-type prefix (id % 1000000). Ranges can be tuned in [VanillaAnimations].
struct AnimRange { int first; int last; };
constexpr std::size_t kAnimCategories = 6;
// Item use (flasks, consumables, picking up: keys 50000-59999 in 2.7.1.0) is not
// in the generated table, so it always uses its ranges, after the table lookup.
constexpr std::size_t kItemCategory = 5;
std::vector<AnimRange> g_ranges[kAnimCategories];  // attack, spell, roll, move, jump, item
constexpr Id kSpeedIds[kAnimCategories]{speed_attack, speed_spell, speed_roll, speed_move, speed_jump, speed_item};

void parse_ranges(const wchar_t* key, const wchar_t* fallback, std::vector<AnimRange>& out) {
    wchar_t text[256]{};
    GetPrivateProfileStringW(L"VanillaAnimations", key, fallback, text,
        static_cast<DWORD>(std::size(text)), g_config_path.c_str());
    wchar_t* context{};
    for (wchar_t* token = wcstok_s(text, L",", &context); token;
         token = wcstok_s(nullptr, L",", &context)) {
        int first{}, last{};
        if (swscanf_s(token, L" %d - %d", &first, &last) == 2 && first <= last) out.push_back({first, last});
        else if (swscanf_s(token, L" %d", &first) == 1) out.push_back({first, first});
    }
}

// Table generated from chr/c0000.anibnd by tools/AnimExtractor: "<key> <category>" per
// line, category 0-4 as in g_ranges. Sorted by key. Ranges set in the INI win over it.
std::vector<std::pair<int, std::uint8_t>> g_anim_table;
bool g_range_override[kAnimCategories]{};

struct BuiltinAnim {
    int key;
    std::uint8_t category;
};
constexpr BuiltinAnim kBuiltinAnims[] = {
#include "animation_table.inc"
};

// The table is built in; a NativeTweaks.animations.txt next to the DLL, if
// present, replaces it (same format) without rebuilding.
void load_anim_table() {
    std::wstring path = g_config_path.substr(0, g_config_path.find_last_of(L"\\/") + 1);
    path += L"NativeTweaks.animations.txt";
    FILE* file{};
    const bool from_file = _wfopen_s(&file, path.c_str(), L"r") == 0 && file;
    if (from_file) {
        char line[128]{};
        while (fgets(line, static_cast<int>(std::size(line)), file)) {
            int key{}, category{};
            if (line[0] != '#' && sscanf_s(line, " %d %d", &key, &category) == 2 && category >= 0 && category < static_cast<int>(kAnimCategories))
                g_anim_table.emplace_back(key, static_cast<std::uint8_t>(category));
        }
        fclose(file);
    } else {
        g_anim_table.reserve(std::size(kBuiltinAnims));
        for (const BuiltinAnim& anim : kBuiltinAnims) g_anim_table.emplace_back(anim.key, anim.category);
    }
    std::sort(g_anim_table.begin(), g_anim_table.end());
    if (g_log) {
        wchar_t message[160]{};
        swprintf_s(message, L"Animation table: %zu entries (%ls).", g_anim_table.size(),
            from_file ? L"NativeTweaks.animations.txt" : L"built in");
        g_log(message);
    }
}

void load_ranges() noexcept {
    try {
        constexpr const wchar_t* kKeys[kAnimCategories]{L"Attack", L"Spell", L"Roll", L"Move", L"Jump", L"Item"};
        constexpr const wchar_t* kDefaults[kAnimCategories]{
            L"30000-39999", L"40000-49999", L"27000-27999", L"20000-26999", L"28000-28999,202000-202999",
            L"50000-59999"};
        load_anim_table();
        for (std::size_t c = 0; c < kAnimCategories; ++c) {
            parse_ranges(kKeys[c], L"", g_ranges[c]);
            g_range_override[c] = !g_ranges[c].empty();
            if (!g_range_override[c] && (g_anim_table.empty() || c == kItemCategory))
                parse_ranges(kKeys[c], kDefaults[c], g_ranges[c]);
        }
        g_range_override[kItemCategory] = true;  // always by range (see kItemCategory)
    } catch (...) {
    }
}

// Returns the speed category (0-4) for an animation key, or -1.
int anim_category(int key) noexcept {
    for (std::size_t c = 0; c < kAnimCategories; ++c) {
        if (c == kItemCategory) continue;  // checked last, below
        if (!g_range_override[c] && !g_anim_table.empty()) continue;
        for (const auto& range : g_ranges[c])
            if (key >= range.first && key <= range.last) return static_cast<int>(c);
    }
    const auto it = std::lower_bound(g_anim_table.begin(), g_anim_table.end(), std::pair<int, std::uint8_t>{key, std::uint8_t{}});
    if (it != g_anim_table.end() && it->first == key && !g_range_override[it->second]) return it->second;
    // Item use comes after the table, so its few spell / roll keys keep their category.
    for (const auto& range : g_ranges[kItemCategory])
        if (key >= range.first && key <= range.last) return static_cast<int>(kItemCategory);
    return -1;
}

// Animation ids of a category for the menu help, merged into ranges of whole
// thousands ("30000-39999, 45000-48999"). Long lists are cut with an ellipsis.
std::wstring describe_ids(std::size_t category) {
    std::vector<AnimRange> blocks;
    const auto add = [&](int first, int last) {
        for (int b = first / 1000; b <= last / 1000; ++b)
            if (blocks.empty() || blocks.back().last < b) blocks.push_back({b, b});
    };
    if (g_range_override[category] || g_anim_table.empty()) {
        for (const auto& range : g_ranges[category]) add(range.first, range.last);
    } else {
        for (const auto& [key, c] : g_anim_table)
            if (c == category) add(key, key);
    }
    std::sort(blocks.begin(), blocks.end(), [](const AnimRange& a, const AnimRange& b) { return a.first < b.first; });
    std::wstring text;
    for (std::size_t i = 0; i < blocks.size();) {
        std::size_t j = i;
        while (j + 1 < blocks.size() && blocks[j + 1].first == blocks[j].last + 1) ++j;
        wchar_t part[32]{};
        swprintf_s(part, L"%ls%d-%d", text.empty() ? L"" : L", ", blocks[i].first * 1000, blocks[j].last * 1000 + 999);
        if (text.size() + wcslen(part) > 120) {
            text += L", ...";
            break;
        }
        text += part;
        i = j + 1;
    }
    return text;
}

bool speed_active() noexcept {
    if (value_of(speed_global) != 100) return true;
    for (const Id id : kSpeedIds)
        if (value_of(id) != 100) return true;
    return false;
}

// Diagnostics for the in-game test: one line per animation change while a
// per-action slider is set, capped so a long session cannot flood the log.
constexpr int kSpeedLogLines = 300;

void speed_log(std::int32_t anim, int category, float target) noexcept {
    static int lines{};
    if (!g_log || !g_diagnostics || lines >= kSpeedLogLines) return;
    constexpr const wchar_t* kNames[kAnimCategories]{L"attack", L"spell", L"roll", L"move", L"jump", L"item"};
    wchar_t message[160]{};
    swprintf_s(message, L"Speed: animation a%03d_%06d -> %ls, speed x%.2f%ls", anim / 1000000, anim % 1000000,
        category >= 0 ? kNames[category] : L"no category", target,
        ++lines == kSpeedLogLines ? L" (log limit reached)" : L"");
    g_log(message);
}

DWORD WINAPI speed_worker(void*) noexcept {
    bool touched{};
    std::int32_t logged_anim{-1};
    perf::Loop loop(L"speed");
    for (;;) {
        // 2 ms stays below one frame even at 360 FPS while halving the wakeups.
        loop.sleep(2);
        const bool active = speed_active();
        if (!active && !touched) {
            idle_wait(loop);
            continue;
        }
        const auto speed_address = game::player_anim_speed(g_statics);
        if (!speed_address) {
            loop.sleep(50);  // no player (menus, loading screens): poll slowly
            continue;
        }
        if (!active) {
            game::write(speed_address, 1.0f);
            touched = false;
            continue;
        }
        float percent = static_cast<float>(value_of(speed_global));
        // The animation id is only needed when a per-action slider is set.
        bool per_action{};
        for (const Id id : kSpeedIds) per_action |= value_of(id) != 100;
        if (per_action) {
            std::int32_t anim{-1};
            if (const auto anim_address = game::player_anim_id(g_statics)) game::read(anim_address, anim);
            const int key = anim >= 0 ? anim % 1000000 : -1;
            const int c = key >= 0 ? anim_category(key) : -1;
            if (c >= 0) percent = percent * value_of(kSpeedIds[c]) / 100.0f;
            if (anim != logged_anim && anim >= 0) {
                logged_anim = anim;
                speed_log(anim, c, percent / 100.0f);
            }
        }
        const float target = percent / 100.0f;
        float current{};
        if (game::read(speed_address, current) && current != target) game::write(speed_address, target);
        touched = true;
    }
}

// Every loaded character except the player gets the enemy speed. The game
// resets the field on some animations, so it is reasserted every 50 ms.
struct EnemySpeedPass { std::uintptr_t player; std::uintptr_t torrent_speed; float target; };

void set_enemy_speed(std::uintptr_t chr, std::uintptr_t speed, void* context) noexcept {
    const auto* pass = static_cast<const EnemySpeedPass*>(context);
    if (chr == pass->player || speed == pass->torrent_speed) return;
    float current{};
    if (game::read(speed, current) && current != pass->target) game::write(speed, pass->target);
}

// Active Torrent's animation speed (practice tool layout, 2.7.1.0): the byte at
// [[PlayerIns+0x190]+0]+0x7F names the WorldChrMan group holding the player's
// Torrent; the static Torrent slot at +0x1CC90 is the fallback. Faster
// animations also move Torrent faster, as with the player.
// Returns the address that holds the active Torrent's ChrIns pointer, so its
// modules are reached with game::chain(holder, {kModules, ...}).
// The group byte reads 255 in 2.7.1.0 and the static slot only holds Torrent
// for a few seconds after summoning it (r123 log), so enemy_speed_worker
// remembers the instance it saw there while it stays loaded.
// How Torrent was found, for the diagnostic log.
struct TorrentProbe {
    int source{};  // 0 not found, 1 player's group, 2 static fallback slot, 3 remembered instance
    bool group_matched{};  // the group exists but its holder failed the check
    std::uint8_t group{};
    std::int32_t count{-1};
    std::uintptr_t slot_chr{};  // what the static slot held (0 = empty)
};

std::uintptr_t torrent_holder(TorrentProbe* probe = nullptr) noexcept {
    TorrentProbe local{};
    TorrentProbe& p = probe ? *probe : local;
    p = {};
    const auto base = g_statics.world_chr_man;
    const auto plausible = [](std::uintptr_t holder) noexcept {
        float value{};
        const auto speed = holder ? game::chain(holder, {game::kModules, 0x28, 0x17C8}) : 0;
        return speed && game::read(speed, value) && value >= 0.01f && value <= 100.0f;
    };
    const auto group_at = game::chain(base, {game::kPlayerIns, game::kModules, 0, 0x7F});
    const auto count_at = game::chain(base, {0x10ED8});
    if (group_at && count_at && game::read(group_at, p.group) && game::read(count_at, p.count) &&
        p.count >= 0 && p.count <= 0x1000) {
        for (std::int32_t i = 0; i < p.count; ++i) {
            const std::ptrdiff_t slot = 0x10DC8 + static_cast<std::ptrdiff_t>(i) * 8;
            std::uint8_t id{};
            const auto id_at = game::chain(base, {slot, 0x08, 0x0C});
            if (!id_at || !game::read(id_at, id) || id != p.group) continue;
            p.group_matched = true;
            const auto holder = game::chain(base, {slot, 0x08, 0x28});
            if (plausible(holder)) {
                p.source = 1;
                return holder;
            }
            break;
        }
    }
    const auto fallback = game::chain(base, {0x1CC90, 0x18, 0});
    if (fallback) game::read(fallback, p.slot_chr);
    if (!plausible(fallback)) return 0;
    p.source = 2;
    return fallback;
}

std::uintptr_t torrent_speed_address(TorrentProbe* probe = nullptr, std::uintptr_t* chr = nullptr) noexcept {
    const auto holder = torrent_holder(probe);
    if (chr) {
        *chr = 0;
        if (holder) game::read(holder, *chr);
    }
    return holder ? game::chain(holder, {game::kModules, 0x28, 0x17C8}) : 0;
}

// Finds a remembered character in the loaded-character list (its speed field,
// or 0 once it has been unloaded).
struct ChrLookup { std::uintptr_t chr; std::uintptr_t speed; };

void find_chr(std::uintptr_t chr, std::uintptr_t speed, void* context) noexcept {
    auto* lookup = static_cast<ChrLookup*>(context);
    if (chr == lookup->chr) lookup->speed = speed;
}

// Diagnostic log for Torrent speed: how Torrent was found, slow applies
// (250 ms or more after the change) and how often the game resets the value.
struct TorrentSpeedLog {
    int last_percent{100};
    DWORD changed_at{};
    bool pending{};  // option changed, value not yet confirmed on Torrent
    std::uintptr_t last_address{};
    int last_source{-1};
    int resets{};
    DWORD window_start{};

    void option(int percent) noexcept {
        if (percent == last_percent) return;
        last_percent = percent;
        changed_at = GetTickCount();
        pending = true;
    }

    void located(std::uintptr_t address, const TorrentProbe& probe) noexcept {
        if (address == last_address && probe.source == last_source) return;
        wchar_t message[200]{};
        const wchar_t* slot = probe.slot_chr ? L"holds a character" : L"empty";
        if (!address) {
            swprintf_s(message, L"Torrent speed: Torrent not found (group %u, %d groups, group %ls, slot %ls).",
                probe.group, probe.count, probe.group_matched ? L"matched but implausible" : L"not matched", slot);
        } else {
            const wchar_t* via = probe.source == 1 ? L"player's group"
                : probe.source == 2 ? L"static fallback slot" : L"remembered instance (still loaded)";
            swprintf_s(message, L"Torrent speed: Torrent found via %ls (group %u, %d groups, slot %ls)%ls.", via,
                probe.group, probe.count, slot, last_address && address != last_address ? L", new instance" : L"");
        }
        log(message);
        last_address = address;
        last_source = probe.source;
    }

    void written(float before, float target, bool wrote) noexcept {
        const DWORD now = GetTickCount();
        if (pending) {
            pending = false;
            if (now - changed_at < 250) return;
            wchar_t message[160]{};
            swprintf_s(message, L"Torrent speed: %.2f on Torrent %lu ms after the change (was %.2f).", target,
                now - changed_at, before);
            log(message);
        } else if (wrote) {
            ++resets;
        }
        if (!window_start) window_start = now;
        if (now - window_start >= 10000) {
            if (resets) {
                wchar_t message[160]{};
                swprintf_s(message, L"Torrent speed: the game reset the value %d time(s) in %lu ms.", resets,
                    now - window_start);
                log(message);
            }
            resets = 0;
            window_start = now;
        }
    }
};

// Torrent options other than Cannot die (jump_worker applies them).
bool torrent_options_active() noexcept {
    return value_of(torrent_speed) != 100 || value_of(torrent_jump_speed) != 100 ||
        value_of(torrent_jump_height) != 100 || value_of(torrent_jump_distance) != 100;
}

// The active Torrent's ChrIns, found by enemy_speed_worker every 50 ms while a
// Torrent option is on (0 = not found), and whether jump_worker still has to
// put Torrent's speed back to 1.
std::atomic<std::uintptr_t> g_torrent_chr{};
std::atomic<bool> g_torrent_restore{};

DWORD WINAPI enemy_speed_worker(void*) noexcept {
    std::ptrdiff_t vector_offset{};
    DWORD next_scan = GetTickCount();
    bool touched{};
    std::uintptr_t torrent_chr{};  // last Torrent seen in its slot
    TorrentSpeedLog torrent_log{};  // only located() is used here
    const auto chr_vector = [&]() noexcept {
        if (!vector_offset && static_cast<LONG>(GetTickCount() - next_scan) >= 0) {
            next_scan = GetTickCount() + 2000;
            vector_offset = game::find_chr_vector(g_statics, g_log);
        }
        return vector_offset;
    };
    perf::Loop loop(L"enemy_speed");
    for (;;) {
        loop.sleep(50);
        const int percent = value_of(enemy_speed);
        const bool torrent_active = torrent_options_active() || g_torrent_restore.load();
        if (percent == 100 && !touched && !torrent_active) {
            g_torrent_chr.store(0);
            idle_wait(loop);
            continue;
        }
        std::uintptr_t manager{};
        EnemySpeedPass pass{0, 0, percent / 100.0f};
        if (!game::read(g_statics.world_chr_man, manager) || !manager ||
            !game::read(manager + game::kPlayerIns, pass.player)) continue;

        // Torrent keeps its own speed, so the enemy pass skips it.
        TorrentProbe probe{};
        std::uintptr_t slot_chr{};
        pass.torrent_speed = torrent_speed_address(&probe, &slot_chr);
        if (pass.torrent_speed) {
            torrent_chr = slot_chr;
        } else if (torrent_chr && chr_vector()) {
            ChrLookup lookup{torrent_chr, 0};
            if (game::for_each_chr_speed(g_statics, vector_offset, &find_chr, &lookup)) {
                pass.torrent_speed = lookup.speed;
                if (lookup.speed) probe.source = 3;
                else torrent_chr = 0;  // unloaded: wait for the next summon
            }
        }
        if (torrent_active) torrent_log.located(pass.torrent_speed, probe);
        g_torrent_chr.store(torrent_active && pass.torrent_speed ? torrent_chr : 0);

        if (percent == 100 && !touched) continue;
        if (!chr_vector()) continue;
        if (game::for_each_chr_speed(g_statics, vector_offset, &set_enemy_speed, &pass))
            touched = percent != 100;
    }
}

// Jump height and distance. Jumps are root-motion arcs with gravity off
// (TAE flag 27): a000_202000-202030 crouch for 0.27 s, rise about 1.15 m in
// 0.3 s, carry 1.35 m standing, 3.2 m running or 5 m sprinting, and end 0.24 m
// up; the fall loop 202040 has no root motion. The physics module's per-frame
// update (eldenring.exe+0x4609C0 in 2.7.1, a vtable method) copies the frame's
// root motion to +0xD0, Y up, then FUN_140467130 turns a "no gravity" request
// at +0x1D5 into no gravity for this frame (which also keeps the fall timer at
// 0). The hook calls jump_motion for the player's or Torrent's physics module.
//
// The animation always plays at its own speed (vanilla, or the jump speed
// options). jump_motion records the vanilla root motion of the jump and, from
// the take-off, plays it back scaled by (distance, height, distance) and
// stretched in time by the square root of the height, as with gravity (500% ->
// 2.24x as long): the higher jump rises faster at first and then hangs, the
// body carries on in the fall loop while the extra flight lasts, and gravity
// stays off until the recorded arc ends. Past its end (0.24 m x height) the
// fall goes on at the same speed down to the take-off level (start_tail), and
// only then does the game's gravity take over, so landings and fall damage
// work as usual. In the air the horizontal motion keeps the vanilla speed times the
// distance option, so a higher jump also goes farther; the distance does not
// change the time (a longer jump at the same height is a faster one, like a
// sprint jump). In-game tests (r131): scaling the
// frames alone looked sped up, slowing the animation looked like slow motion.
// Like the FOV hook, the site is only located at startup and patched the first
// time a jump option is used.
struct alignas(16) JumpData {
    float player_scale[4];   // distance, height, distance, unused
    float torrent_scale[4];
    volatile std::uint64_t player_physics;   // physics module to handle, 0 = none
    volatile std::uint64_t torrent_physics;
    std::uint64_t handler;                   // jump_motion
    volatile std::uint64_t frames[2];        // frames changed (player, Torrent), for the log
    volatile std::uint64_t flights[2];       // flights finished
    volatile float airtime[2];               // last flight, take-off to the end of the arc
};

JumpData* g_jump{};
std::uint8_t* g_jump_site{};

constexpr std::uint8_t kJumpExpected[]{
    0x41, 0x0F, 0x28, 0x00,                     // movaps xmm0,[r8]
    0x0F, 0x29, 0x81, 0xD0, 0x00, 0x00, 0x00};  // movaps [rcx+0D0h],xmm0

// Player animations (id % 1000000) that carry the jump arc, and the fall loop.
bool player_jump_anim(int key) noexcept { return key >= 202000 && key <= 202030; }
// The rider's jumps, which Torrent follows: RideJump_N/F/D and their double
// jumps (122000-122031), the jump loops (122040/122041) and the spiritspring
// jumps (122050-122080). Landings (1221xx) count as running.
bool ride_jump_anim(int key) noexcept { return key >= 122000 && key <= 122099; }
// Spiritspring jumps keep their height and distance: they are set for the cliffs they serve.
bool ride_jump_start_anim(int key) noexcept { return key >= 122000 && key <= 122039; }
bool ride_jump_loop_anim(int key) noexcept { return key == 122040 || key == 122041; }

// Per-frame trace of the first higher jumps of a session, for the log.
constexpr std::size_t kTraceRows = 400;
constexpr int kTracedFlights = 3;
struct TraceRow {
    float time, u, motion_y, position_y;
    std::int32_t key;
    std::int32_t phase;  // 0 recorded arc, 1 tail
};
struct Trace {
    std::size_t count{};
    TraceRow rows[kTraceRows];
};

// One jump in flight, on the game thread (one per subject: the player, Torrent).
constexpr std::size_t kFlightSamples = 1024;  // about 2.8 s at 360 FPS
struct Flight {
    bool active{};
    bool recording{};
    int last_key{-1};
    float time{};          // since the jump animation started
    float takeoff{-1.0f};  // time of the first upward root motion, -1 before
    float u_prev{};        // recorded time played back last frame
    std::size_t count{};
    float sample_time[kFlightSamples];
    float sample[kFlightSamples][3];  // vanilla root motion summed up to sample_time
    float sum[3];
    float peak_y{};          // highest recorded point and when (vanilla time)
    float peak_time{-1.0f};
    float y{};               // vertical motion written since the jump started
    bool tail{};             // past the recorded arc, still falling to the take-off level
    float tail_v{}, tail_a{}, tail_time{};
    float carry[2]{};        // horizontal speed in the tail (per second)
    float apex_time{-1.0f};  // real time of the first frame going down
    float flat_v{};          // vertical speed of the made-up fall (m/s)
    float flat_g{};          // its acceleration, from the rise
    bool in_flat{};
    float max_up_v{};        // fastest rise so far (m/s) and when
    float max_up_time{};
    float fastest{};         // fastest fall written so far (m/s, negative)
    float across_speed[2]{}; // horizontal speed of the last frame not braking (per second)
    Trace trace;
};
Flight g_flight[2]{};

// Finished traces handed to jump_worker, which writes them to the log.
Trace g_trace_out[2]{};
std::atomic<bool> g_trace_ready[2]{};
int g_traces_left[2]{kTracedFlights, kTracedFlights};

// Called when a flight ends (arc and tail done, or landed early).
void finish_trace(int who, Flight& f) noexcept {
    if (f.trace.count == 0 || g_traces_left[who] <= 0 || g_trace_ready[who].load()) return;
    g_trace_out[who].count = f.trace.count;
    std::memcpy(g_trace_out[who].rows, f.trace.rows, f.trace.count * sizeof(TraceRow));
    --g_traces_left[who];
    g_trace_ready[who].store(true);
}

// The recorded arc ends 0.24 m x height up, still falling sqrt(height) times
// as fast as the vanilla one. Handing over to the game there restarts its
// gravity from rest and the body hung in the air (r135 test, 200%). The fall
// carries on at that speed under the arc's own acceleration (the scale is
// height in space and sqrt(height) in time, so the vanilla one) with gravity
// still off, down to the take-off level: on flat ground that is the landing.
// Usually the tail starts earlier, where the arc brakes (see jump_motion);
// this is the fallback for an arc that ends without braking.
void start_tail(Flight& f, const float* scale, float stretch) noexcept {
    const std::size_t last = f.count - 1;
    std::size_t k = last;
    while (k > 0 && f.sample_time[last] - f.sample_time[k] < 0.05f) --k;
    const float span = f.sample_time[last] - f.sample_time[k];
    float vy{}, vx{}, vz{};
    if (span > 0.0f) {
        vy = (f.sample[last][1] - f.sample[k][1]) / span;
        vx = (f.sample[last][0] - f.sample[k][0]) / span;
        vz = (f.sample[last][2] - f.sample[k][2]) / span;
    }
    f.tail = true;
    f.tail_time = 0.0f;
    f.tail_v = (std::min)(0.0f, vy * stretch);
    // Average from the apex (speed 0) to the end of the arc.
    const float fall = f.sample_time[last] - f.peak_time;
    f.tail_a = f.peak_time >= 0.0f && fall > 0.05f && vy < 0.0f ? vy / fall : -9.8f;
    // A fall already made up from the rise (Torrent) carries on as it was.
    if (f.in_flat) {
        f.tail_v = (std::min)(f.tail_v, f.flat_v);
        f.tail_a = -f.flat_g;
    }
    // In real time the vanilla horizontal speed times the distance option.
    f.carry[0] = vx * scale[0];
    f.carry[1] = vz * scale[0];
}

// Recorded vanilla position at time u (linear between samples, clamped).
void flight_at(const Flight& f, float u, float out[3]) noexcept {
    std::size_t hi = 1;
    while (hi < f.count && f.sample_time[hi] < u) ++hi;  // u only moves forward, flights are short
    if (hi >= f.count) {
        for (int i = 0; i < 3; ++i) out[i] = f.sample[f.count - 1][i];
        return;
    }
    const std::size_t lo = hi - 1;
    const float span = f.sample_time[hi] - f.sample_time[lo];
    const float a = span > 0.0f ? (std::clamp)((u - f.sample_time[lo]) / span, 0.0f, 1.0f) : 1.0f;
    for (int i = 0; i < 3; ++i) out[i] = f.sample[lo][i] + (f.sample[hi][i] - f.sample[lo][i]) * a;
}

// Called by the hook on the game thread after the original copy to +0xD0.
// frame + 8 holds this frame's delta time.
void jump_motion(std::uintptr_t physics, const float* vanilla, std::uintptr_t frame) noexcept {
    JumpData* const data = g_jump;
    if (!data) return;
    const int who = physics == data->player_physics ? 0 : 1;
    Flight& f = g_flight[who];
    const float* scale = who == 0 ? data->player_scale : data->torrent_scale;

    std::int32_t anim{-1};
    if (const auto anim_address = game::player_anim_id(g_statics)) game::read(anim_address, anim);
    const int key = anim >= 0 ? anim % 1000000 : -1;
    const bool start = who == 0 ? player_jump_anim(key) : ride_jump_start_anim(key);
    const bool loop = who == 0 ? key == 202040 : ride_jump_loop_anim(key);

    if (start && key != f.last_key) {  // a new jump (Torrent's double jump too)
        f.active = f.recording = true;
        f.time = f.u_prev = 0.0f;
        f.takeoff = -1.0f;
        f.count = 1;
        f.sample_time[0] = 0.0f;
        f.sum[0] = f.sum[1] = f.sum[2] = 0.0f;
        f.sample[0][0] = f.sample[0][1] = f.sample[0][2] = 0.0f;
        f.peak_y = 0.0f;
        f.peak_time = -1.0f;
        f.y = 0.0f;
        f.tail = false;
        f.apex_time = -1.0f;
        f.fastest = 0.0f;
        f.across_speed[0] = f.across_speed[1] = 0.0f;
        f.in_flat = false;
        f.flat_v = 0.0f;
        f.max_up_v = 0.0f;
        f.max_up_time = 0.0f;
        f.trace.count = 0;
    }
    f.last_key = key;
    if (!f.active) return;
    const auto end_flight = [&] {
        f.active = false;
        data->airtime[who] = f.takeoff < 0.0f ? 0.0f : f.time - f.takeoff;
        ++data->flights[who];
        if (scale[1] != 1.0f) finish_trace(who, f);
    };
    if (!start && !loop) {  // landed, attacked or hit: back to the game's own motion
        end_flight();
        return;
    }
    if (!start) f.recording = false;

    float dt{};
    if (!game::read(frame + 8, dt) || !(dt > 0.0f) || dt > 0.25f) dt = 1.0f / 60.0f;
    const float before = f.time;
    f.time += dt;
    if (f.recording) {
        if (f.takeoff < 0.0f && vanilla[1] > 1e-4f) {
            f.takeoff = before;
            f.y = 0.0f;  // heights count from the take-off (a Torrent trace dropped 3.6 m before it)
        }
        for (int i = 0; i < 3; ++i) f.sum[i] += vanilla[i];
        if (f.sum[1] > f.peak_y) {
            f.peak_y = f.sum[1];
            f.peak_time = f.time;
        }
        if (f.count < kFlightSamples) {
            f.sample_time[f.count] = f.time;
            for (int i = 0; i < 3; ++i) f.sample[f.count][i] = f.sum[i];
            ++f.count;
        } else {
            f.recording = false;
        }
    }

    // Before the take-off the jump plays as recorded; after it, slower by the stretch.
    const float stretch = std::sqrt((std::max)(1.0f, scale[1]));
    auto* motion = reinterpret_cast<float*>(physics + 0xD0);
    bool finished = false;
    float u = f.u_prev;
    const auto tail_step = [&] {
        f.tail_time += dt;
        f.tail_v += f.tail_a * dt;
        float down = f.tail_v * dt;
        // At the take-off level (or after 3 s, a fall off a ledge) the game takes over.
        if (f.y + down <= 0.0f || f.tail_time > 3.0f) {
            down = (std::max)(down, -f.y);
            finished = true;
        }
        motion[0] = f.carry[0] * dt;
        motion[1] = down;
        motion[2] = f.carry[1] * dt;
    };
    if (!f.tail) {
        // Playback runs slower by the stretch, except over a stretch of the
        // recording with no vertical motion at all: Torrent's jump sits still
        // for about 0.08 s at its top, which stretched to 0.17 s at 500% and
        // read as the fall stopping (r136 traces, 10-13 frames). Such a
        // stretch plays at its own speed.
        float rate = stretch;
        bool flat = false;
        if (f.takeoff < 0.0f || f.time <= f.takeoff) {
            u = f.time;
            rate = 1.0f;
        } else {
            float step = dt / stretch;
            const float recorded = f.sample_time[f.count - 1];
            if (stretch > 1.0f && f.u_prev + dt < recorded) {
                float here[3]{}, ahead[3]{};
                flight_at(f, f.u_prev, here);
                flight_at(f, f.u_prev + dt, ahead);
                if (std::fabs(ahead[1] - here[1]) < 1e-5f) {
                    step = dt;
                    rate = 1.0f;
                    flat = true;
                }
            }
            u = f.u_prev + step;
        }
        const float end = f.sample_time[f.count - 1];
        if (!f.recording && u >= end) {
            u = end;
            if (f.takeoff >= 0.0f && stretch > 1.0f && f.y > 0.0f) start_tail(f, scale, stretch);
            else finished = true;
        }
        float now[3]{}, last[3]{};
        flight_at(f, u, now);
        flight_at(f, f.u_prev, last);
        f.u_prev = u;
        // In the air the slower playback is made up for, so 100% distance keeps the
        // vanilla horizontal speed (a higher jump goes farther) and the distance
        // option multiplies that speed.
        const float across = scale[0] * rate;
        motion[0] = (now[0] - last[0]) * across;
        motion[1] = (now[1] - last[1]) * scale[1];
        motion[2] = (now[2] - last[2]) * across;
        // Torrent's jump rises fast and stops hard (about 70 m/s^2 in r136
        // traces at 500%) but falls slowly (20-30 m/s^2): floaty at the game's
        // 1.6 m, a hang in the air at 8 m. From the top on it falls with the
        // deceleration it rose with (fastest rise / time from it to the top),
        // as long as that is faster than the recorded fall.
        const float up = motion[1] / dt;
        if (who == 1 && f.takeoff >= 0.0f && stretch > 1.0f) {
            if (!f.in_flat && f.apex_time < 0.0f && up > f.max_up_v) {
                f.max_up_v = up;
                f.max_up_time = f.time;
            }
            const bool top = flat || up <= 0.0f;
            if (!f.in_flat && top && f.max_up_v > 1.0f && f.y > 0.0f) {
                f.in_flat = true;
                f.flat_v = 0.0f;
                f.flat_g = f.max_up_v / (std::max)(0.05f, f.time - f.max_up_time);
            }
            if (f.in_flat) {
                const float next = f.flat_v - f.flat_g * dt;
                if (up > next) {
                    f.flat_v = next;
                    motion[1] = next * dt;
                } else {
                    f.flat_v = up;  // the recorded fall is faster: follow it
                }
            }
        }

        // The recorded arc brakes before its end, 0.24 m x height up (r136
        // traces at 200%: from -0.15 m a frame to 0, then four frames still,
        // which read as the fall stopping). Once the fall slows below 85% of
        // its fastest speed, the tail takes over from that speed, with the
        // average acceleration since the apex and the horizontal speed it had.
        if (f.takeoff >= 0.0f && stretch > 1.0f && !finished && !f.tail) {
            const float v = motion[1] / dt;
            if (f.apex_time < 0.0f && v < 0.0f) f.apex_time = f.time;
            if (f.apex_time >= 0.0f) {
                if (v <= f.fastest) {
                    f.fastest = v;
                    f.tail_a = f.time > f.apex_time ? v / (f.time - f.apex_time + dt) : -9.8f;
                    f.across_speed[0] = motion[0] / dt;
                    f.across_speed[1] = motion[2] / dt;
                } else if (f.fastest < -0.5f && v > f.fastest * 0.85f && f.y > 0.0f) {
                    f.tail = true;
                    f.tail_time = 0.0f;
                    f.tail_v = f.fastest;
                    f.carry[0] = f.across_speed[0];
                    f.carry[1] = f.across_speed[1];
                    tail_step();
                }
            }
        }
    } else {
        tail_step();
    }
    f.y += motion[1];
    ++data->frames[who];

    if (scale[1] != 1.0f && g_traces_left[who] > 0 && f.trace.count < kTraceRows) {
        float position_y{};
        game::read(physics + 0x74, position_y);
        f.trace.rows[f.trace.count++] = {f.time, u, motion[1], position_y, key, f.tail ? 1 : 0};
    }
    if (finished) end_flight();
    else if (f.takeoff >= 0.0f && stretch > 1.0f) {
        *reinterpret_cast<volatile std::uint8_t*>(physics + 0x1D5) = 1;  // no gravity this frame
    }
}

void resolve_jump_site() noexcept {
    std::uint8_t* hit = find_unique(parse_pattern(
        "c6 81 90 00 00 00 01 41 0f 28 00 0f 29 81 d0 00 00 00 41 0f 28 48 10 0f 29 89 e0 00 00 00 "
        "41 0f 28 40 20 0f 29 81 f0 00 00 00"));
    if (!hit) {
        log(L"Vanilla site 'root motion' not found (or ambiguous); jump height and distance disabled.");
        return;
    }
    g_jump_site = hit + 7;
}

void install_jump_hook() noexcept {
    if (g_jump || !g_jump_site) return;
    std::uint8_t* site = g_jump_site;
    if (std::memcmp(site, kJumpExpected, sizeof(kJumpExpected)) != 0) {
        g_jump_site = nullptr;
        log(L"Root motion code was changed by another mod; jump height and distance disabled.");
        return;
    }
    std::uint8_t* code = alloc_near(site);
    if (!code) {
        log(L"Jump height hook: no memory near the game code; option disabled.");
        return;
    }
    auto* data = reinterpret_cast<JumpData*>(code + 256);
    for (int i = 0; i < 4; ++i) data->player_scale[i] = data->torrent_scale[i] = 1.0f;
    data->player_physics = data->torrent_physics = 0;
    data->handler = reinterpret_cast<std::uint64_t>(&jump_motion);
    for (int i = 0; i < 2; ++i) {
        data->frames[i] = data->flights[i] = 0;
        data->airtime[i] = 0.0f;
    }

    std::uint8_t* p = code;
    const auto put = [&](std::initializer_list<std::uint8_t> bytes) {
        for (auto b : bytes) *p++ = b;
    };
    const auto put_rel = [&](const volatile void* target) {
        const auto disp = static_cast<std::int32_t>(reinterpret_cast<std::intptr_t>(target) -
            (reinterpret_cast<std::intptr_t>(p) + 4));
        std::memcpy(p, &disp, 4);
        p += 4;
    };
    // rcx = physics module, r8 = this frame's root motion, rsi = frame info.
    // Up to the call at +0x460A80 only rcx and r8 are still needed (and rsi,
    // which the handler preserves); the flags, rax, rdx, r9-r11 and xmm0-5 are
    // dead, as that call clobbers them. rsp is 16-byte aligned here.
    put({0x41, 0x0F, 0x28, 0x00});                                     // movaps xmm0,[r8]
    put({0x0F, 0x29, 0x81, 0xD0, 0x00, 0x00, 0x00});                   // movaps [rcx+0D0h],xmm0
    put({0x48, 0x3B, 0x0D}); put_rel(&data->player_physics);           // cmp rcx,[player_physics]
    put({0x74, 0x00}); std::uint8_t* const to_call = p;                // je call
    put({0x48, 0x3B, 0x0D}); put_rel(&data->torrent_physics);          // cmp rcx,[torrent_physics]
    put({0x75, 0x00}); std::uint8_t* const to_resume = p;              // jne resume
    to_call[-1] = static_cast<std::uint8_t>(p - to_call);
    put({0x51});                                                       // push rcx
    put({0x41, 0x50});                                                 // push r8
    put({0x49, 0x8B, 0xD0});                                           // mov rdx,r8
    put({0x4C, 0x8B, 0xC6});                                           // mov r8,rsi
    put({0x48, 0x83, 0xEC, 0x20});                                     // sub rsp,20h
    put({0xFF, 0x15}); put_rel(&data->handler);                        // call [handler]
    put({0x48, 0x83, 0xC4, 0x20});                                     // add rsp,20h
    put({0x41, 0x58});                                                 // pop r8
    put({0x59});                                                       // pop rcx
    to_resume[-1] = static_cast<std::uint8_t>(p - to_resume);
    put({0xFF, 0x25, 0x00, 0x00, 0x00, 0x00});                         // jmp [rip]
    const auto resume = reinterpret_cast<std::uint64_t>(site + sizeof(kJumpExpected));
    std::memcpy(p, &resume, 8);
    p += 8;
    FlushInstructionCache(GetCurrentProcess(), code, 256);

    std::uint8_t patch[sizeof(kJumpExpected)]{0xE9, 0, 0, 0, 0, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90};
    const auto disp = static_cast<std::int32_t>(reinterpret_cast<std::intptr_t>(code) -
        reinterpret_cast<std::intptr_t>(site + 5));
    std::memcpy(patch + 1, &disp, 4);
    g_jump = data;  // before the patch: jump_motion reads it
    if (!write_code(site, patch, sizeof(patch))) {
        g_jump = nullptr;
        log(L"Jump height hook could not be written; option disabled.");
        return;
    }
    wchar_t message[160]{};
    swprintf_s(message, L"Jump height hook installed at eldenring.exe+0x%llX.",
        static_cast<unsigned long long>(site - reinterpret_cast<std::uint8_t*>(game::image())));
    log(message);
}

void set_scale(float* scale, int distance, int height) noexcept {
    const float d = distance / 100.0f, h = height / 100.0f;
    if (scale[0] != d) scale[0] = d;
    if (scale[1] != h) scale[1] = h;
    if (scale[2] != d) scale[2] = d;
}

// Logs the first flights of a session, so a test shows whether the hook
// reaches the player and Torrent.
struct JumpLog {
    const wchar_t* who;
    std::uint64_t flights{};
    std::uint64_t frames{};
    int lines{};

    void update(const JumpData& data, int index, int height, int distance) noexcept {
        if (data.flights[index] == flights) return;
        flights = data.flights[index];
        const std::uint64_t total = data.frames[index];
        if (lines < 20) {
            ++lines;
            wchar_t message[200]{};
            swprintf_s(message, L"Jump height: %ls jump ended, %llu frames changed (height %d%%, distance %d%%), "
                L"%.2f s in the air from take-off (%.2fx the vanilla time).", who,
                static_cast<unsigned long long>(total - frames), height, distance, data.airtime[index],
                std::sqrt((std::max)(1.0f, height / 100.0f)));
            log(message);
        }
        frames = total;
    }
};

// Writes a finished per-frame trace (first higher jumps of a session) to the log.
void log_jump_trace(int who) noexcept {
    if (!g_trace_ready[who].load()) return;
    const Trace& trace = g_trace_out[who];
    wchar_t message[160]{};
    swprintf_s(message, L"Jump trace (%ls, %zu frames): time, playback time, vertical motion, height, animation, phase (0 arc, 1 tail).",
        who == 0 ? L"player" : L"Torrent", trace.count);
    log(message);
    for (std::size_t i = 0; i < trace.count; ++i) {
        const TraceRow& row = trace.rows[i];
        swprintf_s(message, L"  %.3f %.3f %+.4f %.3f %d %d",
            row.time, row.u, row.motion_y, row.position_y, row.key, row.phase);
        log(message);
    }
    g_trace_ready[who].store(false);
}

// Torrent's speed (run or jump) and the jump targets, every 2 ms: the jump
// speed must be in place on the first frames of a jump. With one fast speed
// for everything Torrent landed before the rider's double-jump window opened
// (SpEffect 100902 from 0.4 s into a000_122000 and through the whole jump loop
// 122040, at the rider's own speed), so the jump keeps its own speed. A higher
// Torrent jump only lasts longer, which keeps that window open longer.
// enemy_speed_worker finds Torrent.
DWORD WINAPI jump_worker(void*) noexcept {
    TorrentSpeedLog torrent_log{100 * 10000 + 100};  // option() and written() only; run and jump in one number
    bool torrent_touched{};
    float last_target{-1.0f};
    JumpLog player_log{L"player"}, torrent_jump_log{L"Torrent"};
    perf::Loop loop(L"jump");
    for (;;) {
        loop.sleep(2);
        const int run = value_of(torrent_speed), jump = value_of(torrent_jump_speed);
        const int height = value_of(jump_height), distance = value_of(jump_distance);
        const int torrent_height = value_of(torrent_jump_height);
        const int torrent_distance = value_of(torrent_jump_distance);
        const bool torrent_speed_on = run != 100 || jump != 100;
        const bool player_arc = g_jump && (height != 100 || distance != 100);
        const bool torrent_arc = g_jump && (torrent_height != 100 || torrent_distance != 100);
        torrent_log.option(run * 10000 + jump);
        if (g_jump) {
            if (!player_arc) g_jump->player_physics = 0;
            if (!torrent_arc) g_jump->torrent_physics = 0;
        }
        if (!torrent_speed_on && !torrent_touched && !player_arc && !torrent_arc) {
            idle_wait(loop);
            continue;
        }

        // jump_motion follows the jump itself; here only the targets and their scales.
        if (player_arc) {
            std::uintptr_t physics{};
            if (const auto at = game::chain(g_statics.world_chr_man, {game::kPlayerIns, game::kModules, 0x68}))
                game::read(at, physics);
            set_scale(g_jump->player_scale, distance, height);
            g_jump->player_physics = physics;
            player_log.update(*g_jump, 0, height, distance);
            log_jump_trace(0);
        }

        std::uintptr_t chr = g_torrent_chr.load();
        const auto torrent_at = [&](std::initializer_list<std::ptrdiff_t> offsets) noexcept {
            return chr ? game::chain(reinterpret_cast<std::uintptr_t>(&chr), offsets) : 0;
        };
        if (torrent_arc) {
            std::uintptr_t physics{};
            if (const auto at = torrent_at({game::kModules, 0x68})) game::read(at, physics);
            set_scale(g_jump->torrent_scale, torrent_distance, torrent_height);
            g_jump->torrent_physics = physics;
            torrent_jump_log.update(*g_jump, 1, torrent_height, torrent_distance);
            log_jump_trace(1);
        }

        if (torrent_speed_on || torrent_touched) {
            std::int32_t anim{-1};
            if (const auto anim_address = game::player_anim_id(g_statics)) game::read(anim_address, anim);
            const int key = anim >= 0 ? anim % 1000000 : -1;
            // While putting the speed back, enemy_speed_worker keeps looking for Torrent.
            g_torrent_restore.store(!torrent_speed_on);
            const auto speed = torrent_at({game::kModules, 0x28, 0x17C8});
            float current{};
            if (speed && game::read(speed, current) && current >= 0.01f && current <= 100.0f) {
                const float target = (ride_jump_anim(key) ? jump : run) / 100.0f;
                const bool write = current != target;
                if (write) game::write(speed, target);
                torrent_log.written(current, target, write && target == last_target);
                last_target = target;
                torrent_touched = torrent_speed_on;
            }
            if (!torrent_touched) g_torrent_restore.store(false);
            else if (!torrent_speed_on && !player_arc && !torrent_arc) loop.sleep(48);  // Torrent not summoned
        }
        // The arcs alone only need the physics modules, which change on loads
        // and summons: the 2 ms poll is for Torrent's speeds (r131 PerfLog:
        // 12000 wakeups per 30 s with only the jump height set).
        if (!torrent_speed_on && !torrent_touched) loop.sleep(18);
    }
}

// Fall damage and fall death are both decided from the fall timer, so keeping
// it at zero makes every landing a short hop. A fast poll keeps the timer far
// below the lethal threshold between frames. Lethal landings can still kill at
// full HP (r63), so No Dead covers each fall and goes back to the user's choice
// when it ends. The fall hook blocks the landing damage itself.
DWORD WINAPI fall_worker(void*) noexcept {
    std::uintptr_t last_timer{};
    bool falling{};
    int quiet_polls{};
    std::int32_t hp_start{};
    perf::Loop loop(L"fall");
    for (;;) {
        loop.sleep(5);
        const bool protect = value_of(no_fall_damage) != 0;
        const bool torrent = protect;  // one option covers you and Torrent
        if (!protect && !torrent && !falling) {
            if (g_fall) {
                g_fall->player_on = 0;
                g_fall->torrent_on = 0;
            }
            idle_wait(loop);
            continue;
        }
        const auto timer = game::player_fall_timer(g_statics);
        if (timer != last_timer) {
            last_timer = timer;
            if (g_fall && !timer) {
                g_fall->player_fall_module = 0;
                g_fall->torrent_on = 0;  // never match "previous call" against a missing module
            }
            // A fall cut short (abyss, reload) never reaches its end below.
            if (falling) apply_flag(no_death);
            falling = false;
        }
        if (!timer) {
            loop.sleep(45);  // no player (menus, loading screens): poll slowly
            continue;
        }
        float seconds{};
        if (!game::read(timer, seconds)) continue;
        std::int32_t hp{};
        const auto points = game::player_points(g_statics);
        if (points) game::read(points, hp);

        if (seconds > 0.0f) {
            if (!falling) {
                falling = true;
                hp_start = hp;
            }
            quiet_polls = 0;
        } else if (falling && ++quiet_polls >= 60) {  // 300 ms without a non-zero timer
            falling = false;
            if (!g_fall && protect && points && hp > 0 && hp < hp_start) game::write(points, hp_start);
            apply_flag(no_death);
        }
        if (protect && seconds != 0.0f) game::write(timer, 0.0f);
        if (protect && falling && g_statics.chr_dbg_flags)
            game::write<std::uint8_t>(g_statics.chr_dbg_flags + 0x1, 1);
        if (g_fall) {
            // The hook compares rcx with the fall module, which holds the timer at +0x18.
            g_fall->player_fall_module = timer - 0x18;
            g_fall->player_on = protect ? 1u : 0u;
            g_fall->torrent_on = torrent ? 1u : 0u;
            continue;
        }
        // Without the hook, the HP is given back on the next poll.
        if (protect && falling && points && hp > 0 && hp < hp_start) game::write(points, hp_start);
    }
}

// Maximum HP / FP / stamina (%): scales the base maximum (+0xC / +0x18 / +0x24
// from the HP field; the game derives the shown maximum from it). A value the
// game writes itself (level up, loading) becomes the new baseline; back at
// 100% the baseline is written back if the field still holds ours.
struct MaximumHold {
    std::uintptr_t points{};
    std::int32_t baseline{};
    std::int32_t written{};
};
MaximumHold g_maximum[3]{};

void sync_maximum(std::uintptr_t points, int resource, int percent) noexcept {
    constexpr std::ptrdiff_t kBase[3]{0xC, 0x18, 0x24};
    auto& hold = g_maximum[resource];
    if (hold.points != points) hold = {points, 0, 0};
    std::int32_t base{};
    if (!game::read(points + kBase[resource], base) || base <= 0 || base > 1000000) return;
    if (percent == 100) {
        if (hold.written && base == hold.written && hold.baseline > 0)
            game::write(points + kBase[resource], hold.baseline);
        hold.baseline = hold.written = 0;
        return;
    }
    if (!hold.written || base != hold.written) hold.baseline = base;
    const auto target = static_cast<std::int32_t>(std::clamp<std::int64_t>(
        (static_cast<std::int64_t>(hold.baseline) * percent + 50) / 100, 1, 1000000));
    if (base != target) game::write(points + kBase[resource], target);
    hold.written = target;
}

// Regeneration adds flat points per second every 100 ms; the same loop holds
// the maximum HP / FP / stamina.
// Infinite poise without re-equipping: the player's poise module gets a huge
// extra maximum and a full gauge while the option is on (a new character
// gets a new module, so it is checked every tick). The armour param lock
// stays as before.
constexpr float kLivePoise = 1000000.0f;
struct LivePoise {
    bool written{};
    float extra{};  // the module's own extra maximum before the option
};
LivePoise g_live_poise{};

void sync_live_poise() noexcept {
    auto& state = g_live_poise;
    const bool want = value_of(player_poise_lock) != 0;
    if (g_toughness) {
        std::uintptr_t module{};
        if (want) game::read(game::chain(g_statics.world_chr_man, {game::kPlayerIns, game::kModules, 0x48}), module);
        g_toughness->player_module = module;
        g_toughness->on = module ? 1u : 0u;
    }
    if (!want && !state.written) return;
    const auto poise = game::player_poise(g_statics);
    float current{}, extra{};
    if (!poise || !game::read(poise, current) || !game::read(poise + 0x8, extra)) return;
    if (want) {
        if (extra != kLivePoise) {
            if (!state.written && extra < kLivePoise / 10) state.extra = extra;
            game::write(poise + 0x8, kLivePoise);
        }
        if (current < kLivePoise / 2) game::write(poise, kLivePoise);
        if (!state.written) log(L"Infinite poise: applied to the loaded character.");
        state.written = true;
    } else {
        // The next hit brings the gauge back under the armour maximum.
        if (extra == kLivePoise) game::write(poise + 0x8, state.extra);
        state.written = false;
        log(L"Infinite poise: restored.");
    }
}

// Invincible, for the player only: CSChrDataModule (ChrIns modules +0x0)
// +0x19B bit 1. The game's "takes no damage" check (0x437A00 in 2.7.2.0) reads
// it before the global ChrDbgFlags +0xC. A new character gets a new module, so
// it is checked every tick.
bool g_god_written{};

void sync_god_mode() noexcept {
    const bool want = value_of(god_mode) != 0;
    if (!want && !g_god_written) return;
    std::uintptr_t data{};
    std::uint8_t flags{};
    if (!game::read(game::chain(g_statics.world_chr_man, {game::kPlayerIns, game::kModules, 0x0}), data) ||
        !data || !game::read(data + 0x19B, flags))
        return;
    if (want) {
        if (!(flags & 0x2)) game::write<std::uint8_t>(data + 0x19B, static_cast<std::uint8_t>(flags | 0x2));
        if (!g_god_written) log(L"Invincible: applied to the loaded character.");
        g_god_written = true;
    } else {
        game::write<std::uint8_t>(data + 0x19B, static_cast<std::uint8_t>(flags & ~0x2));
        g_god_written = false;
        log(L"Invincible: removed.");
    }
}

DWORD WINAPI regen_worker(void*) noexcept {
    float carry[3]{};
    constexpr std::ptrdiff_t kCurrent[3]{0x0, 0x10, 0x1C};
    constexpr std::ptrdiff_t kMaximum[3]{0x4, 0x14, 0x20};
    perf::Loop loop(L"regen");
    for (;;) {
        loop.sleep(100);
        const int flat[3]{value_of(hp_regen_points), value_of(fp_regen_points), value_of(stamina_regen_points)};
        const int maximum_percent[3]{value_of(max_hp), value_of(max_fp), value_of(max_stamina)};
        // Free skills (cost 0%): the game only runs a skill's full version with
        // FP above 0, so the bar is held at 1 at least (too little for spells).
        const bool free_skills = value_of(skill_fp_cost) == 0;
        bool maximum_work = false;
        for (int i = 0; i < 3; ++i)
            if (maximum_percent[i] != 100 || g_maximum[i].written) maximum_work = true;
        sync_live_poise();
        sync_god_mode();
        if (!flat[0] && !flat[1] && !flat[2] &&
            !free_skills && !maximum_work && !value_of(player_poise_lock) && !value_of(god_mode)) {
            idle_wait(loop);
            continue;
        }
        const auto points = game::player_points(g_statics);
        if (!points) continue;
        std::int32_t hp{};
        if (!game::read(points, hp) || hp <= 0) continue;  // never revive a dead player
        for (int i = 0; i < 3; ++i) sync_maximum(points, i, maximum_percent[i]);
        if (free_skills) {
            std::int32_t fp{}, fp_max{};
            if (game::read(points + 0x10, fp) && game::read(points + 0x14, fp_max) && fp_max > 0 && fp < 1)
                game::write<std::int32_t>(points + 0x10, 1);
        }
        for (int i = 0; i < 3; ++i) {
            if (!flat[i]) continue;
            std::int32_t current{}, maximum{};
            if (!game::read(points + kCurrent[i], current) || !game::read(points + kMaximum[i], maximum)) continue;
            if (maximum <= 0 || maximum > 1000000 || current >= maximum) {
                carry[i] = 0;
                continue;
            }
            carry[i] += flat[i] / 10.0f;
            const int add = static_cast<int>(carry[i]);
            if (add == 0) continue;
            carry[i] -= add;
            game::write(points + kCurrent[i], std::min(current + add, maximum));
        }
    }
}

// Status immunity: with the hook, publishes the player's SpEffect object to
// it (it changes on every load). Without it, holds the player's seven buildup
// values at their resting value, captured when the option turns on or a new
// character loads; that misses a hit that empties a gauge at once.
DWORD WINAPI status_worker(void*) noexcept {
    std::uintptr_t held_at{};
    std::int32_t rest[7]{};
    std::uint32_t logged_blocked{};
    ULONGLONG next_blocked_log{};
    perf::Loop loop(L"status");
    for (;;) {
        loop.sleep(10);
        if (!value_of(status_immunity)) {
            if (g_status_immunity) g_status_immunity->on = 0;
            held_at = 0;
            idle_wait(loop);
            continue;
        }
        if (g_status_immunity) {
            const std::uintptr_t effects = game::player_special_effect(g_statics);
            g_status_immunity->player_effects = effects;
            g_status_immunity->on = effects ? 1u : 0u;
            const std::uint32_t blocked = g_status_immunity->blocked;
            if (blocked != logged_blocked && GetTickCount64() >= next_blocked_log) {
                wchar_t message[96]{};
                swprintf_s(message, L"Status immunity: %u buildups blocked so far.", blocked);
                log(message);
                logged_blocked = blocked;
                next_blocked_log = GetTickCount64() + 10000;
            }
            loop.sleep(90);
            continue;
        }
        const auto resist = game::player_resist(g_statics);
        if (!resist) {
            held_at = 0;
            loop.sleep(190);
            continue;
        }
        std::int32_t now[7]{};
        if (!game::safe_read(resist, now, sizeof(now))) continue;
        if (held_at != resist) {
            held_at = resist;
            std::memcpy(rest, now, sizeof(rest));
            wchar_t message[160]{};
            swprintf_s(message, L"Status immunity: holding %d %d %d %d %d %d %d.",
                rest[0], rest[1], rest[2], rest[3], rest[4], rest[5], rest[6]);
            log(message);
        }
        for (int i = 0; i < 7; ++i)
            if (now[i] != rest[i]) game::write(resist + i * 4, rest[i]);
    }
}

// Finds an FD4Singleton once reflection data is ready (about 15 s after start).
std::uintptr_t wait_for_singleton(const char* name) noexcept {
    Sleep(15000);
    for (int attempt = 0; attempt < 30; ++attempt) {
        if (const auto slot = game::find_singleton(name, attempt == 0 ? g_log : nullptr)) return slot;
        Sleep(2000);
    }
    return 0;
}

// World clock (WorldAreaTime, fromsoftware-rs layout): clock at +0x0, target
// hour/minute/second at +0x28/+0x2C/+0x30 (the game moves the clock there, as
// when resting), time passage multiplier at +0x38.
DWORD WINAPI time_worker(void*) noexcept {
    const std::uintptr_t slot = wait_for_singleton("WorldAreaTime");
    if (!slot) {
        log(L"Time of day: WorldAreaTime not found; options inactive.");
        return 0;
    }
    std::uintptr_t last_instance{};
    bool was_set{}, freeze_held{};
    int last_hour = -1;
    float saved_multiplier = 1.0f;
    perf::Loop loop(L"time");
    for (;;) {
        loop.sleep(100);
        std::uintptr_t time{};
        if (!game::read(slot, time) || !time) {
            last_instance = 0;
            freeze_held = false;
            continue;
        }
        const bool new_instance = time != last_instance;
        last_instance = time;

        const bool set_on = value_of(set_time) != 0;
        const int hour = value_of(time_hour);
        if (set_on && (!was_set || hour != last_hour || new_instance)) {
            const std::uint32_t target[3]{static_cast<std::uint32_t>(hour), 0, 0};
            game::safe_write(time + 0x28, target, sizeof(target));
        }
        was_set = set_on;
        last_hour = hour;

        const auto multiplier_at = time + 0x38;
        if (value_of(freeze_time)) {
            float current{};
            if (!game::read(multiplier_at, current)) continue;
            if (!freeze_held) {
                saved_multiplier = current > 0.0f && current < 1000.0f ? current : 1.0f;
                freeze_held = true;
            }
            if (current != 0.0f) game::write(multiplier_at, 0.0f);
        } else if (freeze_held) {
            game::write(multiplier_at, saved_multiplier);
            freeze_held = false;
        }
    }
}

// Forced weather (beta). Up to r135 the weather was written into the records
// at +0x28 every 2 s, which the game ignores (r136 dumps: the other record
// kept the old weather). Since r136 it goes through the game's own request.
DWORD WINAPI weather_worker(void*) noexcept {
    const std::uintptr_t slot = wait_for_singleton("WorldAreaWeather");
    if (!slot) {
        log(L"Weather: WorldAreaWeather not found; option inactive.");
        return 0;
    }
    // WeatherParam rows of the base table (Smithbox row names); 52, 81 and 82
    // came last so saved choices keep their weather.
    static constexpr std::uint16_t kTypes[]{0, 1, 10, 11, 20, 21, 30, 31, 40, 41, 50, 51, 60, 52, 81, 82};
    // The game's own request (EMEVD ChangeWeather calls it through
    // eldenring.exe+0x5ECFE0 in 2.7.2.0): it only stores the weather, the
    // duration and "immediate" and raises a pending flag; the next weather
    // update applies it as a forced weather (+0x94, flag +0xF9), unless the
    // area holds a fixed one (+0x21C != 0xFFFF). Weather -1 goes back to the
    // usual cycle; duration -1 keeps it. The offsets are read from the method.
    std::uint8_t* const request = find_unique(parse_pattern(
        "f3 0f 11 91 ? ? 00 00 66 89 91 ? ? 00 00 44 88 89 ? ? 00 00 c6 81 ? ? 00 00 01 c3"));
    if (!request) {
        log(L"Weather: the game's weather request was not found; option inactive.");
        return 0;
    }
    std::uint32_t at_duration{}, at_weather{}, at_immediate{}, at_pending{};
    std::memcpy(&at_duration, request + 4, 4);
    std::memcpy(&at_weather, request + 11, 4);
    std::memcpy(&at_immediate, request + 18, 4);
    std::memcpy(&at_pending, request + 24, 4);
    {
        wchar_t message[200]{};
        swprintf_s(message, L"Weather: request at eldenring.exe+0x%llX (weather +0x%X, duration +0x%X, immediate +0x%X, pending +0x%X).",
            static_cast<unsigned long long>(request - reinterpret_cast<std::uint8_t*>(game::image())),
            at_weather, at_duration, at_immediate, at_pending);
        log(message);
    }
    const auto send = [&](std::uintptr_t weather, std::uint16_t type, bool immediate) {
        game::write(weather + at_weather, type);
        game::write(weather + at_duration, -1.0f);
        game::write(weather + at_immediate, static_cast<std::uint8_t>(immediate));
        game::write(weather + at_pending, static_cast<std::uint8_t>(1));  // last: the game reads it first
    };
    // The two weather records ({area u16, weather u16, seconds u32}): one of
    // them holds the requested weather once the game has taken it.
    const auto showing = [&](std::uintptr_t weather, std::uint16_t type) {
        std::uint16_t a{}, b{};
        return game::read(weather + 0x2A, a) && game::read(weather + 0x5A, b) && (a == type || b == type);
    };
    int logs_left = 12;
    const auto report = [&](std::uintptr_t weather, const wchar_t* what, int type) {
        if (logs_left <= 0) return;
        --logs_left;
        std::uint16_t a{}, b{}, lock{}, forced{};
        std::uint8_t forced_on{};
        game::read(weather + 0x2A, a);
        game::read(weather + 0x5A, b);
        game::read(weather + 0x96, forced);
        game::read(weather + 0xF9, forced_on);
        game::read(weather + 0x21C, lock);
        wchar_t message[200]{};
        swprintf_s(message, L"Weather: %ls %d; records %u and %u, forced %u (on %u), area lock 0x%X.",
            what, type, a, b, forced, forced_on, lock);
        log(message);
    };

    perf::Loop loop(L"weather");
    int sent = -1;                 // weather requested last, -1 none
    std::uintptr_t sent_to{};      // WorldAreaWeather it went to (changes on loads)
    ULONGLONG check_at = 0;
    for (;;) {
        loop.sleep(2000);
        std::uintptr_t weather{};
        if (!game::read(slot, weather) || !weather) continue;
        if (!value_of(set_weather)) {
            if (sent >= 0) {  // back to the usual cycle, blending
                send(weather, 0xFFFF, false);
                report(weather, L"released", sent);
                sent = -1;
            }
            continue;
        }
        const std::uint16_t type = kTypes[std::clamp(static_cast<int>(value_of(weather_type)), 0, 15)];
        const ULONGLONG now = GetTickCount64();
        if (type != sent || weather != sent_to) {
            send(weather, type, true);
            report(weather, L"requested", type);
            sent = type;
            sent_to = weather;
            check_at = now + 10000;
        } else if (now >= check_at) {
            // After a load or a scripted change, ask again.
            check_at = now + 10000;
            if (!showing(weather, type)) {
                report(weather, L"not showing, requested again", type);
                send(weather, type, true);
            }
        }
    }
}

// ---------------------------------------------------------------------------
// Graphics options held by CSWindow (same bytes as the game's own menu; method
// from FreecamMod). The game re-reads them every frame, so forcing 0 turns the
// effect off completely, including the temporal anti-aliasing pass.

std::uintptr_t* g_window_slot{};

void resolve_window() noexcept {
    try {
        std::uint8_t* hit = find_unique(parse_pattern("48 8b 0d ? ? ? ? 48 85 c9 74 ? 48 83 c1 ? 48 8d 45"));
        if (!hit) {
            log(L"Vanilla site 'CSWindow' not found (or ambiguous); AA and motion blur options disabled.");
            return;
        }
        std::int32_t relative{};
        std::memcpy(&relative, hit + 3, sizeof(relative));
        g_window_slot = reinterpret_cast<std::uintptr_t*>(hit + 7 + relative);
        wchar_t message[160]{};
        swprintf_s(message, L"Vanilla site 'CSWindow' found; pointer at eldenring.exe+0x%llX.",
            static_cast<unsigned long long>(reinterpret_cast<std::uint8_t*>(g_window_slot) -
                reinterpret_cast<std::uint8_t*>(game::image())));
        log(message);
    } catch (...) {
        g_window_slot = nullptr;
    }
}

// Map debug bytes read by the grace check (practice tool "FuncCheckGraces"):
// +0 shows every map layer, +1 every grace, so the map can travel to all of
// them while the option is on. Nothing is saved; turning it off restores the
// game's own progress.
std::uint8_t* g_map_flags{};

void resolve_map_flags() noexcept {
    try {
        std::uint8_t* hit = find_unique(parse_pattern("0f b6 0d ? ? ? ? e8 ? ? ? ? 44 8b e0"));
        if (!hit) {
            log(L"Vanilla site 'map debug flags' not found (or ambiguous); grace and map options disabled.");
            return;
        }
        std::int32_t relative{};
        std::memcpy(&relative, hit + 3, sizeof(relative));
        g_map_flags = hit + 7 + relative;
        wchar_t message[160]{};
        swprintf_s(message, L"Vanilla site 'map debug flags' found at eldenring.exe+0x%llX (bytes %u %u).",
            static_cast<unsigned long long>(g_map_flags - reinterpret_cast<std::uint8_t*>(game::image())),
            g_map_flags[0], g_map_flags[1]);
        log(message);
    } catch (...) {
        g_map_flags = nullptr;
    }
}

DWORD WINAPI window_option_worker(void*) noexcept {
    struct Forced { Id id; std::ptrdiff_t offset; bool held; std::uint8_t saved; };
    // Menu order AA, SSAO, depth of field, motion blur: depth of field sits
    // between the two verified bytes (r77, confirm with the log line below).
    Forced forced[]{{aa_off, 0xFC, false, 0}, {dof_off, 0x104, false, 0}, {motion_blur_off, 0x108, false, 0}};
    perf::Loop loop(L"window_option");
    for (;;) {
        loop.sleep(250);
        std::uintptr_t window{};
        if (!game::read(reinterpret_cast<std::uintptr_t>(g_window_slot), window) || !window) continue;
        for (auto& f : forced) {
            std::uint8_t current{};
            if (!game::read(window + f.offset, current)) continue;
            if (value_of(f.id)) {
                if (!f.held) {
                    f.saved = current;
                    f.held = true;
                    wchar_t message[128]{};
                    swprintf_s(message, L"Graphics option +0x%llX forced off (menu value %u).",
                        static_cast<unsigned long long>(f.offset), current);
                    log(message);
                }
                if (current != 0) game::write(window + f.offset, std::uint8_t{0});
            } else if (f.held) {
                if (f.saved <= 4) game::write(window + f.offset, f.saved);  // Off..Maximum
                f.held = false;
            }
        }
    }
}

// ---------------------------------------------------------------------------
// Applying options

bool g_paused{};

// Fork export (ERNativeUI with HUD layout support); an older host lacks it.
using SetHudLayoutFn = ERUI_Result (ERUI_CALL*)(std::uint32_t, std::uint32_t);
SetHudLayoutFn hud_layout_export() noexcept {
    const HMODULE host = GetModuleHandleW(L"ERNativeUI.dll");
    return host ? reinterpret_cast<SetHudLayoutFn>(GetProcAddress(host, "ERNativeUIFork_SetHudLayout")) : nullptr;
}

// ERNativeUI answers ERUI_NOT_SUPPORTED when its runtime GFX patch is off
// ([GFX] EnableRuntimePatching = 0) or failed to install; the layout options are then
// greyed out. The last answer counts: the resend once the host API is
// connected comes after the host installed its hook.
std::atomic<bool> g_hud_layout_unsupported{false};

bool hud_layout_available() noexcept {
    return hud_layout_export() != nullptr && !g_hud_layout_unsupported.load(std::memory_order_acquire);
}

// Sends the saved HUD/menu layout to ERNativeUI. NativeTweaks can start
// before ERNativeUI.dll is loaded, so this runs at startup, again once the
// host API is connected and on every change; ERNativeUI loads the movies
// later, at the title screen.
void send_hud_layout(const wchar_t* when) noexcept {
    const auto on = [](Id i) { return g_options[i].value != 0; };
    const SetHudLayoutFn set_layout = hud_layout_export();
    wchar_t message[200]{};
    if (!set_layout) {
        swprintf_s(message, L"HUD layout pending (%ls): ERNativeUI not loaded yet.", when);
        log(message);
        return;
    }
    const std::uint32_t flags = (on(hud_screen_edges) ? 1u : 0u) | (on(menu_edges) ? 2u : 0u);
    const auto scale = static_cast<std::uint32_t>(value_of(hud_scale));
    // A host without the menus bit rejects the call: keep the HUD part.
    ERUI_Result result = set_layout(flags, scale);
    if (result == ERUI_INVALID_ARGUMENT) result = set_layout(flags & 1u, scale);
    g_hud_layout_unsupported.store(result == ERUI_NOT_SUPPORTED, std::memory_order_release);
    if (result == ERUI_NOT_SUPPORTED) {
        swprintf_s(message, L"HUD layout not applied (%ls): ERNativeUI's runtime GFX patch is off "
            L"([GFX] EnableRuntimePatching = 0) or not installed yet.", when);
    } else {
        swprintf_s(message, L"HUD layout sent (%ls): HUD edges %ls, scale %u%%, menu edges %ls; result %d.", when,
            on(hud_screen_edges) ? L"on" : L"off", scale, on(menu_edges) ? L"on" : L"off",
            static_cast<int>(result));
    }
    log(message);
}

void set_death_hook(bool on) noexcept;  // enemy death hook, defined with dead_enemy_worker

void apply(Id id) noexcept {
    AcquireSRWLockExclusive(&g_patch_lock);
    const auto on = [](Id i) { return g_options[i].value != 0; };
    switch (id) {
    case fov_enabled:
    case fov_percent:
        if (on(fov_enabled)) install_fov_hook();
        if (g_fov) {
            g_fov->factor = value_of(fov_percent) / 100.0f;
            g_fov->enabled = on(fov_enabled) ? 1u : 0u;
        }
        break;
    case camera_rotate_off: set_site(g_camera_rotate, on(id)); break;
    case camera_reset_off: set_site(g_camera_reset, on(id)); break;
    case vignette_off: set_site(g_vignette, on(id)); break;
    case chromatic_off: set_site(g_chromatic, on(id)); break;
    case animation_distance: set_site(g_animation, on(id)); break;
    case rune_loss_off: set_site(g_rune_loss, on(id)); break;
    case enemies_no_respawn:
        // r135 in game: skipping the whole respawn also left living enemies
        // where they were after a rest (still aggressive, not healed). The
        // reset now runs as usual and the list kills the listed ones again.
        set_site(g_enemy_respawn, false);
        set_death_hook(on(id));
        break;
    case pause_key:
        if (!on(id) && g_paused) {
            set_site(g_pause, false);
            g_paused = false;
        }
        break;
    case fps_unlock:
    case fps_limit:
    case refresh_rate: {
        if (on(fps_unlock)) {
            const float frame_time = 1.0f / static_cast<float>(value_of(fps_limit));
            std::vector<std::uint8_t> bytes(4);
            std::memcpy(bytes.data(), &frame_time, 4);
            set_bytes(g_frametime, bytes);
        } else {
            set_site(g_frametime, false);
        }
        // The fullscreen mode request stores 60 Hz as an immediate. A chosen
        // rate replaces it; otherwise the unlock clears it (driver default).
        const int hz = value_of(refresh_rate);
        if (hz != 0) {
            std::vector<std::uint8_t> bytes(g_fullscreen_cap.replacement);
            bytes[3] = static_cast<std::uint8_t>(hz);
            set_bytes(g_fullscreen_cap, bytes);
        } else {
            set_site(g_fullscreen_cap, on(fps_unlock));
        }
        break;
    }
    case ultrawide: set_site(g_ultrawide, on(id)); break;
    case ash_multiple: case ash_max_active: case ash_extra_cost: case ash_damage_per_ash:
        spirit_ashes::update(spirit_ash_settings());
        break;
    case ash_dismiss_all:
        // A button: act, then back to off (option_changed / set_option_raw
        // refresh the row).
        if (on(id) && on(ash_multiple)) spirit_ashes::dismiss_all();
        g_options[id].value = 0;
        break;
    case open_mirror: case open_rebirth:
        // Buttons like Dismiss all: the screen opens on the next game frame
        // (talk-script commands, msa\src\game_menus.rs).
        if (on(id)) {
            const int kind = id == open_mirror ? 1 : 3;
            const bool queued = spirit_ashes::open_game_menu(kind);
            wchar_t message[120]{};
            swprintf_s(message, L"Game menu %d: %s (last result %d).", kind,
                queued ? L"requested" : L"not available", spirit_ashes::game_menu_last_result());
            log(message);
        }
        g_options[id].value = 0;
        break;
    case skip_intro: set_site(g_intro, on(id)); break;
    case dialog_yes_default: g_dialog_yes_data->on = on(id) ? 1 : 0; break;
    case enemies_downed_vulnerable:
        if (on(id)) install_downed_hook(g_statics.world_chr_man);
        if (g_downed) g_downed->enabled = on(id) ? 1u : 0u;
        break;
    case hud_screen_edges:
    case hud_scale:
    case menu_edges:
        send_hud_layout(L"option changed");
        break;
    case aa_off: case motion_blur_off: case dof_off: break;  // window_option_worker keeps them in sync.
    case status_immunity: break;  // status_worker
    case set_time: case time_hour: case freeze_time: break;  // time_worker
    case set_weather: case weather_type: break;  // weather_worker
    case torrent_speed: case torrent_jump_speed: break;  // jump_worker
    case dodge_frames: case parry_frames: case knockdown_invincible: case item_super_armor:
        if (g_windows) wake_workers();
        break;  // window_worker
    case jump_height: case jump_distance: case torrent_jump_height: case torrent_jump_distance:
        if (value_of(id) != 100) install_jump_hook();
        break;  // jump_worker
    case all_graces:
    case reveal_map:
        if (g_map_flags)
            game::write<std::uint8_t>(reinterpret_cast<std::uintptr_t>(g_map_flags) + (id == all_graces ? 1 : 0),
                on(id) ? 1 : 0);
        break;
    case camera_distance:
    case damage_dealt: case damage_taken: case stamina_cost: case spell_fp_cost:
    case enemy_hp: case runes_gained: case posture_dealt: case posture_taken:
    case equip_weight: case status_buildup: case player_poise: case enemy_poise:
    case player_poise_lock: case enemy_poise_lock: case posture_one_hit: case status_instant: case die_one_hit:
    case posture_one_hit_taken: case loot_drop: case fast_travel_anywhere:
    case buff_weapon: case buff_self: case buff_enemy: case ash_cost: case upgrade_cost:
    case ash_no_rest: case ash_anywhere: case ash_keep_after_boss:
    case weapon_bleed: case weapon_poison: case weapon_rot: case weapon_frost: case weapon_sleep:
    case dmg_physical: case dmg_magic: case dmg_fire: case dmg_lightning: case dmg_holy:
    case buildup_bleed: case buildup_poison: case buildup_rot: case buildup_frost: case buildup_sleep:
    case buildup_madness: case buildup_blight:
    case weapon_scaling: case weapon_weight:
    case taken_physical: case taken_magic: case taken_fire: case taken_lightning: case taken_holy:
    case buff_weapon_forever: case buff_self_forever: case perfect_guard:
    case knockback_dealt: case knockback_taken:
    case weapon_effects_enemies: case enemies_no_immunity: case enemies_no_lock_on: case flask_hp: case flask_fp:
    case spell_stamina_cost: case spell_one_slot: case spell_no_requirements: case carry_max:
    case shop_price: case craft_free: case weapon_requirements: case upgrade_free: case hide_helmet:
    case charged_launch: case launch_all: case launch_distance:
    case world_journey: case journey_keep_runes: case boss_hp:
    case clean_characters: case hide_buff_effects: case hide_weapon_buff_effects:
        if (g_param_event) SetEvent(g_param_event);
        break;
    case skill_fp_cost:
        apply_flag(skill_fp_cost);  // 0% also sets the engine's "skills without FP" flag
        if (g_param_event) SetEvent(g_param_event);
        break;
    default:
        apply_flag(id);
        break;
    }
    ReleaseSRWLockExclusive(&g_patch_lock);
    wake_workers();
}

bool available(Id id) noexcept {
    switch (id) {
    case fov_enabled: case fov_percent: return g_fov != nullptr || g_fov_site != nullptr;
    case camera_rotate_off: return g_camera_rotate.ready;
    case camera_reset_off: return g_camera_reset.ready;
    case vignette_off: return g_vignette.ready;
    case chromatic_off: return g_chromatic.ready;
    case animation_distance: return g_animation.ready;
    case rune_loss_off: return g_rune_loss.ready;
    case enemies_no_respawn: return g_enemy_respawn.ready;
    case pause_key: return g_pause.ready;
    case fps_unlock: case fps_limit: return g_frametime.ready && g_fullscreen_cap.ready;
    case ultrawide: return g_ultrawide.ready;
    case ash_multiple: case ash_max_active: case ash_dismiss_all: case ash_extra_cost: case ash_damage_per_ash:
        return g_spirit_ashes_started;
    case refresh_rate: return g_fullscreen_cap.ready;
    case skip_intro: return g_intro.ready;
    case open_mirror: case open_rebirth:
        return g_statics.layout_2_7_1 && g_statics.world_chr_man;
    case dialog_yes_default: return g_dialog_yes_data != nullptr;
    case enemies_downed_vulnerable:
        return (g_downed != nullptr || g_downed_site != nullptr) && g_statics.layout_2_7_1 &&
            g_statics.world_chr_man;
    case hud_screen_edges: case hud_scale: case menu_edges: return hud_layout_available();
    case aa_off: case motion_blur_off: case dof_off: return g_window_slot != nullptr;
    // Param tables load with the regulation file, after the menu is built; the
    // row-size check on load disables a table whose layout does not match.
    case camera_distance:
    case damage_dealt: case damage_taken: case stamina_cost: case spell_fp_cost:
    case enemy_hp: case runes_gained: case posture_dealt: case posture_taken:
    case equip_weight: case status_buildup: case player_poise: case enemy_poise:
    case player_poise_lock: case enemy_poise_lock: case posture_one_hit: case status_instant: case die_one_hit:
    case posture_one_hit_taken: case loot_drop: case fast_travel_anywhere:
    case buff_weapon: case buff_self: case buff_enemy: case ash_cost: case upgrade_cost:
    case ash_no_rest: case ash_anywhere: case ash_keep_after_boss:
    case weapon_bleed: case weapon_poison: case weapon_rot: case weapon_frost: case weapon_sleep:
    case dmg_physical: case dmg_magic: case dmg_fire: case dmg_lightning: case dmg_holy:
    case buildup_bleed: case buildup_poison: case buildup_rot: case buildup_frost: case buildup_sleep:
    case buildup_madness: case buildup_blight:
    case weapon_scaling: case weapon_weight:
    case taken_physical: case taken_magic: case taken_fire: case taken_lightning: case taken_holy:
    case buff_weapon_forever: case buff_self_forever: case perfect_guard:
    case knockback_dealt: case knockback_taken:
    case weapon_effects_enemies: case enemies_no_immunity: case enemies_no_lock_on: case flask_hp: case flask_fp:
    case spell_stamina_cost: case spell_one_slot: case spell_no_requirements: case skill_fp_cost: case carry_max:
    case shop_price: case craft_free: case weapon_requirements: case upgrade_free: case hide_helmet:
    case charged_launch: case launch_all: case launch_distance: case boss_hp:
        return g_statics.layout_2_7_1 && g_statics.regulation_manager;
    case clean_characters: return g_statics.layout_2_7_1 && g_statics.regulation_manager && !g_clean.attempted;
    case hide_buff_effects:
        return g_statics.layout_2_7_1 && g_statics.regulation_manager && !g_buff_vfx.attempted;
    case hide_weapon_buff_effects:
        return g_statics.layout_2_7_1 && g_statics.regulation_manager && !g_weapon_buff_vfx.attempted;
    // Also needs the save's cycle (GameDataMan) to add only what the save lacks.
    case world_journey: case journey_keep_runes:
        return g_statics.layout_2_7_1 && g_statics.regulation_manager && g_statics.game_data_man &&
            !g_journey.attempted;
    case max_hp: case max_fp: case max_stamina:
    case hp_regen_points: case fp_regen_points: case stamina_regen_points:
    case speed_global: case speed_attack: case speed_spell: case speed_roll: case speed_move:
    case speed_jump: case speed_item: case enemy_speed: case no_fall_damage:
    case status_immunity:
        return g_statics.layout_2_7_1 && g_statics.world_chr_man;
    case set_time: case time_hour: case freeze_time:
    case set_weather: case weather_type:
        return g_statics.layout_2_7_1;
    case torrent_speed: case torrent_jump_speed: return g_statics.layout_2_7_1 && g_statics.world_chr_man;
    case dodge_frames: case parry_frames: case knockdown_invincible: case item_super_armor:
        return g_windows != nullptr && g_statics.layout_2_7_1 && g_statics.world_chr_man;
    case jump_height: case jump_distance: case torrent_jump_height: case torrent_jump_distance:
        return (g_jump != nullptr || g_jump_site != nullptr) && g_statics.layout_2_7_1 && g_statics.world_chr_man;
    case all_graces: case reveal_map: return g_map_flags != nullptr;
    case god_mode: return g_statics.layout_2_7_1 && g_statics.world_chr_man;
    default:
        return g_statics.chr_dbg_flags != 0;
    }
}

// [General] <key> = 1, or the optional package's marker file beside the INI
// (so installing a package never replaces the user's NativeTweaks.ini).
bool optional_feature(const wchar_t* key, const wchar_t* marker_name) noexcept {
    if (g_config_path.empty()) return false;
    if (GetPrivateProfileIntW(L"General", key, 0, g_config_path.c_str()) != 0) return true;
    std::wstring marker = g_config_path;
    const std::size_t slash = marker.find_last_of(L"\\/");
    marker.resize(slash == std::wstring::npos ? 0 : slash + 1);
    marker += marker_name;
    return GetFileAttributesW(marker.c_str()) != INVALID_FILE_ATTRIBUTES;
}

void load_settings() noexcept {
    for (auto& option : g_options) {
        const int stored = g_config_path.empty() ? option.fallback :
            static_cast<int>(GetPrivateProfileIntW(L"Vanilla", option.key, option.fallback,
                g_config_path.c_str()));
        option.value = clamp_value(option, stored);
    }
    // r121: "Skills without FP" became Skill FP cost at 0%. An INI from before
    // keeps its choice; the old key is removed once migrated.
    if (!g_config_path.empty() &&
        GetPrivateProfileIntW(L"Vanilla", L"SkillsWithoutFp", 0, g_config_path.c_str()) != 0) {
        if (GetPrivateProfileIntW(L"Vanilla", g_options[skill_fp_cost].key, -1, g_config_path.c_str()) ==
            static_cast<UINT>(-1)) {
            g_options[skill_fp_cost].value = 0;
            WritePrivateProfileStringW(L"Vanilla", g_options[skill_fp_cost].key, L"0", g_config_path.c_str());
        }
        WritePrivateProfileStringW(L"Vanilla", L"SkillsWithoutFp", nullptr, g_config_path.c_str());
    }
}

DWORD WINAPI save_worker(void*) noexcept {
    // Each WritePrivateProfileStringW rewrites the whole INI, so only the
    // options that changed since the last save are written.
    int written[count]{};
    bool known[count]{};
    perf::Loop loop(L"save");
    for (;;) {
        loop.pause();
        WaitForSingleObject(g_save_event, INFINITE);
        Sleep(400);  // Coalesce slider drags into one write.
        loop.resume();
        for (std::size_t i = 0; i < count; ++i) {
            const wchar_t* const key = g_options[i].key;
            const int current = g_options[i].value;
            if (known[i] && written[i] == current) continue;
            wchar_t value[12]{};
            swprintf_s(value, L"%d", current);
            if (WritePrivateProfileStringW(L"Vanilla", key, value, g_config_path.c_str())) {
                written[i] = current;
                known[i] = true;
            }
        }
    }
}

void request_save() noexcept {
    if (g_save_event) SetEvent(g_save_event);
}

void refresh_row(Id i) noexcept;

void ERUI_CALL option_changed(void* target, std::uint8_t value) noexcept {
    auto* option = static_cast<Option*>(target);
    if (!option) return;
    option->value = clamp_value(*option, value);
    const std::int16_t requested = option->value;
    apply(static_cast<Id>(option - g_options));
    // An action toggle (Dismiss all ashes) turns itself off again.
    if (option->value != requested) refresh_row(static_cast<Id>(option - g_options));
    request_save();
}

void ERUI_CALL slider_changed(void* target, const ERUI_NumericSliderChangeContext* context) noexcept {
    auto* option = static_cast<Option*>(target);
    if (!option || !context || context->value.decimal_places != 0) return;
    option->value = clamp_value(*option, static_cast<int>(context->value.scaled_value));
    apply(static_cast<Id>(option - g_options));
    request_save();
}

// Named sliders (weather): the index is the option value, minimum 0, step 1.
void ERUI_CALL named_slider_changed(void* target, const ERUI_CustomSliderChangeContext* context) noexcept {
    auto* option = static_cast<Option*>(target);
    if (!option || !context) return;
    option->value = clamp_value(*option, static_cast<int>(context->index));
    apply(static_cast<Id>(option - g_options));
    request_save();
}

bool is_named(Id i) noexcept { return i == weather_type || i == charged_launch || i == world_journey; }

// Fork export (r115) that colours labels away from their default; null when
// ERNativeUI lacks it or the highlight is off.
using SetRowDefaultFn = ERUI_Result (ERUI_CALL*)(ERUI_ProviderHandle, ERUI_RowHandle, std::int64_t);
SetRowDefaultFn g_set_default{};

// Newer fork export (r119): flags bit 0 = higher is worse, so the label turns
// red above the default and green below it. Null on an older fork.
using SetRowDefaultExFn = ERUI_Result (ERUI_CALL*)(ERUI_ProviderHandle, ERUI_RowHandle, std::int64_t,
    std::uint32_t);
SetRowDefaultExFn g_set_default_ex{};

// Sliders where a higher value makes the game easier for enemies or costlier
// for the player.
bool higher_is_worse(Id id) noexcept {
    switch (id) {
    case damage_taken: case posture_taken: case stamina_cost: case spell_fp_cost:
    case equip_weight: case enemy_hp: case boss_hp: case enemy_poise: case enemy_speed:
    case ash_cost: case upgrade_cost: case weapon_weight: case knockback_taken: case spell_stamina_cost:
    case ash_extra_cost: case ash_damage_per_ash:
    case skill_fp_cost:
    case taken_physical: case taken_magic: case taken_fire: case taken_lightning: case taken_holy:
        return true;
    default:
        return false;
    }
}

void set_row_default(Id id, erui::RowHandle row, std::int64_t value) noexcept {
    if (!g_registration) return;
    if (g_set_default_ex)
        (void)g_set_default_ex(g_registration->provider_handle(), row, value, higher_is_worse(id) ? 1u : 0u);
    else if (g_set_default)
        (void)g_set_default(g_registration->provider_handle(), row, value);
}

void restore_option(Id i) noexcept {
    auto& option = g_options[i];
    option.value = option.fallback;
    apply(i);
    if (!g_registration || option.row == ERUI_INVALID_ROW) return;
    if (option.kind == Kind::toggle)
        (void)g_registration->set_value(option.row, static_cast<std::uint8_t>(option.value));
    else if (is_named(i))
        (void)g_registration->set_custom_slider_index(option.row, static_cast<std::uint32_t>(option.value));
    else
        (void)g_registration->set_numeric_slider_value(option.row, option.value);
}

void ERUI_CALL restore_defaults(void*) noexcept {
    for (std::size_t i = 0; i < count; ++i) restore_option(static_cast<Id>(i));
    request_save();
    log(L"Vanilla settings restored to game defaults.");
}

// The pause key toggles the same byte techiew's PauseTheGame uses; it only
// listens while the game window has focus.
DWORD WINAPI pause_worker(void*) noexcept {
    // XInput is loaded at runtime so the hub does not import it.
    using XInputGetStateFn = DWORD(WINAPI*)(DWORD, void*);
    XInputGetStateFn get_state{};
    if (HMODULE xinput = LoadLibraryW(L"xinput1_4.dll"))
        get_state = reinterpret_cast<XInputGetStateFn>(GetProcAddress(xinput, "XInputGetState"));
    if (!get_state) log(L"XInput not available; pause works from the keyboard only.");
    // XInputGetState on an empty slot is slow (it can stall for milliseconds,
    // and the game's own input polling may wait behind it), so a single empty
    // slot is rechecked every 2 s, in turn.
    bool connected[4]{};
    ULONGLONG next_probe{};
    DWORD probe_pad{};
    const auto start_pressed = [&]() noexcept {
        if (!get_state) return false;
        struct { DWORD packet; WORD buttons; std::uint8_t rest[10]; } state{};
        const ULONGLONG now = GetTickCount64();
        const bool probe = now >= next_probe;
        if (probe) next_probe = now + 2000;
        bool pressed{};
        for (DWORD pad = 0; pad < 4; ++pad) {
            if (!connected[pad] && !(probe && pad == probe_pad)) continue;
            connected[pad] = get_state(pad, &state) == ERROR_SUCCESS;
            if (connected[pad] && (state.buttons & 0x0010)) pressed = true;  // START
        }
        if (probe) probe_pad = (probe_pad + 1) % 4;
        return pressed;
    };
    bool was_down{};
    perf::Loop loop(L"pause");
    for (;;) {
        loop.sleep(15);
        if (!value_of(pause_key)) {
            was_down = false;
            idle_wait(loop);
            continue;
        }
        DWORD pid{};
        GetWindowThreadProcessId(GetForegroundWindow(), &pid);
        const bool focused = pid == GetCurrentProcessId();
        const bool down = focused && ((GetAsyncKeyState(VK_ESCAPE) & 0x8000) || start_pressed());
        if (down && !was_down) {
            AcquireSRWLockExclusive(&g_patch_lock);
            if (set_site(g_pause, !g_paused)) g_paused = !g_paused;
            ReleaseSRWLockExclusive(&g_patch_lock);
            log(g_paused ? L"Game paused by hotkey." : L"Game resumed by hotkey.");
        }
        was_down = down;
    }
}

// ---------------------------------------------------------------------------
// Enemies stay dead (beta). Resting, fast travel and loading the save bring
// every enemy back (the game only remembers kills in memory); the respawn
// request (g_enemy_respawn) is left alone so living enemies reset as usual
// (until r135 it was skipped, which kept the living ones in place). So while the
// option is on, each enemy the game pays runes for is written to a list per
// save (slot, character name and New Game+ cycle) and is killed again, with no
// payout, whenever it is loaded. Bosses, NPCs, phantoms and summons are left
// to the game and its event flags (see kept_dead).
//
// ChrIns (2.7.1.0, fromsoftware-rs): FieldInsHandle at +0x08 (selector: index
// in its ChrSet in bits 0-19, i.e. the MSB order of its block) and block ID at
// +0x0C, NpcParam row +0x60, team +0x6C, death flag +0x1C5 bit 7, drop flags
// +0x1C6 (bit 0 item lot, bit 1 runes: "already given"), event entity ID
// +0x1E8; HP at modules -> data module +0x138, maximum +0x13C.
//
// Deaths come from the game itself (death hook below): polling the update
// list never saw one (r129 and d224994 tests), and 6db00cb showed that the
// enemies killed in game read team 0 or 48, not only 6. Loaded characters
// come from every ChrSet of WorldChrMan (game::for_each_loaded_chr), so an
// enemy that is loaded but not updating yet is also caught.

struct DeadEnemy {
    std::uint32_t block;
    std::uint32_t index;
    std::int32_t npc_param;
    bool operator<(const DeadEnemy& other) const noexcept {
        if (block != other.block) return block < other.block;
        if (index != other.index) return index < other.index;
        return npc_param < other.npc_param;
    }
    bool operator==(const DeadEnemy& other) const noexcept {
        return block == other.block && index == other.index && npc_param == other.npc_param;
    }
};

// The fields read from one ChrIns.
struct EnemyState {
    std::uintptr_t vtable, data;
    std::uint32_t entity, selector, block;
    std::int32_t npc_param, hp, hp_max;
    std::uint8_t team, flags, drops;
    DeadEnemy key() const noexcept { return {block, selector & 0xFFFFFu, npc_param}; }
};

bool read_enemy(std::uintptr_t chr, EnemyState& s) noexcept {
    std::uintptr_t modules{};
    return game::read(chr, s.vtable) && s.vtable && game::read(chr + 0x6C, s.team) &&
        game::read(chr + 0x1E8, s.entity) && game::read(chr + 0x8, s.selector) &&
        game::read(chr + 0xC, s.block) && game::read(chr + 0x60, s.npc_param) &&
        game::read(chr + 0x1C5, s.flags) && game::read(chr + 0x1C6, s.drops) &&
        game::read(chr + game::kModules, modules) && modules && game::read(modules, s.data) && s.data &&
        game::read(s.data + 0x138, s.hp) && game::read(s.data + 0x13C, s.hp_max);
}

// Teams that are never kept dead (Smithbox ER TEAM_TYPE): 1-5 players and
// phantoms, 7 Boss, 8 Ally, 10-11 decoys, 12 Battle Ally, 13 and 16-18
// invaders, 19-22 host, co-op, hostile and wandering phantoms, 25 Strong Enemy,
// 26-28 friendly, hostile and co-op NPCs, 30 Object, 31-33 mad phantoms and
// the arch-enemy team, 47 Spirit Summon. Everything else the game pays for is
// an enemy: 0 (None; seen on enemies with an entity ID), 6 Enemy, 9 Hostile
// Ally, 14 Neutral (animals), 15 Charmed, 23-24 Enemy 1/2, 29
// Indiscriminate, and 48 and up (no name in Paramdex; 85 of them around the
// first test, NpcParam 43114010 is a normal enemy).
bool excluded_team(std::uint8_t team) noexcept {
    switch (team) {
    case 1: case 2: case 3: case 4: case 5: case 7: case 8: case 10: case 11: case 12: case 13:
    case 16: case 17: case 18: case 19: case 20: case 21: case 22: case 25: case 26: case 27: case 28:
    case 30: case 31: case 32: case 33: case 47:
        return true;
    default:
        return false;
    }
}

// FromSoftware numbers boss parts xxx800-xxx899 (10000800 Margit,
// 1043360800 a field boss); they keep their own event flags.
bool boss_numbered(const EnemyState& s) noexcept {
    return s.entity && s.entity % 1000 >= 800 && s.entity % 1000 < 900;
}

// Why a death is not kept, or nullptr when it is.
const wchar_t* not_kept_reason(const EnemyState& s) noexcept {
    if (s.selector == 0xFFFFFFFFu || s.block == 0xFFFFFFFFu || s.npc_param <= 0 || s.hp_max <= 0)
        return L"no map handle, skipped";
    if (boss_numbered(s)) return L"boss-numbered, skipped";
    if (excluded_team(s.team)) return L"boss, NPC, phantom or summon team, skipped";
    return nullptr;
}

std::wstring dead_enemy_path(const game::SaveIdentity& id) {
    const std::size_t slash = g_config_path.find_last_of(L"\\/");
    if (slash == std::wstring::npos) return {};
    std::wstring name = id.name;
    for (wchar_t& c : name)
        if (c < 32 || std::wcschr(L"\\/:*?\"<>|.", c)) c = L'_';
    return g_config_path.substr(0, slash + 1) + L"NativeTweaks\\dead-enemies\\Slot" +
        std::to_wstring(id.slot) + L"_" + name + L"_NG" + std::to_wstring(id.cycle) + L".txt";
}

void load_dead_enemies(const std::wstring& path, std::set<DeadEnemy>& dead) {
    dead.clear();
    FILE* file{};
    if (path.empty() || _wfopen_s(&file, path.c_str(), L"r") != 0 || !file) return;
    char line[128]{};
    while (std::fgets(line, sizeof(line), file)) {
        unsigned block{}, index{};
        int npc_param{};
        if (line[0] != ';' && sscanf_s(line, "%x %u %d", &block, &index, &npc_param) == 3)
            dead.insert({block, index, npc_param});
    }
    std::fclose(file);
}

bool save_dead_enemies(const std::wstring& path, const std::set<DeadEnemy>& dead) {
    if (path.empty()) return false;
    const std::wstring folder = path.substr(0, path.find_last_of(L'\\'));
    CreateDirectoryW(folder.substr(0, folder.find_last_of(L'\\')).c_str(), nullptr);
    CreateDirectoryW(folder.c_str(), nullptr);
    std::string text = "; NativeTweaks: enemies that stay dead in this save (block, index, NpcParam).\n"
        "; Delete this file to bring them all back.\n";
    char line[64]{};
    for (const DeadEnemy& e : dead) {
        std::snprintf(line, sizeof(line), "%08X %u %d\n", e.block, e.index, e.npc_param);
        text += line;
    }
    const std::wstring temporary = path + L".tmp";
    HANDLE file = CreateFileW(temporary.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
        FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    DWORD written{};
    const bool ok = WriteFile(file, text.data(), static_cast<DWORD>(text.size()), &written, nullptr) &&
        written == text.size();
    CloseHandle(file);
    if (!ok || !MoveFileExW(temporary.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING)) {
        DeleteFileW(temporary.c_str());
        return false;
    }
    return true;
}

// Death hook. Two functions pay for a character's death, rcx = ChrIns
// (2.7.1.0): eldenring.exe+0x3EDA70 (item lot, then runes) and +0x3EDB60
// (runes only). The death-state update (+0x3F84C0, once the death flag
// +0x1C5 bit 7 is set) and the ChrActionFlag 69 death transition (+0x428EB1,
// +0x428EEB) call them. Both start with "push rbx; sub rsp,40h" (6 bytes): the
// trampoline calls death_event while the option is on and, when it returns
// 1 (an enemy the worker killed again on load), returns at once so the game
// pays nothing a second time; otherwise it runs those two instructions and
// jumps back. Their return value is not used by the callers.

struct DeathData {
    volatile std::uint32_t enabled;
    std::uint32_t pad;
    std::uint64_t handler;
};

DeathData* g_death{};

struct DeathEvent {
    std::uintptr_t chr;
    std::uint32_t kind;  // 0 item lot and runes, 1 runes only
    bool readable;
    bool suppressed;     // killed again on load: payout skipped
    EnemyState state;
};

SRWLOCK g_death_lock = SRWLOCK_INIT;
perf::Timer* g_perf_death_hook{};
perf::Timer* g_perf_death_lock{};
std::vector<DeathEvent> g_death_events{};
std::uintptr_t g_rekilled[256]{};  // characters killed again on load, until they die
std::size_t g_rekilled_next{};
volatile LONG g_death_dropped{};

// Runs on the game thread for every death the game pays for: read the fields
// now, while the character is still whole, and leave the rest to the worker.
std::uint32_t death_event(std::uintptr_t chr, std::uint32_t kind) noexcept {
    perf::Scope timing(g_perf_death_hook);
    bool suppressed = false;
    try {
        DeathEvent e{chr, kind, false, false, {}};
        e.readable = read_enemy(chr, e.state);
        lock_timed(&g_death_lock, g_perf_death_lock);
        for (std::uintptr_t& entry : g_rekilled)
            if (entry == chr) {
                entry = 0;
                suppressed = true;
                break;
            }
        e.suppressed = suppressed;
        if (g_death_events.size() < 1024) g_death_events.push_back(e);
        else InterlockedIncrement(&g_death_dropped);
        ReleaseSRWLockExclusive(&g_death_lock);
    } catch (...) {
    }
    return suppressed ? 1u : 0u;
}

void install_death_hook() noexcept {
    constexpr const char* kBase =
        "40 53 48 83 EC 40 48 C7 44 24 20 FE FF FF FF 0F 29 74 24 30 48 8B D9 48 8B 41 58 48 8B 90 C8 00 00 00 "
        "F6 42 24 01 0F 85 ? ? ? ? E8 ? ? ? ? 8B 43 68 83 F8 03 0F 84 ? ? ? ? 83 F8 0A 0F 84 ? ? ? ? "
        "48 8B CB E8 ? ? ? ?";
    std::uint8_t* sites[2]{
        find_unique(parse_pattern((std::string(kBase) + " 48 8B CB E8 ? ? ? ? 0F 28 F0").c_str())),
        find_unique(parse_pattern((std::string(kBase) + " 0F 28 F0").c_str())),
    };
    if (!sites[0] || !sites[1]) {
        log(L"Vanilla site 'enemy death' not found (or ambiguous); enemies are only kept dead after resting.");
        return;
    }
    constexpr std::uint8_t kPrologue[]{0x40, 0x53, 0x48, 0x83, 0xEC, 0x40};  // push rbx; sub rsp,40h
    for (std::uint8_t* site : sites)
        if (std::memcmp(site, kPrologue, sizeof(kPrologue)) != 0) {
            log(L"Vanilla enemy death code was changed by another mod; enemies are only kept dead after resting.");
            return;
        }
    std::uint8_t* code = alloc_near(sites[0]);
    if (!code) {
        log(L"Vanilla enemy death hook: no memory near the game code; enemies are only kept dead after resting.");
        return;
    }
    auto* data = reinterpret_cast<DeathData*>(code + 512);
    data->enabled = 0;
    data->handler = reinterpret_cast<std::uint64_t>(&death_event);

    std::uint8_t* p = code;
    const auto put = [&](std::initializer_list<std::uint8_t> bytes) {
        for (auto b : bytes) *p++ = b;
    };
    const auto put_u64 = [&](std::uint64_t v) { std::memcpy(p, &v, 8); p += 8; };
    const auto put_rel = [&](const volatile void* target, std::size_t instruction_tail) {
        const auto next = reinterpret_cast<std::intptr_t>(p) + 4 + instruction_tail;
        const auto disp = static_cast<std::int32_t>(reinterpret_cast<std::intptr_t>(target) - next);
        std::memcpy(p, &disp, 4);
        p += 4;
    };
    std::uint8_t* stubs[2]{};
    for (std::uint32_t kind = 0; kind < 2; ++kind) {
        p = code + kind * 128;
        stubs[kind] = p;
        // At entry rsp is 8 mod 16: four pushes keep it so and 28h aligns the call.
        put({0x83, 0x3D}); put_rel(&data->enabled, 1); put({0x00});         // cmp dword [enabled],0
        put({0x74, 0x00});                                                  // je original
        std::uint8_t* const skip = p;
        put({0x51, 0x52, 0x41, 0x50, 0x41, 0x51});                           // push rcx, rdx, r8, r9
        put({0x48, 0x83, 0xEC, 0x28});                                       // sub rsp,28h
        put({0xBA, static_cast<std::uint8_t>(kind), 0x00, 0x00, 0x00});      // mov edx,kind
        put({0xFF, 0x15}); put_rel(&data->handler, 0);                       // call [handler]
        put({0x85, 0xC0});                                                   // test eax,eax
        put({0x48, 0x8D, 0x64, 0x24, 0x28});                                 // lea rsp,[rsp+28h] (flags kept)
        put({0x41, 0x59, 0x41, 0x58, 0x5A, 0x59});                           // pop r9, r8, rdx, rcx
        put({0x74, 0x01});                                                   // jz original
        put({0xC3});                                                         // ret: payout skipped
        skip[-1] = static_cast<std::uint8_t>(p - skip);
        put({0x40, 0x53, 0x48, 0x83, 0xEC, 0x40});                           // push rbx; sub rsp,40h
        put({0xFF, 0x25, 0x00, 0x00, 0x00, 0x00});                           // jmp [rip+0]
        put_u64(reinterpret_cast<std::uint64_t>(sites[kind] + sizeof(kPrologue)));
    }
    FlushInstructionCache(GetCurrentProcess(), code, 512);
    if (!write_jump(sites[0], stubs[0])) {
        log(L"Vanilla enemy death hook could not be written; enemies are only kept dead after resting.");
        return;
    }
    if (!write_jump(sites[1], stubs[1])) {
        write_code(sites[0], kPrologue, 5);
        log(L"Vanilla enemy death hook could not be written; enemies are only kept dead after resting.");
        return;
    }
    g_death = data;
    wchar_t message[160]{};
    const auto base = reinterpret_cast<std::uint8_t*>(game::image());
    swprintf_s(message, L"Vanilla enemy death hook installed at eldenring.exe+0x%llX and +0x%llX.",
        static_cast<unsigned long long>(sites[0] - base), static_cast<unsigned long long>(sites[1] - base));
    log(message);
}

// Forgets the characters killed again on load. Called whenever the area
// reloads (loading screen, the player's death): the game reuses ChrIns
// addresses, and a stale entry would skip the payout of a fresh enemy.
void clear_rekilled() noexcept {
    AcquireSRWLockExclusive(&g_death_lock);
    std::fill(std::begin(g_rekilled), std::end(g_rekilled), std::uintptr_t{});
    ReleaseSRWLockExclusive(&g_death_lock);
}

void set_death_hook(bool on) noexcept {
    if (g_death) g_death->enabled = on ? 1u : 0u;
    if (!on) {
        AcquireSRWLockExclusive(&g_death_lock);
        g_death_events.clear();
        ReleaseSRWLockExclusive(&g_death_lock);
        clear_rekilled();
    }
}

// Character enable state, as event scripts set it: ChrIns debug flags (+0x538,
// fromsoftware-rs ChrDebugFlags) bit 12 "character_disabled" turns off
// updates, rendering and physics. The game's own setters (0x3F5D50 disable,
// 0x3FA770 enable in 2.7.2.0; 0x3F5D50 / 0x3FA6E0 in 2.7.0.0 and 2.7.1.x) also
// update the character it rides or carries. A listed enemy is disabled instead
// of killed again, so it leaves no body and no death animation (r135 request).
constexpr std::uintptr_t kChrDisabledByte = 0x539;
constexpr std::uint8_t kChrDisabledBit = 0x10;
using ChrEnableFn = void (*)(std::uintptr_t chr);
ChrEnableFn g_chr_disable{};
ChrEnableFn g_chr_enable{};
constexpr const char* kChrDisablePattern =
    "40 53 48 83 ec 20 48 8b 81 90 01 00 00 48 8b d9 80 89 39 05 00 00 10 48";
constexpr const char* kChrEnablePattern = "40 53 48 83 ec 20 80 a1 39 05 00 00 ef 48 8b 59";

void resolve_chr_enable() noexcept {
    g_chr_disable = reinterpret_cast<ChrEnableFn>(find_unique(parse_pattern(kChrDisablePattern)));
    g_chr_enable = reinterpret_cast<ChrEnableFn>(find_unique(parse_pattern(kChrEnablePattern)));
    if (!g_chr_disable || !g_chr_enable) {
        g_chr_disable = g_chr_enable = nullptr;
        log(L"Vanilla site 'character enable state' not found (or ambiguous); Enemies stay dead kills listed enemies again.");
        return;
    }
    wchar_t message[160]{};
    swprintf_s(message, L"Vanilla site 'character enable state' found at eldenring.exe+0x%llX and +0x%llX.",
        static_cast<unsigned long long>(reinterpret_cast<std::uint8_t*>(g_chr_disable) -
            reinterpret_cast<std::uint8_t*>(game::image())),
        static_cast<unsigned long long>(reinterpret_cast<std::uint8_t*>(g_chr_enable) -
            reinterpret_cast<std::uint8_t*>(game::image())));
    log(message);
}

bool call_chr_enable(ChrEnableFn function, std::uintptr_t chr) noexcept {
    __try {
        function(chr);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool chr_disabled(std::uintptr_t chr) noexcept {
    std::uint8_t flags{};
    return game::read(chr + kChrDisabledByte, flags) && (flags & kChrDisabledBit);
}

// CSFade (fromsoftware-rs): nine fade plates from +0x10; a plate's current
// colour is at +0x10, alpha at +0x1C. The game fades to black before every
// loading screen and back in once the world is ready.
bool screen_faded(std::uintptr_t fade_slot) noexcept {
    std::uintptr_t fade{};
    if (!fade_slot || !game::read(fade_slot, fade) || !fade) return true;
    for (int i = 0; i < 9; ++i) {
        std::uintptr_t plate{};
        float alpha{};
        if (game::read(fade + 0x10 + i * 8, plate) && plate >= 0x10000 && game::read(plate + 0x1C, alpha) &&
            alpha > 0.5f)
            return true;
    }
    return false;
}

// Loaded characters that are on the list: disabled at once (or killed again,
// payout skipped by the death hook, if the setters were not found).
struct RespawnPass {
    std::uintptr_t player{};
    const std::set<DeadEnemy>* dead{};
    const std::map<std::uintptr_t, int>* attempts{};  // characters killed again -> writes so far
    std::map<std::uintptr_t, int> attempts_now{};
    std::vector<std::pair<std::uintptr_t, EnemyState>> prevented{};
    std::vector<std::uintptr_t> disabled_now{};  // listed characters this pass disabled
    bool use_disable{};    // disable listed enemies (else kill them again)
    bool world_running{};  // in the world: no fade or loading screen for 1 s, player animating
    int characters{}, unreadable{}, alive_listed{}, kept_disabled{};
    int teams[64]{};
};

constexpr int kKillAttempts = 20;  // the game may restore HP while the block loads

void respawn_pass_visit(std::uintptr_t chr, void* context) noexcept {
    auto& pass = *static_cast<RespawnPass*>(context);
    try {
        if (chr == pass.player) return;
        ++pass.characters;
        EnemyState s{};
        if (!read_enemy(chr, s)) {
            ++pass.unreadable;
            return;
        }
        ++pass.teams[s.team < 63 ? s.team : 63];
        if (s.hp <= 0 || (s.flags & 0x80) || boss_numbered(s) || !pass.dead->count(s.key())) return;
        if (pass.use_disable) {
            if (chr_disabled(chr)) {
                ++pass.kept_disabled;
                return;
            }
            // r135 in game: disabling enemies while their area loaded kept the
            // loading screen up forever (the player's animation already runs
            // there). Wait until the screen has been clear of fades for 1 s.
            if (!pass.world_running) {
                ++pass.alive_listed;
                return;
            }
            game::write<std::uint8_t>(chr + 0x1C6, static_cast<std::uint8_t>(s.drops | 0x3));
            if (call_chr_enable(g_chr_disable, chr) && chr_disabled(chr)) {
                pass.prevented.emplace_back(chr, s);
                try {
                    pass.disabled_now.push_back(chr);
                } catch (...) {
                }
                ++pass.kept_disabled;
                return;
            }
        }
        ++pass.alive_listed;
        int attempts = 0;
        const auto previous = pass.attempts->find(chr);
        if (previous != pass.attempts->end()) attempts = previous->second;
        if (attempts < kKillAttempts) {
            if (!attempts) {
                pass.prevented.emplace_back(chr, s);
                AcquireSRWLockExclusive(&g_death_lock);
                g_rekilled[g_rekilled_next++ % std::size(g_rekilled)] = chr;
                ReleaseSRWLockExclusive(&g_death_lock);
            }
            game::write<std::uint8_t>(chr + 0x1C6, static_cast<std::uint8_t>(s.drops | 0x3));
            game::write<std::int32_t>(s.data + 0x138, 0);
            ++attempts;
        }
        pass.attempts_now[chr] = attempts;
    } catch (...) {
    }
}

// Loaded characters, gathered first: two ChrSets can share one, and a sorted
// vector reused every pass costs no allocation per character.
void collect_chr(std::uintptr_t chr, void* context) noexcept {
    try {
        static_cast<std::vector<std::uintptr_t>*>(context)->push_back(chr);
    } catch (...) {
    }
}

// What decides a character's outcome in the pass and changes while it is
// loaded: map handle (+0x08, selector and block), death flag (+0x1C5) and HP.
// A character whose signature is unchanged since the last pass gave the same
// answer then, so between full passes only these few fields are read.
struct ChrSeen {
    std::uintptr_t chr;
    std::uint64_t handle;
    std::int32_t hp;
    std::uint8_t flags;
    std::uint8_t enable;  // +0x539: the game may enable a disabled enemy again
};

bool chr_signature(std::uintptr_t chr, ChrSeen& out) noexcept {
    std::uintptr_t modules{}, data{};
    out = {chr, 0, 0, 0, 0};
    return game::read(chr + 0x8, out.handle) && game::read(chr + 0x1C5, out.flags) &&
        game::read(chr + kChrDisabledByte, out.enable) &&
        game::read(chr + game::kModules, modules) && modules && game::read(modules, data) && data &&
        game::read(data + 0x138, out.hp);
}

constexpr DWORD kFullPassMs = 1000;

DWORD WINAPI dead_enemy_worker(void*) noexcept {
    std::vector<std::uintptr_t> loaded{};
    std::vector<ChrSeen> seen{}, seen_next{};  // sorted by address, from the last pass
    DWORD next_full = GetTickCount();
    bool force_full{};
    std::wstring path{};  // list of the loaded save; empty when none
    std::set<DeadEnemy> dead{};
    std::map<std::uintptr_t, int> attempts{};
    std::set<std::uintptr_t> disabled{};  // characters this worker disabled, while loaded
    // The world runs when the play time of the player's current animation
    // changes; it stands still on loading screens (and while paused).
    float player_play_time = -1.0f;
    DWORD player_play_tick{};
    bool world_running{};
    std::uintptr_t fade_slot{};
    DWORD next_fade_lookup{};
    bool fade_logged{};
    DWORD clear_since{};  // the screen has had no fade since then (0 = faded)
    bool dirty{};
    int recorded{}, prevented{};
    DWORD last_save = GetTickCount();
    DWORD next_summary = GetTickCount();
    // The player's death: the game reloads the area, so every listed enemy
    // comes back as a new character and must be killed again (r129 report:
    // enemies returned after dying; only resting went through +0x50B580).
    std::int32_t player_hp = -1;
    bool after_death{};
    DWORD death_tick{};
    int prevented_after_death{}, alive_listed_now{};
    int event_lines{};  // per-enemy lines logged for this save (capped)
    constexpr int kEventLines = 600;
    const auto line = [&](const wchar_t* message) noexcept {
        if (event_lines >= kEventLines) return;
        ++event_lines;
        log(message);
        if (event_lines == kEventLines) log(L"Enemies stay dead: no more per-enemy lines for this save.");
    };
    const auto flush = [&]() noexcept {
        try {
            if (dirty && save_dead_enemies(path, dead)) {
                wchar_t message[200]{};
                swprintf_s(message, L"Enemies stay dead: %zu in the list (%d new, %d respawns prevented).",
                    dead.size(), recorded, prevented);
                log(message);
                recorded = prevented = 0;
            }
        } catch (...) {
        }
        dirty = false;
        last_save = GetTickCount();
    };
    const auto close = [&]() noexcept {
        flush();
        AcquireSRWLockExclusive(&g_death_lock);
        g_death_events.clear();  // deaths of a save that is no longer loaded
        ReleaseSRWLockExclusive(&g_death_lock);
        path.clear();
        dead.clear();
        attempts.clear();
        clear_rekilled();
        player_hp = -1;
        player_play_time = -1.0f;
        world_running = false;
        clear_since = 0;
        event_lines = 0;
        seen.clear();
        disabled.clear();  // their characters are gone or about to be
        force_full = true;
    };
    // Turning the option off in game: the enemies it disabled come back where
    // they are (the next rest also resets them).
    const auto enable_again = [&]() noexcept {
        if (disabled.empty() || !g_chr_enable) return;
        std::uintptr_t manager{}, player{};
        if (!game::read(g_statics.world_chr_man, manager) || !manager ||
            !game::read(manager + game::kPlayerIns, player) || !player) return;
        loaded.clear();
        game::for_each_loaded_chr(g_statics, &collect_chr, &loaded);
        std::sort(loaded.begin(), loaded.end());
        loaded.erase(std::unique(loaded.begin(), loaded.end()), loaded.end());
        int enabled = 0;
        for (const std::uintptr_t chr : loaded)
            if (disabled.count(chr) && chr_disabled(chr) && call_chr_enable(g_chr_enable, chr)) ++enabled;
        wchar_t message[120]{};
        swprintf_s(message, L"Enemies stay dead: option off, %d disabled enemies enabled again.", enabled);
        log(message);
        disabled.clear();
    };
    perf::Loop loop(L"dead_enemy");
    for (;;) {
        loop.sleep(50);
        try {
            if (!value_of(enemies_no_respawn)) {
                enable_again();
                close();
                idle_wait(loop);
                continue;
            }
            std::uintptr_t manager{}, player{};
            game::SaveIdentity id{};
            if (!game::read(g_statics.world_chr_man, manager) || !manager ||
                !game::read(manager + game::kPlayerIns, player) || !player ||
                !game::save_identity(g_statics, id)) {
                close();  // title screen or loading
                continue;
            }
            const std::wstring current = dead_enemy_path(id);
            if (current != path) {
                close();
                path = current;
                load_dead_enemies(path, dead);
                wchar_t message[1024]{};
                swprintf_s(message, L"Enemies stay dead: %zu enemies listed for %ls.", dead.size(), path.c_str());
                log(message);
            }

            // The player's own death.
            std::int32_t hp = -1;
            const std::uintptr_t points = game::player_points(g_statics);
            if (!points || !game::read(points, hp)) hp = -1;
            if (player_hp > 0 && hp == 0) {
                attempts.clear();  // the area reloads with new characters
                clear_rekilled();
                force_full = true;
                after_death = true;
                death_tick = GetTickCount();
                prevented_after_death = 0;
                wchar_t message[200]{};
                swprintf_s(message,
                    L"Enemies stay dead: you died. %zu enemies listed; the ones in this area are killed again as "
                    L"the game reloads it.", dead.size());
                log(message);
            }
            if (hp >= 0) player_hp = hp;

            float play_time = -1.0f;
            const std::uintptr_t animation = game::player_anim_id(g_statics);
            if (!animation || !game::read(animation + 0x4, play_time)) play_time = -1.0f;
            if (play_time != player_play_time) {
                player_play_time = play_time;
                player_play_tick = GetTickCount();
            }
            if (!fade_slot && g_chr_disable && static_cast<LONG>(GetTickCount() - next_fade_lookup) >= 0) {
                fade_slot = game::find_singleton("CSFade", fade_logged ? nullptr : g_log);
                fade_logged = true;
                next_fade_lookup = GetTickCount() + 5000;
            }
            if (fade_slot && !screen_faded(fade_slot)) {
                if (!clear_since) clear_since = GetTickCount() | 1;
            } else {
                clear_since = 0;
            }
            const bool running_now = play_time >= 0.0f && GetTickCount() - player_play_tick < 500 &&
                clear_since && GetTickCount() - clear_since >= 1000;
            if (running_now && !world_running) force_full = true;  // disable the listed ones at once
            world_running = running_now;

            // Deaths the game paid for (death hook).
            std::vector<DeathEvent> deaths;
            AcquireSRWLockExclusive(&g_death_lock);
            deaths.swap(g_death_events);
            ReleaseSRWLockExclusive(&g_death_lock);
            for (const DeathEvent& d : deaths) {
                if (d.chr == player) continue;
                if (!d.readable) {
                    line(L"Enemies stay dead: death seen, but the character could not be read.");
                    continue;
                }
                const EnemyState& s = d.state;
                const DeadEnemy key = s.key();
                const wchar_t* reason = not_kept_reason(s);
                const bool listed = dead.count(key) != 0;
                const bool recording = !reason && !listed;
                wchar_t message[360]{};
                swprintf_s(message,
                    L"Enemies stay dead: death seen (%ls) block %08X index %u NpcParam %d team %u entity %u "
                    L"HP %d/%d: %ls%ls.",
                    d.kind ? L"runes only" : L"item lot and runes", s.block, key.index, s.npc_param, s.team,
                    s.entity, s.hp, s.hp_max,
                    reason ? reason : recording ? L"recorded" : L"already listed",
                    d.suppressed ? L", killed again on load: payout skipped" : L"");
                line(message);
                if (recording) {
                    dead.insert(key);
                    ++recorded;
                    dirty = true;
                    force_full = true;  // a loaded character may share the new key
                }
            }
            const LONG dropped = InterlockedExchange(&g_death_dropped, 0);
            if (dropped) {
                wchar_t message[120]{};
                swprintf_s(message, L"Enemies stay dead: %ld deaths dropped (queue full).", dropped);
                log(message);
            }

            // Listed enemies that are loaded again. With an empty list there is
            // nothing to kill again, so the ChrSets are only read for the summary.
            // The 30 s summary is a diagnostic line: only with the log on.
            const bool summary_due = g_diagnostics && static_cast<LONG>(GetTickCount() - next_summary) >= 0;
            if (dead.empty() && !summary_due) {
                attempts.clear();
                seen.clear();
            } else {
                // Full pass every second, for the summary and after a change of
                // list or a death; otherwise only new, changed or re-killed characters.
                const bool full = summary_due || force_full || static_cast<LONG>(GetTickCount() - next_full) >= 0;
                if (full) {
                    next_full = GetTickCount() + kFullPassMs;
                    force_full = false;
                }
                loaded.clear();
                const int sets = game::for_each_loaded_chr(g_statics, &collect_chr, &loaded);
                std::sort(loaded.begin(), loaded.end());
                loaded.erase(std::unique(loaded.begin(), loaded.end()), loaded.end());
                RespawnPass pass{};
                pass.world_running = world_running;
                pass.use_disable = g_chr_disable && fade_slot;
                pass.player = player;
                pass.dead = &dead;
                pass.attempts = &attempts;
                seen_next.clear();
                std::size_t known = 0;
                for (const std::uintptr_t chr : loaded) {
                    ChrSeen now{};
                    const bool readable = chr_signature(chr, now);
                    while (known < seen.size() && seen[known].chr < chr) ++known;
                    const bool same = readable && known < seen.size() && seen[known].chr == chr &&
                        seen[known].handle == now.handle && seen[known].hp == now.hp &&
                        seen[known].flags == now.flags && seen[known].enable == now.enable;
                    try {
                        seen_next.push_back(now);
                    } catch (...) {
                    }
                    if (full || !same || attempts.count(chr)) respawn_pass_visit(chr, &pass);
                }
                seen.swap(seen_next);
                for (const std::uintptr_t chr : pass.disabled_now) {
                    try {
                        disabled.insert(chr);
                    } catch (...) {
                    }
                }
                for (const auto& entry : pass.prevented) {
                    const EnemyState& s = entry.second;
                    wchar_t message[300]{};
                    swprintf_s(message,
                        L"Enemies stay dead: respawn prevented block %08X index %u NpcParam %d team %u entity %u "
                        L"HP %d/%d: %ls%ls.",
                        s.block, s.selector & 0xFFFFFu, s.npc_param, s.team, s.entity, s.hp, s.hp_max,
                        pass.use_disable ? L"disabled" : L"killed again", after_death ? L" (after your death)" : L"");
                    line(message);
                }
                if (!pass.prevented.empty()) {
                    prevented += static_cast<int>(pass.prevented.size());
                    if (after_death) prevented_after_death += static_cast<int>(pass.prevented.size());
                    dirty = true;  // reported with the next save
                }
                attempts = std::move(pass.attempts_now);
                if (full) alive_listed_now = pass.alive_listed;

                if (summary_due) {
                    next_summary = GetTickCount() + 30000;
                    std::wstring teams;
                    for (int i = 0; i < 64; ++i)
                        if (pass.teams[i])
                            teams += L" " + std::to_wstring(i) + (i == 63 ? L"+:" : L":") + std::to_wstring(pass.teams[i]);
                    wchar_t message[1400]{};
                    swprintf_s(message,
                        L"Enemies stay dead: %d ChrSets, %d loaded characters (%d unreadable), %d listed alive now, "
                        L"%d listed disabled, %zu listed; teams%ls.",
                        sets, pass.characters, pass.unreadable, pass.alive_listed, pass.kept_disabled, dead.size(),
                        teams.c_str());
                    log(message);
                }
            }
            if (dead.empty()) alive_listed_now = 0;
            // Once the player is back on their feet for a while, report the death.
            if (after_death && player_hp > 0 && GetTickCount() - death_tick >= 45000) {
                after_death = false;
                wchar_t message[240]{};
                swprintf_s(message,
                    L"Enemies stay dead: 45 s after your death: %d respawns prevented, %d listed enemies alive now "
                    L"(0 expected).", prevented_after_death, alive_listed_now);
                log(message);
            }
            if (dirty && GetTickCount() - last_save >= 3000) flush();
        } catch (...) {
        }
    }
}

// Puts a visible mouse pointer on Yes once a Yes/No box opens. The boxes are
// 1920x1080 movies centred in the window: 01_010_MessageBox (centre) and
// 01_011_MessageBox_Small (bottom). Yes is Item_0_0, 220 px left of the
// centre, about 400 x 34 px; its height moves with the number of text lines in
// 01_010 (frames 0-10), and the point used is on it for one to three lines.
// With a controller the pointer is hidden and left alone.
void put_pointer_on_yes(std::uint64_t movie) noexcept {
    const auto skip = [](const wchar_t* why, const wchar_t* box) {
        if (!g_diagnostics) return;
        wchar_t message[200]{};
        swprintf_s(message, L"Yes/No box (%ls): mouse pointer not moved: %ls.", box, why);
        log(message);
    };
    wchar_t name[24]{};
    if (!movie || !game::safe_read(static_cast<std::uintptr_t>(movie), name, sizeof(name) - sizeof(wchar_t))) {
        wchar_t address[40]{};
        swprintf_s(address, L"movie %llX", static_cast<unsigned long long>(movie));
        skip(L"movie name not readable", address);
        return;
    }
    for (wchar_t& c : name)
        if (c && (c < 0x20 || c > 0x7E)) c = L'?';
    double stage_y{};
    if (std::wcsncmp(name, L"01_010_", 7) == 0) stage_y = 585.0;
    else if (std::wcsncmp(name, L"01_011_", 7) == 0) stage_y = 872.0;
    else return skip(L"unknown box layout", name);
    HWND window = GetForegroundWindow();
    DWORD pid{};
    GetWindowThreadProcessId(window, &pid);
    if (pid != GetCurrentProcessId()) return skip(L"game window not in front", name);
    CURSORINFO cursor{sizeof(cursor)};
    if (!GetCursorInfo(&cursor) || !(cursor.flags & CURSOR_SHOWING))
        return skip(L"Windows pointer hidden (controller, or the game draws its own)", name);
    RECT client{};
    if (!GetClientRect(window, &client) || client.right <= 0 || client.bottom <= 0)
        return skip(L"no window size", name);
    const double scale = (std::min)(client.right / 1920.0, client.bottom / 1080.0);
    POINT point{static_cast<LONG>(client.right / 2.0 + (740.0 - 960.0) * scale),
        static_cast<LONG>(client.bottom / 2.0 + (stage_y - 540.0) * scale)};
    if (!ClientToScreen(window, &point) || !SetCursorPos(point.x, point.y))
        return skip(L"SetCursorPos failed", name);
    if (g_diagnostics) {
        wchar_t message[200]{};
        swprintf_s(message, L"Yes/No box (%ls): mouse pointer moved onto Yes at %ld,%ld (window %ldx%ld).",
            name, point.x, point.y, client.right, client.bottom);
        log(message);
    }
}

DWORD WINAPI dialog_pointer_worker(void*) noexcept {
    std::uint32_t seen = g_dialog_yes_data->built;
    std::uint32_t seen_reads = g_dialog_yes_data->reads;
    perf::Loop loop(L"dialog_pointer");
    for (;;) {
        if (!value_of(dialog_yes_default)) {
            seen = g_dialog_yes_data->built;
            seen_reads = g_dialog_yes_data->reads;
            idle_wait(loop);
            continue;
        }
        loop.sleep(50);
        const std::uint32_t reads = g_dialog_yes_data->reads;
        if (reads != seen_reads) {
            seen_reads = reads;
            if (g_diagnostics) {
                wchar_t name[24]{};
                game::safe_read(static_cast<std::uintptr_t>(g_dialog_yes_data->movie), name,
                    sizeof(name) - sizeof(wchar_t));
                for (wchar_t& c : name)
                    if (c && (c < 0x20 || c > 0x7E)) c = L'?';
                wchar_t message[200]{};
                swprintf_s(message, L"Message box created (%ls): %u buttons, first button %d.", name,
                    g_dialog_yes_data->buttons, static_cast<int>(g_dialog_yes_data->value));
                log(message);
            }
        }
        const std::uint32_t built = g_dialog_yes_data->built;
        if (built == seen) continue;
        seen = built;
        if (g_diagnostics) log(L"Yes/No box opened: first button set to Yes.");
        loop.sleep(150);  // the box opens on the next frames
        try {
            put_pointer_on_yes(g_dialog_yes_data->movie);
        } catch (...) {
        }
    }
}

// Holds the dodge and parry frames on past their end (see
// install_window_hooks). The game's window is measured in the animation's own
// time, from its first to its last flagged frame (the start estimated back
// from the first one seen: the flag hooks do not run on every frame, r138
// test). The extra time, that length times (percent - 100) %, then runs in
// real time and is kept even if the next animation starts (a roll ends about
// 0.1 s after its own frames, r138 test), until the character changes (death,
// loading). Polled every 4 ms while either option is above 100 %.
constexpr float kMaxWindowSeconds = 1.0f;

struct WindowTrack {
    const wchar_t* name;
    const wchar_t* kind;
    Id option;
    volatile std::uint32_t* seen;
    volatile std::uint32_t* extend;
    volatile std::uint32_t* injected;
    std::uint32_t last_seen{};
    bool natural{}, extending{};
    std::int32_t window_anim{};
    std::uint32_t window_start{}, observed_seen{};  // flagged frames before the window, at the first look
    float observed_time{}, last_natural_time{};
    std::uint32_t clears_at{}, injected_at{};
    std::uintptr_t window_module{};
    ULONGLONG last_natural_tick{}, extend_until{};

    void finish() noexcept {
        if (extending && g_diagnostics) {
            wchar_t message[160]{};
            swprintf_s(message, L"%ls frames: animation %d done; %u frames, %u of them held on.", name, window_anim,
                g_windows->clears - clears_at, *injected - injected_at);
            log(message);
        }
        *extend = 0;
        natural = extending = false;
    }

    void step(std::uintptr_t module, ULONGLONG now) noexcept {
        const int percent = value_of(option);
        if (percent <= 100) {
            finish();
            last_seen = *seen;
            return;
        }
        if ((natural || extending) && module != window_module) {  // died or reloaded
            finish();
            return;
        }
        const std::uint32_t current = *seen;
        if (current != last_seen) {
            const std::uintptr_t anim = game::player_anim_id(g_statics);
            std::int32_t id{};
            float time{};
            if (!anim || !game::read(anim, id) || !game::read(anim + 4, time)) {
                last_seen = current;
                return;
            }
            if (!natural || id != window_anim || time < last_natural_time) {  // a new window
                finish();
                natural = true;
                window_anim = id;
                window_module = module;
                window_start = last_seen;
                observed_seen = current;
                observed_time = time;
                clears_at = g_windows->clears;
                injected_at = *injected;
            }
            last_seen = current;
            last_natural_time = time;
            last_natural_tick = now;
            *extend = 1;  // already on: no gap when the game's window ends
            return;
        }
        if (natural && now - last_natural_tick >= 50) {  // the game's window is over
            natural = false;
            extending = true;
            const std::uint32_t before = observed_seen - window_start;  // flagged frames before the first look
            const std::uint32_t timed = last_seen - observed_seen;
            const float per_frame = timed && last_natural_time > observed_time
                ? (last_natural_time - observed_time) / timed : 1.0f / 60.0f;
            const float start = (std::max)(0.0f, observed_time - before * per_frame);
            const float length = last_natural_time - start + per_frame;
            // Dodges and parries are short (a roll 0.45 s, a parry 0.25 s); longer
            // windows of the same flags belong to other actions (r138 test: 2 s in
            // c0000 063000, which became 8 s more at 500 %) and are left alone.
            if (length > kMaxWindowSeconds) {
                if (g_diagnostics) {
                    wchar_t message[160]{};
                    swprintf_s(message, L"%ls frames: animation %d, game window %.3f s: not a %ls, left as it is.",
                        name, window_anim, length, kind);
                    log(message);
                }
                *extend = 0;
                extending = false;
                return;
            }
            const auto extra_ms =
                static_cast<ULONGLONG>(length * 1000.0f * static_cast<float>(percent - 100) / 100.0f);
            extend_until = last_natural_tick + extra_ms;
            if (g_diagnostics) {
                wchar_t message[200]{};
                swprintf_s(message,
                    L"%ls frames: animation %d, game window %.3f-%.3f s (%.3f s), held %llu ms more (%d%%).", name,
                    window_anim, start, last_natural_time, length, extra_ms, percent);
                log(message);
            }
        }
        if (extending && now >= extend_until) finish();
    }
};

// Bits held for the whole animation: knockdowns (c0000 017000-017999, getting
// up included: their own flag 95 has no bit in the action flag module) get the
// dodge bit, item use (050000-050999: flasks, consumables) super armour.
std::uint64_t held_bits_for(std::int32_t anim) noexcept {
    const std::int32_t local = anim % 1000000;
    std::uint64_t bits = 0;
    if (value_of(knockdown_invincible) && local >= 17000 && local < 18000) bits |= 0x2;
    if (value_of(item_super_armor) && local >= 50000 && local < 51000) bits |= 0x100;
    return bits;
}

DWORD WINAPI window_worker(void*) noexcept {
    perf::Loop loop(L"dodge_parry");
    WindowTrack tracks[]{
        {L"Dodge", L"dodge", dodge_frames, &g_windows->seen, &g_windows->extend, &g_windows->injected},
        {L"Parry", L"parry", parry_frames, &g_windows->parry_seen, &g_windows->parry_extend, &g_windows->parry_injected},
    };
    for (WindowTrack& track : tracks) track.last_seen = *track.seen;
    for (;;) {
        if (value_of(dodge_frames) <= 100 && value_of(parry_frames) <= 100 && !value_of(knockdown_invincible) &&
            !value_of(item_super_armor)) {
            for (WindowTrack& track : tracks) track.step(0, 0);  // ends any extension, follows the counters
            g_windows->held_bits = 0;
            g_windows->player_module = 0;
            idle_wait(loop);
            continue;
        }
        loop.sleep(4);
        try {
            std::uintptr_t module{};
            if (!game::read(game::chain(g_statics.world_chr_man, {game::kPlayerIns, game::kModules, 0x8}), module) ||
                !module) {
                for (WindowTrack& track : tracks) track.finish();
                g_windows->held_bits = 0;
                g_windows->player_module = 0;
                continue;
            }
            g_windows->player_module = module;
            const ULONGLONG now = GetTickCount64();
            for (WindowTrack& track : tracks) track.step(module, now);
            const std::uintptr_t anim = game::player_anim_id(g_statics);
            std::int32_t id{};
            g_windows->held_bits = anim && game::read(anim, id) ? held_bits_for(id) : 0;
        } catch (...) {
            for (WindowTrack& track : tracks) track.finish();
            g_windows->held_bits = 0;
        }
    }
}

void start_thread(LPTHREAD_START_ROUTINE routine, const wchar_t* name) noexcept {
    if (HANDLE thread = CreateThread(nullptr, 0, routine, nullptr, 0, nullptr)) {
        perf::thread_started(name, thread);
        CloseHandle(thread);
    }
}

// ---------------------------------------------------------------------------
// Menu text

struct Text { const wchar_t* label; const wchar_t* help; };

enum Category : std::size_t {
    cat_survival, cat_equipment, cat_resources, cat_movement, cat_torrent, cat_dealt, cat_taken, cat_poise,
    cat_status, cat_weapon, cat_buffs, cat_enemies, cat_spirits, cat_runes, cat_map, cat_world, cat_camera,
    cat_image, cat_display, cat_interface, cat_count
};

// Option texts follow the order of Id. Options marked (beta) depend on game
// structures that are not fully confirmed and may not work every time.
struct Texts {
    Text root;
    Text categories[cat_count];
    Text options[count];
    Text restore;
    Text restore_page;
    const wchar_t* unavailable;
};

const Texts kEnglish{
    {L"Vanilla Settings", L"Extra options for Elden Ring itself. Changes apply instantly. Offline only (EAC off). Options marked (beta) may not always work."},
    {
        {L"Survival", L"Invincibility, no death, fall damage, maximum HP, FP and stamina, regeneration and flasks."},
        {L"Equipment", L"Equipment and weapon weight, stat requirements, spell slots, carry limits and helmet."},
        {L"Resources & Costs", L"Infinite stamina, FP, items and ammunition, and what actions cost."},
        {L"Movement & Speed", L"Your animation speed, overall and per action, dodge invincibility, the parry window, and how high and far you jump."},
        {L"Torrent", L"Torrent's survival, running and jumping."},
        {L"Damage Dealt", L"Your damage, per element, stat scaling, knockback and one-hit kills."},
        {L"Damage Taken", L"Enemy damage to you, per element, knockback, guarding and one-hit deaths."},
        {L"Poise", L"Your poise and enemies' poise, and the poise damage both sides deal."},
        {L"Status Effects", L"How fast each status builds up for everyone, your immunity and enemy immunities."},
        {L"Weapon Effects", L"Add status effects to your attacks, each with its own switch."},
        {L"Buffs & Durations", L"How long buffs, greases and enemy debuffs last, or make them permanent."},
        {L"Enemies", L"Enemy health, speed, behaviour, respawns and lock-on."},
        {L"Spirit Ashes", L"Summoning cost, several spirits at once, and where and when they can be called."},
        {L"Runes, Shops & Upgrades", L"Runes, item drops, shop prices, upgrades and crafting."},
        {L"Map & Travel", L"Graces, map reveal, fast travel, and the mirror and rebirth anywhere."},
        {L"World", L"Time of day and weather."},
        {L"Camera", L"Field of view and camera behaviour."},
        {L"Image", L"Post-processing and level of detail."},
        {L"Display", L"Frame rate, refresh rate and aspect ratio."},
        {L"Interface & System", L"HUD and menu layout, pause, startup logos and Yes/No dialogs."},
    },
    {
        {L"Custom field of view", L"Scale the game's field of view. Applies instantly."},
        {L"Field of view (%)", L"100% is the game's own value. Zoom effects are preserved."},
        {L"Disable camera auto-rotation", L"The camera no longer swings behind you while moving."},
        {L"Disable camera recentering", L"Lock-on without a target no longer snaps the camera forward."},
        {L"Camera distance (%)", L"Scales how far the camera sits behind you. Applies when the camera changes: mount, lock on or move to another area."},
        {L"Remove vignette", L"Removes the darkened screen edges."},
        {L"Remove chromatic aberration", L"Removes the colour fringing at the screen edges."},
        {L"Full animation distance", L"Distant enemies and NPCs animate at full rate. Slightly higher CPU cost."},
        {L"Disable anti-aliasing (TAA)", L"Forces the game's anti-aliasing off, including the temporal filter. Your menu choice is restored when turned off."},
        {L"Disable motion blur", L"Forces the game's motion blur off. Your menu choice is restored when turned off."},
        {L"Disable depth of field", L"Forces the game's depth of field off. Your menu choice is restored when turned off."},
        {L"Invincible", L"You take no damage."},
        {L"Cannot die", L"Damage still lands, but your health never drops below 1."},
        {L"Infinite stamina", L"Actions no longer consume stamina."},
        {L"Infinite FP", L"Spells and skills no longer consume FP."},
        {L"Infinite consumables", L"Using items does not reduce their count."},
        {L"Infinite arrows and bolts", L"Ammunition is not consumed."},
        {L"Equipment weight (%)", L"Scales the weight of weapons and armour. 0% always gives a light load."},
        {L"Poise (%)", L"Scales the poise of your armour. Re-equip a piece of armour to refresh it."},
        {L"Infinite poise", L"Normal hits never stagger you. Applies at once, with or without armour. Grabs still work."},
        {L"No fall damage", L"Falls never hurt or kill you or Torrent, whatever the height, on foot or riding. In bottomless pits you keep falling."},
        {L"Status immunity", L"Poison, rot, bleed, blight, frost, sleep and madness never build up on you, not even from one big hit. An effect already running when you turn it on still wears off as usual. Enemies are unaffected."},
        {L"Damage dealt (%)", L"Scales your attacks and spells. Applies from the next hit."},
        {L"Damage taken (%)", L"Scales enemy attacks. Also affects attacks between NPCs and from spirit ashes."},
        {L"Poise damage dealt (%)", L"Scales the poise damage of your attacks, weapons and spells: how fast they stagger enemies and break their stance. Applies to your next hit."},
        {L"Poise damage taken (%)", L"Scales the poise damage of enemy attacks: how fast they stagger you. Also affects NPCs and spirit ashes. Applies to the next hit."},
        {L"Stamina cost (%)", L"Scales the stamina used by your attacks and actions."},
        {L"Spell FP cost (%)", L"Scales the FP cost of sorceries and incantations."},
        {L"Status buildup (%)", L"Poison, rot, bleed, blight, frost, sleep and madness. Applies to everyone, you included."},
        {L"One-hit kills", L"Your hits defeat enemies instantly."},
        {L"Weapon buff duration (%)", L"Greases and weapon enchantments. Applies to the next buff you cast or use."},
        {L"Self buff duration (%)", L"Buff spells and incantations, boluses and other timed effects on you and your allies. Applies to the next one."},
        {L"Enemy debuff duration (%)", L"Timed effects aimed at enemies, such as defence reductions. Also affects enemy debuffs on you."},
        {L"Overall speed (%)", L"Scales all of your animations: movement, attacks, rolls and spells."},
        {L"Attack speed (%)", L"Weapon attack animations. Multiplies the overall speed."},
        {L"Spell speed (%)", L"Spell casting animations. Multiplies the overall speed."},
        {L"Roll and dodge speed (%)", L"Rolls and backsteps. Multiplies the overall speed."},
        {L"Movement speed (%)", L"Walking, running and sprinting. Multiplies the overall speed."},
        {L"Jump speed (%)", L"Jump take-off, fall and landing. Jumping attacks use the attack speed. Multiplies the overall speed."},
        {L"Enemy health (%)", L"Scales enemy health. Applies to enemies loaded afterwards: rest at a grace."},
        {L"Enemy poise (%)", L"Scales how much enemies withstand before staggering. Applies to enemies loaded afterwards: rest at a grace."},
        {L"Enemies have infinite poise", L"Enemies, NPCs and spirit ashes are never staggered. Applies after resting at a grace."},
        {L"Enemy speed (%)", L"Scales the animation speed of every other character: enemies, NPCs and spirit ashes. Torrent keeps its own speed. Applies instantly."},
        {L"Enemies do not attack", L"AI-controlled characters stop attacking."},
        {L"Enemies do not move", L"AI-controlled characters stand still."},
        {L"Disable enemy AI", L"AI-controlled characters stop thinking."},
        {L"Torrent cannot die", L"Your mount never falls."},
        {L"Torrent run speed (%)", L"Scales Torrent's animation speed when it is not jumping: walking, galloping, turning and landing. Jumps use Torrent jump speed. Applies instantly."},
        {L"Keep runes on death", L"You no longer drop runes when you die."},
        {L"Runes gained (%)", L"Scales runes from defeated enemies. Applies to enemies loaded afterwards."},
        {L"Item drop rate (%)", L"Scales the chance that enemies drop items. 200% roughly doubles uncommon drops. Stacks with item discovery."},
        {L"Spirit ash cost (%)", L"Scales the FP and HP needed to summon spirit ashes. 0% makes them free."},
        {L"Upgrade cost (%)", L"Scales the runes the smith charges to upgrade weapons and Roderika charges to upgrade spirit ashes. Materials are still needed unless Upgrade without materials is on. 0% makes it free."},
        {L"Pause (Esc / Start)", L"Press Esc or the controller's Start button to pause or resume the game. Both also open the game menu."},
        {L"Set time of day", L"Moves the world clock to the hour below, like resting at a grace. Changing the hour applies it again."},
        {L"Hour", L"Hour of the day (0-23) used by Set time of day."},
        {L"Freeze time", L"Stops the world clock, so the current time of day stays until turned off."},
        {L"Force weather", L"Switches to the weather below at once, the way the game's own events do, and keeps it. Places with a fixed weather (some bosses and dungeons) keep theirs, and nothing shows indoors or underground. Turning it off goes back to the usual weather."},
        {L"Weather", L"Weather kept while the option above is on."},
        {L"Show all graces", L"Every Site of Grace appears on the map and can be travelled to. Nothing is saved: turning it off restores your discovered graces."},
        {L"Reveal the whole map", L"Shows every map fragment area. Nothing is saved: turning it off restores your own map."},
        {L"Fast travel anywhere", L"Allows fast travel from inside legacy dungeons and other closed areas. Nearby enemies still block it."},
        {L"Resummon without resting", L"After a spirit falls you can call it again without resting at a grace. Turning it off restores the usual limit."},
        {L"Summon anywhere", L"Every summoning monument reaches across its whole area, so you can summon wherever the game allows spirits, and ashes are no longer dismissed for walking away from it. Turning it off restores the usual reach."},
        {L"Several ashes at once", L"Multiple Spirit Ashes, built in. Using a different spirit ash keeps the ones already summoned, each paying its FP; using a summoned ash's item again dismisses only that one with a short fade, even with too little FP. No confirmations appear, summoned ashes follow you into new areas and through doors, and each gets a compact row with its name and HP on the HUD (up to 6; the rows need the option on when the game starts). Off, summoning works as in the base game."},
        {L"Most ashes at once", L"How many spirit ashes can be summoned together with Several ashes at once. A summon with several spirits counts as one. The HUD shows up to 6; many ashes can lower performance."},
        {L"Ashes stay after a boss falls", L"Monuments tied to a boss no longer dismiss your spirit ashes when it dies, and stay usable afterwards, also in arenas of bosses you already beat. Applies after the area reloads (rest at a grace)."},
        {L"Dismiss all ashes", L"With Several ashes at once, dismisses every summoned spirit ash with a short fade. Works as a button; it can also be bound to a key in [Hotkeys] of NativeTweaks.ini (SpiritDismissAll = F5)."},
        {L"Cost of extra ashes (%)", L"With Several ashes at once, the FP each ash costs when another one is already summoned, in % of its own cost. 100% is the normal cost; the first ash always costs it."},
        {L"Damage lost per extra ash (%)", L"With Several ashes at once, each summoned ash deals this much less damage for every other ash summoned (down to 25%). 0% keeps their full damage."},
        {L"Unlock frame rate", L"Raises the 60 FPS limit."},
        {L"Frame rate limit", L"Maximum FPS while the frame rate is unlocked."},
        {L"Allow ultrawide", L"Removes the 16:9 black bars on wider monitors. Reapply your resolution if needed."},
        {L"Fullscreen refresh rate (Hz)", L"Refresh rate requested in exclusive fullscreen; the game normally asks for 60 Hz. Set your monitor's rate. 0 leaves it to the frame rate unlock. Re-enter fullscreen to apply."},
        {L"Skip intro logos", L"Skips the logos at startup. Takes effect on the next launch."},
        {L"Weapon: hemorrhage", L"Your attacks build up hemorrhage (bleed). Uses the buildup of the game's own weapons. Applies from the next hit, also to spells and skills that hit. Up to five effects per attack."},
        {L"Weapon: poison", L"Your attacks build up poison. Uses the buildup of the game's own weapons. Applies from the next hit, also to spells and skills that hit. Up to five effects per attack."},
        {L"Weapon: scarlet rot", L"Your attacks build up scarlet rot. Uses the buildup of the game's own weapons. Applies from the next hit, also to spells and skills that hit. Up to five effects per attack."},
        {L"Weapon: frostbite", L"Your attacks build up frostbite. Uses the buildup of the game's own weapons. Applies from the next hit, also to spells and skills that hit. Up to five effects per attack."},
        {L"Weapon: sleep", L"Your attacks build up sleep. Uses the buildup of the game's own weapons. Applies from the next hit, also to spells and skills that hit. Up to five effects per attack."},
        {L"Physical damage (%)", L"Scales the physical part of your attacks and spells. Multiplies Damage dealt. Applies from the next hit."},
        {L"Magic damage (%)", L"Scales the magic part of your attacks and spells. Multiplies Damage dealt. Applies from the next hit."},
        {L"Fire damage (%)", L"Scales the fire part of your attacks and spells. Multiplies Damage dealt. Applies from the next hit."},
        {L"Lightning damage (%)", L"Scales the lightning part of your attacks and spells. Multiplies Damage dealt. Applies from the next hit."},
        {L"Holy damage (%)", L"Scales the holy part of your attacks and spells. Multiplies Damage dealt. Applies from the next hit."},
        {L"Weapon stat scaling (%)", L"Scales how much Strength, Dexterity, Intelligence and Faith add to weapon damage. Re-equip your weapon to refresh it."},
        {L"Weapon weight (%)", L"Scales the weight of weapons only. Multiplies Equipment weight."},
        {L"Physical damage taken (%)", L"Scales the physical part of enemy attacks. Multiplies Damage taken."},
        {L"Magic damage taken (%)", L"Scales the magic part of enemy attacks. Multiplies Damage taken."},
        {L"Fire damage taken (%)", L"Scales the fire part of enemy attacks. Multiplies Damage taken."},
        {L"Lightning damage taken (%)", L"Scales the lightning part of enemy attacks. Multiplies Damage taken."},
        {L"Holy damage taken (%)", L"Scales the holy part of enemy attacks. Multiplies Damage taken."},
        {L"Hemorrhage buildup (%)", L"How fast hemorrhage builds up, on enemies and on you. 0% turns it off. Multiplies Status buildup."},
        {L"Poison buildup (%)", L"How fast poison builds up, on enemies and on you. 0% turns it off. Multiplies Status buildup."},
        {L"Scarlet rot buildup (%)", L"How fast scarlet rot builds up, on enemies and on you. 0% turns it off. Multiplies Status buildup."},
        {L"Frostbite buildup (%)", L"How fast frostbite builds up, on enemies and on you. 0% turns it off. Multiplies Status buildup."},
        {L"Sleep buildup (%)", L"How fast sleep builds up, on enemies and on you. 0% turns it off. Multiplies Status buildup."},
        {L"Madness buildup (%)", L"How fast madness builds up, on enemies and on you. 0% turns it off. Multiplies Status buildup."},
        {L"Death blight buildup (%)", L"How fast death blight builds up, on enemies and on you. 0% turns it off. Multiplies Status buildup."},
        {L"Permanent weapon buffs", L"Greases and weapon enchantments never wear off. Applies to the next buff you use. Overrides Weapon buff duration."},
        {L"Permanent self buffs", L"Buff spells, incantations, boluses and other timed effects on you and your allies never wear off. Applies to the next one. Overrides Self buff duration."},
        {L"Perfect guard", L"Every weapon and shield blocks all physical, magic, fire and lightning damage. Holy damage keeps its usual guard. Re-equip your shield to refresh it."},
        {L"Knockback dealt (%)", L"How far your hits push enemies. Applies from the next hit."},
        {L"Knockback taken (%)", L"How far enemy hits push you. Also affects NPCs and spirit ashes."},
        {L"Effects on enemy attacks too", L"Enemies, NPCs and spirit ashes also apply the weapon effects turned on above. Makes the game harder."},
        {L"Remove enemy immunities", L"Enemies immune to poison, rot, bleed, frostbite or sleep get a typical resistance instead. Madness and death blight still only affect player-type characters. Applies to enemies loaded afterwards: rest at a grace."},
        {L"Crimson flask healing (%)", L"Scales the HP restored by the Flask of Crimson Tears, at every upgrade level. 0% heals nothing."},
        {L"Cerulean flask FP (%)", L"Scales the FP restored by the Flask of Cerulean Tears, at every upgrade level. 0% restores nothing."},
        {L"Spell stamina cost (%)", L"Scales the stamina used by sorceries and incantations, charged casts included."},
        {L"Spells use one slot", L"Spells that take 2 or 3 memory slots take 1. Other spells are unchanged."},
        {L"Ignore spell requirements", L"Cast spells without the Intelligence and Faith they ask for. Arcane requirements are not included."},
        {L"Skill FP cost (%)", L"Scales the FP used by Ashes of War, charged stages and follow-ups included. 0% makes them free and keeps at least 1 FP, which the game needs for the full skill."},
        {L"Carry up to 999 of each item", L"Stackable items hold 999 on you and in the chest. Unique items (key items, whistle, flasks) are unchanged."},
        {L"Item use speed (%)", L"Drinking flasks, using consumables and picking up items. Multiplies the overall speed."},
        {L"Shop prices (%)", L"Scales what merchants and other shops charge, in runes or in items such as Dragon Hearts. 0% makes everything free. Trades for remembrances still need them. Applies the next time you open a shop."},
        {L"Craft without materials", L"Crafting recipes need no materials. You still need the cookbook and the crafting kit. Turning it off restores the usual materials."},
        {L"Weapon stat requirements (%)", L"Scales the Strength, Dexterity, Intelligence, Faith and Arcane needed to wield weapons, shields, staves and seals. 0% removes them. Re-equip your weapon to refresh it."},
        {L"Upgrade without materials", L"Upgrading weapons (smithing and somber stones) and spirit ashes (glovewort) needs no materials. Runes are still charged unless Upgrade cost is 0%. Turning it off restores the usual materials."},
        {L"Hide helmet", L"Your head armour looks like a bare head, so your face and hair show. Defence, poise, weight and effects are unchanged. Re-equip the helmet or pass a loading screen to refresh it. Full-body outfits are not affected."},
        {L"Maximum HP (%)", L"Scales your maximum HP. Level-ups and equipment keep working on top; 100% gives the game's own value back."},
        {L"Maximum FP (%)", L"Scales your maximum FP. Level-ups and equipment keep working on top; 100% gives the game's own value back."},
        {L"Maximum stamina (%)", L"Scales your maximum stamina. Level-ups and equipment keep working on top; 100% gives the game's own value back."},
        {L"HP regeneration (points/s)", L"Restores this many HP every second."},
        {L"FP regeneration (points/s)", L"Restores this many FP every second."},
        {L"Stamina regeneration (points/s)", L"Adds this much stamina every second, on top of the normal recovery."},
        {L"HUD to screen edges", L"On screens wider than 16:9, moves the gauges, equipment, runes and other corner elements to the screen edges. Use with Allow ultrawide. Restart the game to apply: it loads the HUD once at startup. Turn it off if another mod already places the HUD for ultrawide."},
        {L"HUD scale (%)", L"Resizes the gauges, equipment, compass, boss bars, runes and other corner elements, each one from its own corner. Restart the game to apply: it loads the HUD once at startup."},
        {L"Menus to screen edges", L"On screens wider than 16:9, moves the pause menu and the other menus (equipment, inventory, status, shops, level up, spells, storage, options, loading and title screens) to the screen edges and widens their backgrounds. Restart the game to apply: it loads the menus once at startup. Turn it off if another mod replaces these menus."},
        {L"Enemies stay dead", L"Enemies you kill stay dead after resting at a grace, passing time, dying, fast travel or loading the save. Each save keeps its list in DLLs\\NativeTweaks\\dead-enemies; an enemy that loads again is removed at once: no body, no runes or drops. Bosses and NPCs work as usual, and enemies still alive reset as usual. Turn it off and the next rest brings everyone back; turning it on again brings the list back too. Delete the save's file to start over."},
        {L"Yes/No questions start on Yes (beta)", L"BETA: Yes/No questions open with the cursor on Yes instead of No: travel, quit, use an item and the like; with the mouse, the pointer is moved onto Yes too. It applies to every Yes/No question, risky ones too, so check before confirming. Other windows, such as item quantities, do not change."},
        {L"Hit downed enemies", L"Like Nightreign, enemies knocked to the ground by a critical hit or a riposte take damage while they lie there and get up. They get super armour instead, so your hits do not stagger them again, but they still take posture damage. Spirit ashes can also hit them during the critical hit. You keep your own protection when an enemy grabs you."},
        {L"Torrent jump speed (%)", L"Scales Torrent's animation speed during jumps and double jumps, spiritspring jumps included. At 100% the double jump is as easy to get as in the base game, even with a faster run speed. Applies instantly."},
        {L"Jump height (%)", L"Scales how high you jump on foot. 100% is the game's height (about 1.1 m). The animation plays at its normal speed and a higher jump stays longer in the air, as with gravity (400% doubles the time), and falls without slowing down to the ground, so landings and fall damage work as usual. A jumping attack high up falls from there."},
        {L"Jump distance (%)", L"Scales your speed through the air in a jump on foot, standing, running or sprinting, and so how far it carries you. At 100% you move at the game's speed, and a higher jump already goes farther. The time in the air depends only on the height, so above 100% the jump is faster, like a sprint jump."},
        {L"Torrent jump height (%)", L"Scales how high Torrent jumps, double jumps included. The animation plays at its normal speed and a higher jump rises longer; from the top Torrent falls as fast as it rose, so it never hangs in the air, and the double jump stays as easy. Spiritspring jumps keep their height."},
        {L"Torrent jump distance (%)", L"Scales Torrent's speed through the air in a jump, double jumps included, and so how far it carries you. At 100% Torrent keeps the game's speed, and a higher jump already goes farther. Spiritspring jumps keep their distance."},
        {L"No lock-on", L"You cannot lock on to enemies or other characters, and a lock you hold is released. Applies at once."},
        {L"Break poise in one hit", L"Every hit of yours staggers enemies and breaks the stance of bosses that can be broken. Applies from your next hit and overrides Poise damage dealt."},
        {L"Instant status effects", L"Any hit that builds up poison, rot, bleed, death blight, frost, sleep or madness triggers it at once, for everyone: you, enemies and spirit ashes. Overrides the buildup sliders; with Status immunity it only affects others."},
        {L"Die in one hit", L"Any enemy hit that deals damage kills you. Hits between enemies and from your spirit ashes share this data, so they kill in one hit too. Overrides Damage taken; God mode still protects you."},
        {L"Enemies break your poise in one hit", L"Every enemy hit staggers you. Your spirit ashes share this data and also break enemies' poise in one hit. Overrides Poise damage taken."},
        {L"Charged attacks launch enemies", L"Your charged heavy attacks throw enemies through the air instead of staggering them: back, far back or upward. It works with their poise intact. Enemies already on the ground, large enemies and bosses have no throw and keep their own reaction. Far back throws best; Launch distance below sets how far. Applies from your next hit."},
        {L"Every attack launches", L"With the option above on, every attack of yours that makes enemies react throws them the same way, not only charged heavy attacks: light and heavy attacks, skills, spells and thrown items."},
        {L"Launch distance (%) (beta)", L"BETA, still in progress: meant to make launched enemies fly farther. For now it only makes enemies lighter while the launch option above is on (at 200% they weigh half as much, taken when they appear: rest at a grace), and in tests that has not changed how far they fly yet. Leave it at 100% unless you want to experiment."},
        {L"World difficulty", L"Enemies get the strength of a New Game+ cycle, up to NG+7, area by area as in the real New Game+: more health, damage, defence and status resistance, and more runes. Your save stays in its own cycle: bosses, items and endings don't change. In New Game+ it only adds what your cycle lacks. Applies to enemies that appear afterwards: rest at a grace."},
        {L"Keep your runes", L"With World difficulty on, enemies give the runes of your save's own cycle instead of the harder cycle's."},
        {L"Open the mirror", L"Opens the appearance screen of the Roundtable Hold mirror. Use it out of combat."},
        {L"Rebirth", L"Opens Rennala's rebirth screen to reallocate your attributes. No Larval Tear needed. Use it out of combat."},
        {L"Boss health (%)", L"Scales the health of bosses only: main, field, dungeon and DLC bosses. Multiplies Enemy health, so with Enemy health at 100% only bosses change. Applies to bosses loaded afterwards: rest at a grace."},
        {L"Keep characters clean (beta)", L"BETA: for now it also applies to enemies; the aim is your character only. Blood, mud and scorch marks no longer stick to characters' bodies, yours and enemies'. Splatters still appear on the ground and walls. Applies to new hits; marks already on you wear off as usual."},
        {L"Hide buff effects", L"Your own buffs (incantations, sorceries, boluses, talismans and similar) no longer show particles or a glow on your body. They work exactly the same, keep their sound, and invisibility still works. Weapon buffs keep their look (see the option below). Applies to the next buff you use."},
        {L"Hide weapon buff effects", L"Greases and weapon enchantments no longer show their glow or particles on your weapon. They work exactly the same and keep their sound. Applies to the next grease or enchantment you use."},
        {L"Dodge invincibility (%) (beta)", L"BETA: makes the invincibility of rolls, backsteps and dodge skills last longer. 100% is the game's (14 frames for a light or medium roll, 13 for a heavy one); 200% doubles it. The extra time follows the dodge's own invincibility and lasts even if you act right after. Your character only."},
        {L"Parry window (%) (beta)", L"BETA: makes the parry window of shields and parry skills last longer. 100% is the game's; 200% doubles it. The extra time follows the parry's own window. Your character only."},
        {L"Invincible while knocked down (beta)", L"BETA: hits do not hurt you while a big hit has you on the ground, getting up included, as during a roll. Big knockdowns only, not grabs. Your character only."},
        {L"Super armour while using items (beta)", L"BETA: the game's super armour while you drink a flask or use an item: weaker hits no longer interrupt you, very strong ones still can, and you still take the damage. Your character only."},
    },
    {L"Restore game defaults", L"Turn every vanilla option off and restore the original game values."},
    {L"Restore defaults", L"Restore the options on this page to the original game values."},
    L"Unavailable in this game version: the data it changes was not found.",
};

const Texts kSpanish{
    {L"Ajustes vanilla", L"Opciones extra del propio Elden Ring. Se aplican al momento. Solo sin conexión (EAC desactivado). Las opciones marcadas (beta) pueden no funcionar siempre."},
    {
        {L"Supervivencia", L"Invencibilidad, no morir, daño por caída, vida, PC y aguante máximos, regeneración y frascos."},
        {L"Equipo", L"Peso del equipo y de las armas, requisitos de atributos, ranuras de hechizos, límite de objetos y casco."},
        {L"Recursos y costes", L"Aguante, PC, objetos y munición infinitos, y lo que cuestan las acciones."},
        {L"Movimiento y velocidad", L"Tu velocidad de animación, general y por acción, la invencibilidad al esquivar, la ventana de parada y la altura y distancia de tus saltos."},
        {L"Torrent", L"Supervivencia, carrera y saltos de Torrent."},
        {L"Daño infligido", L"Tu daño, por elemento, escalado de atributos, retroceso y matar de un golpe."},
        {L"Daño recibido", L"El daño enemigo que recibes, por elemento, retroceso, bloqueo y morir de un golpe."},
        {L"Equilibrio", L"Tu equilibrio y el de los enemigos, y el daño al equilibrio de ambos lados."},
        {L"Estados alterados", L"Acumulación de cada estado para todos, tu inmunidad y la de los enemigos."},
        {L"Efectos de arma", L"Añade estados alterados a tus ataques, cada uno con su interruptor."},
        {L"Mejoras y duraciones", L"Cuánto duran las mejoras, grasas y efectos sobre enemigos, o hacerlos permanentes."},
        {L"Enemigos", L"Vida, velocidad, comportamiento, reaparición y fijado de los enemigos."},
        {L"Cenizas espirituales", L"Coste de invocación, varias cenizas a la vez y dónde y cuándo puedes llamar a los espíritus."},
        {L"Runas, tiendas y mejoras", L"Runas, objetos que sueltan los enemigos, precios de tiendas, mejoras y fabricación."},
        {L"Mapa y viaje", L"Gracias, mapa revelado, viaje rápido, y el espejo y el renacimiento en cualquier lugar."},
        {L"Mundo", L"Hora del día y clima."},
        {L"Cámara", L"Campo de visión y comportamiento de la cámara."},
        {L"Imagen", L"Posprocesado y nivel de detalle."},
        {L"Pantalla", L"FPS, frecuencia de refresco y relación de aspecto."},
        {L"Interfaz y sistema", L"Disposición del HUD y los menús, pausa, logos de inicio y preguntas Sí/No."},
    },
    {
        {L"Campo de visión personalizado", L"Escala el campo de visión del juego. Se aplica al momento."},
        {L"Campo de visión (%)", L"100% es el valor del juego. Los efectos de zoom se conservan."},
        {L"Desactivar autorrotación de cámara", L"La cámara deja de girar sola detrás de ti al moverte."},
        {L"Desactivar recentrado de cámara", L"Fijar sin objetivo ya no recoloca la cámara hacia delante."},
        {L"Distancia de cámara (%)", L"Escala lo lejos que se sitúa la cámara detrás de ti. Se aplica cuando cambia la cámara: montar, fijar objetivo o cambiar de zona."},
        {L"Quitar viñeta", L"Elimina el oscurecimiento de los bordes de la pantalla."},
        {L"Quitar aberración cromática", L"Elimina los bordes de color en los laterales de la pantalla."},
        {L"Animaciones a distancia completa", L"Los enemigos y PNJ lejanos se animan con fluidez completa. Usa algo más de CPU."},
        {L"Desactivar antialiasing (TAA)", L"Desactiva el antialiasing, incluido el filtro temporal. Al quitarlo vuelve tu ajuste del menú."},
        {L"Desactivar desenfoque de movimiento", L"Fuerza el desenfoque de movimiento a desactivado. Al quitarlo se restaura tu ajuste del menú."},
        {L"Desactivar profundidad de campo", L"Fuerza la profundidad de campo a desactivada. Al quitarlo se restaura tu ajuste del menú."},
        {L"Invencible", L"No recibes daño."},
        {L"No morir", L"Recibes los golpes, pero tu vida nunca baja de 1."},
        {L"Aguante infinito", L"Las acciones no gastan aguante."},
        {L"PC infinitos", L"Los hechizos y habilidades no gastan PC."},
        {L"Consumibles infinitos", L"Usar objetos no reduce su cantidad."},
        {L"Flechas y virotes infinitos", L"La munición no se gasta."},
        {L"Peso del equipo (%)", L"Escala el peso de armas y armaduras. Con 0% siempre llevas carga ligera."},
        {L"Equilibrio (%)", L"Escala el equilibrio de tu armadura. Vuelve a equipar una pieza de armadura para actualizarlo."},
        {L"Equilibrio infinito", L"Los golpes normales no te hacen tambalear. Se aplica al momento, con o sin armadura. Los agarres siguen."},
        {L"Sin daño por caída", L"Las caídas nunca te dañan ni te matan a ti ni a Torrent, sea cual sea la altura, a pie o montado. En abismos sigues cayendo."},
        {L"Inmunidad a estados alterados", L"Veneno, putrefacción, hemorragia, muerte, congelación, sueño y locura no se acumulan en ti, ni siquiera con un golpe fuerte. Un efecto que ya tengas al activarla se pasa como siempre. No afecta a enemigos."},
        {L"Daño infligido (%)", L"Escala tus ataques y hechizos. Se aplica desde el siguiente golpe."},
        {L"Daño recibido (%)", L"Escala los ataques enemigos. También afecta a los ataques entre PNJ y a las cenizas espirituales."},
        {L"Daño al equilibrio infligido (%)", L"Escala el daño al equilibrio de tus ataques, armas y hechizos: lo rápido que hacen tambalear a los enemigos y les rompen la postura. Se aplica en el siguiente golpe."},
        {L"Daño al equilibrio recibido (%)", L"Escala el daño al equilibrio de los ataques enemigos: lo rápido que te hacen tambalear. También afecta a PNJ y cenizas espirituales. Se aplica en el siguiente golpe."},
        {L"Coste de aguante (%)", L"Escala el aguante que gastan tus ataques y acciones."},
        {L"Coste de PC de hechizos (%)", L"Escala el coste en PC de hechicerías y encantamientos."},
        {L"Acumulación de estados (%)", L"Veneno, putrefacción, hemorragia, muerte, congelación, sueño y locura. Afecta a todos, también a ti."},
        {L"Matar de un golpe", L"Tus golpes derrotan a los enemigos al instante."},
        {L"Duración de mejoras de arma (%)", L"Grasas y encantamientos de arma. Se aplica a la siguiente mejora que uses."},
        {L"Duración de mejoras propias (%)", L"Hechizos y encantamientos de mejora, bolos y otros efectos temporales sobre ti y tus aliados. Se aplica al siguiente."},
        {L"Duración de efectos sobre enemigos (%)", L"Efectos temporales dirigidos a enemigos, como reducir su defensa. También afecta a los que los enemigos te aplican."},
        {L"Velocidad general (%)", L"Escala todas tus animaciones: movimiento, ataques, esquivas y hechizos."},
        {L"Velocidad de ataque (%)", L"Animaciones de ataque con armas. Se multiplica por la velocidad general."},
        {L"Velocidad de hechizos (%)", L"Animaciones de lanzamiento de hechizos. Se multiplica por la velocidad general."},
        {L"Velocidad de esquiva (%)", L"Volteretas y pasos atrás. Se multiplica por la velocidad general."},
        {L"Velocidad de movimiento (%)", L"Andar, correr y esprintar. Se multiplica por la velocidad general."},
        {L"Velocidad de salto (%)", L"Despegue, caída y aterrizaje. Los ataques en salto usan la de ataque. Se multiplica por la general."},
        {L"Vida de enemigos (%)", L"Escala la vida enemiga. Se aplica a los enemigos que se carguen después: descansa en una gracia."},
        {L"Equilibrio de enemigos (%)", L"Escala cuánto aguantan los enemigos antes de tambalearse. Se aplica tras descansar en una gracia."},
        {L"Enemigos con equilibrio infinito", L"Enemigos, PNJ y cenizas nunca se tambalean por golpes. Se aplica tras descansar en una gracia."},
        {L"Velocidad de enemigos (%)", L"Velocidad de animación de enemigos, PNJ y cenizas espirituales. Torrent mantiene su propia velocidad. Se aplica al momento."},
        {L"Los enemigos no atacan", L"Los personajes controlados por la IA dejan de atacar."},
        {L"Los enemigos no se mueven", L"Los personajes controlados por la IA se quedan quietos."},
        {L"Desactivar IA enemiga", L"Los personajes controlados por la IA dejan de pensar."},
        {L"Torrent no muere", L"Tu montura nunca cae."},
        {L"Velocidad de carrera de Torrent (%)", L"Escala la velocidad de animación de Torrent cuando no salta: al andar, galopar, girar y aterrizar. Los saltos usan la velocidad de salto de Torrent. Se aplica al momento."},
        {L"Conservar runas al morir", L"Ya no pierdes las runas al morir."},
        {L"Runas obtenidas (%)", L"Escala las runas de los enemigos derrotados. Se aplica a los enemigos que se carguen después."},
        {L"Tasa de botín (%)", L"Probabilidad de que los enemigos suelten objetos. 200% duplica los poco comunes. Se suma al descubrimiento."},
        {L"Coste de cenizas espirituales (%)", L"Escala los PC y la vida necesarios para invocar cenizas espirituales. Con 0% son gratis."},
        {L"Coste de mejora (%)", L"Escala las runas que cobran el herrero por mejorar armas y Roderika por mejorar cenizas de espíritu. Los materiales siguen haciendo falta salvo con Mejorar sin materiales. Con 0% es gratis."},
        {L"Pausa (Esc / Start)", L"Pulsa Esc o el botón Start del mando para pausar o reanudar el juego. Ambos abren también el menú del juego."},
        {L"Fijar hora del día", L"Lleva el reloj del mundo a la hora de abajo, como al descansar en una gracia. Cambiar la hora la vuelve a aplicar."},
        {L"Hora", L"Hora del día (0-23) que usa Fijar hora del día."},
        {L"Congelar el tiempo", L"Detiene el reloj del mundo: la hora actual se mantiene hasta desactivarlo."},
        {L"Forzar clima", L"Cambia al momento al clima de abajo, como hacen los propios eventos del juego, y lo mantiene. Los lugares con clima fijo (algunos jefes y mazmorras) conservan el suyo, y bajo techo o bajo tierra no se ve. Al desactivarla vuelve el clima normal."},
        {L"Clima", L"Clima que se mantiene mientras la opción de arriba está activa."},
        {L"Mostrar todas las gracias", L"Todos los Lugares de Gracia aparecen en el mapa y puedes viajar a ellos. No se guarda: al desactivarlo vuelven tus gracias descubiertas."},
        {L"Revelar el mapa completo", L"Muestra todas las zonas de fragmentos de mapa. No se guarda: al desactivarlo vuelve tu propio mapa."},
        {L"Viaje rápido en cualquier sitio", L"Permite viajar desde dentro de mazmorras legado y otras zonas cerradas. Los enemigos cercanos aún lo impiden."},
        {L"Reinvocar sin descansar", L"Tras caer una ceniza puedes volver a invocarla sin descansar en una gracia. Al desactivarlo vuelve el límite original."},
        {L"Invocar en cualquier lugar", L"Cada monumento de invocación alcanza toda su zona, así que puedes invocar donde el juego permita espíritus, y las cenizas ya no se retiran por alejarte de él. Al desactivarla vuelve el alcance original."},
        {L"Varias cenizas a la vez", L"Multiple Spirit Ashes, integrado. Al usar otra ceniza espiritual se quedan las que ya están invocadas, y cada una cobra sus PC; volver a usar el objeto de una invocada retira solo esa con un fundido corto, aunque no tengas PC. No salen confirmaciones, las cenizas te siguen al cambiar de zona y al cruzar puertas, y cada una tiene una fila con su nombre y su vida en el HUD (hasta 6; las filas necesitan la opción activada al arrancar el juego). Desactivada, invocar funciona como en el juego original."},
        {L"Máximo de cenizas a la vez", L"Cuántas cenizas espirituales puedes tener invocadas juntas con «Varias cenizas a la vez». Una invocación de varios espíritus cuenta como una. El HUD muestra hasta 6; muchas cenizas pueden bajar el rendimiento."},
        {L"Las cenizas se quedan al vencer al jefe", L"Los monumentos ligados a un jefe ya no retiran tus cenizas cuando muere y siguen activos después, también en las salas de jefes que ya venciste. Se aplica al recargar la zona (descansa en una gracia)."},
        {L"Retirar todas las cenizas", L"Con «Varias cenizas a la vez», retira todas las cenizas invocadas con un fundido corto. Funciona como un botón; también puedes asignarle una tecla en [Hotkeys] de NativeTweaks.ini (SpiritDismissAll = F5)."},
        {L"Coste de las cenizas extra (%)", L"Con «Varias cenizas a la vez», los PC que cuesta cada ceniza cuando ya hay otra invocada, en % de su coste. 100% es el coste normal; la primera siempre cuesta lo suyo."},
        {L"Daño perdido por ceniza extra (%)", L"Con «Varias cenizas a la vez», cada ceniza invocada hace este % menos de daño por cada otra ceniza invocada (como mínimo, el 25%). Con 0% hacen todo su daño."},
        {L"Desbloquear FPS", L"Quita el límite de 60 FPS."},
        {L"Límite de FPS", L"FPS máximos con los FPS desbloqueados."},
        {L"Permitir ultrapanorámico", L"Quita las bandas negras 16:9 en monitores anchos. Vuelve a aplicar la resolución si hace falta."},
        {L"Frecuencia en pantalla completa (Hz)", L"Frecuencia que se pide en pantalla completa exclusiva; el juego suele pedir 60 Hz. Pon la de tu monitor. 0 la deja al desbloqueo de FPS. Vuelve a entrar en pantalla completa para aplicarla."},
        {L"Saltar logos de inicio", L"Omite los logos al arrancar. Tiene efecto en el siguiente inicio."},
        {L"Arma: hemorragia", L"Tus ataques acumulan hemorragia (sangrado). Usa la acumulación de las armas del propio juego. Se aplica desde el siguiente golpe, también a hechizos y habilidades que golpean. Hasta cinco efectos por ataque."},
        {L"Arma: veneno", L"Tus ataques acumulan veneno. Usa la acumulación de las armas del propio juego. Se aplica desde el siguiente golpe, también a hechizos y habilidades que golpean. Hasta cinco efectos por ataque."},
        {L"Arma: putrefacción", L"Tus ataques acumulan putrefacción escarlata. Usa la acumulación de las armas del propio juego. Se aplica desde el siguiente golpe, también a hechizos y habilidades que golpean. Hasta cinco efectos por ataque."},
        {L"Arma: congelación", L"Tus ataques acumulan congelación. Usa la acumulación de las armas del propio juego. Se aplica desde el siguiente golpe, también a hechizos y habilidades que golpean. Hasta cinco efectos por ataque."},
        {L"Arma: sueño", L"Tus ataques acumulan sueño. Usa la acumulación de las armas del propio juego. Se aplica desde el siguiente golpe, también a hechizos y habilidades que golpean. Hasta cinco efectos por ataque."},
        {L"Daño físico (%)", L"Escala la parte física de tus ataques y hechizos. Se multiplica con Daño infligido. Se aplica desde el siguiente golpe."},
        {L"Daño mágico (%)", L"Escala la parte mágica de tus ataques y hechizos. Se multiplica con Daño infligido. Se aplica desde el siguiente golpe."},
        {L"Daño de fuego (%)", L"Escala la parte de fuego de tus ataques y hechizos. Se multiplica con Daño infligido. Se aplica desde el siguiente golpe."},
        {L"Daño de rayo (%)", L"Escala la parte de rayo de tus ataques y hechizos. Se multiplica con Daño infligido. Se aplica desde el siguiente golpe."},
        {L"Daño sagrado (%)", L"Escala la parte sagrada de tus ataques y hechizos. Se multiplica con Daño infligido. Se aplica desde el siguiente golpe."},
        {L"Escalado de atributos (%)", L"Escala lo que Fuerza, Destreza, Inteligencia y Fe suman al daño del arma. Vuelve a equipar el arma para actualizarlo."},
        {L"Peso de las armas (%)", L"Escala solo el peso de las armas. Se multiplica con Peso del equipo."},
        {L"Daño físico recibido (%)", L"Escala la parte física de los ataques enemigos. Se multiplica con Daño recibido."},
        {L"Daño mágico recibido (%)", L"Escala la parte mágica de los ataques enemigos. Se multiplica con Daño recibido."},
        {L"Daño de fuego recibido (%)", L"Escala la parte de fuego de los ataques enemigos. Se multiplica con Daño recibido."},
        {L"Daño de rayo recibido (%)", L"Escala la parte de rayo de los ataques enemigos. Se multiplica con Daño recibido."},
        {L"Daño sagrado recibido (%)", L"Escala la parte sagrada de los ataques enemigos. Se multiplica con Daño recibido."},
        {L"Acumulación de hemorragia (%)", L"Lo rápido que se acumula la hemorragia, en enemigos y en ti. 0% la desactiva. Se multiplica con Acumulación de estados."},
        {L"Acumulación de veneno (%)", L"Lo rápido que se acumula el veneno, en enemigos y en ti. 0% lo desactiva. Se multiplica con Acumulación de estados."},
        {L"Acumulación de putrefacción (%)", L"Lo rápido que se acumula la putrefacción escarlata, en enemigos y en ti. 0% la desactiva. Se multiplica con Acumulación de estados."},
        {L"Acumulación de congelación (%)", L"Lo rápido que se acumula la congelación, en enemigos y en ti. 0% la desactiva. Se multiplica con Acumulación de estados."},
        {L"Acumulación de sueño (%)", L"Lo rápido que se acumula el sueño, en enemigos y en ti. 0% lo desactiva. Se multiplica con Acumulación de estados."},
        {L"Acumulación de locura (%)", L"Lo rápido que se acumula la locura, en enemigos y en ti. 0% la desactiva. Se multiplica con Acumulación de estados."},
        {L"Acumulación de muerte (%)", L"Lo rápido que se acumula la muerte, en enemigos y en ti. 0% la desactiva. Se multiplica con Acumulación de estados."},
        {L"Mejoras de arma permanentes", L"Las grasas y encantamientos de arma nunca se acaban. Se aplica a la siguiente mejora que uses. Anula Duración de mejoras de arma."},
        {L"Mejoras propias permanentes", L"Hechizos y encantamientos de mejora, bolos y otros efectos temporales sobre ti y tus aliados nunca se acaban. Se aplica a la siguiente. Anula Duración de mejoras propias."},
        {L"Bloqueo perfecto", L"Todas las armas y escudos bloquean todo el daño físico, mágico, de fuego y de rayo. El daño sagrado mantiene su bloqueo habitual. Vuelve a equipar el escudo para actualizarlo."},
        {L"Retroceso infligido (%)", L"Cuánto empujan tus golpes a los enemigos. Se aplica desde el siguiente golpe."},
        {L"Retroceso recibido (%)", L"Cuánto te empujan los golpes enemigos. También afecta a PNJ y cenizas espirituales."},
        {L"Efectos también en enemigos", L"Enemigos, PNJ y cenizas espirituales también aplican los efectos de arma activados arriba. Hace el juego más difícil."},
        {L"Quitar inmunidades enemigas", L"Los enemigos inmunes a veneno, putrefacción, hemorragia, congelación o sueño pasan a tener una resistencia normal. Locura y muerte siguen afectando solo a personajes tipo jugador. Se aplica a los enemigos cargados después: descansa en una gracia."},
        {L"Curación del frasco carmesí (%)", L"Escala la vida que restaura el frasco de lágrimas carmesí, en todos sus niveles de mejora. 0% no cura."},
        {L"PF del frasco cerúleo (%)", L"Escala los PF que restaura el frasco de lágrimas cerúleas, en todos sus niveles de mejora. 0% no restaura."},
        {L"Coste de aguante de hechizos (%)", L"Escala el aguante que gastan hechicerías y encantamientos, también al cargarlos."},
        {L"Hechizos en una sola ranura", L"Los hechizos que ocupan 2 o 3 ranuras de memoria pasan a ocupar 1. El resto no cambia."},
        {L"Ignorar requisitos de hechizos", L"Lanza hechizos sin la Inteligencia ni la Fe que piden. No incluye los requisitos de Arcano."},
        {L"Coste de PC de habilidades (%)", L"Escala los PC que gastan las cenizas de guerra, también al cargarlas y en los golpes de seguimiento. 0% las hace gratis y te deja siempre al menos 1 PC, que el juego necesita para la versión completa de la habilidad."},
        {L"Llevar hasta 999 de cada objeto", L"Los objetos que se acumulan admiten 999 encima y en el cofre. Los objetos únicos (llaves, silbato, frascos) no cambian."},
        {L"Velocidad de uso de objetos (%)", L"Beber frascos, usar consumibles y recoger objetos. Se multiplica con la velocidad general."},
        {L"Precios de las tiendas (%)", L"Escala lo que cobran los mercaderes y demás tiendas, en runas o en objetos como los corazones de dragón. 0% lo hace todo gratis. Los intercambios por recuerdos siguen pidiéndolos. Se aplica la próxima vez que abras una tienda."},
        {L"Fabricar sin materiales", L"Las recetas de fabricación no piden materiales. Sigues necesitando el libro de recetas y el kit de fabricación. Al desactivarla vuelven los materiales de siempre."},
        {L"Requisitos de atributos de armas (%)", L"Escala la Fuerza, Destreza, Inteligencia, Fe y Arcano necesarios para usar armas, escudos, bastones y sellos. 0% los elimina. Vuelve a equipar el arma para actualizarlo."},
        {L"Mejorar sin materiales", L"Mejorar armas (piedras de forja y piedras sombrías) y cenizas de espíritu (belladona) no pide materiales. Las runas se siguen cobrando salvo con Coste de mejora a 0%. Al desactivarla vuelven los materiales de siempre."},
        {L"Ocultar casco", L"La armadura de cabeza se ve como la cabeza descubierta, así que se ven la cara y el pelo. La defensa, el equilibrio, el peso y los efectos no cambian. Vuelve a equipar el casco o pasa por una pantalla de carga para actualizarlo. No afecta a los atuendos de cuerpo entero."},
        {L"Vida máxima (%)", L"Escala tu vida máxima. Las subidas de nivel y el equipo siguen sumando encima; 100% devuelve el valor del juego."},
        {L"PC máximos (%)", L"Escala tus PC máximos. Las subidas de nivel y el equipo siguen sumando encima; 100% devuelve el valor del juego."},
        {L"Aguante máximo (%)", L"Escala tu aguante máximo. Las subidas de nivel y el equipo siguen sumando encima; 100% devuelve el valor del juego."},
        {L"Regeneración de vida (puntos/s)", L"Recupera esta vida cada segundo."},
        {L"Regeneración de PC (puntos/s)", L"Recupera estos PC cada segundo."},
        {L"Regeneración de aguante (puntos/s)", L"Añade este aguante cada segundo, además de la recuperación normal."},
        {L"HUD hasta los bordes", L"En pantallas más anchas que 16:9, lleva las barras, el equipo, las runas y los demás elementos de las esquinas hasta los bordes de la pantalla. Úsalo con Permitir ultrapanorámico. Reinicia el juego para aplicarlo: carga el HUD una sola vez al arrancar. Desactívalo si otro mod ya coloca el HUD para ultrapanorámico."},
        {L"Escala del HUD (%)", L"Cambia el tamaño de las barras, el equipo, la brújula, las barras de jefe, las runas y los demás elementos de las esquinas, cada uno desde su esquina. Reinicia el juego para aplicarlo: carga el HUD una sola vez al arrancar."},
        {L"Menús hasta los bordes", L"En pantallas más anchas que 16:9, lleva el menú de pausa y los demás menús (equipo, inventario, estado, tiendas, subir de nivel, hechizos, cofre, opciones, pantallas de carga y de título) hasta los bordes y ensancha sus fondos. Reinicia el juego para aplicarlo: carga los menús una sola vez al arrancar. Desactívalo si otro mod sustituye estos menús."},
        {L"Los enemigos no reaparecen", L"Los enemigos que matas siguen muertos tras descansar en una gracia, pasar el tiempo, morir, viajar rápido o cargar la partida. Cada partida guarda su lista en DLLs\\NativeTweaks\\dead-enemies; un enemigo que vuelve a cargarse desaparece al momento: sin cuerpo, runas ni botín. Los jefes y los PNJ funcionan como siempre, y los que siguen vivos se reinician como siempre. Al desactivarla, el siguiente descanso los devuelve a todos; al reactivarla vuelve a aplicarse la lista. Borra el archivo de la partida para empezar de cero."},
        {L"Preguntas Sí/No empiezan en Sí (beta)", L"BETA: las preguntas Sí/No se abren con el cursor en Sí en vez de en No: viajar, salir, usar un objeto, etc.; con el ratón, el puntero también se coloca sobre Sí. Afecta a todas las preguntas Sí/No, también a las arriesgadas, así que comprueba antes de confirmar. Las demás ventanas, como las de cantidad de objetos, no cambian."},
        {L"Golpear enemigos derribados", L"Como en Nightreign, los enemigos que caen al suelo por un golpe crítico o un contraataque reciben daño mientras están tendidos y al levantarse. A cambio tienen superarmadura, así que tus golpes no los vuelven a aturdir, pero sí dañan su postura. Las cenizas espirituales también pueden golpearlos durante el crítico. Tú sigues protegido cuando un enemigo te agarra."},
        {L"Velocidad de salto de Torrent (%)", L"Escala la velocidad de animación de Torrent en los saltos y dobles saltos, también en los de los manantiales espirituales. Al 100 % el doble salto sale tan fácil como en el juego base, aunque la carrera vaya más rápida. Se aplica al momento."},
        {L"Altura del salto (%)", L"Escala la altura de tus saltos a pie. El 100 % es la del juego (aprox. 1,1 m). La animación va a su velocidad normal y un salto más alto dura más en el aire, como con gravedad (400 % duplica el tiempo), y cae sin frenar hasta el suelo, así que el aterrizaje y el daño por caída funcionan como siempre. Un ataque en salto en lo alto cae desde ahí."},
        {L"Distancia del salto (%)", L"Escala tu velocidad en el aire en un salto a pie, parado, corriendo o esprintando, y con ella hasta dónde llegas. Al 100 % vas a la velocidad del juego y un salto más alto ya llega más lejos. El tiempo en el aire solo depende de la altura, así que por encima del 100 % el salto va más rápido, como un salto esprintando."},
        {L"Altura del salto de Torrent (%)", L"Escala la altura de los saltos de Torrent, dobles saltos incluidos. La animación va a su velocidad normal y un salto más alto sube durante más tiempo; desde lo más alto Torrent baja tan rápido como subió, así que nunca se queda colgado, y el doble salto sale igual de fácil. Los saltos de los manantiales espirituales mantienen su altura."},
        {L"Distancia del salto de Torrent (%)", L"Escala la velocidad de Torrent en el aire en un salto, dobles saltos incluidos, y con ella hasta dónde llega. Al 100 % Torrent va a la velocidad del juego y un salto más alto ya llega más lejos. Los saltos de los manantiales espirituales mantienen su distancia."},
        {L"Sin fijar objetivo", L"No puedes fijar a enemigos ni a otros personajes, y se suelta el fijado que tengas. Se aplica al momento."},
        {L"Romper el equilibrio de un golpe", L"Cada golpe tuyo hace tambalear a los enemigos y rompe la postura de los jefes que se pueden romper. Se aplica desde el siguiente golpe y anula «Daño al equilibrio infligido»."},
        {L"Estados alterados al instante", L"Cualquier golpe que acumule veneno, putrefacción, hemorragia, muerte, congelación, sueño o locura lo provoca de una vez, para todos: tú, los enemigos y las cenizas espirituales. Anula los deslizadores de acumulación; con «Inmunidad a estados alterados» solo afecta a los demás."},
        {L"Morir de un golpe", L"Cualquier golpe enemigo que haga daño te mata. Los golpes entre enemigos y los de tus cenizas espirituales usan los mismos datos, así que también matan de un golpe. Anula «Daño recibido»; el modo dios te sigue protegiendo."},
        {L"Los enemigos rompen tu equilibrio de un golpe", L"Cada golpe enemigo te hace tambalear. Tus cenizas espirituales usan los mismos datos y también rompen el equilibrio enemigo de un golpe. Anula «Daño al equilibrio recibido»."},
        {L"Los ataques cargados lanzan a los enemigos", L"Tus ataques fuertes cargados lanzan a los enemigos por los aires en vez de hacerlos tambalear: hacia atrás, lejos o hacia arriba. Funciona aunque no pierdan el equilibrio. Los enemigos que ya están en el suelo, los grandes y los jefes no tienen esa animación y mantienen su reacción. «Lejos» es la que mejor lanza; «Distancia del lanzamiento», más abajo, decide lo lejos que vuelan. Se aplica desde tu siguiente golpe."},
        {L"Todos tus ataques lanzan", L"Con la opción de arriba activada, todos tus ataques que hacen reaccionar al enemigo lo lanzan igual, no solo los fuertes cargados: ataques ligeros y fuertes, habilidades, hechizos y objetos arrojadizos."},
        {L"Distancia del lanzamiento (%) (beta)", L"BETA, en desarrollo: su objetivo es que los enemigos lanzados vuelen más lejos. Por ahora solo los hace más ligeros mientras la opción de lanzar de arriba está activa (al 200 % pesan la mitad; lo toman al aparecer, así que descansa en una gracia), y en las pruebas aún no ha cambiado lo lejos que vuelan. Déjala al 100 % salvo que quieras experimentar."},
        {L"Dificultad del mundo", L"Los enemigos tienen la fuerza de un ciclo de Nueva Partida+, hasta NG+7, zona por zona como en la Nueva Partida+ de verdad: más vida, daño, defensa y resistencia a estados, y más runas. Tu partida sigue en su propio ciclo: jefes, objetos y finales no cambian. En Nueva Partida+ solo añade lo que le falta a tu ciclo. Se aplica a los enemigos que aparecen después: descansa en una gracia."},
        {L"Mantener tus runas", L"Con Dificultad del mundo activa, los enemigos dan las runas del ciclo de tu partida en lugar de las del ciclo más difícil."},
        {L"Abrir el espejo", L"Abre la pantalla de apariencia del espejo de la Mesa Redonda. Úsalo fuera de combate."},
        {L"Renacer", L"Abre la pantalla de renacimiento de Rennala para redistribuir tus atributos. No necesita Lágrima larval. Úsalo fuera de combate."},
        {L"Vida de los jefes (%)", L"Escala solo la vida de los jefes: principales, de campo, de mazmorra y del DLC. Se multiplica con «Vida de enemigos», así que con «Vida de enemigos» al 100 % solo cambian los jefes. Se aplica a los jefes que se carguen después: descansa en una gracia."},
        {L"Personajes limpios (beta)", L"BETA: por ahora también se aplica a los enemigos; el objetivo es que sea solo tu personaje. La sangre, el barro y las quemaduras ya no se pegan al cuerpo de los personajes, ni al tuyo ni al de los enemigos. Las salpicaduras siguen saliendo en el suelo y las paredes. Se aplica a los golpes nuevos; las manchas que ya tienes se van como siempre."},
        {L"Ocultar efectos de mejoras", L"Tus mejoras (encantamientos, hechizos, bolos, talismanes y similares) ya no muestran partículas ni brillo en tu cuerpo. Funcionan exactamente igual, mantienen su sonido y la invisibilidad sigue funcionando. Las mejoras del arma mantienen su aspecto (ver la opción de abajo). Se aplica a la siguiente mejora que uses."},
        {L"Ocultar efectos de mejoras de arma", L"Las grasas y los encantamientos de arma ya no muestran su brillo ni partículas en el arma. Funcionan exactamente igual y mantienen su sonido. Se aplica a la siguiente grasa o encantamiento que uses."},
        {L"Invencibilidad al esquivar (%) (beta)", L"BETA: alarga la invencibilidad de las volteretas, los pasos atrás y las habilidades de esquiva. El 100 % es la del juego (14 fotogramas en una voltereta ligera o media, 13 en una pesada); el 200 % la duplica. El tiempo extra sigue a la invencibilidad de la propia esquiva y dura aunque hagas otra acción justo después. Solo para tu personaje."},
        {L"Ventana de parada (%) (beta)", L"BETA: alarga la ventana de parada de los escudos y las habilidades de parada. El 100 % es la del juego; el 200 % la duplica. El tiempo extra sigue a la ventana de la propia parada. Solo para tu personaje."},
        {L"Invencible mientras estás derribado (beta)", L"BETA: los golpes no te hacen daño mientras un golpe fuerte te tiene en el suelo, incluido el levantarte, como durante una voltereta. Solo los derribos fuertes, no los agarres. Solo para tu personaje."},
        {L"Superarmadura al usar objetos (beta)", L"BETA: la superarmadura del juego mientras bebes un frasco o usas un objeto: los golpes más débiles ya no te interrumpen, los muy fuertes sí pueden, y el daño lo recibes igual. Solo para tu personaje."},
    },
    {L"Restaurar valores del juego", L"Desactiva todas las opciones vanilla y restaura los valores originales del juego."},
    {L"Restablecer valores", L"Devuelve las opciones de esta página a los valores originales del juego."},
    L"No disponible en esta versión del juego: no se encontraron los datos que modifica.",
};

const Texts kChinese{
    {L"原版设置", L"艾尔登法环本体的额外选项。更改立即生效。仅限离线（关闭 EAC）。标有（测试版）的选项可能并非总是有效。"},
    {
        {L"生存", L"无敌、不会死亡、坠落伤害、最大血量、FP 与精力、回复和圣杯瓶。"},
        {L"装备", L"装备与武器重量、能力值需求、法术栏位、携带上限和头盔。"},
        {L"资源与消耗", L"无限精力、FP、道具与弹药，以及各种行动的消耗。"},
        {L"移动与速度", L"你的动作速度（整体和各类动作）、闪避无敌时间、弹反判定时间，以及跳跃高度与距离。"},
        {L"灵马", L"灵马的生存、奔跑与跳跃。"},
        {L"造成伤害", L"你的伤害、各属性伤害、能力补正、击退和一击必杀。"},
        {L"受到伤害", L"你受到的敌人伤害、各属性伤害、击退、格挡和一击即死。"},
        {L"韧性", L"你和敌人的韧性，以及双方造成的削韧。"},
        {L"异常状态", L"所有人的异常状态累积速度、你的免疫和敌人免疫。"},
        {L"武器效果", L"为你的攻击附加异常状态，每种都有单独的开关。"},
        {L"增益与持续时间", L"增益、油脂和敌人减益的持续时间，或使其永久生效。"},
        {L"敌人", L"敌人的血量、速度、行为、重生与锁定。"},
        {L"骨灰", L"召唤消耗、同时召唤多个骨灰，以及何时何地可以召唤骨灰。"},
        {L"卢恩、商店与强化", L"卢恩、道具掉落、商店价格、强化与制作。"},
        {L"地图与传送", L"赐福、地图显示、传送，以及随时使用镜子与重生。"},
        {L"世界", L"时间与天气。"},
        {L"镜头", L"视野与镜头行为。"},
        {L"画面", L"后期处理与细节层次。"},
        {L"显示", L"帧率、刷新率与画面比例。"},
        {L"界面与系统", L"HUD 与菜单布局、暂停、启动标志和是/否对话框。"},
    },
    {
        {L"自定义视野", L"缩放游戏视野。立即生效。"},
        {L"视野 (%)", L"100% 为游戏原值。保留缩放效果。"},
        {L"禁用镜头自动旋转", L"移动时镜头不再自动转到身后。"},
        {L"禁用镜头复位", L"没有目标时按锁定不再将镜头转向前方。"},
        {L"镜头距离 (%)", L"缩放镜头与角色的距离。在镜头切换时生效：骑马、锁定或进入其他区域。"},
        {L"移除暗角", L"移除画面边缘的变暗效果。"},
        {L"移除色差", L"移除画面边缘的彩色描边。"},
        {L"远处完整动画", L"远处的敌人和 NPC 以完整帧率播放动画。CPU 占用略有增加。"},
        {L"禁用抗锯齿 (TAA)", L"强制关闭游戏抗锯齿，包括时间性过滤。关闭此项后恢复你在菜单中的选择。"},
        {L"禁用动态模糊", L"强制关闭游戏的动态模糊。关闭此项后恢复你在菜单中的选择。"},
        {L"禁用景深", L"强制关闭游戏景深。关闭此项后恢复你在菜单中的选择。"},
        {L"无敌", L"你不会受到伤害。"},
        {L"不会死亡", L"仍会受到伤害，但血量永远不会低于 1。"},
        {L"无限精力", L"行动不再消耗精力。"},
        {L"无限 FP", L"法术和战技不再消耗 FP。"},
        {L"无限消耗品", L"使用道具不会减少数量。"},
        {L"无限箭矢与弩箭", L"弹药不会消耗。"},
        {L"装备重量 (%)", L"缩放武器和防具的重量。0% 时始终为轻负重。"},
        {L"韧性 (%)", L"缩放防具的韧性。重新装备一件防具以刷新。"},
        {L"无限韧性", L"普通攻击永远无法使你踉跄。立即生效，无论是否穿戴防具。投技仍然有效。"},
        {L"无坠落伤害", L"无论高度多少，无论步行还是骑乘，坠落都不会伤害或杀死你和灵马。在无底深渊中会一直下落。"},
        {L"异常状态免疫", L"中毒、腐败、出血、咒死、冻伤、睡眠和发狂不会在你身上累积，即使是一次重击也不会。开启时已在生效的异常会照常结束。不影响敌人。"},
        {L"造成伤害 (%)", L"缩放你的攻击和法术伤害。从下一次命中开始生效。"},
        {L"受到伤害 (%)", L"缩放敌人的攻击伤害。也会影响 NPC 之间及骨灰的攻击。"},
        {L"造成削韧 (%)", L"缩放你的攻击、武器和法术的削韧：使敌人踉跄、破坏其架势的速度。下一次攻击即生效。"},
        {L"受到削韧 (%)", L"缩放敌人攻击的削韧：使你踉跄的速度。也会影响 NPC 和骨灰。下一次攻击即生效。"},
        {L"精力消耗 (%)", L"缩放攻击和行动消耗的精力。"},
        {L"法术 FP 消耗 (%)", L"缩放魔法和祷告的 FP 消耗。"},
        {L"异常状态累积 (%)", L"中毒、腐败、出血、咒死、冻伤、睡眠和发狂。对所有人生效，包括你自己。"},
        {L"一击必杀", L"你的攻击会立即击败敌人。"},
        {L"武器附魔持续时间 (%)", L"油脂和武器附魔。对下一次施放或使用的增益生效。"},
        {L"自身增益持续时间 (%)", L"增益法术与祷告、丸药及作用于你和同伴的其他限时效果。对下一次生效。"},
        {L"敌人减益持续时间 (%)", L"作用于敌人的限时效果，例如降低防御。也会影响敌人对你施加的减益。"},
        {L"整体速度 (%)", L"缩放你的所有动作：移动、攻击、翻滚和法术。"},
        {L"攻击速度 (%)", L"武器攻击动作。与整体速度相乘。"},
        {L"法术速度 (%)", L"施法动作。与整体速度相乘。"},
        {L"翻滚与闪避速度 (%)", L"翻滚和后撤步。与整体速度相乘。"},
        {L"移动速度 (%)", L"行走、奔跑和冲刺。与整体速度相乘。"},
        {L"跳跃速度 (%)", L"起跳、下落和落地。跳跃攻击使用攻击速度。与整体速度相乘。"},
        {L"敌人血量 (%)", L"缩放敌人血量。对之后加载的敌人生效：在赐福处休息。"},
        {L"敌人韧性 (%)", L"缩放敌人踉跄前能承受的冲击。对之后加载的敌人生效：在赐福处休息。"},
        {L"敌人无限韧性", L"敌人、NPC 和骨灰永远不会踉跄。在赐福处休息后生效。"},
        {L"敌人速度 (%)", L"缩放所有其他角色的动作速度：敌人、NPC 和骨灰。灵马保持自身速度。立即生效。"},
        {L"敌人不攻击", L"AI 控制的角色停止攻击。"},
        {L"敌人不移动", L"AI 控制的角色原地不动。"},
        {L"禁用敌人 AI", L"AI 控制的角色停止思考。"},
        {L"灵马不会死亡", L"你的坐骑永远不会倒下。"},
        {L"灵马奔跑速度 (%)", L"缩放灵马不跳跃时的动作速度：行走、奔跑、转向和落地。跳跃使用灵马跳跃速度。立即生效。"},
        {L"死亡不掉卢恩", L"死亡时不再掉落卢恩。"},
        {L"获得卢恩 (%)", L"缩放击败敌人获得的卢恩。对之后加载的敌人生效。"},
        {L"道具掉落率 (%)", L"缩放敌人掉落道具的几率。200% 约使罕见掉落翻倍。与道具发现力叠加。"},
        {L"骨灰消耗 (%)", L"缩放召唤骨灰所需的 FP 和血量。0% 为免费。"},
        {L"强化费用 (%)", L"缩放铁匠强化武器和罗德莉卡强化骨灰收取的卢恩。除非开启“强化无需材料”，否则仍需要材料。0% 为免费。"},
        {L"暂停 (Esc / Start)", L"按 Esc 或手柄 Start 键暂停或继续游戏。两者也会打开游戏菜单。"},
        {L"设定时间", L"将世界时钟调到下方的小时，就像在赐福处休息。更改小时会再次应用。"},
        {L"小时", L"“设定时间”使用的小时 (0-23)。"},
        {L"冻结时间", L"停止世界时钟，当前时间保持不变，直到关闭此项。"},
        {L"强制天气", L"像游戏自身事件一样立即切换为下方的天气并保持。天气固定的地点（部分首领战和地下城）保持原样，室内或地下看不到。关闭后恢复正常天气。"},
        {L"天气", L"上方选项开启时保持的天气。"},
        {L"显示所有赐福", L"所有赐福都会显示在地图上并可传送。不会保存：关闭后恢复你已发现的赐福。"},
        {L"显示完整地图", L"显示所有地图碎片区域。不会保存：关闭后恢复你自己的地图。"},
        {L"任意地点传送", L"允许在传说地下城和其他封闭区域内传送。附近有敌人时仍无法传送。"},
        {L"无需休息即可再次召唤", L"骨灰死亡后无需在赐福休息即可再次召唤。关闭后恢复原版限制。"},
        {L"随处召唤", L"每块召唤碑的范围覆盖其整个区域，只要游戏允许召唤灵体即可召唤，骨灰也不会因你远离召唤碑而被解除。关闭后恢复原版范围。"},
        {L"同时召唤多个骨灰", L"内置 Multiple Spirit Ashes。使用另一个骨灰时，已召唤的骨灰会保留，且每个都会消耗 FP；再次使用已召唤骨灰的道具会以短暂淡出只解除该骨灰，即使 FP 不足也可以。不会弹出确认，骨灰会跟随你进入新区域和穿过门，并在 HUD 上各有一行名称和生命值（最多 6 行；这些行需要在游戏启动时开启此选项）。关闭后召唤与原版游戏相同。"},
        {L"同时召唤骨灰上限", L"开启“同时召唤多个骨灰”时，最多可同时召唤多少个骨灰。一次召唤多个灵体算作一个。HUD 最多显示 6 个；骨灰过多可能降低性能。"},
        {L"击败首领后骨灰保留", L"与首领绑定的召唤碑在首领死亡后不再解除你的骨灰，之后仍可使用，已击败首领的场地也一样。区域重新载入后生效（在赐福处休息）。"},
        {L"解除全部骨灰", L"开启“同时召唤多个骨灰”时，以短暂淡出解除所有已召唤的骨灰。像按钮一样使用；也可以在 NativeTweaks.ini 的 [Hotkeys] 中绑定按键（SpiritDismissAll = F5）。"},
        {L"额外骨灰消耗 (%)", L"开启“同时召唤多个骨灰”时，已有骨灰在场时再召唤一个所需的 FP，按其自身消耗的百分比计算。100% 为正常消耗；第一个骨灰始终按原消耗。"},
        {L"每个额外骨灰的伤害降低 (%)", L"开启“同时召唤多个骨灰”时，每多召唤一个骨灰，所有骨灰的伤害降低此百分比（最低 25%）。0% 保持全部伤害。"},
        {L"解锁帧率", L"解除 60 FPS 限制。"},
        {L"帧率上限", L"解锁帧率时的最大 FPS。"},
        {L"允许超宽屏", L"移除宽屏显示器上的 16:9 黑边。如有需要请重新应用分辨率。"},
        {L"全屏刷新率 (Hz)", L"独占全屏时请求的刷新率；游戏通常请求 60 Hz。请设为显示器的刷新率。0 表示交给帧率解锁处理。重新进入全屏后生效。"},
        {L"跳过启动标志", L"跳过启动时的标志画面。下次启动时生效。"},
        {L"武器：出血", L"你的攻击会累积出血。使用游戏自身武器的累积值。从下一次命中开始生效，也适用于命中的法术和战技。每次攻击最多五个效果。"},
        {L"武器：中毒", L"你的攻击会累积中毒。使用游戏自身武器的累积值。从下一次命中开始生效，也适用于命中的法术和战技。每次攻击最多五个效果。"},
        {L"武器：猩红腐败", L"你的攻击会累积猩红腐败。使用游戏自身武器的累积值。从下一次命中开始生效，也适用于命中的法术和战技。每次攻击最多五个效果。"},
        {L"武器：冻伤", L"你的攻击会累积冻伤。使用游戏自身武器的累积值。从下一次命中开始生效，也适用于命中的法术和战技。每次攻击最多五个效果。"},
        {L"武器：睡眠", L"你的攻击会累积睡眠。使用游戏自身武器的累积值。从下一次命中开始生效，也适用于命中的法术和战技。每次攻击最多五个效果。"},
        {L"物理伤害 (%)", L"缩放你的攻击和法术中的物理部分。与“造成伤害”相乘。从下一次命中开始生效。"},
        {L"魔力伤害 (%)", L"缩放你的攻击和法术中的魔力部分。与“造成伤害”相乘。从下一次命中开始生效。"},
        {L"火焰伤害 (%)", L"缩放你的攻击和法术中的火焰部分。与“造成伤害”相乘。从下一次命中开始生效。"},
        {L"雷电伤害 (%)", L"缩放你的攻击和法术中的雷电部分。与“造成伤害”相乘。从下一次命中开始生效。"},
        {L"圣属性伤害 (%)", L"缩放你的攻击和法术中的圣属性部分。与“造成伤害”相乘。从下一次命中开始生效。"},
        {L"能力补正 (%)", L"缩放力气、灵巧、智力和信仰为武器伤害提供的加成。重新装备武器以刷新。"},
        {L"武器重量 (%)", L"仅缩放武器的重量。与“装备重量”相乘。"},
        {L"受到物理伤害 (%)", L"缩放敌人攻击中的物理部分。与“受到伤害”相乘。"},
        {L"受到魔力伤害 (%)", L"缩放敌人攻击中的魔力部分。与“受到伤害”相乘。"},
        {L"受到火焰伤害 (%)", L"缩放敌人攻击中的火焰部分。与“受到伤害”相乘。"},
        {L"受到雷电伤害 (%)", L"缩放敌人攻击中的雷电部分。与“受到伤害”相乘。"},
        {L"受到圣属性伤害 (%)", L"缩放敌人攻击中的圣属性部分。与“受到伤害”相乘。"},
        {L"出血累积 (%)", L"出血的累积速度，对敌人和你都有效。0% 为关闭。与“异常状态累积”相乘。"},
        {L"中毒累积 (%)", L"中毒的累积速度，对敌人和你都有效。0% 为关闭。与“异常状态累积”相乘。"},
        {L"猩红腐败累积 (%)", L"猩红腐败的累积速度，对敌人和你都有效。0% 为关闭。与“异常状态累积”相乘。"},
        {L"冻伤累积 (%)", L"冻伤的累积速度，对敌人和你都有效。0% 为关闭。与“异常状态累积”相乘。"},
        {L"睡眠累积 (%)", L"睡眠的累积速度，对敌人和你都有效。0% 为关闭。与“异常状态累积”相乘。"},
        {L"发狂累积 (%)", L"发狂的累积速度，对敌人和你都有效。0% 为关闭。与“异常状态累积”相乘。"},
        {L"咒死累积 (%)", L"咒死的累积速度，对敌人和你都有效。0% 为关闭。与“异常状态累积”相乘。"},
        {L"武器附魔永久生效", L"油脂和武器附魔永不消失。对下一次使用的增益生效。覆盖“武器附魔持续时间”。"},
        {L"自身增益永久生效", L"增益法术与祷告、丸药及作用于你和同伴的其他限时效果永不消失。对下一次生效。覆盖“自身增益持续时间”。"},
        {L"完美防御", L"所有武器和盾牌完全格挡物理、魔力、火焰和雷电伤害。圣属性伤害保持原有防御。重新装备盾牌以刷新。"},
        {L"造成击退 (%)", L"你的攻击将敌人击退的距离。从下一次命中开始生效。"},
        {L"受到击退 (%)", L"敌人攻击将你击退的距离。也会影响 NPC 和骨灰。"},
        {L"敌人攻击也附带效果", L"敌人、NPC 和骨灰的攻击也会附带上方开启的武器效果。会提高游戏难度。"},
        {L"移除敌人免疫", L"对中毒、腐败、出血、冻伤或睡眠免疫的敌人改为普通抗性。发狂和咒死仍只对玩家类角色生效。对之后加载的敌人生效：在赐福处休息。"},
        {L"红露滴圣杯瓶回复 (%)", L"缩放红露滴圣杯瓶回复的血量，所有强化等级都适用。0% 为不回复。"},
        {L"蓝露滴圣杯瓶回复 (%)", L"缩放蓝露滴圣杯瓶回复的 FP，所有强化等级都适用。0% 为不回复。"},
        {L"法术精力消耗 (%)", L"缩放魔法和祷告消耗的精力，包括蓄力施放。"},
        {L"法术只占一个记忆栏", L"占用 2 或 3 个记忆栏的法术只占 1 个。其他法术不变。"},
        {L"忽略法术需求", L"无需满足智力和信仰需求即可施放法术。不包括感应需求。"},
        {L"战技 FP 消耗 (%)", L"缩放战灰的 FP 消耗，包括蓄力和追加攻击。0% 为免费，并始终保留至少 1 点 FP，游戏需要它来施放完整的战技。"},
        {L"每种道具可携带 999 个", L"可堆叠的道具在身上和仓库中都能存放 999 个。唯一道具（关键道具、灵马笛、圣杯瓶）不变。"},
        {L"道具使用速度 (%)", L"饮用圣杯瓶、使用消耗品和拾取物品的动作。与整体速度相乘。"},
        {L"商店价格 (%)", L"缩放商人和其他商店的要价，无论是卢恩还是龙心脏等道具。0% 为全部免费。用回忆交换的物品仍需要回忆。下次打开商店时生效。"},
        {L"制作无需材料", L"制作配方不再需要材料。仍需要制作笔记和制作工具。关闭后恢复原有材料。"},
        {L"武器能力值需求 (%)", L"缩放使用武器、盾牌、手杖和圣印记所需的力气、灵巧、智力、信仰和感应。0% 为取消需求。重新装备武器即可刷新。"},
        {L"强化无需材料", L"强化武器（锻造石与暗色锻造石）和骨灰（铃兰）不再需要材料。除非强化费用为 0%，否则仍需支付卢恩。关闭后恢复原有材料。"},
        {L"隐藏头盔", L"头部防具显示为未戴头盔的样子，可以看到脸和头发。防御、韧性、重量和效果不变。重新装备头盔或经过一次读取画面即可刷新。不影响全身套装。"},
        {L"最大生命值 (%)", L"按比例调整最大生命值。升级和装备的加成照常叠加；100% 恢复游戏原值。"},
        {L"最大专注值 (%)", L"按比例调整最大专注值。升级和装备的加成照常叠加；100% 恢复游戏原值。"},
        {L"最大精力 (%)", L"按比例调整最大精力。升级和装备的加成照常叠加；100% 恢复游戏原值。"},
        {L"生命回复（点/秒）", L"每秒回复这么多生命值。"},
        {L"专注回复（点/秒）", L"每秒回复这么多专注值。"},
        {L"精力回复（点/秒）", L"每秒额外增加这么多精力，叠加在正常恢复之上。"},
        {L"HUD 延伸至屏幕边缘", L"在宽于 16:9 的屏幕上，将血条、装备、卢恩和其他角落元素移到屏幕边缘。请与“允许超宽屏”一起使用。需重启游戏才能生效：游戏只在启动时加载一次 HUD。如果其他模组已为超宽屏调整 HUD，请关闭此项。"},
        {L"HUD 缩放 (%)", L"调整血条、装备、指南针、头目血条、卢恩和其他角落元素的大小，每个元素以其所在角落为基准缩放。需重启游戏才能生效：游戏只在启动时加载一次 HUD。"},
        {L"菜单延伸至屏幕边缘", L"在宽于 16:9 的屏幕上，将暂停菜单和其他菜单（装备、物品栏、状态、商店、升级、法术、仓库、选项、读取画面和标题画面）移到屏幕边缘，并加宽其背景。需重启游戏才能生效：游戏只在启动时加载一次菜单。如果其他模组替换了这些菜单，请关闭此项。"},
        {L"敌人不再复活", L"被你击杀的敌人在赐福处休息、等待时间流逝、死亡、快速传送或读取存档后仍保持死亡。每个存档的名单保存在 DLLs\\NativeTweaks\\dead-enemies；再次加载的敌人会立即消失：没有尸体，不给予卢恩和掉落物。头目和 NPC 照常运作，仍然存活的敌人也会照常重置。关闭后，下一次休息会让所有敌人复活；重新开启后名单会再次生效。删除该存档的文件即可重新开始。"},
        {L"是/否询问默认选“是”（测试版）", L"测试版：是/否询问打开时光标位于“是”而不是“否”：传送、退出、使用物品等；使用鼠标时，指针也会移到“是”上。对所有是/否询问生效，包括有风险的操作，确认前请看清楚。其他窗口（如物品数量）不变。"},
        {L"可攻击倒地敌人", L"与《艾尔登法环 黑夜君临》相同，被致命一击或弹反后处决击倒在地的敌人，在倒地和起身期间会受到伤害。作为代价，敌人获得霸体，你的攻击不会再次使其硬直，但仍会削减其架势。致命一击期间，骨灰也能攻击它们。敌人抓住你时，你仍受保护。"},
        {L"灵马跳跃速度 (%)", L"缩放灵马跳跃和二段跳时的动作速度，包括借助上升气流的大跳跃。保持 100% 时，即使奔跑速度更快，二段跳也和原版一样容易。立即生效。"},
        {L"跳跃高度 (%)", L"缩放你步行时的跳跃高度。100% 为游戏原本的高度（约 1.1 米）。动作以正常速度播放，跳得越高滞空越久，与重力一致（400% 时间加倍），下落时不减速直到地面，因此落地和坠落伤害照常。在高处发动跳跃攻击会从该处落下。"},
        {L"跳跃距离 (%)", L"缩放你步行跳跃时在空中的速度（原地、奔跑或冲刺），从而改变跳跃距离。100% 时保持游戏原本的速度，跳得更高本身就会跳得更远。滞空时间只取决于高度，因此高于 100% 时跳跃更快，如同冲刺跳跃。"},
        {L"灵马跳跃高度 (%)", L"缩放灵马的跳跃高度，包括二段跳。动作以正常速度播放，跳得越高上升时间越长；从最高点起灵马以上升时同样的速度下落，因此不会悬停在空中，二段跳依然容易。借助上升气流的大跳跃保持原有高度。"},
        {L"灵马跳跃距离 (%)", L"缩放灵马跳跃时在空中的速度（包括二段跳），从而改变跳跃距离。100% 时保持游戏原本的速度，跳得更高本身就会跳得更远。借助上升气流的大跳跃保持原有距离。"},
        {L"禁用锁定", L"无法锁定敌人或其他角色，已有的锁定会解除。立即生效。"},
        {L"一击破韧", L"你的每次攻击都会使敌人踉跄，并击破可被破防的头目的架势。从下一次攻击起生效，并覆盖“造成削韧 (%)”。"},
        {L"异常状态立即触发", L"任何累积中毒、腐败、出血、咒死、冻伤、睡眠或发狂的攻击都会立即触发该状态，对所有人生效：你、敌人和骨灰。覆盖累积滑块；配合“异常状态免疫”则只影响他人。"},
        {L"一击即死", L"任何造成伤害的敌人攻击都会杀死你。敌人之间以及你的骨灰的攻击使用相同数据，也会一击致命。覆盖“受到伤害 (%)”；无敌仍会保护你。"},
        {L"敌人一击破你韧性", L"敌人的每次攻击都会使你踉跄。你的骨灰使用相同数据，也会一击击破敌人的韧性。覆盖“受到削韧 (%)”。"},
        {L"蓄力攻击击飞敌人", L"你的蓄力重攻击会将敌人击飞，而不是使其踉跄：向后、远距或向上。即使敌人韧性未被击破也有效。已倒地的敌人、大型敌人和头目没有击飞动作，保持自身的反应。“远距击飞”效果最好；下方的“击飞距离 (%)”决定距离。从下一次攻击起生效。"},
        {L"所有攻击都会击飞", L"开启上方选项时，你所有会让敌人产生反应的攻击都会以相同方式击飞敌人，而不只是蓄力重攻击：轻攻击、重攻击、战技、法术和投掷物品。"},
        {L"击飞距离 (%)（测试版）", L"测试版，开发中：目标是让被击飞的敌人飞得更远。目前它只会在开启上方击飞选项时减轻敌人重量（200% 时重量减半；敌人出现时读取，请在赐福处休息），测试中尚未改变击飞距离。除非想要试验，请保持 100%。"},
        {L"世界难度", L"敌人获得新游戏+周目的强度，最高至 NG+7，并像真正的新游戏+一样按区域计算：更多生命、伤害、防御和异常状态抗性，以及更多卢恩。你的存档保持原有周目：头目、物品和结局不变。在新游戏+中只补足你的周目所缺少的部分。对之后出现的敌人生效：请在赐福处休息。"},
        {L"保留你的卢恩", L"开启世界难度时，敌人给予你存档所在周目的卢恩，而不是更高难度周目的卢恩。"},
        {L"打开镜子", L"打开圆桌厅堂镜子的外观界面。请在战斗外使用。"},
        {L"重生", L"打开菈妮的重生界面以重新分配属性点。不需要幼虫之泪。请在战斗外使用。"},
        {L"头目血量 (%)", L"仅缩放头目的血量：主要头目、野外头目、地下城头目和 DLC 头目。与“敌人血量”相乘，因此“敌人血量”为 100% 时只改变头目。对之后加载的头目生效：在赐福处休息。"},
        {L"保持角色干净（测试版）", L"测试版：目前也会作用于敌人，目标是只作用于你的角色。血迹、泥污和焦痕不再附着在角色身上，包括你和敌人。地面和墙壁上仍会出现飞溅痕迹。对新的攻击生效；已有的痕迹会照常消失。"},
        {L"隐藏增益效果", L"你自身的增益（祷告、魔法、丸药、护符等）不再在身上显示粒子或光芒。效果完全相同，保留音效，隐身仍然有效。武器增益保持原样（见下方选项）。对你使用的下一个增益生效。"},
        {L"隐藏武器增益效果", L"油脂和武器附魔不再在武器上显示光芒或粒子。效果完全相同，保留音效。对你使用的下一个油脂或附魔生效。"},
        {L"闪避无敌时间 (%)（测试版）", L"测试版：延长翻滚、后撤步和闪避类战技的无敌时间。100% 为游戏原本的时间（轻、中翻滚 14 帧，重翻滚 13 帧）；200% 为两倍。额外时间紧接闪避本身的无敌时间，即使随后进行其他动作也会持续。仅对你的角色生效。"},
        {L"弹反判定时间 (%)（测试版）", L"测试版：延长盾牌和弹反类战技的弹反判定时间。100% 为游戏原本的时间；200% 为两倍。额外时间紧接弹反本身的判定时间。仅对你的角色生效。"},
        {L"倒地时无敌（测试版）", L"测试版：被重击击倒在地时（包括起身）不会受到伤害，与翻滚时相同。仅限重击倒地，不含抓取。仅对你的角色生效。"},
        {L"使用道具时霸体（测试版）", L"测试版：喝圣杯瓶或使用道具时获得游戏的霸体：较弱的攻击不再打断你，极强的攻击仍可能打断，伤害照常承受。仅对你的角色生效。"},
    },
    {L"恢复游戏默认值", L"关闭所有原版选项并恢复游戏原始数值。"},
    {L"恢复默认值", L"将本页的选项恢复为游戏原始数值。"},
    L"此游戏版本不可用：未找到其修改的数据。",
};

// Menu pages in display order; each lists its options in display order.
struct Layout { Category category; std::initializer_list<Id> ids; };
const Layout kLayout[]{
    {cat_survival, {god_mode, no_death, no_fall_damage, max_hp, max_fp, max_stamina, hp_regen_points,
        fp_regen_points, stamina_regen_points, flask_hp, flask_fp}},
    {cat_equipment, {equip_weight, weapon_weight, weapon_requirements, spell_no_requirements, spell_one_slot,
        carry_max, hide_helmet}},
    {cat_resources, {infinite_stamina, infinite_fp, infinite_items, infinite_arrows, stamina_cost,
        spell_fp_cost, spell_stamina_cost, skill_fp_cost}},
    {cat_movement, {speed_global, speed_attack, speed_spell, speed_roll, dodge_frames, parry_frames, knockdown_invincible, item_super_armor, speed_move, speed_jump, speed_item,
        jump_height, jump_distance}},
    {cat_torrent, {torrent_no_death, torrent_speed, torrent_jump_speed, torrent_jump_height,
        torrent_jump_distance}},
    {cat_dealt, {damage_dealt, dmg_physical, dmg_magic, dmg_fire, dmg_lightning, dmg_holy, weapon_scaling,
        knockback_dealt, charged_launch, launch_all, launch_distance, one_hit_kill}},
    {cat_taken, {damage_taken, taken_physical, taken_magic, taken_fire, taken_lightning, taken_holy,
        knockback_taken, perfect_guard, die_one_hit}},
    {cat_poise, {player_poise, player_poise_lock, posture_taken, posture_one_hit_taken, posture_dealt,
        posture_one_hit, enemy_poise, enemy_poise_lock}},
    {cat_status, {status_buildup, buildup_bleed, buildup_poison, buildup_rot, buildup_frost, buildup_sleep,
        buildup_madness, buildup_blight, status_instant, status_immunity, enemies_no_immunity}},
    {cat_weapon, {weapon_bleed, weapon_poison, weapon_rot, weapon_frost, weapon_sleep, weapon_effects_enemies}},
    {cat_buffs, {buff_weapon, buff_weapon_forever, buff_self, buff_self_forever, buff_enemy}},
    {cat_enemies, {world_journey, journey_keep_runes, enemy_hp, boss_hp, enemy_speed, enemies_no_attack, enemies_no_move,
        enemies_no_ai, enemies_no_respawn, enemies_downed_vulnerable, enemies_no_lock_on}},
    {cat_spirits, {ash_cost, ash_multiple, ash_max_active, ash_dismiss_all, ash_extra_cost, ash_damage_per_ash,
        ash_anywhere, ash_keep_after_boss, ash_no_rest}},
    {cat_runes, {rune_loss_off, runes_gained, loot_drop, shop_price, upgrade_cost, upgrade_free, craft_free}},
    {cat_map, {all_graces, reveal_map, fast_travel_anywhere, open_mirror, open_rebirth}},
    {cat_world, {set_time, time_hour, freeze_time, set_weather, weather_type}},
    {cat_camera, {fov_enabled, fov_percent, camera_distance, camera_rotate_off, camera_reset_off}},
    {cat_image, {vignette_off, chromatic_off, aa_off, motion_blur_off, dof_off, animation_distance,
        clean_characters, hide_buff_effects, hide_weapon_buff_effects}},
    {cat_display, {fps_unlock, fps_limit, refresh_rate, ultrawide}},
    {cat_interface, {hud_screen_edges, hud_scale, menu_edges, pause_key, skip_intro, dialog_yes_default}},
};

void ERUI_CALL restore_page_defaults(void* target) noexcept {
    const auto* layout = static_cast<const Layout*>(target);
    if (!layout) return;
    for (const Id i : layout->ids) restore_option(i);
    request_save();
    log(L"Vanilla settings: one page restored to game defaults.");
}

// ---------------------------------------------------------------------------
// Hotkeys and profiles: change options from outside the menu

const Texts& texts_for(std::string_view lang);

bool same_key(std::wstring_view a, std::wstring_view b) noexcept {
    return a.size() == b.size() &&
        CompareStringOrdinal(a.data(), static_cast<int>(a.size()), b.data(),
            static_cast<int>(b.size()), TRUE) == CSTR_EQUAL;
}

Id find_option(std::wstring_view key) noexcept {
    for (std::size_t i = 0; i < count; ++i)
        if (same_key(g_options[i].key, key)) return static_cast<Id>(i);
    return count;
}

// Pushes an option's current value to its menu row (when the menu exists).
void refresh_row(Id i) noexcept {
    const auto& option = g_options[i];
    if (!g_registration || option.row == ERUI_INVALID_ROW) return;
    if (option.kind == Kind::toggle)
        (void)g_registration->set_value(option.row, static_cast<std::uint8_t>(option.value));
    else if (is_named(i))
        (void)g_registration->set_custom_slider_index(option.row, static_cast<std::uint32_t>(option.value));
    else
        (void)g_registration->set_numeric_slider_value(option.row, option.value);
}

// Sets an option's slider/toggle value.
bool set_option_raw(Id i, int value) noexcept {
    if (i >= count || !available(i)) return false;
    auto& option = g_options[i];
    option.value = clamp_value(option, value);
    apply(i);
    refresh_row(i);
    return true;
}

bool hotkey_is_toggle(std::wstring_view key, bool& toggle) noexcept {
    const Id i = find_option(key);
    if (i >= count || !available(i)) return false;
    toggle = g_options[i].kind == Kind::toggle;
    return true;
}

bool hotkey_get(std::wstring_view key, int& value) noexcept {
    const Id i = find_option(key);
    if (i >= count || !available(i)) return false;
    value = value_of(i);
    return true;
}

bool hotkey_set(std::wstring_view key, int value, int& applied) noexcept {
    const Id i = find_option(key);
    if (i >= count || !available(i)) return false;
    if (!set_option_raw(i, value)) return false;
    request_save();
    applied = value_of(i);
    return true;
}

void hotkey_log(const wchar_t* message) noexcept {
    log(message);
}

// Profiles live in DLLs\NativeTweaks\profiles\vanilla\Slot<N>.ini as UTF-16
// INI files: [Profile] Name=..., [Vanilla] the same keys as NativeTweaks.ini.
constexpr int kProfileSlots = 5;
std::string g_profile_language = "en";  // last language the rows were built in

std::wstring profile_folder() {
    const std::size_t slash = g_config_path.find_last_of(L"\\/");
    if (slash == std::wstring::npos) return {};
    return g_config_path.substr(0, slash + 1) + L"NativeTweaks\\profiles\\vanilla";
}

std::wstring profile_path(int slot) {
    const std::wstring folder = profile_folder();
    return folder.empty() ? folder : folder + L"\\Slot" + std::to_wstring(slot) + L".ini";
}

bool profile_exists(int slot) {
    const std::wstring path = profile_path(slot);
    const DWORD attributes = path.empty() ? INVALID_FILE_ATTRIBUTES : GetFileAttributesW(path.c_str());
    return attributes != INVALID_FILE_ATTRIBUTES && !(attributes & FILE_ATTRIBUTE_DIRECTORY);
}

std::wstring profile_name(int slot) {
    wchar_t name[64]{};
    if (profile_exists(slot))
        GetPrivateProfileStringW(L"Profile", L"Name", L"", name, 64, profile_path(slot).c_str());
    return name;
}

bool write_profile(int slot) {
    const std::wstring folder = profile_folder();
    if (folder.empty()) return false;
    const std::wstring parent = folder.substr(0, folder.find_last_of(L'\\'));
    CreateDirectoryW(parent.substr(0, parent.find_last_of(L'\\')).c_str(), nullptr);
    CreateDirectoryW(parent.c_str(), nullptr);
    CreateDirectoryW(folder.c_str(), nullptr);
    std::wstring name = profile_name(slot);
    if (name.empty()) name = L"Profile " + std::to_wstring(slot);
    std::wstring text = L"\xFEFF; NativeTweaks Vanilla Settings profile. Edit Name freely; keys left out\r\n"
        L"; are not changed when the profile is loaded.\r\n[Profile]\r\nName=" + name + L"\r\n[Vanilla]\r\n";
    for (std::size_t i = 0; i < count; ++i)
        text += std::wstring(g_options[i].key) + L"=" + std::to_wstring(g_options[i].value) + L"\r\n";
    const std::wstring path = profile_path(slot);
    const std::wstring temporary = path + L".tmp";
    HANDLE file = CreateFileW(temporary.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
        FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    DWORD written{};
    const DWORD bytes = static_cast<DWORD>(text.size() * sizeof(wchar_t));
    const bool ok = WriteFile(file, text.data(), bytes, &written, nullptr) && written == bytes;
    CloseHandle(file);
    if (!ok || !MoveFileExW(temporary.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING)) {
        DeleteFileW(temporary.c_str());
        return false;
    }
    return true;
}

bool parse_profile_int(const wchar_t* text, int& value) noexcept {
    wchar_t* end{};
    const long parsed = std::wcstol(text, &end, 10);
    if (end == text || *end) return false;
    value = static_cast<int>(parsed);
    return true;
}

// Applies every key the profile lists. Returns false when the file is missing.
bool load_profile_slot(int slot) noexcept {
    try {
        if (!profile_exists(slot)) return false;
        const std::wstring path = profile_path(slot);
        std::vector<wchar_t> section(65536);
        const DWORD length = GetPrivateProfileSectionW(L"Vanilla", section.data(),
            static_cast<DWORD>(section.size()), path.c_str());
        // Profiles saved with the old extended ranges also hold "...Scale" keys:
        // they match no option and are skipped.
        for (const wchar_t* line = section.data(); line < section.data() + length && *line;
             line += std::wcslen(line) + 1) {
            const std::wstring_view entry(line);
            const std::size_t equals = entry.find(L'=');
            if (equals == entry.npos) continue;
            const std::wstring key(entry.substr(0, equals));
            const std::wstring value_text(entry.substr(equals + 1));
            int value{};
            if (!parse_profile_int(value_text.c_str(), value)) continue;
            (void)set_option_raw(find_option(key), value);
        }
        request_save();
        log((L"Vanilla settings: profile slot " + std::to_wstring(slot) + L" loaded.").c_str());
        return true;
    } catch (...) {
        return false;
    }
}

// Hotkeys name a profile by slot number ("1", "Slot1") or by its Name.
bool hotkey_load_profile(std::wstring_view name) noexcept {
    try {
        for (int slot = 1; slot <= kProfileSlots; ++slot) {
            const std::wstring number = std::to_wstring(slot);
            if (same_key(name, number) || same_key(name, L"Slot" + number) ||
                (!profile_name(slot).empty() && same_key(name, profile_name(slot)))) {
                return load_profile_slot(slot);
            }
        }
    } catch (...) {
    }
    return false;
}

struct ProfileTexts {
    const wchar_t* page;
    const wchar_t* page_help;
    const wchar_t* load;        // "Load: %s"
    const wchar_t* load_help;
    const wchar_t* save;        // "Save to slot %d"
    const wchar_t* save_help;
    const wchar_t* empty;       // empty slot name
    const wchar_t* saved;
    const wchar_t* loaded;
    const wchar_t* missing;
    const wchar_t* failed;
};

const ProfileTexts kProfileEnglish{
    L"Profiles", L"Save all Vanilla Settings in one of five slots and load them later, here or with a hotkey ([Hotkeys] Profile.1 = Ctrl+1 in NativeTweaks.ini).",
    L"Load: ", L"Apply every option saved in this slot. Profiles are stored in DLLs\\NativeTweaks\\profiles\\vanilla; you can rename them there.",
    L"Save to slot ", L"Store the current Vanilla Settings in this slot, replacing what it held.",
    L"(empty)", L"Profile saved.", L"Profile loaded.", L"This slot is empty. Save a profile in it first.",
    L"The profile could not be saved."};
const ProfileTexts kProfileSpanish{
    L"Perfiles", L"Guarda todos los ajustes vanilla en una de cinco ranuras y cárgalos después, aquí o con un atajo ([Hotkeys] Profile.1 = Ctrl+1 en NativeTweaks.ini).",
    L"Cargar: ", L"Aplica todas las opciones guardadas en esta ranura. Los perfiles se guardan en DLLs\\NativeTweaks\\profiles\\vanilla; allí puedes renombrarlos.",
    L"Guardar en ranura ", L"Guarda los ajustes vanilla actuales en esta ranura y reemplaza lo que tenía.",
    L"(vacía)", L"Perfil guardado.", L"Perfil cargado.", L"Esta ranura está vacía. Guarda antes un perfil en ella.",
    L"No se pudo guardar el perfil."};
const ProfileTexts kProfileChinese{
    L"配置档", L"将所有原版设置保存到五个槽位之一，之后可在此处或用快捷键加载（NativeTweaks.ini 中 [Hotkeys] Profile.1 = Ctrl+1）。",
    L"加载：", L"应用此槽位保存的所有选项。配置档保存在 DLLs\\NativeTweaks\\profiles\\vanilla，可在那里重命名。",
    L"保存到槽位 ", L"将当前原版设置保存到此槽位，替换原有内容。",
    L"（空）", L"配置档已保存。", L"配置档已加载。", L"此槽位为空。请先保存配置档。",
    L"无法保存配置档。"};

const ProfileTexts& profile_texts(std::string_view lang) noexcept {
    if (lang == "es") return kProfileSpanish;
    if (lang == "zh" || lang == "zht") return kProfileChinese;
    return kProfileEnglish;
}

std::wstring maybe_traditional(std::string_view lang, const wchar_t* text) {
    return lang == "zht" ? zht::convert(std::wstring_view(text)) : std::wstring(text);
}

live_text::Texts profile_row_texts(std::string_view lang, int slot, bool save) {
    const ProfileTexts& t = profile_texts(lang);
    if (save) {
        return {maybe_traditional(lang, t.save) + std::to_wstring(slot),
            maybe_traditional(lang, t.save_help), {}, {}};
    }
    std::wstring name = profile_name(slot);
    if (name.empty()) name = maybe_traditional(lang, t.empty);
    return {maybe_traditional(lang, t.load) + name, maybe_traditional(lang, t.load_help), {}, {}};
}

void profile_alert(const wchar_t* message) noexcept {
    if (!g_registration) return;
    erui::AlertOptions options{};
    options.buttons = erui::AlertButtons::ok;
    options.placement = erui::AlertPlacement::bottom;
    (void)g_registration->alert(message, options);
}

std::wstring profile_message(const wchar_t* ProfileTexts::*field) {
    return maybe_traditional(g_profile_language, profile_texts(g_profile_language).*field);
}

void ERUI_CALL save_profile_clicked(void* target) noexcept {
    const int slot = static_cast<int>(reinterpret_cast<std::uintptr_t>(target));
    try {
        const bool ok = write_profile(slot);
        log(ok ? L"Vanilla settings: profile saved." : L"Vanilla settings: a profile could not be saved.");
        // The Load row shows the slot's name.
        if (ok && g_registration) live_text::apply(g_registration->provider_handle(), g_profile_language.c_str());
        profile_alert(profile_message(ok ? &ProfileTexts::saved : &ProfileTexts::failed).c_str());
    } catch (...) {
    }
}

void ERUI_CALL load_profile_clicked(void* target) noexcept {
    const int slot = static_cast<int>(reinterpret_cast<std::uintptr_t>(target));
    try {
        const bool ok = load_profile_slot(slot);
        profile_alert(profile_message(ok ? &ProfileTexts::loaded : &ProfileTexts::missing).c_str());
    } catch (...) {
    }
}

void add_profile_page(erui::Page& root, std::string_view lang, const wchar_t* parent_title) {
    // Saving refreshes the menu texts in this language until the language
    // selector applies another one (the producers below record it).
    g_profile_language = std::string(lang);
    const ProfileTexts& t = profile_texts(lang);
    const std::wstring title = maybe_traditional(lang, t.page);
    auto page = root.add_submenu(title, maybe_traditional(lang, t.page_help), parent_title, title);
    live_text::track_submenu(page.handle(), [](const char* l) {
        const ProfileTexts& x = profile_texts(l);
        const std::wstring name = maybe_traditional(l, x.page);
        return live_text::Texts{name, maybe_traditional(l, x.page_help),
            maybe_traditional(l, texts_for(l).root.label), name};
    });
    for (int slot = 1; slot <= kProfileSlots; ++slot) {
        void* const user = reinterpret_cast<void*>(static_cast<std::uintptr_t>(slot));
        for (const bool save : {false, true}) {
            const live_text::Texts row_texts = profile_row_texts(lang, slot, save);
            const erui::RowHandle row = save
                ? page.add_button(row_texts.label, row_texts.help, &save_profile_clicked, user)
                : page.add_button(row_texts.label, row_texts.help, &load_profile_clicked, user);
            live_text::track_row(row, [slot, save](const char* l) {
                g_profile_language = l;
                return profile_row_texts(l, slot, save);
            });
        }
    }
}

} // namespace

int audit(LogFn log_fn) noexcept {
    g_log = log_fn;
    for (Site* site : kSites) resolve(*site);
    resolve_dialog_yes_reads(false);
    resolve(g_intro);
    resolve_window();
    resolve_map_flags();
    resolve_fov_site();
    resolve_downed_site();
    resolve_jump_site();
    const bool fall_site = find_unique(parse_pattern(
        "48 8b c4 55 53 56 57 41 54 41 56 41 57 48 8d 68 b1 48 81 ec c0 00 00 00 48 c7 45 b7 fe ff ff ff "
        "0f 29 70 b8 0f 29 78 a8 44 0f 29 40 98 44 0f 29 48 88 48 8b 05 ? ? ? ? 48 33 c4 48 89 45 cf "
        "41 0f b6 f9 44 0f 28 ca 44 0f 28 c1")) != nullptr;
    log(fall_site ? L"Vanilla site 'fall damage' found." : L"Vanilla site 'fall damage' not found (or ambiguous).");
    if (const std::uint8_t* status_site = find_unique(parse_pattern(kStatusImmunityPattern))) {
        wchar_t message[128]{};
        swprintf_s(message, L"Vanilla site 'status immunity' found at eldenring.exe+0x%llX.",
            static_cast<unsigned long long>(status_site - reinterpret_cast<std::uint8_t*>(game::image())));
        log(message);
    } else {
        log(L"Vanilla site 'status immunity' not found (or ambiguous); the option would use the buildup hold.");
    }
    resolve_chr_enable();
    if (const std::uint8_t* toughness_site = find_unique(parse_pattern(kToughnessPattern))) {
        wchar_t message[128]{};
        swprintf_s(message, L"Vanilla site 'toughness' found at eldenring.exe+0x%llX.",
            static_cast<unsigned long long>(toughness_site + kToughnessSite -
                reinterpret_cast<std::uint8_t*>(game::image())));
        log(message);
    } else {
        log(L"Vanilla site 'toughness' not found (or ambiguous).");
    }
    for (const LockRangeSite& site : kLockRangeSites) {
        const std::uint8_t* found = find_unique(parse_pattern(site.pattern));
        wchar_t message[160]{};
        if (found)
            swprintf_s(message, L"Vanilla site 'lock-on range %S' found at eldenring.exe+0x%llX.", site.name,
                static_cast<unsigned long long>(found + site.offset - reinterpret_cast<std::uint8_t*>(game::image())));
        else
            swprintf_s(message, L"Vanilla site 'lock-on range %S' not found (or ambiguous).", site.name);
        log(message);
    }
    g_statics = game::resolve_statics(g_log);
    int unavailable = fall_site ? 0 : 1;
    for (std::size_t i = 0; i < count; ++i) {
        if (available(static_cast<Id>(i))) continue;
        ++unavailable;
        wchar_t message[160]{};
        swprintf_s(message, L"AUDIT: option %ls would be greyed out.", g_options[i].key);
        log(message);
    }
    wchar_t summary[160]{};
    swprintf_s(summary, L"AUDIT: %d of %d options would be unavailable.", unavailable,
        static_cast<int>(count));
    log(summary);
    return unavailable;
}

void initialize(const std::wstring& config_path, LogFn log_fn) noexcept {
    g_log = log_fn;
    g_config_path = config_path;
    g_diagnostics = !g_config_path.empty() &&
        (GetPrivateProfileIntW(L"Logging", L"EnableLog", 0, g_config_path.c_str()) != 0 ||
         GetPrivateProfileIntW(L"Diagnostics", L"PerfLog", 0, g_config_path.c_str()) != 0);
    perf::configure(g_config_path, g_log);
    start_spirit_ashes();
    g_perf_death_hook = perf::timer(L"hook_death");
    g_perf_death_lock = perf::timer(L"hook_death_lock_wait");
    g_perf_downed_hook = perf::timer(L"hook_downed");
    g_perf_downed_lock = perf::timer(L"hook_downed_lock_wait");
    for (Site* site : kSites) resolve(*site);
    install_dialog_yes_hooks();
    install_window_hooks();
    resolve(g_intro);
    resolve_window();
    resolve_map_flags();
    resolve_fov_site();
    resolve_downed_site();
    resolve_jump_site();
    install_fall_hook();
    install_status_immunity_hook();
    install_lock_range_hooks();
    install_toughness_hook();
    install_death_hook();
    resolve_chr_enable();
    g_statics = game::resolve_statics(g_log);
    load_settings();
    load_ranges();
    g_param_event = CreateEventW(nullptr, FALSE, TRUE, nullptr);
    for (std::size_t i = 0; i < count; ++i) {
        if (!available(static_cast<Id>(i))) continue;
        if (value_of(static_cast<Id>(i)) != g_options[i].fallback) apply(static_cast<Id>(i));
    }
    // The layout options are skipped above while ERNativeUI.dll is not loaded yet.
    send_hud_layout(L"startup");
    if (!g_config_path.empty()) {
        g_save_event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (g_save_event) start_thread(&save_worker, L"save");
    }
    if (g_pause.ready) start_thread(&pause_worker, L"pause");
    if (g_dialog_yes_data) start_thread(&dialog_pointer_worker, L"dialog_pointer");
    if (g_window_slot) start_thread(&window_option_worker, L"window_option");
    if (g_param_event && g_statics.layout_2_7_1 && g_statics.regulation_manager) start_thread(&param_worker, L"param");
    if (g_statics.layout_2_7_1 && g_statics.world_chr_man) {
        start_thread(&speed_worker, L"speed");
        start_thread(&regen_worker, L"regen");
        start_thread(&fall_worker, L"fall");
        start_thread(&enemy_speed_worker, L"enemy_speed");
        start_thread(&jump_worker, L"jump");
        start_thread(&dead_enemy_worker, L"dead_enemy");
        start_thread(&status_worker, L"status");
        if (g_windows) start_thread(&window_worker, L"dodge_parry");
    }
    if (g_statics.layout_2_7_1) start_thread(&time_worker, L"time");
    if (g_statics.layout_2_7_1) start_thread(&weather_worker, L"weather");
    if (!g_config_path.empty()) {
        hotkeys::Target target{};
        target.is_toggle = &hotkey_is_toggle;
        target.get = &hotkey_get;
        target.set = &hotkey_set;
        target.load_profile = &hotkey_load_profile;
        target.log = &hotkey_log;
        target.thread_started = &perf::thread_started;
        hotkeys::start(g_config_path, target);
    }
    perf::start();
    log(L"Vanilla settings initialized; saved choices applied.");
}

void sync_hud_layout(const wchar_t* when) noexcept {
    send_hud_layout(when);
}

void attach_registration(const erui::Registration* registration) noexcept {
    g_registration = registration;
    // Fork export (r115): labels of options away from their game default are
    // coloured. An older ERNativeUI.dll simply lacks it.
    const HMODULE host = GetModuleHandleW(L"ERNativeUI.dll");
    const auto set_default = host
        ? reinterpret_cast<SetRowDefaultFn>(GetProcAddress(host, "ERNativeUIFork_SetRowDefault")) : nullptr;
    // Optional (off by default): [General] HighlightChangedOptions = 1, or the
    // optional package's marker file NativeTweaks.HighlightChanged.ini beside
    // NativeTweaks.ini (so installing it never replaces the user's INI).
    const bool enabled = optional_feature(L"HighlightChangedOptions", L"NativeTweaks.HighlightChanged.ini");
    if (!registration || !set_default || !enabled) return;
    const auto set_default_ex =
        reinterpret_cast<SetRowDefaultExFn>(GetProcAddress(host, "ERNativeUIFork_SetRowDefaultEx"));
    std::size_t failures{};
    for (std::size_t i = 0; i < count; ++i) {
        const auto& option = g_options[i];
        if (option.row == ERUI_INVALID_ROW) continue;
        const std::int64_t value = is_named(static_cast<Id>(i))
            ? option.fallback - option.minimum : option.fallback;
        const ERUI_Result result = set_default_ex
            ? set_default_ex(registration->provider_handle(), option.row, value,
                  higher_is_worse(static_cast<Id>(i)) ? 1u : 0u)
            : set_default(registration->provider_handle(), option.row, value);
        if (result != ERUI_OK) ++failures;
    }
    g_set_default = set_default;
    g_set_default_ex = set_default_ex;
    if (failures) log(L"Vanilla settings: some default values were not accepted by ERNativeUI.");
}

namespace {

// language: "en", "es", "zh" or "zht" (Traditional, converted from kChinese).
const Texts& base_texts_for(std::string_view lang) {
    if (lang == "es") return kSpanish;
    if (lang == "zh") return kChinese;
    if (lang == "zht") {
        static const Texts traditional = zht::convert_table(kChinese);
        return traditional;
    }
    return kEnglish;
}

// Built-in table with the lang\<code>.txt overrides, matched by English text.
Texts overridden_table(std::string_view lang) noexcept {
    Texts result = base_texts_for(lang);
    auto** fields = reinterpret_cast<const wchar_t**>(&result);
    const auto* english = reinterpret_cast<const wchar_t* const*>(&kEnglish);
    for (std::size_t i = 0; i < sizeof(Texts) / sizeof(const wchar_t*); ++i)
        fields[i] = translate_text(lang, english[i], fields[i]);
    return result;
}

const Texts& texts_for(std::string_view lang) {
    static const Texts tables[]{overridden_table("en"), overridden_table("es"),
        overridden_table("zh"), overridden_table("zht")};
    if (lang == "es") return tables[1];
    if (lang == "zh") return tables[2];
    if (lang == "zht") return tables[3];
    return tables[0];
}

// Value names of the named sliders, in WeatherType order (see weather_worker).
// Named after the WeatherParam rows they set (Smithbox row names): 21 is rainy
// clouds, 30 windy rain, 31 windy fog and 60 windy clouds (up to r135 they
// were called heavy rain, storm, battle storm and sandstorm).
constexpr std::size_t kWeatherCount = 16;
constexpr const wchar_t* kWeatherEnglish[kWeatherCount]{
    L"Sunny", L"Clear", L"Light clouds", L"Cloudy", L"Rain", L"Rainy clouds", L"Windy rain",
    L"Windy fog", L"Snow", L"Heavy snow", L"Fog", L"Heavy fog", L"Windy clouds",
    L"Rainy fog", L"Snowy fog", L"Showers"};
// About 13 characters at most: the slider value field is 160 px wide (10 px
// left margin) with the packaged GFX (--wide-slider-values) and clips the rest.
constexpr const wchar_t* kWeatherSpanish[kWeatherCount]{
    L"Soleado", L"Despejado", L"Nubes ligeras", L"Nublado", L"Lluvia",
    L"Nubarrones", L"Temporal", L"Bruma ventosa", L"Nieve", L"Nieve fuerte",
    L"Niebla", L"Niebla densa", L"Ventoso", L"Niebla húmeda", L"Niebla helada",
    L"Chubascos"};
// ChargedAttacksLaunch values, in kLaunchLevels order (same width limit).
constexpr std::size_t kLaunchCount = 4;
constexpr const wchar_t* kLaunchEnglish[kLaunchCount]{L"Off", L"Launch back", L"Far back", L"Upward"};
constexpr const wchar_t* kLaunchSpanish[kLaunchCount]{L"Desactivado", L"Hacia atrás", L"Lejos", L"Hacia arriba"};
constexpr const wchar_t* kLaunchChinese[kLaunchCount]{L"关闭", L"击飞", L"远距击飞", L"向上击飞"};
// WorldDifficulty values: the game's own cycle, then NG+1 to NG+7.
constexpr std::size_t kJourneyCount = 8;
constexpr const wchar_t* kJourneyEnglish[kJourneyCount]{
    L"Your save", L"NG+1", L"NG+2", L"NG+3", L"NG+4", L"NG+5", L"NG+6", L"NG+7"};
constexpr const wchar_t* kJourneySpanish[kJourneyCount]{
    L"Tu partida", L"NG+1", L"NG+2", L"NG+3", L"NG+4", L"NG+5", L"NG+6", L"NG+7"};
constexpr const wchar_t* kJourneyChinese[kJourneyCount]{
    L"存档周目", L"NG+1", L"NG+2", L"NG+3", L"NG+4", L"NG+5", L"NG+6", L"NG+7"};
constexpr const wchar_t* kWeatherChinese[kWeatherCount]{
    L"晴天", L"晴朗", L"少云", L"多云", L"雨", L"雨云", L"风雨", L"风雾", L"雪",
    L"大雪", L"雾", L"浓雾", L"大风多云", L"雨雾", L"雪雾", L"阵雨"};

std::vector<std::wstring> value_names(std::string_view lang, Id i) {
    if (!is_named(i)) return {};
    const wchar_t* const* english = kWeatherEnglish;
    const wchar_t* const* spanish = kWeatherSpanish;
    const wchar_t* const* chinese = kWeatherChinese;
    std::size_t count = kWeatherCount;
    if (i == charged_launch) {
        english = kLaunchEnglish, spanish = kLaunchSpanish, chinese = kLaunchChinese;
        count = kLaunchCount;
    } else if (i == world_journey) {
        english = kJourneyEnglish, spanish = kJourneySpanish, chinese = kJourneyChinese;
        count = kJourneyCount;
    }
    const wchar_t* const* names = english;
    if (lang == "es") names = spanish;
    else if (lang == "zh" || lang == "zht") names = chinese;
    std::vector<std::wstring> result;
    result.reserve(count);
    for (std::size_t n = 0; n < count; ++n)
        result.push_back(translate_text(lang, english[n],
            lang == "zht" ? zht::convert(names[n]) : names[n]));
    return result;
}

// Speed sliders list the animation ids they cover.
std::wstring option_help(std::string_view lang, Id i) {
    const Texts& t = texts_for(lang);
    if ((i == hud_screen_edges || i == hud_scale || i == menu_edges) && hud_layout_export() &&
        g_hud_layout_unsupported.load(std::memory_order_acquire)) {
        // ERNativeUI is there but its runtime GFX patch is off.
        const wchar_t* text = lang == "es" ? L"Necesita [GFX] EnableRuntimePatching = 1 en ERNativeUI.ini."
            : lang == "zh" ? L"需要在 ERNativeUI.ini 中设置 [GFX] EnableRuntimePatching = 1。"
            : lang == "zht" ? zht::convert(L"需要在 ERNativeUI.ini 中设置 [GFX] EnableRuntimePatching = 1。")
            : L"Needs [GFX] EnableRuntimePatching = 1 in ERNativeUI.ini.";
        return translate_text(lang, L"Needs [GFX] EnableRuntimePatching = 1 in ERNativeUI.ini.", text);
    }
    if (!available(i)) return t.unavailable;
    std::wstring help = t.options[i].help;
    const std::wstring hotkey = hotkeys::hotkey_for(g_options[i].key);
    if (!hotkey.empty()) {
        if (lang == "es") help += L" Atajo: " + hotkey + L".";
        else if (lang == "zh" || lang == "zht") help += L" 快捷键：" + hotkey + L"。";
        else help += L" Hotkey: " + hotkey + L".";
    }
    for (std::size_t c = 0; c < kAnimCategories; ++c) {
        if (kSpeedIds[c] != i) continue;
        const std::wstring ids = describe_ids(c);
        if (ids.empty()) break;
        if (lang == "zh") help += L" 动作 ID：" + ids + L"。";
        else if (lang == "zht") help += L" 動作 ID：" + ids + L"。";
        else help += L" Ids: " + ids + L".";
        break;
    }
    return help;
}

} // namespace

void add_pages(erui::Page& parent, const char* language) noexcept {
    const std::string_view lang = language ? language : "en";
    const Texts& t = texts_for(lang);
    auto root = parent.add_submenu(t.root.label, t.root.help, t.root.label, t.root.label);
    live_text::track_submenu(root.handle(), [](const char* l) {
        const Texts& x = texts_for(l);
        return live_text::Texts{x.root.label, x.root.help, x.root.label, x.root.label};
    });

    for (const auto& layout : kLayout) {
        const Text& category = t.categories[layout.category];
        auto page = root.add_submenu(category.label, category.help, t.root.label, category.label);
        const Category which = layout.category;
        live_text::track_submenu(page.handle(), [which](const char* l) {
            const Texts& x = texts_for(l);
            return live_text::Texts{x.categories[which].label, x.categories[which].help,
                x.root.label, x.categories[which].label};
        });
        for (const Id i : layout.ids) {
            auto& option = g_options[i];
            if (!t.options[i].label || !t.options[i].help) {  // a missing text must not crash the menu
                log(L"Vanilla settings: an option has no menu text; skipped.");
                continue;
            }
            const bool ok = available(i);
            std::wstring help;
            try {
                help = option_help(lang, i);
            } catch (...) {
                help = ok ? t.options[i].help : t.unavailable;
            }
            if (!ok && option.kind != Kind::toggle) {
                // An unavailable slider is shown as a disabled toggle: in game a
                // disabled native slider row still looked active (HUD scale with
                // RuntimePatch = 0), while disabled toggles are greyed out. The
                // placeholder is not the option's row, so value, default and
                // slider-text updates skip it; it has no multiplier row either.
                const erui::RowHandle placeholder =
                    page.add_toggle(t.options[i].label, help, 0, nullptr, nullptr, false);
                live_text::track_row(placeholder, [i](const char* l) {
                    const bool complete = texts_for(l).options[i].label && texts_for(l).options[i].help;
                    const char* const code = complete ? l : "en";
                    return live_text::Texts{texts_for(code).options[i].label, option_help(code, i), {}, {}};
                });
                continue;
            }
            if (option.kind == Kind::toggle) {
                option.row = page.add_toggle(t.options[i].label, help, option.value != 0,
                    &option_changed, &option, ok);
            } else if (is_named(i)) {
                std::vector<std::wstring> names;
                try {
                    names = value_names(lang, i);
                } catch (...) {
                }
                if (names.size() != static_cast<std::size_t>(option.maximum - option.minimum + 1)) {
                    log(L"Vanilla settings: a named slider has the wrong number of names; skipped.");
                    continue;
                }
                erui::CustomSliderOptions slider{};
                slider.value_count = static_cast<std::uint32_t>(names.size());
                slider.initial_index = static_cast<std::uint32_t>(option.value - option.minimum);
                slider.enabled = ok;
                slider.format_value = [&names](std::uint32_t index, std::uint32_t) {
                    return names[index];
                };
                option.row = page.add_custom_slider(t.options[i].label, help, slider,
                    &named_slider_changed, &option);
            } else {
                erui::NumericSliderOptions slider{};
                slider.minimum = option.minimum;
                slider.maximum = option.maximum;
                slider.step = option.step;
                slider.initial_value = option.value;
                slider.decimal_places = 0u;
                slider.suffix = option.suffix;
                slider.enabled = ok;
                option.row = page.add_slider(t.options[i].label, help, slider,
                    &slider_changed, &option);
            }
            live_text::track_row(option.row, [i](const char* l) {
                // Another language may lack this text: keep the English one.
                const bool complete = texts_for(l).options[i].label && texts_for(l).options[i].help;
                const char* const code = complete ? l : "en";                return live_text::Texts{texts_for(code).options[i].label, option_help(code, i), {}, {},
                    value_names(l, i)};
            });
        }
        const auto page_restore = page.add_button(t.restore_page.label, t.restore_page.help,
            &restore_page_defaults, const_cast<Layout*>(&layout));
        live_text::track_row(page_restore, [](const char* l) {
            const Texts& x = texts_for(l);
            return live_text::Texts{x.restore_page.label, x.restore_page.help, {}, {}};
        });
    }

    add_profile_page(root, lang, t.root.label);

    const auto restore = root.add_button(t.restore.label, t.restore.help, &restore_defaults, nullptr);
    live_text::track_row(restore, [](const char* l) {
        const Texts& x = texts_for(l);
        return live_text::Texts{x.restore.label, x.restore.help, {}, {}};
    });
}

} // namespace vanilla
