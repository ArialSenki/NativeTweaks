#pragma once

#include <ernativeui/ERNativeUI.hpp>

#include <string>
#include <string_view>

namespace vanilla {

using LogFn = void (*)(const wchar_t*) noexcept;

// Provided by native_tweaks.cpp: the DLLs\NativeTweaks\lang\<lang>.txt override
// for this English text, or fallback when there is none.
const wchar_t* translate_text(std::string_view lang, const wchar_t* english,
    const wchar_t* fallback) noexcept;

// Scans eldenring.exe for every supported patch site, reads the [Vanilla]
// section of config_path and applies the saved choices. Call once from the
// startup worker, before the menu is registered.
void initialize(const std::wstring& config_path, LogFn log) noexcept;

// Offline check: resolves every patch site and static in game::image()
// without writing anything, logs each result and returns how many options
// would be greyed out. Used by tools\NativeTweaksAudit.
int audit(LogFn log) noexcept;

// Adds the "Vanilla Settings" submenu with one child page per category.
// Every control applies immediately and is persisted to the [Vanilla] section.
// language: "en", "es" or "zh".
void add_pages(erui::Page& parent, const char* language) noexcept;

// Sends the saved HUD/menu layout to ERNativeUI (logs it as pending when the
// host is not loaded yet). Call it again once the host API is connected.
void sync_hud_layout(const wchar_t* when) noexcept;

// Lets the "restore defaults" button refresh the visible rows.
void attach_registration(const erui::Registration* registration) noexcept;

} // namespace vanilla
