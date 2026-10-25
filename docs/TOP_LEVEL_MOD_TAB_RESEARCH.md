# Native Mod Settings tab: integration map

Status (2026-10-03): the r12 top-level implementation is rejected and remains disabled. Runtime evidence proves category 4 opens Adjust Brightness (two visual slots, zero native rows; Hub row constructors fault with `0xC0000005`). The latest user log at 22:27 identifies build r10: it inserts category 4 after code 8 and before Exit (code 9) once in each of two separate menu lists, with `duplicateCodes=0`. The route enters panel type 6, then materializes one Hub row into a page with zero native rows and two visual slots. This is the known Adjust Brightness shell, not a separate Hub page. The same DLL skips Keyboard/Mouse and Graphics observers, so this run cannot capture vanilla state for categories 7 and 8. The tested r10 archive carries a 46,618-byte experimental movie (`C0D78E3CAEAA09DD4C333C53B1D81A89DC64DCDCEEFAC37D5A6848E7927E0FD9`), unlike the clean 2.7.1.0 extraction at 46,416 bytes (`547CDF0D4FC02348ACC931E226861567B5E9187E8A011F259BD6236377863834`). The earlier GFX-control r1 archive was not a valid isolation package: its host SHA-256 was `6C37762330CB9FCFBAFF56176E0D4D49D75049936289F776DF3240DF3BF51C3D`, the same experimental host that logged r10. A clean r14 recovery/diagnostics package is now assembled with the safe category-4 route disabled, the current Hub, and the clean exact-build GFX. It can capture the vanilla Keyboard/Mouse and Graphics constructor states; it is not a visual fix or a new top-level tab. The active MO2 winner and in-game result remain unverified.

## Latest r10 capture (2026-10-03 22:27)

- The log reports the exact Elden Ring 2.7.1.0 profile and installs the experimental Mod Settings route. It builds category code 4 after Graphics and before Exit in two distinct native lists; each list ends with one code 4 and reports `duplicateCodes=0`. The counts do not show duplicate insertion within either individual list.
- Opening the route logs `panelTypeBefore=6 panelTypeAfter=6 rewritten=0`, then `category=4 panel=Mod-Settings nativeRows=0 visualRows=2`. One Hub button is committed. The visual result is therefore tied to the game's special brightness panel shell.
- `nativeCategory=0` and `nativeCategory=1` in the old route logger are not the chosen Configuration codes: these values vary across calls while the inserted record remains code 4. Do not use that field to infer the current selector category.
- The host says the Keyboard/Mouse and Graphics observers were skipped. The r14 diagnostic package installs read-only observers for native categories 7 and 8 and records their page state after the original constructor returns.
- Neither the log nor the screenshot establishes which GFX won in MO2. The supplied package uses the clean 46,416-byte 2.7.1.0 extraction and includes a hash manifest so the next run can be tied to a specific package.
- The assembled archive is `ERModSettingsHub-Recovery-Panel-State-Diagnostics-2.7.1.0-r14.zip` (SHA-256 `9CA08A799A5EB6BC7BFFC7E4CDE47928CED79E221965003D32DC62324BA4AE29`). Its 30-file manifest matches every packaged file; the ZIP contains 31 entries including the manifest. Static string and hash checks confirm the r14 host, clean GFX, and recovery Hub payload. This verifies package composition only, not an in-game result.

## Rejected r12 preview implementation

- r12 appended category code 4 after Graphics and before Exit, then preserved its built-in panel route. Runtime evidence shows that route is the specialized Adjust Brightness panel, not a provider-owned settings page.
- The r12 log reports `nativeRows=0 visualRows=2`; both Hub row insertions faulted with `0xC0000005` and `added=0`. This matches the screenshots with two empty controls and brightness artwork.
- The r12 package's GFX is an older output that injected the custom image into shared vanilla `ItemIcon` sprites. That explains why tab art could follow selection onto another category. The current patcher source now clones the custom tab's icon, cursor, and item symbols before injecting its artwork, but that change has not been included in r12 or validated in-game.
- A source build or structurally valid GFX does not establish a safe native page lifecycle. No category-4 Hub route should be shipped until its page constructor, row materializer, selection mapping, and teardown are validated together.

## Recovery r6 runtime capture (2026-10-03 11:12)

- The supplied log contains `Native menu comparison diagnostics build=r13`, freezes with `rootRows=1`, and records a root submenu injection route. It does not report a configuration-category insertion. This matches `ERModSettingsHub-Recovery-Vanilla-GFX-Isolation-2.7.1.0-r6.zip`, whose Hub tab is explicitly disabled, rather than `ERModSettingsHub-NativeTab-Vanilla-Preserving-Candidate-2.7.1.0-r6.zip`.
- The Game Options hook observes four native rows and eight visual slots. On four page constructions the Hub button call raises access violation `0xC0000005`; every attempt reports `added=0`. The screenshot's `?MenuText?` row may be residue from this failed call, but the old catch grouped the native constructor and both cleanup steps, so the exact fault phase was not yet recorded.
- The Keyboard/Mouse and Graphics observers log that they are skipped, leaving those vanilla constructors untouched. This capture cannot attribute their blank panel contents or duplicated selector icons to ERNativeUI row hooks. The recovery package uses the clean 2.7.1.0 GFX, but the log does not identify which `02_040_optionsetting.gfx` won in the user's active MO2 virtual file system.
- `native_menu.cpp` now records the phase, exception address, and access-violation read/write target separately for the native button call, text-reference cleanup, and display-text cleanup. This is diagnostic instrumentation only; it does not yet fix the root-row failure.

## Recovery r7 runtime capture (2026-10-03 12:07)

- The supplied log starts with the stale banner `build=r13`, but later confirms `Native menu comparison diagnostics r14 enabled`; the installed behavior matches the r7 diagnostic package.
- The registry has `rootRows=1`: one `Mods` submenu under Game Options and one child action. The native root has four rows and eight visual slots. Both observed root injections report `attempted=1 added=1 faulted=0`; opening the submenu also materializes its child row successfully. This resolves the prior r6 root-button access violation for this recovery branch, but does not create a top-level Mod Settings category.
- The Graphics observer is skipped because the Hub has no Graphics rows registered. The host therefore does not inject into the vanilla Graphics page in this run. The log has no loaded-GFX identity and cannot establish which file won in MO2's virtual file system.
- The GFX in `package-stage-recovery-button-phase-r7/menu/win/02_040_optionsetting.gfx` is 46,416 bytes with SHA-256 `547CDF0D4FC02348ACC931E226861567B5E9187E8A011F259BD6236377863834`, matching `work/toolchain/UXM-sandbox/Game/menu/02_040_optionsetting.gfx` byte for byte. Re-extracting the game asset would reproduce this same file; first confirm the active MO2 winner before rebuilding it.

## Investigation snapshot

The following notes record the static and asset analysis that preceded the r12 runtime capture. Where those notes call route 4 a candidate, the runtime evidence below supersedes that conclusion.

## Exact target build

The installed executable is `2.7.1.0`, 87,042,128 bytes, SHA-256 `1A3547101327F65D0C76DA2F9190AC0AA66871EA42BAE2AECC61E11A8B597891`. This matches the 2.7.1.0 identity recorded in ERNativeUI's update notes.

ERNativeUI's committed `assets/menu/win/02_040_optionsetting.gfx` is a generated, already-patched movie (row capacity and widget presentation). It helps locate names and understand the current patcher, but is not a clean source for generating a new tab slot. The production asset must be derived from a clean 2.7.1.0 game movie and kept in the mod package; the installed game directory has not been modified.

## Current Mods row

`src/ermodsettingshub.cpp` registers a menu with ERNativeUI and adds a `Mods` submenu beneath `menu.root()`. ERNativeUI places that root in the existing Game Options page by extending the native page's row list. The Hub does not create or own the top icon strip.

The optional `menu/win/02_040_optionsetting.gfx` patch currently changes Game Options and Camera Options row capacity and control presentation. It does not add a Configuration tab.

The requested end state is one fixed **Mod Settings** category that opens the Hub's existing provider list directly. It will not create one top-level tab per DLL. The exit control and every game-owned category must retain their original order and behavior.

## Where the top icons live

The Configuration movie is `menu/win/02_040_optionsetting.gfx`. Its top selector is exposed to the game as `TabList`; the reverse-navigation list is `BackTabList`. The selector's item sprites are named `Item_0_%d`, and the native constructor binds the reverse list using those names.

### Exact 2.7.1.0 movie extraction

UXM extracted a clean `02_040_optionsetting.gfx` from the verified 2.7.1.0 game archives into the workspace sandbox. The file is 46,416 bytes with SHA-256 `547CDF0D4FC02348ACC931E226861567B5E9187E8A011F259BD6236377863834`. FFDec 26.3.0's SWF dump shows eight child placements (`Item_0_0` through `Item_0_7`) in each of `TabList` and `BackTabList`.

There is a material asset mismatch to resolve before extending the top strip: the ERNativeUI packaged, row-patched `02_040_optionsetting.gfx` is 45,129 bytes and contains `Item_0_0` through `Item_0_8`. That output was generated from an earlier 44,016-byte reference movie. It is not the clean 2.7.1.0 extraction and must not be treated as proof that the current game movie has a spare ninth selector slot. The exact GFX visible through the user's active MO2 profile must be the source of any new patch.

The extracted movie declares tab art as external `.tga` resources (for example `MENU_Tab_Exit.tga` and `MENU_Tab_Option.tga`); FFDec cannot render those assets when the companion game textures are absent, so its red image previews are placeholders. `TabList` and `BackTabList` contain per-item `ItemIcon`/hit-area sprites. The main timeline's script only stops at its frame markers; category selection and panel transitions are native game behavior. This means matching the icon image alone cannot create a working tab.

The clean movie's root bindings are `BackTabList -> sprite 184` and `TabList -> sprite 195`. Sprite 184 contains the tab-strip base artwork and eight regular item symbols (`Item_0_0` through `Item_0_7`); sprite 195 contains the arrows, title text, and eight selectable item symbols. The current patcher's internal `tab_list`/`back_tab_list` labels follow the opposite structural orientation even though its emitted r5 candidate places the regular clone in sprite 184 and the selected/cursor clone in sprite 195. This naming inversion is a source of confusion, but the static dump alone does not show that it caused the user's runtime overlap; do not treat changing the labels as a UI fix.

The game-side owner is `CS::OptionSettingTopDialog`:

- Constructor: RVA `0x9672C0` on both the 2.7.0 reference map and the inspected 2.7.1.0 executable; builds the selector, category list, and composite panel manager.
- Frame/navigation dispatch: RVA `0x9680D0` on both inspected builds; forwards tab input and advances the panel when selection changes.
- Category-list append: RVA `0x968CF0`; its fixed native list has capacity ten and stores `0x88`-byte category records.
- Generic category builder: RVA `0x86A0A0`; writes the category code at record offset `+0x40` and copies the associated native option data. The remaining fields, activation mapping, and safe custom-page lifetime are not fully established.
- Route 4 (`0x93CA90`) constructs the built-in category-4 Adjust Brightness page through `0x807E10`. The r12 runtime capture rules it out as a generic Hub destination.

The selector's visual placement and the native category record must agree. Adding an image or another GFX placement alone would only draw a decoration; it would not give the game a selectable category or a safe settings panel.

The local ERNativeUI class map originated from executable 2.7.0.0. Targeted Capstone disassembly confirms several mapped RVAs and storage mechanics also match 2.7.1.0, but that does not establish runtime category count, selector-to-route mapping, every record field, or a safe custom-page teardown. The existing ERNativeUI compatibility port validates other UI features, not this new top-tab path.

## Work needed for a real tab

1. Find a native page factory and lifecycle that can own a provider page without borrowing any game category's specialized panel, and validate its rows, selection, Back path, and teardown.
2. Confirm how a genuinely new category maps through the composite manager and the current top-dialog constructor, including title/help data and the live category count.
3. Extend `TabList` and `BackTabList` only after that route is known, preserving native spacing, selection, focus, transitions, and all game-owned categories. The patcher source now uses private cloned symbols for the custom artwork; this still needs an asset build and runtime check.
4. Fail closed if the executable, native page lifecycle, active GFX, category count, or selector layout differs from the validated profile.

Ten native records versus eight placements in the clean 2.7.1.0 movie is a capacity observation, not proof that another category can be appended safely. The r12 package's nine-placement movie is based on an older GFX source and is not confirmed as the active asset in the user's MO2 VFS.

## Relevant source areas

- Hub row registration: `src/ermodsettingshub.cpp`, `register_menu` callback.
- Existing host row injection: ERNativeUI `src/hooks.cpp` and `src/native_menu.cpp`, built-in Game Options materializer.
- Existing GFX-only transformations: ERNativeUI `tools/gfx_patcher/gfx_patch.cpp`; currently limited to page rows and control widgets.
- Native tab ownership and remaining evidence gaps: ERNativeUI `docs/research/architecture/configuration-class-map.md`.

## Current evidence boundary

- The `Item_0_%d` naming and native `BackTabList` binding are confirmed by the local analysis. The clean 2.7.1.0 movie has eight placements per selector list; the existing row-patched package asset has nine. Their difference remains an explicit compatibility issue.
- The 2.7.1.0 executable identity is confirmed locally by version, size, and SHA-256.
- The ERNativeUI host and Hub currently have no public top-category registration path; the Hub's `Mods` row is inserted through the existing Game Options materializer.
- The r12 capture observed native codes `0,1,2,3,5,6,7,8,4,9`, with Exit still at code 9. Code 4 builds the Adjust Brightness panel and is not a safe Hub route.
- On code 4, r12 logged `panelTypeBefore=6`, `panelTypeAfter=6`, `nativeRows=0`, `visualRows=2`; both Hub button constructors faulted and committed zero rows.
- r12 did not record Keyboard/Mouse or Graphics panel state because its diagnostic observers were skipped. The recovery r4 candidate adds read-only observers for those original constructors.
- The r12 package predates the private-symbol cloning now present in the patcher source; its icon carry/duplication report remains consistent with edits to shared vanilla icon symbols.
- The runtime category count, full `MenuOptionCategory` layout, icon/title binding, and custom-page factory/teardown remain unverified. Native vector capacity ten does not prove a free slot.
- No game was launched and no GFX was installed during this investigation.

## Scope distinction

The native-tab work changes navigation and presentation. It does not alter the existing Save/live-apply callbacks or make an INI reload safe for a provider that does not implement a live-reload contract.

## 2026-10-02 tool and profile status

- UXM 2.4.2 and FFDec 26.3.0 are already available in the workspace. UXM extracted the clean 2.7.1.0 movie for inspection; no game archive or installed game file was modified.
- The currently open portable MO2 points to its `Arial` profile, but that profile's `modlist.txt` contains only MO2's generated header and its local `mods` directory is empty. This snapshot therefore cannot establish which `02_040_optionsetting.gfx` the earlier successful in-game profile mounted. The 45,129-byte ERNativeUI movie remains a candidate, not a confirmed active-VFS source.
- Ghidra 12.1.4 and 12.1.3 both imported the exact 2.7.1.0 executable, but targeted headless Java scripts failed during script-bundle loading; the 12.1.4 trace includes `AccessDeniedException` while resolving/closing Ghidra JAR files. A similar script-loading symptom is tracked upstream in [Ghidra issue #9551](https://github.com/NationalSecurityAgency/ghidra/issues/9551), but the local Windows trace is not proven to share that issue's root cause. Bounded Capstone checks below validate the core constructor, dispatcher, append helper, generic category builder, and candidate route 4; the rest of the runtime integration remains unresolved.
- No cosmetic-only tab patch was produced. The category record, icon binding, custom panel factory, and teardown still need a validated native path before the new icon can be made selectable and safe.

## 2026-10-02 bounded native disassembly

A workspace-only Capstone 5.0.6 disassembly of the exact sandboxed Elden Ring 2.7.1.0 executable independently confirms the top-dialog constructor (`0x9672C0`), tab/panel dispatcher (`0x9680D0`), category append helper (`0x968CF0`), and generic category builder (`0x86A0A0`) at the mapped RVAs. The append helper increments the category count, enforces a limit of ten, and stores `0x88`-byte records. The generic builder stores the supplied category code at record offset `+0x40`.

The target constructor contains nine syntactic calls to the append helper, but several are behind runtime/feature checks. Nine therefore is not a confirmed live category count. This static pass revalidates the code locations and storage mechanics; it does not yet establish the full category record, the active runtime count, or a safe external panel factory/teardown contract.

The dispatcher at `0x93D730` accepts values `0..9` and has ten built-in panel routes. The observed top constructor appends categories using codes `0,1,2,3,5,6,7,8,9`; several calls are conditionally gated, so the live count can vary. A further bounded disassembly of the exact 2.7.1.0 executable shows the panel cache write using `categoryCode` directly as the slot index: `manager+0x68+categoryCode*8`. Thus all ten cache slots correspond to built-in codes `0..9`; there is no spare code 10. Route 4 (`0x93CA90`) is a real built-in route and creates category-4 native option data (`0x1AE78`) through `0x807E10`, even though the current constructor does not append a category-4 record. Its wrapper calls the generic panel factory at `0x95FD30` with route-specific type tag `6`. Route 5 (`0x93CE10`) builds native category-5 data (`0x1AEDC`) and calls that factory with type tag `7`; ERNativeUI's existing public `network` destination maps to native category 5, not 4. The r12 runtime capture rejects route 4 for Hub rows: it opens the specialized Adjust Brightness shell (`nativeRows=0`, `visualRows=2`), and generic row construction faults. Route 4 must remain disabled for the Hub unless a different panel-construction path is independently established. The clean 2.7.1.0 GFX has only eight placements per selector list, so native and visual routing both need a validated path that preserves the existing category records and their lifetimes.

## 2026-10-03 read-only Ghidra follow-up and r7 capture

- Ghidra 12.1.4 headless decompilation now works against the imported, exact 2.7.1.0 project when launched with the workspace-bundled JDK 21. The earlier script-bundle loading failure recorded above is superseded; no executable or game archive was changed.
- The decompilation of dispatcher `0x93D730` confirms code 4 always selects the built-in Adjust Brightness wrapper (`0x93CA90`, factory tag 6). Game Options uses a different wrapper/factory tag (2) and its own materializer (`0x959DF0`). This reinforces that category 4 cannot become a generic Hub panel merely by changing the GFX or inserting its category record.
- The 12:07 capture's `build=r13` banner is stale; later lines enable diagnostic revision r14. Its provider registers `rootRows=1`, and its Game Options hook adds one `Mods` submenu (`attempted=1 added=1`). This is the recovery fallback placed inside Game Options, not a dedicated Configuration tab. The Keyboard/Mouse and Graphics observers are explicitly skipped in this run, so this log does not inspect the blank Graphics page.
- The recovery r7 movie is the clean 2.7.1.0 extraction (`46,416` bytes, SHA-256 `547CDF0D4FC02348ACC931E226861567B5E9187E8A011F259BD6236377863834`). Re-extracting that same game asset would reproduce identical bytes. The log does not record the active MO2 VFS winner, so a conflicting loose GFX from another enabled mod remains unruled out; verify the winning `menu/win/02_040_optionsetting.gfx` in MO2 before modifying or rebuilding any movie.

## 2026-10-03 r13 capture and r8 archive audit

- The 12:07 capture confirms the current recovery route is stable in this run: native Game Options materialized four rows in eight visual slots; ERNativeUI inserted one `Mods` submenu (`attempted=1 added=1`). Opening it materialized its one child button, and the callback completed. This is a submenu in the native Game Options page, not a separate top-level tab.
- The same log explicitly skipped Keyboard/Mouse and Graphics observers because no Hub rows were registered for those categories. Thus it does not establish why their pages look blank, nor whether the currently mounted GFX is the clean package copy.
- Recovery r7's packaged `menu/win/02_040_optionsetting.gfx` and the clean extracted `menu/02_040_optionsetting.gfx` are both 46,416 bytes with SHA-256 `547CDF0D4FC02348ACC931E226861567B5E9187E8A011F259BD6236377863834`. Re-extracting the same exact resource is unnecessary; MO2's winning virtual file is still unknown.
- The archive named `ERModSettingsHub-Recovery-Panel-Observers-2.7.1.0-r8.zip` was audited against the r10 package. Its `ERNativeUI.dll` has the same SHA-256 as r10 (`6C37762330CB9FCFBAFF56176E0D4D49D75049936289F776DF3240DF3BF51C3D`), and the supplied r10 log explicitly says the Keyboard/Mouse and Graphics observers were skipped. The archive's README promise of `native panel state` snapshots was incorrect; this package cannot collect those snapshots. Its Hub DLL is different, and its GFX is the clean 2.7.1.0 extraction.
- The r8 instructions requesting `valid`, `nativeRows`, `visualRows`, vtable, and option-data pointer should not be followed with that archive. The current r10 capture also contains no native category-list or panel-route records even though its README says they should appear; this is a logging gap, not evidence that those categories were correct.

## 2026-10-03 follow-up: r7 screenshots and active GFX

- The supplied r7 log records one `Mods` row under Game Options and reports successful materialization (`attempted=1 added=1 faulted=0`). It does not insert a Configuration category. It also explicitly skips the Keyboard/Mouse and Graphics observers, so it cannot explain those panel contents.
- The recovery r6/r7/r8 stages all carry the clean extracted movie, 46,416 bytes with SHA-256 `547CDF0D4FC02348ACC931E226861567B5E9187E8A011F259BD6236377863834`. In contrast, the experimental native-tab r5-r12 stages carry a 46,618-byte movie with SHA-256 `C0D78E3CAEAA09DD4C333C53B1D81A89DC64DCDCEEFAC37D5A6848E7927E0FD9`.
- The user's r7-era screenshots show the Keyboard icon repeated where Graphics should be, with Graphics/Exit absent from the visible strip. This is inconsistent with the clean movie packaged in r7 and points strongly to another `02_040_optionsetting.gfx` winning in MO2's virtual file system, likely an older experimental package. This remains an inference until the MO2 Data-pane winner is captured.
- Re-extracting the same 2.7.1.0 asset is redundant. First make the recovery package's `menu/win/02_040_optionsetting.gfx` the active MO2 winner and disable other old Hub/native-tab GFX copies. Then use r8 to record native Keyboard/Mouse and Graphics page state. A positive `nativeRows` count with a blank image would point to a movie/layout mismatch; zero rows would point to the native page builder/route.
- Candidate route for further controlled research: keep the custom category record and its text identity, but make category 4's dispatcher open the already-supported Game Options panel shell/materializer while suppressing that shell's four vanilla rows only within a thread-local Mod Settings route. This would reuse the validated panel type and existing Hub root page instead of injecting a button into the specialized Adjust Brightness shell. The route substitution, panel title, callback, Back behavior, and teardown remain unvalidated; it is not enabled in r8 and must not be treated as a fix yet.

## 2026-10-03 r10 user capture and clean-GFX control

- The supplied log identifies itself as `build=r10`. The Hub registers `pages=3`, `rows=27`, and `rootRows=1`. Two Game Options constructions each report `nativeRows=4`, `visualRows=8`, followed by one successful root submenu insertion (`attempted=1 added=1 faulted=0`). This confirms the fallback submenu was inserted; it does not confirm a standalone top-level category.
- At startup, r10 reports that its Keyboard/Mouse and Graphics diagnostic observers were skipped. The log has no `native panel state`, `native category list`, or `native panel route` record. Consequently it cannot diagnose the blank panels or prove which tab/category dispatch occurred in the screenshots.
- The exact r10 package movie is 46,618 bytes with SHA-256 `C0D78E3CAEAA09DD4C333C53B1D81A89DC64DCDCEEFAC37D5A6848E7927E0FD9`. The clean movie extracted from the exact 2.7.1.0 sandbox is 46,416 bytes with SHA-256 `547CDF0D4FC02348ACC931E226861567B5E9187E8A011F259BD6236377863834`. If the r10 movie wins in MO2, it is a strong candidate for the repeated/shifted icons, but the current log does not prove it was the active winner.
- `ERModSettingsHub-GFX-Control-2.7.1.0-r1.zip` keeps the r10 host, Hub, INI files, and other packaged resources while replacing only `menu/win/02_040_optionsetting.gfx` with the clean extraction. It isolates the movie as one variable; it is not a fix for blank panels or a dedicated tab. Confirm the MO2 Data winner before interpreting the next screenshot.

## 2026-10-04 r16 capture and structural GFX comparison

- The supplied log identifies `build=r16`. Its title menu records the eight native categories `0,1,2,3,5,6,7,8`; in-game it records nine, adding category `9` (`Quit Game`). The native Mod Settings insertion is skipped at both menu constructions because `hooksReady=0`. The existing Game Options fallback row is materialized successfully (`route=root-main`, `attempted=1 added=1`). This run therefore does not test a dedicated Mod Settings tab.
- Keyboard/Mouse (category 7) and Graphics (category 8) both reach native panel transitions and invoke the text resolver for their menu text. The generic read-only row observer reports `countAt1AF0=6`, `capacityAtB14=1`, `countWithinCapacity=0`, and `injectLayoutValid=0` for both. Those inconsistent fields show that this observer does not validate their row layout; they do not prove that the native panels contain no rows or that the GFX is the cause.
- The r16 stage movie and the UXM extraction are byte-identical: 46,416 bytes, SHA-256 `547CDF0D4FC02348ACC931E226861567B5E9187E8A011F259BD6236377863834`. The older 46,618-byte r5 candidate adds `Item_0_8` to both selector lists at `x=4640`, `y=-6920`; it reuses the vanilla regular/selected item symbols (`183`/`193`) instead of private clones. That is consistent with icon-state bleeding, but does not prove which loose file MO2 mounted during the earlier screenshots.
- The current GFX patcher source now clones five tab symbols privately, but no new GFX produced by that source has been validated in-game. The packaged 45,129-byte legacy movie has different character IDs and is not the exact 2.7.1.0 extraction. Check MO2's winning `menu/win/02_040_optionsetting.gfx` before attributing any current screenshot to the r16 stage.
- The newly supplied 00:51:19 r16 log adds the category-list observer output: title-menu categories are `0,1,2,3,5,6,7,8`, and the in-game list is `0,1,2,3,5,6,7,8,9`; both report `duplicateCodes=0`, and neither contains a Hub category. This confirms the selector is not receiving duplicate native category records in this run. The `hooksReady=0` skip is why the native Hub category is absent; the separate Game Options submenu does materialize.
- The transition observer records selection through every available vanilla category in both contexts. For the in-game list, code 9 (`Quit Game`) occupies selected index 8. The log therefore confirms the game-side category/navigation path reaches Graphics and Quit Game, while it does not show whether their Scaleform artwork or content rendered correctly.
- The read-only row snapshots are coherent for Camera (7/7), Display (10/12), Sound (4/12), and Network (4/7). Keyboard/Mouse and Graphics both report six at `+0x1AF0` against capacity one at `+0xB14`, so this observer's generic capacity interpretation is invalid for those pages and cannot diagnose their rendered rows. Controller uses a different panel vtable from the generic page observer.
- The log does not record the loaded GFX filename, hash, or MO2 VFS winner. It cannot identify which movie supplied the repeated icon; the absence of duplicate native category codes shifts the remaining check to the active movie/symbol linkage, but does not prove an asset conflict.
