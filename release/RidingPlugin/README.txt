把人物放到动物背上 — Kenshi 骑乘 Mod (RidingPlugin)
===================================================

让玩家可以骑乘动物：上马 / 下马、骑手坐姿、坐骑带动骑手移动与转向、坐骑代替骑手战斗。
本 mod 是 RE_Kenshi 注入的 DLL 插件，适用于 Kenshi 1.0.65 (x64, Steam)。

前置需求
--------
- 需要 RE_Kenshi 框架（https://github.com/BFrizzleFoShizzle/RE_Kenshi）。
- 安装 RE_Kenshi 后，本 mod 通过 mods/ 目录加载。
- 仅适配 Kenshi 1.0.65 x64，其他版本可能无法工作。

安装
----
1. 将本 RidingPlugin 文件夹整个放入 Kenshi 游戏目录的 mods/ 目录，
   使 mods/RidingPlugin/ 下含有：
   RidingPlugin.dll、RE_Kenshi.json、riding.cfg、RidingPlugin.mod、README.txt。
2. 启动游戏启动器，勾选启用 RidingPlugin。
3. 确保 RE_Kenshi 已正确注入（见 RE_Kenshi 说明）。

使用（键位）
-----------
- 上马：右键点击动物 -> 菜单选择「上马」。选中的人类会先走到动物身边，到达后再上马。
  菜单项跟随游戏语言显示：中文「上马」、英文 Ride、德文 Reiten 等
  （支持 简中/英/德/法/俄/西/葡/日/韩；其他语言保留原版菜单项，点击同样可以上马）。
- 下马：选中骑手 -> 长按右键选「放倒」即可下马（或按 Numpad 2）。
  下马后人物立即落地站立、可正常行动。
- 座椅微调（每个物种分别标定，自动保存到 riding.cfg）：
    Numpad +/-    ：上 / 下
    Numpad * / /  ：前 / 后
    Ctrl + * / /  ：左 / 右
    Numpad 5      ：座椅模式
    Numpad 6      ：force-sit
    Numpad 7      ：坐 / 站
    Numpad 0      ：重置

调参文件
--------
mods/RidingPlugin/riding.cfg  每个物种一行，格式：
  <物种>=<mode>,<up>,<forward>,<mount>,<sit>,<roll>,<pitch>,<yaw>,<posture>,<lateral>[,<ax>,<ay>,<az>,<abase>]
  mode  0=精确 1=中点 2=脖子
  sit   0=关 1=开
  posture 0=坐 1=站
  roll/pitch/yaw 为兼容保留的死字段；骑手朝向自动跟随坐骑前进方向，无手动参数。
  第 11-14 列由插件自动写入与恢复（座椅锚点），请勿手改；旧 10 列格式仍可读取。
  注意：物种名与游戏内名字的字节一致（UTF-8 编码），请勿用会改变编码的编辑器改动。

已知限制
--------
- 读档后骑乘关系不会自动恢复：读档后骑手站在原地，需重新上马。
- 巨型坐骑（乌龟 / 利维坦）上，骑手的名字标签与镜头焦点会落在地面高度
  （引擎对角色 UI 锚点做地形 fallback，属引擎级限制，DLL 无法修正）。
- 喙嘴猩猩类不可骑（已加入黑名单）；部分物种（如喙嘴兽）骑手可能不易用鼠标点中。
- 铁蜘蛛等部分物种坐姿需用调参键现场标定。
- 骑乘中选中敌人下达攻击指令会转给坐骑代打。
- 注意：Steam 游戏完整性验证可能误删 RE_Kenshi 组件，遇到无法加载时重新安装
  RE_Kenshi 即可。

许可证：MIT
源码 / 更新：见 GitHub 仓库。
