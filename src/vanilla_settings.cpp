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
#include <TlHelp32.h>
#include <intrin.h>

#pragma comment(lib, "synchronization.lib")  // WaitOnAddress

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <map>
#include <set>
#include <unordered_map>
#include <unordered_set>
#include <tuple>
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

// A write over more than one instruction is only safe while no other thread
// can be between those instructions. Once the game runs (after initialize,
// when options turn hooks on mid-game), the other threads are suspended and
// the write waits, at most 200 ms, until none of them has its instruction
// pointer inside the region: the same idea as MinHook's freeze. At startup
// the old path is kept (every mod installs hooks then, some freezing threads
// themselves). Nothing here allocates or takes a lock while threads are
// suspended; a thread that cannot be checked counts as inside.
enum class FrozenWrite { written, busy, unavailable };
std::atomic<bool> g_freeze_writes{false};

FrozenWrite write_code_frozen(std::uint8_t* address, const std::uint8_t* bytes, std::size_t count) noexcept {
    constexpr std::size_t kMaxThreads = 1024;
    static DWORD ids[kMaxThreads];
    static HANDLE handles[kMaxThreads];
    static bool suspended[kMaxThreads];
    static SRWLOCK lock = SRWLOCK_INIT;
    AcquireSRWLockExclusive(&lock);
    std::size_t total = 0;
    const DWORD self = GetCurrentThreadId();
    const DWORD process = GetCurrentProcessId();
    const HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snapshot == INVALID_HANDLE_VALUE) {
        ReleaseSRWLockExclusive(&lock);
        return FrozenWrite::unavailable;
    }
    THREADENTRY32 entry{};
    entry.dwSize = sizeof(entry);
    bool overflow{};
    for (BOOL more = Thread32First(snapshot, &entry); more; more = Thread32Next(snapshot, &entry)) {
        if (entry.th32OwnerProcessID != process || entry.th32ThreadID == self) continue;
        if (total == kMaxThreads) {
            overflow = true;
            break;
        }
        ids[total++] = entry.th32ThreadID;
    }
    CloseHandle(snapshot);
    if (overflow) {
        ReleaseSRWLockExclusive(&lock);
        return FrozenWrite::unavailable;
    }
    std::size_t opened = 0;
    bool missing{};
    for (std::size_t i = 0; i < total; ++i) {
        const HANDLE thread = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT, FALSE, ids[i]);
        if (thread) {
            handles[opened++] = thread;
        } else if (GetLastError() != ERROR_INVALID_PARAMETER) {  // not "the thread has exited"
            missing = true;
        }
    }
    const auto begin = reinterpret_cast<std::uintptr_t>(address);
    const auto end = begin + count;
    FrozenWrite result = missing ? FrozenWrite::unavailable : FrozenWrite::busy;
    const ULONGLONG deadline = GetTickCount64() + 200;
    while (!missing) {
        bool inside{};
        for (std::size_t i = 0; i < opened; ++i) suspended[i] = false;
        for (std::size_t i = 0; i < opened && !inside; ++i) {
            if (SuspendThread(handles[i]) == static_cast<DWORD>(-1)) continue;  // exited meanwhile
            suspended[i] = true;
            CONTEXT context{};
            context.ContextFlags = CONTEXT_CONTROL;
            if (!GetThreadContext(handles[i], &context) || (context.Rip > begin && context.Rip < end))
                inside = true;
        }
        if (!inside) {
            std::memcpy(address, bytes, count);
            FlushInstructionCache(GetCurrentProcess(), address, count);
            result = FrozenWrite::written;
        }
        for (std::size_t i = 0; i < opened; ++i)
            if (suspended[i]) ResumeThread(handles[i]);
        if (result == FrozenWrite::written || GetTickCount64() >= deadline) break;
        Sleep(1);
    }
    for (std::size_t i = 0; i < opened; ++i) CloseHandle(handles[i]);
    ReleaseSRWLockExclusive(&lock);
    return result;
}

bool write_code(std::uint8_t* address, const std::uint8_t* bytes, std::size_t count) noexcept {
    if (!address || !count) return false;
    DWORD old_protect{};
    if (!VirtualProtect(address, count, PAGE_EXECUTE_READWRITE, &old_protect)) return false;

    bool ok{};
    const std::size_t offset = reinterpret_cast<std::uintptr_t>(address) & 15;
    // Five bytes or more can cover several instructions (jumps to hooks),
    // even inside one 16-byte block: those go through the frozen write first.
    const FrozenWrite frozen = count >= 5 && g_freeze_writes.load(std::memory_order_acquire)
        ? write_code_frozen(address, bytes, count) : FrozenWrite::unavailable;
    if (frozen == FrozenWrite::written) {
        ok = true;
    } else if (frozen == FrozenWrite::busy) {
        ok = false;  // a thread stayed inside the region: writing now could crash it
    } else if (offset + count <= 16) {
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
    0x2F, "0f 11 49 20", {0x66, 0x0F, 0xEF, 0xC9}};
// Forced sharpening (Nexus request): GXAntiAliasingFilter runs SharpenFilter.ppo
// after TempAA only if SharpenIntensity (+0x6C) is above 0; otherwise it does the
// game's own plain copy to the same target. jbe -> always jump to that copy.
// 2.7.2.0: 0x1ADA370.
Site g_sharpen{L"sharpening",
    "f3 ? 0f 10 ? 6c ? 0f 2f ? 0f 86 ? ? ? ? ? 83 ? 00 02 00 00 00 0f 84",
    0x0A, "0f 86", {0x90, 0xE9}};
Site g_animation{L"animation distance",
    "c7 ? ? ? 01 00 00 00 f3 ? 0f 10 ? ? ? f3 ? 0f 10 ? ? ? f3 0f 59 ? ? ? ? ? ? 0f 28 ? f3 ? 0f 5c ? ? 58",
    0x48, "f3 41 0f 5e 4c 24 54", {0x0F, 0x57, 0xC9, 0x90, 0x90, 0x90, 0x90}};
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
    0, "c7 45 ef 3c 00 00 00", {0xC7, 0x45, 0xEF, 0x00, 0x00, 0x00, 0x00}};
Site g_ultrawide{L"ultrawide",
    "48 c7 45 b8 fe ff ff ff 48 89 58 10 48 89 70 18 48 89 78 20 0f 29 70 c8 48 8b",
    0x94, "74 ? 45 8b 94 cc", {0xEB}};
// WorldChrMan's respawn request: "if (state == 0) state = 1" on the field at
// +0x1E524 (2.7.1.0), the only writer of 1. Every world reset (resting or
// passing time at a grace, dying, the warp/fade reset) reaches it through
// eldenring.exe+0x5F4410; the WorldChrMan_Respawn task then revives every
// enemy without an event flag. A ret skips the request and nothing else of
// the reset: estus, HP, objects and event scripts reset as usual.
Site g_enemy_respawn{L"enemy respawn",
    "83 b9 ? ? ? ? 00 75 0a c7 81 ? ? ? ? 01 00 00 00 c3",
    0, "83 b9", {0xC3}};

// Summon anywhere: the game's summon state (GetBuddyState, eldenring.exe+0x4B73D0
// in 2.7.2.0) only lets a spirit ash be used while the player has SpEffect
// state 0x175, which the game gives inside a summoning monument's area, and a
// monument in SummonBuddyManager +0x38. Outside the monument's own map tile
// the state is missing (r140 test, Caelid and Radahn's battlefield): skipping
// that check leaves the monument, which Multiple Spirit Ashes keeps from the
// last one found (apply_stone_fallback).
Site g_summon_area{L"summoning area",
    "ba 75 01 00 00 48 8b 88 78 01 00 00 e8 ? ? ? ? 84 c0 74 ? 8b 43 38 85 c0 74 ? 3b 43 3c",
    17, "84 c0 74 ?", {0x84, 0xC0, 0x90, 0x90}};  // the jz alone straddles a 16-byte line: write_code refused it

// The item side of the same rule: a small function (eldenring.exe+0x4B76F0 in
// 2.7.2.0) answers "is the player in a summoning area" with that state alone
// and refused the spirit ash item away from monuments even with the check above
// skipped (r140 test). With Summon anywhere it returns true.
Site g_summon_area_item{L"summoning area (items)",
    "48 8b 80 08 e5 01 00 ba 75 01 00 00 48 8b 88 78 01 00 00 48 83 c4 28 e9",
    7, "ba 75 01 00 00 48 8b 88 78 01 00 00 48 83 c4 28 e9 ? ? ? ?",
    {0xB0, 0x01, 0x48, 0x83, 0xC4, 0x28, 0xC3, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90,
        0x90, 0x90, 0x90, 0x90}};

// Spirit bodies get a slot in WorldChrMan's summon_buddy ChrSet: 80 entries,
// slots 0-19 for other characters (Torrent among them) and a block of 10 per
// player from 20. The slot search (2.7.2.0 +0x493460) starts at
// (player + 2) * 10 and makes 10 tries round that block from a hint; no free
// slot means no body, with the FP already spent (r140 test: Lone Wolf Ashes
// gave 1 wolf of 3 with 9 bodies out, Black Knife Tiche none with 10).
// With several ashes at once:
// - the set gets 520 entries (+0x495590 allocates 80 * 16 bytes through the
//   game's allocator and builds 80 entries): a hook on that function's
//   epilogue, placed when the Hub starts, before the game creates the set,
//   allocates 520 zeroed entries (what the entry constructor leaves) through
//   the same allocator and frees the 80, so the game frees the larger array
//   itself (moving the array to one of our own corrupted the heap on the
//   next map load, c0000374). r140 rewrote the three sizes in the function's
//   body instead, and Seamless Co-op 2.0.1, which looks for those bytes
//   ("BA 00 05 00 00 41 B8 08 ..."), stopped at launch with a fatal error:
//   the body is left as it is, and a set somebody else already resized
//   (not 80 entries) is left alone;
// - the search starts at 20 for everyone and runs over every slot from 20 to
//   the set's real capacity (a code cave reads it, so a set created before the
//   patch keeps to its 80): up to 500 bodies.
// The only code deriving anything from a slot number is the next-search hint,
// (slot - 19) % 10, which stays a valid start.
constexpr std::uint32_t kBuddySetCapacity = 520;
Site g_buddy_slot_start{L"spirit slots (start)",
    "0f b6 c2 83 c0 02 8d 04 80 44 8d 14 00 b8 67 66 66 66 41 f7 e9",
    3, "83 c0 02 8d 04 80 44 8d 14 00", {0x41, 0xBA, 0x14, 0x00, 0x00, 0x00, 0x0F, 0x1F, 0x40, 0x00}};
// The 10-try loop; its replacement (a jump to the cave) is built on first use.
Site g_buddy_slot_loop{L"spirit slots (search)",
    "41 83 f9 0a 7d 0a 43 8d 3c 11 49 8d 1c 12 eb 0e 41 8d 7a f6 41 03 f9 49 8d 5a f6 48 03 da 48 03 d9 48 c1 e3 04 49 03 d8 4c 39 3b 74 14 41 ff c1 48 ff c1 48 83 f9 0a 7c c7 49 8b c7",
    0, "41 83 f9 0a 7d 0a 43 8d 3c 11 49 8d 1c 12 eb 0e 41 8d 7a f6 41 03 f9 49 8d 5a f6 48 03 da 48 03 d9 48 c1 e3 04 49 03 d8 4c 39 3b 74 14 41 ff c1 48 ff c1 48 83 f9 0a 7c c7", std::vector<std::uint8_t>(57, 0x90)};
// From "mov rcx,[allocator]" to the function's ret; the hook goes on the
// epilogue (+0x6B) and its replacement (a jump to the cave) is built on start.
constexpr const char* kBuddySetInit =
    "48 8b 0d ? ? ? ? 48 8b 01 ba 00 05 00 00 41 b8 08 00 00 00 ff 50 50 48 89 46 18 48 85 c0 74 ? "
    "bf 50 00 00 00 89 7e 10 33 d2 41 b8 00 05 00 00 48 8b c8 e8 ? ? ? ? 33 db 66 66 66 0f 1f 84 00 00 00 00 00 "
    "48 8b 4e 18 48 03 cb 48 89 4c 24 58 48 89 4c 24 60 74 08 33 d2 e8 ? ? ? ? 90 48 83 c3 10 48 83 ef 01 "
    "75 db 48 83 c4 30 5f 5e 5b c3";
constexpr std::ptrdiff_t kBuddySetEpilogue = 0x6B;
Site g_buddy_set_size{L"spirit set (size)", kBuddySetInit, kBuddySetEpilogue, "48 83 c4 30 5f 5e 5b c3",
    std::vector<std::uint8_t>(8, 0xCC)};
// Within range of the summoning monument: +0x4BD950 clears it every frame and
// sets it only when the monument is loaded near the player and the player is
// inside its region or range; every other path ends at one point. Outside,
// ashes summoned there are being dismissed and stay frozen, without
// collision, looping the summoning sound (r140 tests: Radahn's battlefield,
// Caelid, dungeons with no monument of their own). Summon anywhere sets it
// there: "cmp [rsi+B5h],r13b; je" becomes "mov byte [rsi+B5h],1".
Site g_summon_range{L"summoning range",
    "44 38 ae b5 00 00 00 74 07 44 88 ae b7 00 00 00 4c 39 ae 00 01 00 00",
    0, "44 38 ae b5 00 00 00 74 07", {0xC6, 0x86, 0xB5, 0x00, 0x00, 0x00, 0x01, 0x66, 0x90}};

// Craft in combat (Nexus request, skar450). The game asks one "in combat"
// check (CSSound+0x328 state; 0xDAA5E0 in 2.7.2.0) from five places; logged
// with the pause menu open in combat (r158-test29-dev), three of them answer
// for it: the crafting kit's own test (0x913400: call; test al,al; sete al),
// a flag (sete dl; 0x5111FC) and the menus' combat state with its timer
// (0x76B197; jz past it when out of combat). Only the first and the last are
// patched: their call becomes xor eax,eax ("not in combat"). The flag is
// ChrDataModule+0x19A bit 0x40, which only the stamina setter (0x438A80)
// reads: out of combat, sprinting costs no stamina. Patching it gave free
// sprinting in fights (Nexus bug report, Mickeekarlsson, 16-11), and the
// crafting kit does not need it. The map's (0x9C4E94) and the message-600
// one (0x7EFF7C, "cannot open the map in combat") are left alone. The idea
// of patching the first is from Erd-Tools (Nordgaren; no code taken).
// Both unique in 2.7.0.0 to 2.7.2.1 (0x912F50 / 0x76ACE7 in 2.7.0.0 and
// 2.7.1.x).
Site g_craft_combat{L"craft in combat",
    "48 83 ec 28 48 8b 0d ? ? ? ? 48 85 c9 75 ? 48 8d 0d ? ? ? ? e8 ? ? ? ? 4c 8b c8 4c 8d 05 ? ? ? ? "
    "ba b4 00 00 00 48 8d 0d ? ? ? ? e8 ? ? ? ? 48 8b 0d ? ? ? ? e8 ? ? ? ? 84 c0 0f 94 c0 48 83 c4 28 c3",
    0x3E, "e8 ? ? ? ?", {0x31, 0xC0, 0x90, 0x90, 0x90}};
Site g_craft_combat_menu{L"craft in combat (menu state)",
    "48 8b 0d ? ? ? ? e8 ? ? ? ? 84 c0 74 67 f3 0f 10 47 0c f3 0f 11 44 24 40 8b 44 24 40 a9 ff ff 7f 00 c6 47 08 01",
    0x07, "e8 ? ? ? ?", {0x31, 0xC0, 0x90, 0x90, 0x90}};

// Map in combat (user, 14-11): the other two places that ask the same "in
// combat" check. Opening the map shows message 600 ("cannot open the map in
// combat"; 0x7EFF7C), and the open map closes when a fight starts (0x9C4E94:
// in combat -> close). Their call becomes xor eax,eax there too. The same
// idea as Erd-Tools' map in combat (Nordgaren; no code taken). Unique in
// 2.7.0.0 to 2.7.2.1 (0x7EFACC / 0x9C49E4 in 2.7.0.0 and 2.7.1.x).
Site g_map_combat_open{L"map in combat (open)",
    "48 8b 0d ? ? ? ? e8 ? ? ? ? 84 c0 74 2e c7 45 50 58 02 00 00 c7 45 54 02 00 00 00",
    0x07, "e8 ? ? ? ?", {0x31, 0xC0, 0x90, 0x90, 0x90}};
Site g_map_combat_close{L"map in combat (close)",
    "48 8b 0d ? ? ? ? e8 ? ? ? ? 84 c0 75 ? 38 83 ? ? ? ? 75 ? 83 e6 fe",
    0x07, "e8 ? ? ? ?", {0x31, 0xC0, 0x90, 0x90, 0x90}};

Site* const kSites[]{&g_camera_rotate, &g_camera_reset, &g_vignette, &g_chromatic, &g_sharpen,
    &g_craft_combat, &g_craft_combat_menu, &g_map_combat_open, &g_map_combat_close,
    &g_animation, &g_rune_loss, &g_pause, &g_frametime, &g_fullscreen_cap, &g_ultrawide, &g_enemy_respawn,
    &g_summon_area, &g_summon_area_item, &g_summon_range, &g_buddy_slot_start, &g_buddy_slot_loop};

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

// Camera shake (Nexus request, BeastBlood1885). TAE events 144/145 ("Rumble
// Cam") and cutscenes start a rumble with RumbleMan::Play (0x567BC0 in
// 2.7.2.0; rcx = RumbleMan, edx = rumble id, r8 = position, r9 = falloff).
// It looks the id up in default.rumblebnd and takes a RumbleIns slot, which
// shakes the camera and, with Rumble on, the controller. While the option is
// on the trampoline returns at once, so no rumble starts; rumbles already
// playing end normally. The same function also drives controller vibration,
// which the option turns off with it. Unique in 2.7.0.0 to 2.7.2.1.
struct CameraShakeData {
    volatile std::uint32_t on;
    volatile std::uint32_t blocked;
};

CameraShakeData* g_camera_shake{};

constexpr const char* kRumblePlayPattern =
    "89 54 24 10 53 56 41 56 48 83 ec 40 48 8b d9 49 8b f1 48 8b 89 ? ? 00 00 4d 8b f0 48 85 c9 0f 84";

void install_camera_shake_hook() noexcept {
    std::uint8_t* site = find_unique(parse_pattern(kRumblePlayPattern));
    if (!site) {
        log(L"Vanilla site 'rumble play' not found (or ambiguous); Disable camera shake is unavailable.");
        return;
    }
    constexpr std::size_t kStolen = 15;  // mov [rsp+10h],edx; push rbx; push rsi; push r14; sub rsp,40h; mov rbx,rcx
    auto* code = static_cast<std::uint8_t*>(VirtualAlloc(
        nullptr, 4096, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
    if (!code) return;
    g_camera_shake = reinterpret_cast<CameraShakeData*>(code + 128);
    g_camera_shake->on = 0;
    g_camera_shake->blocked = 0;

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
    put({0x83, 0x3D}); put_rel(&g_camera_shake->on, 1); put({0x00});        // cmp dword [on],0
    put({0x74, 0x0A});                                                       // je original
    put({0xF0, 0xFF, 0x05}); put_rel(&g_camera_shake->blocked, 0);          // lock inc [blocked]
    put({0x31, 0xC0});                                                       // xor eax,eax (no RumbleIns)
    put({0xC3});                                                             // ret (no rumble)
    std::memcpy(p, site, kStolen); p += kStolen;                             // original prologue
    put({0xFF, 0x25, 0x00, 0x00, 0x00, 0x00});                               // jmp [rip+0]
    const std::uint64_t resume = reinterpret_cast<std::uint64_t>(site + kStolen);
    std::memcpy(p, &resume, 8);
    FlushInstructionCache(GetCurrentProcess(), code, 128);

    std::uint8_t jump[kStolen]{0xFF, 0x25, 0x00, 0x00, 0x00, 0x00};
    const auto trampoline = reinterpret_cast<std::uint64_t>(code);
    std::memcpy(jump + 6, &trampoline, 8);
    jump[14] = 0x90;
    if (!write_code(site, jump, sizeof(jump))) {
        g_camera_shake = nullptr;
        log(L"Vanilla rumble hook could not be written; Disable camera shake is unavailable.");
        return;
    }
    wchar_t message[128]{};
    swprintf_s(message, L"Vanilla rumble hook installed at eldenring.exe+0x%llX.",
        static_cast<unsigned long long>(site - reinterpret_cast<std::uint8_t*>(game::image())));
    log(message);
}

// Ragdoll on death (beta; user request, 13-11). Death animations of human
// characters (model c0000: you, NPCs, invaders) fade the body out with TAE
// event 193 (SetOpacityKeyframe). Player Ragdoll Physics (Dweet and
// HonorKnight26, Nexus 2284) swaps those events for 228 (RagdollReviveTime,
// parameters 0) in c0000.anibnd; here the same happens in memory: the 193
// handler (reached only from CSChrTaeAnimEvent's dispatcher) is hooked and,
// for a dead c0000 character whose event fades to opacity 0, the 228 handler
// runs instead with zero parameters. Tested in game with the RagdollTest
// experiment (13-11). Both handlers are unique in 2.7.0.0 to 2.7.2.1.
constexpr const char* kOpacityEventPattern =
    "48 89 74 24 20 57 48 83 ec 40 48 8b 42 20 48 8b 72 08 f3 0f 10 52 14 48 8b 79 18 48 8d 4c 24 58 "
    "f3 0f 10 40 08 f3 0f 5c 00 f3 0f 5c 10 f3 0f 10 1d ? ? ? ? 48 8d 44 24 50 0f 29 74 24 30 "
    "f3 0f 10 76 04 f3 0f 5c 36";
constexpr const char* kRagdollEventPattern =
    "48 89 5c 24 18 48 89 74 24 20 57 48 83 ec 20 48 8b 79 18 48 8b da 48 8b 72 08 48 8b 87 90 01 00 00 "
    "48 8b 48 28 e8 ? ? ? ? 84 c0 75 ? 48 8b 43 20 48 8d 4c 24 38 f3 0f 10 53 14 48 8b d6 "
    "f3 0f 10 1d ? ? ? ? c7 44 24 38 00 00 80 3f";

using TaeEventHandler = void(__fastcall*)(void* owner, void* event);
TaeEventHandler g_opacity_original{};  // copied prologue, then back into the 193 handler
TaeEventHandler g_ragdoll_event{};     // handler of event 228
volatile long g_ragdoll_on{};
volatile long g_ragdoll_count{};
alignas(16) float g_ragdoll_zero[4]{};

// The event's owner has the ChrIns at +0x18: +0x64 model number (0 = c0000),
// +0x190 modules, the first of which (data) keeps HP at +0x138.
bool dead_human(void* owner) noexcept {
    std::uintptr_t chr{}, modules{}, data{};
    std::int32_t model{-1}, hp{1};
    return game::read(reinterpret_cast<std::uintptr_t>(owner) + 0x18, chr) && chr &&
        game::read(chr + 0x64, model) && model == 0 && game::read(chr + 0x190, modules) && modules &&
        game::read(modules, data) && data && game::read(data + 0x138, hp) && hp <= 0;
}

// Keep bodies, defined with its other hooks below (after g_statics).
volatile long g_keep_bodies_on{};
volatile long g_bodies_kept{};
bool keep_dead_body(std::uintptr_t chr) noexcept;

void __fastcall ragdoll_opacity_event(void* owner, void* event) noexcept {
    if (g_ragdoll_on || g_keep_bodies_on) {
        const auto address = reinterpret_cast<std::uintptr_t>(event);
        std::uintptr_t params{}, chr{};
        float to{1.0f};
        std::int32_t model{-1};
        // Fades end at 0, or at 0.01 for Maliketh (c2110; the only death
        // animation between 0.001 and 0.2 in the 429 models, 13-11).
        if (game::read(address + 8, params) && params && game::read(params + 4, to) && to <= 0.02f &&
            game::read(reinterpret_cast<std::uintptr_t>(owner) + 0x18, chr) && chr &&
            game::read(chr + 0x64, model)) {
            if (g_ragdoll_on && model == 0 && dead_human(owner)) {
                // A copy of the event with zero parameters: the 228 handler reads
                // +0x08 (parameters), +0x14 (time) and +0x20 (start and end).
                alignas(16) std::uint8_t copy[0x40];
                if (game::safe_read(address, copy, sizeof(copy))) {
                    float* zero = g_ragdoll_zero;
                    std::memcpy(copy + 8, &zero, sizeof(zero));
                    InterlockedIncrement(&g_ragdoll_count);
                    g_ragdoll_event(owner, copy);
                    return;
                }
            }
            // Bosses whose death fades them with their own event 193
            // (Malenia c2120, Placidusax c4750, Godrick c4520; log 13-11):
            // the fade is skipped and the body keeps the animation's last pose.
            if (g_keep_bodies_on && model != 0 && keep_dead_body(chr)) {
                // The animation is not frozen (animation_speed = 0 in r158-test7
                // to test9): it stopped death scenes such as Godfrey's halfway,
                // with their effect; the small shift at the end is better.
                InterlockedIncrement(&g_bodies_kept);
                return;
            }
        }
    }
    g_opacity_original(owner, event);
}

void install_ragdoll_hook() noexcept {
    std::uint8_t* site = find_unique(parse_pattern(kOpacityEventPattern));
    std::uint8_t* ragdoll = find_unique(parse_pattern(kRagdollEventPattern));
    if (!site || !ragdoll) {
        log(L"Vanilla sites 'opacity event' / 'ragdoll event' not found (or ambiguous); Ragdoll on death is unavailable.");
        return;
    }
    constexpr std::size_t kStolen = 14;  // mov [rsp+20h],rsi; push rdi; sub rsp,40h; mov rax,[rdx+20h]
    auto* code = static_cast<std::uint8_t*>(VirtualAlloc(
        nullptr, 4096, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
    if (!code) return;
    std::memcpy(code, site, kStolen);                                       // original prologue
    const std::uint8_t jump_back[6]{0xFF, 0x25, 0x00, 0x00, 0x00, 0x00};   // jmp [rip+0]
    std::memcpy(code + kStolen, jump_back, sizeof(jump_back));
    const std::uint64_t resume = reinterpret_cast<std::uint64_t>(site + kStolen);
    std::memcpy(code + kStolen + 6, &resume, 8);
    FlushInstructionCache(GetCurrentProcess(), code, 64);
    g_opacity_original = reinterpret_cast<TaeEventHandler>(code);
    g_ragdoll_event = reinterpret_cast<TaeEventHandler>(ragdoll);

    std::uint8_t jump[kStolen]{0xFF, 0x25, 0x00, 0x00, 0x00, 0x00};
    const auto detour = reinterpret_cast<std::uint64_t>(&ragdoll_opacity_event);
    std::memcpy(jump + 6, &detour, 8);
    if (!write_code(site, jump, sizeof(jump))) {
        g_ragdoll_event = nullptr;
        log(L"Vanilla opacity event hook could not be written; Ragdoll on death is unavailable.");
        return;
    }
    wchar_t message[160]{};
    swprintf_s(message, L"Vanilla opacity event hook installed at eldenring.exe+0x%llX (ragdoll event +0x%llX).",
        static_cast<unsigned long long>(site - reinterpret_cast<std::uint8_t*>(game::image())),
        static_cast<unsigned long long>(ragdoll - reinterpret_cast<std::uint8_t*>(game::image())));
    log(message);
}

// Giving items (r157 item spawner). MapItemMan::ItemGib (0x5618B0 in 2.7.2.0)
// puts up to 10 items in the player's inventory, the same way picking one up
// does: rcx = MapItemMan ([0x3D6EAE0]), rdx = list {u32 count; entries of
// 16 bytes: item id (category << 28 | param id), quantity, -1, Ash of War id
// or -1}, r8 = {-1, 0, 0, 0}, r9d = 0. _RequestRegistGetItem calls it the same
// way. Categories: 0 weapon, 1 armour, 2 talisman, 4 goods, 8 Ash of War.
using ItemGibFn = void(__fastcall*)(void*, void*, void*, int);
struct ItemGibEntry {
    std::uint32_t id;
    std::uint32_t quantity;
    std::int32_t relay;
    std::int32_t gem;
};
struct ItemGibList {
    std::uint32_t count;
    ItemGibEntry entries[10];
};
ItemGibFn g_item_gib{};
void** g_map_item_man{};

constexpr const char* kItemGibPattern =
    "40 55 56 57 41 54 41 55 41 56 41 57 48 8d ac 24 ? ? ? ? 48 81 ec ? ? ? ? 48 c7 45 ? fe ff ff ff "
    "48 89 9c 24 ? ? ? ? 48 8b 05 ? ? ? ? 48 33 c4 48 89 85 ? ? ? ? 44 89 4c 24 ? 4d 8b f8 4c 89 44 24 ? "
    "4c 8b e9 33 ff 89 7c 24 ? 8b 02 83 f8 0a";
// new MapItemManImpl (0x1C0 bytes), then mov [MapItemMan],rax
constexpr const char* kMapItemManPattern =
    "ba 08 00 00 00 b9 c0 01 00 00 e8 ? ? ? ? 48 89 45 ? 48 85 c0 74 0a 48 8b c8 e8 ? ? ? ? eb 03 49 8b c6 48 89 05";

// While one of our shops is open, the game's GetSellValue (0x68BA00 in
// 2.7.2.0) returns 0, as Glorious Merchant (MIT) does: the shop hides a row
// that costs less than the item sells for, so with it any price shows,
// free included, and no param changes. Pattern from that mod (-29 to the
// entry); 15 bytes stolen: mov [rsp+8],rbx; mov [rsp+10h],rsi; mov [rsp+18h],rdi.
volatile std::uint32_t* g_shop_sell_zero{};

void install_sell_value_hook() noexcept {
    std::uint8_t* hit = find_unique(parse_pattern("83 cb ff 41 8b c0 c1 e8 1c 48 8b f1 83 f8 0f"));
    if (!hit) {
        log(L"Vanilla site 'sell value' not found (or ambiguous); item shop prices stay at the sell value.");
        return;
    }
    std::uint8_t* site = hit - 29;
    static constexpr std::uint8_t kPrologue[]{0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x74, 0x24, 0x10, 0x48, 0x89, 0x7C, 0x24, 0x18};
    if (std::memcmp(site, kPrologue, sizeof(kPrologue)) != 0) {
        log(L"Item shop: GetSellValue is not as expected (another mod?); prices stay at the sell value.");
        return;
    }
    auto* code = static_cast<std::uint8_t*>(VirtualAlloc(nullptr, 4096, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
    if (!code) return;
    auto* flag = reinterpret_cast<volatile std::uint32_t*>(code + 256);
    *flag = 0;
    std::uint8_t* q = code;
    const auto put = [&](std::initializer_list<std::uint8_t> bytes) { for (auto b : bytes) *q++ = b; };
    const auto rel = [&](const volatile void* data, std::size_t tail) {
        const auto next = reinterpret_cast<std::intptr_t>(q) + 4 + tail;
        const auto disp = static_cast<std::int32_t>(reinterpret_cast<std::intptr_t>(data) - next);
        std::memcpy(q, &disp, 4); q += 4;
    };
    put({0x83, 0x3D}); rel(flag, 1); put({0x00});  // cmp dword [flag],0
    put({0x74, 0x03});                             // je original
    put({0x31, 0xC0});                             // xor eax,eax
    put({0xC3});                                   // ret
    std::memcpy(q, site, sizeof(kPrologue)); q += sizeof(kPrologue);
    put({0xFF, 0x25, 0x00, 0x00, 0x00, 0x00});
    const std::uint64_t resume = reinterpret_cast<std::uint64_t>(site + sizeof(kPrologue));
    std::memcpy(q, &resume, 8);
    FlushInstructionCache(GetCurrentProcess(), code, 128);
    std::uint8_t jump[15]{0xFF, 0x25, 0x00, 0x00, 0x00, 0x00};
    const auto target = reinterpret_cast<std::uint64_t>(code);
    std::memcpy(jump + 6, &target, 8);
    jump[14] = 0x90;
    if (!write_code(site, jump, sizeof(jump))) {
        log(L"Item shop: GetSellValue hook could not be written; prices stay at the sell value.");
        return;
    }
    g_shop_sell_zero = flag;
    wchar_t message[100]{};
    swprintf_s(message, L"Vanilla sell value hook installed at eldenring.exe+0x%llX.",
        static_cast<unsigned long long>(site - reinterpret_cast<std::uint8_t*>(game::image())));
    log(message);
}

void resolve_item_gib() noexcept {
    std::uint8_t* gib = find_unique(parse_pattern(kItemGibPattern));
    std::uint8_t* man = find_unique(parse_pattern(kMapItemManPattern));
    if (!gib || !man) {
        log(L"Vanilla site 'item gib' not found (or ambiguous); giving items is unavailable.");
        return;
    }
    std::uint8_t* store = man + 37;  // mov [rip+disp32],rax
    std::int32_t disp{};
    std::memcpy(&disp, store + 3, 4);
    g_map_item_man = reinterpret_cast<void**>(store + 7 + disp);
    g_item_gib = reinterpret_cast<ItemGibFn>(gib);
    wchar_t message[160]{};
    const auto base = reinterpret_cast<std::uint8_t*>(game::image());
    swprintf_s(message, L"Vanilla item gib found at eldenring.exe+0x%llX (MapItemMan at +0x%llX).",
        static_cast<unsigned long long>(gib - base),
        static_cast<unsigned long long>(reinterpret_cast<std::uint8_t*>(g_map_item_man) - base));
    log(message);
}

bool give_items(const ItemGibEntry* items, std::uint32_t count) noexcept {
    if (!g_item_gib || !g_map_item_man || count == 0 || count > 10) return false;
    void* man = *g_map_item_man;
    if (!man) return false;
    ItemGibList list{};
    list.count = count;
    for (std::uint32_t i = 0; i < count; ++i) list.entries[i] = items[i];
    std::int32_t unk[4]{-1, 0, 0, 0};
    __try {
        g_item_gib(man, &list, unk, 0);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        log(L"Item gib raised an exception.");
        return false;
    }
    return true;
}

// No lock-on range hooks. The player's lock-on update (0x717560 in 2.7.2.0,
// 0x7170B0 in 2.7.0.0/2.7.1.x) and target search (0x716F30) compute a target's
// reach as its lock distance plus the lock manager's reach, then compare
// squared distances. Each hook runs a few whole instructions of that sum and
// then, while the option is on, zeroes the register that holds the reach:
// nothing can be locked and a held lock drops. Same bytes in 2.7.0.0, 2.7.1.0,
// 2.7.1.1 JP and 2.7.2.0.
// Lock-on distance multiplies that register first. It holds the reach itself
// in acquire and search, the squared reach in keep. NpcParam lockDist alone did
// nothing to see in game: characters copy it when they are created.
struct LockRangeData {
    volatile std::uint32_t on;
    volatile float scale;
    volatile float scale_squared;
};

LockRangeData* g_lock_range{};

struct LockRangeSite {
    const char* name;
    const char* pattern;
    std::size_t offset;    // first stolen instruction
    std::size_t stolen;    // whole instructions, at least 14 bytes
    std::uint8_t zero[4];  // xorps reg, reg
    std::size_t zero_size;
    std::uint8_t multiply[4];  // mulss reg, [rip+disp32], without the disp32
    std::size_t multiply_size;
    bool squared;              // the register holds the squared reach
};

constexpr LockRangeSite kLockRangeSites[]{
    // movzx eax,[r14+74h]; movd xmm9,eax; cvtdq2ps xmm9,xmm9; addss xmm9,[rsi+2924h];
    // stolen: and byte [rbx+90h],6Fh; xor dil,dil; movups xmm15,[r14]
    {"acquire", "41 0f b6 46 74 66 44 0f 6e c8 45 0f 5b c9 f3 44 0f 58 8e 24 29 00 00 "
        "80 a3 90 00 00 00 6f 40 32 ff 45 0f 10 3e", 0x17, 14, {0x45, 0x0F, 0x57, 0xC9}, 4,
        {0xF3, 0x44, 0x0F, 0x59}, 4, false},
    // movups xmm4,[rsi+2930h]; addss xmm4,[rsi+2924h]; ...; stolen: addss xmm4,xmm0;
    // mulss xmm4,xmm4; movups xmm3,[rdi]; movaps xmm0,xmm7 (xmm4 = reach squared)
    {"keep", "0f 10 a6 30 29 00 00 f3 0f 58 a6 24 29 00 00 0f b6 47 74 66 0f 6e c0 0f 5b c0 "
        "f3 0f 58 e0 f3 0f 59 e4 0f 10 1f 0f 28 c7", 0x1A, 14, {0x0F, 0x57, 0xE4}, 3,
        {0xF3, 0x0F, 0x59}, 3, true},
    // stolen: cvtdq2ps xmm12,xmm12; addss xmm0,xmm2; addss xmm12,[rdi+2924h]
    {"search", "45 0f 5b e4 f3 0f 58 c2 f3 44 0f 58 a7 24 29 00 00 e8", 0x0, 17,
        {0x45, 0x0F, 0x57, 0xE4}, 4, {0xF3, 0x44, 0x0F, 0x59}, 4, false},
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
    data->scale = 1.0f;
    data->scale_squared = 1.0f;
    std::uint8_t* p = code;
    const auto put = [&](std::initializer_list<std::uint8_t> bytes) {
        for (auto b : bytes) *p++ = b;
    };
    std::uint8_t* trampolines[std::size(kLockRangeSites)]{};
    for (std::size_t i = 0; i < std::size(kLockRangeSites); ++i) {
        const LockRangeSite& site = kLockRangeSites[i];
        trampolines[i] = p;
        std::memcpy(p, sites[i], site.stolen); p += site.stolen;     // original instructions
        std::memcpy(p, site.multiply, site.multiply_size); p += site.multiply_size;  // mulss reach,[scale]
        // ModRM rip-relative: reg field of xmm4 / xmm9 / xmm12.
        const std::uint8_t reg = i == 0 ? 1 : 4;
        *p++ = static_cast<std::uint8_t>((reg << 3) | 5);
        {
            const auto after = reinterpret_cast<std::intptr_t>(p) + 4;
            const auto* factor = site.squared ? &data->scale_squared : &data->scale;
            const auto factor_disp = static_cast<std::int32_t>(reinterpret_cast<std::intptr_t>(factor) - after);
            std::memcpy(p, &factor_disp, 4); p += 4;
        }
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

// Fork export (r140): turns the coloured labels on or off live; the defaults
// stay registered. An older ERNativeUI.dll lacks it: the option then only
// applies when the game starts.
using SetHighlightEnabledFn = ERUI_Result (ERUI_CALL*)(std::uint32_t);
SetHighlightEnabledFn g_set_highlight{};
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
                // The trampoline reads [base+10F0h] and [base+18h] after the
                // original read and through rax, so the base register can be
                // neither the destination (already overwritten) nor rax.
                const unsigned has_rex = (rex & 0xF0) == 0x40 ? rex : 0u;
                const unsigned dest = ((modrm >> 3) & 7) | ((has_rex & 0x04) ? 8u : 0u);
                const unsigned base_reg = (modrm & 7) | ((has_rex & 0x01) ? 8u : 0u);
                if (base_reg == dest || base_reg == 0) continue;
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

#ifdef NT_DEV_COMBAT_TRACE
// Development builds only (Craft in combat research, 14-11): every place that
// asks the game's "in combat" check (CSSound+0x328 state; 0xDAA5E0 in
// 2.7.2.0) is logged once per answer, with its return address. Opening the
// pause menu in combat then shows which caller greys out Item Crafting.
using InCombatFn = bool(__fastcall*)(void* sound);
InCombatFn g_in_combat_original{};
SRWLOCK g_combat_trace_lock = SRWLOCK_INIT;
std::uintptr_t g_combat_trace_seen[256]{};
std::size_t g_combat_trace_count{};

bool __fastcall in_combat_trace(void* sound) noexcept {
    const bool result = g_in_combat_original(sound);
    const auto image = reinterpret_cast<std::uintptr_t>(game::image());
    const auto caller = reinterpret_cast<std::uintptr_t>(_ReturnAddress());
    const std::uintptr_t key = ((caller - image) << 1) | (result ? 1 : 0);
    bool fresh = true;
    AcquireSRWLockExclusive(&g_combat_trace_lock);
    for (std::size_t i = 0; i < g_combat_trace_count; ++i)
        if (g_combat_trace_seen[i] == key) { fresh = false; break; }
    if (fresh && g_combat_trace_count < std::size(g_combat_trace_seen)) g_combat_trace_seen[g_combat_trace_count++] = key;
    else fresh = false;
    ReleaseSRWLockExclusive(&g_combat_trace_lock);
    if (fresh) {
        void* frames[4]{};
        const USHORT count = RtlCaptureStackBackTrace(1, 4, frames, nullptr);
        wchar_t message[200]{};
        int used = swprintf_s(message, L"Combat trace: in combat = %d, asked from +0x%llX, via", result ? 1 : 0,
            static_cast<unsigned long long>(caller - image));
        for (USHORT i = 1; i < count && used > 0 && used < 160; ++i) {
            const auto a = reinterpret_cast<std::uintptr_t>(frames[i]);
            const int wrote = a >= image && a < image + 0x6000000
                ? swprintf_s(message + used, std::size(message) - used, L" +0x%llX", static_cast<unsigned long long>(a - image))
                : swprintf_s(message + used, std::size(message) - used, L" ?");
            if (wrote <= 0) break;
            used += wrote;
        }
        log(message);
    }
    return result;
}

void install_combat_trace() noexcept {
    std::uint8_t* site = find_unique(parse_pattern("48 8b 89 28 03 00 00 48 85 c9 0f 85 ? ? ? ? 32 c0 c3"));
    if (!site) {
        log(L"Combat trace: the in-combat check was not found.");
        return;
    }
    std::uint8_t* code = alloc_near(site);
    if (!code) return;
    // trampoline: mov rcx,[rcx+328h] (7 bytes, no RIP operand), then back
    std::memcpy(code, site, 7);
    const std::uint8_t jump_abs[6]{0xFF, 0x25, 0x00, 0x00, 0x00, 0x00};
    std::memcpy(code + 7, jump_abs, 6);
    const std::uint64_t resume = reinterpret_cast<std::uint64_t>(site + 7);
    std::memcpy(code + 13, &resume, 8);
    // near stub the 5-byte jump lands on: jmp [rip+0] -> in_combat_trace
    std::memcpy(code + 64, jump_abs, 6);
    const auto detour = reinterpret_cast<std::uint64_t>(&in_combat_trace);
    std::memcpy(code + 70, &detour, 8);
    FlushInstructionCache(GetCurrentProcess(), code, 128);
    g_in_combat_original = reinterpret_cast<InCombatFn>(code);
    log(write_jump(site, code + 64) ? L"Combat trace: on (development build)." : L"Combat trace: could not hook.");
}
#endif

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
    -60, "74 ? 48 8b 05", {0x90, 0x90}};

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
    ash_no_rest, ash_anywhere, ash_multiple, ash_keep_after_boss, ash_dismiss_all, ash_extra_cost,
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
    // Interface: the former optional files, now [General] switches also in the menu
    extended_ranges, highlight_changed,
    // Spirits: the same ash again with its item instead of dismissing it
    ash_repeat,
    // Spirits: where ashes fight freely, without warps (MSA area follow)
    ash_free_fighting,
    // Damage taken: longer Deflecting Hardtear perfect guard (Nexus request)
    deflect_window,
    // Image: spirit ashes without their ghostly look (Nexus request)
    solid_ashes,
    // Buffs: self buffs of the same kind stack instead of replacing each other (Nexus request)
    stack_buffs,
    // Enemies: kills before an enemy stays dead, like Dark Souls II (Nexus request)
    enemy_respawn_kills,
    // Image: no ghostly look on any character (Evergaol bosses, summoned NPCs; Nexus request)
    solid_ghosts,
    // Image: only your character stays clean (Nexus request)
    player_clean,
    // Map & Travel: Melina's level-up screen anywhere (Melina companion, phase 1)
    open_level_up,
    // Spirit Ashes: Melina as a spirit companion (Melina companion, phase 2)
    summon_melina,
    melina_companion,
    // Buffs: weapon buffs of each hand stack instead of replacing each other (Nexus request)
    stack_weapon_buffs,
    // Speed: sprinting on top of Movement speed (Nexus request)
    speed_sprint,
    // Speed: walking and running on top of Movement speed; levers, doors and
    // chests on top of Global speed (Nexus requests)
    speed_walk,
    speed_run,
    speed_interact,
    // Spells: how far the player's projectiles fly; Camera: lock-on reach (Nexus request)
    projectile_range,
    lock_on_distance,
    // Image: the sharpening pass after TempAA (Nexus request)
    sharpen_off,
    // Enemies: Bewitching Branch on every enemy, and on bosses (Nexus request)
    charm_all_enemies,
    charm_bosses,
    // Speed: the chest or door follows Interaction speed (Nexus request)
    interaction_objects,
    // Poise: the poise change of New Game+ 2 and later (Nexus request)
    ng_poise,
    // Camera: no camera shake (or controller rumble) from attacks and events (Nexus request)
    camera_shake_off,
    // Equipment: drop or discard any item, key items optional (Nexus request)
    drop_any,
    drop_key_items,
    // Item shop (F11): upgrade level of weapons and spirit ashes, price
    shop_weapon_level, shop_spirit_level, shop_item_price,
    // Enemies: NPC Spawner built in (r158), started at game start when on
    npc_spawner,
    // Image: human characters fall limp and stay instead of fading at death
    ragdoll_death,
    // Image: bosses' bodies stay instead of fading or dissolving after death
    keep_bodies,
    // Movement & Speed: weapon skills (Ashes of War) apart from attacks (Nexus request)
    speed_skill,
    // Survival: the Deathbed Dress healing aura without wearing it (Nexus request)
    healing_aura,
    // Runes, Shops & Upgrades: more crafting materials from each pick-up (Nexus request)
    materials_found,
    // Runes, Shops & Upgrades: the crafting menu opens in combat too (Nexus request)
    craft_in_combat,
    // Map & Travel: the map opens and stays open in combat
    map_in_combat,
    // Enemies: how far enemies chase you before they go home (Nexus request)
    enemy_chase,
    // Spirit Ashes: most regular / renowned ashes at once (Nexus request, PoisonParadise)
    ash_max_regular, ash_max_renowned,
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
    bool general = false;  // stored in [General]: a menu preference, not in profiles or resets
};

constexpr Option toggle(const wchar_t* key) { return {key, Kind::toggle, 0, 0, 1, 1, 0, L"", ERUI_INVALID_ROW}; }
constexpr Option toggle_on(const wchar_t* key) { return {key, Kind::toggle, 1, 0, 1, 1, 1, L"", ERUI_INVALID_ROW}; }
// A [General] switch read when the game starts (it changes how the menu is built).
constexpr Option general_toggle(const wchar_t* key) {
    Option option = toggle(key);
    option.general = true;
    return option;
}
constexpr Option slider(const wchar_t* key, std::int16_t fallback, std::int16_t minimum,
    std::int16_t maximum, std::int16_t step, const wchar_t* suffix = L"%") {
    return {key, Kind::slider, fallback, minimum, maximum, step, fallback, suffix, ERUI_INVALID_ROW};
}

Option g_options[count]{
    toggle(L"FovEnabled"), slider(L"FovPercent", 100, 25, 1300, 5),
    toggle(L"DisableCameraAutoRotate"), toggle(L"DisableCameraReset"),
    slider(L"CameraDistancePercent", 100, 25, 1300, 5),
    toggle(L"RemoveVignette"), toggle(L"RemoveChromaticAberration"), toggle(L"FullAnimationDistance"),
    toggle(L"DisableAntiAliasing"), toggle(L"DisableMotionBlur"), toggle(L"DisableDepthOfField"),
    toggle(L"GodMode"), toggle(L"NoDeath"), toggle(L"InfiniteStamina"), toggle(L"InfiniteFp"),
    toggle(L"InfiniteItems"), toggle(L"InfiniteArrows"),
    slider(L"EquipWeightPercent", 100, 0, 1275, 5),
    // Sliders with a scale selector use all 256 positions in steps of 5.
    slider(L"PlayerPoisePercent", 100, 0, 1275, 5), toggle(L"PlayerInfinitePoise"),
    toggle(L"NoFallDamage"), toggle(L"StatusImmunity"),
    slider(L"DamageDealtPercent", 100, 10, 1285, 5), slider(L"DamageTakenPercent", 100, 0, 1275, 5),
    slider(L"PostureDamageDealtPercent", 100, 0, 1275, 5), slider(L"PostureDamageTakenPercent", 100, 0, 1275, 5),
    slider(L"StaminaCostPercent", 100, 0, 1275, 5), slider(L"SpellFpCostPercent", 100, 0, 1275, 5),
    slider(L"StatusBuildupPercent", 100, 0, 1275, 5), toggle(L"OneHitKill"),
    slider(L"BuffDurationWeaponPercent", 100, 25, 1300, 5), slider(L"BuffDurationSelfPercent", 100, 25, 1300, 5),
    slider(L"BuffDurationEnemyPercent", 100, 25, 1300, 5),
    slider(L"SpeedGlobalPercent", 100, 25, 1300, 5), slider(L"SpeedAttackPercent", 100, 25, 1300, 5),
    slider(L"SpeedSpellPercent", 100, 25, 1300, 5), slider(L"SpeedRollPercent", 100, 25, 1300, 5),
    slider(L"SpeedMovePercent", 100, 25, 1300, 5), slider(L"SpeedJumpPercent", 100, 25, 1300, 5),
    slider(L"EnemyHpPercent", 100, 10, 1285, 5), slider(L"EnemyPoisePercent", 100, 0, 1275, 5),
    toggle(L"EnemyInfinitePoise"), slider(L"EnemySpeedPercent", 100, 25, 1300, 5),
    toggle(L"EnemiesNoAttack"),
    toggle(L"EnemiesNoMove"), toggle(L"EnemiesNoAi"), toggle(L"TorrentNoDeath"),
    slider(L"TorrentSpeedPercent", 100, 25, 1300, 5),
    toggle(L"DisableRuneLoss"), slider(L"RunesGainedPercent", 100, 0, 1275, 5),
    slider(L"LootDropPercent", 100, 0, 1275, 5),
    slider(L"SpiritAshCostPercent", 100, 0, 1275, 5), slider(L"UpgradeCostPercent", 100, 0, 1275, 5),
    toggle(L"PauseKey"),
    toggle(L"SetTimeOfDay"), slider(L"TimeOfDayHour", 12, 0, 23, 1, L":00"), toggle(L"FreezeTime"),
    toggle(L"SetWeather"), slider(L"WeatherType", 4, 0, 15, 1, L""),
    toggle(L"ShowAllGraces"), toggle(L"RevealMap"), toggle(L"FastTravelAnywhere"),
    // On by default (the user's decision of 24-10, confirmed 21-11; r159-test4
    // had turned them off after Nexus comments, not because of a bug).
    toggle_on(L"SpiritResummonWithoutRest"), toggle_on(L"SpiritSummonFarFromStones"),
    toggle_on(L"SpiritMultipleAshes"),
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
    slider(L"SpeedItemPercent", 100, 25, 1300, 5),
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
    slider(L"TorrentJumpSpeedPercent", 100, 25, 1300, 5),
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
    slider(L"BossHpPercent", 100, 10, 1285, 5),
    toggle(L"KeepCharactersClean"), toggle(L"HideBuffEffects"),
    toggle(L"HideWeaponBuffEffects"),
    slider(L"DodgeFramesPercent", 100, 100, 500, 10),
    slider(L"ParryWindowPercent", 100, 100, 500, 10),
    toggle(L"KnockdownInvincible"), toggle(L"ItemSuperArmor"),
    general_toggle(L"ExtendedRanges"), general_toggle(L"HighlightChangedOptions"),
    // 0 dismisses the ash, 1 summons it again (the old toggle's on), 2 leaves
    // the item to the game: Reforged enrages a summoned ash with it (Nexus,
    // JDdeen / skar450). Same INI key, so saved 0/1 keep their meaning.
    slider(L"SpiritRepeatSameAsh", 0, 0, 2, 1, L""),
    slider(L"SpiritFreeFighting", 0, 0, 2, 1, L""),  // off by default (user, 08-11)
    slider(L"DeflectWindowPercent", 100, 100, 500, 25),
    toggle(L"SolidSpiritAshes"),
    toggle(L"StackSelfBuffs"),
    slider(L"EnemyRespawnKills", 1, 1, 50, 1),
    toggle(L"SolidGhostCharacters"),
    toggle(L"KeepPlayerClean"),
    toggle(L"OpenLevelUp"),
    toggle(L"SummonMelina"),
    toggle(L"MelinaCompanion"),
    toggle(L"StackWeaponBuffs"),
    slider(L"SpeedSprintPercent", 100, 25, 1300, 5),
    slider(L"SpeedWalkPercent", 100, 25, 1300, 5),
    slider(L"SpeedRunPercent", 100, 25, 1300, 5),
    slider(L"SpeedInteractPercent", 100, 25, 1300, 5),
    slider(L"ProjectileRangePercent", 100, 100, 500, 5),
    slider(L"LockOnDistancePercent", 100, 100, 500, 5),
    toggle(L"RemoveSharpening"),
    toggle(L"CharmAllEnemies"),
    toggle(L"CharmBosses"),
    toggle(L"InteractionObjects"),
    slider(L"NgPoisePercent", 100, 0, 100, 5),
    toggle(L"DisableCameraShake"),
    toggle(L"DropAnyItem"),
    toggle(L"DropKeyItems"),
    slider(L"ItemShopWeaponLevel", 0, 0, 25, 1),
    slider(L"ItemShopSpiritLevel", 0, 0, 10, 1),
    slider(L"ItemShopPricePercent", 100, 0, 500, 25),
    toggle(L"NpcSpawner"),  // off by default; read once at startup (start_npc_spawner)
    toggle(L"RagdollOnDeath"),
    toggle(L"KeepBodies"),
    slider(L"SpeedSkillPercent", 100, 25, 1300, 5),
    slider(L"HealingAuraHp", 0, 0, 250, 1, L"/s"),  // at most 256 positions (r158-test22: 501 broke the menu)
    slider(L"MaterialsFoundPercent", 100, 100, 1000, 25),
    toggle(L"CraftInCombat"),
    toggle(L"MapInCombat"),
    slider(L"EnemyChaseDistancePercent", 100, 100, 1000, 25),
    slider(L"SpiritMaxRegularAshes", 0, 0, 10, 1, L""), slider(L"SpiritMaxRenownedAshes", 0, 0, 10, 1, L""),
};

std::int16_t clamp_value(const Option& option, int value) noexcept {
    if (option.kind == Kind::toggle) return value ? 1 : 0;
    value = std::clamp(value, static_cast<int>(option.minimum), static_cast<int>(option.maximum));
    value = option.minimum + (value - option.minimum) / option.step * option.step;
    return static_cast<std::int16_t>(value);
}

// Optional extended ranges ([General] ExtendedRanges = 1, or the optional
// package's marker NativeTweaks.ExtendedRanges.ini beside NativeTweaks.ini):
// the sliders below use all 256 positions and get a scale row that multiplies
// their value. Without it they keep the classic range and step listed here.
constexpr int kScaleFactors[]{1, 2, 5, 10, 100};
constexpr std::size_t kScaleCount = std::size(kScaleFactors);
bool g_extended{};

struct Scale {
    Id id;
    const wchar_t* key;
    std::int16_t classic_minimum;
    std::int16_t classic_maximum;
    std::int16_t classic_step;
    std::uint8_t index;
    erui::RowHandle row;
};

Scale g_scales[]{
    {player_poise, L"PlayerPoiseScale", 0, 500, 10, 0, ERUI_INVALID_ROW},
    {damage_dealt, L"DamageDealtScale", 10, 1000, 10, 0, ERUI_INVALID_ROW},
    {damage_taken, L"DamageTakenScale", 0, 1000, 10, 0, ERUI_INVALID_ROW},
    {posture_dealt, L"PostureDamageDealtScale", 0, 1000, 10, 0, ERUI_INVALID_ROW},
    {posture_taken, L"PostureDamageTakenScale", 0, 1000, 10, 0, ERUI_INVALID_ROW},
    {status_buildup, L"StatusBuildupScale", 0, 1000, 10, 0, ERUI_INVALID_ROW},
    {enemy_hp, L"EnemyHpScale", 10, 1000, 10, 0, ERUI_INVALID_ROW},
    {boss_hp, L"BossHpScale", 10, 2000, 10, 0, ERUI_INVALID_ROW},
    {enemy_poise, L"EnemyPoiseScale", 0, 500, 10, 0, ERUI_INVALID_ROW},
    {runes_gained, L"RunesGainedScale", 0, 1000, 10, 0, ERUI_INVALID_ROW},
    {loot_drop, L"LootDropScale", 0, 1000, 10, 0, ERUI_INVALID_ROW},
    {equip_weight, L"EquipWeightScale", 0, 200, 5, 0, ERUI_INVALID_ROW},
    {stamina_cost, L"StaminaCostScale", 0, 300, 5, 0, ERUI_INVALID_ROW},
    {spell_fp_cost, L"SpellFpCostScale", 0, 300, 5, 0, ERUI_INVALID_ROW},
    {buff_weapon, L"BuffDurationWeaponScale", 25, 500, 25, 0, ERUI_INVALID_ROW},
    {buff_self, L"BuffDurationSelfScale", 25, 500, 25, 0, ERUI_INVALID_ROW},
    {buff_enemy, L"BuffDurationEnemyScale", 25, 500, 25, 0, ERUI_INVALID_ROW},
    {ash_cost, L"SpiritAshCostScale", 0, 300, 10, 0, ERUI_INVALID_ROW},
    {upgrade_cost, L"UpgradeCostScale", 0, 300, 10, 0, ERUI_INVALID_ROW},
    {fov_percent, L"FovScale", 25, 200, 5, 0, ERUI_INVALID_ROW},
    {camera_distance, L"CameraDistanceScale", 25, 500, 5, 0, ERUI_INVALID_ROW},
    {speed_global, L"SpeedGlobalScale", 25, 300, 5, 0, ERUI_INVALID_ROW},
    {speed_attack, L"SpeedAttackScale", 25, 300, 5, 0, ERUI_INVALID_ROW},
    {speed_skill, L"SpeedSkillScale", 25, 300, 5, 0, ERUI_INVALID_ROW},
    {speed_spell, L"SpeedSpellScale", 25, 300, 5, 0, ERUI_INVALID_ROW},
    {speed_roll, L"SpeedRollScale", 25, 300, 5, 0, ERUI_INVALID_ROW},
    {speed_move, L"SpeedMoveScale", 25, 300, 5, 0, ERUI_INVALID_ROW},
    {speed_jump, L"SpeedJumpScale", 25, 300, 5, 0, ERUI_INVALID_ROW},
    {speed_item, L"SpeedItemScale", 25, 300, 5, 0, ERUI_INVALID_ROW},
    {speed_sprint, L"SpeedSprintScale", 25, 300, 5, 0, ERUI_INVALID_ROW},
    {speed_walk, L"SpeedWalkScale", 25, 300, 5, 0, ERUI_INVALID_ROW},
    {speed_run, L"SpeedRunScale", 25, 300, 5, 0, ERUI_INVALID_ROW},
    {speed_interact, L"SpeedInteractScale", 25, 300, 5, 0, ERUI_INVALID_ROW},
    {enemy_speed, L"EnemySpeedScale", 25, 300, 5, 0, ERUI_INVALID_ROW},
    {torrent_speed, L"TorrentSpeedScale", 25, 300, 5, 0, ERUI_INVALID_ROW},
    {torrent_jump_speed, L"TorrentJumpSpeedScale", 25, 300, 5, 0, ERUI_INVALID_ROW},
    // Past 500 % (Nexus request, Mickeekarlsson): the jump hook only scales
    // the flight's root motion, so any factor is safe to apply.
    {jump_height, L"JumpHeightScale", 25, 500, 5, 0, ERUI_INVALID_ROW},
    {jump_distance, L"JumpDistanceScale", 25, 500, 5, 0, ERUI_INVALID_ROW},
    {torrent_jump_height, L"TorrentJumpHeightScale", 25, 500, 5, 0, ERUI_INVALID_ROW},
    {torrent_jump_distance, L"TorrentJumpDistanceScale", 25, 500, 5, 0, ERUI_INVALID_ROW},
};

Scale* scale_of(Id id) noexcept {
    if (!g_extended) return nullptr;
    for (Scale& scale : g_scales)
        if (scale.id == id) return &scale;
    return nullptr;
}
// Effective value: the slider times its scale.
int value_of(Id id) noexcept {
    const int value = g_options[id].value;
    const Scale* scale = scale_of(id);
    return scale ? value * kScaleFactors[scale->index] : value;
}

// ---------------------------------------------------------------------------
// Game state: debug flags, params, speed, regeneration

game::Statics g_statics{};

// Keep bodies (beta; user, 13-11; "Keep boss bodies" in r158-test3/4). Bosses already fall with a ragdoll (event
// 228 is in their own TAE), but their bodies go away by two other paths, both
// blocked here for dead characters only (tested with the RagdollTest
// experiment on a summoned Godrick Soldier, 13-11):
//  1. a timed fade-out, 0x3EE360 in 2.7.2.0 (chr, duration, start opacity,
//     flag): +0x240 opacity = start when 0..1, +0x1C7 bit 7 = flag, +0x244
//     rate = (0 - opacity) / duration. It is short and starts with a
//     relative jump, so it is replaced as a whole (same result when it runs);
//  2. SpEffect 295190 (vfx 57050), applied about 4.4 s after death: it
//     dissolves the body (+0x248) and the game removes it about 10 s later.
//     ChrIns::ApplySpEffect (0x3E8DC0: chr, id, dont_sync; fromsoftware-rs
//     chr_ins_apply_speffect) does not apply it.
//  3. the fade-to-0 event 193 of bosses whose death animation fades them
//     (Malenia, Placidusax, Godrick): skipped in ragdoll_opacity_event, so
//     that hook serves both options.
// You, spirit ashes and allies summoned with you (team 47) and ghostly
// characters (phantom types or a ghost look) keep fading as in the game.
// Both functions are unique in 2.7.0.0 to 2.7.2.1. The hooks go in the first
// time the option is turned on.
constexpr const char* kTimedFadePattern =
    "0f 57 db 0f 2f d3 72 15 f3 0f 10 05 ? ? ? ? 0f 2f c2 72 08 f3 0f 11 91 40 02 00 00 "
    "f3 0f 5c 99 40 02 00 00 80 a1 c7 01 00 00 7f 41 c0 e1 07 44 08 89 c7 01 00 00 "
    "f3 0f 5e d9 f3 0f 11 99 44 02 00 00 c3";
constexpr const char* kApplySpEffectPattern =
    "48 8b c4 48 89 58 08 48 89 70 10 57 48 81 ec c0 00 00 00 0f 28 05 ? ? ? ? 48 8b f1 0f 28 0d ? ? ? ? "
    "48 8d 48 88 0f 29 40 a8 41 0f b6 d8 0f 29 48 b8 0f 57 c0 0f 28 0d ? ? ? ? 8b fa 0f 29 40 c8 "
    "0f 29 48 d8 66 c7 40 e8 00 00 e8";
constexpr std::int32_t kDissolveSpEffect = 295190;
constexpr std::uint8_t kSummonTeam = 47;  // spirit ashes and other allies summoned with you

using ApplySpEffectFn = std::uint64_t(__fastcall*)(void* chr, std::int32_t id, bool dont_sync);
std::uint8_t* g_timed_fade_site{};
std::uint8_t* g_apply_speffect_site{};
ApplySpEffectFn g_apply_speffect_original{};
bool g_keep_bodies_hooked{};

// Models whose bodies keep fading: Torrent (c8000), which comes back when
// summoned again. Two-phase bosses are not listed: in the real fights their
// event removes the first phase (Rykard, Godfrey, Maliketh, Malenia; 13-11);
// only a summoned Malenia, with no event, leaves her first phase's armour.
constexpr std::int32_t kKeepFadingModels[]{8000};

// Characters seen alive (HP > 0) since Keep bodies was turned on. Only their
// bodies are kept: a boss phase that waits with 0 HP until its event brings
// it in (Malenia's second phase, in the flower) is not dead, and skipping its
// fade-to-0 left it as a column of petals (user, 13-11). Filled from the
// SpEffect hook, which sees every fighting character, and from these checks.
// Each one keeps the parts of its model it showed alive and, once kept, when
// it died: 6 s later (its fall over) bodies_worker freezes it and puts those
// parts back, as NPC Spawner does with its own (tested there with Malenia,
// 13-11): death animations hide parts of the model (Malenia's second phase
// kept only its flowers, a first phase only its armour), and an unfrozen body
// without its place's event plays its death fade again and again.
struct BodyState {
    std::uint32_t alive_mask{};
    bool mask_known{};
    ULONGLONG dead_since{};  // GetTickCount64 when first kept, 0 while alive
    bool settled{};
};
SRWLOCK g_seen_alive_lock = SRWLOCK_INIT;
std::unordered_map<std::uintptr_t, BodyState> g_seen_alive;
volatile long g_bodies_pending{};  // kept bodies not settled yet
volatile long g_bodies_worker{};

// The parts its model shows (bit n = part n; NpcParam modelDispMask at
// creation): ChrIns+0x3C8 and +0x138 of its CSChrModelIns (ChrIns+0x50),
// found by NPC Spawner comparing two Malenia rows (2.7.2.0). Only when both
// hold the same mask.
bool model_mask(std::uintptr_t chr, std::uint32_t& mask) noexcept {
    std::uintptr_t ins{};
    std::uint32_t a{}, b{};
    if (!game::read(chr + 0x50, ins) || ins < 0x10000 || (ins & 7) || !game::read(chr + 0x3C8, a) ||
        !game::read(ins + 0x138, b) || a != b)
        return false;
    mask = a;
    return true;
}

// Puts `mask` back in the four places that hold the model's parts
// ([ModelIns+0x18]+0x160 and [ModelIns+0x130]+0x18 too); a place is only
// written when it holds this model's current or alive mask.
bool restore_model_parts(std::uintptr_t chr, std::uint32_t mask) noexcept {
    std::uintptr_t ins{};
    std::uint32_t current{};
    if (!game::read(chr + 0x50, ins) || ins < 0x10000 || (ins & 7) || !game::read(chr + 0x3C8, current) ||
        current == mask)
        return false;
    std::uintptr_t places[4]{chr + 0x3C8, ins + 0x138, 0, 0};
    std::uintptr_t inner{};
    if (game::read(ins + 0x18, inner) && inner > 0x10000 && !(inner & 7)) places[2] = inner + 0x160;
    if (game::read(ins + 0x130, inner) && inner > 0x10000 && !(inner & 7)) places[3] = inner + 0x18;
    bool wrote{};
    for (const std::uintptr_t place : places) {
        std::uint32_t value{};
        if (!place || !game::read(place, value) || (value != current && value != mask)) continue;
        wrote = game::write<std::uint32_t>(place, mask) || wrote;
    }
    return wrote;
}

void note_alive(std::uintptr_t chr) noexcept {
    std::uint32_t mask{};
    const bool known = model_mask(chr, mask);
    // Called by the game thread for every SpEffect applied: the common case
    // (already known alive, same parts) only takes the lock shared.
    AcquireSRWLockShared(&g_seen_alive_lock);
    const auto seen = g_seen_alive.find(chr);
    const bool unchanged = seen != g_seen_alive.end() && !seen->second.dead_since &&
        (!known || (seen->second.mask_known && seen->second.alive_mask == mask));
    ReleaseSRWLockShared(&g_seen_alive_lock);
    if (unchanged) return;
    AcquireSRWLockExclusive(&g_seen_alive_lock);
    try {
        if (g_seen_alive.size() > 8192) {  // addresses are reused; keeps it small
            for (const auto& entry : g_seen_alive)
                if (entry.second.dead_since && !entry.second.settled) InterlockedDecrement(&g_bodies_pending);
            g_seen_alive.clear();
        }
        BodyState& state = g_seen_alive[chr];
        if (state.dead_since && !state.settled) InterlockedDecrement(&g_bodies_pending);
        if (known) {
            state.alive_mask = mask;
            state.mask_known = true;
        }
        state.dead_since = 0;
        state.settled = false;
    } catch (...) {
    }
    ReleaseSRWLockExclusive(&g_seen_alive_lock);
}

bool seen_alive(std::uintptr_t chr) noexcept {
    AcquireSRWLockShared(&g_seen_alive_lock);
    const bool seen = g_seen_alive.count(chr) != 0;
    ReleaseSRWLockShared(&g_seen_alive_lock);
    return seen;
}

void note_kept(std::uintptr_t chr) noexcept {
    AcquireSRWLockExclusive(&g_seen_alive_lock);
    const auto it = g_seen_alive.find(chr);
    if (it != g_seen_alive.end() && !it->second.dead_since) {
        it->second.dead_since = GetTickCount64();
        it->second.settled = false;
        InterlockedIncrement(&g_bodies_pending);
    }
    ReleaseSRWLockExclusive(&g_seen_alive_lock);
}

bool chr_alive(std::uintptr_t chr) noexcept {
    std::uintptr_t modules{}, data{};
    std::int32_t hp{0};
    return chr && game::read(chr + 0x190, modules) && modules && game::read(modules, data) && data &&
        game::read(data + 0x138, hp) && hp > 0;
}

bool keep_dead_body(std::uintptr_t chr) noexcept {
    std::uintptr_t modules{}, data{}, manager{}, player{};
    std::int32_t hp{1}, type{-1}, look{-1}, model{-1};
    std::uint8_t team{};
    if (!chr || !game::read(chr + 0x190, modules) || !modules || !game::read(modules, data) || !data ||
        !game::read(data + 0x138, hp))
        return false;
    if (hp > 0) {
        note_alive(chr);
        return false;
    }
    if (!seen_alive(chr)) return false;
    if (!game::read(chr + 0x64, model) ||
        std::find(std::begin(kKeepFadingModels), std::end(kKeepFadingModels), model) != std::end(kKeepFadingModels))
        return false;
    if (game::read(g_statics.world_chr_man, manager) && manager &&
        game::read(manager + game::kPlayerIns, player) && player == chr)
        return false;
    if (!game::read(chr + 0x6C, team) || team == kSummonTeam) return false;
    // chrType +0x68 (fromsoftware-rs ChrType): 1 white phantom, 2 duelist,
    // 3-4 ghosts, 8 gray, 15-21 invaders, blue phantoms and NPC signs.
    if (!game::read(chr + 0x68, type) || (type >= 1 && type <= 4) || type == 8 || (type >= 15 && type <= 21))
        return false;
    // phantomParamOverride +0x540: a ghost look (999 is Solid ghostly characters').
    if (game::read(chr + 0x540, look) && look > 0) return false;
    note_kept(chr);
    return true;
}

void __fastcall timed_fade(std::uint8_t* chr, float duration, float start, std::uint8_t flag) noexcept {
    if (g_keep_bodies_on && keep_dead_body(reinterpret_cast<std::uintptr_t>(chr))) {
        InterlockedIncrement(&g_bodies_kept);
        return;
    }
    float& opacity = *reinterpret_cast<float*>(chr + 0x240);
    if (start >= 0.0f && start <= 1.0f) opacity = start;
    chr[0x1C7] = static_cast<std::uint8_t>((chr[0x1C7] & 0x7F) | ((flag & 1) << 7));
    *reinterpret_cast<float*>(chr + 0x244) = (0.0f - opacity) / duration;
}

std::uint64_t __fastcall apply_speffect_detour(void* chr, std::int32_t id, bool dont_sync) noexcept {
    if (g_keep_bodies_on) {
        const auto address = reinterpret_cast<std::uintptr_t>(chr);
        if (id == kDissolveSpEffect) {
            if (keep_dead_body(address)) {
                InterlockedIncrement(&g_bodies_kept);
                return 0;
            }
        } else if (chr_alive(address)) {
            note_alive(address);
        }
    }
    return g_apply_speffect_original(chr, id, dont_sync);
}

void resolve_keep_bodies() noexcept {
    g_timed_fade_site = find_unique(parse_pattern(kTimedFadePattern));
    g_apply_speffect_site = find_unique(parse_pattern(kApplySpEffectPattern));
    if (!g_timed_fade_site || !g_apply_speffect_site) {
        g_timed_fade_site = g_apply_speffect_site = nullptr;
        log(L"Vanilla sites 'timed fade' / 'apply SpEffect' not found (or ambiguous); Keep bodies is unavailable.");
    }
}

DWORD WINAPI bodies_worker(void*) noexcept;

bool install_keep_bodies() noexcept {
    if (g_keep_bodies_hooked) return true;
    if (!g_timed_fade_site || !g_apply_speffect_site) return false;
    constexpr std::size_t kStolen = 19;  // mov rax,rsp; mov [rax+8],rbx; mov [rax+10h],rsi; push rdi; sub rsp,0C0h
    auto* code = static_cast<std::uint8_t*>(VirtualAlloc(
        nullptr, 4096, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
    if (!code) return false;
    std::memcpy(code, g_apply_speffect_site, kStolen);
    const std::uint8_t jump_back[6]{0xFF, 0x25, 0x00, 0x00, 0x00, 0x00};  // jmp [rip+0]
    std::memcpy(code + kStolen, jump_back, sizeof(jump_back));
    const std::uint64_t resume = reinterpret_cast<std::uint64_t>(g_apply_speffect_site + kStolen);
    std::memcpy(code + kStolen + 6, &resume, 8);
    FlushInstructionCache(GetCurrentProcess(), code, 64);
    g_apply_speffect_original = reinterpret_cast<ApplySpEffectFn>(code);

    const auto jump_to = [](const void* target) {
        std::array<std::uint8_t, 14> jump{0xFF, 0x25, 0x00, 0x00, 0x00, 0x00};
        const auto address = reinterpret_cast<std::uint64_t>(target);
        std::memcpy(jump.data() + 6, &address, 8);
        return jump;
    };
    const auto fade_jump = jump_to(reinterpret_cast<const void*>(&timed_fade));
    const auto apply_jump = jump_to(reinterpret_cast<const void*>(&apply_speffect_detour));
    if (!write_code(g_timed_fade_site, fade_jump.data(), fade_jump.size())) {
        log(L"Keep bodies: the timed fade could not be replaced.");
        return false;
    }
    if (!write_code(g_apply_speffect_site, apply_jump.data(), apply_jump.size())) {
        log(L"Keep bodies: the SpEffect hook could not be written; only the timed fade is kept off.");
    }
    g_keep_bodies_hooked = true;
    if (!InterlockedExchange(&g_bodies_worker, 1)) {
        if (const HANDLE thread = CreateThread(nullptr, 0, &bodies_worker, nullptr, 0, nullptr)) CloseHandle(thread);
    }
    wchar_t message[160]{};
    swprintf_s(message, L"Keep bodies: timed fade replaced at eldenring.exe+0x%llX, ApplySpEffect hooked at +0x%llX.",
        static_cast<unsigned long long>(g_timed_fade_site - reinterpret_cast<std::uint8_t*>(game::image())),
        static_cast<unsigned long long>(g_apply_speffect_site - reinterpret_cast<std::uint8_t*>(game::image())));
    log(message);
    return true;
}
HANDLE g_param_event{};

// ChrDbgFlags byte offsets.
struct FlagBinding { Id id; std::ptrdiff_t offset; };
constexpr FlagBinding kFlags[]{
    // +0x3 (every hit kills) is no longer used for One-hit kills (r157): it
    // also killed with 0-damage contact, e.g. rolling into an enemy.
    {no_death, 0x1}, {torrent_no_death, 0x2}, {infinite_items, 0x4},
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
    // Damage dealt left AtkParam_Pc in r157: humanoid NPCs and invaders share
    // those rows, so they hit harder too. It is the player's own SpEffect row
    // now (atk*DmgCorrectRate), see sync_damage_cut.
    // Damage taken is no longer an AtkParam_Npc scale (r157): humanoid NPCs and
    // invaders attack with AtkParam_Pc, so it missed them (Nexus report). It is
    // the player's own SpEffect row now, see sync_damage_cut.
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
    std::vector<double> price_floor{};  // shop_price: lowest price on sale, by row
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

// The game stops a character's maximum health at 524287 (2^19 - 1, seen in game)
// but keeps the larger value in max_uncapped_hp, and the health bar is drawn
// against that one: x100 left a boss looking almost dead at full health. Rows
// stop here too, so enemy x boss health can never wrap to a negative value.
constexpr std::int32_t kMaxCharacterHp = 524287;

// Our ShopLineupParam block (prefix + table + index), while it is the live one.
std::uintptr_t g_shop_block_begin{}, g_shop_block_end{};
std::uintptr_t g_shop_table{};  // the table the holder points at
std::uintptr_t g_shop_holder{};

// The item shop's F11 entry (register_item_shop_talk).
bool g_talk_registered = false;
int g_talk_tries = 0;

// Shop prices: the game hides what costs less than it sells for (Nexus report:
// Kale at 10% lost exactly the 8 rows below their sellValue). Runes-only rows
// (costType +0x18 = 0, no mtrlId +0x8) get that price as a floor; alteration
// rows, which need a material, stay on sale below it in the regulation. 0% is
// hidden too (tested), so it gets the floor as well; rows free in the regulation
// (Enia's forging) stay free.
// equipType +0x17 selects the table; weapons in shops can carry an upgrade level.
// Returns false until the tables exist. An empty result means no floors.
bool shop_price_floors(const game::ParamTable& shops, std::vector<double>& floors) noexcept {
    constexpr struct { const wchar_t* name; std::size_t size; std::size_t sell; } kTables[]{
        {L"EquipParamWeapon", 0x298, 0x20}, {L"EquipParamProtector", 0x1A0, 0x20},
        {L"EquipParamAccessory", 0x60, 0x18}, {L"EquipParamGoods", 0xB0, 0x14}, {L"EquipParamGem", 0x60, 0x20}};
    try {
        floors.clear();
        std::unordered_map<std::int32_t, std::int32_t> sell[std::size(kTables)];
        for (std::size_t t = 0; t < std::size(kTables); ++t) {
            game::ParamTable table;
            if (!game::find_param(g_statics, kTables[t].name, kTables[t].size, table, g_log)) return table.file != 0;
            sell[t].reserve(table.rows.size());
            for (std::size_t r = 0; r < table.rows.size(); ++r) {
                std::int32_t id{}, value{};
                if (game::read(table.file + 0x40 + r * 0x18, id) && game::read(table.rows[r] + kTables[t].sell, value))
                    sell[t].emplace(id, value);
            }
        }
        std::vector<double> result(shops.rows.size(), 0.0);
        for (std::size_t r = 0; r < shops.rows.size(); ++r) {
            std::int32_t equip{}, material{};
            std::uint8_t type{0xFF}, cost{0xFF};
            if (!game::read(shops.rows[r], equip) || !game::read(shops.rows[r] + 0x8, material) ||
                !game::read(shops.rows[r] + 0x17, type) || !game::read(shops.rows[r] + 0x18, cost) ||
                cost != 0 || material >= 0 || type >= std::size(kTables))
                continue;
            auto found = sell[type].find(equip);
            if (found == sell[type].end() && type == 0) found = sell[type].find(equip - equip % 100);
            if (found != sell[type].end() && found->second > 0) result[r] = found->second;
        }
        floors = std::move(result);
    } catch (...) {
        floors.clear();
    }
    return true;
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

// Set once the AtkParam SpEffect slots are confirmed (see sync_weapon_effects).
bool g_atk_layout_ok{};

int target_percent(const ParamField& field) noexcept {
    if (field.id == player_poise && value_of(player_poise_lock)) return kLockedPercent;
    if (field.id == enemy_poise && value_of(enemy_poise_lock)) return kLockedPercent;
    if (field.id == posture_dealt && value_of(posture_one_hit)) return kOneHitPosturePercent;
    if (field.id == posture_taken && value_of(posture_one_hit_taken)) return kOneHitPosturePercent;
    if (field.id == status_buildup && value_of(status_instant)) return kInstantStatusPercent;
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
            if (field.id == shop_price) {
                // the item shop prices its own rows (audit 2026-11-08)
                table.rows.erase(std::remove_if(table.rows.begin(), table.rows.end(), [](std::uintptr_t row) {
                    return row >= g_shop_block_begin && row < g_shop_block_end;
                }), table.rows.end());
                if (!shop_price_floors(table, state.price_floor)) continue;
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
            if (field.id == enemy_hp) scaled = std::min<double>(scaled, kMaxCharacterHp);
            if (r < state.price_floor.size() && state.original[r] > 0 && scaled < state.price_floor[r])
                scaled = state.price_floor[r];
            // The game wraps poise damage past 32767 (x100 broke nothing in game), so
            // extended ranges stop at the strongest value that still counts.
            if (field.id == posture_dealt || field.id == posture_taken)
                scaled = std::min(scaled, kMaxPoiseDamage);
            write_field(state.table.rows[r] + field.offset, field.type,
                percent != kLockedPercent ? scaled : locked);
        }
        state.applied_percent = percent;
        state.applied_boss_percent = boss_percent;
        if (field.id == enemy_hp && g_diagnostics) {
            double boss_max{}, written_max{};
            std::size_t bosses{};
            for (std::size_t r = 0; r < state.table.rows.size(); ++r) {
                if (r >= state.boss.size() || !state.boss[r]) continue;
                ++bosses;
                boss_max = std::max(boss_max, state.original[r]);
                written_max = std::max(written_max, read_field(state.table.rows[r] + field.offset, field.type));
            }
            wchar_t message[200]{};
            swprintf_s(message, L"Boss health: enemy %d%%, boss %d%%, %zu rows, largest %.0f -> %.0f.",
                percent, boss_percent, bosses, boss_max, written_max);
            log(message);
        }
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
// weapon = wepParamChange (+0x158) set, except 3 ("Self": what a hit leaves
// on its target, such as Destined Death or a grease's "On Attack" status,
// which stacked forever on enemies with permanent weapon buffs); enemy =
// effectTargetOpposeTarget
// (+0x16C bit 0), unless an item or projectile of the player applies it to
// themselves; self = self or friendly target (+0x16C bits 2/1) that applies
// to the player (+0x15F bit 3).
struct BuffState {
    bool attempted{};
    bool ready{};
    std::vector<std::uintptr_t> rows[3]{};
    std::vector<float> original[3]{};
    int applied_percent[3]{100, 100, 100};
    // Stack self buffs: spCategory (u16 +0x13E) groups effects that replace
    // each other: 151 body (Flame, Grant Me Strength, Barrier of Gold, Boiled
    // Crab...), 160 Golden Vow and aromatics, 161 HP regeneration, 154
    // stealth... Self buffs in 150-199 go to 0 (no group) while it is on.
    std::vector<std::uintptr_t> stack_rows{};
    std::vector<std::uint16_t> stack_original{};
    bool stack_applied{};
    // Stack weapon buffs: weapon buffs in 162 (right hand) and 163 (left hand)
    // go to 0 the same way.
    std::vector<std::uintptr_t> stack_weapon_rows{};
    std::vector<std::uint16_t> stack_weapon_original{};
    bool stack_weapon_applied{};
    // Deflect window: msa\src\deflect.rs carries the perfect guard on.
    int deflect_applied{100};
    // Hide buff effects also covers these: consumables of any length and
    // talismans' effects (Red-Feathered Branchsword, Ritual Sword...).
    std::vector<std::uintptr_t> hide_extra{};
    // Ids of every effect Hide buff effects covers (sorted), for the items
    // that apply them.
    std::vector<std::uint32_t> hidden_ids{};
    std::vector<std::uint32_t> weapon_ids{};  // weapon buffs, sorted
};

BuffState g_buffs{};
constexpr Id kBuffIds[3]{buff_weapon, buff_self, buff_enemy};
constexpr std::uint8_t kWepChangeSelf = 3;
// Self buffs shorter than 5 s that players expect to count (Nexus, r142):
// Endure's poise buff, 3-3.5 s (1650/1655, and 1651/1656 when used without
// FP); its target bits would also file it as an enemy one. The light attacks
// lost in the first test came from Hide weapon buff effects, not from this.
constexpr std::uint32_t kShortSelfBuffs[]{1650, 1651, 1655, 1656};
std::uintptr_t param_row_by_id(const game::ParamTable& table, std::uint32_t id) noexcept;
constexpr std::ptrdiff_t kSpCategory = 0x13E;
// Deflecting Hardtear perfect guard window: 6 frames at 30 per second.
constexpr int kDeflectWindowMs = 200;

extern "C" std::uint8_t nt_set_deflect_window(std::uint32_t extra_ms);

// SpEffect ids named by a param's rows (sorted, for binary search).
std::vector<std::uint32_t> effect_ids(const wchar_t* param, std::size_t row_size,
    std::initializer_list<std::ptrdiff_t> fields) noexcept {
    std::vector<std::uint32_t> ids;
    game::ParamTable table;
    if (!game::find_param(g_statics, param, row_size, table, g_log)) return ids;
    for (const auto row : table.rows)
        for (const auto field : fields) {
            std::int32_t id{};
            if (game::read(row + field, id) && id > 0) ids.push_back(static_cast<std::uint32_t>(id));
        }
    std::sort(ids.begin(), ids.end());
    ids.erase(std::unique(ids.begin(), ids.end()), ids.end());
    return ids;
}

void sync_buffs() noexcept {
    if (g_buffs.attempted) return;
    if (!g_buffs.ready) {
        game::ParamTable table;
        if (!game::find_param(g_statics, L"SpEffectParam", 0x390, table, g_log)) {
            if (table.file) g_buffs.attempted = true;
            return;
        }
        // What the player puts on themselves from items (refId_default) and
        // projectiles (spEffectIDForShooter): tears, cured meats, livers...
        // also carry the enemy target bit, so they were filed as enemy effects
        // (Nexus, r143: their visuals stayed with Hide buff effects).
        const auto own = effect_ids(L"EquipParamGoods", 0xB0, {0x4});
        const auto own_bullets = effect_ids(L"Bullet", 0x110, {0x60});
        // A talisman's own effect is refId (+0x4); residentSpEffectId1-4 are extra.
        const auto talismans = effect_ids(L"EquipParamAccessory", 0x60, {0x4, 0x4C, 0x50, 0x54, 0x58});
        std::size_t moved{};
        for (std::size_t i = 0; i < table.rows.size(); ++i) {
            const auto row = table.rows[i];
            std::uint32_t id{};
            float duration{};
            std::uint8_t weapon{}, targets{}, affiliation{};
            if (!game::read(table.file + 0x40 + i * 0x18, id) || !game::read(row + 0x8, duration)) continue;
            game::read(row + 0x158, weapon);
            game::read(row + 0x16C, targets);
            game::read(row + 0x15F, affiliation);
            const bool for_player = (targets & 0x6) && (affiliation & 0x8);
            // Weapon buffs (greases, wepParamChange 1/2) stay out: clearing
            // their effects' sfx brought back the golden smoke wall.
            const bool own_source = weapon == 0 && (std::binary_search(own.begin(), own.end(), id) ||
                std::binary_search(own_bullets.begin(), own_bullets.end(), id));
            const bool own_buff = for_player && own_source;
            // Some items leave out the player bit (Starlight Shards, Lulling
            // Branch): they still only reach the user, so they are hidden too.
            if ((own_source && (targets & 0x6) && duration > 1.0f && duration <= 999.0f) ||
                std::binary_search(talismans.begin(), talismans.end(), id)) {
                g_buffs.hide_extra.push_back(row);
                g_buffs.hidden_ids.push_back(id);
            }
            if (duration < 5.0f || duration > 999.0f) continue;
            int category = -1;
            if (weapon && weapon != kWepChangeSelf) category = 0;
            else if ((targets & 0x1) && !own_buff) category = 2;
            else if (for_player) category = 1;
            if (own_buff && (targets & 0x1) && category == 1) ++moved;
            if (category == 1) g_buffs.hidden_ids.push_back(id);
            if (category == 0) g_buffs.weapon_ids.push_back(id);
            if (category < 0) continue;
            g_buffs.rows[category].push_back(row);
            g_buffs.original[category].push_back(duration);
        }
        for (const std::uint32_t id : kShortSelfBuffs) {
            const std::uintptr_t row = param_row_by_id(table, id);
            float duration{};
            if (!row || !game::read(row + 0x8, duration) || duration <= 0.0f) continue;
            if (std::find(g_buffs.rows[1].begin(), g_buffs.rows[1].end(), row) != g_buffs.rows[1].end()) continue;
            g_buffs.rows[1].push_back(row);
            g_buffs.original[1].push_back(duration);
        }
        std::sort(g_buffs.hidden_ids.begin(), g_buffs.hidden_ids.end());
        g_buffs.hidden_ids.erase(std::unique(g_buffs.hidden_ids.begin(), g_buffs.hidden_ids.end()),
            g_buffs.hidden_ids.end());
        std::sort(g_buffs.weapon_ids.begin(), g_buffs.weapon_ids.end());
        wchar_t message[240]{};
        swprintf_s(message, L"Effect durations: %zu weapon, %zu self (%zu items/projectiles), %zu enemy SpEffectParam rows; %zu more for Hide buff effects.",
            g_buffs.rows[0].size(), g_buffs.rows[1].size(), moved, g_buffs.rows[2].size(), g_buffs.hide_extra.size());
        log(message);
        for (const auto row : g_buffs.rows[1]) {
            std::uint16_t group{};
            if (!game::read(row + kSpCategory, group) || group < 150 || group > 199) continue;
            g_buffs.stack_rows.push_back(row);
            g_buffs.stack_original.push_back(group);
        }
        swprintf_s(message, L"Stack self buffs: %zu self buffs in replacing groups.", g_buffs.stack_rows.size());
        log(message);
        for (const auto row : g_buffs.rows[0]) {
            std::uint16_t group{};
            if (!game::read(row + kSpCategory, group) || (group != 162 && group != 163)) continue;
            g_buffs.stack_weapon_rows.push_back(row);
            g_buffs.stack_weapon_original.push_back(group);
        }
        swprintf_s(message, L"Stack weapon buffs: %zu weapon buffs in replacing groups.", g_buffs.stack_weapon_rows.size());
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
    const bool stack = value_of(stack_buffs) != 0;
    if (stack != g_buffs.stack_applied) {
        for (std::size_t r = 0; r < g_buffs.stack_rows.size(); ++r)
            game::write<std::uint16_t>(g_buffs.stack_rows[r] + kSpCategory, stack ? 0 : g_buffs.stack_original[r]);
        g_buffs.stack_applied = stack;
    }
    const bool stack_weapon = value_of(stack_weapon_buffs) != 0;
    if (stack_weapon != g_buffs.stack_weapon_applied) {
        for (std::size_t r = 0; r < g_buffs.stack_weapon_rows.size(); ++r)
            game::write<std::uint16_t>(g_buffs.stack_weapon_rows[r] + kSpCategory,
                stack_weapon ? 0 : g_buffs.stack_weapon_original[r]);
        g_buffs.stack_weapon_applied = stack_weapon;
    }
    const int deflect = value_of(deflect_window);
    if (deflect != g_buffs.deflect_applied) {
        const int extra = kDeflectWindowMs * (deflect - 100) / 100;
        const bool ok = nt_set_deflect_window(static_cast<std::uint32_t>(extra)) != 0;
        if (ok) g_buffs.deflect_applied = deflect;
        wchar_t message[128]{};
        swprintf_s(message, ok ? L"Deflect window: %d%% (perfect guard held %d ms more)."
                              : L"Deflect window: %d%% not applied yet (%d ms; game tasks not ready).",
            deflect, extra);
        log(message);
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
// summoning. Summon anywhere opens every monument by distance: widest range
// and no region (266 of 304 have one, with activateRange 999). Outside a
// monument's region the game counts the player out of range, and ashes
// summoned from it freeze half-dismissed (r140 tests at Radahn's battlefield
// and when walking into the next map tile). Where the game then finds no
// monument (Radahn's battlefield, buddy_stone=0), Multiple Spirit Ashes puts
// in the one nearest to the player's map tile.
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
        spirit_ashes::set_summon_anywhere(anywhere);
        // apply() patches code from the menu thread under this lock; two
        // VirtualProtect round trips on one page must not interleave.
        AcquireSRWLockExclusive(&g_patch_lock);
        const bool area = set_site(g_summon_area, anywhere);
        const bool area_item = set_site(g_summon_area_item, anywhere);
        const bool range = set_site(g_summon_range, anywhere);
        ReleaseSRWLockExclusive(&g_patch_lock);
        wchar_t message[200]{};
        swprintf_s(message, L"Summon anywhere %ls: monument rules applied; summoning-area checks %ls/%ls; range %ls.",
            anywhere ? L"on" : L"off", area ? L"ok" : L"not patched", area_item ? L"ok" : L"not patched",
            range ? L"ok" : L"not patched");
        log(message);
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

// The slot search over the whole set (see kBuddySetCapacity). At the loop:
// r14 = ChrSet, r8 = its entries, r9d = hint, r10d = 20 (start), rcx = 0,
// r15 = 0; on a free slot edi = its index and rbx = its entry, as the
// original loop leaves them for the code after it.
bool build_buddy_slot_search() noexcept {
    if (!g_buddy_slot_loop.ready) return false;
    if (g_buddy_slot_loop.replacement[0] == 0xE9) return true;
    std::uint8_t* const site = g_buddy_slot_loop.address;
    std::uint8_t* code = alloc_near(site);
    if (!code) {
        log(L"Spirit slots: no memory near the game code; kept at 10 per player.");
        return false;
    }
    std::uint8_t* p = code;
    const auto put = [&](std::initializer_list<std::uint8_t> bytes) {
        for (auto b : bytes) *p++ = b;
    };
    const auto put_u64 = [&](const std::uint8_t* v) { std::memcpy(p, &v, 8); p += 8; };
    put({0x41, 0x8B, 0x46, 0x10});                                     // mov eax,[r14+10h]  capacity
    put({0x83, 0xE8, 0x14});                                           // sub eax,20         slots from 20
    std::uint8_t* const loop = p;
    put({0x41, 0x39, 0xC1});                                           // cmp r9d,eax
    put({0x43, 0x8D, 0x3C, 0x11});                                     // lea edi,[r9+r10]
    put({0x7C, 0x02});                                                 // jl have
    put({0x29, 0xC7});                                                 // sub edi,eax        wrap round
    put({0x48, 0x63, 0xDF});                                           // have: movsxd rbx,edi
    put({0x48, 0xC1, 0xE3, 0x04});                                     // shl rbx,4
    put({0x4C, 0x01, 0xC3});                                           // add rbx,r8
    put({0x4C, 0x39, 0x3B});                                           // cmp [rbx],r15
    put({0x74, 0x00});                                                 // je found
    std::uint8_t* const to_found = p;
    put({0x41, 0xFF, 0xC1});                                           // inc r9d
    put({0x48, 0xFF, 0xC1});                                           // inc rcx
    put({0x48, 0x39, 0xC1});                                           // cmp rcx,rax
    put({0x7C, static_cast<std::uint8_t>(loop - (p + 2))});           // jl loop
    put({0xFF, 0x25, 0x00, 0x00, 0x00, 0x00}); put_u64(site + 57);     // none free: after the loop
    to_found[-1] = static_cast<std::uint8_t>(p - to_found);
    put({0xFF, 0x25, 0x00, 0x00, 0x00, 0x00}); put_u64(site + 65);     // found: past "mov rax,r15; jmp"
    FlushInstructionCache(GetCurrentProcess(), code, static_cast<SIZE_T>(p - code));
    std::vector<std::uint8_t> jump(57, 0x90);
    jump[0] = 0xE9;
    const auto disp = static_cast<std::int32_t>(reinterpret_cast<std::intptr_t>(code) -
        reinterpret_cast<std::intptr_t>(site + 5));
    std::memcpy(jump.data() + 1, &disp, 4);
    g_buddy_slot_loop.replacement = jump;
    return true;
}

// The spirit set's size (see kBuddySetCapacity). At the epilogue: rsi = the
// new ChrSet, its 80 entries built; rbx and rdi are restored by the epilogue
// and rsp+0..1Fh is free for the calls' shadow space.
bool build_buddy_set_size() noexcept {
    if (!g_buddy_set_size.ready) return false;
    std::uint8_t* const site = g_buddy_set_size.address;
    std::uint8_t* const load = site - kBuddySetEpilogue;                // mov rcx,[rip+disp32]
    std::int32_t disp{};
    std::memcpy(&disp, load + 3, 4);
    const std::uint8_t* const allocator = load + 7 + disp;
    std::uint8_t* code = alloc_near(site);
    if (!code) {
        log(L"Spirit set: no memory near the game code; kept at 80 entries.");
        return false;
    }
    std::uint8_t* p = code;
    const auto put = [&](std::initializer_list<std::uint8_t> bytes) {
        for (auto b : bytes) *p++ = b;
    };
    const auto put_u64 = [&](const std::uint8_t* v) { std::memcpy(p, &v, 8); p += 8; };
    const std::uint32_t bytes = kBuddySetCapacity * 16;
    std::uint8_t* to_done[3]{};
    put({0x83, 0x7E, 0x10, 0x50});                                     // cmp dword [rsi+10h],80
    put({0x75, 0x00}); to_done[0] = p;                                 // jne done
    put({0x48, 0x83, 0x7E, 0x18, 0x00});                               // cmp qword [rsi+18h],0
    put({0x74, 0x00}); to_done[1] = p;                                 // je done
    put({0x48, 0xB9}); put_u64(allocator);                             // mov rcx,&allocator
    put({0x48, 0x8B, 0x09});                                           // mov rcx,[rcx]
    put({0x48, 0x8B, 0x01});                                           // mov rax,[rcx]
    put({0xBA}); std::memcpy(p, &bytes, 4); p += 4;                    // mov edx,520*16
    put({0x41, 0xB8, 0x08, 0x00, 0x00, 0x00});                         // mov r8d,8
    put({0xFF, 0x50, 0x50});                                           // call [rax+50h]  allocate
    put({0x48, 0x85, 0xC0});                                           // test rax,rax
    put({0x74, 0x00}); to_done[2] = p;                                 // je done
    put({0x48, 0x89, 0xC3});                                           // mov rbx,rax
    put({0x48, 0x89, 0xC7});                                           // mov rdi,rax
    put({0x31, 0xC0});                                                 // xor eax,eax
    const std::uint32_t qwords = bytes / 8;
    put({0xB9}); std::memcpy(p, &qwords, 4); p += 4;                   // mov ecx,520*2
    put({0xF3, 0x48, 0xAB});                                           // rep stosq        zeroed entries
    put({0x48, 0x8B, 0x56, 0x18});                                     // mov rdx,[rsi+18h] the 80
    put({0x48, 0x89, 0x5E, 0x18});                                     // mov [rsi+18h],rbx
    put({0xC7, 0x46, 0x10}); std::memcpy(p, &kBuddySetCapacity, 4); p += 4; // mov dword [rsi+10h],520
    put({0x48, 0xB9}); put_u64(allocator);                             // mov rcx,&allocator
    put({0x48, 0x8B, 0x09});                                           // mov rcx,[rcx]
    put({0x48, 0x8B, 0x01});                                           // mov rax,[rcx]
    put({0xFF, 0x50, 0x68});                                           // call [rax+68h]  free
    for (std::uint8_t* at : to_done) at[-1] = static_cast<std::uint8_t>(p - at);
    for (std::uint8_t b : g_buddy_set_size.original) *p++ = b;          // done: the epilogue
    FlushInstructionCache(GetCurrentProcess(), code, static_cast<SIZE_T>(p - code));
    std::vector<std::uint8_t> jump(8, 0xCC);
    jump[0] = 0xE9;
    const auto rel = static_cast<std::int32_t>(reinterpret_cast<std::intptr_t>(code) -
        reinterpret_cast<std::intptr_t>(site + 5));
    std::memcpy(jump.data() + 1, &rel, 4);
    g_buddy_set_size.replacement = jump;
    return true;
}

spirit_ashes::Settings spirit_ash_settings() noexcept {
    // No cap on ashes (0): the spirit set's 500 bodies are the limit, and the
    // game has 94 ashes.
    // toggle_dismiss off when the item is left to the game (Same ash again = 2)
    return {value_of(ash_multiple) != 0, 0u, g_diagnostics,
        value_of(ash_repeat) != 2, true, true, true, true, g_spirit_hud_rows, static_cast<std::uint32_t>(value_of(ash_extra_cost)),
        static_cast<std::uint32_t>(value_of(ash_damage_per_ash)), value_of(ash_repeat) == 1,
        static_cast<std::uint32_t>(value_of(ash_max_regular)), static_cast<std::uint32_t>(value_of(ash_max_renowned))};
}

// First thing in initialize(): its HUD movie hook has to be in before the game
// opens the HUD, so its options are read here, ahead of load_settings().
bool g_npc_spawner_built_in{};   // NPC Spawner is linked in (msa feature npc-spawner)
bool g_npc_spawner_standalone{};  // NpcSpawner.dll is there: the built-in copy stays off
bool g_npc_spawner_started{};     // started without a provider: add_npc_spawner_pages adds its pages

void start_spirit_ashes() noexcept {
    if (!g_config_path.empty())
        g_spirit_hud_rows = GetPrivateProfileIntW(L"SpiritAshes", L"HudRows", 1, g_config_path.c_str()) != 0;
    for (const Id id : {ash_multiple, ash_extra_cost, ash_damage_per_ash, ash_repeat, ash_max_regular, ash_max_renowned}) {
        Option& option = g_options[id];
        if (!g_config_path.empty())
            option.value = clamp_value(option, static_cast<int>(GetPrivateProfileIntW(L"Vanilla", option.key,
                option.fallback, g_config_path.c_str())));
    }
    HMODULE self{};
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
        reinterpret_cast<LPCWSTR>(&start_spirit_ashes), &self);
    // The standalone MultipleSpiritAshes.dll may load after NativeTweaks: if
    // it is in the same folder, the built-in copy and its engine patches
    // (spirit set size, slot search) stay off from the start, or the two race
    // for the same code (08-11 log: "creation code not found" on one start,
    // "spirit slots not found" on another).
    bool standalone = GetModuleHandleW(L"MultipleSpiritAshes.dll") != nullptr;
    if (!standalone && self) {
        wchar_t path[MAX_PATH]{};
        const DWORD length = GetModuleFileNameW(self, path, MAX_PATH);
        if (length && length < MAX_PATH) {
            std::wstring file(path, length);
            file = file.substr(0, file.find_last_of(L"\\/") + 1) + L"MultipleSpiritAshes.dll";
            standalone = GetFileAttributesW(file.c_str()) != INVALID_FILE_ATTRIBUTES;
        }
    }
    if (standalone) log(L"MultipleSpiritAshes.dll is next to NativeTweaks: the built-in Several ashes at once stays off.");
    g_spirit_ashes_started = !standalone && self && spirit_ashes::start(self, spirit_ash_settings());
    // The spirit set is created once, when the game builds WorldChrMan: its
    // size can only be raised here, before that (see kBuddySetCapacity).
    if (g_spirit_ashes_started && value_of(ash_multiple) != 0) {
        resolve(g_buddy_set_size);
        const bool hooked = build_buddy_set_size() && set_site(g_buddy_set_size, true);
        wchar_t message[128]{};
        swprintf_s(message, L"Spirit set: %u entries when the game creates it.",
            hooked ? kBuddySetCapacity : 80u);
        log(message);
    }
    log(g_spirit_ashes_started
            ? L"Several ashes at once (Multiple Spirit Ashes 1.0.0, built in) started."
            : L"Several ashes at once unavailable: unsupported game version or MultipleSpiritAshes.dll is loaded.");
}

// NPC Spawner (built in, r158): it starts here, early in initialize(), and only
// when its option is on; the standalone NpcSpawner.dll wins. It has no Mod
// Settings provider of its own: its pages go into NativeTweaks' menu right
// after Vanilla Settings (add_npc_spawner_pages; user, 13-11: other providers
// would only come after every NativeTweaks row). Its F11 row stays.
void start_npc_spawner() noexcept {
    g_npc_spawner_built_in = npc_spawner::built_in();
    if (!g_npc_spawner_built_in) return;
    Option& option = g_options[npc_spawner];
    if (!g_config_path.empty())
        option.value = clamp_value(option, static_cast<int>(GetPrivateProfileIntW(L"Vanilla", option.key,
            option.fallback, g_config_path.c_str())));
    HMODULE self{};
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
        reinterpret_cast<LPCWSTR>(&start_npc_spawner), &self);
    wchar_t path[MAX_PATH]{};
    const DWORD length = self ? GetModuleFileNameW(self, path, MAX_PATH) : 0;
    if (!length || length >= MAX_PATH) return;
    std::wstring folder(path, length);
    folder.resize(folder.find_last_of(L"\\/"));
    g_npc_spawner_standalone = npc_spawner::standalone_present(folder);
    if (g_npc_spawner_standalone) {
        log(L"NpcSpawner.dll is loaded or next to NativeTweaks: the built-in NPC Spawner stays off.");
        return;
    }
    if (option.value == 0) {
        log(L"NPC Spawner (built in) is off.");
        return;
    }
    g_npc_spawner_started = npc_spawner::start_hosted(folder, self);
    log(g_npc_spawner_started ? L"NPC Spawner (built in) started: F11 > NPC Spawner."
                              : L"NPC Spawner (built in) could not start.");
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
// Param tables rebuilt with rows of ours (append_param_rows): row pointers
// taken before a rebuild point into the old copy.
int g_param_appends{};
std::vector<std::uint8_t*> append_param_rows(const wchar_t* name, std::size_t row_size,
    const std::vector<std::pair<std::int32_t, const std::uint8_t*>>& added, bool& retry) noexcept;
// The game's status SpEffects chosen below may also land on the attacker
// itself or on its allies (effectTargetSelf/Friend +0x15F bits 0/1,
// effectTargetFriendlyTarget/SelfTarget +0x16C bits 1/2): bleed 6401 and
// poison 106000 do, so rolling into an enemy put them on the player (user,
// 08-11). The slots get copies of ours instead: for the player's attacks
// they cannot reach a player character either (effectTargetPlayer, bit 3;
// humanoid NPCs share those rows), for enemies' attacks only opponents.
constexpr std::int32_t kPlayerEffectCopyId = 1999990110;  // + status
constexpr std::int32_t kEnemyEffectCopyId = 1999990120;   // + status
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
    std::int32_t player_effect[kWeaponEffectCount]{};  // our copies (or effect)
    std::int32_t enemy_effect[kWeaponEffectCount]{};
    game::ParamTable npc_attacks{};             // enemies, NPCs and spirit ashes
    std::vector<std::int32_t> npc_original{};
    // Rows that get the effects: they deal damage and hit the other side
    // (Nexus report 08-11: rolling into an enemy applied the status through
    // the no-damage impact rows, and flask or self-buff rows could too).
    std::vector<char> eligible{};
    std::vector<char> npc_eligible{};
    // Player rows with no damage and no effect of their own (impact, push,
    // contact when rolling into an enemy): while weapon effects are on, their
    // disableHitSpEffect (+0x7E bit 2) is set, so the weapon's own effect
    // slots (spEffectBehaviorId, where effects 3-5 go) do not apply on them.
    std::vector<std::uintptr_t> contact_rows{};
    bool contact_blocked{};
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
// AtkParam rows that deal damage (any damage correction or flat damage:
// +0x3E..+0x44, +0x186, +0x50..+0x56, +0x188) and hit opponents
// (opposeTarget, byte +0x81 bit 0) but not the attacker itself (selfTarget,
// bit 2). Impact, push, flask and self-buff rows are left out.
std::vector<char> damaging_attack_rows(const game::ParamTable& table) {
    std::vector<char> eligible(table.rows.size(), 0);
    for (std::size_t r = 0; r < table.rows.size(); ++r) {
        std::uint16_t damage[6]{};
        std::uint16_t flat[6]{};
        std::uint8_t targets{};
        if (!game::read(table.rows[r] + 0x3E, damage[0]) || !game::read(table.rows[r] + 0x40, damage[1]) ||
            !game::read(table.rows[r] + 0x42, damage[2]) || !game::read(table.rows[r] + 0x44, damage[3]) ||
            !game::read(table.rows[r] + 0x186, damage[4]) || !game::read(table.rows[r] + 0x50, flat[0]) ||
            !game::read(table.rows[r] + 0x52, flat[1]) || !game::read(table.rows[r] + 0x54, flat[2]) ||
            !game::read(table.rows[r] + 0x56, flat[3]) || !game::read(table.rows[r] + 0x188, flat[4]) ||
            !game::read(table.rows[r] + 0x81, targets)) continue;
        bool deals{};
        for (int k = 0; k < 5; ++k) deals = deals || damage[k] || flat[k];
        eligible[r] = deals && (targets & 0x1) && !(targets & 0x4);
    }
    return eligible;
}

void fill_effect_slots(const game::ParamTable& table, const std::vector<std::int32_t>& original,
    std::uint32_t mask, const std::int32_t (&ids)[kWeaponEffectCount],
    std::size_t slot_offset = kAtkSlotOffset, std::size_t slot_count = kAtkSlots,
    const std::vector<char>* eligible = nullptr) noexcept {
    if (slot_count > kAtkSlots || original.size() != table.rows.size() * slot_count) return;
    if (eligible && eligible->size() != table.rows.size()) eligible = nullptr;
    for (std::size_t r = 0; r < table.rows.size(); ++r) {
        std::int32_t slots[kAtkSlots]{};
        std::memcpy(slots, &original[r * slot_count], slot_count * sizeof(std::int32_t));
        std::size_t next{};
        for (std::size_t s = 0; s < kWeaponEffectCount && (!eligible || (*eligible)[r]); ++s) {
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
                state.npc_eligible = damaging_attack_rows(state.npc_attacks);
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
            {
                std::vector<std::vector<std::uint8_t>> copies;
                std::vector<std::pair<std::int32_t, const std::uint8_t*>> added;
                copies.reserve(kWeaponEffectCount * 2);
                for (int pass = 0; pass < 2; ++pass)
                    for (std::size_t s = 0; s < kWeaponEffectCount; ++s) {
                        if (state.effect[s] <= 0) continue;
                        const auto found = std::lower_bound(index.begin(), index.end(),
                            std::make_pair(state.effect[s], std::uintptr_t{0}));
                        if (found == index.end() || found->first != state.effect[s]) continue;
                        std::vector<std::uint8_t> row(0x390);
                        if (!game::safe_read(found->second, row.data(), row.size())) continue;
                        row[0x15F] = static_cast<std::uint8_t>(row[0x15F] & ~(pass == 0 ? 0x0B : 0x03));
                        row[0x16C] = static_cast<std::uint8_t>(row[0x16C] & ~0x06);
                        copies.push_back(std::move(row));
                        added.push_back({(pass == 0 ? kPlayerEffectCopyId : kEnemyEffectCopyId) +
                            static_cast<std::int32_t>(s), copies.back().data()});
                    }
                bool retry{};
                const bool made = !append_param_rows(L"SpEffectParam", 0x390, added, retry).empty();
                for (std::size_t s = 0; s < kWeaponEffectCount; ++s) {
                    state.player_effect[s] = made && state.effect[s] > 0 ? kPlayerEffectCopyId + static_cast<std::int32_t>(s)
                                                                         : state.effect[s];
                    state.enemy_effect[s] = made && state.effect[s] > 0 ? kEnemyEffectCopyId + static_cast<std::int32_t>(s)
                                                                        : state.effect[s];
                }
                log(made ? L"Weapon effects: copies that reach opponents only are in use (player: no players, no self)."
                         : L"Weapon effects: copies could not be added; the game's own effects are used.");
            }
            state.attacks = std::move(attacks);
            read_effect_slots(state.attacks, state.original);
            state.eligible = damaging_attack_rows(state.attacks);
            for (std::size_t r = 0; r < state.attacks.rows.size(); ++r) {
                if (state.eligible[r]) continue;
                bool own_effect{};
                for (std::size_t k = 0; k < kAtkSlots; ++k) own_effect = own_effect || state.original[r * kAtkSlots + k] > 0;
                std::uint8_t flags{};
                if (own_effect || !game::read(state.attacks.rows[r] + 0x7E, flags) || (flags & 0x4)) continue;
                std::uint16_t damage[5]{};
                bool deals{};
                for (const std::size_t o : {0x3E, 0x40, 0x42, 0x44, 0x186, 0x50, 0x52, 0x54, 0x56, 0x188}) {
                    std::uint16_t v{};
                    deals = deals || (game::read(state.attacks.rows[r] + o, v) && v);
                }
                (void)damage;
                if (!deals) state.contact_rows.push_back(state.attacks.rows[r]);
            }
            {
                const auto count = [](const std::vector<char>& v) {
                    return static_cast<unsigned long long>(std::count(v.begin(), v.end(), 1));
                };
                wchar_t message[200]{};
                swprintf_s(message, L"Weapon effects: %llu of %zu player attacks and %llu of %zu enemy attacks deal damage to opponents.",
                    count(state.eligible), state.eligible.size(), count(state.npc_eligible), state.npc_eligible.size());
                log(message);
            }
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
    if (attack_mask != state.applied_mask)
        fill_effect_slots(state.attacks, state.original, attack_mask, state.player_effect, kAtkSlotOffset, kAtkSlots,
            &state.eligible);
    if (weapon_mask != state.applied_weapon_mask) {
        fill_effect_slots(state.weapons, state.weapon_original, weapon_mask, state.player_effect, kWeaponSlotOffset,
            kWeaponSlots);
        wchar_t message[160]{};
        swprintf_s(message, L"Weapon effects: attack slots mask %u, weapon slots mask %u.", attack_mask, weapon_mask);
        log(message);
    }
    if (npc_mask != state.applied_npc_mask)
        fill_effect_slots(state.npc_attacks, state.npc_original, npc_mask, state.enemy_effect, kAtkSlotOffset, kAtkSlots,
            &state.npc_eligible);
    const bool block_contact = mask != 0;
    if (block_contact != state.contact_blocked) {
        for (const std::uintptr_t row : state.contact_rows) {
            std::uint8_t flags{};
            if (!game::read(row + 0x7E, flags)) continue;
            game::write<std::uint8_t>(row + 0x7E, block_contact ? (flags | 0x4) : (flags & ~0x4));
        }
        state.contact_blocked = block_contact;
        wchar_t message[120]{};
        swprintf_s(message, L"Weapon effects: %zu contact rows %ls hit effects.", state.contact_rows.size(),
            block_contact ? L"without" : L"back to their");
        log(message);
    }
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
// r156 (Nexus report: gargoyles and Erdtree Avatars stayed immune): about 2000
// NpcParam rows also carry resident SpEffects that the status check walks
// (+0x4FAE40 in 2.7.2.0): 90000 poison, 90010 rot, 90020 bleed, 90040
// frostbite and 90300 sleep, each with one disable bit (SpEffectParam
// disablePoison/Disease/Blood +0x163, disableFreeze +0x259, disableSleep
// +0x15F). While the option is on that bit is cleared. Death blight (90030) and
// madness (90060) stay, as above. Each bit is checked at load.
struct DisableBit { std::uint32_t id; std::ptrdiff_t offset; std::uint8_t mask; };
constexpr DisableBit kNpcDisableEffects[]{
    {90000, 0x163, 0x04}, {90010, 0x163, 0x08}, {90020, 0x163, 0x10}, {90040, 0x259, 0x08}, {90300, 0x15F, 0x80}};
struct ImmunityState {
    bool attempted{};
    bool ready{};
    game::ParamTable table{};
    std::vector<std::size_t> columns{};
    std::vector<std::uint16_t> medians{};
    std::vector<std::pair<std::uintptr_t, std::size_t>> immune_cells{};  // address, column
    std::vector<std::pair<std::uintptr_t, std::uint8_t>> disable_bits{};  // address, mask
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
        game::ParamTable effects;
        if (game::find_param(g_statics, L"SpEffectParam", 0x390, effects, g_log)) {
            for (const auto& bit : kNpcDisableEffects) {
                const std::uintptr_t row = param_row_by_id(effects, bit.id);
                std::uint8_t flags{};
                if (row && game::read(row + bit.offset, flags) && (flags & bit.mask))
                    state.disable_bits.push_back({row + bit.offset, bit.mask});
            }
        }
        wchar_t cells[160]{};
        swprintf_s(cells, L"Enemy immunities: %zu immune values and %zu of %zu immunity effects found.",
            state.immune_cells.size(), state.disable_bits.size(), std::size(kNpcDisableEffects));
        log(cells);
        state.ready = true;
    }
    const bool want = value_of(enemies_no_immunity) != 0;
    if (want == state.applied) return;
    // Only the cells that were 999 when the table loaded are touched.
    for (const auto& [address, column] : state.immune_cells)
        game::write<std::uint16_t>(address, want ? state.medians[column] : kImmune);
    for (const auto& [address, mask] : state.disable_bits) {
        std::uint8_t flags{};
        if (!game::read(address, flags)) continue;
        game::write<std::uint8_t>(address, static_cast<std::uint8_t>(want ? flags & ~mask : flags | mask));
    }
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
    int applied{100};  // percent written to the cells; 0 = No lock-on
};

LockOnState g_lock_on{};

void sync_lock_on() noexcept {
    if (g_lock_range) {
        g_lock_range->on = value_of(enemies_no_lock_on) ? 1u : 0u;
        const float scale = static_cast<float>(value_of(lock_on_distance)) / 100.0f;
        g_lock_range->scale = scale;
        g_lock_range->scale_squared = scale * scale;
    }
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
    const int want = value_of(enemies_no_lock_on) != 0 ? 0 : g_lock_range ? 100 : value_of(lock_on_distance);
    if (want == state.applied) return;
    for (const auto& [address, original] : state.cells)
        game::write<std::uint8_t>(address, static_cast<std::uint8_t>(
            std::min(255, static_cast<int>(original) * want / 100)));
    state.applied = want;
}

// Bewitching Branch on every enemy (Nexus request): an enemy can be charmed
// only while it has SpEffect 90200 ("NPC: Enable Charm", the one row with
// enableCharm), which the 3404 charmable NpcParam rows of 2.7.2.0 carry in
// spEffectID14 (s32 +0x190). Empty slots of the other rows get it; a slot that
// holds anything else (ERR uses it) is left alone. Friendly NPCs, phantoms and
// objects are skipped by team (teamType u8 +0x133). The game reads the row when
// a character spawns, so this reaches enemies that appear afterwards.
bool excluded_team(std::uint8_t team) noexcept;
constexpr std::size_t kNpcCharmSlot = 0x190;
constexpr std::int32_t kEnableCharmEffect = 90200;
struct CharmState {
    bool attempted{};
    bool ready{};
    std::vector<std::pair<std::uintptr_t, std::int32_t>> enemies{};  // address, original
    std::vector<std::pair<std::uintptr_t, std::int32_t>> bosses{};
    bool enemies_applied{};
    bool bosses_applied{};
};

CharmState g_charm{};

void sync_charm() noexcept {
    auto& state = g_charm;
    if (state.attempted) return;
    if (!state.ready) {
        game::ParamTable table;
        if (!game::find_param(g_statics, L"NpcParam", 0x2E0, table, g_log)) {
            if (table.file) state.attempted = true;
            return;
        }
        std::size_t charmable{};
        try {
            for (const auto row : table.rows) {
                std::int32_t slot{};
                std::uint32_t hp{};
                std::uint8_t team{};
                if (!game::read(row + kNpcCharmSlot, slot) || !game::read(row + 0x24, hp) ||
                    !game::read(row + 0x133, team) || hp == 0) continue;
                if (slot == kEnableCharmEffect) {
                    ++charmable;
                    continue;
                }
                if (slot > 0) continue;
                if (npc_row_is_boss(row) || team == 7) state.bosses.push_back({row + kNpcCharmSlot, slot});
                else if (!excluded_team(team)) state.enemies.push_back({row + kNpcCharmSlot, slot});
            }
        } catch (...) {
            state.enemies.clear();
            state.bosses.clear();
            state.attempted = true;
            return;
        }
        // Thousands of rows carry the effect in this slot: another layout would not.
        const bool ok = charmable >= 500;
        wchar_t message[160]{};
        swprintf_s(message, L"Charm: %zu rows already charmable, %zu enemies and %zu bosses can be added%s.",
            charmable, state.enemies.size(), state.bosses.size(), ok ? L"" : L" -> options disabled");
        log(message);
        if (!ok) {
            state.enemies.clear();
            state.bosses.clear();
            state.attempted = true;
            return;
        }
        state.ready = true;
    }
    const auto apply = [](const auto& cells, bool want, bool& applied) {
        if (want == applied) return;
        for (const auto& [address, original] : cells)
            game::write<std::int32_t>(address, want ? kEnableCharmEffect : original);
        applied = want;
    };
    apply(state.enemies, value_of(charm_all_enemies) != 0, state.enemies_applied);
    apply(state.bosses, value_of(charm_bosses) != 0, state.bosses_applied);
}

// How far the player's projectiles fly (Nexus request): flight time
// (BulletParam.life f32 +0x10) and the in-range distance (dist f32 +0x14,
// beyond it a bullet uses its out-of-range speed and gravity). Rows: every
// bullet a spell names (Magic refCategoryN = 1 -> refIdN; r151 test: spells fell
// short), what BehaviorParam_PC fires (refType u8 +0x9 = 1, refId +0xC: weapon
// skills), and the player's id ranges of the base game and the DLC. Enemies live
// in 2000000-9999999 and 205000000-206999999. hitBulletId (+0x68) and
// intervalCreateBulletId (+0xAC) are followed. Only moving bullets change (initVelocity +0x28 or maxVelocity
// +0x34): for a bullet that stays in place, life is how long an area lasts.
//
// A plain percent is not enough: Glintstone Pebble flies 28 m and Lock-on
// distance reaches 40-50 m. Real projectiles (10 m or more) are also brought up
// to kProjectileBaseReach at the same percent, so at 500% both options reach about
// 250 m. Shorter ones (sprays, fragments) only scale by the percent.
constexpr float kProjectileBaseReach = 50.0f;
constexpr float kProjectileMinReach = 10.0f;
constexpr float kProjectileEmitterReach = 2.0f;
// Magic: refCategory1-10 (u8) and refId1-10 (s32), not contiguous.
constexpr std::ptrdiff_t kMagicRefCategories[]{0x1E, 0x20, 0x27, 0x2B, 0x49, 0x61, 0x64, 0x65, 0x66, 0x67};
constexpr std::ptrdiff_t kMagicRefIds[]{0x68, 0x6C, 0x70, 0x78, 0x7C, 0x80, 0x84, 0x88, 0x8C, 0x90};

bool player_bullet_id(std::int32_t id) noexcept {
    return (id > 0 && id < 2000000) ||                 // weapon skills, Rainbow Stone, Smithscript
        (id >= 10000000 && id <= 11999999) ||          // spells, thrown items
        (id >= 20000000 && id <= 20099999) ||          // arrows, bolts
        (id >= 200000000 && id <= 200999999) ||        // DLC weapon skills
        (id >= 210000000 && id <= 210999999);          // DLC spells, thrown items
}

struct ProjectileCell {
    std::uintptr_t life;
    std::uintptr_t dist;  // 0 when the row has none
    float original_life;
    float original_dist;
    float range;          // metres in the game's data
};

struct ProjectileRangeState {
    bool attempted{};
    bool ready{};
    std::vector<ProjectileCell> cells{};
    int applied{100};
};

ProjectileRangeState g_projectile_range{};

void sync_projectile_range() noexcept {
    auto& state = g_projectile_range;
    if (state.attempted) return;
    if (!state.ready) {
        game::ParamTable bullets;
        game::ParamTable behaviors;
        game::ParamTable magic;
        if (!game::find_param(g_statics, L"Bullet", 0x110, bullets, g_log) ||
            !game::find_param(g_statics, L"BehaviorParam_PC", 0x20, behaviors, g_log) ||
            !game::find_param(g_statics, L"Magic", 0xA8, magic, g_log)) {
            if (bullets.file && behaviors.file && magic.file) state.attempted = true;
            return;
        }
        try {
            std::unordered_map<std::int32_t, std::uintptr_t> by_id;
            by_id.reserve(bullets.rows.size());
            std::vector<std::int32_t> pending;
            for (std::size_t i = 0; i < bullets.rows.size(); ++i) {
                std::uint32_t raw_id{};
                if (!game::read(bullets.file + 0x40 + i * 0x18, raw_id)) continue;
                const auto id = static_cast<std::int32_t>(raw_id);
                by_id.emplace(id, bullets.rows[i]);
                if (player_bullet_id(id)) pending.push_back(id);
            }
            std::size_t spell_bullets{};
            for (const auto row : magic.rows)
                for (std::size_t r = 0; r < std::size(kMagicRefIds); ++r) {
                    std::uint8_t category{};
                    std::int32_t id{};
                    if (game::read(row + kMagicRefCategories[r], category) && category == 1 &&
                        game::read(row + kMagicRefIds[r], id) && id > 0) {
                        pending.push_back(id);
                        ++spell_bullets;
                    }
                }
            wchar_t found_message[96]{};
            swprintf_s(found_message, L"Projectile range: %zu bullet references in Magic.", spell_bullets);
            log(found_message);
            for (const auto row : behaviors.rows) {
                std::uint8_t type{};
                std::int32_t id{};
                if (game::read(row + 0x9, type) && type == 1 && game::read(row + 0xC, id) && id > 0)
                    pending.push_back(id);
            }
            std::unordered_set<std::int32_t> seen;
            while (!pending.empty()) {
                const std::int32_t id = pending.back();
                pending.pop_back();
                if (!seen.insert(id).second) continue;
                const auto found = by_id.find(id);
                if (found == by_id.end()) continue;
                const std::uintptr_t row = found->second;
                for (const std::ptrdiff_t child : {0x68, 0xAC}) {
                    std::int32_t next{};
                    if (game::read(row + child, next) && next > 0) pending.push_back(next);
                }
                float initial{}, maximum{}, life{}, dist{};
                game::read(row + 0x28, initial);
                game::read(row + 0x34, maximum);
                game::read(row + 0x10, life);
                game::read(row + 0x14, dist);
                const float speed = std::max(initial, maximum);
                if (speed <= 0.0f || life <= 0.0f) continue;
                // Emitters (Glintblade Trio 0.6 m, Catch Flame 0.8 m) fire their child
                // when they die: a longer life would only delay the spell.
                if (life * speed < kProjectileEmitterReach) continue;
                state.cells.push_back({row + 0x10, dist > 0.0f ? row + 0x14 : 0, life, dist, life * speed});
            }
        } catch (...) {
            state.cells.clear();
            state.attempted = true;
            return;
        }
        const bool ok = state.cells.size() >= 500;
        wchar_t message[160]{};
        swprintf_s(message, L"Projectile range: %zu moving player bullets%s.", state.cells.size(),
            ok ? L"" : L" -> option disabled");
        log(message);
        if (!ok) {
            state.cells.clear();
            state.attempted = true;
            return;
        }
        state.ready = true;
    }
    const int want = value_of(projectile_range);
    if (want == state.applied) return;
    const float percent = static_cast<float>(want) / 100.0f;
    // 0 at 100%, 1 at the top of the slider, so the reach grows smoothly.
    const float blend = static_cast<float>(want - 100) / 400.0f;
    for (const auto& cell : state.cells) {
        float factor = percent;
        if (cell.range >= kProjectileMinReach && cell.range < kProjectileBaseReach)
            factor *= 1.0f + (kProjectileBaseReach / cell.range - 1.0f) * blend;
        game::write<float>(cell.life, cell.original_life * factor);
        if (cell.dist) game::write<float>(cell.dist, cell.original_dist * factor);
    }
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
        // Keep the rows that already throw harder (2, super huge blowout) and the
        // grabs (10 start, 11 finish): 5 would weaken or break them (param audit
        // 21-11; 1.17.2 has 3 rows with 2 and none with 10 or 11).
        const bool keep = r.behavior == 2 || r.behavior == 10 || r.behavior == 11;
        game::write<std::uint8_t>(r.row + kAtkBehaviorOffset, on && !keep ? kGuaranteedReaction : r.behavior);
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
// already sorts out (g_buffs.rows[1]: 247 SpEffects in 2.7.2.0), plus the
// consumables and talismans of g_buffs.hide_extra, draw their
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
        // The effects a buff turns into or repeats (replaceSpEffectId +0x124,
        // cycleOccurrenceSpEffectId +0x128) draw some of its visuals too
        // (Nexus, r142: some effects still showed), up to 4 links deep.
        std::vector<std::uintptr_t> effects = g_buffs.rows[category];
        if (category == 1) effects.insert(effects.end(), g_buffs.hide_extra.begin(), g_buffs.hide_extra.end());
        game::ParamTable effect_table;
        if (game::find_param(g_statics, L"SpEffectParam", 0x390, effect_table, g_log)) {
            std::vector<std::uintptr_t> frontier = effects;
            for (int depth = 0; depth < 4 && !frontier.empty(); ++depth) {
                std::vector<std::uintptr_t> next;
                for (const auto effect : frontier)
                    for (const std::ptrdiff_t link : {0x124, 0x128}) {
                        std::int32_t id{};
                        if (!game::read(effect + link, id) || id <= 0) continue;
                        const std::uintptr_t linked = param_row_by_id(effect_table, static_cast<std::uint32_t>(id));
                        if (!linked || std::find(effects.begin(), effects.end(), linked) != effects.end()) continue;
                        effects.push_back(linked);
                        next.push_back(linked);
                    }
                frontier = std::move(next);
            }
        }
        std::vector<std::int32_t> ids;
        for (const auto effect : effects)
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

// Hide buff effects, part 2: the burst shown while an item is used comes from
// the item, not its buff (Nexus, Gold-Pickled Fowl Foot: castSfxId 301260,
// fireSfxId 301261). EquipParamGoods castSfxId s32 +0x60 and fireSfxId +0x64
// (raw: Smithbox's layout is one byte off here) go to -1 on the items whose
// effect (refCategory u8 +0x3F = 2, refId_default +0x4) Hide buff effects
// covers. Flasks heal instantly, so their effect is not covered and they keep it.
// Hide weapon buff effects does the same for greases (fireSfxId 303000...).
struct ItemUseVfxState {
    bool attempted{};
    bool ready{};
    std::vector<std::uintptr_t> rows{};
    std::vector<std::int32_t> original{};  // 2 per row
    bool applied{};
};

ItemUseVfxState g_item_use_vfx{};         // self buffs
ItemUseVfxState g_weapon_item_use_vfx{};  // greases
constexpr std::ptrdiff_t kItemUseSfx[]{0x60, 0x64};

void sync_item_use_vfx(ItemUseVfxState& state, const std::vector<std::uint32_t>& ids, Id option,
    const wchar_t* what) noexcept {
    if (state.attempted) return;
    if (!state.ready) {
        if (g_buffs.attempted) {
            state.attempted = true;
            return;
        }
        if (!g_buffs.ready) return;
        game::ParamTable table;
        if (!game::find_param(g_statics, L"EquipParamGoods", 0xB0, table, g_log)) {
            if (table.file) state.attempted = true;
            return;
        }
        for (const auto row : table.rows) {
            std::uint8_t category{};
            std::int32_t effect{};
            if (!game::read(row + 0x3F, category) || category != 2 || !game::read(row + 0x4, effect) || effect <= 0 ||
                !std::binary_search(ids.begin(), ids.end(), static_cast<std::uint32_t>(effect)))
                continue;
            std::int32_t sfx[std::size(kItemUseSfx)]{-1, -1};
            bool any = false;
            for (std::size_t f = 0; f < std::size(kItemUseSfx); ++f) {
                game::read(row + kItemUseSfx[f], sfx[f]);
                any = any || sfx[f] > 0;
            }
            if (!any) continue;
            state.rows.push_back(row);
            state.original.insert(state.original.end(), std::begin(sfx), std::end(sfx));
        }
        wchar_t message[120]{};
        swprintf_s(message, L"Hide %s effects: %zu items show a burst when used.", what, state.rows.size());
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
        for (std::size_t f = 0; f < std::size(kItemUseSfx); ++f) {
            const std::int32_t original = state.original[r * std::size(kItemUseSfx) + f];
            if (original > 0) game::write<std::int32_t>(state.rows[r] + kItemUseSfx[f], want ? -1 : original);
        }
    state.applied = want;
}

// Hide weapon buff effects: the weapon buffs (g_buffs.rows[0]) lose their
// links to SpEffectVfxParam (vfxId..vfxId7 -> -1) instead. Clearing the sfx of
// their rows, as for self buffs, left the game spawning a stray effect on the
// weapon (in-game test 2026-10-29, any grease: a big golden smoke wall) and
// light attacks stopped working; the magic glow stayed on the weapon too.
void sync_weapon_buff_vfx() noexcept {
    auto& state = g_weapon_buff_vfx;
    if (state.attempted) return;
    if (!state.ready) {
        if (g_buffs.attempted) {
            state.attempted = true;
            return;
        }
        if (!g_buffs.ready) return;
        for (const auto effect : g_buffs.rows[0]) {
            std::int32_t ids[std::size(kBuffVfxSlots)]{};
            bool any = false;
            for (std::size_t s = 0; s < std::size(kBuffVfxSlots); ++s) {
                ids[s] = -1;
                game::read(effect + kBuffVfxSlots[s], ids[s]);
                any = any || ids[s] > 0;
            }
            if (!any) continue;
            state.rows.push_back(effect);
            state.original.insert(state.original.end(), std::begin(ids), std::end(ids));
        }
        wchar_t message[140]{};
        swprintf_s(message, L"Hide weapon buff effects: %zu of %zu weapon buffs have visuals.",
            state.rows.size(), g_buffs.rows[0].size());
        log(message);
        if (state.rows.empty()) {
            state.attempted = true;
            return;
        }
        state.ready = true;
    }
    const bool want = value_of(hide_weapon_buff_effects) != 0;
    if (want == state.applied) return;
    for (std::size_t r = 0; r < state.rows.size(); ++r)
        for (std::size_t s = 0; s < std::size(kBuffVfxSlots); ++s) {
            const std::int32_t original = state.original[r * std::size(kBuffVfxSlots) + s];
            if (original > 0) game::write<std::int32_t>(state.rows[r] + kBuffVfxSlots[s], want ? -1 : original);
        }
    state.applied = want;
}

// Solid spirit ashes (Nexus request): summoned ashes carry SpEffect 295000
// ([Spirit Summon] Color, puppets 295200), whose SpEffectVfxParam row 57000
// (57100) overrides the character's look with PhantomParam 200 (201): the
// blue ghost shading. phantomParamOverwriteId (s32 +0x80) -1, as on rows
// without an override, leaves them with their normal look. Evergaol bosses
// and summoned NPCs get theirs from other rows and stay as they are.
struct GhostState {
    bool attempted{};
    bool ready{};
    std::vector<std::uintptr_t> rows{};
    std::vector<std::int32_t> original{};
    bool applied{};
};

GhostState g_ghost{};
constexpr std::uint32_t kAshGhostVfx[]{57000, 57100};
constexpr std::ptrdiff_t kPhantomOverwriteId = 0x80;

void sync_ghost() noexcept {
    auto& state = g_ghost;
    if (state.attempted) return;
    if (!state.ready) {
        game::ParamTable table;
        if (!game::find_param(g_statics, L"SpEffectVfxParam", 0xA4, table, g_log)) {
            if (table.file) state.attempted = true;
            return;
        }
        for (const auto id : kAshGhostVfx) {
            const std::uintptr_t row = param_row_by_id(table, id);
            std::int32_t phantom{-1};
            if (!row || !game::read(row + kPhantomOverwriteId, phantom) || phantom <= 0) continue;
            state.rows.push_back(row);
            state.original.push_back(phantom);
        }
        wchar_t message[120]{};
        swprintf_s(message, L"Solid spirit ashes: %zu ghost-look SpEffectVfxParam rows.", state.rows.size());
        log(message);
        if (state.rows.empty()) {
            state.attempted = true;
            return;
        }
        state.ready = true;
    }
    const bool want = value_of(solid_ashes) != 0;
    if (want == state.applied) return;
    for (std::size_t r = 0; r < state.rows.size(); ++r)
        game::write<std::int32_t>(state.rows[r] + kPhantomOverwriteId, want ? -1 : state.original[r]);
    state.applied = want;
}

// Solid ghostly characters, effect side: the other 54 SpEffectVfxParam rows
// with a phantom shading override (+0x80 > 0; golden and ghost looks that
// SpEffects put on NPCs, summons and enemies) lose it while the option is
// on. The ash rows stay with Solid spirit ashes.
GhostState g_ghost_all{};

void sync_ghost_all() noexcept {
    auto& state = g_ghost_all;
    if (state.attempted) return;
    if (!state.ready) {
        game::ParamTable table;
        if (!game::find_param(g_statics, L"SpEffectVfxParam", 0xA4, table, g_log)) {
            if (table.file) state.attempted = true;
            return;
        }
        for (std::size_t i = 0; i < table.rows.size(); ++i) {
            std::uint32_t id{};
            std::int32_t phantom{-1};
            if (!game::read(table.file + 0x40 + i * 0x18, id) ||
                std::find(std::begin(kAshGhostVfx), std::end(kAshGhostVfx), id) != std::end(kAshGhostVfx) ||
                !game::read(table.rows[i] + kPhantomOverwriteId, phantom) || phantom <= 0)
                continue;
            state.rows.push_back(table.rows[i]);
            state.original.push_back(phantom);
        }
        wchar_t message[120]{};
        swprintf_s(message, L"Solid ghostly characters: %zu ghost-look SpEffectVfxParam rows.", state.rows.size());
        log(message);
        if (state.rows.empty()) {
            state.attempted = true;
            return;
        }
        state.ready = true;
    }
    const bool want = value_of(solid_ghosts) != 0;
    if (want == state.applied) return;
    for (std::size_t r = 0; r < state.rows.size(); ++r)
        game::write<std::int32_t>(state.rows[r] + kPhantomOverwriteId, want ? -1 : state.original[r]);
    state.applied = want;
}

// Solid ghostly characters, NPC side: NpcParam.phantomShaderId (s32 +0x1D4,
// PhantomParam row, -1 none) draws a character as a spirit by itself; in
// 2.7.2.0 only Melina (21801000, 21801034: row 80) has one. -1 while the
// option is on; it is read when the character loads.
GhostState g_ghost_npc{};
constexpr std::ptrdiff_t kNpcPhantomShader = 0x1D4;

void sync_ghost_npc() noexcept {
    auto& state = g_ghost_npc;
    if (state.attempted) return;
    if (!state.ready) {
        game::ParamTable table;
        if (!game::find_param(g_statics, L"NpcParam", 0x2E0, table, g_log)) {
            if (table.file) state.attempted = true;
            return;
        }
        for (const auto row : table.rows) {
            std::int32_t phantom{-1};
            if (!game::read(row + kNpcPhantomShader, phantom) || phantom <= 0) continue;
            state.rows.push_back(row);
            state.original.push_back(phantom);
        }
        wchar_t message[120]{};
        swprintf_s(message, L"Solid ghostly characters: %zu NpcParam rows with a phantom shader.", state.rows.size());
        log(message);
        if (state.rows.empty()) {
            state.attempted = true;
            return;
        }
        state.ready = true;
    }
    const bool want = value_of(solid_ghosts) != 0;
    if (want == state.applied) return;
    for (std::size_t r = 0; r < state.rows.size(); ++r)
        game::write<std::int32_t>(state.rows[r] + kNpcPhantomShader, want ? -1 : state.original[r]);
    state.applied = want;
}

// New Game+ poise (Nexus request: "after NG+2 they get a massive reduction"):
// ClearCountCorrectParam rows 2-7, and 102-107, carry SuperArmorDamageRate
// (f32 +0x64), x0.95 at NG+2 down to x0.6 at NG+7 (2.7.2.0 regulation); the
// other cycles have none. 100% keeps the game's value and 0% makes it 1, the
// first playthrough's. World difficulty does not use this column.
constexpr std::ptrdiff_t kNgPoiseOffset = 0x64;
struct NgPoiseState {
    bool attempted{};
    bool ready{};
    std::vector<std::pair<std::uintptr_t, float>> cells{};  // address, game value
    int applied = -1;
};
NgPoiseState g_ng_poise{};

void sync_ng_poise() noexcept {
    auto& state = g_ng_poise;
    if (state.attempted) return;
    if (!state.ready) {
        game::ParamTable correct;
        if (!game::find_param(g_statics, L"ClearCountCorrectParam", 0x80, correct, g_log)) {
            if (correct.file) state.attempted = true;
            return;
        }
        for (const std::uint32_t base : {0u, 100u})
            for (std::uint32_t cycle = 2; cycle <= kJourneyMax; ++cycle) {
                const std::uintptr_t row = param_row_by_id(correct, base + cycle);
                float rate{};
                if (row && game::read(row + kNgPoiseOffset, rate) && rate > 0.0f && rate < 1.0f)
                    state.cells.push_back({row + kNgPoiseOffset, rate});
            }
        wchar_t message[120]{};
        swprintf_s(message, L"New Game+ poise: %zu cycle rows found.", state.cells.size());
        log(message);
        if (state.cells.empty()) {
            state.attempted = true;
            return;
        }
        state.ready = true;
    }
    const int want = value_of(ng_poise);
    if (want == state.applied) return;
    for (const auto& [address, original] : state.cells)
        game::write<float>(address, 1.0f - (1.0f - original) * static_cast<float>(want) / 100.0f);
    state.applied = want;
}

// Drop or discard any item (Nexus request, FJKarl: items that pile up and
// cannot be removed). isDrop and isDiscard are bits in the five EquipParam
// tables (offsets from the Smithbox ER defs, regulation 2.7.2.0; the row
// sizes match the ones used above). The first option sets both bits on every
// item except key items (goodsType 1) and Great Runes (15); the second adds
// those too, and flasks: the menu offers no Leave or Discard for an
// isAutoEquip item, so that bit is cleared while it is on. The bare body
// rows are left alone. Turning them off puts each bit back as the game had it.
struct DropBitsCell {
    std::uintptr_t address;
    std::uint8_t mask;      // isDrop | isDiscard in that byte (or isAutoEquip)
    std::uint8_t original;  // the game's bits under mask
    std::uint8_t unlocked;  // the bits under mask while the option is on
    bool key;               // key item, Great Rune or flask
};
struct DropAnyState {
    bool attempted{};
    bool ready{};
    std::vector<DropBitsCell> cells{};
    int applied = -1;
};
DropAnyState g_drop_any{};

void sync_drop_any() noexcept {
    auto& state = g_drop_any;
    if (state.attempted) return;
    if (!state.ready) {
        struct Table {
            const wchar_t* name;
            std::size_t row_size;
            std::size_t drop_byte;
            std::uint8_t drop_bit;
            std::size_t discard_byte;
            std::uint8_t discard_bit;
        };
        static constexpr Table kTables[]{
            {L"EquipParamGoods", 0xB0, 0x6F, 0, 0x49, 3},
            {L"EquipParamWeapon", 0x298, 0x109, 2, 0x109, 1},
            {L"EquipParamProtector", 0x1A0, 0xE3, 1, 0xE3, 0},
            {L"EquipParamAccessory", 0x60, 0x40, 4, 0x40, 3},
            {L"EquipParamGem", 0x60, 0x34, 1, 0x34, 0},
        };
        std::size_t tables{};
        std::size_t keys{};
        state.cells.clear();  // a pass that stopped at an unloaded table starts over
        for (const Table& table : kTables) {
            game::ParamTable param;
            if (!game::find_param(g_statics, table.name, table.row_size, param, g_log)) {
                if (!param.file) return;  // regulation not loaded yet: try again later
                continue;
            }
            ++tables;
            const bool goods = table.row_size == 0xB0;
            // the bare head, body, arms and legs are not items
            std::uintptr_t bare[4]{};
            if (table.row_size == 0x1A0)
                for (std::uint32_t i = 0; i < 4; ++i) bare[i] = param_row_by_id(param, 10000 + i * 100);
            for (const std::uintptr_t row : param.rows) {
                if (!row || std::find(std::begin(bare), std::end(bare), row) != std::end(bare)) continue;
                bool key = false;
                if (goods) {
                    std::uint8_t type{};
                    std::uint8_t auto_equip{};
                    if (game::read(row + 0x3E, type)) key = type == 1 || type == 15;
                    // Flasks carry isAutoEquip (+0x49 bit 0): the menu offers no
                    // Leave or Discard for them even with both bits set, so with
                    // key items on that bit is cleared too. The menu still keeps
                    // the last flask; clearing isSuppleItem (bit 7) did not
                    // change that (r156-test10), so it is left alone.
                    if (game::read(row + 0x49, auto_equip) && (auto_equip & 1)) {
                        key = true;
                        state.cells.push_back({row + 0x49, 1, 1, 0, true});
                    }
                    if (key) ++keys;
                }
                const auto add = [&](std::size_t offset, std::uint8_t mask) {
                    std::uint8_t value{};
                    if (!game::read(row + offset, value)) return;
                    if ((value & mask) == mask) return;  // already allowed
                    state.cells.push_back({row + offset, mask, static_cast<std::uint8_t>(value & mask), mask, key});
                };
                if (table.drop_byte == table.discard_byte) {
                    add(table.drop_byte, static_cast<std::uint8_t>((1u << table.drop_bit) | (1u << table.discard_bit)));
                } else {
                    add(table.drop_byte, static_cast<std::uint8_t>(1u << table.drop_bit));
                    add(table.discard_byte, static_cast<std::uint8_t>(1u << table.discard_bit));
                }
            }
        }
        wchar_t message[180]{};
        swprintf_s(message, L"Drop any item: %zu locked drop/discard flags in %zu of 5 equipment params (%zu key items or Great Runes).",
            state.cells.size(), tables, keys);
        log(message);
        if (tables == 0) {
            state.attempted = true;
            return;
        }
        state.ready = true;
    }
    const int want = value_of(drop_any) ? (value_of(drop_key_items) ? 2 : 1) : 0;
    if (want == state.applied) return;
    for (const auto& cell : state.cells) {
        std::uint8_t value{};
        if (!game::read(cell.address, value)) continue;
        const bool unlock = want == 2 || (want == 1 && !cell.key);
        const std::uint8_t bits = unlock ? cell.unlocked : cell.original;
        game::write<std::uint8_t>(cell.address, static_cast<std::uint8_t>((value & ~cell.mask) | bits));
    }
    state.applied = want;
}

// Item shop (r157 test). Every named item gets a free, unlimited row in an
// enlarged ShopLineupParam, and each category opens as a shop with the
// talk command OpenRegularShop(first, last) (msa\src\game_menus.rs). The
// vanilla table has 1296 rows, so the Hub builds a new PARAM file (header,
// row table, data, type name) with the original rows plus its own, ids
// 1700000-2499999 (unused in vanilla), and points the table's holder at it
// before any other option reads the table. Names come from the game's
// MsgRepository (LookupEntry, categories patch / base / DLC1 / DLC2).
using MsgLookupFn = const wchar_t*(__fastcall*)(void*, std::uint32_t, std::uint32_t, std::int32_t);
MsgLookupFn g_msg_lookup{};
void** g_msg_repository{};
std::uint8_t* g_goods_name_getter{};  // GetGoodsName(repo, id), 0xD13430 in 2.7.2.0

constexpr const char* kMsgLookupPattern =
    "3b 51 10 73 29 44 3b 41 14 73 23 48 8b 41 08 8b d2 48 8b 0c d0 48 85 c9 74 14 41 8b c0 48 8b 0c c1 48 85 c9 74 08 41 8b d1 e9";
// mov rcx,[MsgRepository]; mov edx,ebx; call GetGoodsName; add rsp,30h; pop rbx; ret
constexpr const char* kMsgRepositoryPattern = "48 8b 0d ? ? ? ? 8b d3 e8 ? ? ? ? 48 83 c4 30 5b c3";

// LookupEntry is found through the goods-name getter that the repository
// pattern calls (its first call is LookupEntry), not by its own prologue:
// other mods hook it (NPC Spawner), and calling a hooked entry still works.
void resolve_msg_lookup() noexcept {
    std::uint8_t* repo = find_unique(parse_pattern(kMsgRepositoryPattern));
    if (!repo) {
        log(L"Vanilla site 'message repository' not found (or ambiguous); the item shop is unavailable.");
        return;
    }
    std::int32_t disp{};
    std::memcpy(&disp, repo + 3, 4);
    void** repository = reinterpret_cast<void**>(repo + 7 + disp);
    std::int32_t rel{};
    std::memcpy(&rel, repo + 10, 4);
    std::uint8_t* getter = repo + 14 + rel;
    std::uint8_t* lookup = nullptr;
    // getter: mov [rsp+8],rbx; push rdi; sub rsp,20h; ...; lea r8d,[rdx+6Fh]; call LookupEntry
    for (int i = 0; i < 0x30 && !lookup; ++i) {
        if (getter[i] == 0xE8 && i >= 4 && getter[i - 4] == 0x44 && getter[i - 3] == 0x8D) {
            std::memcpy(&rel, getter + i + 1, 4);
            lookup = getter + i + 5 + rel;
        }
    }
    const auto base = reinterpret_cast<std::uint8_t*>(game::image());
    bool plausible = false;
    if (lookup) {
        // the original entry, or another mod's jump on it
        const std::vector<int> original = parse_pattern(kMsgLookupPattern);
        bool same = true;
        for (std::size_t i = 0; i < original.size() && same; ++i)
            same = original[i] < 0 || lookup[i] == static_cast<std::uint8_t>(original[i]);
        plausible = same || lookup[0] == 0xE9 || (lookup[0] == 0xFF && lookup[1] == 0x25);
    }
    if (!plausible) {
        log(L"Vanilla site 'message lookup' not found; the item shop is unavailable.");
        return;
    }
    g_msg_repository = repository;
    g_msg_lookup = reinterpret_cast<MsgLookupFn>(lookup);
    g_goods_name_getter = getter;
    wchar_t message[160]{};
    swprintf_s(message, L"Vanilla message lookup at eldenring.exe+0x%llX%s (MsgRepository at +0x%llX).",
        static_cast<unsigned long long>(lookup - base), lookup[0] == 0x3B ? L"" : L", hooked by another mod",
        static_cast<unsigned long long>(reinterpret_cast<std::uint8_t*>(repository) - base));
    log(message);
}

const wchar_t* msg_lookup_guarded(void* repo, std::uint32_t category, std::int32_t id) noexcept {
    __try {
        return g_msg_lookup(repo, 0, category, id);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return nullptr;
    }
}

bool item_name(std::initializer_list<std::uint32_t> categories, std::int32_t id, std::wstring& out,
    int* found = nullptr) noexcept {
    if (!g_msg_lookup || !g_msg_repository) return false;
    void* repo = *g_msg_repository;
    if (!repo) return false;
    int index = -1;
    for (const std::uint32_t category : categories) {
        ++index;
        const wchar_t* text = msg_lookup_guarded(repo, category, id);
        if (!text || !text[0]) continue;
        std::size_t length = 0;
        while (length < 128 && text[length]) ++length;
        std::wstring name(text, length);
        if (name.empty() || name[0] == L'?' || name == L"NULL" || name.rfind(L"[ERROR]", 0) == 0) continue;
        out = std::move(name);
        if (found) *found = static_cast<int>(category >= 300 ? 2 : 1);  // 3xx/4xx: Shadow of the Erdtree
        return true;
    }
    return false;
}

// Shop groups, in menu order, inside one shop per section (base game and
// Shadow of the Erdtree apart). Sections are the list of F11 > Item Shop:
// shields, staves and seals sit with the weapons and notes with the key
// items, close to Glorious Merchant's layout; the game's "item type" sort
// puts a gap between groups. Shields/Staves/Notes sections stay empty.
enum ShopSection : int {
    sec_weapons, sec_shields, sec_catalysts, sec_ammo, sec_armour, sec_talismans, sec_spirits,
    sec_sorceries, sec_incantations, sec_gems, sec_items, sec_materials, sec_key, sec_notes, sec_tarnished, sec_count
};
struct ShopText { const wchar_t* en; const wchar_t* es; const wchar_t* zh; };
constexpr ShopText kShopSections[sec_count]{
    {L"Weapons", L"Armas", L"武器"},
    {L"Shields", L"Escudos", L"盾牌"},
    {L"Staves and seals", L"Bastones y sellos", L"手杖与圣印记"},
    {L"Arrows and bolts", L"Flechas y virotes", L"箭矢与弩箭"},
    {L"Armour sets", L"Conjuntos de armadura", L"防具套装"},
    {L"Talismans", L"Talismanes", L"护符"},
    {L"Spirit ashes", L"Cenizas de espíritu", L"骨灰"},
    {L"Sorceries", L"Hechizos", L"魔法"},
    {L"Incantations", L"Encantamientos", L"祷告"},
    {L"Ashes of War", L"Cenizas de guerra", L"战灰"},
    {L"Consumables", L"Consumibles", L"消耗品"},
    {L"Materials", L"Materiales", L"材料"},
    {L"Key items and notes", L"Objetos clave y notas", L"重要道具与笔记"},
    {L"Notes and paintings", L"Notas y cuadros", L"笔记与画作"},
    {L"Tarnished Edition", L"Tarnished Edition", L"褪色者版"},
};
struct ShopGroupDef { ShopSection section; ShopText text; };
constexpr ShopGroupDef kShopGroups[]{
    // 0-35 weapons by wepType (kShopWeaponTypes below)
    {sec_weapons, {L"Daggers", L"Dagas", L"短剑"}},
    {sec_weapons, {L"Straight swords", L"Espadas rectas", L"直剑"}},
    {sec_weapons, {L"Light greatswords", L"Espadones ligeros", L"轻型大剑"}},
    {sec_weapons, {L"Greatswords", L"Espadones", L"大剑"}},
    {sec_weapons, {L"Colossal swords", L"Espadas colosales", L"特大剑"}},
    {sec_weapons, {L"Thrusting swords", L"Espadas perforantes", L"刺剑"}},
    {sec_weapons, {L"Heavy thrusting swords", L"Espadas perforantes pesadas", L"重刺剑"}},
    {sec_weapons, {L"Curved swords", L"Espadas curvas", L"曲剑"}},
    {sec_weapons, {L"Curved greatswords", L"Espadones curvos", L"大曲剑"}},
    {sec_weapons, {L"Backhand blades", L"Hojas invertidas", L"逆刃刀"}},
    {sec_weapons, {L"Katanas", L"Katanas", L"刀"}},
    {sec_weapons, {L"Great katanas", L"Grandes katanas", L"大太刀"}},
    {sec_weapons, {L"Twinblades", L"Espadas dobles", L"双头剑"}},
    {sec_weapons, {L"Axes", L"Hachas", L"斧"}},
    {sec_weapons, {L"Greataxes", L"Grandes hachas", L"大斧"}},
    {sec_weapons, {L"Hammers", L"Martillos", L"槌"}},
    {sec_weapons, {L"Flails", L"Mayales", L"连枷"}},
    {sec_weapons, {L"Great hammers", L"Grandes martillos", L"大槌"}},
    {sec_weapons, {L"Colossal weapons", L"Armas colosales", L"特大武器"}},
    {sec_weapons, {L"Spears", L"Lanzas", L"矛"}},
    {sec_weapons, {L"Great spears", L"Grandes lanzas", L"大矛"}},
    {sec_weapons, {L"Halberds", L"Alabardas", L"斧枪"}},
    {sec_weapons, {L"Reapers", L"Guadañas", L"镰刀"}},
    {sec_weapons, {L"Whips", L"Látigos", L"鞭子"}},
    {sec_weapons, {L"Fists", L"Puños", L"拳头"}},
    {sec_weapons, {L"Hand-to-hand arts", L"Artes cuerpo a cuerpo", L"格斗术"}},
    {sec_weapons, {L"Claws", L"Garras", L"钩爪"}},
    {sec_weapons, {L"Beast claws", L"Garras de bestia", L"兽爪"}},
    {sec_weapons, {L"Perfume bottles", L"Frascos de perfume", L"香水瓶"}},
    {sec_weapons, {L"Throwing blades", L"Hojas arrojadizas", L"投掷刃"}},
    {sec_weapons, {L"Torches", L"Antorchas", L"火把"}},
    {sec_weapons, {L"Light bows", L"Arcos ligeros", L"小弓"}},
    {sec_weapons, {L"Bows", L"Arcos", L"长弓"}},
    {sec_weapons, {L"Greatbows", L"Grandes arcos", L"大弓"}},
    {sec_weapons, {L"Crossbows", L"Ballestas", L"弩"}},
    {sec_weapons, {L"Ballistas", L"Balistas", L"弩炮"}},
    // 36-39 shields
    {sec_weapons, {L"Small shields", L"Escudos pequeños", L"小盾"}},
    {sec_weapons, {L"Medium shields", L"Escudos medianos", L"中盾"}},
    {sec_weapons, {L"Greatshields", L"Escudos grandes", L"大盾"}},
    {sec_weapons, {L"Thrusting shields", L"Escudos perforantes", L"刺盾"}},
    // 40-41 catalysts
    {sec_weapons, {L"Glintstone staves", L"Bastones de piedra brillante", L"手杖"}},
    {sec_weapons, {L"Sacred seals", L"Sellos sagrados", L"圣印记"}},
    // 42-45 ammunition
    {sec_ammo, {L"Arrows", L"Flechas", L"箭"}},
    {sec_ammo, {L"Greatarrows", L"Grandes flechas", L"大箭"}},
    {sec_ammo, {L"Bolts", L"Virotes", L"弩箭"}},
    {sec_ammo, {L"Ballista bolts", L"Virotes de balista", L"弩炮箭"}},
    // 46-51
    {sec_armour, {L"Armour sets", L"Conjuntos de armadura", L"防具套装"}},
    {sec_talismans, {L"Talismans", L"Talismanes", L"护符"}},
    {sec_spirits, {L"Spirit ashes", L"Cenizas de espíritu", L"骨灰"}},
    {sec_sorceries, {L"Sorceries", L"Hechizos", L"魔法"}},
    {sec_incantations, {L"Incantations", L"Encantamientos", L"祷告"}},
    {sec_gems, {L"Ashes of War", L"Cenizas de guerra", L"战灰"}},
    // 52-60 items
    {sec_items, {L"Flasks and physick", L"Frascos y physick", L"圣杯瓶与露滴"}},
    {sec_items, {L"Crystal tears", L"Lágrimas de cristal", L"结晶露滴"}},
    {sec_items, {L"Food and boluses", L"Comida y bolos", L"食物与药丸"}},
    {sec_items, {L"Pots", L"Vasijas", L"壶"}},
    {sec_items, {L"Aromatics and spraymists", L"Aromáticos y vaporizadores", L"香料与喷雾"}},
    {sec_items, {L"Throwing items", L"Objetos arrojadizos", L"投掷物"}},
    {sec_items, {L"Greases", L"Grasas", L"油脂"}},
    {sec_items, {L"Tools and other items", L"Herramientas y otros", L"工具与其他"}},
    {sec_items, {L"Runes", L"Runas", L"卢恩"}},
    // 61-68 materials
    {sec_materials, {L"Crafting: animal parts", L"Fabricación: partes de animales", L"制作：动物素材"}},
    {sec_materials, {L"Crafting: plants", L"Fabricación: plantas", L"制作：植物素材"}},
    {sec_materials, {L"Crafting: stones and others", L"Fabricación: piedras y otros", L"制作：石头与其他"}},
    {sec_materials, {L"Smithing stones", L"Piedras de forja", L"锻造石"}},
    {sec_materials, {L"Somber smithing stones", L"Piedras de forja sombrías", L"失色锻造石"}},
    {sec_materials, {L"Gloveworts", L"Glovewort", L"手套花"}},
    {sec_materials, {L"Seeds, tears and fragments", L"Semillas, lágrimas y fragmentos", L"种子、露滴与碎片"}},
    {sec_materials, {L"Other upgrade materials", L"Otros materiales de mejora", L"其他强化材料"}},
    // 69-77 key items
    {sec_key, {L"Keys", L"Llaves", L"钥匙"}},
    {sec_key, {L"Quest items", L"Objetos de misión", L"任务道具"}},
    {sec_key, {L"Cookbooks", L"Recetarios", L"制作笔记"}},
    {sec_key, {L"Maps", L"Mapas", L"地图"}},
    {sec_key, {L"Bell bearings", L"Campanas", L"铃珠"}},
    {sec_key, {L"Scrolls and prayerbooks", L"Pergaminos y libros de plegarias", L"卷轴与祷告书"}},
    {sec_key, {L"Remembrances", L"Recuerdos", L"追忆"}},
    {sec_key, {L"Great Runes", L"Grandes Runas", L"大卢恩"}},
    {sec_key, {L"Other key items", L"Otros objetos clave", L"其他重要道具"}},
    // 78-80 notes
    {sec_key, {L"Paintings", L"Cuadros", L"画作"}},
    {sec_key, {L"Notes and messages", L"Notas y mensajes", L"笔记与讯息"}},
    {sec_key, {L"Tutorials", L"Tutoriales", L"教学"}},
    // 81 the Tarnished Edition's own items, in their own shop (user, r157-test26)
    {sec_tarnished, {L"Tarnished Edition", L"Tarnished Edition", L"褪色者版"}},
};
constexpr int kShopTarnished = 81;
constexpr int kShopGroupCount = static_cast<int>(sizeof(kShopGroups) / sizeof(kShopGroups[0]));
// wepType of groups 0-45 (EquipParamWeapon +0x1A6), in group order
constexpr std::uint16_t kShopWeaponTypes[46]{
    1, 3, 93, 5, 7, 15, 16, 9, 11, 92, 13, 94, 14, 17, 19, 21, 24, 23, 41, 25, 28, 29, 31, 39, 35, 88, 37, 95, 89, 91, 87,
    50, 51, 53, 55, 56, 65, 67, 69, 90, 57, 61, 81, 83, 85, 86};
constexpr int kShopArmour = 46, kShopTalismans = 47, kShopSpirits = 48, kShopSorceries = 49,
    kShopIncantations = 50, kShopGems = 51;
constexpr std::int32_t kShopRange = 4000;  // rows per shop (section x 2 + DLC)

int shop_goods_group(std::uint8_t type, std::uint8_t sort_group, const std::wstring& name) noexcept {
    switch (type) {
    case 5: case 17: return kShopSorceries;
    case 16: case 18: return kShopIncantations;
    case 7: case 8: return kShopSpirits;
    case 9: return 52;
    case 10: return 53;
    case 11: return 55;
    case 3: return 75;
    case 15: return 76;
    case 2:
        if (sort_group == 10 || sort_group == 15) return 61;
        if (sort_group == 20 || sort_group == 25) return 62;
        return 63;
    case 14:
        if (sort_group == 20 || sort_group == 25) return 64;
        if (sort_group == 30 || sort_group == 35) return 65;
        if (sort_group == 40 || sort_group == 45) return 66;
        if (sort_group == 10 || sort_group == 15) return 67;
        return 68;
    case 12:
        if (sort_group == 20 || sort_group == 25) return 80;
        if (name.find(L"Painting") != std::wstring::npos) return 78;
        return 79;
    case 1:
        switch (sort_group) {
        case 10: case 15: return 76;
        case 40: case 45: return 69;
        case 50: case 55: return 70;
        case 60: case 65: return 71;
        case 70: case 75: return 72;
        case 80: case 85: case 90: case 95: return 73;
        case 100: case 105: return 74;
        default: return 77;
        }
    case 0:
        switch (sort_group) {
        case 10: case 15: return 52;
        case 20: case 25: return 54;
        case 30: case 35: return 55;
        case 40: case 45: return 56;
        case 50: case 55: return 57;
        case 70: case 75: return 58;
        case 100: case 105: return 60;
        case 120: case 125: case 250: case 255: return -1;  // multiplayer items and gestures
        default: return 59;
        }
    default: return -1;
    }
}

struct ItemShopState {
    bool attempted{};
    bool ready{};
    int tries{};
    std::int32_t first[sec_count][2]{};  // one shop per section and base / DLC
    std::int32_t count[sec_count][2]{};  // rows, gaps included
    std::int32_t items[sec_count][2]{};  // items only
};
ItemShopState g_item_shop{};
void register_item_shop_talk() noexcept;

bool find_param_holder(const wchar_t* name, std::uintptr_t& slot) noexcept {
    std::uintptr_t manager{};
    if (!g_statics.regulation_manager || !game::read(g_statics.regulation_manager, manager) || !manager) return false;
    std::uintptr_t begin{}, end{};
    if (!game::read(manager + 0x18, begin) || !game::read(manager + 0x20, end) || !begin || end <= begin) return false;
    const std::size_t want = std::wcslen(name);
    for (std::uintptr_t s = begin; s < end; s += 8) {
        std::uintptr_t entry{};
        if (!game::read(s, entry) || !entry) continue;
        std::uint64_t length{};
        if (!game::read(entry + 0x28, length) || length != want) continue;
        wchar_t text[65]{};
        if (length <= 7) {
            if (!game::safe_read(entry + 0x18, text, length * 2)) continue;
        } else {
            std::uintptr_t heap{};
            if (!game::read(entry + 0x18, heap) || !game::safe_read(heap, text, length * 2)) continue;
        }
        if (std::wcsncmp(text, name, want) != 0) continue;
        std::uintptr_t holder{};
        if (!game::read(entry + 0x80, holder) || !holder) return false;
        slot = holder + 0x80;
        return true;
    }
    return false;
}


// Paid add-ons: items of a DLC the player does not own are left out, even
// though their data is in every copy of the game (r157-test16: the Idus
// Sword showed without the Tarnished Edition). Steam's flat API from the
// game's own steam_api64.dll; when it cannot answer, the add-on counts as
// not owned. Tarnished Pack items go with the base game (user's choice).
constexpr std::uint32_t kSteamShadowOfTheErdtree = 2778580;
constexpr std::uint32_t kSteamTarnishedPack = 3655690;
constexpr std::int32_t kTarnishedWeapons[]{3560000, 8530000, 13510000, 31540000, 62520000, 64530000, 66530000, 67530000};
constexpr std::int32_t kTarnishedArmour[]{5340000, 5340100, 5340200, 5340300, 5350000, 5350100, 5350200, 5350300, 5351100,
    5360000, 5360100, 5360200, 5360300, 5361000, 5370000, 5370100, 5370200, 5370300};
constexpr std::int32_t kTarnishedGoods[]{2009600, 2009610, 2009620};

template <std::size_t N>
bool in_list(const std::int32_t (&list)[N], std::int32_t id) noexcept {
    return std::find(std::begin(list), std::end(list), id) != std::end(list);
}

// Owned when Steam says the DLC is installed or, failing that, that the
// account owns it (BIsSubscribedApp): a pack that is bought but unticked in
// the game's DLC list reads as "not installed" (Nexus report, FJKarl: his
// Tarnished Pack section did not show). Each answer is logged once.
int steam_dlc_owned(std::uint32_t app) noexcept {  // 1 yes, 0 no, -1 unknown
    using AppsFn = void*(__cdecl*)();
    using AskFn = bool(__cdecl*)(void*, std::uint32_t);
    const HMODULE steam = GetModuleHandleW(L"steam_api64.dll");
    if (!steam) return -1;
    AppsFn apps = nullptr;
    for (const char* name : {"SteamAPI_SteamApps_v008", "SteamAPI_SteamApps_v009"})
        if (!apps) apps = reinterpret_cast<AppsFn>(GetProcAddress(steam, name));
    const auto installed = reinterpret_cast<AskFn>(GetProcAddress(steam, "SteamAPI_ISteamApps_BIsDlcInstalled"));
    const auto subscribed = reinterpret_cast<AskFn>(GetProcAddress(steam, "SteamAPI_ISteamApps_BIsSubscribedApp"));
    if (!apps || !installed) return -1;
    void* interface_ = apps();
    if (!interface_) return -1;
    const bool is_installed = installed(interface_, app);
    const bool is_owned = is_installed || (subscribed && subscribed(interface_, app));
    // Logged when the answer changes (the shop asks again while it waits).
    static std::uint32_t logged_app[2]{};
    static int logged_answer[2]{-2, -2};
    const int slot = app == kSteamTarnishedPack ? 1 : 0;
    const int answer = (is_installed ? 2 : 0) | (is_owned ? 1 : 0);
    if (logged_app[slot] != app || logged_answer[slot] != answer) {
        logged_app[slot] = app;
        logged_answer[slot] = answer;
        wchar_t message[160]{};
        swprintf_s(message, L"Steam: app %u installed %d, owned %d%ls.", app, is_installed ? 1 : 0, is_owned ? 1 : 0,
            subscribed ? L"" : L" (no ownership check in this steam_api64.dll)");
        log(message);
    }
#ifdef NT_DEV_ALL_DLC
    // Local test builds only (the author's, defined when compiling them): the
    // shop shows every add-on's items so they can be checked without owning
    // them. Never in the repository's default build or a released package.
    if (!is_owned) {
        static bool told{};
        if (!told) log(L"Steam: development build, add-on items shown without the add-on.");
        told = true;
        return 1;
    }
#endif
    return is_owned ? 1 : 0;
}

struct ShopItem {
    std::int32_t row_id;
    std::int32_t equip_id;
    std::uint8_t equip_type;
    std::int32_t price;
    bool spacer;
    std::int32_t header;  // -1, or the group whose title this row shows
    std::int32_t sell{};
    std::uint8_t max_level{};
};
// Group titles in the shop: a row of a harmless note (Note: Hidden Cave,
// goods 8700) whose nameMsgId is kShopHeaderMsg + group, at a price no one
// can pay. The game reads that name through GetGoodsName (0xD13430 in
// 2.7.2.0), which install_shop_header_names hooks for those ids only.
constexpr std::int32_t kShopHeaderGoods = 8700;
constexpr std::int32_t kShopHeaderMsg = 1992000000;
constexpr std::int32_t kShopHeaderPrice = 999999999;
struct ShopPending {
    int group;
    int dlc;  // 0 base game, 1 Shadow of the Erdtree
    std::int32_t equip_id;
    std::uint8_t equip_type;
    std::int32_t price;
    std::uintptr_t row;  // the item's EquipParam row
    std::uint8_t max_level;  // weapons 0/10/25, spirit ashes 0-10
    int kind;  // the group it would have outside Tarnished Edition (same as group there)
};
// Our shop rows, to set level and price when a shop opens (options
// ItemShopWeaponLevel / ItemShopSpiritLevel / ItemShopPricePercent).
struct ShopRowRef {
    std::uint8_t* data;       // the ShopLineupParam row
    std::int32_t base_id;     // +0 item
    std::int32_t sell;        // its sell value
    std::uint8_t kind;        // 0 other, 1 weapon, 2 spirit ash
    std::uint8_t max_level;
};
std::vector<ShopRowRef> g_shop_rows[sec_count][2];

// Armour sets one per line (user, r157-test16): the shop starts a new line
// whenever sortGroupId changes and orders by sortId, so while one of our
// armour shops is open every set gets its own sortGroupId (Protector +0xD8,
// u8, 1-254 in set order) and sortId = set x 10 + slot (+0x4: head, body,
// arms, legs). The game's values come back once no menu is open any more
// (nt_top_menu_job, msa\src\game_menus.rs).
struct ArmourLayoutCell {
    std::uintptr_t row;
    std::int32_t sort_id;  // ours
    std::uint8_t sort_group;
    std::int32_t original_sort_id;
    std::uint8_t original_sort_group;
    std::uint8_t original_slot;  // protectorCategory; 0xFF: not armour
    std::uint16_t sort_offset;   // sortId (s32) in the row
    std::uint16_t group_offset;  // sortGroupId (u8)
};
// Lines per section while one of our shops is open (r157-test22): weapons
// by type, normal ones (gemMountType +0x241 == 2) before unique ones;
// Ashes of War by the affinity they give (defaultWepAttr +0x35); spirit
// ashes FP-cost then HP-cost, cheapest first; sorceries and incantations
// attack then support. Talismans, consumables and materials keep the
// game's own groups.
std::vector<ArmourLayoutCell> g_shop_layout[sec_count][2];
std::atomic<bool> g_armour_layout_applied{false};
std::atomic<int> g_armour_layout_generation{0};

// Damage taken (r157, Nexus report 08-11): humanoid NPCs and invaders attack
// with AtkParam_Pc, the player's own attack rows, so scaling AtkParam_Npc
// missed them, and Damage dealt made them hit harder. Now the player carries
// a SpEffect of our own, kept on by msa (nt_keep_player_effect), whose
// defEnemy/defPlayerDmgCorrectRate (+0x260..+0x284: physical, magic, fire,
// lightning, holy) scale every hit the player takes from anyone, as the
// Dragoncrest talismans do (their row 340000 is the template). The row is
// appended to SpEffectParam with an id above the game's (999999999), the same
// way the item shop grows ShopLineupParam; the old table stays allocated.
constexpr std::int32_t kDamageCutEffectId = 1999990100;
constexpr std::int32_t kDamageCutTemplateId = 340000;  // Dragoncrest Shield Talisman
constexpr std::size_t kSpEffectRowSize = 0x390;
extern "C" std::uint8_t nt_keep_player_effect(std::int32_t id);

struct DamageCutState {
    bool attempted{};
    bool ready{};
    std::uint8_t* row{};
    float applied[10]{-1.0f, -1.0f, -1.0f, -1.0f, -1.0f, -1.0f, -1.0f, -1.0f, -1.0f, -1.0f};
    std::int32_t kept{-1};  // id last handed to msa, -1 none yet
    bool keep_pending{};
    int generation{};  // g_param_appends when row was found
};
DamageCutState g_damage_cut{};

// A rebuilt param table must come from the game's own allocator: the game
// owns the table and frees it when it exits, with its CRT's _aligned_free
// (2.7.2.0 0x25234E8: reads the real block at ptr-8). A VirtualAlloc block
// crashed every exit from the world (0xC0000005 at +0x25234F5, 21-11).
// Its _aligned_malloc(size, alignment) follows right after (0x2523504);
// both are unique in 2.7.0.0 to 2.7.2.1.
// That only matches while nobody replaces the game's heaps: me3's mem_patch
// (Reforged; MO2's me3 compat) turns them into mimalloc, and its free then
// crashed on our CRT block at exit (review-er-plugin, 21-11). The game frees
// any block through the DLAllocator that owns it (2.7.2.0 0x1EBD608: owner =
// 0x1EC9340(block), then owner->vtable[0x68] deallocate), so the new table is
// allocated by the owner of the table it replaces: vtable[0x50]
// allocate_aligned(size, 16). The CRT pair stays as the fallback. The lookup
// call is unique in 2.7.0.0 to 2.7.2.1 too.
using GameAlignedMalloc = void* (*)(std::size_t, std::size_t);
using GameAlignedFree = void (*)(void*);
using GameAllocatorOf = void* (*)(const void*);
using DlAllocateAligned = void* (*)(void*, std::size_t, std::size_t);
using DlDeallocate = void (*)(void*, void*);
GameAlignedMalloc g_game_aligned_malloc{};
GameAlignedFree g_game_aligned_free{};
GameAllocatorOf g_game_allocator_of{};
bool g_game_heap_searched{};

void search_game_heap() noexcept {
    if (g_game_heap_searched) return;
    g_game_heap_searched = true;
    std::uint8_t* hit = find_unique(parse_pattern(
        "48 83 ec 28 48 85 c9 74 0d 48 83 e1 f8 48 8b 49 f8 e8 ? ? ? ? 48 83 c4 28 c3 ? "
        "48 89 5c 24 08 57 48 83 ec 20 33 db 48 85 d2 74 ? 48 8d 42 ff 48 85 c2 75 ? "
        "8d 43 08 48 3b d0 48 0f 47 c2 48 8d 78 ff 48 8d 41 08 48 03 c7"));
    if (hit) {
        g_game_aligned_free = reinterpret_cast<GameAlignedFree>(hit);
        g_game_aligned_malloc = reinterpret_cast<GameAlignedMalloc>(hit + 0x1C);
    }
    std::uint8_t* owner = find_unique(parse_pattern(
        "48 8b ce e8 ? ? ? ? 48 8b d8 48 85 c0 75 ? 4c 8d 05 ? ? ? ? 8d 50 64 48 8d 0d ? ? ? ? "
        "e8 ? ? ? ? 4c 8b 03 48 8b d6 48 8b cb 41 ff 50 68"));
    if (owner) {
        std::int32_t rel{};
        std::memcpy(&rel, owner + 4, 4);
        g_game_allocator_of = reinterpret_cast<GameAllocatorOf>(owner + 8 + rel);
    }
    wchar_t message[200]{};
    swprintf_s(message, L"Param tables: the game's allocator lookup %ls, aligned allocator %ls.",
        owner ? L"found" : L"not found", hit ? L"found" : L"not found");
    log(message);
}

void* allocator_of(const void* block) noexcept {
    if (!g_game_allocator_of || !block) return nullptr;
    __try {
        return g_game_allocator_of(block);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return nullptr;
    }
}

void* call_allocate_aligned(void* allocator, std::size_t size) noexcept {
    __try {
        const auto vtable = *static_cast<void***>(allocator);
        return reinterpret_cast<DlAllocateAligned>(vtable[0x50 / 8])(allocator, size, 16);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return nullptr;
    }
}

// `original` is the block of the table being replaced (its file - 16).
std::uint8_t* game_table_alloc(std::size_t size, const void* original) noexcept {
    search_game_heap();
    std::uint8_t* block = nullptr;
    if (void* allocator = allocator_of(original)) {
        block = static_cast<std::uint8_t*>(call_allocate_aligned(allocator, size));
        // The game must find the same owner for the new block.
        if (block && allocator_of(block) != allocator) {
            __try {
                reinterpret_cast<DlDeallocate>((*static_cast<void***>(allocator))[0x68 / 8])(allocator, block);
            } __except (EXCEPTION_EXECUTE_HANDLER) {
            }
            block = nullptr;
        }
    }
    const bool from_owner = block != nullptr;
    if (!block && g_game_aligned_malloc) block = static_cast<std::uint8_t*>(g_game_aligned_malloc(size, 16));
    if (block) std::memset(block, 0, size);
    wchar_t message[160]{};
    swprintf_s(message, L"Param tables: %zu bytes from %ls.", size,
        from_owner ? L"the allocator of the table it replaces" : block ? L"the game's CRT" : L"nowhere (not extended)");
    log(message);
    return block;
}

void game_table_free(std::uint8_t* block) noexcept {
    if (!block) return;
    if (void* allocator = allocator_of(block)) {
        __try {
            reinterpret_cast<DlDeallocate>((*static_cast<void***>(allocator))[0x68 / 8])(allocator, block);
            return;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
        }
    }
    if (g_game_aligned_free) g_game_aligned_free(block);
}

// Appends rows (ids ascending, above every existing one) to a param table
// that keeps its rows sorted, rebuilding the table with its prefix and id
// index once; returns the new rows' data, or nothing.
std::vector<std::uint8_t*> append_param_rows(const wchar_t* name, std::size_t row_size,
    const std::vector<std::pair<std::int32_t, const std::uint8_t*>>& added, bool& retry) noexcept {
    retry = false;
    std::vector<std::uint8_t*> result;
    if (added.empty()) return result;
    for (std::size_t i = 1; i < added.size(); ++i)
        if (added[i].first <= added[i - 1].first) return result;
    std::uintptr_t slot{}, file{};
    if (!find_param_holder(name, slot) || !game::read(slot, file) || !file) {
        retry = true;
        return result;
    }
    std::uint16_t count{};
    std::uint32_t type_offset32{};
    std::uint64_t type_offset{}, data_start{};
    std::uint8_t prefix[16]{};
    if (!game::read(file + 0x0A, count) || !game::read(file + 0x00, type_offset32) ||
        !game::read(file + 0x10, type_offset) || !game::read(file + 0x30, data_start) ||
        !game::safe_read(file - 16, prefix, sizeof(prefix))) {
        retry = true;
        return result;
    }
    std::uint32_t old_size{}, old_index_count{};
    std::memcpy(&old_size, prefix, 4);
    std::memcpy(&old_index_count, prefix + 4, 4);
    if (count == 0 || count + added.size() > 0xFFFF || data_start != 0x40 + static_cast<std::uint64_t>(count) * 0x18 ||
        type_offset32 != type_offset || type_offset <= data_start || old_index_count != count ||
        old_size < type_offset) return result;
    struct Row { std::int32_t id; std::int32_t pad; std::int64_t data; std::int64_t name; };
    std::vector<Row> rows(count);
    if (!game::safe_read(file + 0x40, rows.data(), rows.size() * sizeof(Row))) return result;
    for (std::size_t i = 1; i < rows.size(); ++i)
        if (rows[i].id <= rows[i - 1].id) return result;
    if (rows.back().id >= added.front().first) return result;  // ids must stay sorted
    const std::uint64_t kShift = 0x18ull * added.size();  // more entries in the row table
    const std::uint64_t body = old_size - data_start;  // data, type name, row names
    const std::uint64_t new_data_start = data_start + kShift;
    const std::uint64_t stride = (row_size + 0xF) & ~std::uint64_t{0xF};
    const std::uint64_t row_offset = (old_size + kShift + 0xF) & ~std::uint64_t{0xF};
    const std::uint64_t file_size = row_offset + stride * added.size();
    const std::uint64_t index_offset = (file_size + 0xF) & ~std::uint64_t{0xF};
    const std::uint64_t new_count = count + added.size();
    const std::uint64_t total = 16 + index_offset + new_count * 8 + 16;
    auto* block = game_table_alloc(total, reinterpret_cast<const void*>(file - 16));
    if (!block) return result;
    std::uint8_t* out = block + 16;
    if (!game::safe_read(file, out, 0x40) || !game::safe_read(file + data_start, out + new_data_start, body)) {
        game_table_free(block);
        return result;
    }
    for (Row& r : rows) {
        r.data += static_cast<std::int64_t>(kShift);
        r.name += static_cast<std::int64_t>(kShift);
    }
    const std::int64_t shared_name = rows.front().name;
    for (std::size_t i = 0; i < added.size(); ++i) {
        const std::uint64_t at = row_offset + stride * i;
        rows.push_back({added[i].first, 0, static_cast<std::int64_t>(at), shared_name});
        std::memcpy(out + at, added[i].second, row_size);
    }
    std::memcpy(out + 0x40, rows.data(), rows.size() * sizeof(Row));
    const std::uint16_t count16 = static_cast<std::uint16_t>(new_count);
    const std::uint64_t new_type_offset = type_offset + kShift;
    const std::uint32_t type32 = static_cast<std::uint32_t>(new_type_offset);
    std::memcpy(out + 0x0A, &count16, 2);
    std::memcpy(out + 0x00, &type32, 4);
    std::memcpy(out + 0x10, &new_type_offset, 8);
    std::memcpy(out + 0x30, &new_data_start, 8);
    const std::uint32_t size32 = static_cast<std::uint32_t>(file_size);
    const std::uint32_t count32 = static_cast<std::uint32_t>(new_count);
    std::memcpy(block, prefix, sizeof(prefix));
    std::memcpy(block + 0, &size32, 4);
    std::memcpy(block + 4, &count32, 4);
    for (std::uint32_t i = 0; i < count32; ++i) {
        const std::uint32_t entry[2]{static_cast<std::uint32_t>(rows[i].id), i};
        std::memcpy(out + index_offset + i * 8ull, entry, 8);
    }
    if (!game::write<std::uintptr_t>(slot, reinterpret_cast<std::uintptr_t>(out))) {
        game_table_free(block);
        return result;
    }
    ++g_param_appends;
    for (std::size_t i = 0; i < added.size(); ++i) result.push_back(out + row_offset + stride * i);
    return result;
}

void sync_damage_cut() noexcept {
    auto& state = g_damage_cut;
    if (state.attempted) return;
    if (state.ready && state.generation != g_param_appends) {
        // SpEffectParam was rebuilt again (weapon effect copies): find our row
        game::ParamTable effects;
        if (game::find_param(g_statics, L"SpEffectParam", kSpEffectRowSize, effects, nullptr)) {
            if (const std::uintptr_t row = param_row_by_id(effects, kDamageCutEffectId)) {
                state.row = reinterpret_cast<std::uint8_t*>(row);
                for (float& rate : state.applied) rate = -1.0f;  // rewrite every rate
            }
        }
        state.generation = g_param_appends;
    }
    if (!state.ready) {
        game::ParamTable effects;
        if (!game::find_param(g_statics, L"SpEffectParam", kSpEffectRowSize, effects, g_log)) {
            if (effects.file) state.attempted = true;
            return;
        }
        std::uint8_t row[kSpEffectRowSize]{};
        const std::uintptr_t source = param_row_by_id(effects, kDamageCutTemplateId);
        if (!source || !game::safe_read(source, row, sizeof(row))) {
            state.attempted = true;
            log(L"Damage taken: the Dragoncrest Shield Talisman effect was not found; Damage taken unavailable.");
            return;
        }
        // permanent, no icon, every damage rate neutral until set
        const std::int32_t no_icon = -1;
        const float permanent = -1.0f, one = 1.0f;
        std::memcpy(row + 0x0, &no_icon, 4);
        std::memcpy(row + 0x8, &permanent, 4);
        for (std::size_t o = 0x260; o <= 0x2B0; o += 4) std::memcpy(row + o, &one, 4);
        bool retry{};
        const auto added = append_param_rows(L"SpEffectParam", kSpEffectRowSize, {{kDamageCutEffectId, row}}, retry);
        state.row = added.empty() ? nullptr : added.front();
        state.generation = g_param_appends;
        if (!state.row) {
            if (!retry) {
                state.attempted = true;
                log(L"Damage taken: SpEffectParam could not take a row of ours; Damage taken unavailable.");
            }
            return;
        }
        state.ready = true;
        wchar_t message[120]{};
        swprintf_s(message, L"Damage taken: SpEffect %d added for the player's incoming damage.", kDamageCutEffectId);
        log(message);
    }
    // Taken: def*DmgCorrectRate, from players (+0x260..) and from everyone
    // else (+0x274..). Dealt: atk*DmgCorrectRate, on players (+0x28C..) and
    // on everyone else (+0x2A0..). Physical, magic, fire, lightning, holy.
    const Id taken_parts[5]{taken_physical, taken_magic, taken_fire, taken_lightning, taken_holy};
    const Id dealt_parts[5]{dmg_physical, dmg_magic, dmg_fire, dmg_lightning, dmg_holy};
    float rates[10]{};
    bool neutral = true;
    for (int k = 0; k < 5; ++k) {
        rates[k] = value_of(die_one_hit) ? 1000.0f
            : static_cast<float>(value_of(damage_taken)) * static_cast<float>(value_of(taken_parts[k])) / 10000.0f;
        // a 0-damage contact stays 0, so rolling into an enemy no longer kills
        rates[5 + k] = value_of(one_hit_kill) ? 100000.0f
            : static_cast<float>(value_of(damage_dealt)) * static_cast<float>(value_of(dealt_parts[k])) / 10000.0f;
    }
    for (const float rate : rates) neutral = neutral && rate == 1.0f;
    for (int k = 0; k < 10; ++k) {
        if (rates[k] == state.applied[k]) continue;
        const std::uintptr_t row = reinterpret_cast<std::uintptr_t>(state.row);
        if (k < 5) {
            game::write<float>(row + 0x260 + k * 4, rates[k]);
            game::write<float>(row + 0x274 + k * 4, rates[k]);
        } else {
            game::write<float>(row + 0x28C + (k - 5) * 4, rates[k]);
            game::write<float>(row + 0x2A0 + (k - 5) * 4, rates[k]);
        }
        state.applied[k] = rates[k];
    }
    const std::int32_t want = neutral ? 0 : kDamageCutEffectId;
    if (want != state.kept || state.keep_pending) {
        state.keep_pending = nt_keep_player_effect(want) == 0;
        state.kept = want;
        if (!state.keep_pending) {
            wchar_t message[160]{};
            swprintf_s(message, L"Damage taken/dealt: %ls (taken %.2f %.2f %.2f %.2f %.2f, dealt %.2f %.2f %.2f %.2f %.2f).",
                want ? L"on the player" : L"off", rates[0], rates[1], rates[2], rates[3], rates[4], rates[5], rates[6],
                rates[7], rates[8], rates[9]);
            log(message);
        }
    }
}

void sync_item_shop() noexcept {
    auto& state = g_item_shop;
    if (state.attempted || state.ready) return;
    if (++state.tries > 30) {  // about a minute: give up, let the other options run
        state.attempted = true;
        log(L"Item shop: names or params never became ready; unavailable.");
        return;
    }
    if (!g_msg_lookup || !g_msg_repository) {
        // resolved once at startup: without it there are no names, ever
        state.attempted = true;
        log(L"Item shop: the game's text lookup was not found; unavailable.");
        return;
    }
    if (!*g_msg_repository) return;
    std::vector<ShopPending> pending;
    const int owns_sote = steam_dlc_owned(kSteamShadowOfTheErdtree);
    const int owns_tarnished = steam_dlc_owned(kSteamTarnishedPack);
    std::size_t skipped_dlc = 0;
    std::wstring name;
    std::unordered_set<std::wstring> seen_names[sec_count][2];
    // The shop hides a row that costs less than the item sells for
    // (sellValue; the r154 Shop prices finding), so each row costs exactly
    // that: the least the game shows. Items that cannot be sold stay free.
    const auto add = [&](int group, int dlc, std::int32_t equip_id, std::uint8_t type, std::uintptr_t row,
                         std::size_t sell_offset) {
        if (group < 0 || group >= kShopGroupCount) return;
        const int kind = group;
        const bool tarnished = (type == 0 && in_list(kTarnishedWeapons, equip_id)) ||
            (type == 1 && in_list(kTarnishedArmour, equip_id)) || (type == 3 && in_list(kTarnishedGoods, equip_id));
        if (tarnished) {
            if (owns_tarnished != 1) { ++skipped_dlc; return; }
            group = kShopTarnished;
            dlc = 0;
        } else if (dlc == 1 && owns_sote == 0) {
            ++skipped_dlc;
            return;  // unknown (-1) keeps Shadow of the Erdtree: its data needs the DLC to load
        }
        // never the same name twice in one shop (r157-test23: Winged
        // Misbegotten Ashes + 1..+10 next to the base ash)
        if (!seen_names[kShopGroups[group].section][dlc == 1 ? 1 : 0].insert(name).second) return;
        std::int32_t sell{};
        if (!game::read(row + sell_offset, sell) || sell < 0) sell = 0;
        pending.push_back({group, dlc, equip_id, type, sell, row, 0, kind});
    };
    // Only what a player can obtain: items that some item lot (map or enemy),
    // vanilla shop, crafting recipe or class start gives. That leaves out
    // cut and test content ([NPC] weapons, "Type N" armour, test gems,
    // duplicate Ash of War rows, phantom multiplayer items), which could
    // also look suspicious online. Flasks (replaceItemId) and Great Runes
    // come from events, so they are added by type. Index = ItemLot category:
    // 1 goods, 2 weapon (stored without affinity and level), 3 armour,
    // 4 talisman, 5 Ash of War.
    std::unordered_set<std::int32_t> obtainable[6];
    {
        bool lots_ok = true;
        for (const wchar_t* lot : {L"ItemLotParam_map", L"ItemLotParam_enemy"}) {
            game::ParamTable table;
            if (!game::find_param(g_statics, lot, 0x98, table, nullptr)) { lots_ok = false; continue; }
            for (const std::uintptr_t row : table.rows) {
                std::int32_t ids[8]{}, categories[8]{};
                if (!game::safe_read(row, ids, sizeof(ids)) || !game::safe_read(row + 0x20, categories, sizeof(categories))) continue;
                for (int i = 0; i < 8; ++i) {
                    if (ids[i] <= 0 || categories[i] < 1 || categories[i] > 5) continue;
                    obtainable[categories[i]].insert(categories[i] == 2 ? ids[i] - ids[i] % 10000 : ids[i]);
                }
            }
        }
        static constexpr int kShopToLot[5]{2, 3, 4, 1, 5};
        for (const wchar_t* shop : {L"ShopLineupParam", L"ShopLineupParam_Recipe"}) {
            game::ParamTable table;
            if (!game::find_param(g_statics, shop, 0x34, table, nullptr)) { lots_ok = false; continue; }
            for (const std::uintptr_t row : table.rows) {
                std::int32_t id{};
                std::uint8_t type{};
                if (!game::read(row, id) || !game::read(row + 0x17, type) || id <= 0 || type > 4) continue;
                const int c = kShopToLot[type];
                obtainable[c].insert(c == 2 ? id - id % 10000 : id);
            }
        }
        game::ParamTable init;
        if (game::find_param(g_statics, L"CharaInitParam", 0x140, init, nullptr)) {
            for (const std::uintptr_t row : init.rows) {
                std::int32_t v[0xA4 / 4]{};
                if (!game::safe_read(row, v, sizeof(v))) continue;
                const auto put = [&](int c, std::size_t offset) {
                    const std::int32_t id = v[offset / 4];
                    if (id > 0) obtainable[c].insert(c == 2 ? id - id % 10000 : id);
                };
                for (std::size_t o = 0x10; o <= 0x3C; o += 4) put(o >= 0x20 && o <= 0x2C ? 3 : 2, o);  // weapons, armour, arrows
                for (std::size_t o = 0x40; o <= 0x4C; o += 4) put(4, o);  // talismans
                for (std::size_t o = 0x60; o <= 0xA0; o += 4) put(1, o);  // spells and items
            }
        } else {
            lots_ok = false;
        }
        if (!lots_ok) return;  // params not loaded yet
    }
    // Weapons: only the plain version of each (id % 10000 == 0); affinities
    // come from Ashes of War and upgrades from smithing stones, both sold too.
    // Ids: the row table holds them (file + 0x40 + i * 0x18).
    const auto each_row = [&](const wchar_t* param, std::size_t row_size, auto&& fn) -> bool {
        game::ParamTable table;
        if (!game::find_param(g_statics, param, row_size, table, nullptr)) return false;
        std::uint16_t count{};
        if (!game::read(table.file + 0x0A, count)) return false;
        for (std::uint16_t i = 0; i < count && i < table.rows.size(); ++i) {
            std::int32_t id{};
            if (!game::read(table.file + 0x40 + i * 0x18, id)) continue;
            fn(id, table.rows[i]);
        }
        return true;
    };
    int found = 0;
    std::unordered_set<std::int32_t> reinforce_ids;
    std::unordered_map<std::int32_t, std::uintptr_t> goods_ids;  // id -> row
    each_row(L"ReinforceParamWeapon", 0x80, [&](std::int32_t id, std::uintptr_t) { reinforce_ids.insert(id); });
    each_row(L"EquipParamGoods", 0xB0, [&](std::int32_t id, std::uintptr_t row) { goods_ids.emplace(id, row); });
    const auto weapon_max_level = [&](std::uintptr_t row) -> std::uint8_t {
        std::int16_t type{};
        if (!game::read(row + 0xDA, type) || type < 0) return 0;
        std::uint8_t level = 0;
        for (int l = 1; l <= 25; ++l)
            if (reinforce_ids.count(type + l)) level = static_cast<std::uint8_t>(l);
        return level;
    };
    bool ok = each_row(L"EquipParamWeapon", 0x298, [&](std::int32_t id, std::uintptr_t row) {
        if (id <= 0 || id % 10000 != 0 || id == 110000 || !obtainable[2].count(id)) return;
        std::uint16_t type{};
        if (!game::read(row + 0x1A6, type)) return;
        int group = -1;
        for (int g = 0; g < 46; ++g)
            if (kShopWeaponTypes[g] == type) group = g;
        if (group >= 0 && item_name({115, 11, 310, 410}, id, name, &found)) {
            add(group, found - 1, id, 0, row, 0x20);
            if (!pending.empty() && pending.back().equip_id == id) pending.back().max_level = weapon_max_level(row);
        }
    });
    ok = ok && each_row(L"EquipParamProtector", 0x1A0, [&](std::int32_t id, std::uintptr_t row) {
        std::uint8_t slot{};
        if (id <= 10300 || !obtainable[3].count(id) || !game::read(row + 0xD6, slot) || slot > 3) return;  // no hair
        // Ragged set (and Altered): only an NPC wears it; players could get
        // it by mistake until patch 1.03, then it was removed (r157-test27).
        if (id / 1000 == 610 || id / 1000 == 611) return;
        if (item_name({117, 12, 313, 413}, id, name, &found)) add(kShopArmour, found - 1, id, 1, row, 0x20);
    });
    ok = ok && each_row(L"EquipParamAccessory", 0x60, [&](std::int32_t id, std::uintptr_t row) {
        if (id > 0 && obtainable[4].count(id) && item_name({113, 13, 316, 416}, id, name, &found))
            add(kShopTalismans, found - 1, id, 2, row, 0x18);
    });
    ok = ok && each_row(L"EquipParamGoods", 0xB0, [&](std::int32_t id, std::uintptr_t row) {
        if (id <= 0) return;
        std::uint8_t type{}, sort_group{};
        std::int32_t replace_item{};
        if (!game::read(row + 0x3E, type) || !game::read(row + 0x72, sort_group) ||
            !game::read(row + 0x1C, replace_item)) return;
        if (!obtainable[1].count(id) && type != 15) return;
        if (!item_name({111, 10, 319, 419}, id, name, &found)) return;
        // No flasks (replaceItemId: full <-> empty): their charges belong to
        // the character, and a bought one became extra broken inventory
        // entries (r157-test11). Golden Seeds and Sacred Tears are sold.
        if (replace_item != -1) return;
        std::uint8_t spirit_max = 0;
        if (type == 7 || type == 8) {
            // one row per spirit ash: its upgrades are id + 1..10 (some
            // write "Name + 3", r157-test23: Winged Misbegotten Ashes)
            if (id % 100 != 0) return;
            for (int l = 1; l <= 10; ++l)
                if (goods_ids.count(id + l)) spirit_max = static_cast<std::uint8_t>(l);
        }
        add(shop_goods_group(type, sort_group, name), found - 1, id, 3, row, 0x14);
        if (!pending.empty() && pending.back().equip_id == id) pending.back().max_level = spirit_max;
    });
    ok = ok && each_row(L"EquipParamGem", 0x60, [&](std::int32_t id, std::uintptr_t row) {
        if (id > 0 && obtainable[5].count(id) && item_name({35, 322, 422}, id, name, &found))
            add(kShopGems, found - 1, id, 4, row, 0x20);
    });
    if (!ok) return;  // params not loaded yet
    // One shop per section and base / DLC (user, r157-test9: fewer menus).
    // Inside it the groups follow in menu order, each in param id order, and
    // in sections with more than one group each group starts with a title row
    // (r157-test12: the shop hides empty rows, so gaps did not show).
    std::stable_sort(pending.begin(), pending.end(), [](const ShopPending& x, const ShopPending& y) {
        const int sx = kShopGroups[x.group].section, sy = kShopGroups[y.group].section;
        if (sx != sy) return sx < sy;
        if (x.dlc != y.dlc) return x.dlc < y.dlc;
        if (x.group != y.group) return x.group < y.group;
        return x.equip_id < y.equip_id;
    });
    // Armour layout: complete sets (head, body, arms and legs) first, then
    // the rest (altered versions, partial sets, single pieces), each part in
    // id order; one sortGroupId per set (user, r157-test20). The game keeps
    // no "when you get it" order, so that is not possible.
    for (auto& section_rows : g_shop_rows) section_rows[0].clear(), section_rows[1].clear();
    for (int s = 0; s < sec_count; ++s) g_shop_layout[s][0].clear(), g_shop_layout[s][1].clear();
    // Tarnished Edition items keep the layout of their kind (user, 14-11):
    // armour by set, weapons by type, in the Tarnished Edition shop.
    const auto kind_section = [](const ShopPending& p) { return kShopGroups[p.kind].section; };
    for (const int target : {static_cast<int>(sec_armour), static_cast<int>(sec_tarnished)})
    for (int d = 0; d < 2; ++d) {
        struct Piece { std::uintptr_t row; std::int32_t set; std::uint8_t slot; std::int32_t sort_id; std::uint8_t sort_group; };
        std::vector<Piece> pieces;
        std::map<std::int32_t, std::uint8_t> slots_of_set;  // set -> bit per slot
        for (const ShopPending& p : pending) {
            if (p.kind != kShopArmour || p.dlc != d || kShopGroups[p.group].section != target) continue;
            Piece piece{p.row, p.equip_id / 1000, 0, 0, 0};
            if (!game::read(p.row + 0xD6, piece.slot) || !game::read(p.row + 0x4, piece.sort_id) ||
                !game::read(p.row + 0xD8, piece.sort_group) || piece.slot > 3) continue;
            slots_of_set[piece.set] |= static_cast<std::uint8_t>(1u << piece.slot);
            pieces.push_back(piece);
        }
        std::vector<std::int32_t> order;
        for (const auto& [set, mask] : slots_of_set) if (mask == 0x0F) order.push_back(set);
        for (const auto& [set, mask] : slots_of_set) if (mask != 0x0F) order.push_back(set);
        std::map<std::int32_t, int> index_of_set;
        for (std::size_t i = 0; i < order.size(); ++i) index_of_set[order[i]] = static_cast<int>(std::min<std::size_t>(i, 253));
        for (const Piece& piece : pieces) {
            const int set_index = index_of_set[piece.set];
            g_shop_layout[target][d].push_back({piece.row, (set_index + 1) * 10 + piece.slot,
                static_cast<std::uint8_t>(set_index + 1), piece.sort_id, piece.sort_group, piece.slot, 0x4, 0xD8});
        }
    }
    {
        std::int32_t spirit_rank[2]{};
        for (const ShopPending& p : pending) {
            const int target = kShopGroups[p.group].section;  // the shop it is in
            const int s = kind_section(p);                    // how it is laid out
            std::int32_t sort_id{};
            std::uint8_t sort_group{};
            if (s == sec_weapons && p.equip_type == 0) {
                std::uint8_t mount{};
                if (!game::read(p.row + 0x8, sort_id) || !game::read(p.row + 0x190, sort_group) ||
                    !game::read(p.row + 0x241, mount)) continue;
                const int g = std::min(p.kind, 125);
                g_shop_layout[target][p.dlc].push_back({p.row, sort_id, static_cast<std::uint8_t>(1 + 2 * g + (mount == 2 ? 0 : 1)),
                    sort_id, sort_group, 0xFF, 0x8, 0x190});
            } else if (s == sec_gems && p.equip_type == 4) {
                std::uint8_t affinity{};
                if (!game::read(p.row + 0x28, sort_id) || !game::read(p.row + 0x7, sort_group) ||
                    !game::read(p.row + 0x35, affinity)) continue;
                g_shop_layout[target][p.dlc].push_back({p.row, sort_id, static_cast<std::uint8_t>(1 + std::min<int>(affinity, 200)),
                    sort_id, sort_group, 0xFF, 0x28, 0x7});
            } else if (s == sec_spirits && p.equip_type == 3) {
                std::int16_t fp{}, hp{};
                if (!game::read(p.row + 0x20, sort_id) || !game::read(p.row + 0x72, sort_group) ||
                    !game::read(p.row + 0x80, fp) || !game::read(p.row + 0x82, hp)) continue;
                const bool hp_cost = hp > 0;
                const std::int32_t cost = std::max<std::int32_t>(0, hp_cost ? hp : fp);
                const std::int32_t our_sort = cost * 10000 + (spirit_rank[p.dlc]++ % 10000);
                g_shop_layout[target][p.dlc].push_back({p.row, our_sort,
                    static_cast<std::uint8_t>(hp_cost ? 2 : 1), sort_id, sort_group, 0xFF, 0x20, 0x72});
                // the upgraded rows (+1..+10) have their own goods rows: same place
                for (int l = 1; l <= p.max_level; ++l) {
                    const auto it = goods_ids.find(p.equip_id + l);
                    std::int32_t sid{};
                    std::uint8_t sgr{};
                    if (it == goods_ids.end() || !game::read(it->second + 0x20, sid) || !game::read(it->second + 0x72, sgr)) continue;
                    g_shop_layout[target][p.dlc].push_back({it->second, our_sort,
                        static_cast<std::uint8_t>(hp_cost ? 2 : 1), sid, sgr, 0xFF, 0x20, 0x72});
                }
            } else if ((s == sec_sorceries || s == sec_incantations) && p.equip_type == 3) {
                std::uint8_t type{};
                if (!game::read(p.row + 0x20, sort_id) || !game::read(p.row + 0x72, sort_group) ||
                    !game::read(p.row + 0x3E, type)) continue;
                const bool support = type == 17 || type == 18;
                g_shop_layout[target][p.dlc].push_back({p.row, sort_id, static_cast<std::uint8_t>(support ? 2 : 1),
                    sort_id, sort_group, 0xFF, 0x20, 0x72});
            }
        }
    }
    std::vector<ShopItem> items;
    items.reserve(pending.size() + 600);
    std::int32_t counts[sec_count][2]{};
    std::int32_t item_counts[sec_count][2]{};
    int previous_group[sec_count][2];
    for (auto& row : previous_group) row[0] = row[1] = -1;
    int groups_in_section[sec_count]{};
    for (int g = 0; g < kShopGroupCount; ++g) ++groups_in_section[kShopGroups[g].section];
    for (const ShopPending& p : pending) {
        const int s = kShopGroups[p.group].section;
        std::int32_t& n = counts[s][p.dlc];
        const std::int32_t first = (s * 2 + p.dlc) * kShopRange;
        if (n >= kShopRange - 2) continue;
        // No titles (r157-test13: the shop sorts them into a goods group of
        // their own); with "item type" sorting the game separates groups.
        previous_group[s][p.dlc] = p.group;
        items.push_back({first + n++, p.equip_id, p.equip_type, p.price, false, -1, p.price, p.max_level});
        ++item_counts[s][p.dlc];
    }

    // The vanilla table and its holder.
    std::uintptr_t slot{};
    std::uintptr_t file{};
    if (!find_param_holder(L"ShopLineupParam", slot) || !game::read(slot, file) || !file) return;
    std::uint16_t count{};
    std::uint32_t type_offset32{};
    std::uint64_t type_offset{}, data_start{};
    if (!game::read(file + 0x0A, count) || !game::read(file + 0x00, type_offset32) ||
        !game::read(file + 0x10, type_offset) || !game::read(file + 0x30, data_start)) return;
    if (data_start != 0x40 + static_cast<std::uint64_t>(count) * 0x18 || type_offset32 != type_offset ||
        type_offset <= data_start || count + items.size() > 0xFFFF) {
        state.attempted = true;
        log(L"Item shop: ShopLineupParam has an unexpected layout; unavailable.");
        return;
    }
    struct Row { std::int32_t id; std::int32_t pad; std::int64_t data; std::int64_t name; };
    if (count == 0) {
        state.attempted = true;
        log(L"Item shop: ShopLineupParam is empty; unavailable.");
        return;
    }
    std::vector<Row> rows(count);
    if (!game::safe_read(file + 0x40, rows.data(), rows.size() * sizeof(Row))) return;
    // The merge and the id index need the game's rows in id order.
    for (std::size_t i = 1; i < rows.size(); ++i)
        if (rows[i].id <= rows[i - 1].id) {
            state.attempted = true;
            log(L"Item shop: ShopLineupParam rows are not in id order; unavailable.");
            return;
        }
    // Our ids: above the table's highest one, on a round number.
    const std::int64_t highest = rows.back().id;
    const std::int64_t base64 = ((std::max<std::int64_t>(highest, 9999999) / 1000000) + 1) * 1000000;
    if (base64 + kShopGroupCount * 2LL * kShopRange >= 0x7FFFFFFF) {
        state.attempted = true;
        log(L"Item shop: ShopLineupParam has no free ids; unavailable.");
        return;
    }
    const std::int32_t base = static_cast<std::int32_t>(base64);
    for (ShopItem& item : items) item.row_id += base;
    std::int64_t name_offset = rows[0].name;
    for (const Row& r : rows) {
        if (r.name != name_offset || r.data < static_cast<std::int64_t>(data_start) ||
            r.data + 0x34 > static_cast<std::int64_t>(type_offset)) {
            state.attempted = true;
            log(L"Item shop: ShopLineupParam rows are not as expected; unavailable.");
            return;
        }
    }
    // Template rows: one vanilla merchant row per equipType (0-4); the
    // goods one must be unlimited, the others fall back to it.
    std::uint8_t templ[5][0x34]{};
    bool have[5]{};
    for (const Row& r : rows) {
        std::uint8_t buffer[0x34]{};
        if (!game::safe_read(file + r.data, buffer, sizeof(buffer))) continue;
        std::int16_t quantity{};
        std::memcpy(&quantity, buffer + 0x14, 2);
        const std::uint8_t type = buffer[0x17];
        if (type > 4 || have[type] || (type == 3 && quantity != -1)) continue;
        std::memcpy(templ[type], buffer, sizeof(buffer));
        have[type] = true;
    }
    const bool have_template = have[3];
    for (int i = 0; i < 5; ++i)
        if (!have[i]) std::memcpy(templ[i], templ[3], sizeof(templ[3]));
    if (!have_template || name_offset < static_cast<std::int64_t>(type_offset)) {
        state.attempted = true;
        log(L"Item shop: no template row in ShopLineupParam; unavailable.");
        return;
    }
    // In memory the file is preceded by 16 bytes {u32 size, u32 rows, ...}
    // and followed, at the next 16-byte boundary after size, by a sorted
    // index {u32 id, u32 row} that the game's find-row-by-id (0xD4C820 in
    // 2.7.2.0) binary-searches. Check that the vanilla table has them.
    std::uint8_t prefix[16]{};
    std::uint32_t old_size{}, old_index_count{};
    if (!game::safe_read(file - 16, prefix, sizeof(prefix))) return;
    std::memcpy(&old_size, prefix, 4);
    std::memcpy(&old_index_count, prefix + 4, 4);
    {
        const std::uintptr_t old_index = file + ((static_cast<std::uintptr_t>(old_size) + 0xF) & ~std::uintptr_t{0xF});
        std::uint32_t first[2]{}, last[2]{};
        if (old_index_count != count || old_size < type_offset ||
            !game::safe_read(old_index, first, 8) || !game::safe_read(old_index + (count - 1) * 8ull, last, 8) ||
            static_cast<std::int32_t>(first[0]) != rows.front().id ||
            static_cast<std::int32_t>(last[0]) != rows.back().id) {
            state.attempted = true;
            log(L"Item shop: ShopLineupParam has no id index where expected; unavailable.");
            return;
        }
    }
    const std::uint64_t data_size = type_offset - data_start;
    const std::uint64_t tail_size = static_cast<std::uint64_t>(name_offset) - type_offset + 4;
    const std::uint64_t new_count = count + items.size();
    const std::uint64_t new_data_start = 0x40 + new_count * 0x18;
    const std::uint64_t new_type_offset = new_data_start + data_size + items.size() * 0x34;
    const std::uint64_t new_name_offset = new_type_offset + (name_offset - type_offset);
    const std::uint64_t file_size = new_type_offset + tail_size;
    const std::uint64_t index_offset = (file_size + 0xF) & ~std::uint64_t{0xF};
    const std::uint64_t total = 16 + index_offset + new_count * 8 + 16;
    auto* block = game_table_alloc(total, reinterpret_cast<const void*>(file - 16));
    if (!block) return;
    std::uint8_t* out = block + 16;
    if (!game::safe_read(file, out, 0x40) ||
        !game::safe_read(file + data_start, out + new_data_start, data_size) ||
        !game::safe_read(file + type_offset, out + new_type_offset, tail_size)) {
        game_table_free(block);
        return;
    }
    // Merge the rows by id (both lists are sorted).
    std::vector<Row> merged;
    merged.reserve(new_count);
    std::size_t a = 0;
    std::size_t b = 0;
    std::uint64_t new_row_data = new_data_start + data_size;
    while (a < rows.size() || b < items.size()) {
        if (b >= items.size() || (a < rows.size() && rows[a].id < items[b].row_id)) {
            Row r = rows[a++];
            r.data = static_cast<std::int64_t>(r.data - data_start + new_data_start);
            r.name = static_cast<std::int64_t>(new_name_offset);
            merged.push_back(r);
        } else {
            const ShopItem& item = items[b++];
            std::uint8_t* data = out + new_row_data;
            std::memcpy(data, templ[item.equip_type <= 4 ? item.equip_type : 3], 0x34);
            // Nothing else from the template row: runes, one per purchase, no
            // own name, icon or menu title (audit 2026-11-08: an overhaul
            // regulation could price a whole type in Dragon Hearts).
            {
                const std::uint16_t one_set = 1;
                const std::int32_t zero32 = 0, minus_one = -1;
                const float one_f = 1.0f;
                const std::int16_t minus_one16 = -1;
                data[0x18] = 0;                          // costType: runes
                std::memcpy(data + 0x1A, &one_set, 2);   // setNum
                std::memcpy(data + 0x1C, &zero32, 4);    // value_Add
                std::memcpy(data + 0x20, &one_f, 4);     // value_Magnification
                std::memcpy(data + 0x24, &minus_one, 4); // iconId
                std::memcpy(data + 0x28, &minus_one, 4); // nameMsgId
                std::memcpy(data + 0x2C, &minus_one, 4); // menuTitleMsgId
                std::memcpy(data + 0x30, &minus_one16, 2);  // menuIconId
            }
            // The shop hides an unlimited (-1) entry of equipment that does
            // not stack (r157-test5/6: only arrows and one weapon showed), so
            // weapons other than ammunition, armour, talismans and Ashes of
            // War are offered one at a time, as most merchants do.
            const std::int32_t price = item.price;
            const bool ammo = item.equip_type == 0 && item.equip_id >= 50000000 && item.equip_id < 54000000;
            const bool single = item.equip_type != 3 && !ammo;
            const std::int16_t sold_out = 0;
            const std::int32_t none = -1;
            const std::int16_t unlimited = -1;
            const std::uint32_t zero = 0;
            std::memcpy(data + 0x00, &item.equip_id, 4);
            std::memcpy(data + 0x04, &price, 4);
            std::memcpy(data + 0x08, &none, 4);
            std::memcpy(data + 0x0C, &zero, 4);
            std::memcpy(data + 0x10, &zero, 4);
            const std::int16_t one = 1;
            (void)sold_out;
            std::memcpy(data + 0x14, item.spacer ? &unlimited : single ? &one : &unlimited, 2);
            if (item.header >= 0) {
                const std::int32_t message_id = kShopHeaderMsg + item.header;
                std::memcpy(data + 0x28, &message_id, 4);  // nameMsgId
            }
            data[0x17] = item.equip_type;
            merged.push_back({item.row_id, 0, static_cast<std::int64_t>(new_row_data), static_cast<std::int64_t>(new_name_offset)});
            if (!item.spacer && item.header < 0) {
                const std::int32_t local = item.row_id - base;
                const int s = local / (2 * kShopRange);
                const int d = (local / kShopRange) % 2;
                if (s >= 0 && s < sec_count) {
                    // arrows and bolts stay at +0 (audit 2026-11-08)
                    const std::uint8_t kind = item.equip_type == 0 ? (ammo ? 0 : 1) : (s == sec_spirits ? 2 : 0);
                    g_shop_rows[s][d].push_back({data, item.equip_id, item.sell, kind, item.max_level});
                }
            }
            new_row_data += 0x34;
        }
    }
    std::memcpy(out + 0x40, merged.data(), merged.size() * sizeof(Row));
    const std::uint16_t count16 = static_cast<std::uint16_t>(new_count);
    const std::uint32_t type32 = static_cast<std::uint32_t>(new_type_offset);
    std::memcpy(out + 0x0A, &count16, 2);
    std::memcpy(out + 0x00, &type32, 4);
    std::memcpy(out + 0x10, &new_type_offset, 8);
    std::memcpy(out + 0x30, &new_data_start, 8);
    // The prefix and the id index (merged is sorted by id).
    const std::uint32_t size32 = static_cast<std::uint32_t>(file_size);
    const std::uint32_t count32 = static_cast<std::uint32_t>(new_count);
    std::memcpy(block, prefix, sizeof(prefix));
    std::memcpy(block + 0, &size32, 4);
    std::memcpy(block + 4, &count32, 4);
    for (std::uint32_t i = 0; i < count32; ++i) {
        const std::uint32_t entry[2]{static_cast<std::uint32_t>(merged[i].id), i};
        std::memcpy(out + index_offset + i * 8ull, entry, 8);
    }
    // Point the holder at the new table (the old one stays allocated).
    if (!game::write<std::uintptr_t>(slot, reinterpret_cast<std::uintptr_t>(out))) {
        game_table_free(block);
        for (auto& section_rows : g_shop_rows) section_rows[0].clear(), section_rows[1].clear();
        for (auto& section_layout : g_shop_layout) section_layout[0].clear(), section_layout[1].clear();
        state.attempted = true;
        log(L"Item shop: ShopLineupParam could not be replaced; unavailable.");
        return;
    }
    g_shop_block_begin = reinterpret_cast<std::uintptr_t>(block);
    g_shop_block_end = g_shop_block_begin + total;
    g_shop_table = reinterpret_cast<std::uintptr_t>(out);
    g_shop_holder = slot;
    std::int32_t total_items = 0, shops = 0;
    for (int s = 0; s < sec_count; ++s)
        for (int d = 0; d < 2; ++d) {
            state.first[s][d] = base + (s * 2 + d) * kShopRange;
            state.count[s][d] = counts[s][d];
            state.items[s][d] = item_counts[s][d];
            total_items += counts[s][d];
            if (counts[s][d]) ++shops;
        }
    state.ready = true;
    wchar_t message[300]{};
    swprintf_s(message, L"Item shop: ShopLineupParam %u -> %llu rows (highest vanilla id %lld, ours from %d); %d rows in %d shops, %zu items; Steam: Shadow of the Erdtree %d, Tarnished Pack %d (%zu add-on items left out).",
        count, static_cast<unsigned long long>(new_count), static_cast<long long>(highest), base, total_items, shops,
        pending.size(), owns_sote, owns_tarnished, skipped_dlc);
    log(message);
}

// Healing aura (HP/s; Nexus request, Hinir): the Deathbed Dress's aura, as if
// it were worn, for you and your allies. The dress itself only heals allies
// within 7 m, 2 HP per second, and not its wearer (wikis; user, 13-11: "it
// does nothing"). Its effect, SpEffect 6193010 ([Armor] Deathbed Dress, which
// casts the ally-healing bullet 900), is kept on the player by msa
// (nt_keep_player_effect_slot, slot 1); the heal it gives, SpEffect 6193012
// (Deathbed Dress - Heal), gets changeHpPoint (s32 +0xA0, negative heals;
// Smithbox SpEffect def) scaled from its 2 HP/s to the slider's value, and put
// back at 0. You get the same HP per second through regen_worker.
extern "C" std::uint8_t nt_keep_player_effect_slot(std::uint32_t slot, std::int32_t id);
constexpr std::int32_t kDeathbedAuraEffect = 6193010;
constexpr std::int32_t kDeathbedHealEffect = 6193012;
constexpr std::ptrdiff_t kChangeHpPoint = 0xA0;
constexpr int kDeathbedHpPerSecond = 2;
struct HealingAuraState {
    std::int32_t kept{-1};
    bool pending{};
    int found{-1};  // -1 not looked up, 0 no rows, 1 rows present
    std::uintptr_t heal_row{};
    std::int32_t heal_original{};
    std::int32_t heal_written{};
    int generation{-1};  // g_param_appends when heal_row was found
};
HealingAuraState g_aura{};

void sync_healing_aura() noexcept {
    if (g_aura.found < 0 || (g_aura.found == 1 && g_aura.generation != g_param_appends)) {
        game::ParamTable effects;
        if (!game::find_param(g_statics, L"SpEffectParam", kSpEffectRowSize, effects, nullptr)) return;
        const std::uintptr_t aura = param_row_by_id(effects, kDeathbedAuraEffect);
        const std::uintptr_t heal = param_row_by_id(effects, kDeathbedHealEffect);
        const bool first = g_aura.found < 0;
        g_aura.found = aura && heal ? 1 : 0;
        g_aura.generation = g_param_appends;
        if (!g_aura.found) {
            log(L"Healing aura: SpEffect 6193010 / 6193012 not found in this game version.");
        } else {
            g_aura.heal_row = heal;
            std::int32_t points{};
            game::read(heal + kChangeHpPoint, points);
            if (first) {
                g_aura.heal_original = points;
                g_aura.heal_written = points;
                wchar_t message[128]{};
                swprintf_s(message, L"Healing aura: Deathbed Dress heal (SpEffect 6193012) changeHpPoint %d.", points);
                log(message);
            } else {
                g_aura.heal_written = points == g_aura.heal_original ? points : g_aura.heal_written;
            }
        }
    }
    if (g_aura.found != 1) return;
    const int hp = value_of(healing_aura);
    // The dress's own value gives 2 HP/s; scale it to the slider's value.
    const std::int32_t heal = hp > 0 && g_aura.heal_original < 0
        ? static_cast<std::int32_t>(std::lround(static_cast<double>(g_aura.heal_original) * hp / kDeathbedHpPerSecond))
        : g_aura.heal_original;
    if (heal != g_aura.heal_written && game::write<std::int32_t>(g_aura.heal_row + kChangeHpPoint, heal))
        g_aura.heal_written = heal;
    const std::int32_t want = hp > 0 ? kDeathbedAuraEffect : 0;
    if (want == g_aura.kept && !g_aura.pending) return;
    g_aura.pending = nt_keep_player_effect_slot(1, want) == 0;
    g_aura.kept = want;
    if (!g_aura.pending) {
        wchar_t message[128]{};
        if (want) swprintf_s(message, L"Healing aura: %d HP/s for you and allies within 7 m (heal %d).", hp, heal);
        else swprintf_s(message, L"Healing aura: off.");
        log(message);
    }
}

// Materials found (Nexus request, FozexD): how many of each crafting material
// a pick-up gives. ItemLotParam_map (gathering spots, corpses, chests) and
// ItemLotParam_enemy (what enemies drop): lotItemNum01-08 (u8 +0x8A..+0x91;
// Smithbox ItemLotParam def), only for goods slots (category 1, +0x20 + 4k)
// whose item is a crafting material (one of the recipes' materials, g_craft).
// Item drop rate already scales how often enemies drop them.
struct FoundMaterialsState {
    bool attempted{};
    bool ready{};
    std::vector<std::uintptr_t> slots{};    // address of each lotItemNum to scale
    std::vector<std::uint8_t> original{};
    int applied_percent{100};
};
FoundMaterialsState g_found{};

// Enemy chase distance: the three distances the AI's COMMON_SetBattleActLogic
// takes from NpcThinkParam (u16 metres; NPC_THINK_PARAM_ST is 0xE4 bytes):
// +0x3C "goes home whatever happens" (85 m for most enemies in 2.7.2.0),
// +0x3E "goes home while fighting" (30 m) and +0x40 "gives up going home and
// fights" (12 m). All three scale together, so they keep their order; 9999
// (never) only grows, 0 is left alone. Checked offline on 2.7.2.0's
// regulation.bin (2215 rows, stride 0xE4). Enemies still stop where their
// navmesh ends (a cave mouth, another map tile).
struct ChaseCell {
    std::uintptr_t address;
    std::uint16_t original;
};

struct ChaseState {
    bool attempted{};
    bool ready{};
    std::vector<ChaseCell> cells{};
    int applied{100};
};

ChaseState g_chase{};

void sync_enemy_chase() noexcept {
    auto& state = g_chase;
    if (state.attempted) return;
    if (!state.ready) {
        game::ParamTable table;
        if (!game::find_param(g_statics, L"NpcThinkParam", 0xE4, table, g_log)) {
            if (table.file) state.attempted = true;
            return;
        }
        std::vector<std::uint16_t> battle;
        try {
            state.cells.reserve(table.rows.size() * 3);
            for (const auto row : table.rows) {
                for (const std::ptrdiff_t offset : {0x3C, 0x3E, 0x40}) {
                    std::uint16_t distance{};
                    if (!game::read(row + offset, distance) || distance == 0) continue;
                    state.cells.push_back({row + offset, distance});
                    if (offset == 0x40) battle.push_back(distance);
                }
            }
        } catch (...) {
            state.cells.clear();
            state.attempted = true;
            return;
        }
        std::uint16_t median{};
        if (!battle.empty()) {
            std::nth_element(battle.begin(), battle.begin() + battle.size() / 2, battle.end());
            median = battle[battle.size() / 2];
        }
        // Tens of metres in the game's data; another layout shows another spread.
        const bool ok = battle.size() >= 100 && median >= 5 && median <= 1000;
        wchar_t message[200]{};
        swprintf_s(message, L"Enemy chase distance: %zu NpcThinkParam distances (%zu in battle, median %u m)%ls.",
            state.cells.size(), battle.size(), median, ok ? L"" : L" -> option disabled");
        log(message);
        if (!ok) {
            state.cells.clear();
            state.attempted = true;
            return;
        }
        state.ready = true;
    }
    const int want = value_of(enemy_chase);
    if (want == state.applied) return;
    for (const auto& cell : state.cells)
        game::write<std::uint16_t>(cell.address, static_cast<std::uint16_t>(
            std::clamp<std::int64_t>(static_cast<std::int64_t>(cell.original) * want / 100, 1, 65535)));
    state.applied = want;
}

void sync_found_materials() noexcept {
    if (g_found.attempted) return;
    if (!g_found.ready) {
        if (!g_craft.ready) {
            if (g_materials_attempted) g_found.attempted = true;  // no crafting data: nothing to scale
            return;
        }
        std::vector<std::int32_t> materials;
        for (const std::int32_t id : g_craft.ids)
            if (id > 0) materials.push_back(id);
        std::sort(materials.begin(), materials.end());
        materials.erase(std::unique(materials.begin(), materials.end()), materials.end());
        for (const wchar_t* lot : {L"ItemLotParam_map", L"ItemLotParam_enemy"}) {
            game::ParamTable table;
            if (!game::find_param(g_statics, lot, 0x98, table, nullptr)) {
                if (!table.file) return;  // not loaded yet: retry
                g_found.attempted = true;
                log(L"Materials found: an item lot table has another layout; option disabled.");
                return;
            }
            for (const std::uintptr_t row : table.rows) {
                std::int32_t ids[8]{}, categories[8]{};
                std::uint8_t counts[8]{};
                if (!game::safe_read(row, ids, sizeof(ids)) || !game::safe_read(row + 0x20, categories, sizeof(categories)) ||
                    !game::safe_read(row + 0x8A, counts, sizeof(counts)))
                    continue;
                for (int k = 0; k < 8; ++k) {
                    if (categories[k] != 1 || ids[k] <= 0 || !counts[k] ||
                        !std::binary_search(materials.begin(), materials.end(), ids[k]))
                        continue;
                    g_found.slots.push_back(row + 0x8A + k);
                    g_found.original.push_back(counts[k]);
                }
            }
        }
        g_found.ready = true;
        wchar_t message[128]{};
        swprintf_s(message, L"Materials found: %zu item lot slots with crafting materials (%zu materials).",
            g_found.slots.size(), materials.size());
        log(message);
    }
    const int percent = value_of(materials_found);
    if (percent == g_found.applied_percent) return;
    for (std::size_t i = 0; i < g_found.slots.size(); ++i) {
        const auto scaled = static_cast<std::int64_t>(g_found.original[i]) * percent / 100;
        game::write<std::uint8_t>(g_found.slots[i], static_cast<std::uint8_t>(std::clamp<std::int64_t>(scaled, 1, 255)));
    }
    g_found.applied_percent = percent;
}

bool all_params_ready() noexcept {
    for (const auto& state : g_param_states)
        if (!state.ready && !state.attempted) return false;
    return (g_loot.ready || g_loot.attempted) && (g_travel.ready || g_travel.attempted) &&
        (g_buffs.ready || g_buffs.attempted) && (g_buddy.ready || g_buddy.attempted) &&
        (g_weapon_effects.ready || g_weapon_effects.attempted) && (g_immunity.ready || g_immunity.attempted) &&
        (g_flasks.ready || g_flasks.attempted) && g_materials_attempted && (g_found.ready || g_found.attempted) &&
        (g_chase.ready || g_chase.attempted) &&
        (g_helmet.ready || g_helmet.attempted) && (g_lock_on.ready || g_lock_on.attempted) &&
        (g_projectile_range.ready || g_projectile_range.attempted) && (g_charm.ready || g_charm.attempted) &&
        (g_charged_launch.ready || g_charged_launch.attempted) && (g_journey.ready || g_journey.attempted) &&
        (g_ng_poise.ready || g_ng_poise.attempted) && (g_drop_any.ready || g_drop_any.attempted) &&
        (g_item_shop.ready || g_item_shop.attempted) &&
        (g_damage_cut.attempted || (g_damage_cut.ready && !g_damage_cut.keep_pending)) && !g_aura.pending &&
        (!g_item_shop.ready || g_talk_registered || g_talk_tries > 20) &&
        (g_clean.ready || g_clean.attempted) && (g_buff_vfx.ready || g_buff_vfx.attempted) &&
        (g_item_use_vfx.ready || g_item_use_vfx.attempted) &&
        (g_weapon_item_use_vfx.ready || g_weapon_item_use_vfx.attempted) &&
        (g_weapon_buff_vfx.ready || g_weapon_buff_vfx.attempted) && (g_ghost.ready || g_ghost.attempted) &&
        (g_ghost_all.ready || g_ghost_all.attempted) && (g_ghost_npc.ready || g_ghost_npc.attempted);
}

DWORD WINAPI param_worker(void*) noexcept {
    perf::Loop loop(L"param");
    for (;;) {
        loop.pause();
        // World difficulty follows the loaded save's cycle, so it polls while on.
        WaitForSingleObject(g_param_event, all_params_ready() && !journey_wanted() ? INFINITE : 2000);
        loop.resume();
        // Before anything reads ShopLineupParam: the item shop replaces the table.
        sync_item_shop();
        if (!g_item_shop.ready && !g_item_shop.attempted) continue;
        if (g_item_shop.ready) register_item_shop_talk();
        // Before anything keeps SpEffectParam row pointers: it adds a row.
        sync_damage_cut();
        if (!g_damage_cut.ready && !g_damage_cut.attempted) continue;
        sync_healing_aura();
        // First, so it classifies SpEffects before any buildup slider rescales them.
        sync_weapon_effects();
        sync_params();
        sync_loot();
        sync_travel();
        sync_buffs();
        sync_buddy();
        sync_immunity();
        sync_lock_on();
        sync_projectile_range();
        sync_charm();
        sync_charged_launch();
        sync_journey();
        sync_ng_poise();
        sync_drop_any();
        sync_clean();
        sync_buff_vfx(g_buff_vfx, 1, hide_buff_effects, L"self buff");
        sync_item_use_vfx(g_item_use_vfx, g_buffs.hidden_ids, hide_buff_effects, L"buff");
        sync_weapon_buff_vfx();
        sync_item_use_vfx(g_weapon_item_use_vfx, g_buffs.weapon_ids, hide_weapon_buff_effects, L"weapon buff");
        sync_ghost();
        sync_ghost_all();
        sync_ghost_npc();
        sync_flasks();
        sync_materials();
        sync_found_materials();
        sync_enemy_chase();
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
constexpr int kMoveCategory = 3;
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

// The sliders that apply on top of a category or to animations outside them.
bool per_action_extra_speed() noexcept {
    return value_of(speed_sprint) != 100 || value_of(speed_walk) != 100 ||
        value_of(speed_run) != 100 || value_of(speed_interact) != 100 || value_of(speed_skill) != 100;
}

// Weapon skills (Ashes of War and unique skills): each has its own animation
// section, a600-a999 (data/animations.json: the 40000-49999 attacks live there;
// normal attacks are in the weapon sections a020-a062, spells in a4xx). Skill
// speed replaces Attack speed (or Spell speed) on them, so the slider changes
// skills only (Nexus request, toarai).
bool is_skill(std::int32_t anim) noexcept {
    const std::int32_t section = anim / 1000000;
    return section >= 600 && section <= 999;
}

bool speed_active() noexcept {
    if (value_of(speed_global) != 100) return true;
    for (const Id id : kSpeedIds)
        if (value_of(id) != 100) return true;
    return per_action_extra_speed();
}

// Sprint animations (start, loop, turns), part of the move category; Sprint
// speed multiplies Movement speed on them. Walking and running likewise.
// a000_021xxx and 022xxx are a second family of the same moves (r151 log).
bool is_sprint(int key) noexcept {
    return (key >= 20200 && key <= 20230) || (key >= 22200 && key <= 22230);
}
bool is_walk(int key) noexcept {
    return (key >= 20000 && key <= 20099) || (key >= 21000 && key <= 21099);
}
bool is_run(int key) noexcept {
    return (key >= 20100 && key <= 20199) || (key >= 22100 && key <= 22199);
}
// Levers, doors, chests, pick-ups. The animation table files most of them under
// roll (a000_063000 opening a chest, r151 log), so Roll speed changed them and
// Interaction speed did not: they count as interactions, not as rolls.
bool is_interaction(int key) noexcept {
    return key >= 60000 && key <= 69999;
}
// Resting at a Site of Grace keeps its speed. The game's fade to black, on its own
// clock, hides the cut from lying down (068000) to sitting (068010). At x2 the cut
// came before the fade and showed (r151 test, seen at 200%, not at 100%).
bool is_rest(int key) noexcept {
    return key >= 68000 && key <= 68999;
}
// Getting up from it (both sitting variants) has no such cut.
bool is_rest_stand_up(int key) noexcept {
    return key == 68012 || key == 68022;
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
        per_action |= per_action_extra_speed();
        if (per_action) {
            std::int32_t anim{-1};
            if (const auto anim_address = game::player_anim_id(g_statics)) game::read(anim_address, anim);
            const int key = anim >= 0 ? anim % 1000000 : -1;
            const bool interaction = key >= 0 && is_interaction(key);
            const int c = key >= 0 && !interaction ? anim_category(key) : -1;
            if (anim >= 0 && is_skill(anim)) percent = percent * value_of(speed_skill) / 100.0f;
            else if (c >= 0) percent = percent * value_of(kSpeedIds[c]) / 100.0f;
            if (c == kMoveCategory && is_sprint(key)) percent = percent * value_of(speed_sprint) / 100.0f;
            if (c == kMoveCategory && is_walk(key)) percent = percent * value_of(speed_walk) / 100.0f;
            if (c == kMoveCategory && is_run(key)) percent = percent * value_of(speed_run) / 100.0f;
            if (interaction && (!is_rest(key) || is_rest_stand_up(key)))
                percent = percent * value_of(speed_interact) / 100.0f;
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

// Chests, doors and levers keep their speed while the player follows Interaction
// speed, because each dynamic asset has its own animator (2.7.2.0, from the code):
// CSWorldGeomDynamicIns+0x4C8. Its update (0x1AC010) multiplies the frame time by
// +0xC0, or by +0x78 while +0xC0 is 1. The animator is { u32 slots at +0, slot
// array at +8 (stride 0x68), slot time f32 +0x28, slot state u32 +0x4C }, built by
// 0x1AB720, which sets +0x74 to 5 (used below to tell an animator from what follows
// a plain asset). Nothing else writes +0x78. No link from the interaction to its
// object was found, so during the interaction every asset whose animation has just
// started follows it; what starts elsewhere at that moment only speeds up briefly.
// Lifts must keep their speed: AssetGeometryParam isMoveObj (bit 1 of +0x3C, row
// at CSWorldGeomIns+0x20). The map of loaded blocks is at CSWorldGeomMan+0x18
// (head +0x20: MSVC tree, value +0x28), each block's assets at +0x288 (first +0x290).
constexpr float kObjectStartedSeconds = 0.6f;
// a000_060810, the Stonesword Key statue, lasts 14 s at x2 and its object starts
// late, so 3 s of scanning missed it. Long enough for that, bounded all the same.
constexpr ULONGLONG kObjectScanMs = 30000;
// The object often outlasts the player: the statue's a000_060810 ends in 2.4 s at
// x2 with the statue still moving, and putting it back then undid the speed (r151
// log). Objects keep it until their animation ends, at most this long after the
// player's. What the statue starts afterwards is picked up in the same time.
constexpr ULONGLONG kObjectHoldMs = 15000;
constexpr int kStatueAnimation = 60810;

struct SpedObject {
    std::uintptr_t geom;
    std::uintptr_t animator;
};

template <class Visit>
void for_each_asset(std::uintptr_t man, Visit&& visit) noexcept {
    std::uintptr_t head{};
    if (!game::read(man + 0x20, head) || !head) return;
    std::uintptr_t root{};
    if (!game::read(head + 0x8, root)) return;
    std::uintptr_t stack[64];
    std::size_t depth{};
    std::size_t blocks{};
    if (root && root != head) stack[depth++] = root;
    while (depth > 0 && blocks < 1024) {
        const std::uintptr_t node = stack[--depth];
        ++blocks;
        std::uintptr_t left{}, right{}, block{};
        game::read(node, left);
        game::read(node + 0x10, right);
        if (left && left != head && depth < std::size(stack)) stack[depth++] = left;
        if (right && right != head && depth < std::size(stack)) stack[depth++] = right;
        if (!game::read(node + 0x28, block) || !block) continue;
        std::uintptr_t first{}, last{};
        if (!game::read(block + 0x290, first) || !game::read(block + 0x298, last) || last < first) continue;
        const std::size_t count = std::min<std::size_t>((last - first) / sizeof(void*), 16384);
        for (std::size_t i = 0; i < count; ++i) {
            std::uintptr_t geom{};
            if (game::read(first + i * sizeof(void*), geom) && geom) visit(geom);
        }
    }
}

// The animator of a dynamic asset, or 0 for any other asset.
std::uintptr_t asset_animator(std::uintptr_t geom) noexcept {
    const auto base = reinterpret_cast<std::uintptr_t>(game::image());
    std::uintptr_t table{}, animator{};
    if (!game::read(geom, table) || table < base || table >= base + 0x6000000) return 0;
    if (!game::read(geom + 0x4C8, animator) || !animator) return 0;
    std::uint32_t slots{}, marker{};
    if (!game::read(animator, slots) || slots == 0 || slots > 32) return 0;
    if (!game::read(animator + 0x74, marker) || marker != 5) return 0;
    return animator;
}

bool asset_animation_just_started(std::uintptr_t animator) noexcept {
    std::uint32_t slots{};
    std::uintptr_t array{};
    if (!game::read(animator, slots) || !game::read(animator + 0x8, array) || !array) return false;
    for (std::uint32_t i = 0; i < slots && i < 32; ++i) {
        const std::uintptr_t slot = array + i * 0x68;
        std::uint32_t state{};
        float elapsed{-1.0f};
        if (game::read(slot + 0x4C, state) && state != 0 && game::read(slot + 0x28, elapsed) &&
            elapsed >= 0.0f && elapsed < kObjectStartedSeconds)
            return true;
    }
    return false;
}

bool asset_is_lift(std::uintptr_t geom) noexcept {
    std::uintptr_t row{};
    std::uint8_t flags{};
    if (!game::read(geom + 0x20, row) || !row || !game::read(row + 0x3C, flags)) return true;  // unknown: leave it
    return (flags & 0x2) != 0;
}

bool asset_animation_running(std::uintptr_t animator) noexcept {
    std::uint32_t slots{};
    std::uintptr_t array{};
    if (!game::read(animator, slots) || !game::read(animator + 0x8, array) || !array) return false;
    for (std::uint32_t i = 0; i < slots && i < 32; ++i) {
        std::uint32_t state{};
        if (game::read(array + i * 0x68 + 0x4C, state) && state != 0) return true;
    }
    return false;
}

// Puts one object back, unless it is gone or its memory now holds something else.
void restore_object(const SpedObject& object) noexcept {
    std::uintptr_t animator{};
    if (game::read(object.geom + 0x4C8, animator) && animator == object.animator &&
        asset_animator(object.geom) == animator)
        game::write<float>(animator + 0x78, 1.0f);
}

DWORD WINAPI object_speed_worker(void*) noexcept {
    std::vector<SpedObject> sped;
    std::uintptr_t man_slot{};
    bool interacting{};
    bool scan_after{};
    int last_key{};
    ULONGLONG started{};
    ULONGLONG hold_until{};
    int log_lines{};
    perf::Loop loop(L"object_speed");
    const auto scan = [&](int key, float factor) noexcept {
        if (!man_slot) man_slot = game::find_singleton("CSWorldGeomMan", g_log);
        std::uintptr_t man{};
        if (!man_slot || !game::read(man_slot, man) || !man) return;
        try {
            for_each_asset(man, [&](std::uintptr_t geom) {
                const std::uintptr_t animator = asset_animator(geom);
                if (!animator || !asset_animation_just_started(animator) || asset_is_lift(geom)) return;
                for (const auto& object : sped)
                    if (object.geom == geom) return;
                sped.push_back({geom, animator});
                game::write<float>(animator + 0x78, factor);
                if (g_log && g_diagnostics && log_lines < 40) {
                    ++log_lines;
                    wchar_t message[120]{};
                    swprintf_s(message, L"Interaction objects: a000_%06d sped up an asset x%.2f.", key, factor);
                    g_log(message);
                }
            });
        } catch (...) {
        }
    };
    for (;;) {
        const bool wanted = value_of(interaction_objects) != 0 && value_of(speed_interact) != 100;
        if (!wanted && sped.empty()) {
            interacting = false;
            scan_after = false;
            idle_wait(loop);
            continue;
        }
        loop.sleep(50);
        std::int32_t anim{-1};
        const auto anim_address = game::player_anim_id(g_statics);
        if (anim_address) game::read(anim_address, anim);
        const int key = anim >= 0 ? anim % 1000000 : -1;
        const bool now = wanted && key >= 0 && is_interaction(key) && !is_rest(key);
        const ULONGLONG tick = GetTickCount64();
        const float factor = static_cast<float>(value_of(speed_interact)) / 100.0f;
        if (now) {
            if (!interacting) {
                interacting = true;
                started = tick;
            }
            last_key = key;
            if (tick - started <= kObjectScanMs) scan(key, factor);
            continue;
        }
        if (interacting) {
            interacting = false;
            hold_until = tick + kObjectHoldMs;
            scan_after = last_key == kStatueAnimation;
        }
        const bool holding = wanted && tick < hold_until;
        if (holding && scan_after) scan(last_key, factor);
        if (!holding) scan_after = false;
        for (std::size_t i = sped.size(); i-- > 0;) {
            if (holding && asset_animation_running(sped[i].animator)) continue;
            restore_object(sped[i]);
            sped.erase(sped.begin() + static_cast<std::ptrdiff_t>(i));
        }
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
        // Healing aura heals you too, the same HP per second (the dress alone does not).
        const int flat[3]{value_of(hp_regen_points) + value_of(healing_aura), value_of(fp_regen_points),
            value_of(stamina_regen_points)};
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
extern "C" std::uint8_t nt_summon_melina();  // msa\src\melina.rs
extern "C" std::uint8_t nt_set_melina_companion(std::uint8_t enabled);

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
    case sharpen_off: set_site(g_sharpen, on(id)); break;
    case map_in_combat:
        set_site(g_map_combat_open, on(id));
        set_site(g_map_combat_close, on(id));
        break;
    case craft_in_combat:
        set_site(g_craft_combat, on(id));
        set_site(g_craft_combat_menu, on(id));
        break;
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
    case enemy_respawn_kills: break;  // dead_enemy_worker
    case solid_ghosts:  // ghost_worker and the param worker
        wake_workers();
        if (g_param_event) SetEvent(g_param_event);
        break;
    case player_clean: case melina_companion: wake_workers(); break;  // clean_worker
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
    case ash_max_regular: case ash_max_renowned:
        spirit_ashes::update(spirit_ash_settings());
        break;
    case ash_multiple: case ash_extra_cost: case ash_damage_per_ash: case ash_repeat: {
        spirit_ashes::update(spirit_ash_settings());
        const bool shared = on(ash_multiple) && g_spirit_ashes_started && build_buddy_slot_search();
        const bool start = set_site(g_buddy_slot_start, shared);
        const bool search = set_site(g_buddy_slot_loop, shared);
        if (id == ash_multiple) {
            wchar_t message[128]{};
            swprintf_s(message, L"Spirit slots: %ls (start %ls, search %ls).",
                shared ? L"shared up to the set's capacity" : L"10 per player",
                start ? L"ok" : L"not patched", search ? L"ok" : L"not patched");
            log(message);
        }
        break;
    }
    case ash_free_fighting:
        spirit_ashes::set_free_fighting(static_cast<std::uint8_t>(std::clamp(value_of(id), 0, 2)));
        break;
    case ash_dismiss_all:
        // A button: act, then back to off (option_changed / set_option_raw
        // refresh the row).
        if (on(id) && on(ash_multiple)) spirit_ashes::dismiss_all();
        else if (on(id)) log(L"Dismiss all ashes: needs Several ashes at once; nothing dismissed.");
        g_options[id].value = 0;
        break;
    case summon_melina:
        // A button: msa\src\melina.rs puts her summon trigger on the player.
        if (on(id)) {
            const bool queued = nt_summon_melina() != 0;
            log(queued ? L"Summon Melina: requested." : L"Summon Melina: not available yet.");
        }
        g_options[id].value = 0;
        break;
    case shop_weapon_level: case shop_spirit_level: case shop_item_price: break;  // read when a shop opens
    case open_mirror: case open_level_up: case open_rebirth:
        // Buttons like Dismiss all: the screen opens on the next game frame
        // (talk-script commands, msa\src\game_menus.rs).
        if (on(id)) {
            const int kind = id == open_mirror ? 1 : id == open_level_up ? 2 : 3;
            const bool queued = spirit_ashes::open_game_menu(kind);
            wchar_t message[120]{};
            swprintf_s(message, L"Game menu %d: %s (last result %d).", kind,
                queued ? L"requested" : L"not available", spirit_ashes::game_menu_last_result());
            log(message);
        }
        g_options[id].value = 0;
        break;
    case skip_intro: set_site(g_intro, on(id)); break;
    case dialog_yes_default: if (g_dialog_yes_data) g_dialog_yes_data->on = on(id) ? 1 : 0; break;
    case extended_ranges: break;  // read when the game starts (it shapes the menu)
    case highlight_changed:
        if (g_set_highlight) (void)g_set_highlight(on(id) ? 1u : 0u);
        break;
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
    case npc_spawner: break;  // read once at startup (start_npc_spawner)
    case healing_aura:  // param_worker (sync_healing_aura)
    case materials_found:  // param_worker (sync_found_materials)
    case enemy_chase:      // param_worker (sync_enemy_chase)
        if (g_param_event) SetEvent(g_param_event);
        break;
    case keep_bodies:
        if (on(id) && !install_keep_bodies()) break;
        g_keep_bodies_on = on(id) ? 1 : 0;
        if (g_keep_bodies_hooked) {
            wchar_t message[128]{};
            swprintf_s(message, L"Keep bodies %s (%ld bodies kept so far).", on(id) ? L"on" : L"off",
                g_bodies_kept);
            log(message);
        }
        break;
    case ragdoll_death:
        if (g_ragdoll_event) {
            g_ragdoll_on = on(id) ? 1 : 0;
            wchar_t message[128]{};
            swprintf_s(message, L"Ragdoll on death %s (%ld deaths so far).", on(id) ? L"on" : L"off",
                g_ragdoll_count);
            log(message);
        }
        break;
    case status_immunity: break;  // status_worker
    case camera_shake_off:
        if (g_camera_shake) {
            g_camera_shake->on = on(id) ? 1u : 0u;
            wchar_t message[128]{};
            swprintf_s(message, L"Disable camera shake %s (%u rumbles blocked so far).",
                on(id) ? L"on" : L"off", static_cast<unsigned>(g_camera_shake->blocked));
            log(message);
        }
        break;
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
    case buff_weapon: case buff_self: case buff_enemy: case deflect_window: case stack_buffs: case stack_weapon_buffs: case ash_cost: case upgrade_cost:
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
    case projectile_range: case lock_on_distance: case charm_all_enemies: case charm_bosses:
    case spell_stamina_cost: case spell_one_slot: case spell_no_requirements: case carry_max:
    case shop_price: case craft_free: case weapon_requirements: case upgrade_free: case hide_helmet:
    case charged_launch: case launch_all: case launch_distance:
    case world_journey: case journey_keep_runes: case boss_hp:
    case clean_characters: case hide_buff_effects: case hide_weapon_buff_effects: case solid_ashes:
    case ng_poise: case drop_any: case drop_key_items: case one_hit_kill:
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
    case sharpen_off: return g_sharpen.ready;
    case craft_in_combat: return g_craft_combat.ready && g_craft_combat_menu.ready;
    case map_in_combat: return g_map_combat_open.ready && g_map_combat_close.ready;
    case chromatic_off: return g_chromatic.ready;
    case animation_distance: return g_animation.ready;
    case rune_loss_off: return g_rune_loss.ready;
    case enemies_no_respawn: case enemy_respawn_kills: return g_enemy_respawn.ready;
    case pause_key: return g_pause.ready;
    case fps_unlock: case fps_limit: return g_frametime.ready && g_fullscreen_cap.ready;
    case ultrawide: return g_ultrawide.ready;
    case ash_multiple: case ash_dismiss_all: case ash_extra_cost: case ash_damage_per_ash:
    case ash_repeat: case ash_free_fighting: case ash_max_regular: case ash_max_renowned:
        return g_spirit_ashes_started;
    case refresh_rate: return g_fullscreen_cap.ready;
    case skip_intro: return g_intro.ready;
    case open_mirror: case open_level_up: case open_rebirth:
        return g_statics.layout_2_7_1 && g_statics.world_chr_man;
    case summon_melina: case melina_companion: return false;  // work in progress, separate mod
    case dialog_yes_default: return g_dialog_yes_data != nullptr;
    case extended_ranges: case highlight_changed: return !g_config_path.empty();
    case enemies_downed_vulnerable:
        return (g_downed != nullptr || g_downed_site != nullptr) && g_statics.layout_2_7_1 &&
            g_statics.world_chr_man;
    case hud_screen_edges: case hud_scale: case menu_edges: return hud_layout_available();
    case aa_off: case motion_blur_off: case dof_off: return g_window_slot != nullptr && g_statics.layout_2_7_1;
    // Param tables load with the regulation file, after the menu is built; the
    // row-size check on load disables a table whose layout does not match.
    case camera_distance:
    case damage_dealt: case damage_taken: case stamina_cost: case spell_fp_cost:
    case enemy_hp: case runes_gained: case posture_dealt: case posture_taken:
    case equip_weight: case status_buildup: case player_poise: case enemy_poise:
    case player_poise_lock: case enemy_poise_lock: case posture_one_hit: case status_instant: case die_one_hit:
    case posture_one_hit_taken: case loot_drop: case fast_travel_anywhere:
    case buff_weapon: case buff_self: case buff_enemy: case deflect_window: case stack_buffs: case stack_weapon_buffs: case ash_cost: case upgrade_cost:
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
    case projectile_range: case lock_on_distance: case charm_all_enemies: case charm_bosses:
    case spell_stamina_cost: case spell_one_slot: case spell_no_requirements: case skill_fp_cost: case carry_max:
    case shop_price: case craft_free: case weapon_requirements: case upgrade_free: case hide_helmet:
    case charged_launch: case launch_all: case launch_distance: case boss_hp:
        return g_statics.layout_2_7_1 && g_statics.regulation_manager;
    case clean_characters: return g_statics.layout_2_7_1 && g_statics.regulation_manager && !g_clean.attempted;
    case hide_buff_effects:
        return g_statics.layout_2_7_1 && g_statics.regulation_manager && !g_buff_vfx.attempted;
    case hide_weapon_buff_effects:
        return g_statics.layout_2_7_1 && g_statics.regulation_manager && !g_weapon_buff_vfx.attempted;
    case solid_ashes: return g_statics.layout_2_7_1 && g_statics.regulation_manager && !g_ghost.attempted;
    case ng_poise: return g_statics.layout_2_7_1 && g_statics.regulation_manager && !g_ng_poise.attempted;
    case camera_shake_off: return g_camera_shake != nullptr;
    case ragdoll_death: return g_ragdoll_event != nullptr;
    case keep_bodies: return g_timed_fade_site != nullptr;
    case healing_aura: return g_statics.layout_2_7_1 && g_statics.regulation_manager && g_aura.found != 0;
    case materials_found: return g_statics.layout_2_7_1 && g_statics.regulation_manager && !g_found.attempted;
    case enemy_chase: return g_statics.layout_2_7_1 && g_statics.regulation_manager && !g_chase.attempted;
    case one_hit_kill: return g_statics.layout_2_7_1 && g_statics.regulation_manager && !g_damage_cut.attempted;
    case shop_weapon_level: case shop_spirit_level: case shop_item_price: return g_item_gib != nullptr;
    case npc_spawner: return g_npc_spawner_built_in && !g_npc_spawner_standalone;
    case drop_any: return g_statics.layout_2_7_1 && g_statics.regulation_manager && !g_drop_any.attempted;
    case drop_key_items:
        return g_statics.layout_2_7_1 && g_statics.regulation_manager && !g_drop_any.attempted;
    case solid_ghosts: return g_statics.layout_2_7_1 && g_statics.world_chr_man;
    case player_clean:
        return g_statics.layout_2_7_1 && g_statics.world_chr_man && g_statics.regulation_manager;
    // Also needs the save's cycle (GameDataMan) to add only what the save lacks.
    case world_journey: case journey_keep_runes:
        return g_statics.layout_2_7_1 && g_statics.regulation_manager && g_statics.game_data_man &&
            !g_journey.attempted;
    case max_hp: case max_fp: case max_stamina:
    case hp_regen_points: case fp_regen_points: case stamina_regen_points:
    case speed_global: case speed_attack: case speed_spell: case speed_roll: case speed_move:
    case speed_jump: case speed_item: case speed_sprint: case speed_walk: case speed_run: case speed_interact: case speed_skill:
    case interaction_objects:
    case enemy_speed: case no_fall_damage:
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
    g_extended = optional_feature(L"ExtendedRanges", L"NativeTweaks.ExtendedRanges.ini");
    const bool highlight = optional_feature(L"HighlightChangedOptions", L"NativeTweaks.HighlightChanged.ini");
    if (!g_extended) {
        for (const Scale& scale : g_scales) {
            g_options[scale.id].minimum = scale.classic_minimum;
            g_options[scale.id].maximum = scale.classic_maximum;
            g_options[scale.id].step = scale.classic_step;
        }
    }
    for (auto& option : g_options) {
        const int stored = g_config_path.empty() ? option.fallback :
            static_cast<int>(GetPrivateProfileIntW(option.general ? L"General" : L"Vanilla", option.key, option.fallback,
                g_config_path.c_str()));
        option.value = clamp_value(option, stored);
    }
    // The old optional packages turn these on with a marker file beside the INI.
    if (g_extended) g_options[extended_ranges].value = 1;
    if (highlight) g_options[highlight_changed].value = 1;
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
    for (auto& scale : g_scales) {
        if (!g_extended) break;
        const UINT stored = g_config_path.empty() ? 0 :
            GetPrivateProfileIntW(L"Vanilla", scale.key, 0, g_config_path.c_str());
        scale.index = static_cast<std::uint8_t>(stored < kScaleCount ? stored : 0);
    }
}

DWORD WINAPI save_worker(void*) noexcept {
    // Each WritePrivateProfileStringW rewrites the whole INI, so only the
    // options that changed since the last save are written.
    const std::size_t total = g_extended ? count + std::size(g_scales) : count;
    int written[count + std::size(g_scales)]{};
    bool known[count + std::size(g_scales)]{};
    // What the INI already holds counts as written, so the first save only
    // writes what changed (audit 2026-11-08: it rewrote about 200 keys).
    constexpr int kMissing = INT_MIN + 7;
    for (std::size_t i = 0; i < total && !g_config_path.empty(); ++i) {
        const bool is_scale = i >= count;
        const wchar_t* const key = is_scale ? g_scales[i - count].key : g_options[i].key;
        const wchar_t* const section = !is_scale && g_options[i].general ? L"General" : L"Vanilla";
        const int stored = static_cast<int>(GetPrivateProfileIntW(section, key, kMissing, g_config_path.c_str()));
        if (stored != kMissing) written[i] = stored, known[i] = true;
    }
    perf::Loop loop(L"save");
    for (;;) {
        loop.pause();
        WaitForSingleObject(g_save_event, INFINITE);
        Sleep(400);  // Coalesce slider drags into one write.
        loop.resume();
        for (std::size_t i = 0; i < total; ++i) {
            const bool is_scale = i >= count;
            const wchar_t* const key = is_scale ? g_scales[i - count].key : g_options[i].key;
            const int current = is_scale ? g_scales[i - count].index : static_cast<int>(g_options[i].value);
            if (known[i] && written[i] == current) continue;
            wchar_t value[12]{};
            swprintf_s(value, L"%d", current);
            const wchar_t* const section = !is_scale && g_options[i].general ? L"General" : L"Vanilla";
            if (WritePrivateProfileStringW(section, key, value, g_config_path.c_str())) {
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

bool is_named(Id i) noexcept {
    return i == weather_type || i == charged_launch || i == world_journey || i == ash_free_fighting ||
        i == ash_repeat;
}

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

// The slider position that gives the game default under the current scale,
// so the label colour follows the effective value.
void sync_scaled_default(const Scale& scale) noexcept {
    const auto& option = g_options[scale.id];
    if (!g_set_default || !g_registration || option.row == ERUI_INVALID_ROW) return;
    const int factor = kScaleFactors[scale.index];
    const int value = clamp_value(option, (option.fallback + factor / 2) / factor);
    set_row_default(scale.id, option.row, value);
}

// Fork export (r108; numeric sliders accepted since r119): relabels every
// position so a scaled slider shows the value it applies (250% at x10 reads
// 2500%). An older fork rejects numeric rows and keeps the base value.
using SetSliderTextsFn = ERUI_Result (ERUI_CALL*)(ERUI_ProviderHandle, ERUI_RowHandle,
    const ERUI_Utf16View*, std::uint32_t);
SetSliderTextsFn g_set_slider_texts{};

void sync_scaled_texts(const Scale& scale) noexcept {
    const auto& option = g_options[scale.id];
    if (!g_set_slider_texts || !g_registration || option.row == ERUI_INVALID_ROW) return;
    try {
        const int factor = kScaleFactors[scale.index];
        std::vector<std::wstring> texts;
        for (int value = option.minimum; value <= option.maximum; value += option.step)
            texts.push_back(std::to_wstring(value * factor) + option.suffix);
        std::vector<ERUI_Utf16View> views(texts.size());
        for (std::size_t k = 0; k < texts.size(); ++k) {
            views[k].data = reinterpret_cast<const std::uint16_t*>(texts[k].c_str());
            views[k].length = static_cast<std::uint32_t>(texts[k].size());
        }
        (void)g_set_slider_texts(g_registration->provider_handle(), option.row, views.data(),
            static_cast<std::uint32_t>(views.size()));
    } catch (...) {
    }
}

void ERUI_CALL scale_changed(void* target, const ERUI_CustomSliderChangeContext* context) noexcept {
    auto* scale = static_cast<Scale*>(target);
    if (!scale || !context || context->index >= kScaleCount) return;
    scale->index = static_cast<std::uint8_t>(context->index);
    sync_scaled_default(*scale);
    sync_scaled_texts(*scale);
    apply(scale->id);
    request_save();
}

void restore_option(Id i) noexcept {
    if (!available(i)) return;
    auto& option = g_options[i];
    option.value = option.fallback;
    Scale* const scale = scale_of(i);
    if (scale) {
        const bool was_scaled = scale->index != 0;
        scale->index = 0;
        sync_scaled_default(*scale);
        if (was_scaled) sync_scaled_texts(*scale);
        if (g_registration && scale->row != ERUI_INVALID_ROW)
            (void)g_registration->set_custom_slider_index(scale->row, 0);
    }
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
    for (std::size_t i = 0; i < count; ++i)
        if (!g_options[i].general) restore_option(static_cast<Id>(i));
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

// A line without a count (lists from before Kills before staying dead) is one
// kill, so those enemies stay dead at 1 and come back for more kills above.
// 1000 was what the first test build wrote for them (2026-10-28).
constexpr int kOldListKills = 1000;

void load_dead_enemies(const std::wstring& path, std::map<DeadEnemy, int>& kills) {
    kills.clear();
    FILE* file{};
    if (path.empty() || _wfopen_s(&file, path.c_str(), L"r") != 0 || !file) return;
    char line[128]{};
    while (std::fgets(line, sizeof(line), file)) {
        unsigned block{}, index{};
        int npc_param{}, count{1};
        if (line[0] != ';' && sscanf_s(line, "%x %u %d %d", &block, &index, &npc_param, &count) >= 3 && count > 0)
            kills[{block, index, npc_param}] = count >= kOldListKills ? 1 : count;
    }
    std::fclose(file);
}

bool save_dead_enemies(const std::wstring& path, const std::map<DeadEnemy, int>& kills) {
    if (path.empty()) return false;
    const std::wstring folder = path.substr(0, path.find_last_of(L'\\'));
    CreateDirectoryW(folder.substr(0, folder.find_last_of(L'\\')).c_str(), nullptr);
    CreateDirectoryW(folder.c_str(), nullptr);
    std::string text = "; NativeTweaks: enemies killed in this save (block, index, NpcParam, kills).\n"
        "; They stay dead from Kills before staying dead on. Delete this file to bring them all back.\n";
    char line[64]{};
    for (const auto& [e, count] : kills) {
        std::snprintf(line, sizeof(line), "%08X %u %d %d\n", e.block, e.index, e.npc_param, count);
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

void listed_dead(const std::map<DeadEnemy, int>& kills, int limit, std::set<DeadEnemy>& dead) {
    dead.clear();
    for (const auto& [e, count] : kills)
        if (count >= limit) dead.insert(dead.end(), e);
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

// Melina companion: her model's idle animations (c2180 TAE 0 and 20) set a
// turn speed of 0 for the whole animation (TAE event 224 SetTurnSpeed; Tiche's
// sets 5.0 for 3.33 s), so as a spirit she could not face you and walked off
// in a straight line (in-game log 2026-10-28: always animation 2000, distance
// growing). The event's setter (2.7.2.0 eldenring.exe+0x407020; rcx =
// CSChrActionFlagModule, xmm1 = speed, r8b = priority; it writes +0x84 and
// the priority +0x91) gets 5.0 instead of 0 when the module's character
// (+0x8) is NpcParam 21809000, the row msa\src\melina.rs makes her spirit.
constexpr const char* kTurnSpeedPattern =
    "48 8B D1 44 38 81 91 00 00 00 7C ? F3 0F 11 89 84 00 00 00 44 88 81 91 00 00 00";
constexpr std::uint8_t kTurnSpeedEntry[]{0x48, 0x8B, 0xD1, 0x44, 0x38, 0x81, 0x91, 0x00, 0x00, 0x00};
constexpr std::int32_t kMelinaSpiritNpc = 21809000;
constexpr std::uint32_t kMelinaTurnSpeed = 0x40A00000;  // 5.0f

// Melina companion is work in progress (separate mod): its hook stays off.
constexpr bool kMelinaWip = false;

void install_turn_speed_hook() noexcept {
    std::uint8_t* site = find_unique(parse_pattern(kTurnSpeedPattern));
    if (!site) {
        log(L"Vanilla site 'turn speed' not found (or ambiguous); Melina may not turn.");
        return;
    }
    if (std::memcmp(site, kTurnSpeedEntry, sizeof(kTurnSpeedEntry)) != 0) {
        log(L"Vanilla turn speed code was changed by another mod; left alone.");
        return;
    }
    std::uint8_t* code = alloc_near(site);
    if (!code) {
        log(L"Vanilla turn speed hook: no memory near the game code.");
        return;
    }
    std::uint8_t* p = code;
    const auto put = [&](std::initializer_list<std::uint8_t> bytes) {
        for (auto b : bytes) *p++ = b;
    };
    const auto put_u32 = [&](std::uint32_t v) { std::memcpy(p, &v, 4); p += 4; };
    const auto put_u64 = [&](std::uint64_t v) { std::memcpy(p, &v, 8); p += 8; };
    put({0x50});                                       // push rax
    put({0x48, 0x8B, 0x41, 0x08});                     // mov rax,[rcx+8] (owner ChrIns)
    put({0x48, 0x85, 0xC0});                           // test rax,rax
    put({0x74, 0x00});                                 // jz done
    std::uint8_t* const no_owner = p;
    put({0x81, 0x78, 0x60}); put_u32(static_cast<std::uint32_t>(kMelinaSpiritNpc));  // cmp dword [rax+60h],id
    put({0x75, 0x00});                                 // jne done
    std::uint8_t* const not_melina = p;
    put({0x66, 0x0F, 0x7E, 0xC8});                     // movd eax,xmm1
    put({0xA9}); put_u32(0x7FFFFFFF);                  // test eax,7FFFFFFFh (any speed but +-0)
    put({0x75, 0x00});                                 // jnz done
    std::uint8_t* const has_speed = p;
    put({0xB8}); put_u32(kMelinaTurnSpeed);            // mov eax,5.0f
    put({0x66, 0x0F, 0x6E, 0xC8});                     // movd xmm1,eax
    no_owner[-1] = static_cast<std::uint8_t>(p - no_owner);
    not_melina[-1] = static_cast<std::uint8_t>(p - not_melina);
    has_speed[-1] = static_cast<std::uint8_t>(p - has_speed);
    put({0x58});                                       // done: pop rax
    for (const auto b : kTurnSpeedEntry) *p++ = b;     // mov rdx,rcx; cmp [rcx+91h],r8b
    put({0xFF, 0x25, 0x00, 0x00, 0x00, 0x00});         // jmp [rip+0] -> the jl after them
    put_u64(reinterpret_cast<std::uint64_t>(site + sizeof(kTurnSpeedEntry)));
    FlushInstructionCache(GetCurrentProcess(), code, 128);
    std::uint8_t patch[sizeof(kTurnSpeedEntry)]{0xE9, 0, 0, 0, 0, 0x90, 0x90, 0x90, 0x90, 0x90};
    const auto disp = static_cast<std::int32_t>(reinterpret_cast<std::intptr_t>(code) -
        reinterpret_cast<std::intptr_t>(site + 5));
    std::memcpy(patch + 1, &disp, 4);
    if (!write_code(site, patch, sizeof(patch))) {
        log(L"Vanilla turn speed hook could not be written.");
        return;
    }
    wchar_t message[120]{};
    swprintf_s(message, L"Vanilla turn speed hook installed at eldenring.exe+0x%llX (Melina's spirit turns).",
        static_cast<unsigned long long>(site - reinterpret_cast<std::uint8_t*>(game::image())));
    log(message);
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

bool call_chr_enable_here(ChrEnableFn function, std::uintptr_t chr) noexcept {
    __try {
        function(chr);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// The setters do a plain read-modify-write of ChrIns+0x539 and touch the
// character it rides: run them in the game's task loop (GameMan step, msa
// game_thread.rs), not on this worker, so a frame that changes the same byte
// cannot lose a bit (21-11). Before msa's task is registered (or in the audit
// tool) they run here, as before. Not taken within 250 ms (a loading
// screen): not run; the worker's next pass tries again.
extern "C" std::uint8_t nt_msa_game_thread_ready();
extern "C" std::uint8_t nt_msa_on_game_thread(std::uint8_t (*function)(std::uintptr_t), std::uintptr_t argument,
    std::uint32_t timeout_ms);

struct ChrEnableCall {
    ChrEnableFn function;
    std::uintptr_t chr;
};

std::uint8_t chr_enable_on_game_thread(std::uintptr_t argument) noexcept {
    const auto* call = reinterpret_cast<const ChrEnableCall*>(argument);
    return call_chr_enable_here(call->function, call->chr) ? 1 : 0;
}

std::uint8_t game_thread_noop(std::uintptr_t argument) noexcept {
    *reinterpret_cast<DWORD*>(argument) = GetCurrentThreadId();
    return 1;
}

// Once a character is loaded: one empty call through the queue, so the log
// shows it works and how long the game takes to run it.
DWORD WINAPI game_thread_probe_worker(LPVOID) {
    for (int i = 0; i < 1800; ++i) {  // up to 30 minutes
        Sleep(1000);
        std::uintptr_t manager{}, player{};
        if (!nt_msa_game_thread_ready() || !game::read(g_statics.world_chr_man, manager) || !manager ||
            !game::read(manager + game::kPlayerIns, player) || !player)
            continue;
        DWORD thread{};
        const ULONGLONG start = GetTickCount64();
        const std::uint8_t result =
            nt_msa_on_game_thread(&game_thread_noop, reinterpret_cast<std::uintptr_t>(&thread), 1000);
        wchar_t message[160]{};
        swprintf_s(message, L"Game-thread calls: %ls in %llu ms (game thread T%lu, worker T%lu).",
            result == 2 ? L"ready" : L"not taken", GetTickCount64() - start, thread, GetCurrentThreadId());
        log(message);
        if (result == 2) return 0;
    }
    return 0;
}

bool call_chr_enable(ChrEnableFn function, std::uintptr_t chr) noexcept {
    if (!nt_msa_game_thread_ready()) return call_chr_enable_here(function, chr);
    const ChrEnableCall call{function, chr};
    return nt_msa_on_game_thread(&chr_enable_on_game_thread, reinterpret_cast<std::uintptr_t>(&call), 250) == 2;
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

// Keep bodies: 6 s after a body is kept, it is frozen (ChrIns debug flags
// +0x538 bit 8, disabled_updates) and gets the model parts it had alive, once
// (putting them back every frame fought the game, NPC Spawner found). Only
// characters still loaded are touched; the others are forgotten.
DWORD WINAPI bodies_worker(void*) noexcept {
    constexpr ULONGLONG kSettleMs = 6000;
    std::vector<std::uintptr_t> loaded{};
    struct Settle {
        std::uintptr_t chr;
        std::uint32_t mask;
        bool mask_known;
    };
    std::vector<Settle> settle{};
    int logged = 0;
    for (;;) {
        Sleep(250);
        try {
            if (!g_keep_bodies_on || g_bodies_pending <= 0) continue;
            loaded.clear();
            settle.clear();
            game::for_each_loaded_chr(g_statics, &collect_chr, &loaded);
            std::sort(loaded.begin(), loaded.end());
            settle.reserve(16);
            const ULONGLONG now = GetTickCount64();
            // Only the bookkeeping under the lock: the game thread waits on it
            // in the ApplySpEffect hook, so game writes and logging come after.
            AcquireSRWLockExclusive(&g_seen_alive_lock);
            for (auto it = g_seen_alive.begin(); it != g_seen_alive.end();) {
                BodyState& state = it->second;
                if (!state.dead_since || state.settled) {
                    ++it;
                    continue;
                }
                const std::uintptr_t chr = it->first;
                if (!std::binary_search(loaded.begin(), loaded.end(), chr)) {
                    InterlockedDecrement(&g_bodies_pending);
                    it = g_seen_alive.erase(it);
                    continue;
                }
                if (now - state.dead_since >= kSettleMs && settle.size() < settle.capacity()) {
                    settle.push_back({chr, state.alive_mask, state.mask_known});
                    state.settled = true;
                    InterlockedDecrement(&g_bodies_pending);
                }
                ++it;
            }
            ReleaseSRWLockExclusive(&g_seen_alive_lock);
            for (const Settle& body : settle) {
                // Bit 0 of +0x539 is ours; the game sets other bits of the same
                // byte from its own thread, so the set is one locked OR.
                game::or_byte(body.chr + 0x539, 0x01);
                const bool parts = body.mask_known && restore_model_parts(body.chr, body.mask);
                if (logged < 40) {
                    ++logged;
                    std::int32_t model{-1};
                    game::read(body.chr + 0x64, model);
                    wchar_t message[128]{};
                    swprintf_s(message, L"Keep bodies: c%04d frozen in place%ls.", model,
                        parts ? L", its model parts put back" : L"");
                    log(message);
                }
            }
        } catch (...) {
        }
    }
}

// Characters at the health limit get a bar drawn against their real maximum.
// The game keeps recalculating max_uncapped_hp from base_hp (+0x144), so writing
// only the former made the bar flicker. Both are lowered once per character, by
// the ratio that brings max_uncapped_hp down to the limit: a second pass would
// lower base_hp again, as the game can take seconds to recalculate.
// Only while a health slider is moved: the game's rates alone stay far below it.
DWORD WINAPI health_limit_worker(void*) noexcept {
    std::set<std::uintptr_t> applied;
    std::vector<std::uintptr_t> loaded;
    for (;;) {
        Sleep(250);
        if (value_of(enemy_hp) == 100 && value_of(boss_hp) == 100) continue;
        try {
            loaded.clear();
            game::for_each_loaded_chr(g_statics, &collect_chr, &loaded);
            // sorted once, so forgetting unloaded characters is not quadratic
            std::sort(loaded.begin(), loaded.end());
            for (auto it = applied.begin(); it != applied.end();)
                it = std::binary_search(loaded.begin(), loaded.end(), *it) ? std::next(it) : applied.erase(it);
            for (const std::uintptr_t chr : loaded) {
                EnemyState s{};
                std::int32_t uncapped{}, base{};
                if (applied.count(chr) || !read_enemy(chr, s) || s.hp_max != kMaxCharacterHp ||
                    !game::read(s.data + 0x140, uncapped) || !game::read(s.data + 0x144, base) ||
                    uncapped <= kMaxCharacterHp || base <= 0)
                    continue;
                const auto lowered = std::max(static_cast<std::int32_t>(
                    static_cast<std::int64_t>(base) * kMaxCharacterHp / uncapped), 1);
                game::write(s.data + 0x144, lowered);
                game::write(s.data + 0x140, kMaxCharacterHp);
                applied.insert(chr);
                if (g_diagnostics) {
                    wchar_t message[160]{};
                    swprintf_s(message, L"Health limit: NpcParam %d HP %d/%d uncapped %d: base %d -> %d.",
                        s.npc_param, s.hp, s.hp_max, uncapped, base, lowered);
                    log(message);
                }
            }
        } catch (...) {
        }
    }
}

// Log file only: the real health of loaded bosses and of anything with a lot of
// health, to compare with the "Boss health:" line of sync_params. Logs each
// character once, then when its maximum changes or its health reaches 0.
DWORD WINAPI boss_health_log_worker(void*) noexcept {
    struct Seen { std::int32_t hp, hp_max; };
    std::map<std::uintptr_t, Seen> seen;
    std::vector<std::uintptr_t> loaded;
    for (;;) {
        Sleep(500);
        try {
            loaded.clear();
            game::for_each_loaded_chr(g_statics, &collect_chr, &loaded);
            // forget unloaded characters: the map no longer grows all session
            std::sort(loaded.begin(), loaded.end());
            for (auto it = seen.begin(); it != seen.end();)
                it = std::binary_search(loaded.begin(), loaded.end(), it->first) ? std::next(it) : seen.erase(it);
            for (const std::uintptr_t chr : loaded) {
                EnemyState s{};
                if (!read_enemy(chr, s) || (s.team >= 1 && s.team <= 5)) continue;
                if (s.team != 7 && s.hp_max < 5000 && s.hp_max >= 0) continue;
                const auto found = seen.find(chr);
                const wchar_t* event{};
                if (found == seen.end()) event = L"loaded";
                else if (found->second.hp_max != s.hp_max) event = L"maximum changed";
                else if (found->second.hp > 0 && s.hp <= 0) event = L"health reached 0";
                if (event) {
                    std::int32_t uncapped{};
                    game::read(s.data + 0x140, uncapped);
                    wchar_t message[180]{};
                    swprintf_s(message, L"Boss health live: NpcParam %d team %u HP %d/%d uncapped %d (was %d/%d): %ls.",
                        s.npc_param, s.team, s.hp, s.hp_max, uncapped, found == seen.end() ? 0 : found->second.hp,
                        found == seen.end() ? 0 : found->second.hp_max, event);
                    log(message);
                }
                seen[chr] = {s.hp, s.hp_max};
            }
        } catch (...) {
        }
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
    std::map<DeadEnemy, int> kills{};  // the save's list: kills of each enemy
    std::set<DeadEnemy> dead{};        // the ones with Kills before staying dead or more
    int kills_limit{};
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
            if (dirty && save_dead_enemies(path, kills)) {
                wchar_t message[200]{};
                swprintf_s(message,
                    L"Enemies stay dead: %zu stay dead, %zu with kills counted (%d new, %d respawns prevented).",
                    dead.size(), kills.size(), recorded, prevented);
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
        kills.clear();
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
    // The save's list starts over when the option is turned off with a save
    // loaded, and when it is turned on during the session: only enemies killed
    // while it is on count (user requests 2026-10-28). On from the start of
    // the game keeps the list across sessions. The old list stays as .bak.
    bool was_on = value_of(enemies_no_respawn) != 0;
    bool fresh_start = false;
    const auto reset_list = [&](const wchar_t* why) noexcept {
        try {
            flush();
            const std::wstring backup = path + L".bak";
            const bool kept = MoveFileExW(path.c_str(), backup.c_str(), MOVEFILE_REPLACE_EXISTING) != 0;
            wchar_t message[1200]{};
            swprintf_s(message, L"Enemies stay dead: %ls, list of %zu enemies reset%ls%ls.", why, kills.size(),
                kept ? L"; the old one is " : L"", kept ? backup.c_str() : L"");
            log(message);
        } catch (...) {
        }
        kills.clear();
        dead.clear();
        dirty = false;
    };
    perf::Loop loop(L"dead_enemy");
    for (;;) {
        loop.sleep(50);
        try {
            if (!value_of(enemies_no_respawn)) {
                enable_again();
                if (!path.empty()) reset_list(L"option off");
                was_on = false;
                close();
                idle_wait(loop);
                continue;
            }
            if (!was_on) {
                fresh_start = true;  // turned on during this session
                was_on = true;
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
                load_dead_enemies(path, kills);
                kills_limit = 0;
                wchar_t message[1024]{};
                swprintf_s(message, L"Enemies stay dead: %zu enemies with kills listed for %ls.", kills.size(),
                    path.c_str());
                log(message);
            }
            if (fresh_start) {
                reset_list(L"option on");
                fresh_start = false;
                kills_limit = 0;
            }

            const int limit = value_of(enemy_respawn_kills);
            if (limit != kills_limit) {
                listed_dead(kills, limit, dead);
                // A higher limit: enemies this worker disabled that now need
                // more kills come back where they are, without a rest.
                int back = 0;
                if (!disabled.empty() && g_chr_enable) {
                    loaded.clear();
                    game::for_each_loaded_chr(g_statics, &collect_chr, &loaded);
                    for (const std::uintptr_t chr : loaded) {
                        EnemyState s{};
                        if (!disabled.count(chr) || !read_enemy(chr, s) || dead.count(s.key()) ||
                            !chr_disabled(chr) || !call_chr_enable(g_chr_enable, chr))
                            continue;
                        disabled.erase(chr);
                        ++back;
                    }
                }
                if (kills_limit) {
                    wchar_t message[200]{};
                    swprintf_s(message,
                        L"Enemies stay dead: after %d kill(s) now; %zu of %zu enemies stay dead, %d back at once.",
                        limit, dead.size(), kills.size(), back);
                    log(message);
                }
                kills_limit = limit;
                force_full = true;
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
                int killed = 0;
                if (recording) {
                    killed = ++kills[key];
                    dirty = true;
                }
                const bool now_dead = recording && killed >= limit;
                wchar_t counted[48]{};
                if (recording && !now_dead) swprintf_s(counted, L"kill %d of %d", killed, limit);
                wchar_t message[360]{};
                swprintf_s(message,
                    L"Enemies stay dead: death seen (%ls) block %08X index %u NpcParam %d team %u entity %u "
                    L"HP %d/%d: %ls%ls.",
                    d.kind ? L"runes only" : L"item lot and runes", s.block, key.index, s.npc_param, s.team,
                    s.entity, s.hp, s.hp_max,
                    reason ? reason : now_dead ? L"recorded" : recording ? counted : L"already listed",
                    d.suppressed ? L", killed again on load: payout skipped" : L"");
                line(message);
                if (now_dead) {
                    dead.insert(key);
                    ++recorded;
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

// Solid ghostly characters (Nexus request): ChrIns.phantomParamOverride
// (s32 +0x540, after the debug flags at +0x538; fromsoftware-rs) is the
// PhantomParam row a character is drawn with, -1 for the one its character
// type brings (chrType +0x68: 19 the golden NPC signs, 1 white phantoms...).
// -1 kept those golden (test 2026-10-28), so every loaded character but the
// player that is of a phantom type or has an override gets PhantomParam 999
// instead, a row that changes nothing (black additive edge and front,
// diffuse and specular x1, no glow; same as 813 and 890). Turning the option
// off puts the old value back on the ones still loaded. Each NpcParam /
// chrType / override seen is logged once.
constexpr std::uintptr_t kPhantomOverride = 0x540;
constexpr std::uintptr_t kChrType = 0x68;
constexpr std::int32_t kSolidPhantomParam = 999;

struct GhostChr {
    std::int32_t npc_param;
    std::int32_t original;
};

// fromsoftware-rs ChrType: 1 white phantom, 2 duelist, 3-4 ghosts, 8 gray,
// 15-18 invaders and blue phantoms, 19-21 NPC signs and invaders.
bool phantom_type(std::int32_t type) noexcept {
    return (type >= 1 && type <= 4) || type == 8 || (type >= 15 && type <= 21);
}

DWORD WINAPI ghost_worker(void*) noexcept {
    std::vector<std::uintptr_t> loaded{};
    std::map<std::uintptr_t, GhostChr> changed{};
    std::set<std::tuple<std::int32_t, std::int32_t, std::int32_t>> logged{};
    std::set<std::tuple<std::int32_t, std::int32_t, std::uint8_t>> allies_logged{};
    perf::Loop loop(L"ghost");
    for (;;) {
        try {
            const bool want = value_of(solid_ghosts) != 0;
            if (!want && changed.empty()) {
                idle_wait(loop);
                continue;
            }
            loop.sleep(1000);
            std::uintptr_t manager{}, player{};
            if (!game::read(g_statics.world_chr_man, manager) || !manager ||
                !game::read(manager + game::kPlayerIns, player) || !player) {
                changed.clear();  // title screen or loading: those characters are gone
                continue;
            }
            loaded.clear();
            game::for_each_loaded_chr(g_statics, &collect_chr, &loaded);
            std::sort(loaded.begin(), loaded.end());
            loaded.erase(std::unique(loaded.begin(), loaded.end()), loaded.end());
            if (!want) {
                int restored = 0;
                for (const std::uintptr_t chr : loaded) {
                    const auto it = changed.find(chr);
                    std::int32_t npc_param{}, now{};
                    if (it == changed.end() || !game::read(chr + 0x60, npc_param) ||
                        npc_param != it->second.npc_param || !game::read(chr + kPhantomOverride, now) ||
                        now != kSolidPhantomParam)
                        continue;
                    game::write<std::int32_t>(chr + kPhantomOverride, it->second.original);
                    ++restored;
                }
                wchar_t message[120]{};
                swprintf_s(message, L"Solid ghostly characters: off, %d ghostly looks back.", restored);
                log(message);
                changed.clear();
                continue;
            }
            for (auto it = changed.begin(); it != changed.end();)
                it = std::binary_search(loaded.begin(), loaded.end(), it->first) ? std::next(it) : changed.erase(it);
            for (const std::uintptr_t chr : loaded) {
                std::int32_t look{}, npc_param{}, type{};
                std::uint8_t team{};
                // diagnostics only: extra reads and up to 60 lines (audit 2026-11-08)
                if (g_diagnostics && chr != player && game::read(chr + 0x6C, team) && (team == 2 || team == 8 || team == 12 ||
                    team == 26 || team == 27 || team == 28 || team == 47) && game::read(chr + 0x60, npc_param) &&
                    game::read(chr + kChrType, type) && game::read(chr + kPhantomOverride, look) &&
                    allies_logged.size() < 60 && allies_logged.insert({npc_param, type, team}).second) {
                    wchar_t message[160]{};
                    swprintf_s(message, L"Solid ghostly characters: ally NpcParam %d chrType %d team %u override %d.",
                        npc_param, type, team, look);
                    log(message);
                }
                if (chr == player || !game::read(chr + kPhantomOverride, look) ||
                    look == kSolidPhantomParam || !game::read(chr + 0x60, npc_param) ||
                    !game::read(chr + kChrType, type) || (look <= 0 && !phantom_type(type)))
                    continue;
                if (logged.size() < 200 && logged.insert({npc_param, type, look}).second) {
                    wchar_t message[180]{};
                    swprintf_s(message,
                        L"Solid ghostly characters: NpcParam %d chrType %d PhantomParam override %d -> %d.",
                        npc_param, type, look, kSolidPhantomParam);
                    log(message);
                }
                changed.try_emplace(chr, GhostChr{npc_param, look});
                game::write<std::int32_t>(chr + kPhantomOverride, kSolidPhantomParam);
            }
        } catch (...) {
        }
    }
}

// Keep your character clean (Nexus request): msa\src\clean.rs puts a bare
// copy of the Soap's cleaning (SpEffect 502120: deletes paint decal group 10
// when it ends) on the player every quarter second. Tried first and dropped
// (2026-10-28): the decal request never names the character (decal hooks at
// +0xB2E9F6/+0xB2EA88: 49 paint requests, the player in none), and the
// player's NpcParam 0 paintRenderTargetSize (u16 +0x1CE) set to 0 before the
// save loaded changed nothing.
extern "C" std::uint8_t nt_set_player_clean(std::uint8_t enabled);

// Switches of the msa frame tasks: Keep your character clean (clean.rs) and
// Melina companion (melina.rs). Each asks again every second until the
// game's task scheduler is up.
DWORD WINAPI clean_worker(void*) noexcept {
    struct Switch {
        Id option;
        std::uint8_t (*set)(std::uint8_t);
        const wchar_t* on;
        const wchar_t* off;
        bool running;
    };
    Switch switches[]{
        {player_clean, &nt_set_player_clean, L"Keep your character clean: on.", L"Keep your character clean: off.", false},
    };
    perf::Loop loop(L"clean");
    for (;;) {
        bool pending = false;
        for (auto& sw : switches) {
            const bool want = value_of(sw.option) != 0;
            if (want == sw.running) continue;
            if (sw.set(want ? 1 : 0)) {
                sw.running = want;
                log(want ? sw.on : sw.off);
            } else {
                pending = true;
            }
        }
        if (pending) loop.sleep(1000);
        else idle_wait(loop);
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
    cat_survival, cat_equipment, cat_resources, cat_movement, cat_defense, cat_torrent, cat_dealt, cat_taken, cat_poise,
    cat_status, cat_weapon, cat_buffs, cat_enemies, cat_spirits, cat_runes, cat_map, cat_world, cat_camera,
    cat_image, cat_display, cat_interface, cat_item_shop, cat_count
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
        {L"Movement & Speed", L"Your animation speed, overall and per action, and how high and far you jump."},
        {L"Dodge & Defence", L"Longer dodge invincibility and parry window, perfect guard, and protection while knocked down or using items."},
        {L"Torrent", L"Torrent's survival, running and jumping."},
        {L"Damage Dealt", L"Your damage, per element, stat scaling, knockback and one-hit kills."},
        {L"Damage Taken", L"Enemy damage to you, per element, knockback and one-hit deaths."},
        {L"Poise", L"Your poise and enemies' poise, and the poise damage both sides deal."},
        {L"Status Effects", L"How fast each status builds up for everyone, your immunity and enemy immunities."},
        {L"Weapon Effects", L"Add status effects to your attacks, each with its own switch."},
        {L"Buffs & Durations", L"How long buffs, greases and enemy debuffs last, or make them permanent."},
        {L"Enemies", L"Enemy health, speed, behaviour and respawns."},
        {L"Spirit Ashes", L"Summoning cost, several spirits at once, and where and when they can be called."},
        {L"Runes, Shops & Upgrades", L"Runes, item drops, shop prices, upgrades and crafting."},
        {L"Map & Travel", L"Graces, map reveal, fast travel, and the mirror, level up and rebirth anywhere."},
        {L"World", L"Time of day and weather."},
        {L"Camera", L"Field of view, camera behaviour and lock-on."},
        {L"Image", L"Post-processing and level of detail."},
        {L"Display", L"Frame rate, refresh rate and aspect ratio."},
        {L"Interface & System", L"HUD and menu layout, pause, startup logos and Yes/No dialogs."},
        {L"Item Shop", L"Every item you can obtain, sorted by type, in a shop that opens from the talk menu (F11 > Item Shop). Set its upgrade level and price here or in Shop settings inside that menu."},
    },
    {
        {L"Custom field of view", L"Scale the game's field of view. Applies instantly."},
        {L"Field of view (%)", L"100% is the game's own value. Zoom effects are preserved."},
        {L"Disable camera auto-rotation", L"The camera no longer swings behind you while moving."},
        {L"Disable camera recentering", L"Lock-on without a target no longer snaps the camera forward."},
        {L"Camera distance (%)", L"Scales how far the camera sits behind you. Applies when the camera changes: mount, lock on or move to another area."},
        {L"Remove vignette", L"Removes the darkened screen edges. With Solid Uncapper on, this makes it warn at start that Vignette could not be applied. It is harmless; to avoid it, turn this off and use Disable Vignette in Solid Uncapper."},
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
        {L"Damage dealt (%)", L"Scales your attacks and spells. Only you: humanoid NPCs and invaders, who use the same attacks as players, keep theirs. Applies from the next hit."},
        {L"Damage taken (%)", L"Scales the damage you take from enemies, humanoid NPCs and invaders included. Only you: attacks between NPCs and on your spirit ashes do not change."},
        {L"Poise damage dealt (%)", L"Scales the poise damage of your attacks, weapons and spells: how fast they stagger enemies and break their stance. Applies to your next hit."},
        {L"Poise damage taken (%)", L"Scales the poise damage of enemy attacks: how fast they stagger you. Also affects NPCs and spirit ashes. Applies to the next hit."},
        {L"Stamina cost (%)", L"Scales the stamina used by your attacks and actions."},
        {L"Spell FP cost (%)", L"Scales the FP cost of sorceries and incantations."},
        {L"Status buildup (%)", L"Poison, rot, bleed, blight, frost, sleep and madness. Applies to everyone, you included."},
        {L"One-hit kills", L"Your hits that deal damage defeat enemies instantly (a push or rolling into them does not)."},
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
        {L"Several ashes at once", L"Multiple Spirit Ashes, built in. Using a different spirit ash keeps the ones already summoned, each paying its FP; using a summoned ash's item again dismisses only that one with a short fade, even with too little FP. No confirmations appear, summoned ashes follow you into new areas and through doors, and each gets a compact row with its name and HP on the HUD (up to 7). The HUD only changes after restarting the game: the rows appear if the option is on when the game starts, otherwise the game's own spirit panel stays (going back to the title screen is not enough). Off, summoning works as in the base game."},
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
        {L"Remove enemy immunities", L"Enemies immune to poison, rot, bleed, frostbite or sleep get a typical resistance instead, also those whose immunity comes from a built-in effect, such as gargoyles and Erdtree Avatars. Madness and death blight still only affect player-type characters. Applies to enemies loaded afterwards: rest at a grace."},
        {L"Crimson flask healing (%)", L"Scales the HP restored by the Flask of Crimson Tears, at every upgrade level. 0% heals nothing."},
        {L"Cerulean flask FP (%)", L"Scales the FP restored by the Flask of Cerulean Tears, at every upgrade level. 0% restores nothing."},
        {L"Spell stamina cost (%)", L"Scales the stamina used by sorceries and incantations, charged casts included."},
        {L"Spells use one slot", L"Spells that take 2 or 3 memory slots take 1. Other spells are unchanged."},
        {L"Ignore spell requirements", L"Cast spells without the Intelligence and Faith they ask for. Arcane requirements are not included."},
        {L"Skill FP cost (%)", L"Scales the FP used by Ashes of War, charged stages and follow-ups included. 0% makes them free and keeps at least 1 FP, which the game needs for the full skill."},
        {L"Carry up to 999 of each item", L"Stackable items hold 999 on you and in the chest. Unique items (key items, whistle, flasks) are unchanged."},
        {L"Item use speed (%)", L"Drinking flasks, using consumables and picking up items. Multiplies the overall speed."},
        {L"Merchant prices (%)", L"Scales what merchants and other shops charge, in runes or in items such as Dragon Hearts. Items never cost less than they sell for, because the game would hide them, so 0% only makes free what cannot be sold. Trades for remembrances still need them. Applies the next time you open a shop."},
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
        {L"Enemies stay dead", L"Enemies you kill stay dead after resting at a grace, passing time, dying, fast travel or loading the save. Each save keeps its list in DLLs\\NativeTweaks\\dead-enemies; an enemy that loads again is removed at once: no body, no runes or drops. Bosses and NPCs work as usual, and enemies still alive reset as usual. Turning it off brings everyone back with the next rest. Turning it on or off during play starts the save's list over (the old one is kept as .bak), so only enemies killed while it is on count; on from the start of the game, the list carries over between sessions."},
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
        {L"Die in one hit", L"Any hit that deals damage kills you, from enemies, humanoid NPCs and invaders alike. Only you: enemies and your spirit ashes keep their health. Overrides Damage taken; God mode still protects you."},
        {L"Enemies break your poise in one hit", L"Every enemy hit staggers you. Your spirit ashes share this data and also break enemies' poise in one hit. Overrides Poise damage taken."},
        {L"Charged attacks launch enemies", L"Your charged heavy attacks throw enemies through the air instead of staggering them: back, far back or upward. It works with their poise intact. Enemies already on the ground, large enemies and bosses have no throw and keep their own reaction. Far back throws best; Launch distance below sets how far. Applies from your next hit."},
        {L"Every attack launches", L"With the option above on, every attack of yours that makes enemies react throws them the same way, not only charged heavy attacks: light and heavy attacks, skills, spells and thrown items."},
        {L"Launch distance (%) (beta)", L"BETA, still in progress: meant to make launched enemies fly farther. For now it only makes enemies lighter while the launch option above is on (at 200% they weigh half as much, taken when they appear: rest at a grace), and in tests that has not changed how far they fly yet. Leave it at 100% unless you want to experiment."},
        {L"World difficulty", L"Enemies get the strength of a New Game+ cycle, up to NG+7, area by area as in the real New Game+: more health, damage, defence and status resistance, and more runes. Your save stays in its own cycle: bosses, items and endings don't change. In New Game+ it only adds what your cycle lacks. Applies to enemies that appear afterwards: rest at a grace."},
        {L"Keep your runes", L"With World difficulty on, enemies give the runes of your save's own cycle instead of the harder cycle's."},
        {L"Open the mirror", L"Opens the appearance screen of the Roundtable Hold mirror. Use it out of combat."},
        {L"Rebirth", L"Opens Rennala's rebirth screen to reallocate your attributes. No Larval Tear needed. Use it out of combat."},
        {L"Boss health (%)", L"Scales the health of bosses only: main, field, dungeon and DLC bosses. Multiplies Enemy health, so with Enemy health at 100% only bosses change. Applies to bosses loaded afterwards: rest at a grace. The game stops health at 524287, so going higher adds nothing."},
        {L"Keep characters clean (beta)", L"BETA: applies to everyone, enemies included; for your character alone use Keep your character clean. Blood, mud and scorch marks no longer stick to characters' bodies, yours and enemies'. Splatters still appear on the ground and walls. Applies to new hits; marks already on you wear off as usual."},
        {L"Hide buff effects", L"Your own buffs (incantations, sorceries, boluses, talismans and similar) no longer show particles or a glow on your body. They work exactly the same, keep their sound, and invisibility still works. Weapon buffs keep their look (see the option below). Applies to the next buff you use."},
        {L"Hide weapon buff effects", L"Greases and weapon enchantments no longer show their glow or particles on your weapon. They work exactly the same and keep their sound. Applies to the next grease or enchantment you use."},
        {L"Dodge invincibility (%) (beta)", L"BETA: makes the invincibility of rolls, backsteps and dodge skills last longer. 100% is the game's (14 frames for a light or medium roll, 13 for a heavy one); 200% doubles it. The extra time follows the dodge's own invincibility and lasts even if you act right after. Your character only."},
        {L"Parry window (%) (beta)", L"BETA: makes the parry window of shields and parry skills last longer. 100% is the game's; 200% doubles it. The extra time follows the parry's own window. Your character only."},
        {L"Invincible while knocked down (beta)", L"BETA: hits do not hurt you while a big hit has you on the ground, getting up included, as during a roll. Big knockdowns only, not grabs. Your character only."},
        {L"Super armour while using items (beta)", L"BETA: the game's super armour while you drink a flask or use an item: weaker hits no longer interrupt you, very strong ones still can, and you still take the damage. Your character only."},
        {L"Extended ranges", L"Off by default. Many sliders of this menu go further and get a Scale row (x1 to x100) under them that multiplies their value. Applies when you restart the game."},
        {L"Highlight changed options", L"Off by default. The names of options set away from the game's default are coloured, so you can see at a glance what you changed. Applies at once."},
        {L"Item of a summoned ash", L"With Several ashes at once, what using the item of a spirit ash that is already summoned does. Dismisses it (default): only that ash leaves. Summons it again: you get the same ash once more, and each one pays its FP; Dismiss all ashes removes them. Game decides: the item is left to the game, or to a mod that gives it another use, instead of dismissing the ash."},
        {L"Free fighting", L"Off by default. Where summoned spirit ashes fight freely, with no warp to your side cutting into the fight: Boss arenas, only there; Everywhere, in boss arenas and in any fight on foot. When a fight ends, ashes left far away come back to you. In every mode they still cross fog gates with you. Applies right away."},
        {L"Deflect window (%) (beta)", L"BETA: with the Deflecting Hardtear, makes its perfect guard window last longer. 100% is the game's (6 frames when you raise your guard, fewer after several deflects in a row); 200% doubles the first window. Your character only."},
        {L"Solid spirit ashes", L"Summoned spirit ashes (and Seluvis's puppets) lose their blue ghostly look and appear as solid characters. They work exactly the same. Applies to the next ash you summon."},
        {L"Stack self buffs (beta)", L"BETA: your own buffs of the same kind no longer replace each other, so for example Flame, Grant Me Strength, Barrier of Gold and a Boiled Crab, or Golden Vow and an Uplifting Aromatic, all stay on at once. Using the same buff again still just refreshes it. Applies to the next buffs you use."},
        {L"Kills before staying dead", L"With Enemies stay dead: how many times you have to kill an enemy before it stops coming back, like Dark Souls II (12 there). 1 is right away. Each save counts its kills in the same file as the list."},
        {L"Solid ghostly characters (beta)", L"BETA: characters drawn as ghosts, such as some Evergaol bosses, spectral enemies, Melina and NPCs you summon, lose that look and appear solid. They work exactly the same. Spirit ashes have their own option above. Applies within a second."},
        {L"Keep your character clean (beta)", L"BETA: blood, mud and scorch marks no longer stick to your character's body; enemies get them as usual. Works like the game's Soap, all the time: a mark from a hit is gone within a quarter of a second. Applies at once. Splatters still appear on the ground."},
        {L"Level up", L"Opens the level-up screen Melina offers at a grace, to spend runes on your attributes anywhere. Use it out of combat. It can be bound to a key in [Hotkeys] of NativeTweaks.ini (OpenLevelUp = F9)."},
        {L"Summon Melina (beta)", L"BETA: summons Melina as a spirit companion that fights beside you, using the fighting Melina the game has but never uses. Works best with Summon anywhere and Several ashes at once. She uses her own attacks with Black Knife Tiche's spirit AI."},
        {L"Melina companion (beta)", L"BETA: Melina is always with you as a spirit companion. She is summoned again by herself whenever she is not beside you: after you rest at a grace (she sits beside you while you rest), after a loading screen or if she falls. Works best with Summon anywhere and Several ashes at once. Dismiss all ashes sends her away until she comes back."},
        {L"Stack weapon buffs (beta)", L"BETA: buffs on the same weapon no longer replace each other, so for example a grease and an armament spell or a weapon skill buff stay on together, each with its own damage. The weapon shows one of them. Applies to the next buffs you use."},
        {L"Sprint speed (%)", L"Sprinting only, on top of Movement speed: 200% here with Movement speed at 150% sprints at 300%. Walking and running keep Movement speed."},
        {L"Walk speed (%)", L"Walking only, on top of Movement speed."},
        {L"Run speed (%)", L"Running only, on top of Movement speed."},
        {L"Interaction speed (%)", L"Pulling levers, opening doors and chests, picking things up, getting up from a Site of Grace. Multiplies the overall speed. Sitting down keeps its speed. Only your character speeds up: the chest or door itself and the effects around it keep their speed."},
        {L"Projectile range (%)", L"How far your spells, arrows and thrown items fly before they vanish. Their speed and damage do not change. Use the same value as Lock-on distance so they reach what you lock on to: at 500% both reach about 250 m. Some spells arc down and can still hit the ground first."},
        {L"Lock-on distance (%)", L"How far away you can lock on to enemies and keep them locked. No lock-on wins over this."},
        {L"Remove forced sharpening", L"Turns off the sharpening filter the game applies after anti-aliasing. The image looks softer, with fewer halos on edges."},
        {L"Bewitching on all enemies (beta)", L"BETA: Bewitching Branches also work on enemies the game makes immune, such as Black Knights or skeletons. Reaches enemies that appear after you turn it on: rest at a grace. Some may not fight back well while charmed."},
        {L"Bewitching on bosses (beta)", L"BETA: Bewitching Branches also work on bosses. A charmed boss can stall its phases or a scripted fight for up to 3 minutes. Rest at a grace after changing it."},
        {L"Objects follow interaction speed (beta)", L"BETA: chests, doors and levers also move at Interaction speed. Lifts keep their speed. Effects around them do not change. Other objects that start moving at that same moment can speed up briefly. Off by default."},
        {L"New Game+ poise scaling (%) (beta)", L"BETA: from NG+2 on, the game changes enemy poise, from x0.95 at NG+2 to x0.6 at NG+7. 100% keeps that change and 0% removes it, so enemies keep the poise of a first playthrough. Only matters in a New Game+ save. Applies to enemies that appear afterwards: rest at a grace."},
        {L"Disable camera shake (beta)", L"BETA: attacks, landings and events no longer shake the camera. Controller vibration from them stops too. Applies at once to new shakes."},
        {L"Drop or discard any item (beta)", L"BETA: every weapon, armour piece, talisman, Ash of War and item can be dropped or discarded, except key items, Great Runes and flasks. A dropped item stays on the ground until you leave the area; a discarded one is gone for good. Applies at once."},
        {L"...key items too (beta)", L"BETA: also key items, Great Runes and flasks. Discarding an item the story needs, such as a medallion or a key, can leave you unable to go on. A flask in a quick slot keeps one: remove it from the slot to drop the last one. Needs the option above."},
        {L"Weapon level", L"Upgrade level of the weapons in the item shop (F11 > Item Shop). Somber weapons follow their own scale: +25 here is +10 for them. Weapons that cannot be upgraded stay at +0. Applies the next time you open the shop."},
        {L"Spirit ash level", L"Upgrade level of the spirit ashes in the item shop, up to +10 or the highest level each ash has. Applies the next time you open the shop."},
        {L"Price (%)", L"Price of everything in the item shop, as a percentage of its sell value. 0% makes everything free. Applies the next time you open the shop."},
        {L"NPC Spawner", L"Off by default. Spawn any enemy, boss or NPC wherever you are, to test fights and settings: F11 > NPC Spawner, or Mod Settings > NPC Spawner, with its own settings (NpcSpawner.ini next to NativeTweaks.dll). Restart the game after changing it. Unavailable when the standalone NpcSpawner.dll is installed: that one is used."},
        {L"Ragdoll on death (beta)", L"BETA: when you or a human character (NPCs, invaders, other Tarnished) dies, the body falls limp and stays on the ground instead of fading out, as with Player Ragdoll Physics by Dweet. Other enemies already stay on the ground; bosses keep their own death. Applies from the next death."},
        {L"Keep bodies (beta)", L"BETA: bosses and other creatures that fade out or dissolve after death (skeletons, crabs, trolls, dragons...) stay on the ground instead, in the last pose of their death. You, Torrent, spirit ashes, allies summoned with you and ghostly characters still fade as in the game. Not tested with every boss yet: if a boss's runes, fog wall or event do not come, turn it off. Applies from the next death."},
        {L"Skill speed (%)", L"Your weapon skills (Ashes of War and unique skills). They use this instead of Attack speed, so normal attacks keep theirs. Multiplies the overall speed."},
        {L"Healing aura (HP/s)", L"The Deathbed Dress's aura without wearing it, stronger: you and allies within 7 m, spirit ashes included, recover this many HP every second. The dress alone gives allies 2 HP/s and you nothing. 0 turns it off. Applies within a second."},
        {L"Materials found (%)", L"How many crafting materials you get from each gathering spot, corpse, chest and enemy drop: 200% gives twice as many. Only materials used in crafting recipes. Item drop rate changes how often enemies drop things. Applies to the next pick-up."},
        {L"Craft in combat", L"Item Crafting stays open in the menu while enemies are fighting you, as out of combat. Applies at once."},
        {L"Map in combat", L"The map opens while enemies are fighting you, and stays open when a fight starts. Fast travel still follows the game's rules (or Fast travel anywhere). Applies at once."},
        {L"Enemy chase distance (%)", L"How far enemies follow you away from where they stand before they give up and walk back: 300% is three times as far. They still stop where they cannot walk (cave mouths, cliffs, the edge of their area). To be sure it applies, rest at a site of grace or reload the area after changing it."},
        {L"Most regular ashes at once", L"With Several ashes at once: how many regular spirit ashes (upgraded with Grave Glovewort) can be out together. Summoning one more dismisses one of them with a short fade; renowned ashes stay. 0 = no limit."},
        {L"Most renowned ashes at once", L"With Several ashes at once: how many renowned spirit ashes (upgraded with Ghost Glovewort, like the Mimic Tear or Black Knife Tiche) can be out together. Summoning one more dismisses one of them with a short fade; regular ashes stay. 0 = no limit."},
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
        {L"Movimiento y velocidad", L"Tu velocidad de animación, general y por acción, y la altura y distancia de tus saltos."},
        {L"Esquiva y defensa", L"Más invencibilidad al esquivar y ventana de parada, guardia perfecta, y protección derribado o al usar objetos."},
        {L"Torrent", L"Supervivencia, carrera y saltos de Torrent."},
        {L"Daño infligido", L"Tu daño, por elemento, escalado de atributos, retroceso y matar de un golpe."},
        {L"Daño recibido", L"El daño enemigo que recibes, por elemento, retroceso y morir de un golpe."},
        {L"Equilibrio", L"Tu equilibrio y el de los enemigos, y el daño al equilibrio de ambos lados."},
        {L"Estados alterados", L"Acumulación de cada estado para todos, tu inmunidad y la de los enemigos."},
        {L"Efectos de arma", L"Añade estados alterados a tus ataques, cada uno con su interruptor."},
        {L"Mejoras y duraciones", L"Cuánto duran las mejoras, grasas y efectos sobre enemigos, o hacerlos permanentes."},
        {L"Enemigos", L"Vida, velocidad, comportamiento y reaparición de los enemigos."},
        {L"Cenizas espirituales", L"Coste de invocación, varias cenizas a la vez y dónde y cuándo puedes llamar a los espíritus."},
        {L"Runas, tiendas y mejoras", L"Runas, objetos que sueltan los enemigos, precios de tiendas, mejoras y fabricación."},
        {L"Mapa y viaje", L"Gracias, mapa revelado, viaje rápido, y el espejo, subir de nivel y el renacimiento en cualquier lugar."},
        {L"Mundo", L"Hora del día y clima."},
        {L"Cámara", L"Campo de visión, comportamiento de la cámara y fijado de objetivo."},
        {L"Imagen", L"Posprocesado y nivel de detalle."},
        {L"Pantalla", L"FPS, frecuencia de refresco y relación de aspecto."},
        {L"Interfaz y sistema", L"Disposición del HUD y los menús, pausa, logos de inicio y preguntas Sí/No."},
        {L"Tienda de objetos", L"Todos los objetos que se pueden conseguir, ordenados por tipo, en una tienda que se abre desde el menú de diálogo (F11 > Tienda de objetos). El nivel de mejora y el precio se ajustan aquí o en Ajustes de la tienda, dentro de ese menú."},
    },
    {
        {L"Campo de visión personalizado", L"Escala el campo de visión del juego. Se aplica al momento."},
        {L"Campo de visión (%)", L"100% es el valor del juego. Los efectos de zoom se conservan."},
        {L"Desactivar autorrotación de cámara", L"La cámara deja de girar sola detrás de ti al moverte."},
        {L"Desactivar recentrado de cámara", L"Fijar sin objetivo ya no recoloca la cámara hacia delante."},
        {L"Distancia de cámara (%)", L"Escala lo lejos que se sitúa la cámara detrás de ti. Se aplica cuando cambia la cámara: montar, fijar objetivo o cambiar de zona."},
        {L"Quitar viñeta", L"Elimina el oscurecimiento de los bordes de la pantalla. Con Solid Uncapper activo, hace que avise al iniciar de que no pudo aplicar Vignette. Es inofensivo; para evitarlo, apaga esta opción y usa Disable Vignette de Solid Uncapper."},
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
        {L"Daño infligido (%)", L"Escala tus ataques y hechizos. Solo a ti: los PNJ humanoides y los invasores, que usan los mismos ataques que los jugadores, no cambian. Se aplica desde el siguiente golpe."},
        {L"Daño recibido (%)", L"Escala el daño que recibes de los enemigos, incluidos los PNJ humanoides y los invasores. Solo a ti: los ataques entre PNJ y a tus cenizas espirituales no cambian."},
        {L"Daño al equilibrio infligido (%)", L"Escala el daño al equilibrio de tus ataques, armas y hechizos: lo rápido que hacen tambalear a los enemigos y les rompen la postura. Se aplica en el siguiente golpe."},
        {L"Daño al equilibrio recibido (%)", L"Escala el daño al equilibrio de los ataques enemigos: lo rápido que te hacen tambalear. También afecta a PNJ y cenizas espirituales. Se aplica en el siguiente golpe."},
        {L"Coste de aguante (%)", L"Escala el aguante que gastan tus ataques y acciones."},
        {L"Coste de PC de hechizos (%)", L"Escala el coste en PC de hechicerías y encantamientos."},
        {L"Acumulación de estados (%)", L"Veneno, putrefacción, hemorragia, muerte, congelación, sueño y locura. Afecta a todos, también a ti."},
        {L"Matar de un golpe", L"Tus golpes que hacen daño derrotan a los enemigos al instante (empujarlos o rodar contra ellos no)."},
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
        {L"Varias cenizas a la vez", L"Multiple Spirit Ashes, integrado. Al usar otra ceniza espiritual se quedan las que ya están invocadas, y cada una cobra sus PC; volver a usar el objeto de una invocada retira solo esa con un fundido corto, aunque no tengas PC. No salen confirmaciones, las cenizas te siguen al cambiar de zona y al cruzar puertas, y cada una tiene una fila con su nombre y su vida en el HUD (hasta 7). El HUD solo cambia al reiniciar el juego: las filas salen si la opción está activada al arrancar y, si no, se queda el panel de cenizas del juego (volver al menú principal no basta). Desactivada, invocar funciona como en el juego original."},
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
        {L"Quitar inmunidades enemigas", L"Los enemigos inmunes a veneno, putrefacción, hemorragia, congelación o sueño pasan a tener una resistencia normal, también los que tienen la inmunidad por un efecto propio, como las gárgolas y los avatares del Árbol Áureo. Locura y muerte siguen afectando solo a personajes tipo jugador. Se aplica a los enemigos cargados después: descansa en una gracia."},
        {L"Curación del frasco carmesí (%)", L"Escala la vida que restaura el frasco de lágrimas carmesí, en todos sus niveles de mejora. 0% no cura."},
        {L"PF del frasco cerúleo (%)", L"Escala los PF que restaura el frasco de lágrimas cerúleas, en todos sus niveles de mejora. 0% no restaura."},
        {L"Coste de aguante de hechizos (%)", L"Escala el aguante que gastan hechicerías y encantamientos, también al cargarlos."},
        {L"Hechizos en una sola ranura", L"Los hechizos que ocupan 2 o 3 ranuras de memoria pasan a ocupar 1. El resto no cambia."},
        {L"Ignorar requisitos de hechizos", L"Lanza hechizos sin la Inteligencia ni la Fe que piden. No incluye los requisitos de Arcano."},
        {L"Coste de PC de habilidades (%)", L"Escala los PC que gastan las cenizas de guerra, también al cargarlas y en los golpes de seguimiento. 0% las hace gratis y te deja siempre al menos 1 PC, que el juego necesita para la versión completa de la habilidad."},
        {L"Llevar hasta 999 de cada objeto", L"Los objetos que se acumulan admiten 999 encima y en el cofre. Los objetos únicos (llaves, silbato, frascos) no cambian."},
        {L"Velocidad de uso de objetos (%)", L"Beber frascos, usar consumibles y recoger objetos. Se multiplica con la velocidad general."},
        {L"Precios de los mercaderes (%)", L"Escala lo que cobran los mercaderes y demás tiendas, en runas o en objetos como los corazones de dragón. Nada cuesta menos de lo que se vende, porque el juego lo ocultaría, así que 0% solo deja gratis lo que no se puede vender. Los intercambios por recuerdos siguen pidiéndolos. Se aplica la próxima vez que abras una tienda."},
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
        {L"Los enemigos no reaparecen", L"Los enemigos que matas siguen muertos tras descansar en una gracia, pasar el tiempo, morir, viajar rápido o cargar la partida. Cada partida guarda su lista en DLLs\\NativeTweaks\\dead-enemies; un enemigo que vuelve a cargarse desaparece al momento: sin cuerpo, runas ni botín. Los jefes y los PNJ funcionan como siempre, y los que siguen vivos se reinician como siempre. Al desactivarla, el siguiente descanso los devuelve a todos. Activarla o desactivarla durante la partida empieza la lista de cero (la anterior se guarda como .bak), así que solo cuentan los enemigos que matas con ella activa; si está activa desde que arranca el juego, la lista se conserva entre sesiones."},
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
        {L"Morir de un golpe", L"Cualquier golpe que haga daño te mata, venga de enemigos, PNJ humanoides o invasores. Solo a ti: los enemigos y tus cenizas espirituales no cambian. Anula «Daño recibido»; el modo dios te sigue protegiendo."},
        {L"Los enemigos rompen tu equilibrio de un golpe", L"Cada golpe enemigo te hace tambalear. Tus cenizas espirituales usan los mismos datos y también rompen el equilibrio enemigo de un golpe. Anula «Daño al equilibrio recibido»."},
        {L"Los ataques cargados lanzan a los enemigos", L"Tus ataques fuertes cargados lanzan a los enemigos por los aires en vez de hacerlos tambalear: hacia atrás, lejos o hacia arriba. Funciona aunque no pierdan el equilibrio. Los enemigos que ya están en el suelo, los grandes y los jefes no tienen esa animación y mantienen su reacción. «Lejos» es la que mejor lanza; «Distancia del lanzamiento», más abajo, decide lo lejos que vuelan. Se aplica desde tu siguiente golpe."},
        {L"Todos tus ataques lanzan", L"Con la opción de arriba activada, todos tus ataques que hacen reaccionar al enemigo lo lanzan igual, no solo los fuertes cargados: ataques ligeros y fuertes, habilidades, hechizos y objetos arrojadizos."},
        {L"Distancia del lanzamiento (%) (beta)", L"BETA, en desarrollo: su objetivo es que los enemigos lanzados vuelen más lejos. Por ahora solo los hace más ligeros mientras la opción de lanzar de arriba está activa (al 200 % pesan la mitad; lo toman al aparecer, así que descansa en una gracia), y en las pruebas aún no ha cambiado lo lejos que vuelan. Déjala al 100 % salvo que quieras experimentar."},
        {L"Dificultad del mundo", L"Los enemigos tienen la fuerza de un ciclo de Nueva Partida+, hasta NG+7, zona por zona como en la Nueva Partida+ de verdad: más vida, daño, defensa y resistencia a estados, y más runas. Tu partida sigue en su propio ciclo: jefes, objetos y finales no cambian. En Nueva Partida+ solo añade lo que le falta a tu ciclo. Se aplica a los enemigos que aparecen después: descansa en una gracia."},
        {L"Mantener tus runas", L"Con Dificultad del mundo activa, los enemigos dan las runas del ciclo de tu partida en lugar de las del ciclo más difícil."},
        {L"Abrir el espejo", L"Abre la pantalla de apariencia del espejo de la Mesa Redonda. Úsalo fuera de combate."},
        {L"Renacer", L"Abre la pantalla de renacimiento de Rennala para redistribuir tus atributos. No necesita Lágrima larval. Úsalo fuera de combate."},
        {L"Vida de los jefes (%)", L"Escala solo la vida de los jefes: principales, de campo, de mazmorra y del DLC. Se multiplica con «Vida de enemigos», así que con «Vida de enemigos» al 100 % solo cambian los jefes. Se aplica a los jefes que se carguen después: descansa en una gracia. El juego detiene la vida en 524287: subir más no añade nada."},
        {L"Personajes limpios (beta)", L"BETA: se aplica a todos, enemigos incluidos; para tu personaje solo, usa Mantener limpio a tu personaje. La sangre, el barro y las quemaduras ya no se pegan al cuerpo de los personajes, ni al tuyo ni al de los enemigos. Las salpicaduras siguen saliendo en el suelo y las paredes. Se aplica a los golpes nuevos; las manchas que ya tienes se van como siempre."},
        {L"Ocultar efectos de mejoras", L"Tus mejoras (encantamientos, hechizos, bolos, talismanes y similares) ya no muestran partículas ni brillo en tu cuerpo. Funcionan exactamente igual, mantienen su sonido y la invisibilidad sigue funcionando. Las mejoras del arma mantienen su aspecto (ver la opción de abajo). Se aplica a la siguiente mejora que uses."},
        {L"Ocultar efectos de mejoras de arma", L"Las grasas y los encantamientos de arma ya no muestran su brillo ni partículas en el arma. Funcionan exactamente igual y mantienen su sonido. Se aplica a la siguiente grasa o encantamiento que uses."},
        {L"Invencibilidad al esquivar (%) (beta)", L"BETA: alarga la invencibilidad de las volteretas, los pasos atrás y las habilidades de esquiva. El 100 % es la del juego (14 fotogramas en una voltereta ligera o media, 13 en una pesada); el 200 % la duplica. El tiempo extra sigue a la invencibilidad de la propia esquiva y dura aunque hagas otra acción justo después. Solo para tu personaje."},
        {L"Ventana de parada (%) (beta)", L"BETA: alarga la ventana de parada de los escudos y las habilidades de parada. El 100 % es la del juego; el 200 % la duplica. El tiempo extra sigue a la ventana de la propia parada. Solo para tu personaje."},
        {L"Invencible mientras estás derribado (beta)", L"BETA: los golpes no te hacen daño mientras un golpe fuerte te tiene en el suelo, incluido el levantarte, como durante una voltereta. Solo los derribos fuertes, no los agarres. Solo para tu personaje."},
        {L"Superarmadura al usar objetos (beta)", L"BETA: la superarmadura del juego mientras bebes un frasco o usas un objeto: los golpes más débiles ya no te interrumpen, los muy fuertes sí pueden, y el daño lo recibes igual. Solo para tu personaje."},
        {L"Rangos ampliados", L"Desactivado por defecto. Muchos deslizadores de este menú llegan más lejos y tienen debajo una fila de Escala (x1 a x100) que multiplica su valor. Se aplica al reiniciar el juego."},
        {L"Resaltar opciones cambiadas", L"Desactivado por defecto. Los nombres de las opciones que no están en el valor del juego salen en color, para ver de un vistazo qué has cambiado. Se aplica al momento."},
        {L"Objeto de una ceniza invocada", L"Con «Varias cenizas a la vez», qué hace usar el objeto de una ceniza que ya está invocada. La retira (por defecto): solo se va esa ceniza. La invoca otra vez: tienes la misma ceniza una vez más, y cada una cobra sus PC; «Retirar todas las cenizas» las quita todas. Lo decide el juego: el objeto se deja al juego, o a un mod que le dé otro uso, en vez de retirar la ceniza."},
        {L"Lucha libre", L"Desactivada por defecto. Dónde pelean libres las cenizas invocadas, sin que un teletransporte a tu lado corte la pelea: Arenas de jefe, solo ahí; En todas partes, en arenas de jefe y en cualquier pelea a pie. Al acabar una pelea, las que se hayan quedado lejos vuelven contigo. En todos los modos siguen cruzando la niebla contigo. Se aplica al momento."},
        {L"Ventana de desvío (%) (beta)", L"BETA: con la Lágrima de desvío, alarga la ventana de la guardia perfecta. El 100 % es la del juego (6 fotogramas al alzar la guardia, menos tras varios desvíos seguidos); el 200 % duplica la primera ventana. Solo para tu personaje."},
        {L"Cenizas sin aspecto fantasmal", L"Las cenizas espirituales invocadas (y las marionetas de Seluvis) pierden su aspecto fantasmal azul y se ven como personajes sólidos. Funcionan exactamente igual. Se aplica a la siguiente ceniza que invoques."},
        {L"Acumular mejoras propias (beta)", L"BETA: tus mejoras del mismo tipo ya no se sustituyen entre sí; por ejemplo, Llama, dame fuerza, Barrera de oro y un Cangrejo hervido, o Voto dorado y un Aromático estimulante, siguen activas a la vez. Usar otra vez la misma mejora solo la renueva. Se aplica a las siguientes mejoras que uses."},
        {L"Muertes antes de no reaparecer", L"Con «Los enemigos no reaparecen»: cuántas veces tienes que matar a un enemigo para que deje de volver, como en Dark Souls II (allí 12). 1 es a la primera. Cada partida cuenta las muertes en el mismo archivo que la lista."},
        {L"Personajes fantasmales sólidos (beta)", L"BETA: los personajes que se ven como fantasmas, como algunos jefes de las cárceles eternas, enemigos espectrales, Melina y los PNJ que invocas, pierden ese aspecto y se ven sólidos. Funcionan exactamente igual. Las cenizas espirituales tienen su propia opción más arriba. Se aplica en un segundo."},
        {L"Mantener limpio a tu personaje (beta)", L"BETA: la sangre, el barro y las quemaduras ya no se quedan en el cuerpo de tu personaje; los enemigos se manchan como siempre. Funciona como el Jabón del juego, todo el rato: una marca de un golpe desaparece en menos de un cuarto de segundo. Se aplica al momento. Las salpicaduras siguen saliendo en el suelo."},
        {L"Subir de nivel", L"Abre la pantalla de subir de nivel que ofrece Melina en una gracia, para gastar runas en tus atributos en cualquier lugar. Úsalo fuera de combate. Puedes asignarle una tecla en [Hotkeys] de NativeTweaks.ini (OpenLevelUp = F9)."},
        {L"Invocar a Melina (beta)", L"BETA: invoca a Melina como ceniza acompañante que pelea a tu lado, usando la Melina de combate que el juego tiene sin usar. Funciona mejor con Invocar en cualquier lugar y Varias cenizas a la vez. Usa sus propios ataques con la IA de ceniza de Tiche."},
        {L"Melina acompañante (beta)", L"BETA: Melina está siempre contigo como ceniza acompañante. Se vuelve a invocar sola cuando no está a tu lado: después de descansar en una gracia (se sienta a tu lado mientras descansas), tras una pantalla de carga o si cae. Funciona mejor con Invocar en cualquier lugar y Varias cenizas a la vez. Retirar todas las cenizas la aparta hasta que vuelva."},
        {L"Acumular mejoras de arma (beta)", L"BETA: las mejoras de una misma arma ya no se sustituyen entre sí; por ejemplo, una grasa y un hechizo de arma o la mejora de una habilidad siguen activas a la vez, cada una con su daño. El arma muestra solo una. Se aplica a las siguientes mejoras que uses."},
        {L"Velocidad de esprint (%)", L"Solo al esprintar, encima de «Velocidad de movimiento»: 200 % aquí con movimiento al 150 % esprinta al 300 %. Andar y correr siguen con «Velocidad de movimiento»."},
        {L"Velocidad al andar (%)", L"Solo al andar, encima de «Velocidad de movimiento»."},
        {L"Velocidad al correr (%)", L"Solo al correr, encima de «Velocidad de movimiento»."},
        {L"Velocidad de interacción (%)", L"Tirar de palancas, abrir puertas y cofres, recoger objetos, levantarse de una gracia. Se multiplica por la velocidad general. Sentarse sigue a su velocidad. Solo acelera a tu personaje: el cofre o la puerta y los efectos de alrededor siguen a su velocidad."},
        {L"Alcance de proyectiles (%)", L"Hasta dónde llegan tus hechizos, flechas y objetos arrojadizos antes de desaparecer. Su velocidad y su daño no cambian. Usa el mismo valor que en «Distancia de fijado» para que lleguen a lo que fijas: al 500 % ambas llegan a unos 250 m. Algunos hechizos caen en arco y aun así pueden tocar antes el suelo."},
        {L"Distancia de fijado (%)", L"A qué distancia puedes fijar a los enemigos y mantenerlos fijados. «Sin fijar objetivo» tiene prioridad."},
        {L"Quitar enfoque forzado", L"Desactiva el filtro de enfoque que el juego aplica tras el antialiasing. La imagen se ve más suave, con menos halos en los bordes."},
        {L"Embrujar a todos los enemigos (beta)", L"BETA: las Ramas embrujadoras también funcionan con enemigos que el juego hace inmunes, como Caballeros negros o esqueletos. Afecta a los enemigos que aparezcan después de activarla: descansa en una gracia. Algunos pueden no luchar bien mientras están embrujados."},
        {L"Embrujar a los jefes (beta)", L"BETA: las Ramas embrujadoras también funcionan con los jefes. Un jefe embrujado puede bloquear sus fases o un combate con guion hasta 3 minutos. Descansa en una gracia después de cambiarla."},
        {L"Objetos a velocidad de interacción (beta)", L"BETA: cofres, puertas y palancas también van a la velocidad de interacción. Los ascensores no cambian. Los efectos de alrededor tampoco. Otros objetos que empiecen a moverse en ese momento pueden acelerarse un instante. Apagada por defecto."},
        {L"Equilibrio en Nueva Partida+ (%) (beta)", L"BETA: desde NG+2 el juego cambia el equilibrio de los enemigos, de x0,95 en NG+2 a x0,6 en NG+7. Al 100 % se mantiene ese cambio y al 0 % se quita, así que los enemigos tienen el equilibrio de una primera partida. Solo afecta a una partida en Nueva Partida+. Se aplica a los enemigos que aparecen después: descansa en una gracia."},
        {L"Quitar el temblor de cámara (beta)", L"BETA: los ataques, las caídas y los eventos ya no hacen temblar la cámara. También se quita la vibración del mando que producen. Se aplica al momento a los temblores nuevos."},
        {L"Soltar o tirar cualquier objeto (beta)", L"BETA: cualquier arma, armadura, talismán, ceniza de guerra u objeto se puede soltar o tirar, salvo los objetos clave, las Grandes Runas y los frascos. Lo que sueltas se queda en el suelo hasta que sales de la zona; lo que tiras se pierde para siempre. Se aplica al momento."},
        {L"...también objetos clave (beta)", L"BETA: también los objetos clave, las Grandes Runas y los frascos. Si tiras un objeto que necesita la historia, como un medallón o una llave, puedes quedarte sin poder avanzar. Un frasco en el acceso rápido se queda con uno: quítalo del acceso para soltar el último. Necesita la opción de arriba."},
        {L"Nivel de las armas", L"Nivel de mejora de las armas de la tienda de objetos (F11 > Tienda de objetos). Las armas sombrías siguen su propia escala: +25 aquí equivale a +10 en ellas. Las que no se pueden mejorar se quedan en +0. Se aplica la próxima vez que abras la tienda."},
        {L"Nivel de las cenizas", L"Nivel de mejora de las cenizas de espíritu de la tienda de objetos, hasta +10 o el máximo de cada ceniza. Se aplica la próxima vez que abras la tienda."},
        {L"Precio (%)", L"Precio de todo lo que hay en la tienda de objetos, como porcentaje de su valor de venta. Con 0% todo es gratis. Se aplica la próxima vez que abras la tienda."},
        {L"NPC Spawner", L"Desactivado por defecto. Invoca cualquier enemigo, jefe o PNJ donde estés, para probar combates y ajustes: F11 > NPC Spawner, o Mod Settings > NPC Spawner, con sus propios ajustes (NpcSpawner.ini junto a NativeTweaks.dll). Reinicia el juego después de cambiarlo. No disponible si NpcSpawner.dll está instalado por separado: se usa ese."},
        {L"Cuerpos inertes al morir (beta)", L"BETA: cuando mueres tú o un personaje humano (PNJ, invasores, otros Sinluz), el cuerpo cae inerte y se queda en el suelo en lugar de desvanecerse, como con Player Ragdoll Physics de Dweet. Los demás enemigos ya se quedan en el suelo; los jefes mantienen su propia muerte. Se aplica desde la siguiente muerte."},
        {L"Mantener los cuerpos (beta)", L"BETA: los jefes y otras criaturas que se desvanecen o se deshacen al morir (esqueletos, cangrejos, trols, dragones...) se quedan en el suelo, en la última postura de su muerte. Tú, Torrente, las cenizas de espíritu, los aliados invocados contigo y los personajes fantasmales se siguen desvaneciendo como en el juego. Aún no se ha probado con todos los jefes: si no llegan las runas, la niebla o el evento de un jefe, desactívala. Se aplica desde la siguiente muerte."},
        {L"Velocidad de habilidades (%)", L"Tus habilidades de arma (cenizas de guerra y habilidades únicas). Usan esto en lugar de la velocidad de ataque, así que los ataques normales mantienen la suya. Se multiplica por la velocidad general."},
        {L"Aura curativa (PV/s)", L"El aura del Vestido del lecho de muerte sin llevarlo, y más fuerte: tú y los aliados a menos de 7 m, cenizas de espíritu incluidas, recuperáis estos PV cada segundo. El vestido solo da 2 PV/s a los aliados y nada a quien lo lleva. Con 0 se apaga. Se aplica en un segundo."},
        {L"Materiales obtenidos (%)", L"Cuántos materiales de fabricación consigues de cada punto de recolección, cadáver, cofre y botín de enemigos: con 200% el doble. Solo los materiales que usan las recetas. La tasa de botín cambia cada cuánto sueltan cosas los enemigos. Se aplica a la siguiente recogida."},
        {L"Fabricar en combate", L"Fabricación de objetos sigue disponible en el menú mientras luchas contra enemigos, igual que fuera de combate. Se aplica al momento."},
        {L"Mapa en combate", L"El mapa se abre mientras luchas contra enemigos y no se cierra cuando empieza un combate. El viaje rápido sigue las reglas del juego (o Viaje rápido en cualquier sitio). Se aplica al momento."},
        {L"Distancia de persecución (%)", L"Hasta dónde te siguen los enemigos lejos de su sitio antes de rendirse y volver: al 300 % llegan tres veces más lejos. Siguen parándose donde no pueden caminar (entradas de cuevas, acantilados, el borde de su zona). Para asegurarte de que se aplica, descansa en un lugar de gracia o recarga la zona tras cambiarla."},
        {L"Máximo de cenizas normales", L"Con «Varias cenizas a la vez»: cuántas cenizas normales (las que se mejoran con Hierba sepulcral) puede haber a la vez. Al invocar una más, una de ellas se retira con un desvanecimiento corto; las renombradas se quedan. 0 = sin límite."},
        {L"Máximo de cenizas renombradas", L"Con «Varias cenizas a la vez»: cuántas cenizas renombradas (las que se mejoran con Hierba sepulcral fantasmal, como el Espejismo o Tiche) puede haber a la vez. Al invocar una más, una de ellas se retira con un desvanecimiento corto; las normales se quedan. 0 = sin límite."},
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
        {L"移动与速度", L"你的动作速度（整体和各类动作），以及跳跃高度与距离。"},
        {L"闪避与防御", L"更长的闪避无敌与弹反判定、完美防御，以及倒地或使用道具时的保护。"},
        {L"灵马", L"灵马的生存、奔跑与跳跃。"},
        {L"造成伤害", L"你的伤害、各属性伤害、能力补正、击退和一击必杀。"},
        {L"受到伤害", L"你受到的敌人伤害、各属性伤害、击退和一击即死。"},
        {L"韧性", L"你和敌人的韧性，以及双方造成的削韧。"},
        {L"异常状态", L"所有人的异常状态累积速度、你的免疫和敌人免疫。"},
        {L"武器效果", L"为你的攻击附加异常状态，每种都有单独的开关。"},
        {L"增益与持续时间", L"增益、油脂和敌人减益的持续时间，或使其永久生效。"},
        {L"敌人", L"敌人的血量、速度、行为与重生。"},
        {L"骨灰", L"召唤消耗、同时召唤多个骨灰，以及何时何地可以召唤骨灰。"},
        {L"卢恩、商店与强化", L"卢恩、道具掉落、商店价格、强化与制作。"},
        {L"地图与传送", L"赐福、地图显示、传送，以及随时使用镜子、升级与重生。"},
        {L"世界", L"时间与天气。"},
        {L"镜头", L"视野、镜头行为与锁定。"},
        {L"画面", L"后期处理与细节层次。"},
        {L"显示", L"帧率、刷新率与画面比例。"},
        {L"界面与系统", L"HUD 与菜单布局、暂停、启动标志和是/否对话框。"},
        {L"物品商店", L"在对话菜单（F11 > 物品商店）中打开的商店，按类型列出所有可获得的物品。可在此处或该菜单内的“商店设置”中调整强化等级和价格。"},
    },
    {
        {L"自定义视野", L"缩放游戏视野。立即生效。"},
        {L"视野 (%)", L"100% 为游戏原值。保留缩放效果。"},
        {L"禁用镜头自动旋转", L"移动时镜头不再自动转到身后。"},
        {L"禁用镜头复位", L"没有目标时按锁定不再将镜头转向前方。"},
        {L"镜头距离 (%)", L"缩放镜头与角色的距离。在镜头切换时生效：骑马、锁定或进入其他区域。"},
        {L"移除暗角", L"移除画面边缘的变暗效果。启用 Solid Uncapper 时，它会在启动时提示无法应用 Vignette。这没有害处；如要避免，请关闭此选项并使用 Solid Uncapper 的 Disable Vignette。"},
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
        {L"造成伤害 (%)", L"缩放你的攻击和法术伤害。只作用于你：使用与玩家相同攻击的人形 NPC 和入侵者不受影响。从下一次命中开始生效。"},
        {L"受到伤害 (%)", L"缩放你受到的敌人伤害，包括人形 NPC 和入侵者。只作用于你：NPC 之间以及对骨灰的攻击不变。"},
        {L"造成削韧 (%)", L"缩放你的攻击、武器和法术的削韧：使敌人踉跄、破坏其架势的速度。下一次攻击即生效。"},
        {L"受到削韧 (%)", L"缩放敌人攻击的削韧：使你踉跄的速度。也会影响 NPC 和骨灰。下一次攻击即生效。"},
        {L"精力消耗 (%)", L"缩放攻击和行动消耗的精力。"},
        {L"法术 FP 消耗 (%)", L"缩放魔法和祷告的 FP 消耗。"},
        {L"异常状态累积 (%)", L"中毒、腐败、出血、咒死、冻伤、睡眠和发狂。对所有人生效，包括你自己。"},
        {L"一击必杀", L"你造成伤害的攻击会立即击败敌人（推挤或翻滚撞到敌人不算）。"},
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
        {L"同时召唤多个骨灰", L"内置 Multiple Spirit Ashes。使用另一个骨灰时，已召唤的骨灰会保留，且每个都会消耗 FP；再次使用已召唤骨灰的道具会以短暂淡出只解除该骨灰，即使 FP 不足也可以。不会弹出确认，骨灰会跟随你进入新区域和穿过门，并在 HUD 上各有一行名称和生命值（最多 7 行）。HUD 仅在重新启动游戏后才会改变：游戏启动时开启此选项才会显示这些行，否则保留原版的骨灰面板（返回标题画面不够）。关闭后召唤与原版游戏相同。"},
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
        {L"移除敌人免疫", L"对中毒、腐败、出血、冻伤或睡眠免疫的敌人改为普通抗性，包括因自身效果而免疫的敌人，例如石像鬼和黄金树化身。发狂和咒死仍只对玩家类角色生效。对之后加载的敌人生效：在赐福处休息。"},
        {L"红露滴圣杯瓶回复 (%)", L"缩放红露滴圣杯瓶回复的血量，所有强化等级都适用。0% 为不回复。"},
        {L"蓝露滴圣杯瓶回复 (%)", L"缩放蓝露滴圣杯瓶回复的 FP，所有强化等级都适用。0% 为不回复。"},
        {L"法术精力消耗 (%)", L"缩放魔法和祷告消耗的精力，包括蓄力施放。"},
        {L"法术只占一个记忆栏", L"占用 2 或 3 个记忆栏的法术只占 1 个。其他法术不变。"},
        {L"忽略法术需求", L"无需满足智力和信仰需求即可施放法术。不包括感应需求。"},
        {L"战技 FP 消耗 (%)", L"缩放战灰的 FP 消耗，包括蓄力和追加攻击。0% 为免费，并始终保留至少 1 点 FP，游戏需要它来施放完整的战技。"},
        {L"每种道具可携带 999 个", L"可堆叠的道具在身上和仓库中都能存放 999 个。唯一道具（关键道具、灵马笛、圣杯瓶）不变。"},
        {L"道具使用速度 (%)", L"饮用圣杯瓶、使用消耗品和拾取物品的动作。与整体速度相乘。"},
        {L"商人价格 (%)", L"缩放商人和其他商店的要价，无论是卢恩还是龙心脏等道具。物品价格不会低于其售出价，否则游戏会将其隐藏，因此 0% 只会让无法出售的物品免费。用回忆交换的物品仍需要回忆。下次打开商店时生效。"},
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
        {L"敌人不再复活", L"被你击杀的敌人在赐福处休息、等待时间流逝、死亡、快速传送或读取存档后仍保持死亡。每个存档的名单保存在 DLLs\\NativeTweaks\\dead-enemies；再次加载的敌人会立即消失：没有尸体，不给予卢恩和掉落物。头目和 NPC 照常运作，仍然存活的敌人也会照常重置。关闭后，下一次休息会让所有敌人复活。在游戏中开启或关闭此选项会让该存档的名单重新开始（旧名单保存为 .bak），因此只计算开启期间击杀的敌人；若从游戏启动时就已开启，名单会在多次游玩之间保留。"},
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
        {L"一击即死", L"任何造成伤害的攻击都会杀死你，无论来自敌人、人形 NPC 还是入侵者。只作用于你：敌人和你的骨灰不受影响。覆盖“受到伤害 (%)”；无敌模式仍会保护你。"},
        {L"敌人一击破你韧性", L"敌人的每次攻击都会使你踉跄。你的骨灰使用相同数据，也会一击击破敌人的韧性。覆盖“受到削韧 (%)”。"},
        {L"蓄力攻击击飞敌人", L"你的蓄力重攻击会将敌人击飞，而不是使其踉跄：向后、远距或向上。即使敌人韧性未被击破也有效。已倒地的敌人、大型敌人和头目没有击飞动作，保持自身的反应。“远距击飞”效果最好；下方的“击飞距离 (%)”决定距离。从下一次攻击起生效。"},
        {L"所有攻击都会击飞", L"开启上方选项时，你所有会让敌人产生反应的攻击都会以相同方式击飞敌人，而不只是蓄力重攻击：轻攻击、重攻击、战技、法术和投掷物品。"},
        {L"击飞距离 (%)（测试版）", L"测试版，开发中：目标是让被击飞的敌人飞得更远。目前它只会在开启上方击飞选项时减轻敌人重量（200% 时重量减半；敌人出现时读取，请在赐福处休息），测试中尚未改变击飞距离。除非想要试验，请保持 100%。"},
        {L"世界难度", L"敌人获得新游戏+周目的强度，最高至 NG+7，并像真正的新游戏+一样按区域计算：更多生命、伤害、防御和异常状态抗性，以及更多卢恩。你的存档保持原有周目：头目、物品和结局不变。在新游戏+中只补足你的周目所缺少的部分。对之后出现的敌人生效：请在赐福处休息。"},
        {L"保留你的卢恩", L"开启世界难度时，敌人给予你存档所在周目的卢恩，而不是更高难度周目的卢恩。"},
        {L"打开镜子", L"打开圆桌厅堂镜子的外观界面。请在战斗外使用。"},
        {L"重生", L"打开菈妮的重生界面以重新分配属性点。不需要幼虫之泪。请在战斗外使用。"},
        {L"头目血量 (%)", L"仅缩放头目的血量：主要头目、野外头目、地下城头目和 DLC 头目。与“敌人血量”相乘，因此“敌人血量”为 100% 时只改变头目。对之后加载的头目生效：在赐福处休息。游戏将血量上限限制为 524287，继续调高不会增加血量。"},
        {L"保持角色干净（测试版）", L"测试版：作用于所有角色，包括敌人；若只想让你的角色保持干净，请使用“保持你的角色干净”。血迹、泥污和焦痕不再附着在角色身上，包括你和敌人。地面和墙壁上仍会出现飞溅痕迹。对新的攻击生效；已有的痕迹会照常消失。"},
        {L"隐藏增益效果", L"你自身的增益（祷告、魔法、丸药、护符等）不再在身上显示粒子或光芒。效果完全相同，保留音效，隐身仍然有效。武器增益保持原样（见下方选项）。对你使用的下一个增益生效。"},
        {L"隐藏武器增益效果", L"油脂和武器附魔不再在武器上显示光芒或粒子。效果完全相同，保留音效。对你使用的下一个油脂或附魔生效。"},
        {L"闪避无敌时间 (%)（测试版）", L"测试版：延长翻滚、后撤步和闪避类战技的无敌时间。100% 为游戏原本的时间（轻、中翻滚 14 帧，重翻滚 13 帧）；200% 为两倍。额外时间紧接闪避本身的无敌时间，即使随后进行其他动作也会持续。仅对你的角色生效。"},
        {L"弹反判定时间 (%)（测试版）", L"测试版：延长盾牌和弹反类战技的弹反判定时间。100% 为游戏原本的时间；200% 为两倍。额外时间紧接弹反本身的判定时间。仅对你的角色生效。"},
        {L"倒地时无敌（测试版）", L"测试版：被重击击倒在地时（包括起身）不会受到伤害，与翻滚时相同。仅限重击倒地，不含抓取。仅对你的角色生效。"},
        {L"使用道具时霸体（测试版）", L"测试版：喝圣杯瓶或使用道具时获得游戏的霸体：较弱的攻击不再打断你，极强的攻击仍可能打断，伤害照常承受。仅对你的角色生效。"},
        {L"扩展范围", L"默认关闭。此菜单中的许多滑块可调范围更大，下方另有一行“倍率”（x1 至 x100）用于放大数值。重启游戏后生效。"},
        {L"高亮已更改的选项", L"默认关闭。不在游戏默认值的选项名称会以颜色显示，一眼就能看出改过什么。立即生效。"},
        {L"已召唤骨灰的道具", L"开启“同时召唤多个骨灰”时，再次使用已召唤骨灰的道具会怎样。解除（默认）：只解除该骨灰。再次召唤：再召唤一个相同的骨灰，每个都会消耗 FP；“解除所有骨灰”可将其全部解除。交给游戏：道具交由游戏（或为其赋予其他用途的模组）处理，而不是解除骨灰。"},
        {L"自由战斗", L"默认关闭。召唤的骨灰在哪里自由战斗，不会被传送回你身边而打断战斗：头目场地，仅在那里；全部，头目场地和任何徒步战斗。战斗结束后，留在远处的骨灰会回到你身边。所有模式下骨灰仍会和你一起穿过雾门。立即生效。"},
        {L"偏转判定时间 (%)（测试版）", L"测试版：使用偏转硬泪滴时，延长完美防御的判定时间。100% 为游戏原本的判定（举盾时 6 帧，连续偏转后会变短）；200% 将第一次的判定时间翻倍。仅作用于你的角色。"},
        {L"骨灰不再呈幽灵外观", L"召唤的骨灰（以及瑟留斯的人偶）不再呈现蓝色幽灵外观，而是显示为实体角色。效果完全不变。从下一次召唤的骨灰开始生效。"},
        {L"叠加自身增益（测试版）", L"测试版：同类的自身增益不再互相覆盖，例如“火焰，赐我力量”、“黄金屏障”和水煮螃蟹，或“黄金誓约”和振奋香药，都能同时生效。再次使用同一增益仍只会刷新时间。从下一次使用的增益开始生效。"},
        {L"不再复活前的击杀次数", L"开启“敌人不再复活”时：敌人需要被击杀多少次才不再复活，类似《黑暗之魂 II》（该作为 12 次）。1 表示第一次击杀后即不再复活。每个存档的击杀次数与名单保存在同一个文件中。"},
        {L"幽灵角色实体化（测试版）", L"测试版：以幽灵形态显示的角色（例如部分永恒监牢的头目、灵体敌人、梅琳娜和你召唤的 NPC）不再呈现幽灵外观，而是显示为实体。效果完全不变。骨灰有上方的单独选项。约一秒内生效。"},
        {L"保持你的角色干净（测试版）", L"测试版：血迹、泥土和焦痕不再留在你的角色身上；敌人照常沾染。效果如同游戏中的肥皂，并且持续生效：被击中留下的痕迹会在四分之一秒内消失。立即生效。地面上的溅射痕迹仍会出现。"},
        {L"升级", L"打开梅琳娜在赐福处提供的升级界面，可随时随地消耗卢恩提升属性。请在战斗外使用。也可以在 NativeTweaks.ini 的 [Hotkeys] 中绑定按键（OpenLevelUp = F9）。"},
        {L"召唤梅琳娜（测试版）", L"测试版：将梅琳娜作为骨灰同伴召唤，在你身边战斗，使用游戏中存在但未使用的战斗版梅琳娜。配合“随处召唤”与“同时召唤多个骨灰”效果最佳。她使用自己的攻击动作，并采用黑刀蒂契的骨灰AI。"},
        {L"梅琳娜同伴（测试版）", L"测试版：梅琳娜作为骨灰同伴始终陪伴你。当她不在你身边时会自动重新召唤：在赐福处休息后（你休息时她会坐在你身旁）、读取画面后或她倒下后。配合“随处召唤”与“同时召唤多个骨灰”效果最佳。“解除全部骨灰”会让她暂时离开，之后她会再次出现。"},
        {L"叠加武器增益（测试版）", L"测试版：同一把武器上的增益不再互相覆盖，例如油脂与武器附魔法术或战技增益可同时生效，各自提供伤害。武器只显示其中一种效果。从下一次使用的增益开始生效。"},
        {L"冲刺速度 (%)", L"仅作用于冲刺，并与“移动速度”相乘：移动速度为 150% 时，此处设为 200% 则冲刺速度为 300%。行走和奔跑仍使用“移动速度”。"},
        {L"行走速度 (%)", L"仅作用于行走，并与“移动速度”相乘。"},
        {L"奔跑速度 (%)", L"仅作用于奔跑，并与“移动速度”相乘。"},
        {L"互动速度 (%)", L"拉拉杆、开门、开宝箱、拾取物品、从赐福起身。与整体速度相乘。坐下保持原速。只加快你的角色：宝箱、门以及周围的特效保持原速。"},
        {L"投射物射程 (%)", L"你的法术、箭矢和投掷物消失前能飞多远。速度和伤害不变。与“锁定距离”设为相同数值即可打到锁定的目标：500% 时两者都约为 250 米。部分法术呈弧线下坠，仍可能先落地。"},
        {L"锁定距离 (%)", L"可以锁定敌人并保持锁定的最远距离。“禁用锁定”优先。"},
        {L"移除强制锐化", L"关闭游戏在抗锯齿之后应用的锐化滤镜。画面更柔和，边缘光晕更少。"},
        {L"魅惑所有敌人（测试版）", L"测试版：魅惑树枝也对游戏设为免疫的敌人生效，例如黑骑士或骷髅。只影响开启后出现的敌人：请在赐福处休息。部分敌人被魅惑时可能无法正常战斗。"},
        {L"魅惑头目（测试版）", L"测试版：魅惑树枝也对头目生效。被魅惑的头目可能使其阶段或脚本战斗停滞最多 3 分钟。更改后请在赐福处休息。"},
        {L"物体跟随互动速度（测试版）", L"测试版：宝箱、门和拉杆也按互动速度运动。升降机保持原速。周围的特效不变。同一时刻开始运动的其他物体可能短暂加速。默认关闭。"},
        {L"新游戏+ 架势调整 (%)（测试版）", L"测试版：从 NG+2 起，游戏会改变敌人的架势，从 NG+2 的 x0.95 到 NG+7 的 x0.6。100% 保留该改变，0% 将其移除，让敌人保持一周目的架势。仅对新游戏+存档有效。对之后出现的敌人生效：在赐福处休息。"},
        {L"关闭镜头震动（测试版）", L"测试版：攻击、落地和事件不再使镜头震动，由此产生的手柄震动也会停止。对新的震动立即生效。"},
        {L"丢弃或扔掉任意物品（测试版）", L"测试版：除重要道具和大卢恩外，所有武器、防具、护符、战灰和道具都可以丢弃或扔掉（圣杯瓶除外）。丢在地上的物品在离开区域前会一直留着；扔掉的物品会永久消失。立即生效。"},
        {L"……包括重要道具（测试版）", L"测试版：重要道具、大卢恩和圣杯瓶也可以丢弃。扔掉剧情需要的物品（如徽章或钥匙）可能导致无法继续推进。装备在快捷栏中的圣杯瓶会保留一个：从快捷栏卸下后才能丢弃最后一个。需要先开启上一个选项。"},
        {L"武器强化等级", L"物品商店（F11 > 物品商店）中武器的强化等级。失色武器按自己的等级换算：这里的 +25 对它们是 +10。无法强化的武器保持 +0。下次打开商店时生效。"},
        {L"骨灰强化等级", L"物品商店中骨灰的强化等级，最高 +10 或各骨灰的最高等级。下次打开商店时生效。"},
        {L"价格 (%)", L"物品商店中所有物品的价格，按出售价值的比例计算。0% 时全部免费。下次打开商店时生效。"},
        {L"NPC 召唤器", L"默认关闭。在任何地方召唤任意敌人、头目或 NPC，用于测试战斗和设置：F11 > NPC Spawner，或 Mod Settings > NPC Spawner，有其自己的设置（NativeTweaks.dll 旁的 NpcSpawner.ini）。更改后需重启游戏。若单独安装了 NpcSpawner.dll，则此项不可用，使用该文件。"},
        {L"死亡布娃娃（测试版）", L"测试版：你或人形角色（NPC、入侵者、其他褪色者）死亡时，身体会瘫倒并留在地上，而不是逐渐消失，效果同 Dweet 的 Player Ragdoll Physics。其他敌人本来就会留在地上；头目保持其原有的死亡方式。从下一次死亡开始生效。"},
        {L"保留尸体（测试版）", L"测试版：死亡后会逐渐消失或消散的头目及其他生物（骷髅、螃蟹、巨魔、龙等）会以死亡时的最后姿势留在地上。你、灵马、骨灰、与你一同召唤的同伴以及幽灵角色仍会像游戏中一样消失。尚未在所有头目上测试：如果头目的卢恩、雾门或事件没有出现，请关闭此项。从下一次死亡开始生效。"},
        {L"战技速度 (%)", L"你的武器战技（战灰与专属战技）。它们使用此项而不是攻击速度，普通攻击保持各自的速度。与整体速度相乘。"},
        {L"治愈光环（HP/秒）", L"无需穿着即可获得更强的临终礼服光环：你和 7 米内的同伴（包括骨灰）每秒恢复这么多血量。礼服本身只为同伴每秒恢复 2 点，不会治疗穿着者。设为 0 关闭。约一秒内生效。"},
        {L"获得材料 (%)", L"每个采集点、尸体、宝箱和敌人掉落给你的制作材料数量：200% 为两倍。仅限制作配方使用的材料。物品掉落率决定敌人掉落的频率。下次拾取时生效。"},
        {L"战斗中制作", L"与敌人战斗时，菜单中的物品制作仍然可用，与战斗外一样。立即生效。"},
        {L"战斗中查看地图", L"与敌人战斗时也能打开地图，战斗开始时地图也不会关闭。快速传送仍遵循游戏规则（或随处快速传送）。立即生效。"},
        {L"敌人追击距离（%）", L"敌人离开原位追击你多远才会放弃并返回：300% 即三倍距离。它们仍会在无法行走的地方停下（洞口、悬崖、区域边缘）。更改后在赐福处休息或重新载入区域以确保生效。"},
        {L"普通骨灰同时上限", L"开启“同时召唤多个骨灰”时：普通骨灰（用坟地钟形花强化）同时最多存在的数量。再召唤一个时，其中一个会短暂淡出离开；名人骨灰不受影响。0 = 无上限。"},
        {L"名人骨灰同时上限", L"开启“同时召唤多个骨灰”时：名人骨灰（用灵魂坟地钟形花强化，如仿身泪滴、黑刀蒂秋）同时最多存在的数量。再召唤一个时，其中一个会短暂淡出离开；普通骨灰不受影响。0 = 无上限。"},
    },
    {L"恢复游戏默认值", L"关闭所有原版选项并恢复游戏原始数值。"},
    {L"恢复默认值", L"将本页的选项恢复为游戏原始数值。"},
    L"此游戏版本不可用：未找到其修改的数据。",
};

// Menu pages in display order; each lists its options in display order.
struct Layout { Category category; std::initializer_list<Id> ids; };
const Layout kLayout[]{
    {cat_survival, {god_mode, no_death, no_fall_damage, max_hp, max_fp, max_stamina, hp_regen_points, healing_aura,
        fp_regen_points, stamina_regen_points, flask_hp, flask_fp}},
    {cat_equipment, {equip_weight, weapon_weight, weapon_requirements, spell_no_requirements, spell_one_slot,
        carry_max, hide_helmet, drop_any, drop_key_items}},
    {cat_resources, {infinite_stamina, infinite_fp, infinite_items, infinite_arrows, stamina_cost,
        spell_fp_cost, spell_stamina_cost, skill_fp_cost}},
    {cat_movement, {speed_global, speed_attack, speed_skill, speed_spell, speed_roll, speed_move, speed_walk, speed_run, speed_sprint,
        speed_jump, speed_item, speed_interact, interaction_objects,
        jump_height, jump_distance}},
    {cat_defense, {dodge_frames, parry_frames, deflect_window, perfect_guard, knockdown_invincible, item_super_armor}},
    {cat_torrent, {torrent_no_death, torrent_speed, torrent_jump_speed, torrent_jump_height,
        torrent_jump_distance}},
    {cat_dealt, {damage_dealt, dmg_physical, dmg_magic, dmg_fire, dmg_lightning, dmg_holy, weapon_scaling,
        knockback_dealt, charged_launch, launch_all, launch_distance, one_hit_kill, projectile_range}},
    {cat_taken, {damage_taken, taken_physical, taken_magic, taken_fire, taken_lightning, taken_holy,
        knockback_taken, die_one_hit}},
    {cat_poise, {player_poise, player_poise_lock, posture_taken, posture_one_hit_taken, posture_dealt,
        posture_one_hit, enemy_poise, enemy_poise_lock, ng_poise}},
    {cat_status, {status_buildup, buildup_bleed, buildup_poison, buildup_rot, buildup_frost, buildup_sleep,
        buildup_madness, buildup_blight, status_instant, status_immunity, enemies_no_immunity}},
    {cat_weapon, {weapon_bleed, weapon_poison, weapon_rot, weapon_frost, weapon_sleep, weapon_effects_enemies}},
    {cat_buffs, {buff_weapon, buff_weapon_forever, stack_weapon_buffs, buff_self, buff_self_forever, stack_buffs, buff_enemy}},
    {cat_enemies, {world_journey, journey_keep_runes, enemy_hp, boss_hp, enemy_speed, enemy_chase, charm_all_enemies, charm_bosses,
        enemies_no_attack, enemies_no_move,
        enemies_no_ai, enemies_no_respawn, enemy_respawn_kills, enemies_downed_vulnerable, npc_spawner}},
    // Summon Melina and Melina companion (summon_melina, melina_companion) stay out
    // of the menu: work in progress, moving to a separate mod (2026-10-28).
    {cat_spirits, {ash_cost, ash_multiple, ash_repeat, ash_free_fighting, ash_dismiss_all, ash_extra_cost, ash_damage_per_ash,
        ash_max_regular, ash_max_renowned,
        ash_anywhere, ash_keep_after_boss, ash_no_rest}},
    {cat_runes, {rune_loss_off, runes_gained, loot_drop, materials_found, shop_price, upgrade_cost, upgrade_free, craft_free,
        craft_in_combat}},
    {cat_item_shop, {shop_weapon_level, shop_spirit_level, shop_item_price}},
    {cat_map, {all_graces, reveal_map, map_in_combat, fast_travel_anywhere, open_mirror, open_level_up, open_rebirth}},
    {cat_world, {set_time, time_hour, freeze_time, set_weather, weather_type}},
    {cat_camera, {fov_enabled, fov_percent, camera_distance, camera_rotate_off, camera_reset_off, enemies_no_lock_on,
        lock_on_distance, camera_shake_off}},
    {cat_image, {vignette_off, chromatic_off, sharpen_off, aa_off, motion_blur_off, dof_off, animation_distance,
        clean_characters, player_clean, hide_buff_effects, hide_weapon_buff_effects, solid_ashes, solid_ghosts,
        ragdoll_death, keep_bodies}},
    {cat_display, {fps_unlock, fps_limit, refresh_rate, ultrawide}},
    {cat_interface, {hud_screen_edges, hud_scale, menu_edges, pause_key, skip_intro, dialog_yes_default,
        extended_ranges, highlight_changed}},
};

void ERUI_CALL restore_page_defaults(void* target) noexcept {
    const auto* layout = static_cast<const Layout*>(target);
    if (!layout) return;
    for (const Id i : layout->ids)
        if (!g_options[i].general) restore_option(i);
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

Scale* find_scale(std::wstring_view key) noexcept {
    if (!g_extended) return nullptr;
    for (Scale& scale : g_scales)
        if (same_key(scale.key, key)) return &scale;
    return nullptr;
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

void set_scale_index(Scale& scale, std::uint8_t index) noexcept {
    if (index >= kScaleCount || scale.index == index) return;
    scale.index = index;
    sync_scaled_default(scale);
    sync_scaled_texts(scale);
    if (g_registration && scale.row != ERUI_INVALID_ROW)
        (void)g_registration->set_custom_slider_index(scale.row, index);
}

// Sets an option's raw slider/toggle value (not multiplied by its scale).
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

// Hotkey values are effective values: a scaled slider goes back to x1 first
// so "> 200" always means 200 %.
bool hotkey_set(std::wstring_view key, int value, int& applied) noexcept {
    const Id i = find_option(key);
    if (i >= count || !available(i)) return false;
    if (Scale* const scale = scale_of(i)) set_scale_index(*scale, 0);
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
        if (!g_options[i].general)
            text += std::wstring(g_options[i].key) + L"=" + std::to_wstring(g_options[i].value) + L"\r\n";
    if (g_extended)
        for (const Scale& scale : g_scales)
            text += std::wstring(scale.key) + L"=" + std::to_wstring(scale.index) + L"\r\n";
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
        // Scales first, so the slider values that follow are read at x1..x100.
        for (int pass = 0; pass < 2; ++pass) {
            for (const wchar_t* line = section.data(); line < section.data() + length && *line;
                 line += std::wcslen(line) + 1) {
                const std::wstring_view entry(line);
                const std::size_t equals = entry.find(L'=');
                if (equals == entry.npos) continue;
                const std::wstring key(entry.substr(0, equals));
                const std::wstring value_text(entry.substr(equals + 1));
                int value{};
                if (!parse_profile_int(value_text.c_str(), value)) continue;
                if (Scale* const scale = find_scale(key)) {
                    if (pass == 0 && value >= 0 && value < static_cast<int>(kScaleCount))
                        set_scale_index(*scale, static_cast<std::uint8_t>(value));
                    continue;
                }
                const Id id = find_option(key);
                if (pass == 1 && id < count && !g_options[id].general) (void)set_option_raw(id, value);
            }
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

// Item Shop in the ERNativeUI fork's talk menu (F11). Pages: 0 = sections,
// 100 + section = its groups, 1000 + group = base game / Shadow of the
// Erdtree. A shop row closes the menu and opens the game's shop over that
// group's rows (msa\src\game_menus.rs waits a few frames for the menu to go).
using TalkBuildFn = void(ERUI_CALL*)(void*, void*, std::uint32_t);
using TalkChooseFn = void(ERUI_CALL*)(void*, void*, std::uint32_t, std::uint32_t);
using TalkRegisterFn = ERUI_Result(ERUI_CALL*)(const ERUI_Utf16View*, TalkBuildFn, TalkChooseFn, void*, std::uint64_t*);
using TalkAddRowFn = ERUI_Result(ERUI_CALL*)(void*, std::uint32_t, const ERUI_Utf16View*);
using TalkMenuFn = ERUI_Result(ERUI_CALL*)(void*, std::uint32_t);
using TalkCloseFn = ERUI_Result(ERUI_CALL*)(void*);
using TalkOpenFn = ERUI_Result(ERUI_CALL*)(std::uint64_t);
using TalkOpenAtFn = ERUI_Result(ERUI_CALL*)(std::uint64_t, std::uint32_t);
struct TalkApi {
    TalkRegisterFn register_entry{};
    TalkAddRowFn add_row{};
    TalkMenuFn open_page{};
    TalkCloseFn close{};
    TalkOpenFn open{};
    TalkCloseFn back{};  // optional
    TalkOpenAtFn open_at{};  // optional (fork r157-talk13): reopen at one page
};
// The page to come back to after a shop: the base / DLC choice of its
// section when it has both, else the section list (0).
std::atomic<std::uint32_t> g_talk_return_page{0};
TalkApi g_talk{};
std::uint64_t g_talk_entry{};

const wchar_t* shop_text(const ShopText& text) noexcept {
    if (g_profile_language == "es") return text.es;
    if (g_profile_language == "zh" || g_profile_language == "zht") return text.zh;
    return text.en;
}

void talk_row(void* menu, std::uint32_t row, const std::wstring& text) noexcept {
    try {
        const std::wstring shown = maybe_traditional(g_profile_language, text.c_str());
        ERUI_Utf16View view{};
        view.data = reinterpret_cast<const std::uint16_t*>(shown.c_str());
        view.length = static_cast<std::uint32_t>(shown.size());
        g_talk.add_row(menu, row, &view);
    } catch (...) {
    }
}

std::int32_t shop_section_total(int s) noexcept { return g_item_shop.items[s][0] + g_item_shop.items[s][1]; }

// The shop's "all items" sort: GameDataMan +0x60 = CSMenuSystemSaveLoad,
// +0x1440 sorts[20] (u32), index 12 = all items; 0x80005141 = item type,
// ascending. With it the shop groups items by type with a gap between
// groups, as the inventory does (Glorious Merchant, MIT, sets the same).
// It is the player's saved sort for that list, so restore_shop_sort puts
// the player's own value back when our shop closes.
std::uint32_t write_shop_sort() noexcept {
    std::uintptr_t gdm{}, menu_save{};
    if (!g_statics.game_data_man || !game::read(g_statics.game_data_man, gdm) || !gdm ||
        !game::read(gdm + 0x60, menu_save) || !menu_save) return 0xFFFFFFFFu;
    std::uint32_t current{};
    if (!game::read(menu_save + 0x1470, current)) return 0xFFFFFFFEu;
    game::write<std::uint32_t>(menu_save + 0x1470, 0x80005141u);
    return current;
}

// Glorious Merchant writes the sort right after the game opens the shop, so
// it is written now and again while the shop opens (the request waits four
// frames), for about one second; the log says what was there.
// The player's own "all items" sort, put back when our shop closes: it is
// saved with the character (audit 2026-11-08). 0 = nothing to restore.
std::atomic<std::uint32_t> g_saved_shop_sort{0};

void set_shop_sort_by_type() noexcept {
    const std::uint32_t before = write_shop_sort();
    std::uint32_t none = 0;
    if (before < 0xFFFFFFFEu && before != 0x80005141u) g_saved_shop_sort.compare_exchange_strong(none, before);
    // msa\src\game_menus.rs sets it again right after the shop opens.
}

void restore_shop_sort() noexcept {
    const std::uint32_t saved = g_saved_shop_sort.exchange(0);
    if (!saved) return;
    std::uintptr_t gdm{}, menu_save{};
    if (!g_statics.game_data_man || !game::read(g_statics.game_data_man, gdm) || !gdm ||
        !game::read(gdm + 0x60, menu_save) || !menu_save) return;
    game::write<std::uint32_t>(menu_save + 0x1470, saved);
}

extern "C" std::uintptr_t nt_top_menu_job();

void write_shop_layout(int section, int dlc, bool ours) noexcept {
    for (const ArmourLayoutCell& cell : g_shop_layout[section][dlc]) {
        game::write<std::int32_t>(cell.row + cell.sort_offset, ours ? cell.sort_id : cell.original_sort_id);
        game::write<std::uint8_t>(cell.row + cell.group_offset, ours ? cell.sort_group : cell.original_sort_group);
        if (cell.original_slot != 0xFF)
            game::write<std::uint8_t>(cell.row + 0xD6, ours ? std::uint8_t{0} : cell.original_slot);
    }
}

std::atomic<int> g_shop_layout_section{-1};
std::atomic<int> g_shop_layout_dlc{0};

struct ArmourLayoutJob { int dlc; int generation; };

// Waits for the shop to open (a menu job appears after the first 300 ms,
// which the F11 list may still use) and then to close (no menu job for
// 100 ms), and puts the game's values back. If it never opens, or the menu
// state is unknown, it restores after 5 s.
DWORD WINAPI armour_layout_worker(void* parameter) noexcept {
    const ArmourLayoutJob job = *static_cast<ArmourLayoutJob*>(parameter);
    delete static_cast<ArmourLayoutJob*>(parameter);
    bool seen_open = false;
    int closed_ticks = 0;
    // 25 ms polls (user, 08-11: coming back felt slower than going in):
    // closed after two empty reads, about 50 ms.
    for (int tick = 0; tick < 24000; ++tick) {
        Sleep(25);
        if (g_armour_layout_generation.load() != job.generation) return 0;  // a newer shop took over
        const std::uintptr_t top = nt_top_menu_job();
        if (top == static_cast<std::uintptr_t>(-1)) {
            if (!seen_open && tick > 200) break;  // no menu state: never opened
            continue;
        }
        if (top != 0) {
            if (tick >= 12) seen_open = true;
            closed_ticks = 0;
        } else if (seen_open && ++closed_ticks >= 2) {
            break;
        } else if (!seen_open && tick > 200) {
            break;  // never opened
        }
    }
    if (g_armour_layout_generation.load() != job.generation) return 0;
    if (g_shop_sell_zero) *g_shop_sell_zero = 0;
    if (g_armour_layout_applied.exchange(false)) {
        write_shop_layout(g_shop_layout_section.load(), job.dlc, false);
    }
    restore_shop_sort();
    // Leaving the shop goes back to F11 > Item Shop (user, r157-test22), so
    // the next shop is one choice away; Back there closes the menu.
    if (seen_open && g_talk.open && g_talk_entry) {
        const std::uint32_t page = g_talk_return_page.load();
        const ERUI_Result result = page && g_talk.open_at ? g_talk.open_at(g_talk_entry, page) : g_talk.open(g_talk_entry);
        if (result != ERUI_OK) {
            wchar_t message[100]{};
            swprintf_s(message, L"Item shop: back to the talk menu refused (result %u).", static_cast<unsigned>(result));
            log(message);
        }
    }
    return 0;
}

void apply_shop_layout(int section, int dlc) noexcept {
    const int generation = ++g_armour_layout_generation;
    if (g_armour_layout_applied.exchange(false))  // the previous shop's values, if still applied
        write_shop_layout(g_shop_layout_section.load(), g_shop_layout_dlc.load(), false);
    g_shop_layout_section.store(section);
    g_shop_layout_dlc.store(dlc);
    if (!g_shop_layout[section][dlc].empty()) {
        write_shop_layout(section, dlc, true);
        g_armour_layout_applied.store(true);
    }
    auto* job = new (std::nothrow) ArmourLayoutJob{dlc, generation};
    if (!job) return;
    if (HANDLE thread = CreateThread(nullptr, 0, armour_layout_worker, job, 0, nullptr)) CloseHandle(thread);
    else delete job;
}

void apply_shop_options(int section, int dlc) noexcept {
    const int weapon_level = std::clamp(value_of(shop_weapon_level), 0, 25);
    const int spirit_level = std::clamp(value_of(shop_spirit_level), 0, 10);
    const int percent = std::clamp(value_of(shop_item_price), 0, 500);
    const bool free_ok = g_shop_sell_zero != nullptr;
    for (const ShopRowRef& r : g_shop_rows[section][dlc]) {
        std::int32_t id = r.base_id;
        if (r.kind == 1 && r.max_level > 0) {
            const int level = r.max_level >= 25 ? weapon_level : (weapon_level * r.max_level + 12) / 25;
            id += std::min<int>(level, r.max_level);
        } else if (r.kind == 2) {
            id += std::min<int>(spirit_level, r.max_level);
        }
        // without the sell-value hook nothing may cost less than it sells for
        std::int32_t price = static_cast<std::int32_t>(static_cast<std::int64_t>(r.sell) * percent / 100);
        if (!free_ok) price = std::max(price, r.sell);
        std::memcpy(r.data + 0x00, &id, 4);
        std::memcpy(r.data + 0x04, &price, 4);
    }
    if (!free_ok && percent < 100) log(L"Item shop: no sell value hook, so prices stay at least at the sell value.");
}

void open_item_shop(void* menu, int section, int dlc) noexcept {
    g_talk_return_page.store(g_item_shop.items[section][0] > 0 && g_item_shop.items[section][1] > 0
        ? 1000u + static_cast<std::uint32_t>(section) : 0u);
    const std::int32_t first = g_item_shop.first[section][dlc];
    const std::int32_t n = g_item_shop.count[section][dlc];
    g_talk.close(menu);
    // Another mod or a regulation reload may have replaced ShopLineupParam.
    std::uintptr_t live{};
    if (n <= 0 || !g_shop_holder || !game::read(g_shop_holder, live) || live != g_shop_table) {
        log(L"Item shop: ShopLineupParam is no longer ours (another mod replaced it?); not opened.");
        return;
    }
    apply_shop_options(section, dlc);
    // With "item type" sorting the shop orders armour by slot first, so a
    // set's own sortGroupId alone put every piece on its own line
    // (r157-test18). The layout now also files every piece as a head while
    // the shop is open (protectorCategory +0xD6), so sortGroupId (the set)
    // and sortId (set x 10 + real slot) decide the order: one set per line
    // with the game's gap between sets (test).
    apply_shop_layout(section, dlc);  // a newer generation first: an old worker can't undo what follows
    if (g_shop_sell_zero) *g_shop_sell_zero = 1;
    set_shop_sort_by_type();
    const bool queued = spirit_ashes::open_shop(first, first + n - 1);
    if (!queued) {
        ++g_armour_layout_generation;  // the worker stops
        if (g_shop_sell_zero) *g_shop_sell_zero = 0;
        if (g_armour_layout_applied.exchange(false)) write_shop_layout(section, dlc, false);
        restore_shop_sort();
    }
    wchar_t message[200]{};
    swprintf_s(message, L"Item shop: section %d %s, %d rows, weapons +%d, spirit ashes +%d, price %d%%: %s.", section,
        dlc ? L"DLC" : L"base", n, std::clamp(value_of(shop_weapon_level), 0, 25), std::clamp(value_of(shop_spirit_level), 0, 10),
        std::clamp(value_of(shop_item_price), 0, 500), queued ? L"opened" : L"not available");
    log(message);
}

// F11 > Item Shop > Shop settings (user, r157-test25): the same three
// options as the Item Shop page of Mod Settings, with a short list of
// values each. Page 2000 shows them with their value; 2001-2003 list the
// values, the current one marked; choosing one goes back to 2000.
constexpr std::uint32_t kShopSettingsRow = 900;
// F11 > Item Shop > Recover flasks (user, 08-11): a lost flask comes back
// empty, one of it, as after drinking every charge; resting at a grace fills
// it with the allotment the character keeps (Golden Seeds included). Only
// what is missing; Crimson and Cerulean share the Sacred Tear level, so a
// missing one takes the other's, or the level chosen on page 3001.
constexpr std::uint32_t kFlaskRow = 901;
constexpr std::uint32_t kFlaskPage = 3000;
constexpr std::uint32_t kFlaskLevelPage = 3001;
extern "C" std::uint8_t nt_flask_state(std::int32_t* out);
int g_flask_level = 0;  // used when neither tears flask is left
bool g_flask_given[3]{};

std::wstring goods_name(std::int32_t id) {
    std::wstring name;
    int found = 0;
    if (!item_name({111, 10, 319, 419}, id, name, &found)) name = L"#" + std::to_wstring(id);
    return name;
}
constexpr std::uint32_t kShopSettingsPage = 2000;
struct ShopSetting {
    Id id;
    ShopText name;
    std::initializer_list<int> values;
};
const ShopSetting kShopSettings[]{
    {shop_weapon_level, {L"Weapon level", L"Nivel de las armas", L"武器强化等级"}, {0, 5, 10, 15, 20, 25}},
    {shop_spirit_level, {L"Spirit ash level", L"Nivel de las cenizas", L"骨灰强化等级"}, {0, 3, 5, 8, 10}},
    {shop_item_price, {L"Price", L"Precio", L"价格"}, {0, 25, 50, 100, 200}},
};

std::wstring shop_setting_value(Id id, int value) {
    static constexpr ShopText kFree{L"Free", L"Gratis", L"免费"};
    static constexpr ShopText kMax{L" (max, somber +10)", L" (máx., sombrías +10)", L"（最高，失色武器 +10）"};
    if (id == shop_item_price) return value == 0 ? std::wstring(shop_text(kFree)) : std::to_wstring(value) + L"%";
    std::wstring text = L"+" + std::to_wstring(value);
    if (id == shop_weapon_level && value == 25) text += shop_text(kMax);
    return text;
}

void ERUI_CALL item_shop_build(void*, void* menu, std::uint32_t page) noexcept {
    try {
        static constexpr ShopText kSettings{L"Shop settings", L"Ajustes de la tienda", L"商店设置"};
        static constexpr ShopText kCurrent{L" (current)", L" (actual)", L"（当前）"};
        static constexpr ShopText kFlasks{L"Recover flasks", L"Recuperar frascos", L"找回圣杯瓶"};
        if (page == 0) {
            talk_row(menu, kShopSettingsRow, shop_text(kSettings));
            if (g_item_gib) talk_row(menu, kFlaskRow, shop_text(kFlasks));
            for (int s = 0; s < sec_count; ++s)
                if (shop_section_total(s))
                    talk_row(menu, static_cast<std::uint32_t>(s),
                        std::wstring(shop_text(kShopSections[s])) + L" (" + std::to_wstring(shop_section_total(s)) + L")");
        } else if (page >= 1000 && page < 1000u + sec_count) {
            static constexpr ShopText kBase{L"Base game", L"Juego base", L"本体"};
            static constexpr ShopText kDlc{L"Shadow of the Erdtree", L"Shadow of the Erdtree", L"黄金树幽影"};
            const int s = static_cast<int>(page - 1000);
            talk_row(menu, 0, std::wstring(shop_text(kBase)) + L" (" + std::to_wstring(g_item_shop.items[s][0]) + L")");
            talk_row(menu, 1, std::wstring(shop_text(kDlc)) + L" (" + std::to_wstring(g_item_shop.items[s][1]) + L")");
        } else if (page == kFlaskPage) {
            static constexpr ShopText kHave{L": in your inventory", L": lo tienes", L"：已持有"};
            static constexpr ShopText kGiven{L": recovered, rest at a grace", L": recuperado, descansa en un lugar de gracia",
                L"：已找回，请在赐福处休息"};
            static constexpr ShopText kRecover{L": recover", L": recuperar", L"：找回"};
            static constexpr ShopText kLevel{L"Level for the tears flasks", L"Nivel de los frascos de lágrimas", L"露滴圣杯瓶等级"};
            static constexpr ShopText kNoPlayer{L"Load a character first.", L"Carga antes un personaje.", L"请先读取角色。"};
            static constexpr ShopText kHint{L"Resting at a site of grace fills them with your uses.",
                L"Al descansar en un lugar de gracia se llenan con tus usos.", L"在赐福处休息即可按你的次数补满。"};
            std::int32_t s[5]{};
            if (!nt_flask_state(s)) {
                talk_row(menu, 99, shop_text(kNoPlayer));
            } else {
                const int level = s[0] >= 0 ? (s[0] - 1000) / 2 : s[1] >= 0 ? (s[1] - 1050) / 2 : g_flask_level;
                const std::int32_t empty[3]{1000 + level * 2, 1050 + level * 2, 251};
                for (int k = 0; k < 3; ++k) {
                    const bool have = s[k] >= 0;
                    if (have) g_flask_given[k] = false;
                    const std::wstring name = goods_name(have ? s[k] : empty[k]);
                    talk_row(menu, static_cast<std::uint32_t>(k), name +
                        shop_text(have ? kHave : g_flask_given[k] ? kGiven : kRecover));
                }
                if (s[0] < 0 && s[1] < 0)
                    talk_row(menu, 10, std::wstring(shop_text(kLevel)) + L": +" + std::to_wstring(g_flask_level));
                talk_row(menu, 99, shop_text(kHint));
            }
        } else if (page == kFlaskLevelPage) {
            for (int l = 0; l <= 12; ++l)
                talk_row(menu, static_cast<std::uint32_t>(l),
                    L"+" + std::to_wstring(l) + (l == g_flask_level ? shop_text(kCurrent) : L""));
        } else if (page == kShopSettingsPage) {
            for (std::uint32_t k = 0; k < std::size(kShopSettings); ++k)
                talk_row(menu, k, std::wstring(shop_text(kShopSettings[k].name)) + L": " +
                    shop_setting_value(kShopSettings[k].id, value_of(kShopSettings[k].id)));
        } else if (page > kShopSettingsPage && page <= kShopSettingsPage + std::size(kShopSettings)) {
            const ShopSetting& setting = kShopSettings[page - kShopSettingsPage - 1];
            const int current = value_of(setting.id);
            std::uint32_t row = 0;
            for (const int value : setting.values)
                talk_row(menu, row++, shop_setting_value(setting.id, value) + (value == current ? shop_text(kCurrent) : L""));
        }
    } catch (...) {
    }
}

void ERUI_CALL item_shop_choose(void*, void* menu, std::uint32_t page, std::uint32_t row) noexcept {
    if (page == 0 && row == kShopSettingsRow) {
        g_talk.open_page(menu, kShopSettingsPage);
        return;
    }
    if (page == 0 && row == kFlaskRow) {
        g_talk.open_page(menu, kFlaskPage);
        return;
    }
    if (page == kFlaskLevelPage) {
        if (row <= 12) g_flask_level = static_cast<int>(row);
        if (g_talk.back) g_talk.back(menu);
        return;
    }
    if (page == kFlaskPage) {
        if (row == 10) {
            g_talk.open_page(menu, kFlaskLevelPage);
            return;
        }
        std::int32_t s[5]{};
        if (row > 2 || !nt_flask_state(s) || s[row] >= 0 || g_flask_given[row]) return;  // only what is missing
        const int level = s[0] >= 0 ? (s[0] - 1000) / 2 : s[1] >= 0 ? (s[1] - 1050) / 2 : g_flask_level;
        const std::int32_t empty[3]{1000 + level * 2, 1050 + level * 2, 251};
        const ItemGibEntry item{0x40000000u | static_cast<std::uint32_t>(empty[row]), 1, -1, -1};
        const bool given = give_items(&item, 1);
        if (given) g_flask_given[row] = true;
        wchar_t message[160]{};
        swprintf_s(message, L"Recover flasks: goods %d (empty, +%d) %ls; HP/FP allotment %d/%d.", empty[row], level,
            given ? L"given" : L"not given", s[3], s[4]);
        log(message);
        return;  // the page is built again with the new state
    }
    if (page == kShopSettingsPage) {
        if (row < std::size(kShopSettings)) g_talk.open_page(menu, kShopSettingsPage + 1 + row);
        return;
    }
    if (page > kShopSettingsPage && page <= kShopSettingsPage + std::size(kShopSettings)) {
        const ShopSetting& setting = kShopSettings[page - kShopSettingsPage - 1];
        if (row < setting.values.size()) {
            const int value = setting.values.begin()[row];
            if (Scale* const scale = scale_of(setting.id)) set_scale_index(*scale, 0);
            if (set_option_raw(setting.id, value)) request_save();
        }
        // back to Shop settings, rebuilt with the new value
        if (g_talk.back) g_talk.back(menu);
        return;
    }
    if (!g_item_shop.ready) return;
    if (page == 0 && row < static_cast<std::uint32_t>(sec_count)) {
        const int s = static_cast<int>(row);
        const bool base = g_item_shop.items[s][0] > 0;
        const bool dlc = g_item_shop.items[s][1] > 0;
        if (base && dlc) g_talk.open_page(menu, 1000u + row);
        else if (base || dlc) open_item_shop(menu, s, dlc ? 1 : 0);
    } else if (page >= 1000 && page < 1000u + sec_count && row < 2) {
        open_item_shop(menu, static_cast<int>(page - 1000), static_cast<int>(row));
    }
}



void register_item_shop_talk() noexcept {
    if (g_talk_registered || g_talk_tries > 20) return;
    ++g_talk_tries;
    const HMODULE host = GetModuleHandleW(L"ERNativeUI.dll");
    if (!host) return;
    g_talk.register_entry = reinterpret_cast<TalkRegisterFn>(GetProcAddress(host, "ERNativeUIFork_TalkRegister"));
    g_talk.add_row = reinterpret_cast<TalkAddRowFn>(GetProcAddress(host, "ERNativeUIFork_TalkAddRow"));
    g_talk.open_page = reinterpret_cast<TalkMenuFn>(GetProcAddress(host, "ERNativeUIFork_TalkOpenPage"));
    g_talk.close = reinterpret_cast<TalkCloseFn>(GetProcAddress(host, "ERNativeUIFork_TalkClose"));
    if (!g_talk.register_entry || !g_talk.add_row || !g_talk.open_page || !g_talk.close) {
        g_talk_tries = 100;
        log(L"Item shop: this ERNativeUI has no talk menu (F11); the item shop has no menu.");
        return;
    }
    try {
        static constexpr ShopText kName{L"Item Shop", L"Tienda de objetos", L"物品商店"};
        const std::wstring name = maybe_traditional(g_profile_language, shop_text(kName));
        ERUI_Utf16View view{};
        view.data = reinterpret_cast<const std::uint16_t*>(name.c_str());
        view.length = static_cast<std::uint32_t>(name.size());
        std::uint64_t entry{};
        g_talk.open = reinterpret_cast<TalkOpenFn>(GetProcAddress(host, "ERNativeUIFork_TalkOpen"));
        g_talk.back = reinterpret_cast<TalkCloseFn>(GetProcAddress(host, "ERNativeUIFork_TalkBack"));
        g_talk.open_at = reinterpret_cast<TalkOpenAtFn>(GetProcAddress(host, "ERNativeUIFork_TalkOpenAt"));
        const ERUI_Result result = g_talk.register_entry(&view, item_shop_build, item_shop_choose, nullptr, &entry);
        g_talk_registered = result == ERUI_OK;
        if (g_talk_registered) g_talk_entry = entry;
        wchar_t message[120]{};
        swprintf_s(message, L"Item shop: talk menu entry %s (result %u).", g_talk_registered ? L"registered" : L"refused",
            static_cast<unsigned>(result));
        log(message);
    } catch (...) {
    }
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

// The placeholder of an unavailable option (disabled, so never pressed).
void ERUI_CALL unavailable_clicked(void*) noexcept {}

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
    resolve(g_buddy_set_size);
    // The hook installers run as in the game: they check their bytes and
    // write their jumps into this process's copy of the mapped exe, which
    // never runs. That covers the options a pattern alone does not prove.
    // (In the order initialize() uses.)
    install_dialog_yes_hooks();
    install_window_hooks();
    resolve(g_intro);
    resolve_window();
    resolve_map_flags();
    resolve_fov_site();
    resolve_downed_site();
    resolve_jump_site();
    install_camera_shake_hook();
    install_ragdoll_hook();
    resolve_keep_bodies();
    resolve_item_gib();
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
    // The write used for hooks turned on mid-game (other threads suspended),
    // once, through the jump hook: it has no worker and starts nothing.
    g_freeze_writes.store(true, std::memory_order_release);
    install_jump_hook();
    g_freeze_writes.store(false, std::memory_order_release);
    g_statics = game::resolve_statics(g_log);
    int unavailable = fall_site ? 0 : 1;
    int elsewhere = 0;
    for (std::size_t i = 0; i < count; ++i) {
        const Id id = static_cast<Id>(i);
        if (available(id)) continue;
        // Not decided by eldenring.exe here: Multiple Spirit Ashes and the
        // HUD layout are checked by msa's own offline tests (every exe);
        // the rest need the INI, the built-in NPC Spawner or are off on purpose.
        const wchar_t* reason = nullptr;
        switch (id) {
        case ash_multiple: case ash_dismiss_all: case ash_extra_cost: case ash_damage_per_ash:
        case ash_repeat: case ash_free_fighting: case ash_max_regular: case ash_max_renowned:
            reason =L"Multiple Spirit Ashes (msa tests: native_rvas_match_every_executable)"; break;
        case hud_screen_edges: case hud_scale: case menu_edges:
            reason = L"HUD layout (msa tests: scaleform_targets_resolve_on_every_executable)"; break;
        case extended_ranges: case highlight_changed: reason = L"needs NativeTweaks.ini only"; break;
        case npc_spawner: reason = L"built-in NPC Spawner"; break;
        case summon_melina: case melina_companion: reason = L"off on purpose (separate mod)"; break;
        default: break;
        }
        wchar_t message[200]{};
        if (reason) {
            ++elsewhere;
            swprintf_s(message, L"AUDIT: option %ls not checked here: %ls.", g_options[i].key, reason);
        } else {
            ++unavailable;
            swprintf_s(message, L"AUDIT: option %ls would be greyed out.", g_options[i].key);
        }
        log(message);
    }
    wchar_t summary[200]{};
    swprintf_s(summary, L"AUDIT: %d of %d options would be unavailable (%d checked elsewhere).", unavailable,
        static_cast<int>(count), elsewhere);
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
    start_npc_spawner();
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
    install_camera_shake_hook();
    install_ragdoll_hook();
#ifdef NT_DEV_COMBAT_TRACE
    install_combat_trace();
#endif
    resolve_keep_bodies();
    resolve_item_gib();
    resolve_msg_lookup();
    install_sell_value_hook();
    install_lock_range_hooks();
    install_toughness_hook();
    install_death_hook();
    if (kMelinaWip) install_turn_speed_hook();
    resolve_chr_enable();
    g_statics = game::resolve_statics(g_log);
    load_settings();
    load_ranges();
    g_param_event = CreateEventW(nullptr, FALSE, TRUE, nullptr);
    for (std::size_t i = 0; i < count; ++i) {
        if (!available(static_cast<Id>(i))) continue;
        if (value_of(static_cast<Id>(i)) != g_options[i].fallback) apply(static_cast<Id>(i));
    }
    // Only apply() patches the spirit slot search (audit 2026-11-08: with
    // the old on-by-default setting it stayed at 10 per player).
    if (available(ash_multiple) && value_of(ash_multiple) != 0) apply(ash_multiple);
    // The layout options are skipped above while ERNativeUI.dll is not loaded yet.
    send_hud_layout(L"startup");
    // From here on, hooks turned on from the menu are written frozen.
    g_freeze_writes.store(true, std::memory_order_release);
    start_thread(&game_thread_probe_worker, L"game thread probe");
    if (!g_config_path.empty()) {
        g_save_event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (g_save_event) start_thread(&save_worker, L"save");
    }
    if (g_pause.ready) start_thread(&pause_worker, L"pause");
    if (g_dialog_yes_data) start_thread(&dialog_pointer_worker, L"dialog_pointer");
    if (g_window_slot) start_thread(&window_option_worker, L"window_option");
    if (g_param_event && g_statics.layout_2_7_1 && g_statics.regulation_manager) start_thread(&param_worker, L"param");
    if (g_statics.layout_2_7_1 && g_statics.world_chr_man) {
        if (g_diagnostics) start_thread(&boss_health_log_worker, L"boss_health_log");
        start_thread(&health_limit_worker, L"health_limit");
        start_thread(&speed_worker, L"speed");
        start_thread(&regen_worker, L"regen");
        start_thread(&fall_worker, L"fall");
        start_thread(&enemy_speed_worker, L"enemy_speed");
        start_thread(&object_speed_worker, L"object_speed");
        start_thread(&jump_worker, L"jump");
        start_thread(&dead_enemy_worker, L"dead_enemy");
        start_thread(&ghost_worker, L"ghost");
        start_thread(&clean_worker, L"clean");
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
    // Scaled sliders show the applied value whether or not labels are coloured.
    g_set_slider_texts = host
        ? reinterpret_cast<SetSliderTextsFn>(GetProcAddress(host, "ERNativeUIFork_SetCustomSliderValues")) : nullptr;
    if (registration)
        for (const Scale& scale : g_scales)
            if (scale.index != 0) sync_scaled_texts(scale);
    // Highlight changed options (off by default): the menu switch, [General]
    // HighlightChangedOptions = 1, or the old optional package's marker file
    // NativeTweaks.HighlightChanged.ini beside NativeTweaks.ini. With the live
    // switch the defaults are always registered and the colours turned on or
    // off; before registering, so nothing is coloured for a frame.
    const bool enabled = value_of(highlight_changed) != 0;
    const auto set_highlight = host
        ? reinterpret_cast<SetHighlightEnabledFn>(GetProcAddress(host, "ERNativeUIFork_SetHighlightEnabled")) : nullptr;
    if (!registration || !set_default || (!enabled && !set_highlight)) return;
    if (set_highlight) (void)set_highlight(enabled ? 1u : 0u);
    g_set_highlight = set_highlight;
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
    for (const Scale& scale : g_scales) {
        if (scale.row == ERUI_INVALID_ROW) continue;
        if (set_default(registration->provider_handle(), scale.row, 0) != ERUI_OK) ++failures;
        sync_scaled_default(scale);
    }
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
// SpiritFreeFighting values (msa area_follow FREE_FIGHTING_*).
constexpr std::size_t kFreeFightCount = 3;
constexpr const wchar_t* kFreeFightEnglish[kFreeFightCount]{L"Off", L"Boss arenas", L"Everywhere"};
constexpr const wchar_t* kFreeFightSpanish[kFreeFightCount]{L"Desactivada", L"Arenas de jefe", L"En todas partes"};
constexpr const wchar_t* kFreeFightChinese[kFreeFightCount]{L"关闭", L"头目场地", L"全部"};
constexpr std::size_t kRepeatCount = 3;
constexpr const wchar_t* kRepeatEnglish[kRepeatCount]{L"Dismisses it", L"Summons it again", L"Game decides"};
constexpr const wchar_t* kRepeatSpanish[kRepeatCount]{L"La retira", L"La invoca otra vez", L"Lo decide el juego"};
constexpr const wchar_t* kRepeatChinese[kRepeatCount]{L"解除", L"再次召唤", L"交给游戏"};
constexpr const wchar_t* kWeatherChinese[kWeatherCount]{
    L"晴天", L"晴朗", L"少云", L"多云", L"雨", L"雨云", L"风雨", L"风雾", L"雪",
    L"大雪", L"雾", L"浓雾", L"大风多云", L"雨雾", L"雪雾", L"阵雨"};

// Row under a scaled slider.
Text builtin_scale_text(std::string_view lang) noexcept {
    if (lang == "es")
        return {L"    Escala", L"Multiplica el valor del control de arriba. Por ejemplo, 250% con ×10 aplica 2500%."};
    if (lang == "zh") return {L"    倍率", L"将上方数值乘以此倍率。例如 250% ×10 即为 2500%。"};
    if (lang == "zht") return {L"    倍率", L"將上方數值乘以此倍率。例如 250% ×10 即為 2500%。"};
    return {L"    Scale", L"Multiplies the value above. For example, 250% at ×10 applies 2500%."};
}

Text scale_text(std::string_view lang) noexcept {
    const Text english = builtin_scale_text("en");
    const Text text = builtin_scale_text(lang);
    return {translate_text(lang, english.label, text.label),
        translate_text(lang, english.help, text.help)};
}

std::vector<std::wstring> scale_names() {
    std::vector<std::wstring> names;
    for (const int factor : kScaleFactors) names.push_back(L"×" + std::to_wstring(factor));
    return names;
}

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
    } else if (i == ash_free_fighting) {
        english = kFreeFightEnglish, spanish = kFreeFightSpanish, chinese = kFreeFightChinese;
        count = kFreeFightCount;
    } else if (i == ash_repeat) {
        english = kRepeatEnglish, spanish = kRepeatSpanish, chinese = kRepeatChinese;
        count = kRepeatCount;
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

// Give runes (user, 14-11): a text box on the Runes page; the number typed
// is added to the runes you carry (PlayerGameData +0x6C, rune_count in
// fromsoftware-rs; GameDataMan -> +0x8), up to 999,999,999, when confirmed.
void ERUI_CALL give_runes_changed(void*, const ERUI_TextInputChangeContext* context) noexcept {
    erui::TextInputChange change{};
    if (!erui::detail::make_text_input_change(context, change)) return;
    std::wstring digits;
    for (const wchar_t c : change.value)
        if (c >= L'0' && c <= L'9') digits += c;
    if (digits.empty() || digits.size() > 10) return;
    const unsigned long long amount = std::wcstoull(digits.c_str(), nullptr, 10);
    std::uintptr_t manager{}, player{};
    std::uint32_t runes{};
    if (!amount || !g_statics.game_data_man || !game::read(g_statics.game_data_man, manager) || !manager ||
        !game::read(manager + 0x8, player) || !player || !game::read(player + 0x6C, runes))
        return;
    const unsigned long long total = std::min<unsigned long long>(999999999ull, runes + amount);
    if (!game::write<std::uint32_t>(player + 0x6C, static_cast<std::uint32_t>(total))) return;
    wchar_t message[128]{};
    swprintf_s(message, L"Give runes: +%llu (now %llu).", total - runes, total);
    log(message);
}

void add_give_runes_row(erui::Page& page, std::string_view l) noexcept {
    const bool es = l == "es", zh = l == "zh" || l == "zht";
    erui::TextInputOptions options{};
    options.placeholder = es ? L"Cantidad" : zh ? L"数量" : L"Amount";
    options.maximum_length = 10;
    const erui::RowHandle row = page.add_text_input(
        es ? L"Dar runas" : zh ? L"给予卢恩" : L"Give runes",
        es ? L"Escribe una cantidad y confírmala: se suma a las runas que llevas (hasta 999.999.999). Hace falta una partida cargada."
           : zh ? L"输入数量并确认：会加到你携带的卢恩上（最多 999,999,999）。需要已载入存档。"
                : L"Type an amount and confirm it: it is added to the runes you carry (up to 999,999,999). Needs a loaded game.",
        options, &give_runes_changed, nullptr);
    if (row == ERUI_INVALID_ROW) log(L"Give runes: the text box could not be added.");
}

void add_npc_spawner_pages(erui::Page& parent) noexcept {
    if (!g_npc_spawner_started) return;
    log(npc_spawner::add_pages(parent.provider_handle(), parent.handle())
            ? L"NPC Spawner (built in): its pages are in Mod Settings after Vanilla Settings."
            : L"NPC Spawner (built in): its pages could not all be added (some rows may be missing).");
}

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
            if (!ok) {
                // An unavailable option is shown as a disabled button, without a
                // value: a disabled native slider row still looked active (HUD
                // scale with RuntimePatch = 0), and a disabled toggle reads "On"
                // whatever its value (Nexus, r142: greyed-out options "stuck on"
                // with ME3). The placeholder is not the option's row, so value,
                // default and slider-text updates skip it; it has no multiplier
                // row either.
                const erui::RowHandle placeholder =
                    page.add_button(t.options[i].label, help, &unavailable_clicked, nullptr, false);
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
            if (Scale* const scale = scale_of(i)) {
                const std::vector<std::wstring> names = scale_names();
                const Text text = scale_text(lang);
                erui::CustomSliderOptions slider{};
                slider.value_count = static_cast<std::uint32_t>(names.size());
                slider.initial_index = scale->index;
                slider.enabled = ok;
                slider.format_value = [&names](std::uint32_t index, std::uint32_t) {
                    return names[index];
                };
                scale->row = page.add_custom_slider(text.label, text.help, slider, &scale_changed, scale);
                live_text::track_row(scale->row, [](const char* l) {
                    const Text x = scale_text(l);
                    return live_text::Texts{x.label, x.help, {}, {}, scale_names()};
                });
            }
        }
        if (which == cat_runes) add_give_runes_row(page, lang);
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
