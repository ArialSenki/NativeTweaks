// Structure offsets were cross-checked against the public research in
// veeenu's eldenring-practice-tool (2.07.1 tables) and re-derived here from
// eldenring.exe 2.7.1.0 code references; no code from that project is used.

#include "game_memory.hpp"

#include "perf_log.hpp"

#include <Windows.h>

#include <algorithm>
#include <cstring>
#include <map>
#include <string>

#pragma comment(lib, "version.lib")

namespace game {

namespace {
void* g_image_override{};
} // namespace

void* image() noexcept {
    return g_image_override ? g_image_override : static_cast<void*>(GetModuleHandleW(nullptr));
}

void set_image(void* module) noexcept {
    g_image_override = module;
}

namespace {

bool copy_guarded(void* destination, const void* source, std::size_t size) noexcept {
    __try {
        std::memcpy(destination, source, size);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        // Each fault goes through the kernel and every exception handler in
        // the process, so PerfLog counts them.
        perf::count_fault();
        return false;
    }
}

std::vector<int> parse(const char* text) {
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

std::uint8_t* find_unique(const char* text) noexcept {
    try {
        const auto pattern = parse(text);
        // Anchor the search on the first concrete byte; leading wildcards are allowed.
        std::size_t anchor = 0;
        while (anchor < pattern.size() && pattern[anchor] < 0) ++anchor;
        if (anchor == pattern.size()) return nullptr;
        auto* base = reinterpret_cast<std::uint8_t*>(game::image());
        const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
        const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
        const auto* section = IMAGE_FIRST_SECTION(nt);
        std::uint8_t* found{};
        for (unsigned i = 0; i < nt->FileHeader.NumberOfSections; ++i, ++section) {
            if (!(section->Characteristics & IMAGE_SCN_MEM_EXECUTE)) continue;
            if (section->Misc.VirtualSize < pattern.size()) continue;
            std::uint8_t* p = base + section->VirtualAddress;
            std::uint8_t* const end = p + section->Misc.VirtualSize - pattern.size();
            for (; p <= end; ++p) {
                std::uint8_t* a = static_cast<std::uint8_t*>(std::memchr(p + anchor, pattern[anchor],
                    static_cast<std::size_t>(end - p) + 1));
                if (!a) break;
                p = a - anchor;
                std::size_t k = 0;
                for (; k < pattern.size(); ++k)
                    if (pattern[k] >= 0 && p[k] != pattern[k]) break;
                if (k != pattern.size()) continue;
                if (found) return nullptr;
                found = p;
            }
        }
        return found;
    } catch (...) {
        return nullptr;
    }
}

std::uintptr_t rip_target(const std::uint8_t* hit, std::size_t disp_at, std::size_t next_at) noexcept {
    if (!hit) return 0;
    std::int32_t disp{};
    std::memcpy(&disp, hit + disp_at, 4);
    return reinterpret_cast<std::uintptr_t>(hit + next_at) + disp;
}

// 2.7.1.1 is a rebuild of 2.7.1.0 (same section layout, byte-identical game
// code; only the protection layer and padding differ), so it shares the layout.
// 2.7.2.0 moves code but keeps every structure offset used here (each offset
// constant appears in the game's code as often as in 2.7.1.0, every pattern
// stays unique).
// 2.7.2.1 (Japanese 1.17.2) is to 2.7.2.0 what 2.7.1.1 is to 2.7.1.0: same
// section layout, and the code at every address checked is the same; only
// some read-only data (17 vtables) sits a few bytes away (24-10, offline).
bool game_has_2_7_1_layout() noexcept {
    wchar_t path[MAX_PATH]{};
    if (!GetModuleFileNameW(static_cast<HMODULE>(game::image()), path, MAX_PATH)) return false;
    DWORD handle{};
    const DWORD size = GetFileVersionInfoSizeW(path, &handle);
    if (!size) return false;
    std::vector<std::uint8_t> data(size);
    if (!GetFileVersionInfoW(path, 0, size, data.data())) return false;
    VS_FIXEDFILEINFO* info{};
    UINT length{};
    if (!VerQueryValueW(data.data(), L"\\", reinterpret_cast<void**>(&info), &length) || !info) return false;
    return info->dwFileVersionMS == ((2u << 16) | 7u) &&
        (info->dwFileVersionLS == ((1u << 16) | 0u) || info->dwFileVersionLS == ((1u << 16) | 1u) ||
         info->dwFileVersionLS == ((2u << 16) | 0u) || info->dwFileVersionLS == ((2u << 16) | 1u));
}

} // namespace

bool safe_read(std::uintptr_t address, void* out, std::size_t size) noexcept {
    if (address < 0x10000) return false;
    return copy_guarded(out, reinterpret_cast<const void*>(address), size);
}

bool safe_write(std::uintptr_t address, const void* in, std::size_t size) noexcept {
    if (address < 0x10000) return false;
    return copy_guarded(reinterpret_cast<void*>(address), in, size);
}

std::uintptr_t chain(std::uintptr_t base, std::initializer_list<std::ptrdiff_t> offsets) noexcept {
    std::uintptr_t p{};
    if (!read(base, p)) return 0;
    const std::size_t count = offsets.size();
    std::size_t index = 0;
    for (const std::ptrdiff_t offset : offsets) {
        if (++index == count) return p ? p + offset : 0;
        if (!p || !read(p + offset, p)) return 0;
    }
    return p;
}

Statics resolve_statics(LogFn log) noexcept {
    Statics s{};
    s.world_chr_man = rip_target(find_unique("48 8B 05 ?? ?? ?? ?? 48 85 C0 74 0F 48 39 88"), 3, 7);
    // "?? 80 3D disp32 00": the compare reads flag +1, so the block starts one byte earlier.
    const std::uintptr_t flag = rip_target(find_unique("?? 80 3D ?? ?? ?? ?? 00 0F 85 ?? ?? ?? ?? 32 C0 48") , 3, 8);
    s.chr_dbg_flags = flag ? flag - 1 : 0;
    s.regulation_manager = rip_target(find_unique("48 8B 0D ?? ?? ?? ?? 48 85 C9 74 0B 4C 8B C0 48 8B D7"), 3, 7);
    // GameDataMan getter "mov rax,[static]; test; jz; mov rax,[rax+58]; ret" and
    // GameMan::GetSaveSlot "mov rax,[static]; mov eax,[rax+AC0]; ret".
    s.game_data_man = rip_target(find_unique("48 8B 05 ?? ?? ?? ?? 48 85 C0 74 05 48 8B 40 58 C3 C3"), 3, 7);
    s.game_man = rip_target(find_unique("48 8B 05 ?? ?? ?? ?? 8B 80 C0 0A 00 00 C3"), 3, 7);
    s.layout_2_7_1 = game_has_2_7_1_layout();

    if (log) {
        const auto base = reinterpret_cast<std::uintptr_t>(game::image());
        wchar_t message[256]{};
        swprintf_s(message,
            L"Vanilla statics: WorldChrMan=+0x%llX ChrDbgFlags=+0x%llX Regulation=+0x%llX GameDataMan=+0x%llX "
            L"GameMan=+0x%llX, game 2.7.1.0/2.7.1.1/2.7.2.0/2.7.2.1 layout=%ls.",
            static_cast<unsigned long long>(s.world_chr_man ? s.world_chr_man - base : 0),
            static_cast<unsigned long long>(s.chr_dbg_flags ? s.chr_dbg_flags - base : 0),
            static_cast<unsigned long long>(s.regulation_manager ? s.regulation_manager - base : 0),
            static_cast<unsigned long long>(s.game_data_man ? s.game_data_man - base : 0),
            static_cast<unsigned long long>(s.game_man ? s.game_man - base : 0),
            s.layout_2_7_1 ? L"yes" : L"no");
        log(message);
    }
    return s;
}

std::uintptr_t player_points(const Statics& s) noexcept {
    if (!s.layout_2_7_1 || !s.world_chr_man) return 0;
    return chain(s.world_chr_man, {kPlayerIns, kModules, 0x0, 0x138});
}

// CSPlayerSuperArmorModule at modules+0x40 (module 8). A hit does
// current = min(current, armour max + [+0x18]) - poise damage, then clamps
// to [-50, max] (0x47E0F0 in 2.7.2.0; same code in 2.7.0.0 to 2.7.2.0).
std::uintptr_t player_poise(const Statics& s) noexcept {
    if (!s.layout_2_7_1 || !s.world_chr_man) return 0;
    return chain(s.world_chr_man, {kPlayerIns, kModules, 0x40, 0x10});
}

std::uintptr_t player_anim_speed(const Statics& s) noexcept {
    if (!s.layout_2_7_1 || !s.world_chr_man) return 0;
    return chain(s.world_chr_man, {kPlayerIns, kModules, 0x28, 0x17C8});
}

// CSChrTimeActModule at modules+0x18 (fromsoftware-rs): a ring of 10 animations
// {id, play time, play time, length} from +0x20, write index at +0xC0 and read
// index (the animation playing now) at +0xC4. +0x20 alone is slot 0, which only
// holds the current animation one time in ten.
std::uintptr_t chr_anim_id(std::uintptr_t chr) noexcept {
    std::uintptr_t modules{}, time_act{};
    std::uint32_t index{};
    if (!chr || !read(chr + kModules, modules) || !modules || !read(modules + 0x18, time_act) || !time_act ||
        !read(time_act + 0xC4, index) || index >= 10) return 0;
    return time_act + 0x20 + index * 0x10;
}

std::uintptr_t player_anim_id(const Statics& s) noexcept {
    if (!s.layout_2_7_1 || !s.world_chr_man) return 0;
    std::uintptr_t manager{}, player{};
    if (!read(s.world_chr_man, manager) || !manager || !read(manager + kPlayerIns, player)) return 0;
    return chr_anim_id(player);
}

// Module container: physics at +0x68, fall at +0x70 (fromsoftware-rs).
std::uintptr_t player_fall_timer(const Statics& s) noexcept {
    if (!s.layout_2_7_1 || !s.world_chr_man) return 0;
    return chain(s.world_chr_man, {kPlayerIns, kModules, 0x70, 0x18});
}

// CSChrResistModule at modules+0x20: poison, rot, bleed, blight, frost, sleep,
// madness from +0x10, their maximums from +0x2C. Each hit subtracts its buildup
// from the value; at zero the status triggers and the value goes back to the
// maximum (eldenring.exe+0x43DE90 in 2.7.2.0).
std::uintptr_t player_resist(const Statics& s) noexcept {
    if (!s.layout_2_7_1 || !s.world_chr_man) return 0;
    return chain(s.world_chr_man, {kPlayerIns, kModules, 0x20, 0x10});
}

// The SpEffect object the resist module checks for status immunities.
std::uintptr_t player_special_effect(const Statics& s) noexcept {
    if (!s.layout_2_7_1 || !s.world_chr_man) return 0;
    std::uintptr_t manager{}, player{}, effects{};
    if (!read(s.world_chr_man, manager) || !manager || !read(manager + kPlayerIns, player) || !player ||
        !read(player + 0x178, effects)) return 0;
    return effects;
}

// ---------------------------------------------------------------------------
// FD4Singleton lookup by reflection name. Port of the scan in Dasaav-dsv's
// from-singleton (MIT/Apache-2.0): each singleton getter lazily creates its
// instance with "mov reg,[static]; test reg,reg; jne; lea rcx,[reflection];
// call get_name ...; mov edx,size; lea r8; mov r9,rax" and the reflection
// object yields the class name.

namespace {

struct Section { std::uint8_t* begin{}; std::uint8_t* end{}; };

Section find_section(const char* name) noexcept {
    auto* base = reinterpret_cast<std::uint8_t*>(game::image());
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
    const auto* section = IMAGE_FIRST_SECTION(nt);
    for (unsigned i = 0; i < nt->FileHeader.NumberOfSections; ++i, ++section)
        if (std::strncmp(reinterpret_cast<const char*>(section->Name), name, 8) == 0)
            return {base + section->VirtualAddress, base + section->VirtualAddress + section->Misc.VirtualSize};
    return {};
}

struct SingletonEntry { std::uintptr_t slot; std::uintptr_t reflection; };
using GetNameFn = const char* (*)(const void*);

std::vector<SingletonEntry> g_singletons;
GetNameFn g_get_name{};
bool g_singletons_scanned{};
// The weather and time workers call find_singleton at the same moment; the
// scan fills g_singletons, so it must finish before anyone reads the list.
SRWLOCK g_singleton_lock = SRWLOCK_INIT;

std::uintptr_t rel32(const std::uint8_t* field) noexcept {
    std::int32_t disp{};
    std::memcpy(&disp, field, 4);
    return reinterpret_cast<std::uintptr_t>(field + 4) + disp;
}

bool same3(const std::uint8_t* p, std::uint8_t a, std::uint8_t b, std::uint8_t c) noexcept {
    return p[0] == a && p[1] == b && p[2] == c;
}

// Kinds of the four argument instructions around "mov edx, imm32".
enum : int { kLeaRcx = 1, kLeaR8 = 2, kLeaR9 = 4, kMovR9 = 8, kMovEdx = 16 };

int kind_at(const std::uint8_t* p) noexcept {
    if (same3(p, 0x48, 0x8D, 0x0D)) return kLeaRcx;
    if (same3(p, 0x4C, 0x8D, 0x05)) return kLeaR8;
    if (same3(p, 0x4C, 0x8D, 0x0D)) return kLeaR9;
    if (same3(p, 0x4C, 0x8B, 0xC8)) return kMovR9;
    return 0;
}

int length_of(int kind) noexcept { return kind == kMovR9 ? 3 : kind == kMovEdx ? 5 : 7; }

void scan_singletons() noexcept {
    const Section text = find_section(".text");
    const Section data = find_section(".data");
    if (!text.begin || !data.begin) return;
    const std::uint8_t* const lo = text.begin + 64;
    const std::uint8_t* const hi = text.end - 64;
    for (const std::uint8_t* at = lo; at < hi; ++at) {
        at = static_cast<const std::uint8_t*>(std::memchr(at, 0xBA, static_cast<std::size_t>(hi - at)));
        if (!at) break;
        // Collect four instructions: forward until a call, then backward.
        int kinds[4]{kMovEdx};
        const std::uint8_t* pos[4]{at};
        int n = 1;
        bool bad = false;
        while (n < 4) {
            const std::uint8_t* next = pos[n - 1] + length_of(kinds[n - 1]);
            if (next[0] == 0xE8) break;
            const int k = kind_at(next);
            if (!k) { bad = true; break; }
            kinds[n] = k; pos[n] = next; ++n;
        }
        while (!bad && n < 4) {
            const std::uint8_t* prev = pos[0] - 7;
            int k = kind_at(prev);
            if (k == kMovR9) k = 0;  // a 3-byte mov sits in the last three bytes
            if (!k && kind_at(prev + 4) == kMovR9) { k = kMovR9; prev += 4; }
            if (!k) { bad = true; break; }
            std::memmove(kinds + 1, kinds, sizeof(int) * n);
            std::memmove(pos + 1, pos, sizeof(void*) * n);
            kinds[0] = k; pos[0] = prev; ++n;
        }
        if (bad || n < 4) continue;
        int mask = 0;
        for (int i = 0; i < 4; ++i) mask |= kinds[i] & (kLeaRcx | kLeaR8 | kLeaR9 | kMovR9);
        if (mask != (kLeaRcx | kLeaR8 | kMovR9)) continue;

        for (int pad = 0; pad <= 1; ++pad) {
            const std::uint8_t* call = pos[0] - 5 - pad;
            if (call[0] != 0xE8) continue;
            const std::uint8_t* lea = call - 7;
            if (!same3(lea, 0x48, 0x8D, 0x0D)) continue;
            const std::uint8_t* jump{};
            if (lea[-2] == 0x75) jump = lea - 2;
            else if (lea[-6] == 0x0F && lea[-5] == 0x85) jump = lea - 6;
            else continue;
            const std::uint8_t* test = jump - 3;
            const std::uint8_t test_reg = test[2] & 7;
            if ((test[0] & 0xF8) != 0x48 || (test[2] & 0xC0) != 0xC0 || ((test[2] >> 3) & 7) != test_reg) continue;
            std::uintptr_t slot{};
            for (int pad2 = 0; pad2 <= 3 && !slot; ++pad2) {
                const std::uint8_t* mov = test - 7 - pad2;
                if ((mov[0] & 0xF8) == 0x48 && (mov[2] & 0xC0) == 0 && (mov[2] & 7) == 5 &&
                    ((mov[0] >> 2) & 1) == (test[0] & 1) && ((mov[2] >> 3) & 7) == test_reg)
                    slot = rel32(mov + 3);
            }
            if (!slot || slot % 8 || slot < reinterpret_cast<std::uintptr_t>(data.begin) ||
                slot >= reinterpret_cast<std::uintptr_t>(data.end)) break;
            const std::uintptr_t function = rel32(call + 1);
            if (!g_get_name && function >= reinterpret_cast<std::uintptr_t>(text.begin) &&
                function < reinterpret_cast<std::uintptr_t>(text.end))
                g_get_name = reinterpret_cast<GetNameFn>(function);
            bool known = false;
            for (const auto& e : g_singletons) known = known || e.slot == slot;
            if (!known) g_singletons.push_back({slot, rel32(lea + 3)});
            break;
        }
    }
}

const char* singleton_name(const SingletonEntry& e) noexcept {
    __try {
        return g_get_name ? g_get_name(reinterpret_cast<const void*>(e.reflection)) : nullptr;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return nullptr;
    }
}

} // namespace

std::uintptr_t find_singleton(const char* name, LogFn log, const char* list_filter) noexcept {
    AcquireSRWLockExclusive(&g_singleton_lock);
    bool scanned = true;
    try {
        if (!g_singletons_scanned) scan_singletons();
    } catch (...) {
        scanned = false;
    }
    g_singletons_scanned = true;
    ReleaseSRWLockExclusive(&g_singleton_lock);
    if (!scanned) return 0;
    const auto base = reinterpret_cast<std::uintptr_t>(game::image());
    std::uintptr_t found{};
    for (const auto& e : g_singletons) {
        const char* n = singleton_name(e);
        char copy[96]{};
        if (!n || !safe_read(reinterpret_cast<std::uintptr_t>(n), copy, sizeof(copy) - 1)) continue;
        copy[sizeof(copy) - 1] = 0;
        if (std::strcmp(copy, name) == 0) found = e.slot;
        if (log && list_filter && std::strstr(copy, list_filter)) {
            wchar_t message[160]{};
            swprintf_s(message, L"Singleton %hs at eldenring.exe+0x%llX.", copy,
                static_cast<unsigned long long>(e.slot - base));
            log(message);
        }
    }
    if (log) {
        wchar_t message[160]{};
        swprintf_s(message, L"Singleton '%hs': %ls (%zu singletons scanned).", name,
            found ? L"found" : L"not found", g_singletons.size());
        log(message);
    }
    return found;
}

namespace {

// A character's animation speed (float) lives at modules+0x28 -> +0x17C8.
std::uintptr_t chr_anim_speed(std::uintptr_t chr) noexcept {
    std::uintptr_t modules{}, anim{};
    if (!read(chr + kModules, modules) || !modules || !read(modules + 0x28, anim) || !anim) return 0;
    return anim + 0x17C8;
}

bool looks_like_chr(std::uintptr_t chr) noexcept {
    const auto speed = chr_anim_speed(chr);
    float value{};
    return speed && read(speed, value) && value >= 0.05f && value <= 20.0f;
}

} // namespace

std::ptrdiff_t find_chr_vector(const Statics& s, LogFn log) noexcept {
    if (!s.layout_2_7_1 || !s.world_chr_man) return 0;
    std::uintptr_t manager{};
    if (!read(s.world_chr_man, manager) || !manager) return 0;
    std::ptrdiff_t best{};
    std::size_t best_count{};
    // A std::vector is {begin, end, capacity}; the one holding every loaded
    // character is the largest whose sampled elements all look like ChrIns.
    for (std::ptrdiff_t offset = 0; offset < 0x20000; offset += 8) {
        std::uintptr_t begin{}, end{}, capacity{};
        if (!read(manager + offset, begin) || !read(manager + offset + 8, end) ||
            !read(manager + offset + 16, capacity)) continue;
        if (begin < 0x10000 || end <= begin || capacity < end || (end - begin) % 8) continue;
        const std::size_t count = (end - begin) / 8;
        if (count < 8 || count > 5000 || count <= best_count) continue;
        bool valid = true;
        const std::size_t stride = (std::max<std::size_t>)(1, count / 16);
        for (std::size_t i = 0; i < count && valid; i += stride) {
            std::uintptr_t chr{};
            valid = read(begin + i * 8, chr) && chr && looks_like_chr(chr);
        }
        if (!valid) continue;
        best = offset;
        best_count = count;
    }
    if (log) {
        wchar_t message[128]{};
        swprintf_s(message, L"Character list: WorldChrMan+0x%llX, %zu characters.",
            static_cast<unsigned long long>(best), best_count);
        log(best ? message : L"Character list not found yet.");
    }
    return best;
}

bool for_each_chr_speed(const Statics& s, std::ptrdiff_t vector_offset,
    void (*visit)(std::uintptr_t chr, std::uintptr_t speed, void* context), void* context) noexcept {
    std::uintptr_t manager{}, begin{}, end{};
    if (!vector_offset || !read(s.world_chr_man, manager) || !manager ||
        !read(manager + vector_offset, begin) || !read(manager + vector_offset + 8, end)) return false;
    if (begin < 0x10000 || end < begin || (end - begin) / 8 > 5000) return false;
    for (std::uintptr_t slot = begin; slot < end; slot += 8) {
        std::uintptr_t chr{};
        if (!read(slot, chr) || !chr) continue;
        if (const auto speed = chr_anim_speed(chr)) visit(chr, speed, context);
    }
    return true;
}

// ChrSet (vtable 0: capacity = [+0x10]; 2: entry = [+0x18] + index * 0x10,
// ChrIns at entry +0); the open-field set is a ChrSet subclass.
int for_each_loaded_chr(const Statics& s,
    void (*visit)(std::uintptr_t chr, void* context), void* context) noexcept {
    if (!s.layout_2_7_1 || !s.world_chr_man) return 0;
    std::uintptr_t manager{};
    if (!read(s.world_chr_man, manager) || !manager) return 0;
    int sets = 0;
    const auto visit_set = [&](std::uintptr_t set) noexcept {
        std::uint32_t capacity{};
        std::uintptr_t entries{};
        if (!set || !read(set + 0x10, capacity) || !capacity || capacity > 8192 ||
            !read(set + 0x18, entries) || entries < 0x10000) return;
        ++sets;
        for (std::uint32_t i = 0; i < capacity; ++i) {
            std::uintptr_t chr{};
            if (read(entries + std::uintptr_t{i} * 0x10, chr) && chr >= 0x10000) visit(chr, context);
        }
    };
    visit_set(manager + 0x11040);
    for (int i = 0; i < 196; ++i) {
        std::uintptr_t set{};
        if (read(manager + 0x1DED8 + i * 8, set)) visit_set(set);
    }
    return sets;
}

bool save_identity(const Statics& s, SaveIdentity& out) noexcept {
    out = {};
    if (!s.layout_2_7_1 || !s.game_data_man) return false;
    std::uintptr_t data{}, player{};
    if (!read(s.game_data_man, data) || !data || !read(data + 0x8, player) || !player) return false;
    if (!safe_read(player + 0x9C, out.name, 16 * sizeof(wchar_t)) || !out.name[0]) return false;
    out.name[16] = 0;
    if (!read(data + 0x120, out.cycle) || out.cycle > 100) return false;
    std::uintptr_t game_man{};
    if (s.game_man && read(s.game_man, game_man) && game_man) {
        std::int32_t slot{};
        if (read(game_man + 0xAC0, slot) && slot >= 0 && slot < 100) out.slot = slot;
    }
    return true;
}

bool find_param(const Statics& s, std::wstring_view name, std::size_t row_size,
    ParamTable& table, LogFn log) noexcept {
    try {
        if (!s.layout_2_7_1 || !s.regulation_manager) return false;
        std::uintptr_t manager{};
        if (!read(s.regulation_manager, manager) || !manager) return false;
        std::uintptr_t begin{}, end{};
        if (!read(manager + 0x18, begin) || !read(manager + 0x20, end)) return false;
        if (!begin || end <= begin || (end - begin) / 8 < 100 || (end - begin) / 8 > 1000) return false;

        for (std::uintptr_t slot = begin; slot < end; slot += 8) {
            std::uintptr_t entry{};
            if (!read(slot, entry) || !entry) continue;
            std::uint64_t length{};
            if (!read(entry + 0x28, length) || length == 0 || length > 64) continue;
            wchar_t text[65]{};
            if (length <= 7) {
                if (!safe_read(entry + 0x18, text, length * 2)) continue;
            } else {
                std::uintptr_t heap{};
                if (!read(entry + 0x18, heap) || !safe_read(heap, text, length * 2)) continue;
            }
            if (name != std::wstring_view(text, static_cast<std::size_t>(length))) continue;

            std::uintptr_t file = 0;
            if (!read(entry + 0x80, file) || !read(file + 0x80, file) || !file) return false;
            std::uint16_t count{};
            if (!read(file + 0x0A, count) || count == 0) return false;

            std::vector<std::uintptr_t> rows;
            rows.reserve(count);
            std::map<std::int64_t, int> strides;
            std::int64_t previous = -1;
            for (std::uint16_t i = 0; i < count; ++i) {
                std::int64_t offset{};
                if (!read(file + 0x40 + i * 0x18 + 8, offset) || offset <= 0) return false;
                rows.push_back(file + static_cast<std::uintptr_t>(offset));
                if (previous >= 0) ++strides[offset - previous];
                previous = offset;
            }
            const auto common = std::max_element(strides.begin(), strides.end(),
                [](const auto& a, const auto& b) { return a.second < b.second; });
            const bool stride_ok = count == 1 || row_size == 0 ||  // 0: any stride (exploration)
                (common != strides.end() && common->first == static_cast<std::int64_t>(row_size));

            wchar_t message[200]{};
            swprintf_s(message, L"Vanilla param %.*ls: %u rows, stride %lld (expected %zu) -> %ls.",
                static_cast<int>(name.size()), name.data(), count,
                common != strides.end() ? static_cast<long long>(common->first) : 0LL, row_size,
                stride_ok ? L"ok" : L"layout mismatch, option disabled");
            if (log) log(message);
            if (!stride_ok) {
                table.file = file;  // found but unusable: tells the caller to stop retrying
                return false;
            }
            table.file = file;
            table.rows = std::move(rows);
            return true;
        }
        return false;
    } catch (...) {
        return false;
    }
}

bool layout_supported() noexcept {
    return game_has_2_7_1_layout();
}

} // namespace game
