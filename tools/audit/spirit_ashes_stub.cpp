// The audit tool does not link the Rust library (msa\); it never starts it.
#include "../../src/spirit_ashes.hpp"

namespace spirit_ashes {

bool start(HMODULE, const Settings&) noexcept { return false; }
void update(const Settings&) noexcept {}
void dismiss_all() noexcept {}
bool open_game_menu(int) noexcept { return false; }
int game_menu_last_result() noexcept { return 0; }
void set_summon_anywhere(bool) noexcept {}

} // namespace spirit_ashes
