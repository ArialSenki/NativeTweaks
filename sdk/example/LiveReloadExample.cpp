// LiveReloadExample.dll - minimal mod showing NativeTweaks live reload.
// Build (x64 Developer Command Prompt):
//   cl /nologo /LD /O2 /std:c++17 /EHsc /utf-8 /I ..\include LiveReloadExample.cpp
//   (from the source repo instead of the release zip: /I ..\..\include)
// Install LiveReloadExample.dll + LiveReloadExample.ini as an external DLL.
// Change a value in NativeTweaks, press "Save changes": the new values are
// written to LiveReloadExample.log at once, without restarting the game.

#define NATIVETWEAKS_LIVE_RELOAD_IMPLEMENTATION
#include "nativetweaks_live_reload.hpp"

#include <cstdio>

namespace {

struct Config {
    bool enabled = true;
    double speed = 1.0;
    int quality = 1;
};

nativetweaks::Live<Config> g_config;  // what the mod's own code reads
HMODULE g_module = nullptr;

// Parses the whole INI into `output`; false when a value is invalid.
bool load_config(const wchar_t* path, Config& output) {
    Config next;
    double quality = 0;
    if (!nativetweaks::ini_bool(path, L"Example", L"Enabled", next.enabled)) return false;
    if (!nativetweaks::ini_number(path, L"Example", L"Speed", next.speed) ||
        next.speed < 0.5 || next.speed > 2.0) return false;
    if (!nativetweaks::ini_number(path, L"Example", L"Quality", quality) ||
        quality < 0 || quality > 2) return false;
    next.quality = static_cast<int>(quality);
    output = next;
    return true;
}

std::wstring sibling(const wchar_t* extension) {
    wchar_t path[MAX_PATH]{};
    GetModuleFileNameW(g_module, path, MAX_PATH);
    std::wstring result = path;
    return result.substr(0, result.find_last_of(L'.')) + extension;
}

void log_config(const wchar_t* reason) {
    const auto config = g_config.get();
    FILE* file = nullptr;
    if (_wfopen_s(&file, sibling(L".log").c_str(), L"a, ccs=UTF-8") != 0 || !file) return;
    std::fwprintf(file, L"%ls: Enabled=%d Speed=%.2f Quality=%d\n", reason,
        config->enabled ? 1 : 0, config->speed, config->quality);
    std::fclose(file);
}

}  // namespace

nativetweaks::ReloadResult nativetweaks::on_reload(const wchar_t* ini_path) {
    Config next;
    if (!load_config(ini_path, next)) return ReloadResult::rejected;  // keep old values
    g_config.publish(next);
    log_config(L"Live reload");
    return ReloadResult::applied;
}

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        g_module = instance;
        DisableThreadLibraryCalls(instance);
        // Reading a small INI here keeps the example short; a real mod usually
        // loads its config from its own init thread or hook setup instead.
        Config initial;
        if (load_config(sibling(L".ini").c_str(), initial)) g_config.publish(initial);
        log_config(L"Startup");
    }
    return TRUE;
}
