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
};

// Once, early (the HUD hook must be in before the HUD movie opens). False when
// the game version is not supported or MultipleSpiritAshes.dll is loaded too.
bool start(HMODULE own_module, const Settings& settings) noexcept;

// Applies changed settings immediately.
void update(const Settings& settings) noexcept;

// Dismisses every summoned ash with the short fade (next game frame).
void dismiss_all() noexcept;

} // namespace spirit_ashes
