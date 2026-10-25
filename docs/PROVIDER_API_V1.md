# Provider API v1

> **Superseded design proposal.** The current client uses ERNativeUI directly and does not export `ERMS_GetHostApiV1`. This document and its matching header are retained only as an unimplemented option for future provider plug-ins; they are not an integration contract and must not be used to build a provider for the current client.

This draft describes an opt-in C ABI for DLLs that want their settings listed in ER Mod Settings Hub. It does not discover arbitrary DLL settings or write another mod's INI behind its back.

## Lifecycle

1. Load the hub before provider DLLs in the ME3 profile.
2. After DllMain has returned and the provider's normal initialization is safe, the provider resolves ERMS_GetHostApiV1 from ERModSettingsHub.dll.
3. The provider fills a static ERMS_ProviderV1 descriptor and registers it once.
4. The hub copies bounded descriptor text and calls provider callbacks only while the Mods page is open or after the user confirms a change.
5. If the hub is missing or has an unknown ABI, the provider continues using its normal standalone configuration.

Providers must not call the host from DllMain, block on game rendering, or perform per-frame registration/polling. Descriptor strings and callbacks must remain valid until process exit. ABI v1 has no unregister operation; the initial ME3 integration assumes native mod DLLs stay loaded until game exit.

## Contract

- Initial controls are boolean toggles, bounded floating-point sliders, and declared choice lists. Numeric sliders declare a scale mode: linear for ordinary ranges or `log1p` for wide, nonnegative ranges. The game-style row places the formatted numeric readout in a shared right-aligned value column; left/right changes one declared `step` at a time. V1 caps providers at 32, settings at 64 per provider, choices at 32 per setting, and descriptor strings at the byte limits declared in the header.
- IDs are stable ASCII/UTF-8 keys; labels and descriptions are UTF-8.
- Both sides validate IDs, struct sizes, ABI version, value kind, ranges, scale mode, choice indices, and finite numeric values. `log1p` is valid only when minimum is nonnegative and maximum exceeds minimum. IDs use stable UTF-8/ASCII keys; labels are size-limited UTF-8.
- Unknown setting flags are rejected. `ERMS_SETTING_RESTART_REQUIRED_V1` marks a control whose persisted changes need a restart; the result code also reports whether the specific change was saved and needs a restart.
- The menu edits a local draft while a slider moves. It calls set_value once when the user confirms; set_value returns whether the value applied now, needs a game restart, was rejected, or failed.
- reset_value returns the same result and restores that setting's declared default.
- The provider owns persistence. The hub never edits provider files directly.
- For a float slider, `step` is the left/right increment and the slider snaps to valid steps. `log1p` maps the normalized track position through `log(1 + value)` so broad ranges remain usable near zero; it does not change the value written by the provider.
- A setting that still requires a restart must say so in its description and return ERMS_SET_RESTART_REQUIRED_V1.
- Provider callbacks run only on a menu action or page-open path, never on a render callback. They must be short, must not wait on the game/render thread, and must not allow C++ exceptions to cross the C ABI.
- The host does not hold its provider-registry lock while calling provider code.

## Safe fallback

Registration is optional. A provider that cannot find the hub, rejects the ABI, or fails to register remains usable through its existing INI/config path. The hub omits invalid providers rather than guessing their setting layout.
