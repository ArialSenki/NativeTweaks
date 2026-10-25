# Native Mod Settings tab preview

This preview targets Elden Ring 2.7.1.0. It adds the Hub as native top-level category 4, inserts a ninth GFX tab before the original Exit category, and opens the provider list directly. Existing native categories and the Exit route are preserved.

The GFX patcher starts from the clean 2.7.1.0 `02_040_optionsetting.gfx`, adds the Hub's two 80×80 external image resources, places them in the reserved clear frame of both `ItemIcon` states, extends `TabList` and `BackTabList`, and shifts the reverse-list depths plus title depth to preserve native alignment. It also expands Game Options and Camera Options row capacity for the host's native UI pages. It rejects movies whose recognized structure does not match the supported build.

The packaged GFX and TGA files must be mounted together under `menu/win`. If another enabled mod also supplies `02_040_optionsetting.gfx`, MO2's winner must include that mod's needed changes. Use the bundled patcher on the desired winning source if necessary; it does not overwrite its input.

Revision 2 removes a rejected `set_page_presentation` call on the game-owned Mod Settings built-in page. The previous startup log showed `providers=0`, so the client had not registered its rows and the native category hook was not armed even though the patched GFX could still be mounted. This corrects the registration failure; it does not establish that the earlier crash is resolved.

The Hub DLL compiles successfully. Runtime selection, icon display, navigation, provider rendering, save behavior, stability, and CTD behavior still require manual validation in an isolated offline profile with EAC disabled. The native hook uses executable-specific addresses and remains build-bound.
