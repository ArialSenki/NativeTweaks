#include <ernativeui/ERNativeUI.hpp>

#include "game_memory.hpp"
#include "live_text.hpp"
#include "perf_log.hpp"
#include "traditional_chinese.hpp"
#include "vanilla_settings.hpp"

#include <Windows.h>
#include <TlHelp32.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <climits>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cwchar>
#include <cwctype>
#include <iterator>
#include <memory>
#include <new>
#include <string>
#include <string_view>
#include <tuple>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace {

HMODULE g_own_module{};
erui::Registration g_registration{};
// Build tag shown in the log and, with ERUI 1.3, on the title screen.
constexpr wchar_t kBuildTag[] = L"r135";
SRWLOCK g_settings_lock = SRWLOCK_INIT;
SRWLOCK g_save_lock = SRWLOCK_INIT;
SRWLOCK g_log_lock = SRWLOCK_INIT;
SRWLOCK g_generic_lock = SRWLOCK_INIT;
std::wstring g_ini_path{};
std::wstring g_log_path{};
std::wstring g_config_path{};
bool g_log_enabled{};

// How a discovered INI value is shown. `text` means no usable control: such a
// value is left out of the menu, because ERNativeUI does not draw text inputs.
enum class GenericControl : std::uint8_t { text, toggle, slider, choice };

struct GenericIniSetting {
    std::string section;
    std::string key;
    std::wstring label;
    std::wstring help;                 // empty -> generic help text
    std::wstring category;             // empty -> mod's main page
    GenericControl control{GenericControl::text};
    std::vector<std::wstring> tokens;  // INI value written for each toggle/choice index
    std::vector<std::wstring> choice_labels;
    std::int32_t slider_minimum{};
    std::int32_t slider_maximum{};
    std::int32_t slider_step{1};
    std::uint8_t initial_index{};
    std::wstring initial_value;
    std::wstring current_value;
    std::string comment;               // INI comment lines above the key + inline comment
    // Texts in every menu language (index: Language), for live switching.
    // Empty means "use the default" (key name / generic help / no category).
    std::array<std::wstring, 4> label_by_language;
    std::array<std::wstring, 4> help_by_language;
    std::array<std::wstring, 4> category_by_language;
};

struct GenericIniMod {
    std::wstring path;
    std::wstring module_path;
    std::wstring display_name;
    std::wstring help;                 // [NativeTweaks] Description=
    std::vector<GenericIniSetting> settings;
    std::array<std::wstring, 4> name_by_language;
    std::array<std::wstring, 4> help_by_language;
};

std::vector<GenericIniMod> g_generic_mods{};

void ERUI_CALL generic_value_changed(void* target, std::uint8_t value) noexcept;
void ERUI_CALL generic_save_clicked(void* target) noexcept;

struct Settings {
    std::uint8_t general_enabled{1};
    std::uint8_t bar_size_enabled{1};
    std::uint8_t fit_screen{1};
    std::uint8_t hp_enabled{1};
    std::uint8_t fp_enabled{1};
    std::uint8_t stamina_enabled{1};
    std::uint8_t hp_percent{100};
    std::uint8_t fp_percent{100};
    std::uint8_t stamina_percent{100};
};

struct Rows {
    erui::RowHandle general_enabled{};
    erui::RowHandle bar_size_enabled{};
    erui::RowHandle hp_enabled{};
    erui::RowHandle fp_enabled{};
    erui::RowHandle stamina_enabled{};
    erui::RowHandle hp_percent{};
    erui::RowHandle fp_percent{};
    erui::RowHandle stamina_percent{};
};

struct Texts {
    const wchar_t* provider;
    const wchar_t* mods;
    const wchar_t* mods_help;
    const wchar_t* hud;
    const wchar_t* hud_help;
    const wchar_t* unavailable;
    const wchar_t* unavailable_help;
    const wchar_t* general_enabled;
    const wchar_t* general_help;
    const wchar_t* bar_size_enabled;
    const wchar_t* bar_size_help;
    const wchar_t* hp_enabled;
    const wchar_t* fp_enabled;
    const wchar_t* stamina_enabled;
    const wchar_t* bar_toggle_help;
    const wchar_t* hp_width;
    const wchar_t* fp_width;
    const wchar_t* stamina_width;
    const wchar_t* width_help;
    const wchar_t* save;
    const wchar_t* save_help;
    const wchar_t* fit_screen_button;
    const wchar_t* fit_screen_button_help;
    const wchar_t* generic_settings_help;
    const wchar_t* generic_save;
    const wchar_t* generic_save_help;
    const wchar_t* no_mods;
    const wchar_t* no_mods_help;
};

const Texts* g_texts{};
Settings g_current{};
Settings g_initial{};
Rows g_rows{};
bool g_target_available{};
bool g_config_valid{};
bool g_fit_screen_values_safe{};

const Texts kEnglish{
    L"NativeTweaks", L"Mods", L"Open settings for supported mods.",
    L"Tarnished HUD", L"Adjust the HUD bars provided by Tarnished HUD.",
    L"Tarnished HUD unavailable",
    L"Install TarnishedHUD.dll with its INI beside it. Startup discovery finds the loaded module.",
    L"Tarnished HUD enabled", L"Enable or disable Tarnished HUD. Applies at once with Tarnished HUD build 4; older builds need a restart.",
    L"Custom bar sizing", L"Enable the width settings below. Applied after saving.",
    L"Custom HP sizing", L"Custom FP sizing", L"Custom stamina sizing",
    L"Apply this bar's custom width; off restores its standard length.",
    L"HP width (%)", L"FP width (%)", L"Stamina width (%)",
    L"Adjust from 1 to 100 percent. Applied after saving.",
    L"Save changes", L"Write changed settings to TarnishedHUD.ini and apply supported values now.",
    L"Enable Fit Screen sizing", L"Change this INI to percentage-based widths. Applies at once with Tarnished HUD build 4; older builds need a restart.",
    L"Edit this mod's INI. Save applies it now when its DLL supports the reload API; otherwise a restart is needed.",
    L"Save changes", L"Back up and save this mod's INI, then request a safe live reload from its DLL.",
    L"No configurable mods found", L"Mods with an INI beside their DLL appear here after a restart."
};

const Texts kSpanish{
    L"NativeTweaks", L"Mods", L"Abre los ajustes de los mods compatibles.",
    L"Tarnished HUD", L"Ajusta las barras de Tarnished HUD.",
    L"Tarnished HUD no disponible",
    L"Instala TarnishedHUD.dll con su INI junto a la DLL. La detección de inicio encuentra el módulo cargado.",
    L"Tarnished HUD activado", L"Activa o desactiva Tarnished HUD. Se aplica al momento con la versión 4 de Tarnished HUD; las anteriores requieren reiniciar.",
    L"Tamaño personalizado de barras", L"Activa los ajustes de anchura. Se aplica al guardar.",
    L"Ajuste de vida personalizado", L"Ajuste de PC personalizado", L"Ajuste de aguante personalizado",
    L"Aplica el ancho personalizado; al desactivarlo restaura el ancho estándar.",
    L"Anchura de vida (%)", L"Anchura de PC (%)", L"Anchura de aguante (%)",
    L"Ajusta del 1 al 100 por ciento. Se aplica al guardar.",
    L"Guardar cambios", L"Guarda TarnishedHUD.ini y aplica ahora los ajustes compatibles.",
    L"Activar ajuste a pantalla", L"Cambia el INI a anchuras porcentuales. Se aplica al momento con la versión 4 de Tarnished HUD; las anteriores requieren reiniciar.",
    L"Edita el INI del mod. Al guardar, se aplica ahora si su DLL admite la recarga; si no, hará falta reiniciar.",
    L"Guardar cambios", L"Crea una copia de seguridad, guarda el INI y solicita una recarga segura al DLL.",
    L"No hay mods configurables", L"Los mods con un INI junto a su DLL aparecen aquí tras reiniciar."
};

const Texts kChinese{
    L"NativeTweaks", L"模组", L"打开受支持模组的设置。",
    L"Tarnished HUD", L"调整 Tarnished HUD 提供的 HUD 状态条。",
    L"Tarnished HUD 不可用",
    L"请安装 TarnishedHUD.dll，并将其 INI 放在旁边。启动时会自动检测已加载的模块。",
    L"启用 Tarnished HUD", L"启用或禁用 Tarnished HUD。Tarnished HUD 版本 4 立即生效；旧版本需要重启游戏。",
    L"自定义状态条长度", L"启用下方的宽度设置。保存后生效。",
    L"自定义 HP 条", L"自定义 FP 条", L"自定义精力条",
    L"应用此状态条的自定义宽度；关闭后恢复标准长度。",
    L"HP 条宽度 (%)", L"FP 条宽度 (%)", L"精力条宽度 (%)",
    L"可在 1% 到 100% 之间调整。保存后生效。",
    L"保存更改", L"将更改写入 TarnishedHUD.ini，并立即应用支持的数值。",
    L"启用适应屏幕", L"将此 INI 改为按百分比计算宽度。Tarnished HUD 版本 4 立即生效；旧版本需要重启游戏。",
    L"编辑此模组的 INI。若其 DLL 支持重载接口，保存后立即生效；否则需要重启游戏。",
    L"保存更改", L"备份并保存此模组的 INI，然后请求其 DLL 安全地实时重载。",
    L"未找到可配置的模组", L"DLL 旁带有 INI 的模组会在重启后显示在这里。"
};

// Follows the game language unless [General] Language in NativeTweaks.ini
// forces one. Traditional Chinese is converted at runtime from the Simplified texts.
enum class Language { english, spanish, chinese, chinese_traditional };
constexpr Language kLanguages[] = {
    Language::english, Language::spanish, Language::chinese, Language::chinese_traditional};
// Written by the startup worker and the game thread (language selector), read
// by save workers when they show alerts.
std::atomic<Language> g_language{Language::english};
Language g_game_language{Language::english};  // what "auto" resolves to
// In-game selector index: 0 auto, 1 English, 2 Spanish, 3 Simplified, 4 Traditional.
std::uint8_t g_language_choice{};
constexpr const wchar_t* kLanguageSettings[] = {L"auto", L"en", L"es", L"zh", L"zht"};
std::string g_steam_language{};  // host's Steam identifier, e.g. "latam"

std::uint8_t language_choice_from_setting(const wchar_t* setting) noexcept {
    if (_wcsicmp(setting, L"en") == 0) return 1;
    if (_wcsicmp(setting, L"es") == 0) return 2;
    if (_wcsicmp(setting, L"zh") == 0 || _wcsicmp(setting, L"zh-cn") == 0) return 3;
    if (_wcsicmp(setting, L"zht") == 0 || _wcsicmp(setting, L"zh-tw") == 0) return 4;
    return 0;
}

Language resolve_language_choice(std::uint8_t choice) noexcept {
    switch (choice) {
    case 1: return Language::english;
    case 2: return Language::spanish;
    case 3: return Language::chinese;
    case 4: return Language::chinese_traditional;
    default: return g_game_language;
    }
}

// ---- Translation files ------------------------------------------------------
// DLLs\NativeTweaks\lang\<en|es|zh|zht>.txt overrides menu texts. Each line is
// "<English text> = <translation>"; lines starting with ; or # are comments.

bool read_file_bytes(const std::wstring& path, std::string& contents) noexcept;
bool utf8_to_wide(std::string_view value, std::wstring& output) noexcept;
void log_message(const wchar_t* message) noexcept;

// Optional features, off unless [General] <key> = 1 in NativeTweaks.ini.
bool feature_enabled(const wchar_t* key) noexcept {
    return !g_config_path.empty() &&
        GetPrivateProfileIntW(L"General", key, 0, g_config_path.c_str()) != 0;
}

using TextOverrides = std::unordered_map<std::wstring, std::wstring>;

std::array<TextOverrides, 4> load_text_overrides() noexcept {
    std::array<TextOverrides, 4> result;
    if (!feature_enabled(L"TranslationFiles")) return result;
    try {
        const std::size_t slash = g_config_path.find_last_of(L'\\');
        if (slash == std::wstring::npos) return result;
        static constexpr const wchar_t* kFiles[] = {L"en", L"es", L"zh", L"zht"};
        for (std::size_t index = 0; index < std::size(kFiles); ++index) {
            const std::wstring path = g_config_path.substr(0, slash + 1) +
                L"NativeTweaks\\lang\\" + kFiles[index] + L".txt";
            if (GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES) continue;
            std::string bytes;
            std::wstring text;
            if (!read_file_bytes(path, bytes) || !utf8_to_wide(bytes, text)) {
                log_message((L"Skipped translation file (unreadable or not UTF-8): " + path).c_str());
                continue;
            }
            if (!text.empty() && text.front() == L'\xFEFF') text.erase(0, 1);
            std::size_t start = 0;
            while (start < text.size()) {
                std::size_t end = text.find(L'\n', start);
                if (end == std::wstring::npos) end = text.size();
                std::wstring line = text.substr(start, end - start);
                if (!line.empty() && line.back() == L'\r') line.pop_back();
                start = end + 1;
                if (line.empty() || line.front() == L';' || line.front() == L'#') continue;
                const std::size_t separator = line.find(L" = ");
                if (separator == std::wstring::npos || separator == 0) continue;
                std::wstring value = line.substr(separator + 3);
                if (value.empty() || value.size() > 1024) continue;
                std::size_t escape = 0;  // "\n" in a translation becomes a line break
                while ((escape = value.find(L"\\n", escape)) != std::wstring::npos)
                    value.replace(escape, 2, L"\n");
                result[index][line.substr(0, separator)] = std::move(value);
            }
            log_message((L"Translation file loaded: " + std::wstring(kFiles[index]) + L".txt (" +
                std::to_wstring(result[index].size()) + L" texts).").c_str());
        }
    } catch (...) {
        log_message(L"Loading translation files failed; built-in texts are used.");
    }
    return result;
}

const TextOverrides& text_overrides(Language language) noexcept {
    static const std::array<TextOverrides, 4> overrides = load_text_overrides();
    return overrides[static_cast<std::size_t>(language) % overrides.size()];
}

const wchar_t* translated(Language language, const wchar_t* english,
    const wchar_t* fallback) noexcept {
    const TextOverrides& overrides = text_overrides(language);
    if (overrides.empty() || !english) return fallback;
    try {
        const auto found = overrides.find(english);
        return found == overrides.end() ? fallback : found->second.c_str();
    } catch (...) {
        return fallback;
    }
}

const wchar_t* tr_in(Language language, const wchar_t* en, const wchar_t* es,
    const wchar_t* zh) noexcept {
    switch (language) {
    case Language::spanish: return translated(language, en, es);
    case Language::chinese: return translated(language, en, zh);
    case Language::chinese_traditional: return translated(language, en, zht::convert(zh));
    default: return translated(language, en, en);
    }
}

const wchar_t* tr(const wchar_t* en, const wchar_t* es, const wchar_t* zh) noexcept {
    return tr_in(g_language, en, es, zh);
}

const char* language_code(Language language) noexcept {
    switch (language) {
    case Language::spanish: return "es";
    case Language::chinese: return "zh";
    case Language::chinese_traditional: return "zht";
    default: return "en";
    }
}

Language language_from_code(std::string_view code) noexcept {
    if (code == "es") return Language::spanish;
    if (code == "zh") return Language::chinese;
    if (code == "zht") return Language::chinese_traditional;
    return Language::english;
}

// The host's own texts (tab title and help, Previous/Next Page) follow the
// menu language with a fork that has ERNativeUIFork_SetHostLanguage (r128).
// Auto goes back to the Steam language the host started with.
void sync_host_language(const wchar_t* reason) noexcept {
    using SetHostLanguageFn = ERUI_Result (ERUI_CALL*)(const char*);
    const HMODULE host = GetModuleHandleW(L"ERNativeUI.dll");
    const auto set_host_language = host ? reinterpret_cast<SetHostLanguageFn>(
        GetProcAddress(host, "ERNativeUIFork_SetHostLanguage")) : nullptr;
    if (!set_host_language) return;
    const char* identifier{};
    if (g_language_choice == 0 && !g_steam_language.empty()) {
        identifier = g_steam_language.c_str();
    } else {
        switch (g_language.load()) {
        case Language::spanish: identifier = "spanish"; break;
        case Language::chinese: identifier = "schinese"; break;
        case Language::chinese_traditional: identifier = "tchinese"; break;
        default: identifier = "english"; break;
        }
    }
    const ERUI_Result result = set_host_language(identifier);
    wchar_t message[160]{};
    swprintf_s(message, L"Host language set to %hs (%ls): result %d.",
        identifier, reason, static_cast<int>(result));
    log_message(message);
}

const Texts& base_texts_for(Language language);

// Built-in table with the translation file's overrides, matched by English text.
Texts overridden_table(Language language) noexcept {
    Texts result = base_texts_for(language);
    auto** fields = reinterpret_cast<const wchar_t**>(&result);
    const auto* english = reinterpret_cast<const wchar_t* const*>(&kEnglish);
    for (std::size_t i = 0; i < sizeof(Texts) / sizeof(const wchar_t*); ++i)
        fields[i] = translated(language, english[i], fields[i]);
    return result;
}

const Texts& texts_for(Language language) {
    static const std::array<Texts, 4> tables{
        overridden_table(Language::english), overridden_table(Language::spanish),
        overridden_table(Language::chinese), overridden_table(Language::chinese_traditional)};
    return tables[static_cast<std::size_t>(language) % tables.size()];
}

const Texts& base_texts_for(Language language) {
    switch (language) {
    case Language::spanish: return kSpanish;
    case Language::chinese: return kChinese;
    case Language::chinese_traditional: {
        static const Texts traditional = zht::convert_table(kChinese);
        return traditional;
    }
    default: return kEnglish;
    }
}

// Evaluates language-dependent code (tr, template_text) in another language.
// Only used while the menu is built or relabelled, on one thread at a time.
class LanguageScope {
public:
    explicit LanguageScope(Language language) noexcept : saved_(g_language) {
        g_language = language;
    }
    ~LanguageScope() { g_language = saved_; }
    LanguageScope(const LanguageScope&) = delete;
    LanguageScope& operator=(const LanguageScope&) = delete;

private:
    Language saved_;
};

using TextField = const wchar_t* Texts::*;

void track_texts(erui::RowHandle row, TextField label, TextField help) {
    live_text::track_row(row, [label, help](const char* code) {
        const Texts& t = texts_for(language_from_code(code));
        return live_text::Texts{t.*label, t.*help, {}, {}};
    });
}

void track_submenu_texts(const erui::Page& page, TextField label, TextField help,
    TextField title, TextField page_help) {
    live_text::track_submenu(page.handle(), [=](const char* code) {
        const Texts& t = texts_for(language_from_code(code));
        return live_text::Texts{t.*label, t.*help, t.*title, t.*page_help};
    });
}

// The log file is written by its own thread. A caller, often a hook on the
// game thread, only formats the line and appends it to a buffer: opening and
// writing the file goes through MO2's virtual file system and must never
// hold up the game (stutter reports, r129).
constexpr std::size_t kLogBufferLimit = std::size_t{1} << 20;
std::string g_log_pending{};  // UTF-8 lines not written yet
std::size_t g_log_dropped{};
HANDLE g_log_event{};

HANDLE open_log_file() noexcept {
    return CreateFileW(g_log_path.c_str(), FILE_APPEND_DATA,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
}

DWORD WINAPI log_writer(void*) noexcept {
    HANDLE file = open_log_file();
    std::string batch;
    for (;;) {
        WaitForSingleObject(g_log_event, INFINITE);
        AcquireSRWLockExclusive(&g_log_lock);
        batch.swap(g_log_pending);
        const std::size_t dropped = g_log_dropped;
        g_log_dropped = 0;
        ReleaseSRWLockExclusive(&g_log_lock);
        if (dropped) {
            char note[96]{};
            const int length = std::snprintf(note, sizeof(note),
                "[NativeTweaks] %zu log line(s) dropped (log buffer full).\n", dropped);
            if (length > 0) batch.append(note, static_cast<std::size_t>(length));
        }
        if (file == INVALID_HANDLE_VALUE) file = open_log_file();
        if (file != INVALID_HANDLE_VALUE && !batch.empty() && batch.size() <= MAXDWORD) {
            DWORD written{};
            (void)WriteFile(file, batch.data(), static_cast<DWORD>(batch.size()), &written, nullptr);
        }
        batch.clear();
    }
}

void log_message(const wchar_t* message) noexcept {
    if (!message) return;
    try {
        std::wstring line(L"[NativeTweaks] ");
        line.append(message);
        line.push_back(L'\n');
        OutputDebugStringW(line.c_str());

        if (!g_log_enabled || !g_log_event) return;
        SYSTEMTIME now{};
        GetLocalTime(&now);
        wchar_t timestamp[48]{};
        const int timestamp_length = swprintf_s(
            timestamp, std::size(timestamp),
            L"%04u-%02u-%02u %02u:%02u:%02u.%03u ",
            now.wYear, now.wMonth, now.wDay,
            now.wHour, now.wMinute, now.wSecond, now.wMilliseconds);
        if (timestamp_length <= 0) return;
        line.insert(0, timestamp, static_cast<std::size_t>(timestamp_length));
        if (line.size() > static_cast<std::size_t>(INT_MAX)) return;
        const int required = WideCharToMultiByte(
            CP_UTF8, 0, line.data(), static_cast<int>(line.size()),
            nullptr, 0, nullptr, nullptr);
        if (required <= 0) return;
        std::string utf8(static_cast<std::size_t>(required), '\0');
        if (WideCharToMultiByte(
                CP_UTF8, 0, line.data(), static_cast<int>(line.size()),
                utf8.data(), required, nullptr, nullptr) != required) return;

        AcquireSRWLockExclusive(&g_log_lock);
        if (g_log_pending.size() + utf8.size() <= kLogBufferLimit) {
            g_log_pending.append(utf8);
        } else {
            ++g_log_dropped;
        }
        ReleaseSRWLockExclusive(&g_log_lock);
        SetEvent(g_log_event);
    } catch (...) {
        // Diagnostics must never interfere with initialization or menu input.
    }
}

void initialize_logging() noexcept {
    try {
        wchar_t module_path[32768]{};
        const DWORD length = GetModuleFileNameW(
            g_own_module, module_path, static_cast<DWORD>(std::size(module_path)));
        if (length == 0 || length >= std::size(module_path)) return;

        wchar_t* slash = wcsrchr(module_path, L'\\');
        if (!slash) return;
        std::wstring directory(module_path, slash + 1);
        g_config_path = directory + L"NativeTweaks.ini";
        // Keep settings saved by builds released as Mod Forge (ERModSettingsHub.ini).
        if (GetFileAttributesW(g_config_path.c_str()) == INVALID_FILE_ATTRIBUTES)
            CopyFileW((directory + L"ERModSettingsHub.ini").c_str(), g_config_path.c_str(), TRUE);
        // [Diagnostics] PerfLog needs the log, so it turns it on as well.
        if (GetPrivateProfileIntW(
                L"Logging", L"EnableLog", 0, g_config_path.c_str()) == 0 &&
            GetPrivateProfileIntW(
                L"Diagnostics", L"PerfLog", 0, g_config_path.c_str()) == 0) return;

        g_log_path = directory + L"NativeTweaks.log";
        g_log_event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (!g_log_event) return;
        HANDLE writer = CreateThread(nullptr, 0, &log_writer, nullptr, 0, nullptr);
        if (!writer) {
            CloseHandle(g_log_event);
            g_log_event = nullptr;
            return;
        }
        CloseHandle(writer);
        g_log_enabled = true;
        log_message(L"Diagnostic logging enabled; startup worker running.");
    } catch (...) {
        OutputDebugStringW(L"[NativeTweaks] Could not initialize file logging.\n");
    }
}

bool snapshot_loaded_mod_modules(std::vector<std::wstring>& paths);

void wait_for_startup_discovery_window() noexcept {
    const int maximum_configured = g_config_path.empty() ? 10000 :
        GetPrivateProfileIntW(L"Discovery", L"StartupWaitMs", 10000,
            g_config_path.c_str());
    const int minimum_configured = g_config_path.empty() ? 2500 :
        GetPrivateProfileIntW(L"Discovery", L"MinimumWaitMs", 2500,
            g_config_path.c_str());
    const int quiet_configured = g_config_path.empty() ? 2500 :
        GetPrivateProfileIntW(L"Discovery", L"StableModuleMs", 2500,
            g_config_path.c_str());
    const DWORD maximum_ms = static_cast<DWORD>(
        std::clamp(maximum_configured, 0, 10000));
    if (maximum_ms == 0) return;
    const DWORD minimum_ms = static_cast<DWORD>(
        std::clamp(minimum_configured, 0, static_cast<int>(maximum_ms)));
    const DWORD quiet_ms = static_cast<DWORD>(
        std::clamp(quiet_configured, 250, 4000));

    const ULONGLONG started_at = GetTickCount64();
    ULONGLONG last_change_at = started_at;
    std::vector<std::wstring> previous_modules;
    bool have_snapshot = snapshot_loaded_mod_modules(previous_modules);

    while (GetTickCount64() - started_at < maximum_ms) {
        Sleep(200);
        std::vector<std::wstring> current_modules;
        if (snapshot_loaded_mod_modules(current_modules) &&
            (!have_snapshot || current_modules != previous_modules)) {
            previous_modules = std::move(current_modules);
            have_snapshot = true;
            last_change_at = GetTickCount64();
        }

        const ULONGLONG now = GetTickCount64();
        if (now - started_at >= minimum_ms &&
            now - last_change_at >= quiet_ms) {
            break;
        }
    }

    wchar_t message[192]{};
    swprintf_s(message, std::size(message),
        L"Startup discovery settled after %llu ms; loaded mod DLLs=%zu, snapshot=%ls.",
        static_cast<unsigned long long>(GetTickCount64() - started_at),
        previous_modules.size(), have_snapshot ? L"ready" : L"unavailable");
    log_message(message);
}

std::size_t language_index(Language language) noexcept {
    return static_cast<std::size_t>(language);
}

std::wstring pick(const std::array<std::wstring, 4>& texts, Language language,
    std::wstring_view fallback) {
    const std::wstring& text = texts[language_index(language)];
    return text.empty() ? std::wstring(fallback) : text;
}

std::wstring mod_help_for(const GenericIniMod& mod, Language language) {
    return pick(mod.help_by_language, language, texts_for(language).generic_settings_help);
}

void add_generic_setting_row(erui::Page& page, GenericIniSetting& setting) {
    const std::wstring_view help = setting.help.empty()
        ? std::wstring_view(g_texts->generic_settings_help) : std::wstring_view(setting.help);
    erui::RowHandle row{ERUI_INVALID_ROW};
    switch (setting.control) {
    case GenericControl::toggle:
        row = page.add_toggle(setting.label, help, setting.initial_index,
            &generic_value_changed, &setting);
        break;
    case GenericControl::slider: {
        erui::SliderOptions slider{};
        slider.minimum = setting.slider_minimum;
        slider.maximum = setting.slider_maximum;
        slider.step = setting.slider_step;
        slider.initial_value = setting.initial_index;
        row = page.add_slider(setting.label, help, slider, &generic_value_changed, &setting);
        break;
    }
    case GenericControl::text:
        return;  // never registered: ERNativeUI does not draw text inputs
    case GenericControl::choice: {
        std::vector<std::wstring_view> values(
            setting.choice_labels.begin(), setting.choice_labels.end());
        erui::ChoiceOptions choice{};
        choice.values = values.data();
        choice.count = values.size();
        choice.initial_index = setting.initial_index;
        if (values.size() <= 8) {
            row = page.add_inline_choice(setting.label, help, choice, &generic_value_changed, &setting);
        } else {
            row = page.add_popup_choice(setting.label, help, choice, &generic_value_changed, &setting);
        }
        break;
    }
    }
    // Choice values keep the registration language (the fork has no API for them).
    const GenericIniSetting* const source = &setting;
    live_text::track_row(row, [source](const char* code) {
        const Language language = language_from_code(code);
        return live_text::Texts{pick(source->label_by_language, language, source->label),
            pick(source->help_by_language, language, texts_for(language).generic_settings_help),
            {}, {}};
    });
}

// ---- Export / import --------------------------------------------------------
// Export copies the mod's INI to DLLs\NativeTweaks\profiles\<file name>;
// import copies it back (keeping <ini>.bak) and applies after a restart.

void show_alert(std::wstring_view message) noexcept;
bool read_file_bytes(const std::wstring& path, std::string& contents) noexcept;
bool utf8_to_wide(std::string_view value, std::wstring& output) noexcept;

std::wstring profile_path_for(const GenericIniMod& mod) {
    const std::size_t config_slash = g_config_path.find_last_of(L'\\');
    const std::size_t ini_slash = mod.path.find_last_of(L"\\/");
    if (config_slash == std::wstring::npos || ini_slash == std::wstring::npos) return {};
    return g_config_path.substr(0, config_slash + 1) + L"NativeTweaks\\profiles\\" +
        mod.path.substr(ini_slash + 1);
}

live_text::Texts profile_row_texts(Language language, bool import) {
    return import
        ? live_text::Texts{tr_in(language, L"Import settings", L"Importar ajustes", L"导入设置"),
            tr_in(language,
                L"Replace this mod's INI with the copy in DLLs\\NativeTweaks\\profiles (a .bak backup is kept). Applies after restarting the game.",
                L"Sustituye el INI de este mod por la copia de DLLs\\NativeTweaks\\profiles (se guarda una copia .bak). Se aplica al reiniciar el juego.",
                L"用 DLLs\\NativeTweaks\\profiles 中的副本替换此模组的 INI（保留 .bak 备份）。重启游戏后生效。"),
            {}, {}}
        : live_text::Texts{tr_in(language, L"Export settings", L"Exportar ajustes", L"导出设置"),
            tr_in(language,
                L"Copy this mod's saved INI to DLLs\\NativeTweaks\\profiles to share it or restore it later.",
                L"Copia el INI guardado de este mod a DLLs\\NativeTweaks\\profiles para compartirlo o restaurarlo después.",
                L"将此模组已保存的 INI 复制到 DLLs\\NativeTweaks\\profiles，以便分享或日后恢复。"),
            {}, {}};
}

void ERUI_CALL export_profile_clicked(void* target) noexcept {
    if (!target) return;
    try {
        const auto& mod = *static_cast<const GenericIniMod*>(target);
        const std::wstring profile = profile_path_for(mod);
        const std::size_t slash = profile.find_last_of(L'\\');
        const std::wstring folder = slash == std::wstring::npos ? L"" : profile.substr(0, slash);
        const std::wstring parent = folder.substr(0, folder.find_last_of(L'\\'));
        if (!profile.empty()) {
            CreateDirectoryW(parent.c_str(), nullptr);
            CreateDirectoryW(folder.c_str(), nullptr);
        }
        if (profile.empty() || !CopyFileW(mod.path.c_str(), profile.c_str(), FALSE)) {
            log_message(L"Could not export a mod INI to the profiles folder.");
            show_alert(tr(L"Could not export the settings.", L"No se pudieron exportar los ajustes.",
                L"无法导出设置。"));
            return;
        }
        log_message((L"Exported settings to " + profile).c_str());
        show_alert(tr(L"Settings exported to DLLs\\NativeTweaks\\profiles.",
            L"Ajustes exportados a DLLs\\NativeTweaks\\profiles.",
            L"设置已导出到 DLLs\\NativeTweaks\\profiles。"));
    } catch (...) {
        log_message(L"Exporting a mod INI failed.");
    }
}

void ERUI_CALL import_profile_clicked(void* target) noexcept {
    if (!target) return;
    try {
        const auto& mod = *static_cast<const GenericIniMod*>(target);
        const std::wstring profile = profile_path_for(mod);
        std::string contents;
        std::wstring checked;
        if (profile.empty() || GetFileAttributesW(profile.c_str()) == INVALID_FILE_ATTRIBUTES) {
            show_alert(tr(L"No exported settings were found for this mod.",
                L"No hay ajustes exportados para este mod.", L"未找到此模组已导出的设置。"));
            return;
        }
        if (!read_file_bytes(profile, contents) || !utf8_to_wide(contents, checked)) {
            log_message(L"Import skipped: the exported INI is unreadable or not UTF-8.");
            show_alert(tr(L"The exported file could not be read.",
                L"No se pudo leer el archivo exportado.", L"无法读取导出的文件。"));
            return;
        }
        (void)CopyFileW(mod.path.c_str(), (mod.path + L".bak").c_str(), FALSE);
        if (!CopyFileW(profile.c_str(), mod.path.c_str(), FALSE)) {
            log_message(L"Could not import an exported INI; the original was preserved.");
            show_alert(tr(L"Could not import the settings. The original file was preserved.",
                L"No se pudieron importar los ajustes. El archivo original se ha conservado.",
                L"无法导入设置。原文件已保留。"));
            return;
        }
        log_message((L"Imported settings from " + profile).c_str());
        show_alert(tr(L"Settings imported. Restart the game to apply them.",
            L"Ajustes importados. Reinicia el juego para aplicarlos.",
            L"设置已导入。请重启游戏以应用。"));
    } catch (...) {
        log_message(L"Importing a mod INI failed.");
    }
}

void add_profile_buttons(erui::Page& page, GenericIniMod& mod) {
    for (const bool import : {false, true}) {
        const live_text::Texts texts = profile_row_texts(g_language, import);
        const erui::RowHandle row = import
            ? page.add_button(texts.label, texts.help, &import_profile_clicked, &mod)
            : page.add_button(texts.label, texts.help, &export_profile_clicked, &mod);
        live_text::track_row(row, [import](const char* code) {
            return profile_row_texts(language_from_code(code), import);
        });
    }
}

void add_generic_ini_pages(erui::Page& mods) {
    for (auto& mod : g_generic_mods) {
        const std::wstring_view mod_help = mod.help.empty()
            ? std::wstring_view(g_texts->generic_settings_help) : std::wstring_view(mod.help);
        auto page = mods.add_submenu(mod.display_name,
            mod_help, g_texts->provider, mod.display_name);
        const GenericIniMod* const source = &mod;
        live_text::track_submenu(page.handle(), [source](const char* code) {
            const Language language = language_from_code(code);
            const std::wstring name = pick(source->name_by_language, language, source->display_name);
            return live_text::Texts{name, mod_help_for(*source, language),
                texts_for(language).provider, name};
        });
        // Uncategorized rows first, then one submenu per Category= in order of
        // first appearance.
        std::vector<std::wstring> categories;
        for (auto& setting : mod.settings) {
            if (setting.category.empty()) {
                add_generic_setting_row(page, setting);
            } else if (std::find(categories.begin(), categories.end(), setting.category) ==
                categories.end()) {
                categories.push_back(setting.category);
            }
        }
        for (const auto& category : categories) {
            auto sub = page.add_submenu(category, mod_help, mod.display_name, category);
            const GenericIniSetting* first{};
            for (auto& setting : mod.settings) {
                if (setting.category != category) continue;
                if (!first) first = &setting;
                add_generic_setting_row(sub, setting);
            }
            if (!first) continue;
            live_text::track_submenu(sub.handle(), [source, first](const char* code) {
                const Language language = language_from_code(code);
                const std::wstring name = pick(source->name_by_language, language, source->display_name);
                const std::wstring title = pick(first->category_by_language, language, first->category);
                return live_text::Texts{title, mod_help_for(*source, language), name, title};
            });
        }
        const auto save = page.add_button(g_texts->generic_save, g_texts->generic_save_help,
            &generic_save_clicked, &mod);
        track_texts(save, &Texts::generic_save, &Texts::generic_save_help);
        if (feature_enabled(L"ProfileButtons")) add_profile_buttons(page, mod);
    }
}

void show_alert(std::wstring_view message) noexcept {
    if (!g_registration.valid()) return;
    erui::AlertOptions options{};
    options.buttons = erui::AlertButtons::ok;
    options.placement = erui::AlertPlacement::bottom;
    (void)g_registration.alert(message, options);
}

live_text::Texts language_row_texts(Language language, bool live) {
    const auto tr = [language](const wchar_t* en, const wchar_t* es, const wchar_t* zh) {
        return tr_in(language, en, es, zh);
    };
    return {
        tr(L"Language", L"Idioma", L"语言"),
        live ? tr(L"Menu language. Auto follows the game language. Applies immediately; reopen a page to refresh it. Chinese needs the game in Chinese to display.",
                  L"Idioma del menú. Auto sigue el idioma del juego. Se aplica al momento; vuelve a abrir una página para refrescarla. El chino necesita el juego en chino para verse.",
                  L"菜单语言。自动表示跟随游戏语言。立即生效；重新打开页面即可刷新。中文需要游戏本身设为中文才能正常显示。")
             : tr(L"Menu language. Auto follows the game language. Applies after restarting the game. Chinese needs the game in Chinese to display.",
                  L"Idioma del menú. Auto sigue el idioma del juego. Se aplica al reiniciar el juego. El chino necesita el juego en chino para verse.",
                  L"菜单语言。自动表示跟随游戏语言。重启游戏后生效。中文需要游戏本身设为中文才能正常显示。"),
        {}, {}};
}

// Language selector: saves [General] Language and, with the ERNativeUI fork,
// relabels every tracked row at once.
void ERUI_CALL language_changed(void*, std::uint8_t value) noexcept {
    if (value >= std::size(kLanguageSettings)) return;
    g_language_choice = value;
    if (!g_config_path.empty()) {
        WritePrivateProfileStringW(L"General", L"Language", kLanguageSettings[value],
            g_config_path.c_str());
    }
    const Language next = resolve_language_choice(value);
    const bool same = next == g_language;
    g_language = next;
    // Before the early return: es -> auto can still move the host from
    // "spanish" back to Steam's "latam".
    sync_host_language(L"menu language changed");
    if (same) return;
    try {
        g_language = next;
        g_texts = &texts_for(next);
        if (!live_text::supported()) {
            log_message(L"Language saved; this ERNativeUI.dll has no live text API, so it applies after a restart.");
            show_alert(tr(L"Language saved. Restart the game to apply it.",
                L"Idioma guardado. Reinicia el juego para aplicarlo.",
                L"语言已保存。请重启游戏以应用。"));
            return;
        }
        const std::size_t failures = live_text::apply(
            g_registration.provider_handle(), language_code(next));
        wchar_t message[160]{};
        swprintf_s(message, L"Menu language switched live to %hs; rows not updated: %zu.",
            language_code(next), failures);
        log_message(message);
    } catch (...) {
        log_message(L"Language switch failed; the new language applies after a restart.");
    }
}

void add_language_row(erui::Page& page) {
    static constexpr std::wstring_view kNames[] = {
        L"Auto", L"English", L"Español", L"简体中文", L"繁體中文"};
    const bool live = live_text::supported();
    const live_text::Texts texts = language_row_texts(g_language, live);
    erui::ChoiceOptions choice{};
    choice.values = kNames;
    choice.count = std::size(kNames);
    choice.initial_index = g_language_choice;
    const erui::RowHandle row = page.add_inline_choice(
        texts.label, texts.help, choice, &language_changed, nullptr);
    live_text::track_row(row, [live](const char* code) {
        return language_row_texts(language_from_code(code), live);
    });
}

bool resolve_tarnished_hud_ini() {
    HMODULE hud_module = GetModuleHandleW(L"TarnishedHUD.dll");
    if (!hud_module) {
        log_message(L"TarnishedHUD.dll was not loaded when the Hub initialized (not installed, not in the "
            L"loader's list, or it did not load on this game version); its page is not shown.");
        return false;
    }

    wchar_t module_path[32768]{};
    const DWORD length = GetModuleFileNameW(
        hud_module, module_path, static_cast<DWORD>(std::size(module_path)));
    if (length == 0 || length >= std::size(module_path)) {
        log_message(L"Could not resolve the loaded TarnishedHUD.dll path.");
        return false;
    }

    wchar_t* slash = wcsrchr(module_path, L'\\');
    if (!slash) {
        log_message(L"The loaded TarnishedHUD.dll path has no parent directory.");
        return false;
    }
    *(slash + 1) = L'\0';

    g_ini_path.assign(module_path);
    g_ini_path.append(L"TarnishedHUD.ini");
    const DWORD attributes = GetFileAttributesW(g_ini_path.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES ||
        (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
        log_message(L"TarnishedHUD.ini was not found beside TarnishedHUD.dll.");
        return false;
    }
    log_message(L"Found TarnishedHUD.dll and its adjacent TarnishedHUD.ini.");
    return true;
}

bool read_ini_number(
    const wchar_t* section,
    const wchar_t* key,
    const wchar_t* fallback,
    double& value) noexcept {
    wchar_t text[128]{};
    const DWORD count = GetPrivateProfileStringW(
        section, key, fallback, text, static_cast<DWORD>(std::size(text)),
        g_ini_path.c_str());
    if (count == 0 || count >= std::size(text) - 1) return false;

    wchar_t* end{};
    const double parsed = wcstod(text, &end);
    while (*end == L' ' || *end == L'\t') ++end;
    if (end == text || *end != L'\0' || !std::isfinite(parsed)) return false;
    value = parsed;
    return true;
}

bool read_ini_bool(
    const wchar_t* section,
    const wchar_t* key,
    const wchar_t* fallback,
    std::uint8_t& value) noexcept {
    double parsed{};
    if (!read_ini_number(section, key, fallback, parsed) ||
        (parsed != 0.0 && parsed != 1.0)) return false;
    value = parsed == 1.0 ? 1 : 0;
    return true;
}

bool read_tarnished_hud_settings(Settings& settings) noexcept {
    if (!read_ini_bool(L"General", L"Enabled", L"0", settings.general_enabled) ||
        !read_ini_bool(L"BarSize", L"Enabled", L"0", settings.bar_size_enabled) ||
        !read_ini_bool(L"BarSize", L"FitScreen", L"0", settings.fit_screen) ||
        !read_ini_bool(L"BarSize", L"HPEnabled", L"1", settings.hp_enabled) ||
        !read_ini_bool(L"BarSize", L"FPEnabled", L"1", settings.fp_enabled) ||
        !read_ini_bool(L"BarSize", L"StaminaEnabled", L"1", settings.stamina_enabled)) {
        return false;
    }

    const wchar_t* keys[] = {L"HPWidth", L"FPWidth", L"StaminaWidth"};
    std::uint8_t* output[] = {
        &settings.hp_percent, &settings.fp_percent, &settings.stamina_percent};
    g_fit_screen_values_safe = true;
    for (std::size_t index = 0; index < std::size(keys); ++index) {
        double width{};
        if (!read_ini_number(L"BarSize", keys[index], L"1.00", width) ||
            width < (settings.fit_screen ? 0.01 : 0.1) ||
            width > (settings.fit_screen ? 1.00 : 10.0)) return false;
        if (settings.fit_screen) {
            *output[index] = static_cast<std::uint8_t>(
                std::clamp(std::lround(width * 100.0), 1L, 100L));
        } else {
            *output[index] = 100;
            if (width > 1.0) g_fit_screen_values_safe = false;
        }
    }
    return true;
}

std::string trim_ascii(std::string_view value) {
    std::size_t first = 0;
    std::size_t last = value.size();
    while (first < last && (value[first] == ' ' || value[first] == '\t')) ++first;
    while (last > first && (value[last - 1] == ' ' || value[last - 1] == '\t')) --last;
    return std::string(value.substr(first, last - first));
}

bool ascii_equal(std::string_view left, std::string_view right) noexcept {
    if (left.size() != right.size()) return false;
    for (std::size_t index = 0; index < left.size(); ++index) {
        char a = left[index];
        char b = right[index];
        if (a >= 'A' && a <= 'Z') a = static_cast<char>(a - 'A' + 'a');
        if (b >= 'A' && b <= 'Z') b = static_cast<char>(b - 'A' + 'a');
        if (a != b) return false;
    }
    return true;
}

bool replace_ini_value(
    std::string& contents,
    std::string_view section,
    std::string_view key,
    std::string_view replacement) {
    std::size_t line_start = 0;
    std::size_t match_start = 0;
    std::size_t match_end = 0;
    std::size_t equals = 0;
    std::size_t matches = 0;
    bool in_section = false;

    while (line_start < contents.size()) {
        const std::size_t newline = contents.find('\n', line_start);
        const std::size_t line_end = newline == std::string::npos
            ? contents.size() : newline;
        std::size_t text_end = line_end;
        if (text_end > line_start && contents[text_end - 1] == '\r') --text_end;
        std::string_view line(contents.data() + line_start, text_end - line_start);
        if (line_start == 0 && line.size() >= 3 &&
            static_cast<unsigned char>(line[0]) == 0xEF &&
            static_cast<unsigned char>(line[1]) == 0xBB &&
            static_cast<unsigned char>(line[2]) == 0xBF) {
            line.remove_prefix(3);
        }
        const std::string trimmed = trim_ascii(line);

        if (trimmed.size() >= 2 && trimmed.front() == '[' && trimmed.back() == ']') {
            in_section = ascii_equal(
                trim_ascii(std::string_view(trimmed).substr(1, trimmed.size() - 2)),
                section);
        } else if (in_section) {
            const std::size_t separator = line.find('=');
            if (separator != std::string_view::npos &&
                ascii_equal(trim_ascii(line.substr(0, separator)), key)) {
                ++matches;
                match_start = line_start;
                match_end = text_end;
                equals = separator;
            }
        }

        if (newline == std::string::npos) break;
        line_start = newline + 1;
    }

    if (matches != 1) return false;

    const std::string_view line(contents.data() + match_start, match_end - match_start);
    const std::string_view rest = line.substr(equals + 1);
    std::size_t comment = std::string_view::npos;
    for (std::size_t index = 0; index < rest.size(); ++index) {
        if ((rest[index] == ';' || rest[index] == '#') && index > 0 &&
            (rest[index - 1] == ' ' || rest[index - 1] == '\t')) {
            comment = index;
            break;
        }
    }
    const std::size_t value_end = comment == std::string_view::npos
        ? rest.size() : comment;
    const std::string_view old_value = rest.substr(0, value_end);
    std::size_t leading = 0;
    std::size_t trailing = old_value.size();
    while (leading < trailing && (old_value[leading] == ' ' || old_value[leading] == '\t')) ++leading;
    while (trailing > leading && (old_value[trailing - 1] == ' ' || old_value[trailing - 1] == '\t')) --trailing;

    std::string updated;
    updated.reserve(line.size() + replacement.size());
    updated.append(line.substr(0, equals + 1));
    updated.append(old_value.substr(0, leading));
    updated.append(replacement);
    updated.append(old_value.substr(trailing));
    if (comment != std::string_view::npos) updated.append(rest.substr(comment));
    contents.replace(match_start, match_end - match_start, updated);
    return true;
}

bool read_file_bytes(const std::wstring& path, std::string& contents) noexcept {
    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;

    LARGE_INTEGER size{};
    const bool size_ok = GetFileSizeEx(file, &size) && size.QuadPart >= 0 &&
        size.QuadPart <= 1024 * 1024;
    if (!size_ok) {
        CloseHandle(file);
        return false;
    }

    try {
        contents.assign(static_cast<std::size_t>(size.QuadPart), '\0');
    } catch (...) {
        CloseHandle(file);
        return false;
    }

    DWORD read = 0;
    const DWORD requested = static_cast<DWORD>(contents.size());
    const bool read_ok = requested == 0 ||
        (ReadFile(file, contents.data(), requested, &read, nullptr) && read == requested);
    CloseHandle(file);
    if (!read_ok || contents.find('\0') != std::string::npos) return false;
    return true;
}

bool write_file_atomically(
    const std::wstring& path, const std::string& contents) {
    const std::wstring backup_path = path + L".ermodsettings.bak";
    if (!CopyFileW(path.c_str(), backup_path.c_str(), FALSE)) return false;

    const std::wstring temp_path = path + L".ermodsettings." +
        std::to_wstring(GetCurrentProcessId()) + L".tmp";
    HANDLE file = CreateFileW(temp_path.c_str(), GENERIC_WRITE, 0, nullptr,
        CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;

    DWORD written = 0;
    const DWORD requested = static_cast<DWORD>(contents.size());
    const bool write_ok = requested == 0 ||
        (WriteFile(file, contents.data(), requested, &written, nullptr) && written == requested);
    const bool flush_ok = write_ok && FlushFileBuffers(file);
    CloseHandle(file);
    if (!flush_ok || !MoveFileExW(temp_path.c_str(), path.c_str(),
        MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        DeleteFileW(temp_path.c_str());
        return false;
    }
    return true;
}

struct IniEdit {
    std::string section;
    std::string key;
    std::string value;
};

bool update_ini_values(
    const std::wstring& path, const std::vector<IniEdit>& values) noexcept {
    try {
        std::string contents;
        if (!read_file_bytes(path, contents)) return false;
        for (const auto& edit : values) {
            if (!replace_ini_value(contents, edit.section, edit.key, edit.value)) return false;
        }
        return write_file_atomically(path, contents);
    } catch (...) {
        return false;
    }
}

bool utf8_to_wide(std::string_view value, std::wstring& output) noexcept {
    if (value.empty()) {
        output.clear();
        return true;
    }
    if (value.size() > static_cast<std::size_t>(INT_MAX)) return false;
    const int input_length = static_cast<int>(value.size());
    const int required = MultiByteToWideChar(
        CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), input_length, nullptr, 0);
    if (required <= 0) return false;
    try {
        output.resize(static_cast<std::size_t>(required));
    } catch (...) {
        return false;
    }
    return MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
        value.data(), input_length, output.data(), required) == required;
}

bool wide_to_utf8(std::wstring_view value, std::string& output) noexcept {
    if (value.empty()) {
        output.clear();
        return true;
    }
    if (value.size() > static_cast<std::size_t>(INT_MAX)) return false;
    const int input_length = static_cast<int>(value.size());
    const int required = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS,
        value.data(), input_length, nullptr, 0, nullptr, nullptr);
    if (required <= 0) return false;
    try {
        output.resize(static_cast<std::size_t>(required));
    } catch (...) {
        return false;
    }
    return WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS,
        value.data(), input_length, output.data(), required,
        nullptr, nullptr) == required;
}

std::string ini_identity(std::string_view section, std::string_view key) {
    std::string output;
    output.reserve(section.size() + key.size() + 1);
    for (char ch : section) {
        if (ch >= 'A' && ch <= 'Z') ch = static_cast<char>(ch - 'A' + 'a');
        output.push_back(ch);
    }
    output.push_back('\0');
    for (char ch : key) {
        if (ch >= 'A' && ch <= 'Z') ch = static_cast<char>(ch - 'A' + 'a');
        output.push_back(ch);
    }
    return output;
}

std::wstring lowercase_path(std::wstring value) {
    if (!value.empty() && value.size() <= static_cast<std::size_t>(INT_MAX)) {
        CharLowerBuffW(value.data(), static_cast<DWORD>(value.size()));
    }
    return value;
}

bool is_regular_file(const std::wstring& path) noexcept {
    const DWORD attributes = GetFileAttributesW(path.c_str());
    return attributes != INVALID_FILE_ATTRIBUTES &&
        (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0;
}

bool is_mod_module_path(std::wstring_view path) {
    std::wstring lowered(path);
    if (lowered.size() > static_cast<std::size_t>(INT_MAX)) return false;
    if (!lowered.empty()) CharLowerBuffW(lowered.data(), static_cast<DWORD>(lowered.size()));
    constexpr std::wstring_view markers[] = {
        L"\\natives\\", L"\\mods\\", L"\\external_dlls\\", L"\\dlls\\"};
    return std::any_of(std::begin(markers), std::end(markers),
        [&lowered](std::wstring_view marker) {
            return lowered.find(marker) != std::wstring::npos;
        });
}

bool is_framework_module(std::wstring_view path) noexcept {
    const std::size_t slash = path.find_last_of(L"\\/");
    const std::wstring_view filename = slash == std::wstring_view::npos
        ? path : path.substr(slash + 1);
    constexpr std::wstring_view ignored[] = {
        L"ERNativeUI.dll", L"NativeTweaks.dll", L"ERModSettingsHub.dll", L"dinput8.dll",
        L"ModEngine3.dll", L"ModEngine2.dll"};
    return std::any_of(std::begin(ignored), std::end(ignored),
        [filename](std::wstring_view item) {
            if (filename.size() != item.size()) return false;
            return CompareStringOrdinal(filename.data(),
                static_cast<int>(filename.size()), item.data(),
                static_cast<int>(item.size()), TRUE) == CSTR_EQUAL;
        });
}

bool valid_ini_candidate(const std::wstring& path) noexcept;
std::wstring game_relative_path(std::wstring_view directory);

bool has_matching_ini_sidecar(std::wstring_view module_path) {
    const std::size_t slash = module_path.find_last_of(L"\\/");
    const std::size_t dot = module_path.find_last_of(L'.');
    if (slash == std::wstring_view::npos || dot == std::wstring_view::npos || dot <= slash) {
        return false;
    }
    const std::wstring_view extension = module_path.substr(dot);
    if (CompareStringOrdinal(extension.data(), static_cast<int>(extension.size()),
            L".dll", 4, TRUE) != CSTR_EQUAL) {
        return false;
    }

    // MO2 and ME3 can load a DLL from a nested deployment folder while its
    // configuration stays in the mod's root. Use the DLL basename as the
    // stable pairing key and check a bounded set of parent folders.
    std::wstring sidecar_name(module_path.substr(slash + 1, dot - slash - 1));
    const std::wstring own_folder = std::wstring(module_path.substr(0, slash + 1)) +
        sidecar_name + L"\\" + sidecar_name + L".ini";
    if (valid_ini_candidate(own_folder)) return true;
    const std::wstring virtual_folder = game_relative_path(module_path.substr(0, slash + 1));
    if (!virtual_folder.empty() &&
        (valid_ini_candidate(virtual_folder + sidecar_name + L"\\" + sidecar_name + L".ini") ||
         valid_ini_candidate(virtual_folder + sidecar_name + L".ini"))) return true;
    sidecar_name.append(L".ini");
    std::size_t directory_end = slash;
    for (unsigned parent = 0; parent <= 4; ++parent) {
        std::wstring candidate(module_path.substr(0, directory_end + 1));
        candidate.append(sidecar_name);
        if (valid_ini_candidate(candidate)) return true;
        if (parent == 4 || directory_end == 0) break;
        const std::size_t previous = module_path.find_last_of(L"\\/", directory_end - 1);
        if (previous == std::wstring_view::npos) break;
        directory_end = previous;
    }
    return false;
}

bool is_discoverable_mod_module(std::wstring_view path) {
    if (is_framework_module(path)) return false;
    return is_mod_module_path(path) || has_matching_ini_sidecar(path);
}

bool snapshot_loaded_mod_modules(std::vector<std::wstring>& paths) {
    // Taken every 200 ms during the startup discovery, under the loader lock.
    perf::Scope timing(perf::timer(L"module_snapshot"));
    paths.clear();
    HANDLE snapshot = INVALID_HANDLE_VALUE;
    for (unsigned attempt = 0; attempt < 8; ++attempt) {
        snapshot = CreateToolhelp32Snapshot(
            TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, GetCurrentProcessId());
        if (snapshot != INVALID_HANDLE_VALUE) break;
        if (GetLastError() != ERROR_BAD_LENGTH) return false;
        Sleep(25);
    }
    if (snapshot == INVALID_HANDLE_VALUE) return false;

    MODULEENTRY32W module{};
    module.dwSize = sizeof(module);
    if (Module32FirstW(snapshot, &module)) {
        do {
            const std::wstring_view module_path(module.szExePath);
            if (!is_discoverable_mod_module(module_path)) continue;
            paths.push_back(lowercase_path(std::wstring(module_path)));
        } while (Module32NextW(snapshot, &module));
    }
    CloseHandle(snapshot);
    std::sort(paths.begin(), paths.end());
    return true;
}

// Config files that are not <Name>.ini / <Name>_config.ini, by DLL name.
constexpr std::pair<std::wstring_view, std::wstring_view> kKnownConfigFiles[] = {
    {L"souls_vision", L"sv_config.toml"},
};

bool valid_ini_candidate(const std::wstring& path) noexcept {
    if (!is_regular_file(path)) return false;
    const std::size_t slash = path.find_last_of(L"\\/");
    const std::wstring_view filename = slash == std::wstring::npos
        ? std::wstring_view(path) : std::wstring_view(path).substr(slash + 1);
    constexpr std::wstring_view ignored[] = {
        L"ERNativeUI.ini", L"NativeTweaks.ini", L"ERModSettingsHub.ini",
        L"mod_loader_config.ini", L"eldenmodloader.ini", L"meta.ini"};
    if (std::any_of(std::begin(ignored), std::end(ignored),
        [filename](std::wstring_view item) {
            return filename.size() == item.size() &&
                CompareStringOrdinal(filename.data(),
                    static_cast<int>(filename.size()), item.data(),
                    static_cast<int>(item.size()), TRUE) == CSTR_EQUAL;
        })) return false;
    return true;
}

// Maps <MO2>\mods\<Mod>\<rest> to <game folder>\<rest>; empty when the path
// is not inside an MO2 mods folder.
std::wstring game_relative_path(std::wstring_view directory) {
    const std::wstring lower = lowercase_path(std::wstring(directory));
    const std::size_t mods = lower.rfind(L"\\mods\\");
    if (mods == std::wstring::npos) return {};
    const std::size_t mod_end = lower.find(L'\\', mods + 6);
    if (mod_end == std::wstring::npos) return {};
    wchar_t exe[MAX_PATH]{};
    const DWORD length = GetModuleFileNameW(nullptr, exe, MAX_PATH);
    if (length == 0 || length >= MAX_PATH) return {};
    std::wstring game(exe, length);
    const std::size_t exe_slash = game.find_last_of(L"\\/");
    if (exe_slash == std::wstring::npos) return {};
    game.resize(exe_slash + 1);
    game.append(directory.substr(mod_end + 1));
    return game;
}

// sidecar_only: return the DLL's own INI (<Name>.ini / <Name>_config.ini)
// or nothing; otherwise fall back to every INI in the DLL's folder.
std::vector<std::wstring> ini_paths_for_module(std::wstring_view module_path, bool sidecar_only) {
    std::vector<std::wstring> paths;
    const std::size_t slash = module_path.find_last_of(L"\\/");
    if (slash == std::wstring_view::npos) return paths;
    const std::wstring_view filename = module_path.substr(slash + 1);
    std::vector<std::wstring> directories;
    directories.emplace_back(module_path.substr(0, slash + 1));

    std::size_t folder_start = 0;
    if (slash > 0) {
        const std::size_t previous_slash = module_path.find_last_of(L"\\/", slash - 1);
        if (previous_slash != std::wstring_view::npos) folder_start = previous_slash + 1;
    }
    const std::wstring_view folder = module_path.substr(folder_start, slash - folder_start);
    constexpr std::wstring_view nested_folders[] = {
        L"natives", L"external_dlls", L"dlls", L"bin"};
    const bool is_nested_mod_folder = std::any_of(
        std::begin(nested_folders), std::end(nested_folders),
        [folder](std::wstring_view item) {
            return folder.size() == item.size() &&
                CompareStringOrdinal(folder.data(), static_cast<int>(folder.size()),
                    item.data(), static_cast<int>(item.size()), TRUE) == CSTR_EQUAL;
        });
    if (is_nested_mod_folder) {
        const std::size_t parent_slash = module_path.find_last_of(L"\\/", slash - 1);
        if (parent_slash != std::wstring_view::npos) {
            directories.emplace_back(module_path.substr(0, parent_slash + 1));
        }
    }

    const std::size_t dot = filename.find_last_of(L'.');
    if (dot != std::wstring_view::npos &&
        CompareStringOrdinal(filename.data() + dot,
            static_cast<int>(filename.size() - dot), L".dll", 4, TRUE) == CSTR_EQUAL) {
        const std::wstring basename(filename.substr(0, dot));
        const std::wstring virtual_folder = game_relative_path(module_path.substr(0, slash + 1));
        // <Name>.ini first; <Name>_config.ini (Adaptive Bar Scaling) next, so
        // that INI is tied to its own DLL even in a folder shared with others.
        std::vector<std::wstring> sidecars{basename + L".ini", basename + L"_config.ini"};
        for (const auto& [dll, config] : kKnownConfigFiles) {
            if (CompareStringOrdinal(basename.c_str(), static_cast<int>(basename.size()),
                    dll.data(), static_cast<int>(dll.size()), TRUE) == CSTR_EQUAL)
                sidecars.emplace(sidecars.begin(), config);
        }
        for (const std::wstring& sidecar : sidecars) {
            // Some mods (Solid Uncapper 2.3+) keep their INI in a folder named
            // after the DLL: DLLs\<Name>\<Name>.ini.
            const std::wstring own_folder =
                std::wstring(module_path.substr(0, slash + 1)) + basename + L"\\" + sidecar;
            if (valid_ini_candidate(own_folder)) {
                paths.push_back(own_folder);
                return paths;
            }
            // MO2 loads the DLL from mods\<Mod>\..., but files the mod writes at
            // runtime land in Overwrite and are only visible through the game
            // folder. Retry the same relative path under the game directory.
            if (!virtual_folder.empty()) {
                for (const std::wstring& candidate : {
                         virtual_folder + basename + L"\\" + sidecar, virtual_folder + sidecar}) {
                    if (valid_ini_candidate(candidate)) {
                        paths.push_back(candidate);
                        return paths;
                    }
                }
            }
            std::size_t directory_end = slash;
            for (unsigned parent = 0; parent <= 4; ++parent) {
                const std::wstring exact =
                    std::wstring(module_path.substr(0, directory_end + 1)) + sidecar;
                if (valid_ini_candidate(exact)) {
                    paths.push_back(exact);
                    return paths;
                }
                if (parent == 4 || directory_end == 0) break;
                const std::size_t previous = module_path.find_last_of(L"\\/", directory_end - 1);
                if (previous == std::wstring_view::npos) break;
                directory_end = previous;
            }
        }
    }
    if (sidecar_only) return paths;

    for (const auto& directory : directories) {
        WIN32_FIND_DATAW data{};
        HANDLE search = FindFirstFileW((directory + L"*.ini").c_str(), &data);
        if (search == INVALID_HANDLE_VALUE) continue;
        do {
            if ((data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) continue;
            std::wstring candidate = directory + data.cFileName;
            if (valid_ini_candidate(candidate)) paths.push_back(std::move(candidate));
            if (paths.size() >= 16) break;
        } while (FindNextFileW(search, &data));
        FindClose(search);
        if (paths.size() >= 16) break;
    }
    return paths;
}

// ---- Optional modder template -------------------------------------------
// [NativeTweaks.<Section>.<Key>] describes how <Section>/<Key> is shown:
//   Label, Description, Category (each may have a .es / .en variant),
//   Type=bool|int|float|enum|text, Min, Max, Step, Options=v:Label|v:Label,
//   Hidden=1. See docs/INI_TEMPLATE_FOR_MODDERS.md.
using TemplateFields = std::unordered_map<std::string, std::string>;  // lower-case field -> value

constexpr std::string_view kTemplatePrefix = "nativetweaks.";
constexpr std::size_t kMaxChoices = 32;

#include "solid_uncapper_templates.inc"
#include "adaptive_bar_scaling_templates.inc"
#include "souls_vision_templates.inc"

// INI files of other mods that get a built-in template (matched by file name).
struct BuiltinIni {
    std::wstring_view filename;
    const BuiltinTemplate* templates;
    std::size_t template_count;
    const BuiltinLabel* labels;
    std::size_t label_count;
    const wchar_t* display_name;  // nullptr: the file name without extension
    const wchar_t* help_en;
    const wchar_t* help_es;
    const wchar_t* help_zh;
    // The mod watches the file and reloads it by itself (no NativeTweaks API).
    bool reloads_itself = false;
    // Rows (and so submenus) follow the template table, not the file.
    bool template_order = false;
};

const BuiltinIni kBuiltinInis[] = {
    {L"Solid Uncapper.ini", kSolidUncapperTemplates, std::size(kSolidUncapperTemplates),
        kSolidUncapperLabelsEn, std::size(kSolidUncapperLabelsEn), nullptr,
        L"Solid Uncapper settings. Save writes Solid Uncapper.ini; most changes apply after restarting the game.",
        L"Ajustes de Solid Uncapper. Guardar escribe Solid Uncapper.ini; la mayoría de cambios se aplican al reiniciar el juego.",
        L"Solid Uncapper 设置。保存会写入 Solid Uncapper.ini；大多数更改在重启游戏后生效。"},
    {L"AdaptiveBarScaling_config.ini", kAdaptiveBarScalingTemplates, std::size(kAdaptiveBarScalingTemplates),
        kAdaptiveBarScalingLabelsEn, std::size(kAdaptiveBarScalingLabelsEn), L"Adaptive Bar Scaling",
        L"HP, FP and stamina bar lengths of Adaptive Bar Scaling. Changes apply in game when you save (build 4 or later).",
        L"Longitud de las barras de vida, PC y aguante de Adaptive Bar Scaling. Los cambios se aplican en el juego al guardar (versión 4 o posterior).",
        L"Adaptive Bar Scaling 的生命、专注和耐力条长度。保存后在游戏中立即生效（需版本 4 或更新）。"},
    {L"sv_config.toml", kSoulsVisionTemplates, std::size(kSoulsVisionTemplates),
        kSoulsVisionLabelsEn, std::size(kSoulsVisionLabelsEn), L"Souls Vision",
        L"Enemy bars of Souls Vision (sv_config.toml). Changes apply in game as soon as you save, except those under Requires restart.",
        L"Barras de enemigos de Souls Vision (sv_config.toml). Los cambios se aplican en el juego nada más guardar, salvo los de Requiere reiniciar.",
        L"Souls Vision 的敌人状态条（sv_config.toml）。保存后更改立即在游戏中生效，“Requires restart”中的设置除外。",
        true, true},
};

const BuiltinIni* builtin_ini_for(const std::wstring& path) noexcept {
    const std::size_t slash = path.find_last_of(L"\\/");
    const std::wstring_view filename = slash == std::wstring::npos
        ? std::wstring_view(path) : std::wstring_view(path).substr(slash + 1);
    for (const auto& ini : kBuiltinInis) {
        if (CompareStringOrdinal(filename.data(), static_cast<int>(filename.size()),
                ini.filename.data(), static_cast<int>(ini.filename.size()), TRUE) == CSTR_EQUAL)
            return &ini;
    }
    return nullptr;
}

// "<Section>.<Key>" first (keys repeated across sections), then the key alone.
const BuiltinTemplate* find_builtin_template(
    const BuiltinIni& ini, std::string_view section, std::string_view key) {
    const std::string qualified = std::string(section) + "." + std::string(key);
    for (std::size_t index = 0; index < ini.template_count; ++index) {
        if (ascii_equal(ini.templates[index].key, qualified)) return &ini.templates[index];
    }
    for (std::size_t index = 0; index < ini.template_count; ++index) {
        if (ascii_equal(ini.templates[index].key, key)) return &ini.templates[index];
    }
    return nullptr;
}

TemplateFields builtin_template_fields(const BuiltinIni& ini, const BuiltinTemplate& entry) {
    TemplateFields fields;
    const auto set = [&](const char* name, const char* value) {
        if (value && *value) fields[name] = value;
    };
    if (std::string_view(entry.type) == "hidden") {
        fields["hidden"] = "1";
        return fields;
    }
    set("type", entry.type);
    set("min", entry.minimum);
    set("max", entry.maximum);
    set("step", entry.step);
    set("options", entry.options);
    if (entry.options) fields["allowcustom"] = "1";
    set("label", entry.key);
    for (std::size_t index = 0; index < ini.label_count; ++index) {
        if (ascii_equal(ini.labels[index].key, entry.key)) {
            set("label", ini.labels[index].label_en);
            break;
        }
    }
    set("label.es", entry.label_es);
    set("description", entry.description_en);
    set("description.es", entry.description_es);
    set("category", entry.category_en);
    set("category.es", entry.category_es);
    return fields;
}

std::string lower_ascii(std::string_view value) {
    std::string output(value);
    for (char& ch : output) {
        if (ch >= 'A' && ch <= 'Z') ch = static_cast<char>(ch - 'A' + 'a');
    }
    return output;
}

const char* template_language() noexcept {
    return language_code(g_language);
}

// Localized field first (Label.es), then the plain one (Label).
const std::string* template_field(const TemplateFields& fields, std::string_view name) {
    if (g_language == Language::chinese_traditional) {
        // Label.zht wins over the Simplified Label.zh.
        const auto traditional = fields.find(std::string(name) + ".zht");
        if (traditional != fields.end()) return &traditional->second;
        const auto simplified = fields.find(std::string(name) + ".zh");
        if (simplified != fields.end()) return &simplified->second;
    }
    const auto localized = fields.find(std::string(name) + "." + template_language());
    if (localized != fields.end()) return &localized->second;
    const auto plain = fields.find(std::string(name));
    return plain == fields.end() ? nullptr : &plain->second;
}

bool template_text(const TemplateFields& fields, std::string_view name,
    std::size_t maximum, std::wstring& output) {
    const std::string* value = template_field(fields, name);
    std::wstring wide;
    if (!value || value->empty() || !utf8_to_wide(*value, wide) || wide.size() > maximum ||
        wide.find_first_of(L"\r\n") != std::wstring::npos) return false;
    // A Simplified Label.zh is shown in Traditional; Traditional text is unchanged.
    output = g_language == Language::chinese_traditional ? zht::convert(wide) : std::move(wide);
    return true;
}

bool parse_template_number(const std::string& text, double& output) {
    if (text.empty() || text.size() > 32) return false;
    char* end = nullptr;
    const double value = std::strtod(text.c_str(), &end);
    if (end != text.c_str() + text.size() || !std::isfinite(value)) return false;
    output = value;
    return true;
}

std::size_t decimals_of(const std::string& text) {
    const std::size_t dot = text.find('.');
    return dot == std::string::npos ? 0 : std::min<std::size_t>(text.size() - dot - 1, 6);
}

std::wstring format_number(double value, std::size_t decimals) {
    wchar_t buffer[64]{};
    swprintf_s(buffer, L"%.*f", static_cast<int>(decimals), value);
    return buffer;
}

// Toggle words in the same spelling the INI already uses.
bool bool_tokens(const std::wstring& current, std::wstring& off, std::wstring& on, bool& is_on) {
    static constexpr std::pair<const wchar_t*, const wchar_t*> kPairs[] = {
        {L"0", L"1"}, {L"false", L"true"}, {L"off", L"on"}, {L"no", L"yes"}};
    for (const auto& pair : kPairs) {
        for (int which = 0; which < 2; ++which) {
            const std::wstring_view word = which ? pair.second : pair.first;
            if (current.size() != word.size() ||
                CompareStringOrdinal(current.c_str(), static_cast<int>(current.size()),
                    word.data(), static_cast<int>(word.size()), TRUE) != CSTR_EQUAL) continue;
            off = pair.first;
            on = pair.second;
            const bool upper = !current.empty() && iswupper(current[0]);
            const bool all_upper = current.size() > 1 && iswupper(current[1]);
            for (std::wstring* word_out : {&off, &on}) {
                if (all_upper) {
                    for (auto& ch : *word_out) ch = towupper(ch);
                } else if (upper && !word_out->empty()) {
                    (*word_out)[0] = towupper((*word_out)[0]);
                }
            }
            is_on = which == 1;
            return true;
        }
    }
    return false;
}

// Turns a template into a typed control. On any inconsistency the setting
// keeps (or falls back to) a plain text field and the reason is logged.
void apply_setting_template(GenericIniSetting& setting, const TemplateFields& fields) {
    std::wstring text;
    for (std::size_t index = 0; index < std::size(kLanguages); ++index) {
        LanguageScope scope(kLanguages[index]);
        if (template_text(fields, "label", 64, text)) setting.label_by_language[index] = text;
        if (template_text(fields, "description", 512, text)) setting.help_by_language[index] = text;
        if (template_text(fields, "category", 64, text)) setting.category_by_language[index] = text;
    }
    if (template_text(fields, "label", 64, text)) setting.label = std::move(text);
    if (template_text(fields, "description", 512, text)) setting.help = std::move(text);
    if (template_text(fields, "category", 64, text)) setting.category = std::move(text);

    const std::string* type_field = template_field(fields, "type");
    const std::string type = type_field ? lower_ascii(*type_field) : std::string();
    const auto fallback = [&](const wchar_t* reason) {
        setting.control = GenericControl::text;
        setting.tokens.clear();
        setting.choice_labels.clear();
        std::wstring message = L"Template for ";
        std::wstring wide;
        if (utf8_to_wide(setting.section + "/" + setting.key, wide)) message += wide;
        message += L" not usable: ";
        message += reason;
        log_message(message.c_str());
    };

    if (type.empty() || type == "text" || type == "string") return;

    if (type == "bool") {
        std::wstring off, on;
        bool is_on = false;
        if (!bool_tokens(setting.current_value, off, on, is_on)) {
            fallback(L"current value is not 0/1, true/false, on/off or yes/no.");
            return;
        }
        setting.control = GenericControl::toggle;
        setting.tokens = {off, on};
        setting.initial_index = is_on ? 1 : 0;
        return;
    }

    if (type == "enum") {
        const std::string* options = template_field(fields, "options");
        if (!options || options->empty()) { fallback(L"Options= is missing."); return; }
        std::size_t start = 0;
        while (start <= options->size()) {
            const std::size_t bar = options->find('|', start);
            const std::string item = trim_ascii(std::string_view(*options).substr(
                start, (bar == std::string::npos ? options->size() : bar) - start));
            if (!item.empty()) {
                const std::size_t colon = item.find(':');
                const std::string raw = trim_ascii(std::string_view(item).substr(0, colon));
                const std::string shown = colon == std::string::npos
                    ? raw : trim_ascii(std::string_view(item).substr(colon + 1));
                std::wstring token, label;
                if (raw.empty() || !utf8_to_wide(raw, token) || token.size() > ERUI_TEXT_INPUT_MAX_LENGTH ||
                    !utf8_to_wide(shown.empty() ? raw : shown, label) || label.size() > 64) {
                    fallback(L"an option is empty or too long.");
                    return;
                }
                setting.tokens.push_back(std::move(token));
                if (g_language == Language::chinese_traditional) label = zht::convert(label);
                setting.choice_labels.push_back(std::move(label));
            }
            if (bar == std::string::npos) break;
            start = bar + 1;
        }
        if (setting.tokens.empty() || setting.tokens.size() > kMaxChoices) {
            fallback(L"Options= needs 1 to 32 values.");
            return;
        }
        for (std::size_t index = 0; index < setting.tokens.size(); ++index) {
            const std::wstring& token = setting.tokens[index];
            if (CompareStringOrdinal(token.c_str(), static_cast<int>(token.size()),
                    setting.current_value.c_str(), static_cast<int>(setting.current_value.size()),
                    TRUE) == CSTR_EQUAL) {
                setting.control = GenericControl::choice;
                setting.initial_index = static_cast<std::uint8_t>(index);
                return;
            }
        }
        const std::string* allow_custom = template_field(fields, "allowcustom");
        if (allow_custom && *allow_custom == "1" && setting.tokens.size() < kMaxChoices &&
            !setting.current_value.empty()) {
            setting.tokens.push_back(setting.current_value);
            setting.choice_labels.push_back(setting.current_value);
            setting.control = GenericControl::choice;
            setting.initial_index = static_cast<std::uint8_t>(setting.tokens.size() - 1);
            return;
        }
        fallback(L"the current value is not one of Options=.");
        return;
    }

    if (type != "int" && type != "float") {
        fallback(L"unknown Type= (use bool, int, float, enum or text).");
        return;
    }
    const std::string* min_text = template_field(fields, "min");
    const std::string* max_text = template_field(fields, "max");
    const std::string* step_text = template_field(fields, "step");
    double minimum = 0, maximum = 0, step = 1, current = 0;
    std::string current_utf8;
    if (!min_text || !max_text || !parse_template_number(*min_text, minimum) ||
        !parse_template_number(*max_text, maximum) || maximum <= minimum ||
        (step_text && (!parse_template_number(*step_text, step) || step <= 0))) {
        fallback(L"Min/Max/Step are missing or invalid.");
        return;
    }
    if (!wide_to_utf8(setting.current_value, current_utf8) ||
        !parse_template_number(current_utf8, current)) {
        fallback(L"the current value is not a number.");
        return;
    }
    const bool is_int = type == "int";
    if (is_int && (minimum != std::floor(minimum) || maximum != std::floor(maximum) ||
        step != std::floor(step))) {
        fallback(L"Type=int needs whole Min/Max/Step.");
        return;
    }
    const double positions = std::floor((maximum - minimum) / step + 1e-9) + 1;
    const double clamped = std::clamp(current, minimum, maximum);
    const auto nearest = static_cast<std::size_t>(std::llround((clamped - minimum) / step));

    // Whole numbers inside 0..255 use the native slider, which shows the real value.
    if (is_int && minimum >= 0 && maximum <= 255) {
        setting.control = GenericControl::slider;
        setting.slider_minimum = static_cast<std::int32_t>(minimum);
        setting.slider_maximum = static_cast<std::int32_t>(maximum);
        setting.slider_step = static_cast<std::int32_t>(step);
        setting.initial_index = static_cast<std::uint8_t>(std::clamp<double>(
            minimum + static_cast<double>(nearest) * step, minimum, maximum));
        return;
    }
    const std::size_t decimals = is_int ? 0 : std::max(
        decimals_of(step_text ? *step_text : std::string()),
        std::max(decimals_of(*min_text), decimals_of(*max_text)));
    if (positions <= static_cast<double>(kMaxChoices)) {
        for (std::size_t index = 0; index < static_cast<std::size_t>(positions); ++index) {
            std::wstring value = format_number(minimum + static_cast<double>(index) * step, decimals);
            setting.choice_labels.push_back(value);
            setting.tokens.push_back(std::move(value));
        }
        setting.control = GenericControl::choice;
        setting.initial_index = static_cast<std::uint8_t>(std::min(nearest, setting.tokens.size() - 1));
        return;
    }

    // Too many steps for a list: ERNativeUI text inputs are accepted but never
    // drawn, so offer a coarser list of round values (1/2/2.5/5 x 10^n, a
    // multiple of Step) and keep the current value as one of the entries.
    const double span = maximum - minimum;
    double coarse = step;
    for (double magnitude = std::pow(10.0, std::floor(std::log10(span / 30.0)));
        coarse == step || std::floor(span / coarse + 1e-9) + 1 > kMaxChoices - 2;
        magnitude *= 10.0) {
        for (const double factor : {1.0, 2.0, 2.5, 5.0, 10.0}) {
            double candidate = magnitude * factor;
            candidate = std::max(step, std::round(candidate / step) * step);
            if (std::floor(span / candidate + 1e-9) + 1 <= kMaxChoices - 2) {
                coarse = candidate;
                break;
            }
        }
    }
    std::vector<double> values;
    for (double value = minimum; value <= maximum + coarse * 1e-9; value += coarse) {
        values.push_back(value);
    }
    if (values.back() < maximum - coarse * 1e-9) values.push_back(maximum);
    const double snapped = minimum + static_cast<double>(nearest) * step;
    if (std::none_of(values.begin(), values.end(),
            [&](double value) { return std::fabs(value - snapped) < step * 1e-6; })) {
        values.insert(std::upper_bound(values.begin(), values.end(), snapped), snapped);
    }
    std::size_t current_index = 0;
    for (std::size_t index = 0; index < values.size(); ++index) {
        std::wstring value = format_number(values[index], decimals);
        if (std::fabs(values[index] - snapped) < step * 1e-6) current_index = index;
        setting.choice_labels.push_back(value);
        setting.tokens.push_back(std::move(value));
    }
    setting.control = GenericControl::choice;
    setting.initial_index = static_cast<std::uint8_t>(current_index);
    std::wstring message = L"Template for ";
    std::wstring wide;
    if (utf8_to_wide(setting.section + "/" + setting.key, wide)) message += wide;
    message += L" shown as " + std::to_wstring(values.size()) + L" round values (every " +
        format_number(coarse, decimals) + L").";
    log_message(message.c_str());
}

// ---- Automatic detection (no template) -----------------------------------
// The key name, its value and the INI comments around it decide the control:
// switches, "0 = Off, 1 = Low" lists, "(0-100)" / "min 0 max 100" ranges,
// and finally a range guessed from the number itself.

bool is_ascii_digit(char ch) { return ch >= '0' && ch <= '9'; }
bool is_ascii_alpha(char ch) { return (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z'); }

// Reads -12 / 3.5 at `index` (not glued to a word or another number).
bool read_comment_number(std::string_view text, std::size_t& index, std::string& output) {
    std::size_t at = index;
    if (at > 0 && (is_ascii_alpha(text[at - 1]) || is_ascii_digit(text[at - 1]) ||
        text[at - 1] == '.')) return false;
    if (at < text.size() && text[at] == '-') ++at;
    const std::size_t digits = at;
    while (at < text.size() && is_ascii_digit(text[at])) ++at;
    if (at == digits) return false;
    if (at + 1 < text.size() && text[at] == '.' && is_ascii_digit(text[at + 1])) {
        ++at;
        while (at < text.size() && is_ascii_digit(text[at])) ++at;
    }
    if (at < text.size() && (is_ascii_alpha(text[at]) || text[at] == '_')) return false;
    output.assign(text.substr(index, at - index));
    index = at;
    return true;
}

void skip_spaces(std::string_view text, std::size_t& index) {
    while (index < text.size() && (text[index] == ' ' || text[index] == '\t')) ++index;
}

// "<number> = Label" or "<number>: Label" at `index`.
bool read_comment_option(std::string_view text, std::size_t& index,
    std::string& value, std::string& label) {
    std::size_t at = index;
    if (!read_comment_number(text, at, value)) return false;
    skip_spaces(text, at);
    if (at >= text.size() || (text[at] != '=' && text[at] != ':')) return false;
    ++at;
    skip_spaces(text, at);
    const std::size_t start = at;
    while (at < text.size()) {
        const char ch = text[at];
        if (ch == ',' || ch == ';' || ch == '|' || ch == '(' || ch == ')' || ch == '/') break;
        if (ch == ' ') {
            std::size_t probe = at;
            skip_spaces(text, probe);
            std::string ignored;
            std::size_t after = probe;
            if (read_comment_number(text, after, ignored)) {
                skip_spaces(text, after);
                if (after < text.size() && (text[after] == '=' || text[after] == ':')) break;
            }
        }
        ++at;
    }
    label = trim_ascii(text.substr(start, at - start));
    if (!label.empty() && (label.back() == '.')) label.pop_back();
    if (label.empty() || label.size() > 32 ||
        std::none_of(label.begin(), label.end(), is_ascii_alpha)) return false;
    index = at;
    return true;
}

// "0 = Off, 1 = Low, 2 = High" -> Options=0:Off|1:Low|2:High.
bool comment_options(std::string_view comment, const std::string& current, std::string& options) {
    std::vector<std::pair<std::string, std::string>> found;
    for (std::size_t index = 0; index < comment.size() && found.size() <= kMaxChoices;) {
        std::string value, label;
        if (read_comment_option(comment, index, value, label)) {
            if (std::none_of(found.begin(), found.end(),
                    [&](const auto& entry) { return entry.first == value; })) {
                found.emplace_back(std::move(value), std::move(label));
            }
        } else {
            ++index;
        }
    }
    if (found.size() < 2 || found.size() > kMaxChoices ||
        std::none_of(found.begin(), found.end(),
            [&](const auto& entry) { return entry.first == current; })) return false;
    options.clear();
    for (const auto& [value, label] : found) {
        if (!options.empty()) options += '|';
        options += value + ':' + label;
    }
    return true;
}

// "(0-100)", "0 to 100", "0 ~ 2.5", "min 0 max 100" -> bounds that contain `current`.
bool comment_range(std::string_view comment, double current,
    std::string& minimum, std::string& maximum) {
    const std::string lower = lower_ascii(comment);
    double low = 0, high = 0;
    for (std::size_t index = 0; index < comment.size(); ++index) {
        std::size_t at = index;
        std::string first, second;
        if (!read_comment_number(comment, at, first)) continue;
        skip_spaces(comment, at);
        std::size_t connector = at;
        if (lower.compare(at, 2, "to") == 0) connector += 2;
        else if (lower.compare(at, 2, "..") == 0) connector += 2;
        else if (lower.compare(at, 3, "\xE2\x80\x93") == 0) connector += 3;  // en dash
        else if (at < comment.size() && (comment[at] == '-' || comment[at] == '~')) ++connector;
        if (connector == at) continue;
        skip_spaces(comment, connector);
        if (!read_comment_number(comment, connector, second)) continue;
        if (parse_template_number(first, low) && parse_template_number(second, high) &&
            low < high && current >= low && current <= high) {
            minimum = first;
            maximum = second;
            return true;
        }
    }
    const auto number_after = [&](std::string_view word, std::string& output) {
        for (std::size_t found = lower.find(word); found != std::string::npos;
            found = lower.find(word, found + 1)) {
            if (found > 0 && is_ascii_alpha(lower[found - 1])) continue;
            std::size_t at = found + word.size();
            while (at < lower.size() && is_ascii_alpha(lower[at])) ++at;  // min/minimum
            skip_spaces(comment, at);
            if (at < comment.size() && (comment[at] == ':' || comment[at] == '=')) ++at;
            skip_spaces(comment, at);
            if (read_comment_number(comment, at, output)) return true;
        }
        return false;
    };
    std::string first, second;
    if (number_after("min", first) && number_after("max", second) &&
        parse_template_number(first, low) && parse_template_number(second, high) &&
        low < high && current >= low && current <= high) {
        minimum = first;
        maximum = second;
        return true;
    }
    return false;
}

// Key names that read as a switch: bFoo, EnableFoo, UseFoo, ShowFoo, FooEnabled...
bool switch_like_key(std::string_view key) {
    if (key.size() >= 2 && key[0] == 'b' && key[1] >= 'A' && key[1] <= 'Z') return true;
    const std::string lower = lower_ascii(key);
    for (const std::string_view prefix : {"enable", "disable", "use", "show", "hide", "allow",
             "is", "no", "skip", "toggle", "force", "unlock"}) {
        if (lower.size() > prefix.size() && lower.compare(0, prefix.size(), prefix) == 0 &&
            (key[prefix.size()] == '_' || (key[prefix.size()] >= 'A' && key[prefix.size()] <= 'Z')))
            return true;
    }
    for (const std::string_view suffix : {"enabled", "disabled", "_on"}) {
        if (lower.size() > suffix.size() &&
            lower.compare(lower.size() - suffix.size(), suffix.size(), suffix) == 0) return true;
    }
    return false;
}

bool switch_like_comment(std::string_view comment) {
    const std::string lower = lower_ascii(comment);
    for (const std::string_view hint : {"true/false", "0/1", "1/0", "on/off", "enable",
             "disable", "toggle", "1 = on", "1=on", "0 = off", "0=off"}) {
        if (lower.find(hint) != std::string::npos) return true;
    }
    return false;
}

// Upper bound for a guessed range: room for at least twice the current value.
double guessed_bound(double magnitude, bool is_int) {
    const double needed = std::max(magnitude * 2.0, is_int ? 10.0 : 1.0);
    if (is_int && needed <= 10) return 10;
    if (is_int && needed <= 100) return 100;
    if (is_int && needed <= 255) return 255;
    double bound = is_int ? 1000.0 : 1.0;
    while (bound < needed && bound < 1e9) bound *= 10.0;
    return bound;
}

// Without a template, picks a control from the key, value and comments.
// Returns false when the value cannot be shown (e.g. free text).
bool infer_control(GenericIniSetting& setting) {
    std::wstring off, on;
    bool is_on = false;
    std::string options;
    std::string plain;
    // A "0 = Off, 1 = Low, 2 = High" list beats the 0/1 switch hints it contains.
    const bool listed = wide_to_utf8(setting.current_value, plain) &&
        comment_options(setting.comment, plain, options);
    if (!listed && bool_tokens(setting.current_value, off, on, is_on) &&
        (off != L"0" || switch_like_key(setting.key) || switch_like_comment(setting.comment))) {
        setting.control = GenericControl::toggle;
        setting.tokens = {off, on};
        setting.initial_index = is_on ? 1 : 0;
        return true;
    }
    std::string value;
    double current = 0;
    // Plain decimals only: hex key codes (0x70), 1e5 or 1.0f stay as hand-edited text.
    if (!wide_to_utf8(setting.current_value, value) || value.empty() ||
        std::any_of(value.begin(), value.end(),
            [](char ch) { return !is_ascii_digit(ch) && ch != '-' && ch != '.'; }) ||
        !parse_template_number(value, current)) {
        return false;
    }
    TemplateFields fields;
    std::string minimum, maximum;
    if (listed) {
        fields["type"] = "enum";
        fields["options"] = std::move(options);
    } else {
        const bool from_comment = comment_range(setting.comment, current, minimum, maximum);
        double low = 0, high = 0;
        if (from_comment) {
            (void)parse_template_number(minimum, low);
            (void)parse_template_number(maximum, high);
        }
        const bool is_int = value.find('.') == std::string::npos &&
            minimum.find('.') == std::string::npos && maximum.find('.') == std::string::npos;
        if (!from_comment) {
            high = guessed_bound(std::fabs(current), is_int);
            low = current < 0 ? -high : 0;
            minimum = std::to_string(static_cast<long long>(low));
            maximum = std::to_string(static_cast<long long>(high));
        }
        fields["type"] = is_int ? "int" : "float";
        fields["min"] = minimum;
        fields["max"] = maximum;
        if (!is_int) {
            const std::size_t decimals = std::max<std::size_t>(1, std::max(decimals_of(value),
                std::max(decimals_of(minimum), decimals_of(maximum))));
            std::string step = "0.";
            step.append(decimals - 1, '0');
            step += '1';
            fields["step"] = std::move(step);
        }
    }
    apply_setting_template(setting, fields);
    if (setting.control == GenericControl::text) return false;
    std::wstring message = L"Detected ";
    std::wstring wide;
    if (utf8_to_wide(setting.section + "/" + setting.key, wide)) message += wide;
    message += setting.control == GenericControl::slider ? L" as a slider."
        : L" as a list of " + std::to_wstring(setting.tokens.size()) + L" values.";
    log_message(message.c_str());
    return true;
}

// INI comment text without ;/# markers and separator lines (---- / ====).
std::string comment_text(std::string_view line) {
    while (!line.empty() && (line.front() == ';' || line.front() == '#' ||
        line.front() == ' ' || line.front() == '\t')) line.remove_prefix(1);
    const std::string text = trim_ascii(line);
    if (std::none_of(text.begin(), text.end(),
            [](char ch) { return is_ascii_alpha(ch) || is_ascii_digit(ch) ||
                static_cast<unsigned char>(ch) >= 0x80; })) return {};
    return text;
}

// Help text from the comments: at most 300 characters, cut at a word.
bool comment_help(const std::string& comment, std::wstring& output) {
    std::wstring wide;
    if (comment.empty() || !utf8_to_wide(comment, wide)) return false;
    if (wide.size() > 300) {
        std::size_t cut = wide.find_last_of(L' ', 297);
        if (cut == std::wstring::npos || cut < 200) cut = 297;
        wide.resize(cut);
        wide += L"...";
    }
    output = std::move(wide);
    return true;
}

// Reads [NativeTweaks] and [NativeTweaks.<Section>.<Key>] sections from an
// external template, DLLs\NativeTweaks\templates\<same file name as the INI>.
// Templates inside the mod's own INI are parsed afterwards and win.
void load_external_template(const std::wstring& ini_path,
    std::unordered_map<std::string, TemplateFields>& templates, TemplateFields& mod_fields) {
    if (!feature_enabled(L"ExternalTemplates")) return;
    const std::size_t config_slash = g_config_path.find_last_of(L'\\');
    const std::size_t ini_slash = ini_path.find_last_of(L"\\/");
    if (config_slash == std::wstring::npos) return;
    const std::wstring file_name = ini_slash == std::wstring::npos
        ? ini_path : ini_path.substr(ini_slash + 1);
    const std::wstring template_path = g_config_path.substr(0, config_slash + 1) +
        L"NativeTweaks\\templates\\" + file_name;
    if (GetFileAttributesW(template_path.c_str()) == INVALID_FILE_ATTRIBUTES) return;
    std::string contents;
    std::wstring checked;
    if (!read_file_bytes(template_path, contents) || !utf8_to_wide(contents, checked)) {
        log_message((L"Skipped external template (unreadable or not UTF-8): " + file_name).c_str());
        return;
    }
    std::string section;
    std::size_t entries = 0;
    std::size_t line_start = 0;
    while (line_start < contents.size()) {
        const std::size_t newline = contents.find('\n', line_start);
        const std::size_t line_end = newline == std::string::npos ? contents.size() : newline;
        std::string_view line(contents.data() + line_start, line_end - line_start);
        if (line_start == 0 && line.size() >= 3 && static_cast<unsigned char>(line[0]) == 0xEF &&
            static_cast<unsigned char>(line[1]) == 0xBB && static_cast<unsigned char>(line[2]) == 0xBF) {
            line.remove_prefix(3);
        }
        const std::string trimmed = trim_ascii(line);
        if (trimmed.empty() || trimmed.front() == ';' || trimmed.front() == '#') {
        } else if (trimmed.size() >= 2 && trimmed.front() == '[' && trimmed.back() == ']') {
            section = trim_ascii(std::string_view(trimmed).substr(1, trimmed.size() - 2));
        } else if (const std::size_t equals = trimmed.find('='); equals != std::string::npos) {
            const std::string key = lower_ascii(trim_ascii(std::string_view(trimmed).substr(0, equals)));
            const std::string value = trim_ascii(std::string_view(trimmed).substr(equals + 1));
            const std::string lower_section = lower_ascii(section);
            if (key.empty() || value.size() > 1024) {
            } else if (lower_section.size() > kTemplatePrefix.size() &&
                lower_section.compare(0, kTemplatePrefix.size(), kTemplatePrefix) == 0) {
                const std::string target = section.substr(kTemplatePrefix.size());
                const std::size_t dot = target.find_last_of('.');
                if (dot != std::string::npos && dot > 0 && dot + 1 < target.size()) {
                    templates[ini_identity(target.substr(0, dot), target.substr(dot + 1))][key] = value;
                    ++entries;
                }
            } else if (lower_section == "nativetweaks") {
                mod_fields[key] = value;
                ++entries;
            }
        }
        if (newline == std::string::npos) break;
        line_start = newline + 1;
    }
    log_message((L"External template loaded: " + file_name + L" (" +
        std::to_wstring(entries) + L" fields).").c_str());
}

bool parse_generic_ini(const std::wstring& path, GenericIniMod& output) {
    std::string contents;
    if (!read_file_bytes(path, contents)) {
        log_message(L"Skipped a discovered INI that could not be read safely.");
        return false;
    }
    std::wstring checked_encoding;
    if (!utf8_to_wide(contents, checked_encoding)) {
        log_message(L"Skipped a discovered INI that is not valid UTF-8.");
        return false;
    }

    std::unordered_map<std::string, std::size_t> counts;
    std::vector<GenericIniSetting> parsed;
    std::unordered_map<std::string, TemplateFields> templates;  // ini_identity -> fields
    TemplateFields mod_fields;                                  // [NativeTweaks] Name/Description
    load_external_template(path, templates, mod_fields);
    std::string section;
    std::string pending_comment;  // comment lines waiting for the next key
    std::size_t line_start = 0;
    while (line_start < contents.size()) {
        const std::size_t newline = contents.find('\n', line_start);
        const std::size_t line_end = newline == std::string::npos
            ? contents.size() : newline;
        std::size_t text_end = line_end;
        if (text_end > line_start && contents[text_end - 1] == '\r') --text_end;
        std::string_view line(contents.data() + line_start, text_end - line_start);
        if (line_start == 0 && line.size() >= 3 &&
            static_cast<unsigned char>(line[0]) == 0xEF &&
            static_cast<unsigned char>(line[1]) == 0xBB &&
            static_cast<unsigned char>(line[2]) == 0xBF) {
            line.remove_prefix(3);
        }
        const std::string trimmed = trim_ascii(line);
        if (trimmed.empty()) {
            pending_comment.clear();  // a blank line ends a comment block
        } else if (trimmed.front() == ';' || trimmed.front() == '#') {
            const std::string text = comment_text(trimmed);
            if (!text.empty() && pending_comment.size() < 2048) {
                if (!pending_comment.empty()) pending_comment += ' ';
                pending_comment += text;
            }
        } else if (trimmed.size() >= 2 && trimmed.front() == '[' && trimmed.back() == ']') {
            section = trim_ascii(std::string_view(trimmed).substr(1, trimmed.size() - 2));
            pending_comment.clear();
        } else {
            const std::size_t equals = line.find('=');
            if (equals != std::string_view::npos && !section.empty()) {
                const std::string key = trim_ascii(line.substr(0, equals));
                if (!key.empty()) {
                    std::string_view raw = line.substr(equals + 1);
                    std::size_t comment = std::string_view::npos;
                    for (std::size_t index = 0; index < raw.size(); ++index) {
                        if ((raw[index] == ';' || raw[index] == '#') && index > 0 &&
                            (raw[index - 1] == ' ' || raw[index - 1] == '\t')) {
                            comment = index;
                            break;
                        }
                    }
                    std::string notes = std::move(pending_comment);
                    pending_comment.clear();
                    if (comment != std::string_view::npos) {
                        const std::string inline_text = comment_text(raw.substr(comment));
                        if (!inline_text.empty()) {
                            if (!notes.empty()) notes += ' ';
                            notes += inline_text;
                        }
                        raw = raw.substr(0, comment);
                    }
                    const std::string value = trim_ascii(raw);
                    const std::string lower_section = lower_ascii(section);
                    if (lower_section.size() > kTemplatePrefix.size() &&
                        lower_section.compare(0, kTemplatePrefix.size(), kTemplatePrefix) == 0) {
                        // [NativeTweaks.<Section>.<Key>]: the key is after the last dot.
                        const std::string target = section.substr(kTemplatePrefix.size());
                        const std::size_t dot = target.find_last_of('.');
                        if (dot != std::string::npos && dot > 0 && dot + 1 < target.size() &&
                            value.size() <= 1024) {
                            templates[ini_identity(target.substr(0, dot), target.substr(dot + 1))]
                                [lower_ascii(key)] = value;
                        }
                    } else if (lower_section == "nativetweaks" || lower_section == "ermodsettingshub") {
                        mod_fields[lower_ascii(key)] = value;
                    } else if (ini_identity(section, "") != ini_identity("NativeTweaks", "") &&
                        ini_identity(section, "") != ini_identity("ERModSettingsHub", "")) {
                        std::wstring wide_label{};
                        std::wstring wide_value{};
                        const std::string label = section + " / " + key;
                        if (label.size() <= 128 && value.size() <= 35 &&
                            utf8_to_wide(label, wide_label) && wide_label.size() <= 64 &&
                            utf8_to_wide(value, wide_value) &&
                            wide_value.find_first_of(L"\r\n") == std::wstring::npos) {
                            GenericIniSetting setting{};
                            setting.section = section;
                            setting.key = key;
                            setting.label = std::move(wide_label);
                            setting.initial_value = wide_value;
                            setting.current_value = std::move(wide_value);
                            setting.comment = std::move(notes);
                            ++counts[ini_identity(section, key)];
                            parsed.push_back(std::move(setting));
                        }
                    }
                }
            }
        }
        if (newline == std::string::npos) break;
        line_start = newline + 1;
    }

    std::size_t templated = 0;
    std::wstring skipped;
    std::size_t skipped_count = 0;
    const BuiltinIni* const builtin_ini = builtin_ini_for(path);
    for (auto& setting : parsed) {
        const std::string identity = ini_identity(setting.section, setting.key);
        if (counts[identity] != 1) continue;
        // A template inside the INI wins; known mods (Solid Uncapper, Adaptive
        // Bar Scaling) fall back to a built-in table matched by key name.
        const auto found = templates.find(identity);
        TemplateFields builtin;
        const TemplateFields* fields = nullptr;
        if (found != templates.end()) {
            fields = &found->second;
        } else if (builtin_ini) {
            if (const BuiltinTemplate* entry = find_builtin_template(*builtin_ini, setting.section, setting.key)) {
                builtin = builtin_template_fields(*builtin_ini, *entry);
                fields = &builtin;
            }
        }
        if (fields) {
            const std::string* hidden = template_field(*fields, "hidden");
            if (hidden && (*hidden == "1" || ascii_equal(*hidden, "true"))) continue;
            apply_setting_template(setting, *fields);
        }
        // The INI's own comments are the help text when no template gives one.
        std::wstring notes;
        if (setting.help.empty() && comment_help(setting.comment, notes)) {
            for (auto& help : setting.help_by_language) {
                if (help.empty()) help = notes;
            }
            setting.help = std::move(notes);
        }
        // ERNativeUI text inputs are never drawn, so a value without a
        // toggle/slider/choice is left out of the menu (edit the INI by hand).
        // Without a Type= in a template, the control is detected automatically.
        const bool typed = fields && template_field(*fields, "type");
        if (setting.control == GenericControl::text && (typed || !infer_control(setting))) {
            if (++skipped_count <= 12) {
                std::wstring wide;
                if (utf8_to_wide(setting.section + "/" + setting.key, wide)) {
                    if (!skipped.empty()) skipped += L", ";
                    skipped += wide;
                }
            }
            continue;
        }
        if (fields) ++templated;
        output.settings.push_back(std::move(setting));
    }
    if (builtin_ini && builtin_ini->template_order) {
        const auto rank = [builtin_ini](const GenericIniSetting& setting) {
            const BuiltinTemplate* entry = find_builtin_template(*builtin_ini, setting.section, setting.key);
            return entry ? static_cast<std::size_t>(entry - builtin_ini->templates) : builtin_ini->template_count;
        };
        std::stable_sort(output.settings.begin(), output.settings.end(),
            [&rank](const GenericIniSetting& left, const GenericIniSetting& right) {
                return rank(left) < rank(right);
            });
    }
    if (skipped_count != 0) {
        std::wstring message = L"Left out of the menu (no template control; edit by hand): " +
            skipped + (skipped_count > 12 ? L" and " + std::to_wstring(skipped_count - 12) +
                L" more." : L".");
        log_message(message.c_str());
    }
    if (templated != 0) {
        wchar_t message[128]{};
        swprintf_s(message, L"INI template applied to %zu of %zu settings.",
            templated, output.settings.size());
        log_message(message);
    }
    std::wstring metadata_display_name;
    (void)template_text(mod_fields, "name", 64, metadata_display_name);
    (void)template_text(mod_fields, "description", 512, output.help);
    for (std::size_t index = 0; index < std::size(kLanguages); ++index) {
        LanguageScope scope(kLanguages[index]);
        (void)template_text(mod_fields, "name", 64, output.name_by_language[index]);
        (void)template_text(mod_fields, "description", 512, output.help_by_language[index]);
        if (output.help_by_language[index].empty() && builtin_ini) {
            output.help_by_language[index] = tr(builtin_ini->help_en, builtin_ini->help_es, builtin_ini->help_zh);
        }
    }
    if (output.help.empty() && builtin_ini) {
        output.help = tr(builtin_ini->help_en, builtin_ini->help_es, builtin_ini->help_zh);
    }
    if (metadata_display_name.empty() && builtin_ini && builtin_ini->display_name) {
        metadata_display_name = builtin_ini->display_name;
    }
    if (output.settings.empty()) {
        log_message(L"A discovered INI had no values the native editor can show; no page was added.");
        return false;
    }
    output.path = path;
    const std::size_t slash = path.find_last_of(L"\\/");
    const std::wstring_view filename = slash == std::wstring::npos
        ? std::wstring_view(path) : std::wstring_view(path).substr(slash + 1);
    const std::size_t dot = filename.find_last_of(L'.');
    if (!metadata_display_name.empty()) {
        output.display_name = std::move(metadata_display_name);
    } else {
        output.display_name.assign(dot == std::wstring_view::npos
            ? filename : filename.substr(0, dot));
    }
    return true;
}

// True if a loaded mod DLL names ERUI_GetApi in its read-only data: it builds
// its own ERNativeUI menu (Spirit Multiplier, for one), so a generic page for
// its INI would only duplicate that menu.
bool module_has_own_erui_menu(const std::wstring& module_path) noexcept {
    const std::size_t slash = module_path.find_last_of(L"\\/");
    const std::wstring file_name = module_path.substr(slash == std::wstring::npos ? 0 : slash + 1);
    const auto* base = reinterpret_cast<const unsigned char*>(GetModuleHandleW(file_name.c_str()));
    if (!base) return false;
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return false;
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) return false;
    constexpr std::string_view needle("ERUI_GetApi\0", 12);
    const IMAGE_SECTION_HEADER* section = IMAGE_FIRST_SECTION(nt);
    for (WORD index = 0; index < nt->FileHeader.NumberOfSections; ++index, ++section) {
        const DWORD flags = section->Characteristics;
        if (!(flags & IMAGE_SCN_MEM_READ) || (flags & (IMAGE_SCN_MEM_WRITE | IMAGE_SCN_MEM_EXECUTE)))
            continue;
        const std::string_view data(reinterpret_cast<const char*>(base + section->VirtualAddress),
            (std::min)(section->Misc.VirtualSize, section->SizeOfRawData));
        if (data.find(needle) != std::string_view::npos) return true;
    }
    return false;
}

void discover_generic_ini_mods() noexcept {
    try {
        g_generic_mods.clear();
        std::unordered_set<std::wstring> seen_paths;
        std::vector<std::wstring> module_paths;
        if (!snapshot_loaded_mod_modules(module_paths)) {
            log_message(L"Could not enumerate loaded modules for INI discovery.");
            return;
        }
        std::size_t discovered_count = 0;
        std::unordered_set<std::wstring> own_menu_modules;
        for (const auto& module_path : module_paths) {
            if (!module_has_own_erui_menu(module_path)) continue;
            own_menu_modules.insert(lowercase_path(module_path));
            std::wstring skipped(L"Skipped INI discovery for a DLL with its own ERNativeUI menu: ");
            skipped.append(module_path);
            log_message(skipped.c_str());
        }
        // Pass 1 ties each DLL to its own INI; pass 2 adds the rest of each
        // folder, so a folder scan never claims another DLL's own INI.
        for (const bool sidecar_only : {true, false})
        for (const auto& module_path : module_paths) {
            if (own_menu_modules.count(lowercase_path(module_path))) {
                // Still claim its own INI, so no other DLL's folder scan shows it.
                if (sidecar_only) {
                    for (const auto& ini_path : ini_paths_for_module(module_path, true))
                        seen_paths.insert(lowercase_path(ini_path));
                }
                continue;
            }
            for (const auto& ini_path : ini_paths_for_module(module_path, sidecar_only)) {
                const std::wstring key = lowercase_path(ini_path);
                if (!seen_paths.insert(key).second) continue;
                const std::wstring hud_key = lowercase_path(g_ini_path);
                if (!g_ini_path.empty() && key == hud_key) continue;
                if (g_generic_mods.size() >= 32) break;
                GenericIniMod candidate{};
                candidate.module_path = module_path;
                if (parse_generic_ini(ini_path, candidate)) {
                    if (candidate.settings.size() > 128) {
                        candidate.settings.resize(128);
                        log_message(L"Capped a generic INI page at 128 setting rows.");
                    }
                    g_generic_mods.push_back(std::move(candidate));
                    ++discovered_count;
                    std::wstring detected(L"Discovered mod INI: ");
                    detected.append(ini_path);
                    log_message(detected.c_str());
                }
            }
            if (g_generic_mods.size() >= 32) break;
        }

        std::sort(g_generic_mods.begin(), g_generic_mods.end(),
            [](const GenericIniMod& left, const GenericIniMod& right) {
                return CompareStringOrdinal(left.display_name.data(),
                    static_cast<int>(left.display_name.size()),
                    right.display_name.data(),
                    static_cast<int>(right.display_name.size()), TRUE) == CSTR_LESS_THAN;
            });

        wchar_t message[160]{};
        swprintf_s(message, std::size(message),
            L"INI discovery completed: loaded mod DLLs=%zu config files=%zu.",
            module_paths.size(), discovered_count);
        log_message(message);
    } catch (...) {
        log_message(L"INI discovery stopped safely after an internal error.");
        g_generic_mods.clear();
    }
}

std::string percent_value(std::uint8_t value) {
    const unsigned percent = std::clamp<unsigned>(value, 1, 100);
    const unsigned whole = percent / 100;
    const unsigned fraction = percent % 100;
    if (whole != 0) return "1.00";
    const char digits[] = {
        static_cast<char>('0' + fraction / 10),
        static_cast<char>('0' + fraction % 10)};
    return std::string("0.") + digits[0] + digits[1];
}

void ERUI_CALL setting_changed(void* target, std::uint8_t value) noexcept {
    if (!target) return;
    AcquireSRWLockExclusive(&g_settings_lock);
    *static_cast<std::uint8_t*>(target) = value;
    ReleaseSRWLockExclusive(&g_settings_lock);
}

bool collect_current_values(Settings& settings) noexcept {
    AcquireSRWLockShared(&g_settings_lock);
    settings = g_current;
    ReleaseSRWLockShared(&g_settings_lock);
    const std::pair<erui::RowHandle, std::uint8_t*> rows[] = {
        {g_rows.general_enabled, &settings.general_enabled},
        {g_rows.bar_size_enabled, &settings.bar_size_enabled},
        {g_rows.hp_enabled, &settings.hp_enabled},
        {g_rows.fp_enabled, &settings.fp_enabled},
        {g_rows.stamina_enabled, &settings.stamina_enabled},
        {g_rows.hp_percent, &settings.hp_percent},
        {g_rows.fp_percent, &settings.fp_percent},
        {g_rows.stamina_percent, &settings.stamina_percent}};

    for (const auto& entry : rows) {
        if (entry.first == ERUI_INVALID_ROW) continue;
        if (g_registration.get_value(entry.first, *entry.second) != ERUI_OK) return false;
    }
    return true;
}

enum class SaveJobKind : std::uint8_t {
    settings,
    fit_screen,
    generic_ini
};

struct SaveJob {
    SaveJobKind kind{};
    Settings current{};
    GenericIniMod* generic_mod{};
    std::wstring generic_path{};
    std::wstring generic_module_path{};
    std::vector<IniEdit> generic_edits{};
    std::vector<std::pair<std::size_t, std::wstring>> generic_values{};
};

enum class LiveApplyResult : std::uint8_t {
    applied,
    rejected,
    unavailable,
    timed_out
};

enum class GenericLiveApplyResult : std::uint8_t {
    applied,
    restart_required,
    rejected,
    unsupported,
    timed_out,
    failed
};

LiveApplyResult request_tarnished_hud_live_reload() noexcept {
    using RequestFn = std::uint32_t(__cdecl*)();
    using StatusFn = std::uint32_t(__cdecl*)(std::uint32_t);
    constexpr std::uint32_t pending = 0;
    constexpr std::uint32_t applied = 1;
    constexpr std::uint32_t failed = 2;

    const HMODULE module = GetModuleHandleW(L"TarnishedHUD.dll");
    if (!module) return LiveApplyResult::unavailable;
    const auto request = reinterpret_cast<RequestFn>(
        GetProcAddress(module, "TarnishedHUD_RequestLiveReloadV1"));
    const auto status = reinterpret_cast<StatusFn>(
        GetProcAddress(module, "TarnishedHUD_GetLiveReloadStatusV1"));
    if (!request || !status) return LiveApplyResult::unavailable;

    const std::uint32_t request_id = request();
    if (!request_id) return LiveApplyResult::unavailable;

    for (unsigned attempt = 0; attempt < 100; ++attempt) {
        const std::uint32_t result = status(request_id);
        if (result == applied) return LiveApplyResult::applied;
        if (result == failed) return LiveApplyResult::rejected;
        if (result != pending) return LiveApplyResult::unavailable;
        Sleep(20);
    }
    return LiveApplyResult::timed_out;
}

GenericLiveApplyResult request_generic_live_reload(
    std::wstring_view module_path, std::wstring_view ini_path) noexcept {
    using RequestFn = std::uint32_t(__cdecl*)(const wchar_t*);
    using StatusFn = std::uint32_t(__cdecl*)(std::uint32_t);
    constexpr std::uint32_t pending = 0;
    constexpr std::uint32_t applied = 1;
    constexpr std::uint32_t rejected = 2;
    constexpr std::uint32_t restart_required = 3;
    constexpr std::uint32_t failed = 4;

    try {
        if (module_path.empty()) return GenericLiveApplyResult::unsupported;
        // Find the module by the same Toolhelp path discovery used: under MO2
        // that path (usvfs) can differ from what GetModuleFileNameW reports.
        const std::wstring owned_module_path = lowercase_path(std::wstring(module_path));
        HMODULE module = nullptr;
        const HANDLE snapshot = CreateToolhelp32Snapshot(
            TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, GetCurrentProcessId());
        if (snapshot != INVALID_HANDLE_VALUE) {
            MODULEENTRY32W entry{};
            entry.dwSize = sizeof(entry);
            for (BOOL more = Module32FirstW(snapshot, &entry); more;
                more = Module32NextW(snapshot, &entry)) {
                if (lowercase_path(std::wstring(entry.szExePath)) == owned_module_path) {
                    module = entry.hModule;
                    break;
                }
            }
            CloseHandle(snapshot);
        }
        if (!module) {
            // Snapshot failed or missed it: fall back to the DLL file name.
            const std::size_t slash = module_path.find_last_of(L"\\/");
            const std::wstring file_name(module_path.substr(
                slash == std::wstring_view::npos ? 0 : slash + 1));
            module = GetModuleHandleW(file_name.c_str());
        }
        if (!module) {
            log_message(L"Live reload: the mod DLL is no longer loaded.");
            return GenericLiveApplyResult::unsupported;
        }

        auto request = reinterpret_cast<RequestFn>(
            GetProcAddress(module, "NativeTweaks_RequestReloadV1"));
        auto status = reinterpret_cast<StatusFn>(
            GetProcAddress(module, "NativeTweaks_GetReloadStatusV1"));
        // Mods built against the pre-rename SDK export the ERModSettingsHub_ names.
        if (!request || !status) {
            request = reinterpret_cast<RequestFn>(
                GetProcAddress(module, "ERModSettingsHub_RequestReloadV1"));
            status = reinterpret_cast<StatusFn>(
                GetProcAddress(module, "ERModSettingsHub_GetReloadStatusV1"));
        }
        if (!request || !status) {
            log_message(L"Live reload: the mod DLL does not export NativeTweaks_RequestReloadV1/GetReloadStatusV1.");
            return GenericLiveApplyResult::unsupported;
        }

        const std::wstring owned_ini_path(ini_path);
        const std::uint32_t request_id = request(owned_ini_path.c_str());
        if (!request_id) return GenericLiveApplyResult::failed;

        for (unsigned attempt = 0; attempt < 250; ++attempt) {
            const std::uint32_t result = status(request_id);
            if (result == applied) return GenericLiveApplyResult::applied;
            if (result == rejected) return GenericLiveApplyResult::rejected;
            if (result == restart_required) return GenericLiveApplyResult::restart_required;
            if (result == failed) return GenericLiveApplyResult::failed;
            if (result != pending) return GenericLiveApplyResult::failed;
            Sleep(20);
        }
        return GenericLiveApplyResult::timed_out;
    } catch (...) {
        return GenericLiveApplyResult::failed;
    }
}

void persist_settings(Settings current) noexcept {
    bool saved = false;
    bool no_changes = false;
    bool failed = false;
    bool general_restart_required = false;
    LiveApplyResult live_result = LiveApplyResult::unavailable;
    AcquireSRWLockExclusive(&g_save_lock);
    try {
        // Tarnished HUD build 4 (it also exports the generic reload API)
        // switches itself on and off live; older builds need a restart.
        const HMODULE hud = GetModuleHandleW(L"TarnishedHUD.dll");
        const bool hud_switches_live = hud && GetProcAddress(hud, "NativeTweaks_RequestReloadV1");
        general_restart_required = !hud_switches_live &&
            current.general_enabled != g_initial.general_enabled;
        std::vector<IniEdit> changed;
        if (current.general_enabled != g_initial.general_enabled)
            changed.push_back({"General", "Enabled", current.general_enabled ? "1" : "0"});
        if (current.bar_size_enabled != g_initial.bar_size_enabled)
            changed.push_back({"BarSize", "Enabled", current.bar_size_enabled ? "1" : "0"});
        if (current.hp_enabled != g_initial.hp_enabled)
            changed.push_back({"BarSize", "HPEnabled", current.hp_enabled ? "1" : "0"});
        if (current.fp_enabled != g_initial.fp_enabled)
            changed.push_back({"BarSize", "FPEnabled", current.fp_enabled ? "1" : "0"});
        if (current.stamina_enabled != g_initial.stamina_enabled)
            changed.push_back({"BarSize", "StaminaEnabled", current.stamina_enabled ? "1" : "0"});
        if (current.hp_percent != g_initial.hp_percent)
            changed.push_back({"BarSize", "HPWidth", percent_value(current.hp_percent)});
        if (current.fp_percent != g_initial.fp_percent)
            changed.push_back({"BarSize", "FPWidth", percent_value(current.fp_percent)});
        if (current.stamina_percent != g_initial.stamina_percent)
            changed.push_back({"BarSize", "StaminaWidth", percent_value(current.stamina_percent)});

        if (changed.empty()) {
            no_changes = true;
        } else if (!update_ini_values(g_ini_path, changed)) {
            failed = true;
        } else {
            g_initial = current;
            AcquireSRWLockExclusive(&g_settings_lock);
            g_current = current;
            ReleaseSRWLockExclusive(&g_settings_lock);
            saved = true;
        }
        if (saved) live_result = request_tarnished_hud_live_reload();
    } catch (...) {
        failed = true;
    }
    ReleaseSRWLockExclusive(&g_save_lock);

    if (saved) {
        if (live_result == LiveApplyResult::applied && !general_restart_required) {
            log_message(L"Changed TarnishedHUD.ini settings saved and live-applied; backup created.");
            show_alert(tr(L"Changes saved and applied in game. A .bak copy of the INI was created.",
                L"Cambios guardados y aplicados en el juego. Se creó una copia .bak del INI.",
                L"更改已保存并在游戏中生效。已创建 INI 的 .bak 备份。"));
        } else if (live_result == LiveApplyResult::applied) {
            log_message(L"Visual settings live-applied; General.Enabled still requires a restart.");
            show_alert(tr(L"Visual settings were applied now. The Tarnished HUD enabled switch still requires a restart. A .bak copy of the INI was created.",
                L"Los ajustes visuales se aplicaron ahora. El cambio de Tarnished HUD activado requiere reiniciar. Se creó una copia .bak del INI.",
                L"视觉设置已立即生效。Tarnished HUD 启用开关仍需重启游戏。已创建 INI 的 .bak 备份。"));
        } else if (live_result == LiveApplyResult::rejected) {
            log_message(L"Changed TarnishedHUD.ini was saved, but live reload validation failed; active settings were preserved.");
            show_alert(tr(L"The INI was saved, but Tarnished HUD rejected the reload. Active settings were preserved; check TarnishedHUD.log. Restart to apply the saved file.",
                L"El INI se guardó, pero Tarnished HUD rechazó la recarga. Los ajustes activos no cambiaron; revisa TarnishedHUD.log. Reinicia para aplicar el archivo guardado.",
                L"INI 已保存，但 Tarnished HUD 拒绝了重新加载。当前设置保持不变，请查看 TarnishedHUD.log。重启游戏以应用已保存的文件。"));
        } else {
            log_message(live_result == LiveApplyResult::timed_out
                ? L"Changed TarnishedHUD.ini was saved, but live reload did not complete before the timeout."
                : L"Changed TarnishedHUD.ini was saved, but this Tarnished HUD DLL does not expose the live-reload API.");
            show_alert(tr(L"The INI was saved, but this DLL did not confirm live application. Restart the game to apply the changes. A .bak copy of the INI was created.",
                L"El INI se guardó, pero este DLL no confirmó la aplicación inmediata. Reinicia el juego para aplicar los cambios. Se creó una copia .bak del INI.",
                L"INI 已保存，但此 DLL 未确认实时生效。请重启游戏以应用更改。已创建 INI 的 .bak 备份。"));
        }
    } else if (no_changes) {
        log_message(L"Save requested with no changed settings.");
        show_alert(tr(L"There are no changes to save.",
            L"No hay cambios pendientes.",
            L"没有需要保存的更改。"));
    } else if (failed) {
        log_message(L"Could not save TarnishedHUD.ini; original file was preserved.");
        show_alert(tr(L"Could not save TarnishedHUD.ini. The original file was preserved.",
            L"No se pudo guardar TarnishedHUD.ini. El archivo original se ha conservado.",
            L"无法保存 TarnishedHUD.ini。原文件已保留。"));
    }
}

void persist_fit_screen() noexcept {
    AcquireSRWLockExclusive(&g_save_lock);
    bool saved = false;
    try {
        saved = update_ini_values(
            g_ini_path, {IniEdit{"BarSize", "FitScreen", "1"}});
    } catch (...) {
        saved = false;
    }
    ReleaseSRWLockExclusive(&g_save_lock);

    log_message(saved
        ? L"Fit Screen mode saved to TarnishedHUD.ini; backup created."
        : L"Could not save Fit Screen mode; original TarnishedHUD.ini was preserved.");

    show_alert(saved
        ? (tr(L"Fit Screen was saved. Restart the game; percentage sliders will then be available.",
            L"Se guardó el ajuste a pantalla. Reinicia el juego; después aparecerán los deslizadores porcentuales.",
            L"适应屏幕已保存。请重启游戏，之后即可使用百分比滑块。"))
        : (tr(L"Could not save TarnishedHUD.ini. The original file was preserved.",
            L"No se pudo guardar TarnishedHUD.ini. El archivo original se ha conservado.",
            L"无法保存 TarnishedHUD.ini。原文件已保留。")));
}

bool encode_generic_ini_value(
    std::wstring_view value, std::string& output) noexcept {
    if (value.size() > ERUI_TEXT_INPUT_MAX_LENGTH ||
        value.find_first_of(L"\r\n") != std::wstring_view::npos ||
        value.find(L'\0') != std::wstring_view::npos ||
        value.find(L';') != std::wstring_view::npos ||
        value.find(L'#') != std::wstring_view::npos) return false;
    return wide_to_utf8(value, output);
}

DWORD WINAPI process_save_job(void* context) noexcept;

void persist_generic_ini(SaveJob& job) noexcept {
    bool saved = false;
    AcquireSRWLockExclusive(&g_save_lock);
    saved = update_ini_values(job.generic_path, job.generic_edits);
    ReleaseSRWLockExclusive(&g_save_lock);

    if (saved && job.generic_mod) {
        AcquireSRWLockExclusive(&g_generic_lock);
        try {
            for (const auto& item : job.generic_values) {
                if (item.first < job.generic_mod->settings.size()) {
                    job.generic_mod->settings[item.first].initial_value = item.second;
                }
            }
        } catch (...) {
            log_message(L"INI saved, but the menu baseline could not be refreshed.");
        }
        ReleaseSRWLockExclusive(&g_generic_lock);
    }

    if (!saved) {
        log_message(L"Could not save a discovered mod INI; its original file was preserved.");
        show_alert(tr(L"Could not save the INI. The original file was preserved.",
            L"No se pudo guardar el INI. El archivo original se ha conservado.",
            L"无法保存 INI。原文件已保留。"));
        return;
    }

    // Souls Vision and the like poll their own file: nothing to request.
    if (const BuiltinIni* builtin = builtin_ini_for(job.generic_path); builtin && builtin->reloads_itself) {
        log_message(L"A discovered mod config was saved; the mod reloads the file by itself; backup created.");
        show_alert(tr(L"Changes saved and applied in game (Requires restart settings: after restarting). A .bak copy was created.",
            L"Cambios guardados y aplicados en el juego (los de Requiere reiniciar, al reiniciar). Se creó una copia .bak.",
            L"更改已保存并在游戏中生效（“Requires restart”中的设置需重启后生效）。已创建 .bak 备份。"));
        return;
    }

    const GenericLiveApplyResult result = request_generic_live_reload(
        job.generic_module_path, job.generic_path);
    switch (result) {
    case GenericLiveApplyResult::applied:
        log_message(L"A discovered mod INI was saved and its DLL confirmed live application; backup created.");
        show_alert(tr(L"Changes saved and applied in game. A .bak copy of the INI was created.",
            L"Cambios guardados y aplicados en el juego. Se creó una copia .bak del INI.",
            L"更改已保存并在游戏中生效。已创建 INI 的 .bak 备份。"));
        break;
    case GenericLiveApplyResult::restart_required:
        log_message(L"A discovered mod INI was saved; its DLL reported that the changed settings require a restart.");
        show_alert(tr(L"The INI was saved, but the mod reported that these settings require a restart. A .bak copy was created.",
            L"El INI se guardó, pero el mod indicó que estos ajustes requieren reiniciar. Se creó una copia .bak.",
            L"INI 已保存，但模组表示这些设置需要重启游戏。已创建 .bak 备份。"));
        break;
    case GenericLiveApplyResult::rejected:
        log_message(L"A discovered mod INI was saved, but its DLL rejected live application; active settings were not confirmed changed.");
        show_alert(tr(L"The INI was saved, but the mod rejected live reload. Check the mod log; active settings were not confirmed changed.",
            L"El INI se guardó, pero el mod rechazó la recarga en vivo. Revisa el log del mod; los ajustes activos no se confirmaron.",
            L"INI 已保存，但模组拒绝了实时重载。请查看模组日志，当前设置未确认更改。"));
        break;
    case GenericLiveApplyResult::timed_out:
        log_message(L"A discovered mod INI was saved, but live application did not finish before the Hub timeout.");
        show_alert(tr(L"The INI was saved, but the mod did not confirm application in time. Restart if the changes do not appear.",
            L"El INI se guardó, pero el mod no confirmó la aplicación a tiempo. Reinicia el juego si los cambios no aparecen.",
            L"INI 已保存，但模组未及时确认生效。如果更改没有出现，请重启游戏。"));
        break;
    case GenericLiveApplyResult::unsupported:
        log_message(L"A discovered mod INI was saved; its DLL does not expose the NativeTweaks live-reload API.");
        show_alert(tr(L"INI saved. This mod has no live reload: restart the game to apply.",
            L"INI guardado. Este mod no tiene recarga en vivo: reinicia el juego para aplicarlo.",
            L"INI 已保存，但此 DLL 尚不支持实时重载。需要重启游戏以应用更改。"));
        break;
    default:
        log_message(L"A discovered mod INI was saved, but its live-reload request failed.");
        show_alert(tr(L"The INI was saved, but the reload request failed. Check the mod log or restart the game.",
            L"El INI se guardó, pero falló la solicitud de recarga. Revisa el log del mod o reinicia el juego.",
            L"INI 已保存，但重载请求失败。请查看模组日志或重启游戏。"));
        break;
    }
}

bool start_save_job(SaveJob* job) noexcept {
    if (!job) return false;
    HANDLE thread = CreateThread(nullptr, 0, &process_save_job, job, 0, nullptr);
    if (!thread) {
        delete job;
        log_message(L"Could not start the settings-save worker.");
        return false;
    }
    CloseHandle(thread);
    return true;
}

bool queue_generic_ini_save(GenericIniMod& mod) noexcept {
    SaveJob* job = new (std::nothrow) SaveJob{};
    if (!job) return false;
    job->kind = SaveJobKind::generic_ini;
    job->generic_mod = &mod;
    try {
        job->generic_path = mod.path;
        job->generic_module_path = mod.module_path;
    } catch (...) {
        delete job;
        return false;
    }
    AcquireSRWLockShared(&g_generic_lock);
    try {
        for (std::size_t index = 0; index < mod.settings.size(); ++index) {
            const GenericIniSetting& setting = mod.settings[index];
            if (setting.current_value == setting.initial_value) continue;
            std::string encoded{};
            if (!encode_generic_ini_value(setting.current_value, encoded)) {
                ReleaseSRWLockShared(&g_generic_lock);
                delete job;
                show_alert(tr(L"One value cannot be saved safely. Remove ;, #, and line breaks, then try again.",
                    L"Uno de los valores no se puede guardar de forma segura. Quita ;, # y saltos de línea y vuelve a intentarlo.",
                    L"有一个值无法安全保存。请删除 ;、# 和换行后重试。"));
                return true;
            }
            job->generic_edits.push_back({setting.section, setting.key, std::move(encoded)});
            job->generic_values.emplace_back(index, setting.current_value);
        }
        ReleaseSRWLockShared(&g_generic_lock);
    } catch (...) {
        ReleaseSRWLockShared(&g_generic_lock);
        delete job;
        return false;
    }

    if (job->generic_edits.empty()) {
        delete job;
        show_alert(tr(L"There are no changes to save.",
            L"No hay cambios pendientes.",
            L"没有需要保存的更改。"));
        return true;
    }
    return start_save_job(job);
}


// Toggle/choice report an index into tokens; a slider reports its real value.
void ERUI_CALL generic_value_changed(void* target, std::uint8_t value) noexcept {
    if (!target) return;
    auto* setting = static_cast<GenericIniSetting*>(target);
    AcquireSRWLockExclusive(&g_generic_lock);
    try {
        if (setting->control == GenericControl::slider) {
            setting->current_value = std::to_wstring(value);
        } else if (value < setting->tokens.size()) {
            setting->current_value = setting->tokens[value];
        }
    } catch (...) {
        log_message(L"Could not retain a generic INI value change.");
    }
    ReleaseSRWLockExclusive(&g_generic_lock);
}

void ERUI_CALL generic_save_clicked(void* target) noexcept {
    if (!target) return;
    if (!queue_generic_ini_save(*static_cast<GenericIniMod*>(target))) {
        log_message(L"Could not queue saving a discovered mod INI.");
        show_alert(tr(L"Could not start saving. No files were changed.",
            L"No se pudo iniciar el guardado. No se modificó ningún archivo.",
            L"无法开始保存。未修改任何文件。"));
    }
}

DWORD WINAPI process_save_job(void* context) noexcept {
    std::unique_ptr<SaveJob> job(static_cast<SaveJob*>(context));
    if (!job) return 1;
    if (job->kind == SaveJobKind::settings) {
        persist_settings(job->current);
    } else if (job->kind == SaveJobKind::fit_screen) {
        persist_fit_screen();
    } else {
        persist_generic_ini(*job);
    }
    return 0;
}

bool queue_save_job(SaveJobKind kind, const Settings* current = nullptr) noexcept {
    SaveJob* job = new (std::nothrow) SaveJob{};
    if (!job) return false;
    job->kind = kind;
    if (current) job->current = *current;

    return start_save_job(job);
}

void save_settings() noexcept {
    Settings current{};
    if (!collect_current_values(current)) {
        log_message(L"Could not read current native menu values; nothing was saved.");
        show_alert(tr(L"Could not read the current menu values. No settings were saved.",
            L"No se pudieron leer los valores del menú. No se guardó nada.",
            L"无法读取当前菜单数值。未保存任何设置。"));
        return;
    }
    if (!queue_save_job(SaveJobKind::settings, &current)) {
        log_message(L"Could not queue the settings save; TarnishedHUD.ini was not changed.");
        show_alert(tr(L"Could not start the save operation. The file was not changed.",
            L"No se pudo iniciar el guardado. No se modificó el archivo.",
            L"无法开始保存操作。文件未被修改。"));
    }
}

void fit_screen_confirmed(
    void*, ERUI_Result result, erui::AlertResponse response) noexcept {
    if (result != ERUI_OK || response != erui::AlertResponse::primary) return;
    if (!queue_save_job(SaveJobKind::fit_screen)) {
        log_message(L"Could not queue the Fit Screen save; TarnishedHUD.ini was not changed.");
        show_alert(tr(L"Could not start the save operation. The file was not changed.",
            L"No se pudo iniciar el guardado. No se modificó el archivo.",
            L"无法开始保存操作。文件未被修改。"));
    }
}

void enable_fit_screen() noexcept {
    if (!g_fit_screen_values_safe) {
        show_alert(tr(L"One or more widths exceed 1.00. Adjust them in the INI before enabling percentage mode; no file was changed.",
            L"Una o más anchuras superan 1.00. Ajústalas primero en el INI para activar el modo porcentual; no se ha cambiado ningún archivo.",
            L"有宽度值超过 1.00。请先在 INI 中调整，再启用百分比模式。未修改任何文件。"));
        return;
    }
    erui::AlertOptions options{};
    options.buttons = erui::AlertButtons::yes_no;
    options.placement = erui::AlertPlacement::bottom;
    const std::wstring_view prompt = tr(L"Fit Screen changes width values to percentages of the calibrated bar length. Save this change?",
        L"Ajustar a pantalla cambiará el significado de las anchuras a porcentajes de la longitud calibrada. ¿Guardar el cambio?",
        L"适应屏幕会将宽度值改为校准长度的百分比。要保存此更改吗？");
    (void)g_registration.alert(prompt, options, &fit_screen_confirmed);
}

void unavailable_message() noexcept {
    show_alert(tr(L"Tarnished HUD is unavailable in this profile. Other discovered INI files remain available here.",
        L"Tarnished HUD no está disponible en este perfil. Los demás INI detectados siguen apareciendo aquí.",
        L"此配置中 Tarnished HUD 不可用。其他已检测到的 INI 仍可在此使用。"));
}

void no_mods_message() noexcept {
    show_alert(tr(L"No loaded mod with a configurable INI was found. Install a mod with its INI beside its DLL and restart the game.",
        L"No se encontró ningún mod cargado con un INI configurable. Instala un mod con su INI junto a su DLL y reinicia el juego.",
        L"未找到带有可配置 INI 的已加载模组。请将模组的 INI 放在其 DLL 旁边，然后重启游戏。"));
}

// The menu is built once from the DLLs loaded during startup discovery. A mod
// DLL that a loader injects later gets no page; say so in the log, with the
// setting that waits longer, instead of leaving it unexplained.
std::size_t g_discovered_module_count{};

DWORD WINAPI late_module_check(void*) noexcept {
    try {
        Sleep(30000);
        std::vector<std::wstring> modules;
        if (!snapshot_loaded_mod_modules(modules)) return 0;
        const bool hud_late = !g_target_available && GetModuleHandleW(L"TarnishedHUD.dll");
        if (hud_late || modules.size() > g_discovered_module_count) {
            wchar_t message[320]{};
            swprintf_s(message, std::size(message),
                L"WARNING: %zu mod DLL(s) loaded after startup discovery (%zu -> %zu)%ls. "
                L"They have no page until the next start; if this repeats, raise [Discovery] "
                L"StartupWaitMs / StableModuleMs in NativeTweaks.ini.",
                modules.size() - std::min(modules.size(), g_discovered_module_count),
                g_discovered_module_count, modules.size(),
                hud_late ? L", including TarnishedHUD.dll" : L"");
            log_message(message);
        }
    } catch (...) {
    }
    return 0;
}

DWORD WINAPI initialize_mod(void*) noexcept {
    try {
        initialize_logging();
        log_message((std::wstring(L"NativeTweaks initialization started (build ") + kBuildTag + L", vanilla settings, weapon effects, hotkeys, vanilla profiles, INI templates, lang files, profiles, en/es/zh/zht).").c_str());
        vanilla::initialize(g_config_path, &log_message);
        if (GetModuleHandleW(L"ERNativeUI.dll")) {
            log_message(L"ERNativeUI.dll is present in the game process.");
        } else {
            log_message(L"ERNativeUI.dll is not present in the game process.");
        }

        const auto connection = erui::connect();
        if (!connection) {
            log_message(connection.error().message().c_str());
            return 1;
        }
        log_message(L"Connected to the ERNativeUI host API.");
        // NativeTweaks may have started before ERNativeUI.dll: send the saved
        // layout now, before the game opens the HUD and menu movies.
        vanilla::sync_hud_layout(L"host connected");

        wait_for_startup_discovery_window();

        // [General] Language = auto | en | es | zh | zht (forced menu language).
        wchar_t forced[16]{};
        const wchar_t* default_language = L"auto";
        if (g_config_path.empty()) wcscpy_s(forced, default_language);
        else GetPrivateProfileStringW(L"General", L"Language", default_language,
            forced, static_cast<DWORD>(std::size(forced)), g_config_path.c_str());

        const erui::LanguageInfo language = connection.value().game_language();
        if (language.known == erui::GameLanguage::spanish ||
            language.known == erui::GameLanguage::spanish_latin_america) {
            g_game_language = Language::spanish;
        } else if (language.known == erui::GameLanguage::chinese_simplified) {
            g_game_language = Language::chinese;
        } else if (language.known == erui::GameLanguage::chinese_traditional) {
            g_game_language = Language::chinese_traditional;
        } else {
            g_game_language = Language::english;
        }
        g_language_choice = language_choice_from_setting(forced);
        g_language = resolve_language_choice(g_language_choice);
        g_texts = &texts_for(g_language);
        if (language.available()) g_steam_language = language.identifier;
        // Still before the menu freezes: the host builds its texts in the
        // forced language. Auto already matches the host's Steam language.
        if (g_language_choice != 0) sync_host_language(L"host connected");
        const std::string code = template_language();
        log_message((L"Menu language: " + std::wstring(code.begin(), code.end()) +
            L" (setting: " + forced + L").").c_str());

        g_target_available = resolve_tarnished_hud_ini();
        if (g_target_available) {
            g_config_valid = read_tarnished_hud_settings(g_current);
            if (g_config_valid) {
                g_initial = g_current;
                log_message(L"TarnishedHUD.ini settings parsed successfully.");
            } else {
                log_message(L"TarnishedHUD.ini exists but expected settings could not be parsed.");
            }
        }
        discover_generic_ini_mods();
        {
            std::vector<std::wstring> modules;
            if (snapshot_loaded_mod_modules(modules)) {
                g_discovered_module_count = modules.size();
                if (const HANDLE late = CreateThread(nullptr, 0, &late_module_check, nullptr, 0, nullptr)) {
                    CloseHandle(late);
                }
            }
        }

        erui::ProviderOptions options{};
        options.provider_id = "er-mod-settings-hub";
        options.display_name = g_texts->provider;
        options.owner_module = g_own_module;
        options.root_priority = 1000;

        const auto registration = connection.value().register_menu(
            options, [](erui::Menu& menu) {
            // The native tab host only enables category 4 when the provider
            // contributes rows to the built-in Mod Settings page. Keep each
            // mod's settings as a submenu within that page; do not add a
            // duplicate Mods row to Game Options.
            // ERUI 1.3: one line with the version on the title screen, so a
            // player (or a bug report screenshot) shows which build is loaded.
            // [General] ShowOnTitleScreen = 0 hides it; restart to apply.
            if (menu.supports(erui::Capability::title_screen_text) &&
                (g_config_path.empty() ||
                    GetPrivateProfileIntW(L"General", L"ShowOnTitleScreen", 1,
                        g_config_path.c_str()) != 0)) {
                const std::wstring line = std::wstring(L"NativeTweaks ") + kBuildTag;
                menu.add_title_screen_text(
                    erui::TitleScreenCorner::bottom_left, {line});
                // A warning the player would otherwise only find in the log.
                // Known before the menu is built, so it fits the title text,
                // which cannot change once published.
                if (!game::layout_supported()) {
                    const Language language = g_language.load(std::memory_order_relaxed);
                    menu.add_title_screen_text(erui::TitleScreenCorner::bottom_left, {
                        tr_in(language,
                            L"Game version not supported by NativeTweaks: most options are disabled",
                            L"Versión del juego no compatible con NativeTweaks: casi todas las opciones están desactivadas",
                            L"NativeTweaks 不支持此游戏版本：大部分选项已禁用"),
                        erui::Color{216, 167, 103}});
                }
            }

            auto mods = menu.page(erui::BuiltinPage::mod_settings);
            if (!mods.valid()) {
                log_message(L"The ERNativeUI host did not provide the native Mod Settings page.");
                return;
            }

            add_language_row(mods);
            vanilla::add_pages(mods, template_language());

            if (!g_target_available || !g_config_valid) {
                // Only mention Tarnished HUD when it is loaded but its INI is
                // unusable; otherwise the page stays empty except for a
                // placeholder that keeps the native tab enabled.
                if (g_target_available) {
                    track_texts(mods.add_button<&unavailable_message>(
                        g_texts->unavailable, g_texts->unavailable_help),
                        &Texts::unavailable, &Texts::unavailable_help);
                } else if (g_generic_mods.empty()) {
                    track_texts(mods.add_button<&no_mods_message>(
                        g_texts->no_mods, g_texts->no_mods_help),
                        &Texts::no_mods, &Texts::no_mods_help);
                }
            } else {
                auto hud = mods.add_submenu(
                    g_texts->hud, g_texts->hud_help,
                    g_texts->provider, g_texts->hud);
                track_submenu_texts(hud, &Texts::hud, &Texts::hud_help,
                    &Texts::provider, &Texts::hud);

                if (g_current.fit_screen) {
                    const auto add_width = [&](TextField label,
                        TextField help, std::uint8_t& value,
                        erui::RowHandle& row) {
                        erui::SliderOptions slider{};
                        slider.minimum = 1;
                        slider.maximum = 100;
                        slider.step = 1;
                        slider.initial_value = value;
                        row = hud.add_slider(g_texts->*label, g_texts->*help, slider,
                            &setting_changed, &value);
                        track_texts(row, label, help);
                    };
                    add_width(&Texts::hp_width, &Texts::width_help,
                        g_current.hp_percent, g_rows.hp_percent);
                    add_width(&Texts::fp_width, &Texts::width_help,
                        g_current.fp_percent, g_rows.fp_percent);
                    add_width(&Texts::stamina_width, &Texts::width_help,
                        g_current.stamina_percent, g_rows.stamina_percent);
                } else {
                    track_texts(hud.add_button<&enable_fit_screen>(
                        g_texts->fit_screen_button, g_texts->fit_screen_button_help),
                        &Texts::fit_screen_button, &Texts::fit_screen_button_help);
                }

                const auto add_hud_toggle = [&](TextField label, TextField help,
                    std::uint8_t& value, erui::RowHandle& row) {
                    row = hud.add_toggle(g_texts->*label, g_texts->*help,
                        value, &setting_changed, &value);
                    track_texts(row, label, help);
                };
                add_hud_toggle(&Texts::general_enabled, &Texts::general_help,
                    g_current.general_enabled, g_rows.general_enabled);
                add_hud_toggle(&Texts::bar_size_enabled, &Texts::bar_size_help,
                    g_current.bar_size_enabled, g_rows.bar_size_enabled);
                add_hud_toggle(&Texts::hp_enabled, &Texts::bar_toggle_help,
                    g_current.hp_enabled, g_rows.hp_enabled);
                add_hud_toggle(&Texts::fp_enabled, &Texts::bar_toggle_help,
                    g_current.fp_enabled, g_rows.fp_enabled);
                add_hud_toggle(&Texts::stamina_enabled, &Texts::bar_toggle_help,
                    g_current.stamina_enabled, g_rows.stamina_enabled);

                track_texts(hud.add_button<&save_settings>(g_texts->save, g_texts->save_help),
                    &Texts::save, &Texts::save_help);
            }

            add_generic_ini_pages(mods);
        });

        if (!registration) {
            log_message(registration.error().message().c_str());
            return 1;
        }

        g_registration = registration.value();
        vanilla::attach_registration(&g_registration);
        log_message(L"Mod settings registered on the dedicated native Mod Settings page.");
        return 0;
    } catch (...) {
        log_message(L"Initialization failed without changing game files.");
        return 1;
    }
}

} // namespace

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        g_own_module = module;
        DisableThreadLibraryCalls(module);
        if (HANDLE thread = CreateThread(
            nullptr, 0, &initialize_mod, nullptr, 0, nullptr)) {
            CloseHandle(thread);
        } else {
            log_message(L"Could not create the initialization worker.");
        }
    }
    return TRUE;
}

const wchar_t* vanilla::translate_text(std::string_view lang, const wchar_t* english,
    const wchar_t* fallback) noexcept {
    return translated(language_from_code(lang), english, fallback);
}
