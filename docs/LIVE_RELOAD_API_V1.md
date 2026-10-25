# Live reload for mod authors

NativeTweaks shows a settings page for any DLL mod with an INI beside it. When the
player presses **Save changes**, NativeTweaks writes the INI (with a `.bak` backup).
Without live reload, the player must restart the game. With it, your DLL applies
the new values at once and the player sees "Changes saved and applied in game".

Only your mod knows how to apply its own values, so this is opt-in. It takes
about 15 lines with the bundled helper. Adapting a mod that already exists? Follow
the step-by-step guide in `LIVE_RELOAD_GUIDE.md`.

## Quick start (C++17)

1. Copy `sdk\include\nativetweaks_live_reload.hpp` into your project.
2. In exactly one `.cpp` of your DLL:

```cpp
#define NATIVETWEAKS_LIVE_RELOAD_IMPLEMENTATION
#include "nativetweaks_live_reload.hpp"

struct Config { bool enabled = true; double speed = 1.0; };
nativetweaks::Live<Config> g_config;          // your code reads g_config.get()

bool load_config(const wchar_t* path, Config& out) {
    Config next;
    if (!nativetweaks::ini_bool(path, L"Main", L"Enabled", next.enabled)) return false;
    if (!nativetweaks::ini_number(path, L"Main", L"Speed", next.speed)) return false;
    out = next;
    return true;
}

nativetweaks::ReloadResult nativetweaks::on_reload(const wchar_t* ini_path) {
    Config next;
    if (!load_config(ini_path, next)) return ReloadResult::rejected;  // old values stay
    g_config.publish(next);                                           // atomic swap
    return ReloadResult::applied;
}
```

3. Wherever your mod uses a setting: `auto config = g_config.get(); if (config->enabled) ...`

That is all. The helper exports the two functions NativeTweaks looks for, checks
that the saved file is `<YourDll>.ini`, catches exceptions and keeps the results
NativeTweaks polls.

A complete example that builds with one `cl` command is in `sdk\example`
(`LiveReloadExample.cpp` + `.ini`); it writes each reload to `LiveReloadExample.log`.

## What to return

| Result | When | Player sees |
|---|---|---|
| `applied` | new values are active | "Changes saved and applied in game" |
| `rejected` | a value is invalid; you kept the old ones | the INI was saved but rejected |
| `restart_required` | some value cannot change mid-session (e.g. a hook installed at startup) | restart needed |
| `failed` | unexpected error | restart needed |

## Rules

- Parse into a **new** object and publish it whole (`Live<T>::publish`). Never edit
  the live config field by field while gameplay code reads it.
- `on_reload` runs on NativeTweaks' save worker thread, not the menu or render
  thread. Do not call rendering or game UI functions from it. Keep it under 5 s.
- Never unload or reload your DLL. NativeTweaks does not own its lifetime.
- Values that need a restart: still publish the rest, then return `restart_required`.
- In per-frame code, call `g_config.get()` once per frame/call and read every field
  from that snapshot.

## Good to know

- **The INI must be named like your DLL** (`MyMod.dll` -> `MyMod.ini`). Otherwise the
  helper answers `rejected` without calling `on_reload`.
- **`rejected` does not undo the save.** The file on disk keeps the new values, so
  your startup code reads them on the next launch. Validate at startup the same way
  (the example uses one `load_config` for both).
- **The player waits for you.** `on_reload` runs before the result message appears.
  After 5 s NativeTweaks shows "did not confirm in time", but a call that never
  returns still blocks later saves, so never wait on the game thread from it.
- **Missing keys:** `ini_bool`/`ini_number` return `false` for a missing or
  malformed key. Treat that as `rejected`, or keep your default if the key is optional.

## Raw ABI (any language)

If you do not use the helper, export these from your DLL (`include\erms_live_reload_v1.h`):

```c
uint32_t __cdecl NativeTweaks_RequestReloadV1(const wchar_t* ini_path);   // nonzero request ID, 0 = refused
uint32_t __cdecl NativeTweaks_GetReloadStatusV1(uint32_t request_id);    // 0 pending, 1 applied,
                                                                        // 2 rejected, 3 restart required, 4 failed
```

NativeTweaks calls `RequestReloadV1` after the INI is replaced, then polls
`GetReloadStatusV1` every 20 ms for up to 5 seconds. You may finish the work
synchronously inside `RequestReloadV1` or on your own worker; keep each result
queryable for at least 5 seconds. Mods built against the older SDK that export
`ERModSettingsHub_RequestReloadV1` / `ERModSettingsHub_GetReloadStatusV1` still work.

Mods that export neither still get their INI saved; the player is told to restart.
