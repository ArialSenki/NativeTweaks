#pragma once

#include <ernativeui/erui.h>

#include <functional>
#include <string>
#include <vector>

// Live language switching. Every row registered through track_* remembers how
// to rebuild its texts; apply() pushes them to the ERNativeUI fork, which
// swaps the menu texts without a restart. The official ERNativeUI lacks the
// fork exports, so supported() is false there and the switch needs a restart.
namespace live_text {

struct Texts {
    std::wstring label;
    std::wstring help;
    std::wstring page_title;   // submenus only
    std::wstring page_help;    // submenus only
    std::vector<std::wstring> values;  // CustomSlider rows only (same count)
};

// language: "en", "es", "zh" or "zht".
using Producer = std::function<Texts(const char* language)>;

void track_row(ERUI_RowHandle row, Producer producer);
void track_submenu(ERUI_PageHandle page, Producer producer);

[[nodiscard]] bool supported() noexcept;

// Returns the number of rows that could not be updated.
std::size_t apply(ERUI_ProviderHandle provider, const char* language) noexcept;
} // namespace live_text
