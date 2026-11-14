#pragma once

#include <Windows.h>

#include <cstdint>
#include <string>

// Multiple Spirit Ashes (Nexus 11158), built in from msa\ (Rust static
// library): several spirit ashes at once, dismissing one of them, and compact
// HUD rows for each. The Hub only starts it and passes the page's settings.
namespace spirit_ashes {

struct Settings {
    bool enabled;
    std::uint32_t max_active;
    bool log;
    bool toggle_dismiss;     // using a summoned ash's item again dismisses only it
    bool suppress_prompts;   // no spirit ash confirmations at all
    bool follow_areas;       // native warp after a block change or other player jump
    bool fast_dismissal;     // 1.5 s fade instead of the full native exit animation
    bool low_fp_dismissal;   // summoned ashes' items stay usable with too little FP
    bool hud_rows;           // compact HUD rows (read at startup)
    std::uint32_t extra_cost_percent;        // FP cost of each additional ash (100 = native)
    std::uint32_t damage_reduction_percent;  // damage lost per extra living ash (0 = native)
    bool repeat_same_ash;    // a summoned ash's item summons it again instead of dismissing it
};

// Once, early (the HUD hook must be in before the HUD movie opens). False when
// the game version is not supported or MultipleSpiritAshes.dll is loaded too.
bool start(HMODULE own_module, const Settings& settings) noexcept;

// Applies changed settings immediately.
void update(const Settings& settings) noexcept;

// Dismisses every summoned ash with the short fade (next game frame).
void dismiss_all() noexcept;

// Not spirit ashes, but in the same Rust library (msa\src\game_menus.rs):
// opens a game screen on the next frame, as an NPC's talk script would.
// 1 = mirror (appearance), 3 = rebirth.
// False when the game version or its task scheduler isn't supported.
bool open_game_menu(int kind) noexcept;

// Summon anywhere beyond the monument's own map tile: while the game finds no
// summoning monument near the player, the last one it found is kept.
void set_summon_anywhere(bool enabled) noexcept;
// Where summoned ashes fight freely without warps: 0 off, 1 boss arenas,
// 2 boss arenas and any fight on foot (msa area_follow, default 2).
void set_free_fighting(std::uint8_t mode) noexcept;
// 0 none yet, 1 opened, 2 no character loaded, 3 refused by the game.
int game_menu_last_result() noexcept;
// Opens a shop over ShopLineupParam rows begin..end on the next frame
// (talk command OpenRegularShop), like open_game_menu.
bool open_shop(int begin, int end) noexcept;

} // namespace spirit_ashes

// NPC Spawner (the author's mod), built into the same Rust library
// (msa\src\npc.rs, NPCTestArena\mod): spawn any NPC or enemy, with its own
// Mod Settings page and F11 row. Started once, early, when its option is on.
namespace npc_spawner {

// False in a build without it.
bool built_in() noexcept;
// The standalone NpcSpawner.dll is loaded or in `folder`: the built-in copy stays off.
bool standalone_present(const std::wstring& folder) noexcept;
// Starts it (own threads, never blocks); NpcSpawner.ini and .log in `folder`.
bool start(const std::wstring& folder, HMODULE own_module) noexcept;
// The same, but without a Mod Settings provider of its own: add_pages puts
// its pages into NativeTweaks' menu (after Vanilla Settings).
bool start_hosted(const std::wstring& folder, HMODULE own_module) noexcept;
// From NativeTweaks' menu registration, before its commit (ERUI handles).
bool add_pages(std::uint64_t provider, std::uint64_t parent_page) noexcept;

} // namespace npc_spawner
