# 把人物放到动物背上 · Kenshi Riding Plugin

让 Kenshi 玩家可以**骑乘动物**：上马 / 下马、骑手坐姿、坐骑带动骑手移动与转向、坐骑代替骑手战斗。

本项目是一个由 **[RE_Kenshi](https://github.com/BFrizzleFoShizzle/RE_Kenshi)** 注入的 C++ DLL 插件，适用于 **Kenshi 1.0.65 (x64, Steam / Newland)**。

> **Put your characters on the backs of animals in Kenshi.** Mount / dismount, a seated rider that moves and turns with its mount, and mounted combat. A C++ DLL plugin injected by RE_Kenshi, for Kenshi 1.0.65 x64.

---

## ✨ 功能 / Features

- **右键动物 →「上马」**：选中的人类会**自己走到动物身边再上马**（不是瞬间挂载）。
- 骑手稳坐坐骑背上，随坐骑移动、转身。
- **下马干净利落**：人物落地站立、立刻恢复行走与指令（v1.0 的下马瘫倒已修复）。
- 坐骑可代替骑手战斗。
- 每个物种的座椅位置可用小键盘热键**现场微调**，自动保存到 `riding.cfg`。
- 已内置 **18 个常见可骑物种**的调参（野牛、驮兽、埋骨地狼、喙嘴兽、沼泽乌龟、铁蜘蛛……）。

## 📦 前置需求 / Requirements

- **[RE_Kenshi](https://github.com/BFrizzleFoShizzle/RE_Kenshi)** 框架 —— 本插件由它加载注入。
- **Kenshi 1.0.65 x64**（Steam 版，Newland）。其他版本的函数地址（RVA）不匹配，可能无法工作甚至崩溃。

## 🔧 安装 / Installation

1. 先安装并配置好 RE_Kenshi。
2. 把 [`release/RidingPlugin/`](release/RidingPlugin) 整个文件夹（或 Releases 页里的 zip 解压后）放到：
   ```
   <Kenshi 安装目录>/mods/RidingPlugin/
   ```
   确保该目录下含有：`RidingPlugin.dll`、`RE_Kenshi.json`、`riding.cfg`、`RidingPlugin.mod`、`README.txt`。
3. 在游戏启动器里勾选启用 **RidingPlugin**。
4. 启动游戏，确认 RE_Kenshi 已正确注入（见 RE_Kenshi 说明）。

> ⚠️ Steam 的"验证游戏文件完整性"可能误删 RE_Kenshi 组件；若加载失败，重装 RE_Kenshi 即可。

## 🎮 使用 / Controls

| 操作 | 键位 |
|---|---|
| **上马** | 右键动物 → 菜单「上马」（选中的人类会走过去再上马） |
| **下马** | 选中骑手 → `Numpad 2` |
| 上 / 下微调 | `Numpad +` / `Numpad -` |
| 前 / 后微调 | `Numpad *` / `Numpad /` |
| 左右微调 | `Ctrl + *` / `Ctrl + /` |
| 座椅模式 | `Numpad 5` |
| force-sit | `Numpad 6` |
| 坐 / 站 | `Numpad 7` |
| 重置 | `Numpad 0` |
| 连续诊断（调试用） | `Numpad .` |

微调会自动写回 `riding.cfg`（跨会话持久化）。

## ⚙️ 调参文件 / riding.cfg

每个物种一行：

```
<物种>=<mode>,<up>,<forward>,<mount>,<sit>,<roll>,<pitch>,<yaw>,<posture>,<lateral>[,<ax>,<ay>,<az>,<abase>]
```

- `mode`：`0`=精确　`1`=中点　`2`=脖子
- `sit`：`0`=关　`1`=开
- `posture`：`0`=坐　`1`=站
- 骑手朝向**自动跟随坐骑前进方向**，无手动朝向参数（第 6-8 列为兼容保留的死字段）
- 第 11-14 列由插件**自动写入与恢复**（座椅锚点标定），请勿手改；旧 10 列格式仍可正常读取，捕获后自动补齐
- 物种名与游戏内 `getName()` 的字节一致（**UTF-8** 编码，请勿用会改变编码的编辑器改动物种名）。

## ⚠️ 已知限制 / Known limitations

- **读档后骑乘关系不会自动恢复**：存档不保存插件维护的骑乘状态，读档后骑手站在原地，需重新上马。
- **巨型坐骑**（乌龟、利维坦）：骑手的名字标签与镜头焦点会落到地面高度 —— 引擎对角色 UI 锚点做了地形 fallback，属**引擎级限制**，DLL 无法修正。
- 喙嘴猩猩类不可骑（已加入黑名单）；部分物种（如喙嘴兽）骑手可能不易用鼠标点中。
- 铁蜘蛛等物种坐姿需用热键**现场标定**。
- 骑乘中选中敌人下达的攻击指令会转给坐骑代打。

## 🛠️ 从源码构建 / Building

- 工具链：**Visual Studio 2010 (v100 toolset)**，x64。
- 依赖：**KenshiLib**、**Ogre**、**boost 1.60**（路径见 `build_ridingplugin.cmd`，按你的环境自行调整绝对路径）。
- 构建：运行 `build_ridingplugin.cmd` → 输出 `RidingPlugin.dll`。
- 逆向分析与实现笔记见 [`RidingPlugin_RE_NOTES.md`](RidingPlugin_RE_NOTES.md)。

## 📜 更新日志 / Changelog

- **2026-08-24**：**修复下马后人物瘫倒 / 悬空 / 无法移动**（v1.0 遗留：引擎「被携带」状态未正确解除，人物物理体被移出世界）。现在下马后人物立即落地站立、可自由走动、可正常接受指令。
- **v1.0（initial release）**：上马 / 下马、坐姿随行、坐骑代战、per-species 座椅调参。

## 📄 许可证 / License

[MIT](LICENSE)。

## 🙏 致谢 / Credits

- [RE_Kenshi](https://github.com/BFrizzleFoShizzle/RE_Kenshi) by BFrizzle —— 注入框架与 KenshiLib。
- Kenshi 骑乘社区指南（Cattrina）：<https://kenshi.fandom.com/wiki/Creating_a_rideable_animal/vehicle>
