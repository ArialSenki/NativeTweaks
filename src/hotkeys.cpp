#include "hotkeys.hpp"

#include "perf_log.hpp"

#include <Windows.h>

#include <algorithm>
#include <atomic>
#include <cwctype>
#include <map>

namespace hotkeys {
namespace {

std::wstring_view trim(std::wstring_view text) {
    while (!text.empty() && std::iswspace(text.front())) text.remove_prefix(1);
    while (!text.empty() && std::iswspace(text.back())) text.remove_suffix(1);
    return text;
}

std::wstring lower(std::wstring_view text) {
    std::wstring out(text);
    for (wchar_t& c : out) c = static_cast<wchar_t>(std::towlower(c));
    return out;
}

const std::map<std::wstring, int>& named_keys() {
    static const std::map<std::wstring, int> keys = [] {
        std::map<std::wstring, int> k{
            {L"insert", VK_INSERT}, {L"ins", VK_INSERT}, {L"delete", VK_DELETE}, {L"del", VK_DELETE},
            {L"home", VK_HOME}, {L"end", VK_END}, {L"pageup", VK_PRIOR}, {L"pgup", VK_PRIOR},
            {L"pagedown", VK_NEXT}, {L"pgdn", VK_NEXT}, {L"up", VK_UP}, {L"down", VK_DOWN},
            {L"left", VK_LEFT}, {L"right", VK_RIGHT}, {L"space", VK_SPACE}, {L"tab", VK_TAB},
            {L"backspace", VK_BACK}, {L"enter", VK_RETURN}, {L"pause", VK_PAUSE},
            {L"scrolllock", VK_SCROLL}, {L"capslock", VK_CAPITAL},
            {L"numadd", VK_ADD}, {L"numplus", VK_ADD}, {L"numsubtract", VK_SUBTRACT},
            {L"numminus", VK_SUBTRACT}, {L"nummultiply", VK_MULTIPLY}, {L"numdivide", VK_DIVIDE},
            {L"numdecimal", VK_DECIMAL}, {L"minus", VK_OEM_MINUS}, {L"plus", VK_OEM_PLUS},
            {L"comma", VK_OEM_COMMA}, {L"period", VK_OEM_PERIOD},
        };
        for (int i = 1; i <= 24; ++i) k[L"f" + std::to_wstring(i)] = VK_F1 + i - 1;
        for (int i = 0; i <= 9; ++i) k[L"num" + std::to_wstring(i)] = VK_NUMPAD0 + i;
        return k;
    }();
    return keys;
}

std::optional<std::uint8_t> parse_key(std::wstring_view token) {
    const std::wstring name = lower(token);
    if (name.size() == 1) {
        const wchar_t c = name[0];
        if (c >= L'a' && c <= L'z') return static_cast<std::uint8_t>(L'A' + (c - L'a'));
        if (c >= L'0' && c <= L'9') return static_cast<std::uint8_t>(c);
    }
    const auto& keys = named_keys();
    const auto found = keys.find(name);
    if (found == keys.end()) return std::nullopt;
    return static_cast<std::uint8_t>(found->second);
}

std::wstring key_name(std::uint8_t key) {
    if ((key >= 'A' && key <= 'Z') || (key >= '0' && key <= '9')) return std::wstring(1, key);
    if (key >= VK_F1 && key <= VK_F24) return L"F" + std::to_wstring(key - VK_F1 + 1);
    if (key >= VK_NUMPAD0 && key <= VK_NUMPAD9) return L"Num" + std::to_wstring(key - VK_NUMPAD0);
    static const struct { int code; const wchar_t* name; } names[]{
        {VK_INSERT, L"Insert"}, {VK_DELETE, L"Delete"}, {VK_HOME, L"Home"}, {VK_END, L"End"},
        {VK_PRIOR, L"PageUp"}, {VK_NEXT, L"PageDown"}, {VK_UP, L"Up"}, {VK_DOWN, L"Down"},
        {VK_LEFT, L"Left"}, {VK_RIGHT, L"Right"}, {VK_SPACE, L"Space"}, {VK_TAB, L"Tab"},
        {VK_BACK, L"Backspace"}, {VK_RETURN, L"Enter"}, {VK_PAUSE, L"Pause"},
        {VK_SCROLL, L"ScrollLock"}, {VK_CAPITAL, L"CapsLock"}, {VK_ADD, L"NumAdd"},
        {VK_SUBTRACT, L"NumSubtract"}, {VK_MULTIPLY, L"NumMultiply"}, {VK_DIVIDE, L"NumDivide"},
        {VK_DECIMAL, L"NumDecimal"}, {VK_OEM_MINUS, L"Minus"}, {VK_OEM_PLUS, L"Plus"},
        {VK_OEM_COMMA, L"Comma"}, {VK_OEM_PERIOD, L"Period"},
    };
    for (const auto& entry : names)
        if (entry.code == key) return entry.name;
    return L"?";
}

std::optional<int> parse_int(std::wstring_view text) {
    text = trim(text);
    if (text.empty() || text.size() > 6) return std::nullopt;
    bool negative = false;
    if (text.front() == L'+' || text.front() == L'-') {
        negative = text.front() == L'-';
        text.remove_prefix(1);
    }
    if (text.empty()) return std::nullopt;
    int value = 0;
    for (wchar_t c : text) {
        if (c < L'0' || c > L'9') return std::nullopt;
        value = value * 10 + (c - L'0');
    }
    return negative ? -value : value;
}

// ---- runtime state --------------------------------------------------------

SRWLOCK g_lock = SRWLOCK_INIT;
std::atomic<bool> g_sound{true};
std::vector<Binding> g_bindings;
std::atomic<unsigned> g_bindings_version{};  // bumped by every reload
std::map<std::wstring, int> g_previous;  // set_value: value to return to
std::wstring g_config_path;
Target g_target{};
FILETIME g_config_time{};

// True once the INI changed and then stayed untouched for 300 ms: editors and
// WritePrivateProfileString rewrite the file several times while saving.
ULONGLONG g_change_seen{};
bool config_changed() {
    WIN32_FILE_ATTRIBUTE_DATA data{};
    if (!GetFileAttributesExW(g_config_path.c_str(), GetFileExInfoStandard, &data)) return false;
    const ULONGLONG now = GetTickCount64();
    if (CompareFileTime(&data.ftLastWriteTime, &g_config_time) != 0) {
        g_config_time = data.ftLastWriteTime;
        g_change_seen = now;
        return false;
    }
    if (!g_change_seen || now - g_change_seen < 300) return false;
    g_change_seen = 0;
    return true;
}

bool same_bindings(const std::vector<Binding>& a, const std::vector<Binding>& b) {
    if (a.size() != b.size()) return false;
    for (std::size_t i = 0; i < a.size(); ++i) {
        if (a[i].target != b[i].target || !(a[i].chord == b[i].chord) ||
            a[i].kind != b[i].kind || a[i].value != b[i].value) {
            return false;
        }
    }
    return true;
}

void logf(const wchar_t* format, const std::wstring& a, const std::wstring& b = {}, int n = 0) {
    if (!g_target.log) return;
    wchar_t buffer[512]{};
    swprintf_s(buffer, format, a.c_str(), b.c_str(), n);
    g_target.log(buffer);
}

void reload(bool first = false) {
    std::vector<Binding> bindings = read_bindings(g_config_path);
    g_sound.store(GetPrivateProfileIntW(L"Hotkeys", L"Sound", 1, g_config_path.c_str()) != 0,
        std::memory_order_relaxed);
    AcquireSRWLockExclusive(&g_lock);
    const bool changed = !same_bindings(g_bindings, bindings);
    g_bindings.swap(bindings);
    const std::size_t total = g_bindings.size();
    g_bindings_version.fetch_add(1, std::memory_order_release);
    ReleaseSRWLockExclusive(&g_lock);
    // NativeTweaks rewrites the INI after every change; log real edits only.
    if (changed || first) logf(L"Hotkeys: %s%s%d binding(s) loaded.", L"", L"", static_cast<int>(total));
}

std::uint8_t current_modifiers() {
    std::uint8_t mods = mod_none;
    if (GetAsyncKeyState(VK_CONTROL) & 0x8000) mods |= mod_ctrl;
    if (GetAsyncKeyState(VK_SHIFT) & 0x8000) mods |= mod_shift;
    if (GetAsyncKeyState(VK_MENU) & 0x8000) mods |= mod_alt;
    return mods;
}

void tone(bool on) {
    if (g_sound.load(std::memory_order_relaxed)) Beep(on ? 880 : 440, 70);
}

void run(const Binding& binding) {
    if (binding.kind == ActionKind::profile) {
        const bool ok = g_target.load_profile && g_target.load_profile(binding.target);
        logf(ok ? L"Hotkeys: %s loaded profile %s." : L"Hotkeys: %s could not load profile %s.",
            binding.text, binding.target);
        tone(ok);
        return;
    }
    bool is_toggle = false;
    int current = 0;
    if (!g_target.is_toggle || !g_target.is_toggle(binding.target, is_toggle) ||
        !g_target.get(binding.target, current)) {
        logf(L"Hotkeys: %s targets %s, which is unknown or unavailable.", binding.text, binding.target);
        return;
    }
    int wanted = current;
    if (is_toggle || binding.kind == ActionKind::toggle) {
        wanted = current ? 0 : 1;
        if (!is_toggle) {
            logf(L"Hotkeys: %s: %s is a slider; use \"> value\".", binding.text, binding.target);
            return;
        }
    } else if (binding.kind == ActionKind::set_value) {
        AcquireSRWLockExclusive(&g_lock);
        if (current == binding.value) {
            const auto previous = g_previous.find(binding.target);
            wanted = previous != g_previous.end() ? previous->second : current;
        } else {
            g_previous[binding.target] = current;
            wanted = binding.value;
        }
        ReleaseSRWLockExclusive(&g_lock);
    } else {
        wanted = current + binding.value;
    }
    int applied = current;
    if (!g_target.set(binding.target, wanted, applied)) return;
    logf(L"Hotkeys: %s set %s to %d.", binding.text, binding.target, applied);
    tone(is_toggle ? applied != 0 : applied >= current);
}

// The INI is checked once a second: under MO2 every file query goes through
// its virtual file system, which the game's own file loading also waits on.
constexpr ULONGLONG kConfigCheckMs = 1000;

DWORD WINAPI worker(void*) noexcept {
    std::vector<Chord> held;
    std::vector<Binding> bindings;
    unsigned bindings_version = ~0u;
    ULONGLONG next_config_check{};
    perf::Loop loop(L"hotkeys");
    for (;;) {
        // Without bindings only the INI is watched.
        loop.sleep(bindings.empty() ? 250 : 15);
        try {
            const ULONGLONG tick = GetTickCount64();
            if (tick >= next_config_check) {
                next_config_check = tick + kConfigCheckMs;
                if (config_changed()) reload();
            }
            if (g_bindings_version.load(std::memory_order_acquire) != bindings_version) {
                AcquireSRWLockShared(&g_lock);
                bindings = g_bindings;
                bindings_version = g_bindings_version.load(std::memory_order_relaxed);
                ReleaseSRWLockShared(&g_lock);
                held.clear();
            }
            if (bindings.empty()) continue;
            DWORD pid{};
            GetWindowThreadProcessId(GetForegroundWindow(), &pid);
            if (pid != GetCurrentProcessId()) {
                held.clear();
                continue;
            }
            const std::uint8_t mods = current_modifiers();
            std::vector<Chord> now;
            for (const Binding& binding : bindings) {
                if (!(GetAsyncKeyState(binding.chord.key) & 0x8000)) continue;
                if (binding.chord.modifiers != mods) continue;
                now.push_back(binding.chord);
                if (std::find(held.begin(), held.end(), binding.chord) != held.end()) continue;
                run(binding);
            }
            held.swap(now);
        } catch (...) {
        }
    }
}

} // namespace

std::optional<Chord> parse_chord(std::wstring_view text) {
    Chord chord{};
    bool have_key = false;
    text = trim(text);
    if (text.empty()) return std::nullopt;
    std::size_t start = 0;
    for (;;) {
        // '+' separates tokens; a trailing "Plus" names the key itself.
        const std::size_t plus = text.find(L'+', start);
        const std::wstring_view token = trim(text.substr(start, plus == std::wstring_view::npos ? text.npos : plus - start));
        const std::wstring name = lower(token);
        if (name == L"ctrl" || name == L"control") chord.modifiers |= mod_ctrl;
        else if (name == L"shift") chord.modifiers |= mod_shift;
        else if (name == L"alt") chord.modifiers |= mod_alt;
        else {
            const auto key = parse_key(token);
            if (!key || have_key) return std::nullopt;
            chord.key = *key;
            have_key = true;
        }
        if (plus == std::wstring_view::npos) break;
        start = plus + 1;
    }
    if (!have_key) return std::nullopt;
    return chord;
}

std::wstring chord_text(const Chord& chord) {
    std::wstring text;
    if (chord.modifiers & mod_ctrl) text += L"Ctrl+";
    if (chord.modifiers & mod_shift) text += L"Shift+";
    if (chord.modifiers & mod_alt) text += L"Alt+";
    return text + key_name(chord.key);
}

std::optional<Binding> parse_binding(std::wstring_view key, std::wstring_view value) {
    key = trim(key);
    value = trim(value);
    // Inline comments.
    if (const std::size_t semicolon = value.find(L';'); semicolon != value.npos)
        value = trim(value.substr(0, semicolon));
    if (key.empty() || value.empty()) return std::nullopt;
    Binding binding{};
    const std::wstring_view prefix = L"Profile.";
    if (key.size() > prefix.size() && lower(key.substr(0, prefix.size())) == L"profile.") {
        binding.kind = ActionKind::profile;
        binding.target = std::wstring(trim(key.substr(prefix.size())));
    } else {
        binding.target = std::wstring(key);
    }
    std::wstring_view chord_part = value;
    const std::size_t arrow = value.find(L'>');
    if (arrow != value.npos) {
        if (binding.kind == ActionKind::profile) return std::nullopt;
        chord_part = trim(value.substr(0, arrow));
        std::wstring_view amount = trim(value.substr(arrow + 1));
        const bool relative = !amount.empty() && (amount.front() == L'+' || amount.front() == L'-');
        const auto number = parse_int(amount);
        if (!number) return std::nullopt;
        binding.kind = relative ? ActionKind::step_value : ActionKind::set_value;
        binding.value = *number;
    }
    const auto chord = parse_chord(chord_part);
    if (!chord) return std::nullopt;
    binding.chord = *chord;
    binding.text = chord_text(*chord);
    return binding;
}

std::vector<Binding> read_bindings(const std::wstring& config_path) {
    std::vector<Binding> bindings;
    std::vector<wchar_t> section(32768);
    const DWORD length = GetPrivateProfileSectionW(
        L"Hotkeys", section.data(), static_cast<DWORD>(section.size()), config_path.c_str());
    for (const wchar_t* line = section.data(); line < section.data() + length && *line;
         line += std::wcslen(line) + 1) {
        const std::wstring_view text(line);
        if (text.empty() || text.front() == L';' || text.front() == L'#') continue;
        const std::size_t equals = text.find(L'=');
        if (equals == text.npos) continue;
        const std::wstring_view key = trim(text.substr(0, equals));
        if (lower(key) == L"sound") continue;  // reserved setting
        auto binding = parse_binding(key, text.substr(equals + 1));
        if (binding) bindings.push_back(std::move(*binding));
    }
    return bindings;
}

void start(const std::wstring& config_path, const Target& target) noexcept {
    try {
        g_config_path = config_path;
        g_target = target;
        config_changed();
        g_change_seen = 0;  // the first read below covers it
        reload(true);
        if (HANDLE thread = CreateThread(nullptr, 0, &worker, nullptr, 0, nullptr)) {
            if (g_target.thread_started) g_target.thread_started(L"hotkeys", thread);
            CloseHandle(thread);
        }
    } catch (...) {
    }
}

std::wstring hotkey_for(std::wstring_view option_key) {
    std::wstring result;
    AcquireSRWLockShared(&g_lock);
    for (const Binding& binding : g_bindings) {
        if (binding.kind == ActionKind::profile) continue;
        if (lower(binding.target) != lower(option_key)) continue;
        if (!result.empty()) result += L", ";
        result += binding.text;
        if (binding.kind == ActionKind::set_value) result += L" > " + std::to_wstring(binding.value);
        if (binding.kind == ActionKind::step_value)
            result += (binding.value >= 0 ? L" +" : L" ") + std::to_wstring(binding.value);
    }
    ReleaseSRWLockShared(&g_lock);
    return result;
}

} // namespace hotkeys
