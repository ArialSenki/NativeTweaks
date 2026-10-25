# Provider discovery and live settings

## Goal

Give compatible mods one native in-game settings interface. When a user adds another compatible mod to the MO2/ME3 profile, the Hub should discover its identity and settings and create its page automatically. Saving should update that mod's configuration and apply settings that the mod marks as safe for live changes.

## Why an INI alone is not enough

An INI file has keys and values, but it does not explain what a value means, what range is valid, which other values it depends on, or whether the running mod can safely change it. A DLL that reads its INI only at startup has no way to know that the Hub changed the file. The Hub must not guess those semantics or patch arbitrary process memory.

The discovery preview now has a conservative legacy fallback: it watches the
loaded module list briefly, then finds INIs beside DLLs in common MO2/ME3 mod
paths (`natives`, `mods`, `external_dlls`, `DLLs`, and `bin`). For DLLs nested
in standard loader folders, it also checks the immediate mod folder. Unique
short values become native text inputs and save to the original file. After
the save, the Hub checks whether the owning DLL exposes the shared live-reload
API. This makes simple sidecar INIs discoverable without changing their DLLs;
only DLLs that implement the API can apply changes immediately. It does not
claim that all fields are semantically safe or that an INI alone describes
typed controls, named choices, or validation.

Mods may optionally provide a friendly menu name in the same INI:

```ini
[ERModSettingsHub]
Name=Example Mod
```

The Hub hides this reserved metadata section from the generic editor. Without
it, the page uses the INI filename.

## Optional typed provider contract (future work)

Each compatible DLL exposes one versioned C ABI entry point and a bounded descriptor containing:

- Stable provider ID, display name, version, and description.
- Typed settings with stable IDs, labels, help text, current/default values, valid ranges, steps, and optional choices.
- Per-setting apply capability: live-safe or restart-required.
- A transaction callback for the complete set of changed values, plus a result that distinguishes applied, restart-required, rejected, and failed.

The provider owns the actual INI format and location. It validates the complete candidate, writes its backup and updated file atomically, then publishes the new in-memory configuration at a safe point in its own worker or update loop. If validation or persistence fails, the old runtime configuration remains active. The Hub displays the provider's result and never writes a guessed path or arbitrary memory. This richer descriptor is separate from the now-implemented INI reload ABI in `LIVE_RELOAD_API_V1.md`.

## Discovery and runtime rules

- During the bounded startup discovery window, enumerate loaded mod modules and inspect only their adjacent configuration files or documented provider export. The Hub waits for a short stable-module interval, then registers one fixed menu tree. Do not scan every file on disk and do not run discovery per frame.
- The supplied host profile keeps provider registration open for up to 15 seconds. The ERNativeUI host must load before the Hub; mod DLL order within this startup window does not matter. Modules arriving after the host freezes its registry cannot be added to that menu.
- Validate ABI version, descriptor sizes, setting limits, IDs, UTF-8 strings, value kinds, ranges, and callback presence before creating a page. Skip providers that fail validation and record the reason in the Hub log.
- Call providers only after the player confirms Save, on a Hub worker. A provider may queue its update and return a request ID; the Hub can wait briefly for an explicit result without blocking menu input.
- Provider code must not call game/render APIs from `DllMain`, reread its INI every frame, or expose undocumented memory-patching callbacks.
- INI-only mods use the restricted generic editor. If their loaded DLL exports the shared reload ABI, Save requests a live apply; otherwise the Hub reports restart-required. Mods with richer controls can adopt a typed provider descriptor later.

## Rollout

1. Manually validate startup discovery, the generic INI editor, and safe backup/save behavior in the user's offline profile. The user has confirmed the native Hub entry and Tarnished HUD adapter; generic discovery still needs its first manual check.
2. Convert the user's other DLL mods to the shared reload ABI one by one and provide the export header to other modders.
3. Add the richer typed provider descriptor after the save/reload result path has been manually checked in the user's offline profile.
4. Add optional reviewed adapters for third-party mods only when their configuration semantics and reload behavior are documented.

This gives modders one integration target while preserving a clear restart-required path for settings that cannot be applied safely in a running game.
