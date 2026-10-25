// nativetweaks_live_reload.hpp - live reload for mods whose INI appears in NativeTweaks.
//
// Header-only helper over the live-reload ABI v1 (erms_live_reload_v1.h). When the
// player presses "Save changes" on your mod's page, NativeTweaks writes your INI and
// then calls your DLL, so the new values apply without restarting the game.
//
// Usage (in exactly ONE .cpp of your DLL):
//
//     #define NATIVETWEAKS_LIVE_RELOAD_IMPLEMENTATION
//     #include "nativetweaks_live_reload.hpp"
//
//     nativetweaks::ReloadResult nativetweaks::on_reload(const wchar_t* ini_path) {
//         Config next;                              // parse into a NEW object
//         if (!load_config(ini_path, next)) return ReloadResult::rejected;
//         g_config.publish(std::move(next));        // swap it in atomically
//         return ReloadResult::applied;
//     }
//
// Other .cpp files may include the header without the define to use the helpers.
// Rules: never edit the live config field by field, never unload your DLL, and
// return restart_required for values that cannot change mid-session.
// MIT License - see NativeTweaks licenses.

#ifndef NATIVETWEAKS_LIVE_RELOAD_HPP
#define NATIVETWEAKS_LIVE_RELOAD_HPP

#ifndef NOMINMAX
#define NOMINMAX  // keep std::min/std::max usable in your code
#endif
#include <Windows.h>

#include <cstdint>
#include <cwchar>
#include <cwctype>
#include <memory>
#include <mutex>
#include <string>
#include <utility>

namespace nativetweaks {

enum class ReloadResult : std::uint32_t {
    applied = 1,           // new values are active
    rejected = 2,          // invalid INI; the old values stay active
    restart_required = 3,  // saved, but takes effect after a restart
    failed = 4             // unexpected error
};

// Implement this once in your DLL. Runs on NativeTweaks' save worker thread (not the
// menu or render thread), right after the INI was saved. Keep it short (< 5 s).
ReloadResult on_reload(const wchar_t* ini_path);

// Holds the active configuration. Readers get a complete, immutable snapshot; a
// reload publishes a whole new one, so gameplay code never sees half an update.
//
//     nativetweaks::Live<Config> g_config;
//     auto config = g_config.get();  if (config->enabled) ...
//
// get() is cheap (a short lock + refcount), but in per-frame hooks call it once
// per frame/call and read every field from that snapshot.
template <class T>
class Live {
public:
    Live() : current_(std::make_shared<const T>()) {}
    explicit Live(T initial) : current_(std::make_shared<const T>(std::move(initial))) {}

    std::shared_ptr<const T> get() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return current_;
    }
    void publish(T next) {
        auto snapshot = std::make_shared<const T>(std::move(next));
        std::lock_guard<std::mutex> lock(mutex_);
        current_ = std::move(snapshot);
    }

private:
    mutable std::mutex mutex_;
    std::shared_ptr<const T> current_;
};

// Small INI readers (Win32 GetPrivateProfile*) for use inside on_reload.
inline std::wstring ini_string(const wchar_t* path, const wchar_t* section, const wchar_t* key,
    const wchar_t* fallback = L"") {
    wchar_t buffer[1024]{};
    GetPrivateProfileStringW(section, key, fallback, buffer, 1024, path);
    return buffer;
}

inline bool ini_number(const wchar_t* path, const wchar_t* section, const wchar_t* key,
    double& output) {
    const std::wstring text = ini_string(path, section, key);
    if (text.empty()) return false;
    wchar_t* end = nullptr;
    const double value = std::wcstod(text.c_str(), &end);
    while (end && (*end == L' ' || *end == L'\t')) ++end;
    if (!end || *end != L'\0') return false;
    output = value;
    return true;
}

inline bool ini_bool(const wchar_t* path, const wchar_t* section, const wchar_t* key,
    bool& output) {
    std::wstring text = ini_string(path, section, key);
    for (auto& ch : text) ch = static_cast<wchar_t>(towlower(ch));
    if (text == L"1" || text == L"true" || text == L"on" || text == L"yes") { output = true; return true; }
    if (text == L"0" || text == L"false" || text == L"off" || text == L"no") { output = false; return true; }
    return false;
}

// True when ini_path names "<this DLL's name>.ini" (MO2 may report the game-folder
// path while your DLL loads from mods\..., so only the file name is compared).
inline bool is_own_ini(const wchar_t* ini_path) {
    HMODULE self = nullptr;
    if (!ini_path || !GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
            GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            reinterpret_cast<LPCWSTR>(&is_own_ini), &self)) return false;
    std::wstring expected(32768, L'\0');  // MO2 mod paths can exceed MAX_PATH
    const DWORD length = GetModuleFileNameW(self, expected.data(),
        static_cast<DWORD>(expected.size()));
    if (length == 0 || length >= expected.size()) return false;
    expected.resize(length);
    expected = expected.substr(expected.find_last_of(L"\\/") + 1);
    expected = expected.substr(0, expected.find_last_of(L'.')) + L".ini";
    std::wstring given = ini_path;
    given = given.substr(given.find_last_of(L"\\/") + 1);
    return CompareStringOrdinal(expected.c_str(), -1, given.c_str(), -1, TRUE) == CSTR_EQUAL;
}

}  // namespace nativetweaks

#ifdef NATIVETWEAKS_LIVE_RELOAD_IMPLEMENTATION

namespace nativetweaks::detail {
// The last few request results stay queryable (NativeTweaks polls for up to 5 s).
inline std::mutex g_requests_mutex;
inline std::uint32_t g_next_request = 1;
inline std::pair<std::uint32_t, std::uint32_t> g_requests[16]{};
}  // namespace nativetweaks::detail

extern "C" __declspec(dllexport) std::uint32_t __cdecl
NativeTweaks_RequestReloadV1(const wchar_t* ini_path) {
    using namespace nativetweaks;
    ReloadResult result = ReloadResult::failed;
    try {
        result = is_own_ini(ini_path) ? on_reload(ini_path) : ReloadResult::rejected;
    } catch (...) {
        result = ReloadResult::failed;
    }
    std::lock_guard<std::mutex> lock(detail::g_requests_mutex);
    std::uint32_t id = detail::g_next_request++;
    if (id == 0) id = detail::g_next_request++;
    detail::g_requests[id % 16] = {id, static_cast<std::uint32_t>(result)};
    return id;
}

extern "C" __declspec(dllexport) std::uint32_t __cdecl
NativeTweaks_GetReloadStatusV1(std::uint32_t request_id) {
    std::lock_guard<std::mutex> lock(nativetweaks::detail::g_requests_mutex);
    const auto& entry = nativetweaks::detail::g_requests[request_id % 16];
    return entry.first == request_id && request_id != 0 ? entry.second
        : static_cast<std::uint32_t>(nativetweaks::ReloadResult::failed);
}

#endif  // NATIVETWEAKS_LIVE_RELOAD_IMPLEMENTATION
#endif  // NATIVETWEAKS_LIVE_RELOAD_HPP
