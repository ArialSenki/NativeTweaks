#pragma once

// Keyboard shortcuts for Vanilla Settings options and profiles, read from the
// [Hotkeys] section of NativeTweaks.ini:
//
//   GodMode            = F1             toggle a switch
//   SpeedGlobalPercent = Ctrl+F5 > 200  set a slider to 200, again: back
//   FovPercent         = F6 > +10       step a slider (+ or -)
//   Profile.Testing    = Ctrl+1         load a saved profile
//
// Keys: F1-F24, A-Z, 0-9, Num0-Num9, NumAdd/NumSubtract/NumMultiply/NumDivide,
// Insert, Delete, Home, End, PageUp, PageDown, Up, Down, Left, Right, Space,
// Tab, Backspace, Enter, Pause, ScrollLock, CapsLock, Minus, Plus, Comma,
// Period, plus Ctrl/Shift/Alt modifiers joined with '+'.

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace hotkeys {

enum Modifier : std::uint8_t {
    mod_none = 0,
    mod_ctrl = 1,
    mod_shift = 2,
    mod_alt = 4,
};

struct Chord {
    std::uint8_t key{};        // Windows virtual-key code
    std::uint8_t modifiers{};  // Modifier bits; must match exactly
};
inline bool operator==(const Chord& a, const Chord& b) noexcept {
    return a.key == b.key && a.modifiers == b.modifiers;
}

enum class ActionKind : std::uint8_t {
    toggle,      // flip a switch
    set_value,   // slider: set value, again restores the previous one
    step_value,  // slider: add value (may be negative)
    profile,     // load the named profile
};

struct Binding {
    std::wstring target;   // option key or profile name
    Chord chord{};
    ActionKind kind{ActionKind::toggle};
    int value{};
    std::wstring text;     // normalised chord text for menus ("Ctrl+F5")
};

// Parses "Ctrl+Shift+F1". Returns nothing on any unknown token.
[[nodiscard]] std::optional<Chord> parse_chord(std::wstring_view text);

// Parses one "key = value" line of [Hotkeys]. The key decides the target
// ("Profile.<name>" is a profile); value is "<chord> [> [+|-]N]".
[[nodiscard]] std::optional<Binding> parse_binding(
    std::wstring_view key, std::wstring_view value);

[[nodiscard]] std::wstring chord_text(const Chord& chord);

// Options and profiles the hotkeys act on (implemented by vanilla_settings).
struct Target {
    // Returns false when the option does not exist or is unavailable.
    bool (*is_toggle)(std::wstring_view key, bool& toggle) noexcept;
    bool (*get)(std::wstring_view key, int& value) noexcept;
    // Applies, saves and refreshes the visible row. Returns the applied value.
    bool (*set)(std::wstring_view key, int value, int& applied) noexcept;
    bool (*load_profile)(std::wstring_view name) noexcept;
    void (*log)(const wchar_t* message) noexcept;
    // Optional: told about the polling thread (for [Diagnostics] PerfLog).
    void (*thread_started)(const wchar_t* name, void* thread) noexcept;
};

// Starts the polling thread. Bindings are re-read when the INI changes, so
// players can edit them while the game runs.
void start(const std::wstring& config_path, const Target& target) noexcept;

// Chord text bound to an option (for its help line), or empty.
[[nodiscard]] std::wstring hotkey_for(std::wstring_view option_key);

// Reads the bindings from an INI file (exposed for tests).
[[nodiscard]] std::vector<Binding> read_bindings(const std::wstring& config_path);

} // namespace hotkeys
