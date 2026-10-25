#include "live_text.hpp"

#include <Windows.h>

#include <vector>

namespace live_text {
namespace {

using SetRowText = ERUI_Result (ERUI_CALL*)(ERUI_ProviderHandle, ERUI_RowHandle,
    const ERUI_Utf16View*, const ERUI_Utf16View*);
using SetSubmenuText = ERUI_Result (ERUI_CALL*)(ERUI_ProviderHandle, ERUI_PageHandle,
    const ERUI_Utf16View*, const ERUI_Utf16View*, const ERUI_Utf16View*, const ERUI_Utf16View*);
using SetCustomSliderValues = ERUI_Result (ERUI_CALL*)(ERUI_ProviderHandle, ERUI_RowHandle,
    const ERUI_Utf16View*, std::uint32_t);

struct Entry {
    bool submenu{};
    ERUI_RowHandle row{};
    ERUI_PageHandle page{};
    Producer producer;
};

std::vector<Entry> g_entries{};
SRWLOCK g_lock = SRWLOCK_INIT;

template <class Function>
Function fork_export(const char* name) noexcept {
    const HMODULE host = GetModuleHandleW(L"ERNativeUI.dll");
    return host ? reinterpret_cast<Function>(GetProcAddress(host, name)) : nullptr;
}

ERUI_Utf16View view(const std::wstring& text) noexcept {
    ERUI_Utf16View result{};
    result.data = reinterpret_cast<const std::uint16_t*>(text.c_str());
    result.length = static_cast<std::uint32_t>(text.size());
    return result;
}

void add(Entry entry) {
    AcquireSRWLockExclusive(&g_lock);
    try {
        g_entries.push_back(std::move(entry));
    } catch (...) {
    }
    ReleaseSRWLockExclusive(&g_lock);
}

} // namespace

void track_row(ERUI_RowHandle row, Producer producer) {
    if (row == ERUI_INVALID_ROW || !producer) return;
    add({false, row, ERUI_INVALID_PAGE, std::move(producer)});
}

void track_submenu(ERUI_PageHandle page, Producer producer) {
    if (page == ERUI_INVALID_PAGE || !producer) return;
    add({true, ERUI_INVALID_ROW, page, std::move(producer)});
}

bool supported() noexcept {
    return fork_export<SetRowText>("ERNativeUIFork_SetRowText") &&
        fork_export<SetSubmenuText>("ERNativeUIFork_SetSubmenuText");
}

std::size_t apply(ERUI_ProviderHandle provider, const char* language) noexcept {
    const auto set_row = fork_export<SetRowText>("ERNativeUIFork_SetRowText");
    const auto set_submenu = fork_export<SetSubmenuText>("ERNativeUIFork_SetSubmenuText");
    if (!set_row || !set_submenu) return static_cast<std::size_t>(-1);
    // Newer fork export (r108): an older fork keeps the value names it started with.
    const auto set_values = fork_export<SetCustomSliderValues>("ERNativeUIFork_SetCustomSliderValues");

    // Build every text under the lock, then call the host without it: the host
    // takes its own registry lock, and holding ours across that call could
    // invert the lock order with a row being tracked during registration.
    struct Update {
        bool submenu;
        ERUI_RowHandle row;
        ERUI_PageHandle page;
        Texts texts;
    };
    std::vector<Update> updates;
    std::size_t failures{};
    AcquireSRWLockShared(&g_lock);
    try {
        updates.reserve(g_entries.size());
        for (const Entry& entry : g_entries) {
            try {
                updates.push_back({entry.submenu, entry.row, entry.page, entry.producer(language)});
            } catch (...) {
                ++failures;
            }
        }
    } catch (...) {
        ReleaseSRWLockShared(&g_lock);
        return static_cast<std::size_t>(-1);
    }
    ReleaseSRWLockShared(&g_lock);

    for (const Update& update : updates) {
        const Texts& texts = update.texts;
        const ERUI_Utf16View label = view(texts.label);
        const ERUI_Utf16View help = view(texts.help);
        ERUI_Result result{};
        if (update.submenu) {
            const ERUI_Utf16View title = view(texts.page_title);
            const ERUI_Utf16View page_help = view(texts.page_help);
            result = set_submenu(provider, update.page, &label, &help,
                texts.page_title.empty() ? nullptr : &title,
                texts.page_help.empty() ? nullptr : &page_help);
        } else {
            result = set_row(provider, update.row, &label, &help);
            if (result == ERUI_OK && set_values && !texts.values.empty()) {
                std::vector<ERUI_Utf16View> views;
                try {
                    views.reserve(texts.values.size());
                    for (const std::wstring& value : texts.values) views.push_back(view(value));
                    result = set_values(provider, update.row, views.data(),
                        static_cast<std::uint32_t>(views.size()));
                } catch (...) {
                    result = ERUI_OUT_OF_MEMORY;
                }
            }
        }
        if (result != ERUI_OK) ++failures;
    }
    return failures;
}

} // namespace live_text
