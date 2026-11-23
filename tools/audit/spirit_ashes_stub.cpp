// The audit tool does not link the Rust library (msa\); it never starts it.
#include "../../src/spirit_ashes.hpp"

#include <cstdint>

extern "C" std::uint8_t nt_set_deflect_window(std::uint32_t) { return 0; }
extern "C" std::uint8_t nt_summon_melina() { return 0; }
extern "C" std::uint8_t nt_set_player_clean(std::uint8_t) { return 0; }
extern "C" std::uint8_t nt_msa_game_thread_ready() { return 0; }
extern "C" std::uint8_t nt_msa_on_game_thread(std::uint8_t (*)(std::uintptr_t), std::uintptr_t, std::uint32_t) {
    return 0;
}

namespace spirit_ashes {

bool start(HMODULE, const Settings&) noexcept { return false; }
void update(const Settings&) noexcept {}
void dismiss_all() noexcept {}
bool open_game_menu(int) noexcept { return false; }
bool open_shop(int, int) noexcept { return false; }
int game_menu_last_result() noexcept { return 0; }
void set_summon_anywhere(bool) noexcept {}
void set_free_fighting(std::uint8_t) noexcept {}

} // namespace spirit_ashes

namespace npc_spawner {

bool built_in() noexcept { return false; }
bool standalone_present(const std::wstring&) noexcept { return false; }
bool start(const std::wstring&, HMODULE) noexcept { return false; }
bool start_hosted(const std::wstring&, HMODULE) noexcept { return false; }
bool add_pages(std::uint64_t, std::uint64_t) noexcept { return false; }

} // namespace npc_spawner

extern "C" std::uintptr_t nt_top_menu_job() { return 0; }
extern "C" std::uint8_t nt_flask_state(std::int32_t*) { return 0; }
extern "C" std::uint8_t nt_keep_player_effect(std::int32_t) { return 1; }
extern "C" std::uint8_t nt_keep_player_effect_slot(std::uint32_t, std::int32_t) { return 1; }
