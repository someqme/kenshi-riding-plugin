# RidingPlugin — 索引卡

Kenshi 骑乘 Mod（上/下马、坐姿、随坐骑移动转向、可骑乘战斗）。C++ **单文件** DLL `RidingPlugin.cpp`，由 RE_Kenshi 注入 Kenshi 1.0.65 x64。

**细节全在 `doc.md`**（旧 AGENTS.md 的逐字内容，92 KB）。本文件只留每次都用得上的指针与红线。

## 文档分工（按需 Grep，都别整读）

| 文件 | 装什么 | 什么时候开 |
|---|---|---|
| `doc.md` | 当前实现、约束、格式、键位、tools 用法 | 要动实现前 |
| `TASK.md` | 在建阶段划分与验收标准 | 动姿势/战斗代码前 |
| `RidingPlugin_RE_NOTES.md` | 逆向手册（§0 禁 hook、§12 RVA 表、§17 骑乘战斗、§18 静态定址、§19 离线读游戏数据、§20 例本 `ride.dll` 的做法、§21 `AnimationState` 生命周期＝mask 泄漏定案） | hook 新函数 / 直调新方法 / 做新逆向 |
| `RidingPlugin_HISTORY.md` | 历史档（失败方案、原始实测数字、被推翻的结论；**§R ＝ TASK.md 已闭合阶段的原文**、**附录 `PRETRIM-20260831` ＝ 减重前快照**） | 同类症状复发 / 想知道「当初为什么这么做」 |
| `TEST_REQUIRED.md` | 已收实测的判据与读数（**欠账清零**：✅ T32 三档坐姿 ＋ ✅ T33 坐坐垫不参战 ＋ ✅ T34 SEH 保险网 ＋ ✅ 护主删除/65260 大体形（2026-09-12）；⏳ 只剩 ✅ T31 机器判据「未测量」脚注）。顶块记录着回滚件链、日志存档、以及当前件身份（`Build\` ＝ `mods\` ＝ `release\` ＝ `6F456C3B…` ＝ 实测通过件，⏳ 尚未 commit/tag 发 v1.9） | 改动要出货前 |

阶段做完：结论进 `doc.md`，经过进 HISTORY，TASK 条目删掉。

⚠️ **旧文档（TASK / RE_NOTES / HISTORY / TEST_REQUIRED，共 53 处）里写的「`AGENTS.md`」指的是现在的 `doc.md`** —— 它们点的节名（「关键机制」「姿势维持」「人形动画表」「待办」）都在 `doc.md` 里。这些引用没有逐个改，因为 HISTORY 是记录、改了就失真。

## 红线（违反会毁东西，理由都在别处）

- **构建** `build_ridingplugin.cmd`（VS2010 v100，全绝对路径）→ `D:\KenshiModDev\Build\RidingPlugin\`。仓库这份是唯一真脚本，**不要复制出第二份**。部署要复制到 `mods\RidingPlugin\`，**游戏运行时 DLL 被占用，先关游戏**。
- **`release\` 只放实测过的构建 —— 换出货件必须先有对应实测**，探针与未实测改动**故意不进玩家包**。当前三处一致（2026-09-12 起）：`release\` ＝ `Build\` ＝ `mods\` ＝ **`366080 B` / md5 `6F456C3B5E9F2E4D27B304A0EAFE9FB8`**（＝ v1.8 `e71de118…` ＋ **删 P4-2 护主下令** ＋ **`kCombatRaceDeny` 把国王/卷缩者 65260 放进大体形、人物不战斗**；用户实测「通过」；源码快照 `D:\KenshiModDev\RidingPlugin_src_6f456c3b.cpp`、回滚件 `RidingPlugin_prev_e71de118.dll`）。⏳ **尚未 commit / tag / 发 v1.9**。前一版 **v1.8 `e71de118…`** 五渠道已闭环（tag/zip/GitHub/Nexus/工坊 id `3787280895`），本体由 git tag `v1.8` 保管。再前一版 **v1.7 `0ed47dd6…`** 由 git `81a236a` 保管。⬛ 判身份看 md5；回滚件链见 `TEST_REQUIRED.md` 顶块。出货 `README.txt` 的「BOM ＋ 纯 LF」由 `.gitattributes` 的 `-text` 守着。**X-6（上马那一瞬的姿势弹跳）用户已裁定不修、挂起**（`TASK.md` X-6）。
- **两个 hook 永久禁用**（`beingCarriedUpdate` / `updateAnimationTransforms`）。理由写在 `RidingPlugin.cpp` 的 `DISABLED HOOKS` 块（约 8749 行）与 RE_NOTES §0。`KenshiLib::AddHook` 固定拷 5 字节、无 trampoline ⇒ **没有新实测就不要重新注册**。
- **`getAnimationData()` 是 `operator[]`，miss 会插 NULL**。查存在性一律走 `FindAnimData()`（`RidingPlugin.cpp:4160-4173`；2026-09-02 复核过 —— 旧记的 `:2463` 是上马信封夹取，不是它）。
- ⚠️ **构建不是逐字节可复现的**：同一份源码、同一个脚本连编两次，字节数一样（`308224`）但 md5 不同（`601E39FF…` → `635436E9…`，2026-09-02 实测）。`/GL` + `/LTCG` 每次链接都会换掉时间戳与 LTCG 签名。⇒ md5 仍然是**「哪一份二进制被实测过」**的唯一标识（这条不变），但**反过来不成立**：不能靠「重编一次比 md5」来验证「`release\` 里那份是这份源码编的」。要验源码对应关系只能靠部署时留的 `RidingPlugin_src_<md5 前 8 位>.cpp` 快照或 git。
- **`RE_Kenshi_log.txt` 绝不整读、绝不贴进对话**（见过 25.9 MB）。它一次性：新会话一开就把整份顶掉 ⇒ **测完先复制出来再重启游戏**。判读只用 `python tools\ridelog.py`，判据细则在脚本自己的输出里，别在文档复述。
- **`stringID` 后缀一个都不许猜**（已被证伪两次）——用 `tools\race_by_name.py`。
- **地址的唯一真相源是 `RidingPlugin_RE_NOTES.md`（整体，不是某一张表）**。别的文档复述任何 exe 地址时，**那一行必须带 `§nn` 指针**指回去；改完文档跑 `python tools\addrcheck.py --all`，**TODO 与「无真相源」两栏必须为 0**（`--refs` 顺带核「`TASK.md <段名>`」引用没有悬空）。⚠️ HISTORY 是记录，它的地址是当时快照、**故意不回填**。
- 重复性检查全在 `tools\`（座位表 / race key / 静态定址 / 调用图 / 离线读游戏数据 / 日志判读 / 文档自检），**别再往 `%TEMP%` 写一次性脚本**。用法见 `doc.md`。
- 反编译走本机 Ghidra（`D:\KenshiModDev\revtools\`，仓库外）；地址与结论进 RE_NOTES，**不要把反编译读数当实测**。

## 上下文管理

**读取**
- 先 grep/glob 定位，再按行号范围读需要的片段；大文件不无脑整读。
- 已读过、内容没变的文件不重复读。

**执行**
- 大任务先拆成子步骤列出来，一步步做。
- 每步做完立刻验证（这个项目没有测试套件：**编译 → `ridelog.py` → 游戏内肉眼**，三层里能做哪层做哪层），不要攒到最后一次性排查。
- 试错过程（反复失败的调试）完成后自己总结成一两句结论，不保留过程细节。

**输出**
- 中间产物、草稿、探索性代码放临时文件，不整段贴进对话。
- 改文件用编辑操作，不贴修改前后的完整文件内容。
- 只在真需要展示时才完整输出代码，其余用简短描述 + 文件路径。

**压缩**
- 当前任务背景交代清楚、后续不再需要时，主动提示可以 `/compact` 或开新会话（阶段收尾就重开，别等自动 compact）。
- 跨会话要记住的项目知识写回文档，不依赖对话历史。
