// Offline tests for the [Hotkeys] parser. Build with tests\build_tests.cmd.

#include "../src/hotkeys.hpp"

#include <Windows.h>

#include <cstdio>
#include <fstream>
#include <string>

namespace {

int g_failures = 0;

void check(bool condition, const char* what) {
    std::printf("%s %s\n", condition ? "ok  " : "FAIL", what);
    if (!condition) ++g_failures;
}

} // namespace

int main() {
    using namespace hotkeys;

    auto c = parse_chord(L"F1");
    check(c && c->key == VK_F1 && c->modifiers == mod_none, "F1");
    c = parse_chord(L" ctrl + shift + f12 ");
    check(c && c->key == VK_F12 && c->modifiers == (mod_ctrl | mod_shift), "ctrl+shift+f12 with spaces");
    c = parse_chord(L"Alt+K");
    check(c && c->key == 'K' && c->modifiers == mod_alt, "Alt+K");
    c = parse_chord(L"Num5");
    check(c && c->key == VK_NUMPAD5, "Num5");
    c = parse_chord(L"Ctrl+Plus");
    check(c && c->key == VK_OEM_PLUS && c->modifiers == mod_ctrl, "Ctrl+Plus");
    check(!parse_chord(L"Ctrl"), "modifier alone is rejected");
    check(!parse_chord(L"F1+F2"), "two keys are rejected");
    check(!parse_chord(L"Hyper+F1"), "unknown token is rejected");
    check(!parse_chord(L""), "empty is rejected");
    check(chord_text(*parse_chord(L"shift+ctrl+pgup")) == L"Ctrl+Shift+PageUp", "normalised text");

    auto b = parse_binding(L"GodMode", L"F1");
    check(b && b->kind == ActionKind::toggle && b->target == L"GodMode", "toggle binding");
    b = parse_binding(L"SpeedGlobalPercent", L"Ctrl+F5 > 200");
    check(b && b->kind == ActionKind::set_value && b->value == 200 && b->text == L"Ctrl+F5", "set binding");
    b = parse_binding(L"FovPercent", L"F6 > +10");
    check(b && b->kind == ActionKind::step_value && b->value == 10, "step up binding");
    b = parse_binding(L"FovPercent", L"F7 > -10 ; zoom out");
    check(b && b->kind == ActionKind::step_value && b->value == -10, "step down with comment");
    b = parse_binding(L"Profile.Testing", L"Ctrl+1");
    check(b && b->kind == ActionKind::profile && b->target == L"Testing", "profile binding");
    check(!parse_binding(L"Profile.X", L"Ctrl+1 > 5"), "profile with value is rejected");
    check(!parse_binding(L"GodMode", L"F1 > abc"), "non-numeric value is rejected");
    check(!parse_binding(L"GodMode", L""), "empty value is rejected");

    wchar_t folder[MAX_PATH]{};
    GetTempPathW(MAX_PATH, folder);
    const std::wstring path = std::wstring(folder) + L"nt_hotkeys_test.ini";
    {
        std::ofstream ini(path, std::ios::binary);
        ini << "[Vanilla]\r\nGodMode=1\r\n[Hotkeys]\r\n; comment\r\nSound = 0\r\nGodMode = F1\r\n"
               "SpeedGlobalPercent = Ctrl+F5 > 200\r\nBroken = Hyper+Q\r\nProfile.1 = Ctrl+1\r\n";
    }
    const auto bindings = read_bindings(path);
    check(bindings.size() == 3, "INI: three valid bindings, comment/Sound/broken skipped");
    DeleteFileW(path.c_str());

    std::printf("%s (%d failures)\n", g_failures ? "FAILED" : "PASSED", g_failures);
    return g_failures ? 1 : 0;
}
