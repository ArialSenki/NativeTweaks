#pragma once

#include <Windows.h>

#include <cstdint>

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
// 0 none yet, 1 opened, 2 no character loaded, 3 refused by the game.
int game_menu_last_result() noexcept;

} // namespace spirit_ashes
