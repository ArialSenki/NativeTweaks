#include "spirit_ashes.hpp"

namespace {

// Matches NtMsaSettings in msa\src\lib.rs.
struct NtMsaSettings {
    std::uint8_t enabled;
    std::uint32_t max_active_ashes;
    std::uint8_t log_enabled;
    std::uint8_t toggle_dismiss;
    std::uint8_t suppress_prompts;
    std::uint8_t follow_areas;
    std::uint8_t fast_dismissal;
    std::uint8_t low_fp_dismissal;
    std::uint8_t hud_rows;
    std::uint32_t extra_cost_percent;
    std::uint32_t damage_reduction_percent;
    std::uint8_t repeat_same_ash;
};
static_assert(sizeof(NtMsaSettings) == 28, "must match the #[repr(C)] struct in Rust");

NtMsaSettings to_rust(const spirit_ashes::Settings& settings) noexcept {
    const auto byte = [](bool value) { return static_cast<std::uint8_t>(value ? 1 : 0); };
    return {byte(settings.enabled), settings.max_active, byte(settings.log), byte(settings.toggle_dismiss),
        byte(settings.suppress_prompts), byte(settings.follow_areas), byte(settings.fast_dismissal),
        byte(settings.low_fp_dismissal), byte(settings.hud_rows), settings.extra_cost_percent,
        settings.damage_reduction_percent, byte(settings.repeat_same_ash)};
}

} // namespace

extern "C" std::uint8_t nt_msa_start(void* module, const NtMsaSettings* settings);
extern "C" void nt_msa_set(const NtMsaSettings* settings);
extern "C" void nt_msa_dismiss_all();
extern "C" std::uint8_t nt_open_game_menu(int kind);
extern "C" void nt_msa_set_summon_anywhere(std::uint8_t enabled);
extern "C" int nt_game_menu_last_result();

namespace spirit_ashes {

bool start(HMODULE own_module, const Settings& settings) noexcept {
    const NtMsaSettings rust = to_rust(settings);
    return nt_msa_start(own_module, &rust) != 0;
}

void update(const Settings& settings) noexcept {
    const NtMsaSettings rust = to_rust(settings);
    nt_msa_set(&rust);
}

void dismiss_all() noexcept {
    nt_msa_dismiss_all();
}

void set_summon_anywhere(bool enabled) noexcept {
    nt_msa_set_summon_anywhere(enabled ? 1 : 0);
}

bool open_game_menu(int kind) noexcept {
    return nt_open_game_menu(kind) != 0;
}

int game_menu_last_result() noexcept {
    return nt_game_menu_last_result();
}

} // namespace spirit_ashes
