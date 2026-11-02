# Changelog

Same entries as the [Nexus page](https://www.nexusmods.com/eldenring/mods/11339). Newest first.

## r153

- Changed: Boss health goes up to 2000% (x20) without Extended ranges (it stopped at 1000%). Nexus request.

## r152

- Fixed with Solid Uncapper: the help line showing two texts on top of each other (Nexus report, thanks for the log).
- Fixed with Solid Uncapper: menu values not refreshing live and changed options getting no colour.
- Changed: Remove vignette explains why Solid Uncapper warns at start that Vignette could not be applied; it is harmless, and turning Remove vignette off avoids it.

## r151

- New: Walk speed, Run speed and Interaction speed (levers, doors, chests, picking things up, getting up from a grace) (Nexus requests).
- New: Objects follow interaction speed (beta, off by default): chests, doors, levers and Stonesword Key statues keep up with you; lifts do not change.
- New: Projectile range for spells, arrows, thrown items and weapon skills, base game and DLC, and Lock-on distance (Nexus request).
- New: Remove forced sharpening; it only matters with anti-aliasing on (Nexus request).
- New: Bewitching on all enemies and on bosses (beta) (Nexus request).
- Fixed: Sprint speed missed some sprint animations; Roll speed also changed opening chests and doors.

## r150

- Quality update: fixes and improvements to the menu engine (ERNativeUI) that were ready but had not been released yet. No settings changed; just replace the files.
- Changed: help texts from mods now have the game's own shadow, like the game's help line.
- Changed: faster start; ERNativeUI.dll no longer needs the Visual C++ runtime (fixes a crash at start on Linux/Proton with an old runtime).
- New: DLLs\ERNativeUI-status.txt says at every start whether the menu started and why not (even with logging off); after an unknown game patch the menus stay off cleanly.
- Fixed: hotkeys could stay blocked after quitting to the title screen with a message box open; a menu change made at the moment the mod checked its values could be undone.
- Changed: the log names a mod that already hooked a needed game function and keeps its last lines on exit; full third-party license texts included.

## r144

- Fixed: Summon anywhere did nothing while Several ashes at once was off - you still could not summon away from a monument (Roundtable Hold, Radahn). It now works on its own: one ash at a time, as in the game, anywhere (Nexus report).
- Fixed: Hide buff effects now also hides item buffs (Physick tears, cured meats, livers, fowl feet...), item-use bursts and talisman glows; Self buff duration and Permanent self buffs apply to item buffs too (Nexus report).
- Changed: Hide weapon buff effects also hides the flash when you apply a grease.
- New: Sprint speed (%) in Movement - sprinting only, on top of Movement speed (Nexus request).
- New: a Pending section in the README and on this page lists known issues and work in progress.

## r143

- New: Stack weapon buffs (beta) - a grease and an armament spell or skill buff on the same weapon stay on together.
- New: Open level up - Melina's level-up screen anywhere, from the menu or a key.
- Changed: Endure counts as a self buff, so Permanent self buffs keeps it on (Nexus request).
- Fixed: Hide weapon buff effects no longer puts a golden smoke wall beside you with a grease or stops light attacks; Hide buff effects hides more (Nexus reports).
- Fixed: unavailable (greyed out) options no longer show "On" or "Yes"; the help line no longer shows two texts with a controller (Nexus reports).

## r142

- New: Deflect window (%) (beta) for the Deflecting Hardtear's perfect guard; Stack self buffs (beta) (Nexus requests).
- New: Kills before staying dead, like Dark Souls II; turning Enemies stay dead on or off during play starts the save's list over (Nexus request).
- New: Solid spirit ashes, Solid ghostly characters (beta) for summoned NPCs, spectral enemies and Melina, and Keep your character clean (beta) for your character only (Nexus requests).
- Fixed: Permanent weapon buffs no longer make Destined Death and other on-hit effects last forever and stack on enemies (Nexus report).
- Fixed: the example key for Dismiss all ashes (and Open mirror / rebirth) in NativeTweaks.ini works once uncommented; a key on a button gives the high beep (Nexus report).

## r141

- Fixed: Seamless Co-op no longer stops at launch with "No such pattern" while Several ashes at once is on; the change that makes room for 500 spirits now leaves the game code Seamless Co-op looks for untouched (Nexus report).
- Fixed: the help line at the bottom of Vanilla Settings no longer shows two texts on top of each other (user report).
- Fixed: the game no longer closes at launch on Linux (Proton) with older Visual C++ runtimes (user report).
- Other: ERNativeUI writes crashes during its own startup to ERNativeUI.log when the log is on; Multiple Spirit Ashes 0.2.3.

## r140

- Fixed: upgraded spirit ashes (+1 to +10) work with several ashes at once - they can be summoned beside others, and their item dismisses only that ash (Nexus report).
- Fixed: Summon anywhere works in every area of the game and the DLC, boss arenas included (Radahn's battlefield, Nexus report); ashes summoned away from a monument no longer stand frozen, and a refused summon no longer comes out later in place of the ash you chose.
- Fixed: after all your ashes die you can summon again at once (before, summoning stayed blocked until you rested).
- Fixed: summons past 10 spirits no longer spend the FP for nothing (the game keeps 10 spirits per player; Lone Wolf Ashes gave 1 wolf of 3).
- Changed: up to 500 spirits at once and no ash limit (Most ashes at once is gone); Several ashes at once is on by default; the HUD shows 7 ash rows, the last one reads +N with more ashes.
- New: Same ash again (off by default) - using a summoned ash's item summons it once more instead of dismissing it; Dismiss all ashes removes them.
- New: Highlight changed options (now applies at once) and Extended ranges (applies when the game starts) are included in the main download, off by default - Interface & System page or [General] in NativeTweaks.ini; the two optional downloads are no longer needed.
- Changed: menu pages tidied up (21 pages) - new Dodge & Defence page (dodge invincibility, parry window, perfect guard, invincible while knocked down, super armour while using items); No lock-on moves from Enemies to Camera.
- Other: NativeTweaks.log is written line by line until the mod has started, so a crash at launch still leaves the lines before it.
- ERNativeUI: new export to turn the coloured labels on and off live.

## r139

- New: Dodge invincibility (%) (beta, Movement & Speed, Nexus request) - rolls, backsteps and dodge skills stay invincible longer; 200% doubles the game's i-frames, and the extra time lasts even if you act right after.
- New: Parry window (%) (beta, Movement & Speed, Nexus request) - the parry window of shields and parry skills lasts longer; 200% doubles it.
- New: Invincible while knocked down (beta) - no damage while a big hit has you on the ground, getting up included.
- New: Super armour while using items (beta) - the game's super armour while you drink a flask or use an item; weaker hits no longer interrupt you.
- Fixed: Yes/No questions start on Yes (now beta) never worked; it now starts on Yes in the game's question boxes (quit, travel...) and moves the mouse pointer onto Yes.
- Removed: the extended ranges and their x1-x100 Scale rows under the sliders; sliders are back to their normal ranges.
- ERNativeUI: the startup log is no longer lost when the game closes right at launch.

## r138

- New: Boss health (%) (Enemies, Nexus request) - scales the health of bosses only (main, field, dungeon and DLC bosses); it multiplies Enemy health, so with Enemy health at 100% only bosses change.
- New: Hide buff effects and Hide weapon buff effects (Image, Nexus request) - your buffs, greases and enchantments no longer show particles or a glow; they work exactly the same and keep their sound.
- New: Keep characters clean (beta) (Image, Nexus request) - blood, mud and scorch marks no longer stick to bodies; for now it also applies to enemies.
- Fixed (Several spirit ashes at once): summoning right after the current ash died, or summoning the same ash again after it died, no longer dismisses the other living ashes.
- Fixed (Several spirit ashes at once): using an ash's item could dismiss instead of summon (summoning animation but nobody came); items are now matched to their ash by the game's item data.
- Fixed (Several spirit ashes at once): fallen ashes are released two seconds after they die; they used to pile up until no new summon worked.
- Same changes as the main r138 file: Boss health (%), Hide buff effects, Hide weapon buff effects, Keep characters clean (beta), and spirit ash fixes for long boss fights. Spirit ash HUD rows stay off in this file.

## r137

- Weapon stat scaling (%) (Damage Dealt page) now goes up to 1000% (it stopped at 500%). User request.
- Same as the main r137 file: Weapon stat scaling (%) now goes up to 1000% (it stopped at 500%). Spirit ash HUD rows stay off in this file.

## r136

- Compatible with Body Shape Customizer: with it (or any mod that hooks the menu's Back function) the NativeTweaks menu did not load; fixed in the included ERNativeUI.dll.
- New: World difficulty (Enemies) - enemies get the strength of New Game+ 1 to 7 in any playthrough, scaled area by area like the real New Game+ (health, damage, defence, status resistance, runes); your save's cycle, bosses, items and endings don't change; in New Game+ it only adds what your cycle lacks.
- New: Keep your runes (Enemies) - with World difficulty on, enemies give the runes of your own cycle.
- New: Open the mirror and Rebirth (Map & Travel) - the Roundtable Hold mirror and Rennala's rebirth from the menu, anywhere; rebirth needs no Larval Tear; both can be bound to keys (OpenMirror, OpenRebirth).
- New: Charged attacks launch enemies (Damage Dealt, Nexus request) - charged heavy attacks throw enemies back, far back or upward, with their poise intact; Every attack launches extends it to all your attacks, skills, spells and thrown items.
- New: Launch distance (%) (beta, in progress) - for now it only makes launched enemies lighter; leave it at 100%.
- Fixed: Jump height on foot no longer stops in mid-air before landing; Torrent's jump no longer hangs at the top.
- Fixed: Force weather asks the game to change the weather, so it changes within 2-3 seconds everywhere; four weather names corrected and three new types (Rainy fog, Snowy fog, Showers).
- Fixed: Menus to screen edges - the inventory's item menu is no longer pushed off the left edge on ultrawide screens.
- Ashes stay after a boss falls also works in arenas of bosses defeated before.
- Out of beta: Jump height/distance, Torrent jump height/distance, Force weather, Upgrade and Craft without materials, Yes/No questions start on Yes, Several ashes at once, Summon anywhere, Resummon without resting, Ashes stay after a boss falls, HUD to screen edges, HUD scale, Menus to screen edges.
- Source code now public: github.com/ArialSenki/NativeTweaks.
- Same changes as NativeTweaks r136 (see the main file's changelog): compatible with Body Shape Customizer, World difficulty (NG+1-7), Keep your runes, Open the mirror, Rebirth, Charged attacks launch enemies, Every attack launches, jump and weather fixes, 15 options out of beta.
- This file keeps the spirit ash HUD rows off ([SpiritAshes] HudRows = 0).

## r135

- Supports Elden Ring 1.17.2 Japanese (eldenring.exe 2.7.2.1) besides 2.7.2.0, 2.7.1.0 and 2.7.1.1.
- Multiple Spirit Ashes built in (Spirit Ashes page): Several ashes at once (beta) - summon different spirit ashes together (up to 20), each paying its FP; using a summoned ash's item again dismisses only that one with a short fade, even with too little FP; no confirmation boxes; ashes follow you into new areas and through doors and fog walls, and a stuck ash is brought back; a compact HUD row per ash with its name and HP (up to 6).
- New Spirit Ashes options: Dismiss all ashes (button or [Hotkeys] SpiritDismissAll), Cost of extra ashes (%), Damage lost per extra ash (%), Ashes stay after a boss falls (beta).
- Summon anywhere (beta) replaces "Summon far from monuments" (your setting is kept); Summon anywhere, Ashes stay after a boss falls and Resummon without resting work together and are on by default.
- Two downloads: the main file shows the spirit ash HUD rows; the Compatibility file leaves the HUD alone (Seamless Co-op, other HUD mods).
- Vanilla Settings are sorted into 20 pages.
- New options: No lock-on, Break poise in one hit, Instant status effects, Die in one hit, Enemies break your poise in one hit.
- Fixed: Status immunity works (game's own immunity check); Infinite poise also stops the light stagger; Invincible protects only you; with Enemies stay dead, living enemies reset at graces and killed ones stay gone with no body; all five weapon effects apply at once.
- "Posture damage" is now "Poise damage" (dealt and taken, up to 1000%).
- Menu: the NativeTweaks tab stays in the System menu (ERNativeUI 1.3 tab mode experimental: [Tabs] ModSettingsTab = 1); reopening the menu on it no longer shows leftover rows; an unlisted game exe with a known layout is recognised; the title screen warns when the game version is not supported.
- ERNativeUI updated to 1.3.0 (Flammrock): 13 rows per page, faster start, less work per frame (r134). Fixed on 2.7.1.1: hotkeys could stop the menu from loading.

## r133

- Fixed on Elden Ring 2.7.2.0: the NativeTweaks entry was missing from the System menu (options menu checks still used 2.7.1.0 addresses; they now follow the running game version).
- Fixed on 2.7.2.0: changed settings (hotkeys, profiles, Restore defaults) update the visible row at once again and changed options are coloured again (colours also work again on 2.7.0.0).
- The log warns when a feature is turned off for the game version and when a mod DLL loads after the menu was built.

## r132

- Support for Elden Ring 2.7.2.0 (Steam update of October 20, 2026): the settings menu, text input, color picker and every option work again on the new version.
- 2.7.1.0 and 2.7.1.1 still work. No option changes.

## r131

- Options marked (beta) are still in testing: they can change in later versions and are not the final product yet.
- New: Torrent speed split into Torrent run speed and Torrent jump speed; with jump speed at 100 % the double jump works as in vanilla. Old TorrentSpeedPercent settings keep working as the run speed.
- New (beta): jump height and distance, on foot and on Torrent (25-500 %). The animation plays at normal speed; a higher jump lasts longer in the air. Fall damage does not change.
- Status buildup (overall and per status) now goes up to 1000 %.
- Fixed: per-action speeds (attack, spell, roll, movement, jump, item use) read the wrong animation; attack speed at 200 % now works.
- Enemies stay dead (beta) now also works after fast travel, loading the save and dying. Each save keeps its own list in DLLs\NativeTweaks\dead-enemies; listed enemies fall at once when they load, with no runes or drops.
- Hit downed enemies (beta): an enemy killed while lying on the ground or getting up now dies on the spot, also after a normal hit.
- Smoother play: the log is written by its own thread, background checks sleep or poll less when their options are off, diagnostics are only computed with the log on.
- PerfLog also reports the longest pass of every thread and hook, to track down stutters.

## r129

- New (beta, off by default): Enemies stay dead - killed enemies stay dead when you rest, pass time or die. Not finished yet: fast travel and loading the save still bring them back.
- New (beta, off by default): Hit downed enemies - like Nightreign, enemies knocked down by a critical hit or riposte take damage on the ground and while getting up, and fall on the spot if they die then.
- New (beta, off by default): Yes/No questions start on Yes (Display & System). OK/Cancel windows do not change.
- New: Souls Vision page in Mod Settings (Souls Vision 0.6.1, sv_config.toml): most changes apply in game as soon as you save. Souls Vision is a separate download.
- Simpler install: the DLLs folder holds only four files. Translations and the animation table are built into the DLLs (old locales\ and NativeTweaks.animations.txt can be deleted).
- "Previous/Next page" follow the NativeTweaks menu language at once.
- The modder docs, live reload SDK and examples moved to the optional "NativeTweaks SDK" file. Licenses are in one THIRD_PARTY_NOTICES.txt.
- Ultrawide: with [Gfx] RuntimePatch = 0 the ultrawide options show as unavailable instead of doing nothing; unavailable sliders now look unavailable too.
- A HUD mod that scales a whole block (e.g. the gauges) no longer stops the HUD from moving to the edges.

## r127

- Ultrawide (beta), off by default, for screens wider than 16:9 with "Allow ultrawide": HUD to screen edges, HUD scale (50-150 %), Menus to screen edges. Restart the game after changing them.
- Menus to screen edges covers pause menu, equipment, inventory, status, options, shops, level up, spells, storage, map, loading and title screens; backgrounds, dialog dimmers and subtitle/banner bands cover the whole screen.
- Nothing is installed in a menu folder: the game files are adapted in memory. Tested with the vanilla HUD, Tarnished HUD build 4 (bars reach the real edge) and Convergence 3.0.2.0.
- Mods with their own ERNativeUI menu (tested with Spirit Multiplier 1.3.1) now appear inside NativeTweaks' Mod Settings instead of Game Options.
- Fixed: the NativeTweaks menu could disappear when such a mod registered its menu first.
- Tarnished HUD and Adaptive Bar Scaling (build 4 of both mods) now work live: change their settings in game and press Save, they apply at once without restarting.
- Adaptive Bar Scaling gets its own page with labels and help for every option, including Bar style (dynamic / static).
- A mod INI named `<Mod>_config.ini` is now tied to its own DLL, so Save reaches the right mod even when several DLLs share a folder (ME3 natives).
- New in Vanilla Settings > Player & Torrent: maximum HP / FP / stamina (10-1000 %) and HP / FP / stamina regeneration in points per second.
- Removed: the old HP / FP / stamina regeneration (%/s) options, replaced by the points per second ones; a saved value for them is ignored.
- No separate ERNativeUI download needed: NativeTweaks ships its own.

## r125

- New: Hide helmet (Player & Torrent) - head armour looks like a bare head; defence, poise, weight and effects unchanged.
- Elden Ring 2.7.1.1 is recognised (same game code as 2.7.1.0): the menu and every option work on both.
- No menu files any more: the options menu is adapted in memory, so it works with other menu mods and with Solid Uncapper (load ERNativeUI and NativeTweaks before it).
- Vanilla Settings reorganised into 16 pages with many new options: weapon status effects, damage per element, buildup per status, permanent buffs (beta), spell and skill costs, carry 999 of each item, item use speed, flask healing.
- New: Shop prices (%), Craft without materials (beta), Upgrade without materials (beta), Weapon stat requirements (%); Upgrade cost also covers spirit ashes.
- Hotkeys for Vanilla Settings ([Hotkeys] in NativeTweaks.ini) and five profile slots to save and load all settings.
- External INI templates, Export / Import buttons on mod pages and translation files (all off by default).
- Scaled sliders show the real value; changed-value colors follow each setting's meaning.
- Fixes: Damage taken scales flat enemy damage; No fall damage covers Torrent; Torrent speed applies at once (no longer beta).
- Performance: far less background work, PerfLog option for stutter reports, ERNativeUI.dll about 110 KB smaller.
- More robust against game updates: 15 menu interfaces no longer rely on fixed addresses.

## r118

- Other mods' INIs without a template: switches, option lists ("0 = Off, 1 = Low"), ranges ("(0-100)", "Min 0 Max 100") and other numbers are now detected automatically as menu controls.
- The INI's own comments become each option's help text.
- Live reload kit for mod authors: one-header C++17 helper (sdk\include\nativetweaks_live_reload.hpp) and a complete example.
- New docs\LIVE_RELOAD_GUIDE.md: step-by-step guide to add live reload to an existing mod; docs\LIVE_RELOAD_API_V1.md rewritten.
- Fix: live reload now finds the mod's DLL under Mod Organizer 2.
- Shorter "restart to apply" message, no longer cut off.

## r116

- Optional Highlight changed options: changed toggles gold, sliders above default green, below red; back at default they show the menu's own colour again.
- Optional Extended ranges: sliders use all 256 positions and gain a Scale row (x1/x2/x5/x10/x100); long pages continue on a second page.
- Every Vanilla Settings page ends with a Restore defaults row for that page; restoring updates the visible values at once.
- One DLL for English, Spanish, Simplified and Traditional Chinese, with an in-game language selector and language-switch stability fixes.
- Built on ERNativeUI 1.2.1: slider units (%, %/s, FPS, Hz, hour) and wider ranges (damage/HP/runes/drops up to 1000%, speeds 25-300%, FOV 25-200%, FPS 30-360).
- Negative regeneration (HP never drops below 1).
- Weather shows its full name in the menu language instead of a number.
- Wider help line; long help texts wrap onto a second line; vanilla options keep the game's own help line.
- Wider slider value field in the options menu and mod pages, so long value names are no longer cut.
- In-game alert if another mod replaces or removes 02_040/02_042 (loose-file installs).
- Performance: lighter background checks, no polling of empty controller slots (fixes XInput micro-stutter), saving writes only changed options.
- Logs are off by default.
- Refreshed ERNativeUI fallback addresses for Elden Ring 2.7.1.0.
