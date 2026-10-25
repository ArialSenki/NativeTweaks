// The audit tool does not link the Rust library (msa\); it never starts it.
#include "../../src/spirit_ashes.hpp"

namespace spirit_ashes {

bool start(HMODULE, const Settings&) noexcept { return false; }
void update(const Settings&) noexcept {}
void dismiss_all() noexcept {}

} // namespace spirit_ashes
