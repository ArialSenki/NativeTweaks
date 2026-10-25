#pragma once

#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <string_view>
#include <vector>

// Guarded access to Elden Ring's runtime structures for Vanilla Settings.
// Every read and write is protected against access violations, so a stale
// pointer (loading screen, main menu, quit-out) fails softly instead of
// crashing the game.
namespace game {

using LogFn = void (*)(const wchar_t*) noexcept;

// The image every pattern scan reads: eldenring.exe in the game, or a copy an
// offline audit mapped with LoadLibraryEx (set_image). HMODULE as void*.
void* image() noexcept;
void set_image(void* module) noexcept;

// True when the game's structure layout is the one the options use (2.7.1.0,
// 2.7.1.1, 2.7.2.0). Reads only the executable's version resource.
bool layout_supported() noexcept;

bool safe_read(std::uintptr_t address, void* out, std::size_t size) noexcept;
bool safe_write(std::uintptr_t address, const void* in, std::size_t size) noexcept;

template <class T>
bool read(std::uintptr_t address, T& out) noexcept { return safe_read(address, &out, sizeof(T)); }
template <class T>
bool write(std::uintptr_t address, const T& in) noexcept { return safe_write(address, &in, sizeof(T)); }

// Follows base -> [base] -> [p+o0] -> ... -> p+o_last. Returns 0 on failure.
std::uintptr_t chain(std::uintptr_t base, std::initializer_list<std::ptrdiff_t> offsets) noexcept;

// Statics resolved from code references in eldenring.exe.
struct Statics {
    std::uintptr_t world_chr_man{};       // address of the WorldChrMan pointer
    std::uintptr_t chr_dbg_flags{};       // base of the ChrDbgFlags byte block
    std::uintptr_t regulation_manager{};  // address of the CSRegulationManager pointer
    std::uintptr_t game_data_man{};       // address of the GameDataMan pointer
    std::uintptr_t game_man{};            // address of the GameMan pointer
    bool layout_2_7_1{};                  // structure offsets below are for 2.7.1.0 (also 2.7.1.1, 2.7.2.0)
};

Statics resolve_statics(LogFn log) noexcept;

// Player character fields (2.7.1.0 layout).
constexpr std::ptrdiff_t kPlayerIns = 0x1E508;
constexpr std::ptrdiff_t kModules = 0x190;
std::uintptr_t player_points(const Statics& s) noexcept;     // &hp; fp=+0x10, stamina=+0x1C
std::uintptr_t player_poise(const Statics& s) noexcept;      // f32 current poise; +0x8 = f32 added to the maximum
std::uintptr_t player_anim_speed(const Statics& s) noexcept; // float
std::uintptr_t player_anim_id(const Statics& s) noexcept;    // int32, the animation playing now
std::uintptr_t chr_anim_id(std::uintptr_t chr) noexcept;     // the same for any ChrIns
std::uintptr_t player_fall_timer(const Statics& s) noexcept;    // f32, CSChrFallModule::fall_timer
std::uintptr_t player_resist(const Statics& s) noexcept;        // i32[7] resistance left, then i32[7] maximum
std::uintptr_t player_special_effect(const Statics& s) noexcept; // value of PlayerIns+0x178 (SpEffect object)

// Address of an FD4Singleton's static pointer, by reflection name. Call after
// the game has initialised (the names come from reflection data). Every
// singleton whose name contains list_filter is logged.
std::uintptr_t find_singleton(const char* name, LogFn log, const char* list_filter = nullptr) noexcept;

// Loaded characters (player, enemies, NPCs, Torrent, spirit ashes). The vector
// offset inside WorldChrMan is found by scanning; 0 means not found yet.
std::ptrdiff_t find_chr_vector(const Statics& s, LogFn log) noexcept;
bool for_each_chr_speed(const Statics& s, std::ptrdiff_t vector_offset,
    void (*visit)(std::uintptr_t chr, std::uintptr_t speed, void* context), void* context) noexcept;

// Every character in WorldChrMan's ChrSets: the open-field set (+0x11040) and
// chr_sets[196] (+0x1DED8), 2.7.1.0. Unlike the update list this includes
// characters that are loaded but not updating. A character may be visited
// twice when two sets share it. Returns the number of sets read.
int for_each_loaded_chr(const Statics& s,
    void (*visit)(std::uintptr_t chr, void* context), void* context) noexcept;

// The loaded character's save slot (GameMan, -1 when unknown), name (GameDataMan
// -> PlayerGameData +0x9C, UTF-16) and New Game+ cycle (GameDataMan +0x120).
struct SaveIdentity {
    std::int32_t slot{-1};
    std::uint32_t cycle{};
    wchar_t name[17]{};
};
bool save_identity(const Statics& s, SaveIdentity& out) noexcept;

// A loaded param table, found by name in the regulation manager.
struct ParamTable {
    std::uintptr_t file{};
    std::vector<std::uintptr_t> rows{};
};

// Finds a param table and checks that its row stride matches row_size
// (0 accepts any stride; the log line still reports the real one).
bool find_param(const Statics& s, std::wstring_view name, std::size_t row_size,
    ParamTable& table, LogFn log) noexcept;

} // namespace game
