# NativeTweaks

An Elden Ring mod that adds a **NativeTweaks** tab to the game's own System
menu. Download and player instructions are on
[Nexus Mods](https://www.nexusmods.com/eldenring/mods/11339); this repository
has the source.

What's in the tab:

- **Mod settings.** Every DLL mod with an INI next to its DLL gets a page, so
  its options can be changed from inside the game. Mod authors can describe
  their options with a template ([docs/INI_TEMPLATE_FOR_MODDERS.md](docs/INI_TEMPLATE_FOR_MODDERS.md))
  and pick changes up live ([docs/LIVE_RELOAD_GUIDE.md](docs/LIVE_RELOAD_GUIDE.md)).
- **Vanilla Settings.** More than 150 options for the game itself on 21 pages
  (camera, HUD, combat, dodging, Torrent, weather, spirit ashes and more),
  applied at once, with hotkeys and profiles.
- **[Multiple Spirit Ashes](https://github.com/ArialSenki/MultipleSpiritAshes)**,
  built in: several spirit ashes at once, up to 500 spirits.

Supported game versions: Elden Ring 2.7.2.0, 2.7.1.0 and 2.7.1.1, PC. Options
whose game data isn't found on another version are greyed out. Offline only,
with Easy Anti-Cheat disabled.

## Layout

| Path | Contents |
| --- | --- |
| `src/` | The mod: menu pages (`native_tweaks.cpp`), vanilla options (`vanilla_settings.cpp`), hotkeys, game memory helpers, Chinese text |
| `msa/` | [Multiple Spirit Ashes](https://github.com/ArialSenki/MultipleSpiritAshes) (git submodule, Rust), linked in as a static library |
| `include/`, `sdk/` | C/C++ headers and an example for mods that want live reload or a provider page |
| `third_party/ERNativeUI/` | ERNativeUI headers (MIT, by Flammrock) |
| `docs/` | Guides for mod authors and design notes |
| `data/` | Animation tables used by some options |
| `tests/`, `tools/` | Unit tests, an AOB scanner and packaging helpers |

## Building

Requirements: MSVC 2022 build tools, CMake 3.24+, and Rust (stable,
`x86_64-pc-windows-msvc`) for the spirit ashes library.

```bat
git clone --recursive https://github.com/ArialSenki/NativeTweaks
cd NativeTweaks
cmake -S . -B build -G "NMake Makefiles" -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

That produces `NativeTweaks.dll`. The menu itself runs on ERNativeUI, and
NativeTweaks needs the extended build of it from the
[`nativetweaks` branch of my ERNativeUI fork](https://github.com/ArialSenki/ERNativeUI/tree/nativetweaks)
(the official ERNativeUI.dll won't work). Build its host with
`-DERNATIVEUI_BUILD_HOST=ON` and ship that `ERNativeUI.dll` next to
`NativeTweaks.dll`.

## Credits

- [ERNativeUI](https://github.com/Flammrock/ERNativeUI) by Flammrock, the
  native menu framework this is built on.
- [fromsoftware-rs](https://github.com/vswarte/fromsoftware-rs) by Vincent
  Swarte and contributors, used by Multiple Spirit Ashes.

## License

[MIT](LICENSE). Elden Ring is a trademark of FromSoftware / Bandai Namco. This
is a fan-made mod, not affiliated with them.
