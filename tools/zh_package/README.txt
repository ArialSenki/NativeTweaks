NativeTweaks r99 - 简体中文版 - 艾尔登法环 2.7.1.0（PC）
==================================================================

本版本仅提供简体中文界面：无论游戏语言为何，模组界面均显示简体中文。
另有繁体中文版。如需英文或西班牙文界面，请使用普通版。

在游戏的“系统”菜单中添加原生的“NativeTweaks”选项卡。它会列出所有在
DLL 旁边带有 INI 文件的 DLL 模组，并允许直接在游戏菜单中修改其设置。
另外还包含“原版设置”：70 多项艾尔登法环本体选项，修改后立即生效。

运行要求
- 艾尔登法环 2.7.1.0，PC（Steam）。其他版本：找不到所需数据的选项会
  自动变灰。
- DLL 加载器：Mod Engine 2 / Mod Engine 3 或 Mod Organizer 2。
- 仅限离线游玩，并关闭 Easy Anti-Cheat。切勿在线使用。

安装
- Mod Organizer 2：将压缩包作为普通模组安装并启用。
- Mod Engine：将“DLLs”和“menu”文件夹复制到你的模组文件夹，并把
  DLLs\ERNativeUI.dll 和 DLLs\NativeTweaks.dll 作为外部 DLL 加载。
- 请将 NativeTweaks.animations.txt 保留在 DLL 旁边（各动作速度滑块
  会用到它）。

原版设置（系统 > NativeTweaks > 原版设置）
- 玩家：无敌、不会死亡、无坠落伤害、异常状态免疫（测试版）、
  HP/FP/精力恢复、装备重量、韧性、无限韧性。
- 资源与消耗：无限精力/FP/道具/箭矢、战技不消耗 FP、精力消耗、
  法术 FP 消耗、骨灰消耗。
- 战斗：造成/受到的伤害、造成/受到的削韧、异常状态累积、
  一击必杀、武器/自身/敌人增益持续时间（测试版）。
- 玩家速度：整体、攻击、施法、翻滚、移动和跳跃速度。
- 敌人：血量、韧性、无限韧性、速度、不攻击/不移动/无 AI。
- 灵马：不会死亡、速度、无坠落伤害（测试版）。
- 卢恩与掉落：死亡时保留卢恩、获得卢恩倍率、道具掉落率、武器强化
  消耗。
- 世界：设置/冻结时间、强制天气（测试版）。
- 地图与传送：显示所有赐福、显示完整地图、随处快速传送（附近有敌人
  时仍无法传送）。
- 镜头：视野、镜头距离、关闭自动旋转/自动回正。
- 画面：关闭暗角、色差、TAA、动态模糊、景深（测试版）；全距离动画。
- 显示与系统：解锁帧率 + 帧率上限、超宽屏、暂停键（Esc / Start）、
  跳过开场标志。
- “恢复游戏默认值”会关闭所有选项并恢复游戏原始数值。

关于（测试版）选项
标有（测试版）的选项依赖尚未完全确认的游戏结构。它们可能只是偶尔生效、
需要重新读取，或在你的环境中完全无效。它们绝不会修改你的存档；关闭后
即恢复游戏数值。

模组设置与 INI 模板
- 任何在 DLL 旁边带有 INI 的 DLL 模组都会获得独立页面。模组作者可以在
  INI 中添加可选的 [NativeTweaks] 模板，以显示正确的名称、说明、滑块
  和列表：参见 docs\INI_TEMPLATE_FOR_MODDERS.md。
- 保存时会写入 INI（并先创建备份）。如果模组的 DLL 导出了实时重载 API
  （docs\LIVE_RELOAD_API_V1.md），修改会立即生效；否则在下次启动游戏
  时生效。

r99 更新内容
- 新增简体中文版和繁体中文版（界面、说明文档及 ERNativeUI 翻页文字
  均为中文）。
- 修正“自定义血条长度”开关显示了错误的说明文字。
- INI 模板支持 zh（简体中文）和 zht（繁体中文，缺失时回退到 zh）。

r98 更新内容
- 修正读取存档约 15 秒后游戏闪退的问题（两个后台线程同时扫描游戏的
  单例对象）。

注意事项
- 本模组原名 Mod Forge：首次启动时会把旧的 ERModSettingsHub.ini 复制为
  NativeTweaks.ini；请删除旧的 Mod Forge 文件。
- 选项保存在 DLLs\NativeTweaks.ini（[Vanilla] 段）。不会写入你的存档，
  除了游戏本身会保存的内容（例如开启选项时获得的道具或卢恩）。
- 部分参数类选项（敌人血量/韧性、获得卢恩）仅对之后读取的敌人生效：
  请在赐福处休息。
- GFX 文件会替换原版的选项菜单文件。与其他替换
  menu\win\02_040_optionsetting.gfx 的模组不兼容。
- 使用 Mod Organizer 2 时，请确认 MO2 中设置的游戏路径（gamePath）指向
  正确的 ELDEN RING\Game 文件夹。
- 报告问题：在 NativeTweaks.ini 和 ERNativeUI.ini 中设置 EnableLog = 1，
  然后发送 DLL 旁边的 NativeTweaks.log / ERNativeUI.log。
- 卸载：删除文件即可。

致谢 ==============================================================
- techiew - EldenRingMods（MIT）：镜头、画面、暂停和帧率补丁位置。
- uberhalit - EldenRingFpsUnlockAndMore：帧率相关位置。
- veeenu - eldenring-practice-tool：结构偏移交叉验证、赐福。
- The Grand Archives - Elden Ring CT：传送限制研究。
- soulsmods - Paramdex：参数结构。
- Dasaav-dsv - from-singleton（MIT/Apache-2.0）：单例查找移植自其扫描
  方法。
- fromsoftware-rs、FreecamMod：运行时结构研究。
- Flammrock - ERNativeUI（MIT）：本模组所基于的原生菜单框架。
  NativeTweaks 是独立项目，并非由 Flammrock 制作或认可。
  ERNativeUI 许可证：licenses\ERNativeUI\。
