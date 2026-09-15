# RidingPlugin standalone v1.0.68

本包允许 Steam Kenshi 1.0.68 玩家在不安装 RE_Kenshi 的情况下使用 RidingPlugin。

## 方案

- 包含 KenshiLib 0.4.0 和匹配的 7409 项 RVA 表。
- 启动 Steam 1.0.68 时，使用官方 RE_Kenshi Courgette 补丁生成旁置的
  `kenshi_x64_riding_1.0.65.exe`。
- 不修改或覆盖玩家原始 `Kenshi_x64.exe`。
- 不要与 `RE_Kenshi.dll` 同时运行。

## 已验证

2026-09-15，卸载 RE_Kenshi 后在 Steam 1.0.68 实机验证：

- 上马和坐姿正常；
- 骑手随坐骑移动、转向正常；
- 下马后可站立、移动、接受指令；
- 小型/中型坐骑战斗正常；
- 连续游玩 10 分钟无异常。

## 已知限制

当前没有公开、可靠的 Steam 1.0.68 直接 RVA 表，因此采用官方同款降级兼容路径。
原版游戏文件验证、更新或重装后，如旁置兼容副本被删除，重新运行启动器即可生成。
