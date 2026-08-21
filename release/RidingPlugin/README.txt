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
- 下马：选中骑手 -> 按 Numpad 2。
- 座椅微调（每个物种分别标定，自动保存到 riding.cfg）：
    Numpad +/-    ：上 / 下
    Numpad * / /  ：前 / 后
    Ctrl + * / /  ：左 / 右
    Numpad 3 / 9  ：高度微调（Ctrl 加大步长）
    Numpad 5      ：座椅模式
    Numpad 6      ：force-sit
    Numpad 7      ：坐 / 站
    Numpad 8 / 4  ：roll
    Ctrl + 8 / 4  ：pitch
    Ctrl + +/-    ：yaw
    Numpad 0      ：重置
    Numpad .      ：连续诊断（调试用）

调参文件
--------
mods/RidingPlugin/riding.cfg  每个物种一行，格式：
  <物种>=<mode>,<up>,<forward>,<mount>,<sit>,<roll>,<pitch>,<yaw>,<posture>,<lateral>
  mode  0=精确 1=中点 2=脖子
  sit   0=关 1=开
  posture 0=坐 1=站
  roll/pitch/yaw = 骑手朝向微调（度）
  注意：物种名与游戏内名字的字节一致（GBK 编码），请勿用会改变编码的编辑器改动。

已知限制
--------
- 巨型坐骑（乌龟 / 利维坦）上，骑手的名字标签与镜头焦点会落在地面高度
  （引擎对"被携带角色"的 UI 锚点做地形 fallback，属引擎级限制，DLL 无法修正）。
- 部分物种（如喙嘴兽）骑手可能无法用鼠标点中（引擎携带碰撞体与渲染位置偏差）。
- 铁蜘蛛等部分物种坐姿需用调参键现场标定。
- 骑手被原生携带时无法主动攻击（引擎限制）；坐骑会代为战斗。
- 注意：Steam 游戏完整性验证可能误删 RE_Kenshi 组件，遇到无法加载时重新安装
  RE_Kenshi 即可。

许可证：MIT
源码 / 更新：见 GitHub 仓库。
