# NativeTweaks INI template (for mod authors)

NativeTweaks lists the INI of every loaded DLL mod (an `.ini` next to the `.dll`)
in its in-game tab. Add the optional sections below to your own INI and NativeTweaks
shows proper names, descriptions, switches, sliders and lists. Your DLL
does not need to change, and the template is ignored by everything else.

## Example

```ini
[Settings]
bEnabled=1
iMode=2
iOpacity=80
fRollSpeed=1.0

; ---------- NativeTweaks menu template (optional) ----------
[NativeTweaks]
Name=My Mod
Description=Settings for My Mod.
Description.es=Ajustes de My Mod.

[NativeTweaks.Settings.bEnabled]
Type=bool
Label=Enable mod
Label.es=Activar mod

[NativeTweaks.Settings.iMode]
Type=enum
Label=Mode
Options=0:Off|1:Soft|2:Normal|3:Strong
Options.es=0:Desactivado|1:Suave|2:Normal|3:Intenso

[NativeTweaks.Settings.iOpacity]
Type=int
Label=Opacity
Min=0
Max=100
Step=5
Category=Display

[NativeTweaks.Settings.fRollSpeed]
Type=float
Label=Roll speed
Description=Multiplier for roll speed.
Min=0.5
Max=2.0
Step=0.05
Category=Movement
Category.es=Movimiento
```

## Sections

- `[NativeTweaks]`: `Name` is the page title; `Description` is its help text.
- `[NativeTweaks.<Section>.<Key>]` describes the value `<Key>` in `[<Section>]`.
  The key is the text after the last dot, so section names may contain dots.

## Fields

| Field | Meaning |
|---|---|
| `Type` | `bool`, `int`, `float` or `enum`. |
| `Label` | Row name, max 64 characters. |
| `Description` | Help text shown at the bottom of the menu. |
| `Category` | Groups rows into a submenu with this name. |
| `Min`, `Max`, `Step` | Range for `int`/`float`. `Step` defaults to 1. |
| `Options` | `enum` values: `value:Shown text|value:Shown text` (max 32). |
| `Hidden` | `1` hides the key from the menu. |

Any text field can be translated by adding `.<lang>`: `Label.es`, `Description.es`,
`Category.es`, `Options.es`, `Name.es`. Currently `en` and `es` are recognised, plus
`zh` in the Chinese edition (save the INI as UTF-8).
Without a translation the plain field is used.

## How each type is shown

- `bool`: an On/Off switch. Your current spelling is kept when saving
  (`0/1`, `true/false`, `on/off`, `yes/no`, including capitals).
- `enum`: a list with your `Options`; the INI receives the value before `:`.
- `int` with `Min >= 0` and `Max <= 255`: a native slider showing the real value.
- Other `int` / `float` ranges: a list of every step, up to 32 entries
  (e.g. 0.5 to 2.0 with step 0.05). The number of decimals follows `Step`/`Min`/`Max`.
- Ranges with more than 32 steps (e.g. 0 to 5000 with step 1): a list of about
  30 round values (here every 200), plus the current value. Pick a `Step` yourself
  if you want specific values.

## Current limitation: no free text

The in-game menu (ERNativeUI) cannot show editable text fields yet, so only
switches, sliders and lists are possible. In practice:

- Every key you want in the menu needs a template with `Type=bool`, `enum`, `int`
  or `float`. Free text (names, paths, key lists...) cannot be edited in-game: set
  `Hidden=1` and document it for manual editing.
- Without a template, a key is only shown when it is clearly a switch: `true/false`,
  `on/off`, `yes/no`, or `0/1` under a key such as `bEnabled` (`b` + capital).
  Everything else is left out of the menu.
- A template that does not fit the current value (e.g. the value is not in
  `Options`) also leaves the key out.
- Skipped keys and the reason are written to `NativeTweaks.log` (with `EnableLog=1`).
  A mod with nothing to show gets no page.
## Notes

- Values are written to your INI only for changed rows. A `.bak` backup is made first;
  comments and order are preserved.
- Avoid `;` or `#` after a space in template texts: they start an INI comment.
- Changes apply after a restart unless your DLL exports the NativeTweaks live-reload
  API (`LIVE_RELOAD_API_V1.md`).
