# Adding live reload to an existing mod

A step-by-step guide for a DLL mod that already reads its INI at startup. At the end,
changes saved from NativeTweaks apply in game without a restart. For the reference
(ABI, return values, rules) see `LIVE_RELOAD_API_V1.md`.

Requirements: C++17, MSVC (or any compiler that builds Win32 DLLs), and an INI named
like your DLL and placed beside it (`MyMod.dll` + `MyMod.ini`).

## Step 1 - Add the header

Copy `sdk\include\nativetweaks_live_reload.hpp` into your project. It is a single
file with no other dependencies.

## Step 2 - Gather your settings in one struct

Most mods keep settings in loose globals:

```cpp
// Before
bool  g_enabled = true;
float g_speed = 1.0f;
int   g_quality = 1;
```

Put them in one struct and hold it in `Live<T>`:

```cpp
// After
#include "nativetweaks_live_reload.hpp"

struct Config {
    bool  enabled = true;
    float speed = 1.0f;
    int   quality = 1;
};
nativetweaks::Live<Config> g_config;
```

Why: a reload must replace all values at once. With loose globals, a hook could
read the new `speed` together with the old `quality`.

## Step 3 - One function that loads and validates

Turn your existing INI reading into a function that fills a **new** `Config` and
returns `false` when a value is invalid. Use the same function at startup and on
reload, so both apply the same checks.

```cpp
bool load_config(const wchar_t* path, Config& out) {
    Config next;  // starts with your defaults
    double number = 0;
    if (!nativetweaks::ini_bool(path, L"Main", L"Enabled", next.enabled)) return false;
    if (!nativetweaks::ini_number(path, L"Main", L"Speed", number) ||
        number < 0.5 || number > 2.0) return false;
    next.speed = static_cast<float>(number);
    // Optional key: keep the default if it is missing.
    if (nativetweaks::ini_number(path, L"Main", L"Quality", number)) {
        if (number < 0 || number > 2) return false;
        next.quality = static_cast<int>(number);
    }
    out = next;
    return true;
}
```

You can keep your own INI parser. Only the shape matters: parse into a new object,
then report whether it is valid.

## Step 4 - Load at startup

Replace the old startup reading:

```cpp
void init_config(const std::wstring& ini_path) {
    Config initial;
    if (load_config(ini_path.c_str(), initial)) g_config.publish(initial);
    // else: keep the defaults and write a line to your log
}
```

## Step 5 - Read from a snapshot

Replace every use of the old globals:

```cpp
// Before
if (g_enabled) apply_speed(g_speed);

// After
const auto config = g_config.get();
if (config->enabled) apply_speed(config->speed);
```

In code that runs every frame, call `get()` once at the top of the function and read
all fields from that `config`. Do not store the pointer for later; get a new one next
time.

## Step 6 - Implement `on_reload`

In **exactly one** `.cpp`, define the implementation macro before the include and
write the callback:

```cpp
#define NATIVETWEAKS_LIVE_RELOAD_IMPLEMENTATION
#include "nativetweaks_live_reload.hpp"

nativetweaks::ReloadResult nativetweaks::on_reload(const wchar_t* ini_path) {
    Config next;
    if (!load_config(ini_path, next)) return ReloadResult::rejected;  // old values stay
    g_config.publish(next);
    return ReloadResult::applied;
}
```

Other `.cpp` files include the header **without** the macro. Defining it twice
causes duplicate-symbol link errors.

## Step 7 - Settings that cannot change while playing

Some values only work at startup: a hook that is installed or not, a memory patch,
an allocated buffer size. Keep the value that was used at startup and compare:

```cpp
nativetweaks::ReloadResult nativetweaks::on_reload(const wchar_t* ini_path) {
    Config next;
    if (!load_config(ini_path, next)) return ReloadResult::rejected;
    const bool needs_restart = next.hook_enabled != g_config.get()->hook_enabled;
    next.hook_enabled = g_config.get()->hook_enabled;  // keep the running value
    g_config.publish(next);                            // everything else applies now
    return needs_restart ? ReloadResult::restart_required : ReloadResult::applied;
}
```

The player sees that a restart is needed; the file already holds the new value for
the next launch.

## Step 8 - Build and test

1. Build as before. If you compile by hand:
   `cl /nologo /LD /O2 /std:c++17 /EHsc /utf-8 /I <folder with the header> MyMod.cpp`
2. Check the exports: `dumpbin /exports MyMod.dll` must list
   `NativeTweaks_RequestReloadV1` and `NativeTweaks_GetReloadStatusV1`.
3. In game: open your mod's page in NativeTweaks, change a value, press
   **Save changes**. Expected message: "Changes saved and applied in game".
4. Save an invalid value by editing the INI by hand and saving again from the menu:
   expect the "rejected" message, and the mod keeps working with its old values.

## Troubleshooting

| Message / symptom | Cause | Fix |
|---|---|---|
| "This mod has no live reload" | exports missing | the macro is not defined in any `.cpp`, or the DLL is an old build |
| "rejected" on every save | INI name differs from DLL name | rename to `<DllName>.ini`, or `load_config` fails on a valid file |
| "did not confirm in time" | `on_reload` took over 5 s | do not wait on the game thread or other locks inside `on_reload` |
| Values change but behavior does not | code still reads a copy made at startup | read through `g_config.get()` (step 5) |
| Duplicate symbol at link time | macro defined in two `.cpp` files | keep it in one |
| `min`/`max` errors | `<Windows.h>` macros | the header defines `NOMINMAX`; include it before `<Windows.h>` or define `NOMINMAX` yourself |

## Checklist

- [ ] Settings in one `Config` struct held in `Live<Config>`
- [ ] One `load_config` used at startup and on reload
- [ ] All reads through `g_config.get()`
- [ ] Macro defined in exactly one `.cpp`
- [ ] Startup-only settings return `restart_required`
- [ ] Both exports visible in `dumpbin /exports`
- [ ] Tested in game: applied and rejected
