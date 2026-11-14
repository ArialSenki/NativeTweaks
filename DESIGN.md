# NativeTweaks — design

## Menu

NativeTweaks is an ERNativeUI provider. The fork of ERNativeUI owns every native UI hook (Mod Settings tab, rows, sliders, text inputs, dialogs, the F11 talk menu) and NativeTweaks only registers content and reacts to callbacks. Nothing is drawn outside the game's own menus.

- **Start-up.** A worker starts after `DllMain` returns. It connects to the host, waits a short discovery window for other mod DLLs to load (never past the point where ERNativeUI closes provider registration), then registers one menu: Vanilla Settings first, then one page per discovered mod INI.
- **Live texts.** Labels and help texts are kept per language and swapped through the fork's live-text exports, so a language change applies without a restart.
- **Talk menu (F11).** The item shop registers its own talk entry; pages are built on the game thread in the host's callbacks.

## Vanilla Settings

- **Options** are defined once (`g_options`, enum `Id`) and their texts follow the same order in each language table. `kLayout` decides the pages. `available()` hides what the running game version does not support; a hidden option cannot be changed from hotkeys or profiles either.
- **Game changes** come in three kinds:
  - **code sites** found by unique AOB patterns, with the stolen bytes checked before any hook;
  - **param fields** scaled from a snapshot of their original values, so turning an option off restores the game's data;
  - **workers** for state that lives on loaded characters.
  
  Patterns are checked against every supported executable.
- **Saving.** `NativeTweaks.ini` is written by a worker that only writes keys that changed. Mod INIs are replaced atomically with a `.ermodsettings.bak` copy.

## Item shop

At load the shop builds an enlarged copy of `ShopLineupParam` (one shop range per section and base game / Shadow of the Erdtree) and swaps the param holder; the old table stays allocated. Only items the game gives (item lots, shops, recipes, class start, event rewards on NPC templates) are listed, with known removed content left out. While one of our shops is open:
- sort ids and groups of the shown items are rewritten for a clean layout;
- the sell value reads as 0 so any price shows;
- the player's own "all items" sort is put back when it closes.

## Multiple Spirit Ashes (`msa/`)

Rust staticlib linked into NativeTweaks.dll and also built on its own. Its FD4 frame tasks run on the game thread and catch panics. The C++ side calls a small C ABI (`nt_msa_*`, `nt_open_*`, `nt_flask_state`). It installs nothing on a game version it does not know.

## Failure behaviour

- **Unknown structures.** If a pattern, param layout or structure is not as expected, the option is greyed out and the log says why; nothing is patched blind.
- **Game memory.** Every read of game memory outside the game thread goes through guarded copies (access violations only).
- **Crash dumps.** Release builds write a PDB next to the DLL, kept with each release, so players' crash dumps can be read.

The work log, with every test and its result, is [`PROGRESS.md`](PROGRESS.md).
