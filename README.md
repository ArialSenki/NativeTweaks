# NativeTweaks

An Elden Ring mod DLL that adds a native **NativeTweaks** tab to the game's System menu, through a fork of [ERNativeUI](https://github.com/Flammrock/ERNativeUI). The tab holds two things:

- **Vanilla Settings**: more than 180 options for Elden Ring itself, grouped in pages (survival, equipment, resources, movement, defense, Torrent, damage, poise, status effects, spirit ashes, runes and shops, map, world, camera, image, display, interface, item shop). They apply at once and are saved in `NativeTweaks.ini`.
- **Mod pages**: every DLL mod with an INI next to it gets a page where its values can be edited from the game. Mods can describe their INI with a template (labels, types, ranges) and can apply saved values live through an optional API.

It also adds entries to the ERNativeUI talk menu (F11): an **Item Shop** with every obtainable item by type, its own settings (upgrade level, price) and **Recover flasks**.

Supported game versions: 2.7.0.0 (1.17), 2.7.1.0 (1.17.1), 2.7.2.0 (1.17.2), Japanese 2.7.1.1 and 2.7.2.1, PC. Offline only, with Easy Anti-Cheat disabled.

Player documentation (installation for MO2, Mod Engine 3, Mod Engine 2 and Elden Mod Loader; every option) is the `README.txt` shipped in each release archive. The work log is [`PROGRESS.md`](PROGRESS.md) (in Spanish).

## Layout

| Path | What it is |
|---|---|
| `src/native_tweaks.cpp` | Start-up, connection to ERNativeUI, mod discovery, INI pages, templates, profiles, language, logging. |
| `src/vanilla_settings.cpp` | Vanilla Settings: options, texts (en/es/zh; zht converted at run time), game hooks and param patches, item shop. |
| `src/hotkeys.cpp` | `[Hotkeys]` in `NativeTweaks.ini`: keys that set options or load profiles. |
| `src/game_memory.cpp` | Guarded reads, AOB scans, game statics, param tables. |
| `src/spirit_ashes.cpp` | C++ side of the Rust staticlib below. |
| `msa/` | Multiple Spirit Ashes (Rust, staticlib): several spirit ashes at once, area follow and free fighting, compact HUD rows, game menus (mirror, level up, rebirth, shop), flask state. `msa/standalone` builds the same code as `MultipleSpiritAshes.dll`. |
| `third_party/ERNativeUI/include` | ERNativeUI public headers (MIT). |
| `include/`, `sdk/`, `examples/` | Headers and samples for mod authors. |
| `docs/` | API and template guides; `docs/historico/` keeps early research notes. |
| `tools/audit/` | Off-line audit target (stubs for the Rust side). |

## For mod authors

- [`docs/INI_TEMPLATE_FOR_MODDERS.md`](docs/INI_TEMPLATE_FOR_MODDERS.md) and [`docs/INI_TEMPLATE_GUIDE.txt`](docs/INI_TEMPLATE_GUIDE.txt): describe an INI so its page shows proper toggles, sliders and lists.
- [`docs/LIVE_RELOAD_API_V1.md`](docs/LIVE_RELOAD_API_V1.md) and [`docs/LIVE_RELOAD_GUIDE.md`](docs/LIVE_RELOAD_GUIDE.md): the optional `NativeTweaks_RequestReloadV1` / `NativeTweaks_GetReloadStatusV1` exports that let NativeTweaks apply saved values without a restart.
- Your own native pages: register them with ERNativeUI directly (its public API); the old ERMS provider proposal is kept in `docs/historico/`.
- The ERNativeUI fork's own API (talk menu, text inputs, live texts) is documented in the fork, `docs/fork/api.md`.

## Build

Windows x64, MSVC (Visual Studio 2022 Build Tools) and Rust (for `msa/`), CMake 3.24 or newer.

```powershell
cmake -S . -B build-nativetweaks-msvc -G "NMake Makefiles" -DCMAKE_BUILD_TYPE=Release
cmake --build build-nativetweaks-msvc
```

The Rust library is built by `msa\build.cmd` into `-DNT_MSA_TARGET_DIR` (default `%LOCALAPPDATA%\NativeTweaks-msa-build`). Give every checkout or worktree its own target folder: Cargo decides by path whether the library is up to date, and a shared folder can link another checkout's code. Release builds also write `NativeTweaks.pdb` (only its file name is stored in the DLL).

Tests: `msa\build.cmd test` (needs the game executables of each supported version, see `msa/src/hud.rs`) and the ERNativeUI fork's own CTest suite.

## Licences

NativeTweaks source: see the release archive. ERNativeUI headers: MIT, [`third_party/ERNativeUI/LICENSE.txt`](third_party/ERNativeUI/LICENSE.txt). Rust dependencies: `msa/THIRD-PARTY-NOTICES.txt`.
