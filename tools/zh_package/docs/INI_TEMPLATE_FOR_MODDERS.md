# NativeTweaks INI 模板（面向模组作者）

NativeTweaks 会在游戏内选项卡中列出每个已加载 DLL 模组的 INI（与 `.dll` 同目录的 `.ini`）。
在你自己的 INI 中加入下面的可选段落后，NativeTweaks 就会显示正确的名称、说明、开关、
滑块和列表。你的 DLL 无需任何修改，其他程序也会忽略这些模板段落。

## 示例

```ini
[Settings]
bEnabled=1
iMode=2
iOpacity=80
fRollSpeed=1.0

; ---------- NativeTweaks 菜单模板（可选） ----------
[NativeTweaks]
Name=My Mod
Name.zh=我的模组
Description=Settings for My Mod.
Description.zh=我的模组的设置。

[NativeTweaks.Settings.bEnabled]
Type=bool
Label=Enable mod
Label.zh=启用模组

[NativeTweaks.Settings.iMode]
Type=enum
Label=Mode
Label.zh=模式
Options=0:Off|1:Soft|2:Normal|3:Strong
Options.zh=0:关闭|1:轻度|2:普通|3:强烈

[NativeTweaks.Settings.iOpacity]
Type=int
Label=Opacity
Label.zh=不透明度
Min=0
Max=100
Step=5
Category=Display
Category.zh=显示

[NativeTweaks.Settings.fRollSpeed]
Type=float
Label=Roll speed
Label.zh=翻滚速度
Description=Multiplier for roll speed.
Description.zh=翻滚速度倍率。
Min=0.5
Max=2.0
Step=0.05
Category=Movement
Category.zh=移动
```

## 段落

- `[NativeTweaks]`：`Name` 为页面标题；`Description` 为页面说明文字。
- `[NativeTweaks.<段名>.<键名>]` 描述 `[<段名>]` 中的 `<键名>`。
  键名取最后一个点之后的文字，因此段名中可以包含点。

## 字段

| 字段 | 含义 |
|---|---|
| `Type` | `bool`、`int`、`float` 或 `enum`。 |
| `Label` | 行名称，最多 64 个字符。 |
| `Description` | 显示在菜单底部的说明文字。 |
| `Category` | 将各行归入以此命名的子菜单。 |
| `Min`、`Max`、`Step` | `int`/`float` 的范围。`Step` 默认为 1。 |
| `Options` | `enum` 的取值：`值:显示文字|值:显示文字`（最多 32 项）。 |
| `Hidden` | `1` 表示在菜单中隐藏该键。 |

任何文字字段都可以通过添加 `.<语言>` 进行翻译：`Label.zh`、`Description.zh`、
`Category.zh`、`Options.zh`、`Name.zh`。可识别的语言：`en`、`es`、`zh`（简体中文）和
`zht`（繁体中文；缺失时回退到 `zh`）。没有翻译时使用不带后缀的字段。
中文文字请以 UTF-8 编码保存 INI。

## 各类型的显示方式

- `bool`：开/关开关。保存时保留你原来的写法
  （`0/1`、`true/false`、`on/off`、`yes/no`，包括大小写）。
- `enum`：显示 `Options` 列表；INI 中写入 `:` 前面的值。
- `Min >= 0` 且 `Max <= 255` 的 `int`：显示真实数值的原生滑块。
- 其他范围的 `int` / `float`：列出每一步的数值，最多 32 项
  （例如 0.5 到 2.0，步长 0.05）。小数位数取决于 `Step`/`Min`/`Max`。
- 超过 32 步的范围（例如 0 到 5000，步长 1）：显示约 30 个取整的数值
  （此例为每 200 一个），外加当前值。如需特定数值，请自行设置 `Step`。

## 当前限制：不支持自由文本

游戏内菜单（ERNativeUI）目前还无法显示可编辑的文本框，因此只能使用开关、滑块和列表。具体而言：

- 每个想在菜单中显示的键都需要带有 `Type=bool`、`enum`、`int` 或 `float` 的模板。
  自由文本（名称、路径、按键列表等）无法在游戏内编辑：请设置 `Hidden=1`，
  并在说明中注明需手动编辑。
- 没有模板时，只有明显是开关的键才会显示：`true/false`、`on/off`、`yes/no`，
  或位于 `bEnabled` 这类键（`b` + 大写字母）下的 `0/1`。其他键都不会出现在菜单中。
- 如果模板与当前值不符（例如当前值不在 `Options` 中），该键也不会显示。
- 被跳过的键及原因会写入 `NativeTweaks.log`（需 `EnableLog=1`）。
  没有任何可显示内容的模组不会生成页面。

## 注意事项

- 只有被修改的行才会写入你的 INI。写入前会先创建 `.bak` 备份；
  注释和顺序都会保留。
- 模板文字中不要在空格后使用 `;` 或 `#`：它们会被当作 INI 注释的开始。
- 除非你的 DLL 导出了 NativeTweaks 实时重载 API（`LIVE_RELOAD_API_V1.md`），
  否则修改需重启游戏后生效。
