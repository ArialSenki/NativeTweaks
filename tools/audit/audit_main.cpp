// NativeTweaksAudit: checks, without starting the game, which Vanilla Settings
// options would work on an eldenring.exe. Run it after a game update:
//
//   NativeTweaksAudit.exe "C:\...\ELDEN RING\Game\eldenring.exe"
//
// The executable is mapped as an image (no code runs); every pattern scan and
// static lookup is the same code NativeTweaks uses in the game.

#include "../../src/game_memory.hpp"
#include "../../src/vanilla_settings.hpp"

#include <Windows.h>

#include <cstdio>

namespace {

void print(const wchar_t* message) noexcept {
    std::wprintf(L"%ls\n", message);
}

} // namespace

// native_tweaks.cpp normally provides this; the audit has no menu texts.
const wchar_t* vanilla::translate_text(std::string_view, const wchar_t*, const wchar_t* fallback) noexcept {
    return fallback;
}

int wmain(int argc, wchar_t** argv) {
    const wchar_t* path = argc > 1 ? argv[1]
        : L"C:\\Program Files (x86)\\Steam\\steamapps\\common\\ELDEN RING\\Game\\eldenring.exe";
    HMODULE image = LoadLibraryExW(path, nullptr, DONT_RESOLVE_DLL_REFERENCES);
    if (!image) {
        std::wprintf(L"Could not map %ls (error %lu).\n", path, GetLastError());
        return 2;
    }
    std::wprintf(L"Auditing %ls\n", path);
    game::set_image(image);
    const int unavailable = vanilla::audit(&print);
    FreeLibrary(image);
    return unavailable == 0 ? 0 : 1;
}
