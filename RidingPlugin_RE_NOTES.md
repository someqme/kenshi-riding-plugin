# RidingPlugin 逆向笔记（2026-08-13）

目标：解决"跑动甩飞/晃动"。本笔记记录对 kenshi_x64.exe 的只读逆向结论。

工具：
- `pefile`（RVA→文件偏移、pdata 函数边界、E8/E9 调用扫描、字符串引用扫描）
- `capstone`（x64 反汇编）
- VS2022 `dumpbin /DISASM /RANGE`（交叉验证）
- 分析脚本：`C:\Users\SomEQ\AppData\Local\Temp\opencode\kenshi_disasm.py`
- 反汇编缓存：`C:\Users\SomEQ\AppData\Local\Temp\opencode\{beingCarriedUpdate,getPickedUp,_carryMode,pickupObject}.txt`

游戏二进制：`kenshi_x64.exe`，ImageBase `0x140000000`，.text 含大量 hotpatch jmp 跳板（patch stub 表）。

---

## 0. 最重要的发现：KenshiLib 的 RVA 表对 carry/骨骼系函数系统性不可靠

| 函数 | KenshiLib 头文件 RVA | 实测真实入口 | 证据 |
|---|---|---|---|
| `AnimationClass::beingCarriedUpdate` | 0x5B5200 | **0x5B5980** | 0x5B5200 落在无关结构体构造函数[0x5B50F0..0x5B5263)指令中部；真实函数[0x5B5980..0x5B5C1B)有干净 prologue，ABI 与插件 hook 声明完全一致 |
| `AnimationClass::updateAnimationTransforms` | 0x5B0E30 | 未知（待定位） | 0x5B0E30 在更大函数中部（用未初始化 rsi/rbp/rax），pdata[0x5B0E19..0x5B0E6E)只是其子区 |
| `Character::pickupObject` | 0x5CF810 | **0x5CFF90** | 0x5CF810 落指令中部（0x5CF78E 才是 `mov rax,rsp` 起点）；真实 pickupObject 语义完整（见 §4） |
| `Character::getPickedUp` | 0x5CE610 | **0x5CED90** | 0x5CE610 指向无关 getter[0x5CE5F0..0x5CE66C)；0x5CED90 才设 `_isBeingCarried=1` |
| `Character::_carryMode` | 0x5CE240 | **0x5CE1E0** | 0x5CE240 偏移 0x60、落指令中部 |
| `AnimationClass::update`（_NV） | 0x5B6140 | 0x5B5C30 | 0x5B6140 偏移 0x110 |
| `AnimationClass::updateThreaded` | 0x5B3FF0 | 非该函数 | 0x5B3FF0 是字符串比较循环中部 |

推论（重要）：
1. **旧 beingCarriedUpdate hook 从未真正拦截 carry 定位**。头文件注释"hook 生效但 rBip 仍跳"的结论不可信——挂错地址。交接摘要"carry 关系在骨骼层驱动、我们够不到"需重新验证。
2. **旧 updateAnimationTransforms hook 挂在另一真实函数的指令中部**，会破坏该函数。已删除（见 §6）。
3. pickupObject/getPickedUp 的调用关系链已重建，真实地址已定位。

注：KenshiLib.dll 导出的是跳板桩（`jmp [rip+off]`，经 IAT 由 RE_Kenshi 加载时填充），头文件 RVA 注释与实际表可能不一致。因此新代码用 `GetModuleHandleA(NULL) + RVA` 直接求运行时地址（ASLR 不变），不依赖 GetRealAddress 表。

---

## 1. 函数调用图（已确认）

```
mount->pickupObject(rider)            [0x5CFF90] 载体侧：设载体 isCarryingSomething=1、清载体 _isBeingCarried、
    ├── call getPickedUp(rider)       [0x5CED90] 被扛侧：设骑手 _isBeingCarried=1、清 inSomething(0x2F8)、播 "carry me"
    └── call animation->setCarryMode  [thunk 0x43A54 → 0x51C8D0]

每帧（被扛者）：
carry 位置驱动函数（含 beingCarriedUpdate 调用）     [0x5CDA20..0x5CDEEB]
    ├── 计算 carry 位置（大量 xmm 数学/四元数）
    ├── call beingCarriedUpdate(anim,&result,&pos,&rot)  [call 0x5CDEC4 → thunk 0x12A3 → 0x5B5980]
    ├── 写 [char+0x48]（位置字段）+ 更新 movement
    └── 再次计算并写位置

beingCarriedUpdate                          [0x5B5980..0x5B5C1B]
    (this=rcx, result*=rdx, pos*=r8, rot*=r9)  返回 pos（必要时经 0x282AE 变换），并确保播 "carry me"
    ├── if !isActivated(body)(entity)(skeleton) → result=pos 直接返回
    ├── call [obj+0x2D8]->vfn0x248  （null → 播 "carry me"）
    └── call helper 0x282AE(this,pos,rot-derived)   [0x5B5BCA]
```

关键点：beingCarriedUpdate **本身不定位**，只是把调用者传入的 pos 返回（可能经 0x282AE 变换）。真实 carry 位置由 0x5CDA20 驱动函数计算后传入。因此：
- 若在 0x5B5980 拦截并把 result 置为我们的座椅位置 → 驱动函数会把骑手放到座椅而非摆动的 carry bone。

---

## 2. `_carryMode`（真实入口 0x5CE1E0，size 0x2EB）

```c
_carryMode(on, makeRagdoll, makeHull)   // 对 Character
{
    if (_isBeingCarried==0 && isCarryingSomething==0) return;   // 早退守卫 [0x5CE21B-0x5CE22B]
    if (全局抑制 [0x1421349CA]!=0) return;                        // [0x5CE231]
    _isBeingCarried = 0; isCarryingSomething = 0;                 // 主路径清两标志
    animation->setCarryMode(false,false,false);                   // [0x5CE25B → 0x51C8D0]
    animation->[0x1E8]=0xB; [0x1F0]=0; [0x1F8]=0; [0x1A8]=0; [0x240]=-1.0f;
    ... 解析 carryingObject(0x380) 找到对方角色 rdi ...
    if (!makeRagdoll && rdi) { ... 对 rdi 做若干操作 ... call 0x14003E536(rdi, on, true) }
}
```

结论：
- **单独 `_carryMode(true,false,false)` 对非 carry 角色是空操作**（早退守卫）。不能作为方案 Z 的"无碰撞"替代。
- 主路径是"解除 carry"（清标志 + animation->setCarryMode(false,...)）。
- `makeRagdoll=false` 时还会操纵对方角色（`0x14003E536(rdi, on, 1)`），语义待继续注释。

---

## 3. `getPickedUp`（真实入口 0x5CED90，size 0x38D）—— 被扛侧

```c
getPickedUp(byWhom)
{
    if (isCarryingSomething) return;                // [0x5CEDC3]
    if (_isBeingCarried==0) { ...partner 校验... }   // [0x5CEDD7]
    ...
    _isBeingCarried = 1;                            // [0x5CEEFD]
    inSomething(0x2F8) = 0;                         // [0x5CEF13]
    从 byWhom 拷贝状态到 [0x388..0x398];             // [0x5CEF27+]
    播 "carry me"                                   // 引用 0x1416F0B40
}
```

---

## 4. `pickupObject`（真实入口 0x5CFF90，size 0x118）—— 载体侧

```c
pickupObject(who)
{
    if (_isBeingCarried) return;
    if (isCarryingSomething) return;
    if (who->_isBeingCarried) return;
    if (who->inSomething != 2) ...                   // [0x5CFFC7]
    call 0x1400461b4(who, 0, 0);                     // [0x5CFFD8]
    _isBeingCarried = 0;                             // 载体
    isCarryingSomething = 1;                         // [0x5CFFEC] 载体标记"正在扛人"
    拷贝 who 的 [0x60..0x70] 到 this [0x388..0x398];
    inSomething(0x2F8)=0;
    animation->setCarryMode(on, side);               // [0x5D0042 → 0x43A54 → 0x51C8D0]
    写 animation [0x1E8..0x1F8]、[0x1A8]=0、[0x240]=-1;
    call getPickedUp(rider=who)                       // [0x5D0099 → thunk 0x309DB → 0x5CED90]
}
```

KenshiLib 标 0x5CF810 严重错误（0x5CF810 在另一函数[0x5CF78E..]（身体部位/伤害检查）中部）。插件当前 `mount->pickupObject(rider)` 经 KenshiLib.dll 跳板调用，游戏内可用（上马生效），说明实际跳板解析到了正确地址 0x5CFF90，头文件注释不可信。

---

## 5. 甩飞机理（MECH 诊断实锤，2026-08-13）

`Riding: MECH` 逐帧诊断（牛挂载）：
- `carried=1 carryM=1` **恒定** → carry 关系确实建立（pickupObject 生效），但 `beingCarriedUpdate`(0x5B5980) 从不触发 → 证实它是 /LTCG 内联死副本，carry 定位在调用方内联。
- `boneLocal=(−0.04,6.36,−0.69)` **完全恒定** → 骑手根骨本地偏移是常量。
- `offLocal ≈ boneLocal`、`resid=0.00` → **`rBip = node + nodeQ×boneLocal` 精确成立**。

**机制**：`boneLocal` 有 ~6.36 垂直偏移；`nodeQ`（节点朝向 = 座骨世界朝向，ApplyRiderOrientation 设定）在跑动/急转时剧烈旋转 → `rBip = node + nodeQ×boneLocal` 中偏移随朝向旋转 → 世界位置摆 ±6.5。DBG 里 `node≠tgt` 是读取时序（DBG 在 setPosition 之前读 node，读的是上一帧值），非 bug。

## 6. 本轮代码改动

**路径 A（已确认失败）**：beingCarriedUpdate hook 改到 0x5B5980 → 从未触发（死副本）。`updateAnimationTransforms` hook（错误地址）已删除。

**修复（SyncRiderNode 根骨锚定 + 首挂自动补偿）**：
- `mountAnchor` 映射：每个 mount 首个被补偿帧捕获 `anchor = rBip − node`（首挂自动补偿，保留现有调参基准）。
- `SyncRiderNode(rider, mount, rAnim, seatPos)`：`node->setPosition((seatPos + anchor) − nodeQ×boneLocal)`、`rootBonePosition = seatPos`。因 `resid=0` 且 `boneLocal` 恒定，这是**精确等式**：`rBip = seatPos + anchor`（恒定，朝向旋转不再导致摆动）。
- 应用点：`animUpdate_hook` 与 `mainLoop_hook` 1b 段，替换原 `setPosition(seatPos)`；`Dismount` 清除 `mountAnchor`。

## 7. 待办 / 开放问题

1. 修复实测：跑动/急转甩飞是否消失（预期 `rBip` 恒定于 `seatPos+anchor`）。
2. 若仍有残余摆动 → 渲染在更晚阶段用不同 nodeQ（slave 挂载覆盖朝向）→ 备选：非 rootAnchor 也移除 `slaveAttachToBoneMode`，或定位真实骨骼应用点固定根骨。
3. `rBipQ` 朝向随座骨旋转（急转时身体转向合理）；若仍有视觉翻转，考虑全 mount 启用 nodeQ nlerp 平滑。
4. 方案 Z（绕开 pickupObject）已不需要（carry 不参与甩飞）；若需"无碰撞"再做：`_carryMode(true,false,false)` 有早退守卫不可单独用，候选 `isPhysicalMode`(0x608)/`destroyPhysical`/伪造 carry 标志。
5. `updateAnimationTransforms` 真实地址仍未定位（若需在骨骼应用层拦截时再查）。

## 8. 甩飞动物清单（现状）

代码 rootAnchor 清单（RidingPlugin.cpp kFlingSkeletons，19 种）：
- 螃蟹组：螃蟹、巨型螃蟹、提多、巴纳布斯、螃蟹终结者、巨蟹先生
- 蜘蛛组：铁蜘蛛、安全蜘蛛
- 狗组：骨犬、埋骨地狼、定居者的小狗
- 喙嘴猩猩组：喙嘴猩猩、喙嘴猩猩之王、战斗喙嘴猩猩、黑色喙嘴猩猩、巨型白色喙嘴猩猩
- 卷缩者组：卷缩者、国王
- 喙嘴兽

注意：诊断显示"跑动甩飞"影响所有物种（连牛都甩），清单里的物种是"背骨锚点摆动"的加强症状。方案 A/Z 生效后需对 37 物种重测分类。

---

## 9. 名字标签高度问题 Review（2026-08-16）

**症状**：巨型坐骑（乌龟/利维坦）上骑手的名字/头顶标签显示在地面（~409 地形高度），骑手本身渲染在背上（headY≈477）。小动物正常（背≈地形，错位不可见）。

### 9.1 全部盲测结果（每轮重建部署，游戏内观察 + NAME 诊断）

| # | 修改 | mvY（诊断） | 名字观察 | 结论 |
|---|---|---|---|---|
| 1 | moveField.y += 999 | 1470（写入成功） | 不动 | ❌ 不读 moveField.y |
| 2 | moveField.x += 999 | — | 不动 | ❌ 不读 moveField.x |
| 3 | mount moveField.y = 500 | mount=500 生效 | 不动 | ❌ 不读 mount moveField |
| 4 | **node.y += 999** | — | **骑手起飞，名字没飞** | ❌ 不读渲染节点 node |
| 5 | rider +0x670.y = 600 | — | 不动 | ❌ 不读 rider audioData |
| 6 | mount +0x670.y = 600 | — | 不动 | ❌ 不读 mount audioData |
| 7 | moveField.x += 50 | rider.x=-53788（+50 生效）| 人物名字都不动 | ❌ **moveField 完全不影响渲染/名字** |

关键观察（用户）：
- 名字**一直存在**，选中只高亮。
- 镜头拉远名字"在坐骑上面"，拉近"固定那么高"→ **名字世界 Y 固定 ≈ 409（地形）**，透视造成远近观感差异。
- 不同身高的骑手名字高度相同；名字高度**与种族有关**（种族身高常量参与）。

### 9.2 已确立的机制

- `mvPre`（mainLoop_hook 开头，orig 前）= `mvY`（我们写后）= 471 → **moveField.y 写后保留到下一帧渲染，渲染阶段没有把它钉回地形**。名字读的不是 moveField。
- `rBip = node + nodeQ×boneLocal`，SyncRiderNode 把它钉到 seatPos+anchor ≈ 471（渲染正确）。
- 读断点（ba r4 moveField.y，14 次）命中全在 mainLoop 链，**0x145E50 从不读 moveField.y**。
- 0x145E50 prologue `mov rax,rsp` 承重 → 不可 hook。**ba e1 硬件执行断点 30s 未命中 → 0x145E50 根本不是名字投影函数**（旧 Plan Z 时代结论作废）。名字渲染走别的路径。
- NAME-SCAN（Character 0x000-0xB00 + CharMovement 0x000-0x500 + nameTag 对象 0x000-0x400 扫 y∈[400,425]）：唯一命中 `Character+0x670`（KenshiLib 标 AkSoundPosition audioData），值=mount 位置(410)，且覆写无效 → 非名字源。
- **结论**：名字的世界位置 = 某个被 carry 钉到地形(~409) + 种族身高的字段，与 moveField/node/mount 位置/audioData 全无关。**尚未定位该字段**。

### 9.2.5 决定性突破（2026-08-17）：名字 = 地形高度（实测）

`TERR` 诊断（直调 KenshiLib 导出桩 `UtilityT::getTerrainHeight(x,z)`，RVA 0x9B1F60 / 桩 0x1F9C0）：
- 骑乌龟：`terrainY=409.81`、骑手 mvPos.y=468.3、headY=474.9、**名字≈409**
- **地面名字 = `UtilityT::getTerrainHeight(rider.x, rider.z)` 现算值**。名字渲染对被扛/骑乘角色 fallback 到地形高度查询，不读角色实际渲染位置。
- 讽刺：`Character+0x698 terrainHeightPosition` 覆写无效（名字不读字段，现算不存储）；`Character+0x638 "nameTag"` 是头文件错误字段（游戏代码无 `+0x638` 引用，对象含 "personality" 字符串，实为无关对象）。
- **方案 A（结果劫持）hook 点已定位**：找到为名字渲染调用地形查询的代码路径，骑乘状态返回 headY。绝不全局改 `getTerrainHeight`（影响寻路/物理/粒子等所有读者）。
- 下一步：找名字渲染路径里对 `getTerrainHeight`（或等效内部地形函数）的调用点。

### 9.2.6 镜头聚焦点同样沉底（2026-08-17）——与名字同源

`CAM` 探针（直调 `ou->player->getCamera()` 导出桩，getCenter/getCenterNode/getCameraPos）：
- 骑乌龟：`center=(-53901,434.32,-248)`、`cnode`(中心节点本地 pos)、`camPos`、`rBip=(-53897,486,-231)`、`rMv=(-53897,486,-231)`（rMv 已与 rBip 一致，moveField 修复生效）
- **`center.y=434.32` vs `rBip.y=485.99` → 镜头中心比骑手渲染低 ~52 单位**（用户双击头像后镜头定位在坐垫下方，实锤）。
- `cnode` 系 Ogre SceneNode（`getCenterNode()`），其世界位置 = 相机聚焦点。
- **结论：镜头 center node = 名字标签 = 地形高度 fallback**（`center.y 434 ≈ 骑手处地形`）。游戏对 carried 角色把 UI 锚点（名字）+ 相机聚焦点都钉到地形高度，而非渲染位置。**同根因**。
- **修复思路（比名字简单）**：相机 center node 是 Ogre SceneNode，可编程。骑乘态每帧把它置到骑手渲染根骨位置（rBip）。不碰 game logic，无崩溃风险。
- 注意：cnode 是局部坐标（父节点含世界平移），需用父节点逆变换或直接验证 `center = cnode->世界位置`。待实现时确认 Ogre SceneNode 的父子关系。

### 9.2.7 FCS/Blender 内容层实验（2026-08-17）——"动物无 carry 骨"实锤 + Prop1 实验无效

**骨架分析（Blender 实视 + 正则交叉验证）**：
- 所有动物骨架 = 纯 Bip01 树（乌龟/牛/驮畜/犬全查过），**无任何 carry/挂接骨**。
- 人类骨架（`male_skeleton.skeleton`）额外骨仅 `Bip01 Jaw` + `Bip01 Prop1/Prop2`（挂 L/R Hand，手部道具挂接）。
- `carry bone` 是种族 GameData 字段，FCS 工具提示明说 "被角色抱起时用的骨（仅动物）" → **方向相反**（角色抱动物，非动物抱角色）。

**结论：Kenshi 动物"扛角色"引擎级不支持**（社区"动物不能拾取人"的真相）。插件用 pickupObject 强制动物扛人，carry 状态建立（上马/下马/坐姿正常），但载体侧无挂接骨 → 被携带物逻辑锚点（名字/镜头/选中）钉到默认位置（地形）。SyncRiderNode 只改渲染，改不了逻辑锚点。

**Prop1 骨实验（2026-08-17，已做，无效）**：
- Blender 给 `elephant_turtle.skeleton` 加 `Bip01 Prop1`（id=40，挂 Bip01 Spine2）+ 从 blend 重导 mesh。
- **游戏实测：名字/镜头无变化** → 游戏不按 "Prop1/Prop2" 骨名硬编码挂接被携带物。该路线封闭。
- **教训**：blend 重导会丢原版动画（blend 仅 7 动画，原版乌龟动画更多），且 mesh 重导丢材质绑定 → 乌龟透明。**已用 Steam 验证（AppID 233860）恢复原版 mesh/skeleton**。

**最终状态**：名字/镜头/选中锚点沉底 = 引擎级限制（与喙嘴猩猩翻转同类），**归入搁置**。任何运行时字段覆写 + FCS 骨/挂接配置都无法解决。唯一根治 = 社区路线（人类种族重塑 / FileRebinds 重绑 ragdoll 内容）。


### 9.3 明天方向

1. **找真正的名字渲染路径**：名字一直存在 → 每帧必有一次"世界坐标→屏幕坐标"投影。0x145E50 排除后，需在渲染阶段定位读名字位置字段的函数。候选：hook `_fireFrameEnded` 外层（kenshi+0x82AC70，prologue `push rbx; sub rsp,0x50` 安全已验证）在 orig 后 dump 所有角色 nameTag 对象/Character 候选字段。
2. **种族身高线索**：名字高度与种族相关 → 渲染层有 `basePos + raceHeight`。若能找到读 basePos 的函数，覆写该 basePos 字段 = 骑手头顶即可。
3. **nameTag 对象 0x638 深挖**：NTDUMP 显示 +0x20=0（可能是投影后才写）。考虑在渲染阶段（_updateAllRenderTargets 内）读 nameTag 对象看哪个字段被写入 409。
4. 备选：hook 0x145E50 的**调用者**（静态无 E8/指针/rip 引用 → 运行时间接调用，需动态定位）。

### 9.4 本轮代码残留

- `riderPreY` 诊断（mvPre）+ NAME/NTAG/MTP/SCAN/NTDUMP 诊断：保留在 `debugContinuous`（Numpad `.`）门控内，无 TMP 盲测代码。
- 已部署干净版（无任何 TMP 修改），构建 OK。

---

## 10. 名字标签存储 + 镜头验证 + 重定向（2026-08-18）

### 10.1 名字标签存储不在 ForgottenGUI::guiScreenLabels（5 轮排除）

`gui`（GUIVALID 确认）是真 ForgottenGUI（mbar/tip/dlg/scale/mgr 全为有效值），但 `gui+0x260/0x268` 列表（count=18）里的对象**不是 ScreenLabel**：
- LABELDEEP：`[0]` 对象 0x00-0x88 是 20 个间隔 0x80 的堆指针（非 ScreenLabel 布局；90/98 的 6.0 是碰巧数据）
- LBLVTABLE：18 个对象同 vtable `0x7FF6E83B2A20`（带指针数组的注册对象类）
- addScreenLabel(0x6E8FF0) 反汇编：操作 `gui+0x100` 而非 0x258 → 0x258 不是 ScreenLabel 列表
- **结论：名字标签存储不在 ForgottenGUI ScreenLabel 列表**（可能在渲染线程 KingOfRenderThread 或 Character 自身未知字段）。既有运行时手段不可行。**保守归档：非"已证伪"，而是"现有手段不可行"**。

### 10.2 镜头入口验证（未定论，入口对/布局错）

`getCamera` KenshiLib 桩 = 标准 IAT 跳板（`jmp [rip+X]`）→ 桩可靠。CAM 探针稳定运行不崩 + `getCenter()` 返回合理世界坐标 = `cam` 指针有效。CAMDUMP 显示对象"垃圾" = **字段偏移假设错**（对象非头文件 CameraClass 布局），非入口问题。**定论：入口正确、字段布局错，未定论，留待对 cam 指针做大范围盲扫（0x0-0x200 找含合理 world-Y 的 Vector3）复查。与名字标签分开标注，不归入"已耗尽"。**

### 10.3 骑手重定向（阶段 A，验证完成）

- **原生 carry 已完美处理骑手移动**：选骑手右键移动 → 坐骑正常驱动（无需 DLL 干预）。
- 我们原本的 `newPlayerTask_hook` move 重定向（把 selected 换 mount + halt 骑手）**冗余且可能 double 处理** → **已移除**（保留 BODYGUARD 上马 / PUT_DOWN 下马 / attack 重定向）。
- 验证无异常。**重定向 = 原生自带，归档为"无需插件干预"**。

### 10.4 归档原则（采纳社区建议）

区分两类条目：
- **已排除（强证据）**：名字标签 moveField/node/audioData/mount moveField/carry 动画层；guiScreenLabels 非标签存储
- **未定论/留档**：镜头（入口对/布局错）、physics 根缩放路径、名字投影函数真身、"读地形 vs 读载体根骨"区分
- 不再用"引擎级硬编码限制""三层实验全部无效"等绝对表述

---

## 11. 阶段 B 多物种回归（2026-08-18）

### 11.1 Phase 2i：moveField = 渲染根骨（修复点击命中）

- **问题**：喙嘴兽/部分物种上马后 moveField（点击碰撞体）比渲染根骨(rBip)低 ~6 单位 → 鼠标点不中骑手（渲染在背上但选不中）。
- **修复**：`mainLoop` 里 SetRiderMoveField 的目标从 `seatPos+mountAnchor` 改为**骑手 `Bip01` 渲染根骨世界位置**（rBip）。click hull 落在可见位置。
- DBG 实证：修复前 `rMove.y=432.71` vs `rBip.y=438.89`（差 6.18）。
- **注意**：rAnim 需在 SetRiderMoveField 前定义（移动了声明行）。

### 11.2 物种回归结果

| 物种 | 结果 |
|---|---|
| 野牛/螃蟹/卷缩者/乌龟 | ✅ 正常 |
| 喙嘴猩猩 | ✅ 已修复（翻转） |
| 铁蜘蛛/铁之蜘蛛 | ✅ 可上马/下马；坐姿需**现场调参标定**（追求实现，非代码 bug）→ 用户放弃 |
| 喙嘴兽 | ❌ 仍点不中（Phase 2i 后仍选不中）——**列为已知限制**，用户"不管了" |
| 网格/多物种总览 | 上马/下马/坐姿/移动均无崩溃 |

### 11.3 已知限制清单（发布文档用）

- 巨型坐骑（乌龟/利维坦）名字/镜头锚点沉底（引擎级，DLL 不可及）
- 喙嘴兽骑手点不中（选中失败，moveField 已改渲染位置仍无效）
- 铁蜘蛛等部分物种坐姿需现场调参标定
- 喙嘴猩猩翻转（已修复）/ 所有物种多物种共存待进一步验证



---

## 12. 关键 RVA / 偏移（反汇编验证；2026-08-27 从 `CLAUDE.md` 迁入）

CLAUDE.md 只留指针，需要 hook 新函数 / 直调新方法时查本节。

⚠️ **本表里的数字全部是 KenshiLib 头文件里的 RVA，不是安装的 exe 里的 RVA**（除了少数标注了「真实」的）。**直调**用不着它们（导出桩解析）；只有**离线看序言判能不能 hook**时才要换算成真实地址 —— 换算与验证走 **§18**（`tools\hook_probe.py 0x<头RVA>`）。

- carry：`pickupObject` 0x5CFF90、`getPickedUp` 0x5CED90、**`getDropped(bool ragdollHim, bool hull)` 0x5CC420（被携方放下 handler，下马核心）**、`dropCarriedObject` 0x5CDA60、`_carryMode` 0x5CE1E0、`setCarryMode` 0x51C8D0、`beingCarriedUpdate` 0x5B5980（⛔ 勿 hook；⚠️ 「死副本」这个说法已存疑，见 §18.5——它其实是 `0x5B5200+0x780` 的精确入口，但**禁令不变**）；CharMovement `destroy`/`restore` 0x65F6C0/0x661810（pickupObject 对被携方 destroy movement，getDropped 经 restore 复活——rev4/6 根因链）
- ragdoll：`Character::ragdollMode` 0x5CB5E0（**排队**+恢复回调，骑乘勿用）、`CharacterHuman::postRagdollCallback` 0x5CBAB0、`AnimationClass::ragdollModeUT` 0x5B9290（虚，vtable +0x50）/ **`AnimationClassHuman::_NV_ragdollModeUT` 0x5B98D0（人类 override，骑手用这个）**；`Character::isRagdoll` 0x7D01E0、`Character::isDown` 0x28D910（疑为「只出放倒」菜单的门）；`RagdollPart`(Enums.h:1001) `NONE/WHOLE/RIGHT_ARM/LEFT_ARM=4/HEAD=8/RIGHT_LEG=0x10/LEFT_LEG=0x20/CARRY_MODE=0x800/ARMS=6/LEGS=0x30/ALL=0xFFFF8000`
- **人类骑手要调 override 版、且走 `_NV_` 导出桩**：`AnimationClassHuman` 覆写了 `ragdollModeUT`（0x5B98D0 ≠ 基类 0x5B9290），虚分派语义正确但依赖 KenshiLib 头**复现 vtable 顺序**——而头里 RVA/offset 注释已知不可靠（人类头把该 override 的 vtable offset 写成 0x0）。故 `static_cast<AnimationClassHuman*>(pAnim)->_NV_ragdollModeUT(...)`：导出桩加载时解析真实地址（= `_NV_update` hook 同一机制），单继承 offset 0x0 指针无需调整。人类判定沿用插件既有的 `!isAnimal()`（`AnimationClass::isHuman()` 是纯虚、无 `_NV_` 桩，不可直调）。
- Character：`_isBeingCarried` 0x3D4、`isCarryingSomething` 0x348、`carryingObject` 0x380、`animation` 0x448、`ai` 0x650；`getAI` 0x268220、`setDestination(Vec3,bool)` 0x5C8E30、`getPosition`(virt) 0x5CDF00、`getRadius` 0x5C7C30（非虚，导出桩直调）
- AI：`isTargetInRangeForOrders(RootObject*)` 0x5997B0（⚠️ 语义=「可否对目标下指令」，对玩家恒 true，**非捡起距离，勿用于到达判定**）、`isTargetInRangeForOrders(Vec3)` 0x597010、`isWithinGivenRange(hand,Vec3,float)` 0x5A2900、`addGoal(TaskType,RootObjectBase*)` 0x5C88A0（备选：若 setDestination 驱动不动，改用它的目标机制）
- CharMovement：`pos` 0xC4、`destination` 0x0DC、`movementMode` 0x378；vtable 槽 `getPosition`=0x40、`isDestinationReached`=0x60、`halt`=0x98（AbstractMovementBase 段）
- ContextMenuGUI：`contextMenuTarget` 0xA8、`optionsList` 0xF8；`ContextMenu::orders`(lektor<int>) = 与 optionsList 子项平行的 order 数组
- 输入/设置窗口（未实现的自定义按键用，见 §14）：`InputHandler::addCommand` 0x363130、`InputHandler::loadConfig` 0x361F80、`controlEnabled` 偏移 0xD0、`DataPanelLine_KeyConfig` 0x3E7740、`OptionsWindow::getSingleton` 0x4067B0（`keysDatapanel` 0x100）、`resetAllKeys` 0x3E7030、`saveOptions` 0x3EC570、`DatapanelGUI::createLine` 0x6FC270（protected → 需 shim）
- **2026-09-01 收容进来的五个地址**（此前只写在 `TASK.md`／`HISTORY` 里、本节完全没有 ⇒ 一旦要改就没有真相源可改。都是**头 RVA**、都走导出桩直调，出货代码不依赖数字本身）：
  - `GameWorld::isPaused()` 0xDEDC0（非虚公有，全局 `ou`）—— **出货代码在用**：暂停帧提前 return、不推进捕获状态机（否则冻结的 mid-gait 基线会被烤进 cfg ＝ 暂停瞬移，全过程见 `HISTORY`「暂停」那条）。
  - `Character::getAppearance()` 0x645C0（`Character.h:584`，返回 `AppearanceBase*`，**非虚**）—— P4-3 用它做**指针相等**过滤，替掉了原来那条「裸走 `Character+0x448` → `+0xE8`」的两次解引用（`0x448` 只有 `sheatheWeapon` 体内一路佐证，没有第二路）⇒ 零布局依赖。
  - 「按层」动画权重那一组（P4-3 上半身门用；⚠️ **同名函数有两个，抓错就退回全局门**）：`AnimationClassBase::AnimationLayer::getTotalActionAnimationWeight()` **0x5B2430 ＝ 按层的那个**；`AnimationClassBase::getTotalActionAnimationWeight()` 0x5B2B50 ＝ 挂在外层类、是整个角色的和；`AnimationRequirement::isUpperBodyOnlyAction()` 0x51C2E0（`animationRequirements` 在 `AnimationClass` +0xF0，**已经在跑的通路**）。
  - `_setPositionAndTeleport` 0x65E1C0 —— ⛔ **走过的死路，别再用它治下马落地**：它的 `floor` 参数是**楼层索引，不是地面吸附**，而且传送的是**已被 destroy 的 movement 的逻辑位** ⇒ 只把渲染拖到体心高度（更扎进地形），物理没重建（实测记录见 `HISTORY`「修复尝试⑥ rev 5」）。

## 13. RE 工具经验（2026-08-27 从 `CLAUDE.md` 迁入）

- **inline hook 崩溃**：hook 引擎只搬 5 字节不校验指令边界。⚠️ **「安全目标＝纯 push/sub 序言」这个说法要按 §18.2 修正**：两个在产 hook 的 5 字节切点**都落在指令中间**，实战容忍；判序言别追求边界对齐，**拿在产 hook 比形状**（KenshiLib::AddHook 无 replay 跳板）。
- **KenshiLib 头文件 RVA 注释不可靠 —— 现在有了可执行的判定流程，见 §18**：`tools\hook_probe.py` 用 exe 自己的 `.pdata`（77108 条）回答「这个 RVA 到底是不是函数入口」并给出 `SizeOfProlog`；`header RVA + 0x780` 是**启发式**（7/14 命中，另一族要 +0x790，5 个头值落在别的函数里）。⚠️ **但 hook 装不装得上跟这些无关**——7 个 `AddHook` 全走 `GetRealAddress(&Symbol)`，源码里没有字面 RVA。
- **cdb**：`-y C:\symbols_local -s`；`q` 杀进程、`qd` 才安全分离；`~*e ba` 每线程硬件断点；`bp` 高频函数崩。
- **半套复刻状态下直调游戏 teardown = UB**；用 KenshiLib 直调完整原生函数才安全。
- 源文件是 **UTF-8**；MSVC v100 无 `/utf-8`，中文字面量能否匹配游戏运行时字符串需按具体 API 编码验证（菜单 caption 与物种名都用显式 `\xNN` UTF-8 转义字节最稳）。
- **KenshiLib 头文件 RVA 注释不可靠**（carry/骨骼系系统性错误，详见 §0）；但导出桩由 RE_Kenshi 加载时解析成真实地址——直调方法（pickupObject/dropCarriedObject/setCarryMode/ragdollModeUT/clearAllTasks）无需逆向 RVA。GetRealAddress 只用于 hook。
- **dumpbin 定位无 PDB 崩溃点的手法**：dump 里 RidingPlugin+0x133d3 → 找 DLL 内字节模式 `ff 90 98 00 00 00` → 文件偏移−0x400+0x1000=RVA 对上 → dumpbin /imports 按 IAT 槽位序数（(槽址−IAT基址)/8）对出导入名 → 反汇编上下文与源码逐行吻合锁定函数。
- **头冲突时的「最小 shim」技术（`AI/AI.h` vs `Character.h`）**：`Character.h` 把 `CharacterMessage` 定义成 `enum`，`AI/AI.h` 又前置声明成 `class` → C2011 硬冲突，两个头不可同时 include。只需 `AI` 的一个**非虚**方法时，别 include AI.h，改在 `Character.h` 已有的 `class AI;` 前置声明上补：`class AI { public: bool isTargetInRangeForOrders(RootObject* who); };`。调用 mangle 成 `AI::isTargetInRangeForOrders`，链接器从 KenshiLib.lib 导出桩解析（= 全插件直调方法同一机制，链接不报未解析符号即证明匹配）。非虚 + 不碰成员 → shim 空布局无所谓：`getAI()` 返回的就是规范 `AI*`，原样当 `this` 传，无 this 调整。**虚函数不能这么做**（要复现 vtable 布局）。⚠️ 该 shim 已从源码撤除（引入它的 `isTargetInRangeForOrders` 到达判定被证明是错信号），技术本身有效，留档备用；`DatapanelGUI::createLine` 是 protected，将来也要靠它直调。

## 14. 自定义按键进原版设置菜单 —— **已实现（2026-08-28 游戏内实测通过）**

落地实现与硬约束见 `CLAUDE.md`「当前实现 → 自定义按键」与「关键机制」；本节只留 RE 侧的结论、以及**被推翻的草案判断**（免得下次照着错结论再走一遍）。

**⚠️ 草案里最关键的一句是错的：「RE_Kenshi 不注入原版设置窗口，没有现成先例可抄」。** 先例就在注入本插件的加载器里，而且做的正是同一件事（给控制页加一行可重绑定的 "Toggle Free Camera mode"）：`D:\KenshiModDev\RE_Kenshi\MiscHooks.cpp:270-286`（hook `DatapanelGUI::addCustomLine`，看到 `command == "editor_toggle"` 就在其后 `addCustomLine(new DataPanelLine_KeyConfig(...))`）+ `:313-314`（hook `InputHandler::loadConfig`，在 orig 之前 `addCommand`），派发在 `dllmain.cpp:236-283`（自己包 `OIS::KeyListener`，从 `key->map[arg.key]->name` 读命令名）。**据此整个方案简化掉了草案的大半**：不需要 shim、不需要碰 `OptionsWindow`/`getSingleton`/`keysDatapanel`/`createLine`、不需要单独的只读诊断 DLL。§335 那五六项「待采集」里只有 ⑤⑥ 还有意义。

实测结论（按草案的编号回答）：
- ③ `addCommand` **单独不会**让行出现在控制页——那只是把命令加进名字表。菜单行要**另外**用 `DatapanelGUI::addCustomLine` 追加一个 `DataPanelLine_KeyConfig`。原版改键流程/`默认` 按钮/`controls.cfg` 读写对我们的命令全部照常生效。
- ④ 不需要轮询 `OptionsWindow::created`/`getSingleton`：hook `addCustomLine`，`thisptr` 就是控制页的 keysDatapanel，行随原版每次 `create()` 重建被重新追加。
- ⑤ `controlEnabled`（0xD0）**确实在 UI 吃键盘时落 false**，`HotkeyPass` 开头查它即修掉「打字触发热键」。
- ⑥ **Masks 是精确匹配的，但机制和草案设想的不一样**：`InputHandler::map` 的键是**复合整数 `keycode | mask`**（原版自己就把 `editor_toggle` 存成 Shift+F12），所以裸 `NUM*` 与 `Ctrl+NUM*` 占不同槽位、共存无冲突 → v1.2.1 的整套键位（含 4 个 Ctrl 变体）原样搬成默认值。⚠️ **修饰键要 OR 进 `addCommand` 的「键」参数，传 `masks` 参数是错的**（会注册到裸键上、跟裸键那条抢槽位、输的那条整行空白）。
- **启动时序**（草案 §334 标为「唯一持久化风险」）：`InputHandler::loadConfig` 的 hook 对**我们**永不触发——RidingPlugin 是 post-load 插件，装 hook 时开机那次 `loadConfig` 早跑完了（freecam 能用这个 hook 是因为 RE_Kenshi 是 preload）。解法＝在 `startPlugin()` 里直接注册完**自己补调一次 `key->loadConfig()`**（草案猜的候选解法是对的），且**必须在插件加载时就注册**，不能等到控制页构建时——那一次构建的行会画成空白。
- **派发不能用 `isKeyState(name)`**：对插件加的命令恒 false（详见 CLAUDE.md 关键机制）。我们不像先例那样再包一层 `OIS::KeyListener`（RE_Kenshi 已经包了一层），改成每 15 帧扫 `key->map` 反查每条命令**当前**的复合键码，再 OIS 轮询物理键 + 校验修饰位。

仍未做：caption 只有英文（原版 `gettext` 不含我们的串，这也是先例那行 freecam 不翻译的原因），多语言＝自带 9 语言表（同 `kMountLabels[]`）为增强项。

## 15. 人形动画表枚举（TASK.md P1，2026-08-29 游戏内实测一次通过）

**目的**：`AnimsListsManager::getSingleton()->getCharacterList()->allAnims` ＝ 引擎真正认的全部人形动画记录。姿势重做与骑乘战斗都要先知道「表里有什么」。诊断代码在 `RidingPlugin.cpp`（`DumpHumanAnimTable*` / `LogAnimRow` / `AnimRowSnap`，`Mount()` 末尾一次性触发，`gAnimTableDumped` 预算 1／每次 DLL 加载）。

**取表的路径与实锤**
- 走 `AnimationClass::getAnimationDatasList()`（RVA 0x6DFF0，＝这个角色自己解析用的那张表）而不是直接用单例，再与 `getCharacterList()` **对照打指针**：实测 `own=charList=00000001741ABAA0 same=1` ⇒ **人类骑手确实解析人形表**，两条路等价。
- `AnimsListsManager::getSingleton()` 0xB70A0、`getCharacterList()` 0x8AF50、`getAnimalList(GameData*,GameData*)` 0x5C5D60。全部有 KenshiLib 导出桩，直调即可。
- 嵌套 `AnimList` 布局（`AnimationClass.h:265-305`）：`movementAnimsBase` 0x0 / `movementAnimsUpper` 0x18 / `idleAnims` 0x30 / `strafeAnims` 0x48 / `attacks` 0x60（`lektor<AnimationData*>`）、`actionAnims` 0x78 与 **`allAnims` 0xB8**（boost `unordered_map<string, AnimationData*>`）、`turningAnim` 0xF8 / `stumbleAnims` 0x100 / `weatherOverlayAnims` 0x140，`sizeof=384`。
- **迭代引擎 boost 容器的两道保险都值得留着抄**：①把 map 类型**完整拼出来**成一个 typedef（`EngineAnimMap`，含 `Ogre::STLAllocator`）＝ 让「我们的 boost 1.60 与游戏同构」这个假设变成**编译期**检查；②运行时自检 `offsetof(actionAnims)==0x78 && offsetof(allAnims)==0xB8 && sizeof==384`，不符就拒绝迭代。实测打出 `sizeof=384 off(action)=120 off(all)=184` ＝ 完全吻合 ⇒ **boost 布局同构这件事现在是实测结论，不再是赌**。另配 4000 条上限（防坏桶链死循环）、每条 `AnimationData` 过 POD SEH 快照、整个 dump 外层再套 SEH 壳。

**⚠️ `AnimationClass::getAnimationData(name)` 查不到会往 `allAnims` 里插一条 NULL——它会改表**
- 实锤：counts 行（probe 之前）报 `all=119`，走表时数出 **122** 条，多出来的 3 条 key 正是那 3 个 probe 报 `absent` 的名字（`jog lower-7` / `jog upper-6` / `sitting_new`），`ptr=0000000000000000 UNREADABLE`。119+3=122，逐条对得上。我们自己的 `allAnims.find()` 不会插，**插的是 `getAnimationData()`**（典型 `operator[]` 语义）。
- 另有 2 条 NULL key **不是我们造的**（`jog lower-6` / `jog upper-5` ＝ 本会话的 clip 名，没被 probe 过）⇒ **引擎自己也在拿 clip 名调 `getAnimationData()`**，同样在往表里下毒。
- **两条硬约束**：①**任何名字在传给 `getAnimationData()` 之前先 `find()` 验存在**——否则一个拼错/mod 才有的名字就永久往引擎表里留一条 NULL，而别的引擎代码遍历 `allAnims` 时会踩空指针（我们每帧 `getAnimationData(poseAnim)` 目前安全只因为两个姿势名都真实存在）；②**读 counts 要注意先后**：`size()` 是 probe 之前的数，走表是之后的数，差值＝probe 制造的 NULL 数。

**记录名 vs clip 名（钉法的地基）**
- map 的**键 ＝ 记录名（`dataName`）**，`animName` 才是 clip 名，两者常常不同：`jog lower` → clip `jog lower-6`、`jog upper` → clip `jog upper-5`、`stealth idle` → `crouch idle`、`eat` → `cannibaleating`、`knockout` → `stealthKO`、`crouch walk lower`/`crouch walk upper` → `crouch walk`/`crouch walk-10`。
- **clip 名的数字后缀不稳定**：同一条记录在 P0 那趟是 `jog lower-7`/`jog upper-6`，这趟是 `-6`/`-5` ⇒ 后缀是加载期分配的，**绝对不许拿 clip 名当键**。`runAnimation`/`getAnimationData`/我们每帧的钉法全部走记录名；只有 POSEDUMP 打的是 clip 名（它读的是 `SingleAnimation`），两边名字不一致是**正常**的。

**表的形状（原版人形，未装动画 mod 的那份；117 条可读 + 5 条 NULL）**
- 按层：**UPPER 79 / OVERLAY 21 / LOWER 17**。按 category：`NORMAL` 101 / `RANGED` 6 / `SWIM` 5 / `GROUND` 4 / `CARRIED` 1 —— **`ANIM_ATTACKS` 与 `ANIM_COMBAT` 各 0 条**，`ANIM_IMPRISONED`/`ANIM_SLEEPING` 也是 0。
- **`attacks` lektor 是空的（`attacks=0`）**，`idle=23 / moveBase=17 / moveUpper=20 / strafe=7 / action=29`。⇒ 人类的挥砍**不在人形表里**（候选：per-combat-technique 数据、或 `getAnimalList()` 那条路），P4-3 的原料得另找。
- 表里唯一像近战的是 6 条 `blow` 家族（`back blow high/light/low`、`mid blow`/`mid blow drop`/`mid blow light`），全部 `UPPER` + `whole,action,norm,reloc,restrict,Rarm,Larm`、spd `-999/0/999` ＝ 击倒类重击，不是普通武器攻击。
- `weaponTypeFlags` 是能用的位掩码：`guard 1h`=0x04、`guard4 main`=0x03、`guard5`=0x12、`guard6`=0x08、`guard polearm`=0x100，大多数通用行是 0x13F。

## 16. 手控腿骨 + blend mask（TASK.md P2-1b-1，2026-08-29 游戏内实测一次通过）

**目的**：姿势 clip 压全身（UPPER + `wholeBodyAllLayer`）而 LOWER 层一条坐姿都没有 ⇒ 想做跨骑只能把腿**整体从动画系统里拿走**。一次上马分三段验两个杠杆。诊断代码在 `RidingPlugin.cpp`（`LegProbe*` / `DumpRiderSkeleton`，`HaltAndForceSitPass` 内、`debugContinuous` 门控、每段 900 帧、段 3 自复原、`Dismount()` 另带一份复原）。

**杠杆 A（放一条 LOWER 静态 clip 当基底）＝ 死**
- `crawl idle down` 确实存在（`allAnims.find()` 命中，`list=...E76E6830 off=184` 自检通过，`crawl idle up` 没用上）。
- 每帧 `runAnimation(base,1,1)` 连跑 900 帧：`play=1 dw=1.000 layer=0`、`t01` **在推进**（0.01→0.51→0.33→0.62…），而 **`w` 恒 `0.000`、`ms` 0.000/-1.000** ⇒ 请求被接受、时间在跑，**实际权重永远起不来**，对骨架零贡献。
- ⚠️ **两条判读更正**：①P1 记的「spd 0/0/0、play=0.00 时间不推进」是**表里的静态字段**，活体上时间是推进的；②原来担心的「`cat=GROUND` 被角色状态门掉」不成立——那会让请求根本进不去，实测是**进去了但权重被压在 0**。头号嫌疑＝被钉住的 `kRidePose` 带 `wholeBodyAllLayer`（跨层压制）；`cat=GROUND` 也压权重这条无法从本趟数据里排除，但**两者指向同一个结论**：只要那条 `whole` 姿势还钉着，别的层的 clip 就上不了台面。⇒ 这一条同时打到 P2-1c（自制 LOWER clip 若不先解决 `whole`，一样是 `w=0.000`）与 P4-3（攻击 clip 同理）。

**杠杆 B（`setManuallyControlled` + 每帧写朝向）＝ 成立，但 blend mask 是必需件不是可选件**
- **段 1（只写朝向）**：`kept` 稳定在 **0.7638 / 0.7656**（`kept = |dot(读回, 我们写的)|`，⇒ 2·acos ≈ **80°** 的污染），`had ≠ want`，小腿留在姿势的位置 ⇒ **姿势轨道每帧累加到手控骨上**（`Skeleton::reset(false)` 不碰手控骨，`NodeAnimationTrack::applyToNode` 随后 rotate 上去）。数值稳定不漂是因为**我们每帧覆写**，不是因为污染不存在。
- **段 2（同样的写入 ＋ `mainState->setBlendMaskEntry(handle, 0.0f)`）**：`kept = 1.0000`、`had` 与 `want` **逐位相同**，f=1860…2640 共 14 个采样点无一例外（f=1800 仍读 0.7636 是因为 mask 在那一帧的写入**之后**才建）。`ms=1.000` 全程 ⇒ 屏蔽两根骨**没有**动摇姿势自己的总权重。⚠️ mask 建在**那条姿势自己的 `Ogre::AnimationState`** 上（`createBlendMask(numBones, 1.0f)` 后逐骨置 0），不是全局开关。
- **父骨写入沿链传播**：只手控大腿，小腿 derived 位置从坐姿的 `(-0.47,5.51,3.10)` / `(-2.39,6.44,3.21)` 跳到 `(0.95,2.16,-0.73)` / `(-3.72,3.13,-0.74)`，而小腿**没有**被 mask ⇒ 它的**局部**膝弯仍由姿势轨道提供。**「大腿归我们、膝弯借 clip」是可行的分工。**
- **清理路径实测**：段 3 `setManuallyControlled(false)` + `reset()` + `needUpdate()` + `destroyBlendMask()` 全过。不带 `Dismount()` 那份复原＝骑手带着手控大腿走掉、重载才恢复。
- **副作用零**：同一趟 7868 个 DBG 帧姿势权重 **7857 帧恒 1.00**（11 个低值全是那一次上马的淡入斜坡、`[0.50,0.995)` 零帧）、`wn=(0,0,0)` 全帧、`aRag=0` 全程 ⇒ 请求那条基底 clip 与 mask 两根骨**都没有**扰动 v1.6 的钉权重与逐帧杀 ragdoll。

**骨骼清单与轴向约定（实测，不用再猜）**
- 骑手骨架 **30 根**：`Bip01 L Thigh`=**2** / `L Calf`=**3** / `L Foot`=4，`R Thigh`=**7** / `R Calf`=**8** / `R Foot`=9。
- **骨局部 +X ＝ 沿骨指向子骨**：`L Calf` 在大腿局部 `(4.39,0,0)`、`L Foot` 在小腿局部 `(4.61,0,0)`；`L Thigh` 在骨盆局部 `(0,+0.99,0)`、`R Thigh` `(0,−0.99,0)`（⇒ 骨盆局部 ±Y 是左右）。
- ⚠️ **两条大腿的绑定朝向几乎相同、不是镜像**：`(0.018,0.015,-0.001,1.000)` 对 `(-0.018,0.015,0.001,1.000)`（w,x,y,z，≈ 绕 Z 转 180°）⇒ **同一个 delta 会把两条腿甩向同一边，对称姿势必须自己镜像 delta。**
- **绕大腿局部各轴的语义**（骨架空间实测 **+X=左 / +Y=上 / +Z=前**；髋 L `(0.95,6.33,−0.74)` / R `(−1.03,6.33,−0.74)`；绑定姿势髋→膝 ＝ 笔直向下、长 ~4.17）：
  - **X ＝ 沿股骨扭转**：左腿 +40°X → 髋→膝 `(0.00,−4.17,+0.01)` ＝ **仍然笔直向下、膝一点没挪**（观感上的「小腿外八」来自扭转带着姿势提供的膝弯一起转）。
  - **Z ＝ 外展/内收（额面摆动）**：右腿 +40°Z → 髋→膝 `(−2.69,−3.20,0.00)`，长 4.18、离竖直 **40.06°**、**前后分量 0** ＝ 纯外展（对右腿 +Z 是向外）。
  - **Y ＝ 屈伸（前后摆动）**：唯一剩下的垂直轴（两个测过的轴都给出 0 前后分量）。**跨骑最需要的就是它，本趟没测。**
- **用户观感与三段的对应**（决定性的另一半）：段 1「腿先向右偏」＝ 被污染的合成；段 2「大腿八字呈八字跪姿，小腿向后外八分开」＝ **绑定姿势（直腿向下）＋ 我们的 40°**，「小腿向后」＝ 姿势轨道留下的局部膝弯。三条数据流（`kept`／derived 位置／肉眼）互相独立且完全吻合。

### 16.1 定型手法（P2-1b-2 符号定案 + P2-1b-3 出货，2026-08-29 两趟实测）
**这一节是 §16 的收尾：把「能写骨」变成「写出一个能出货的跨骑姿势」还缺的四件手法。**

- **屈伸轴与符号已实测，不再是推论**：大腿局部 **Y ＝ 纯屈伸，`+Y` 把膝盖甩向前**。证据是等幅翻转——同一幅度 ±45°Y 给出 `fore=+2.96` 对 `−2.95`，股骨长两边都 4.18（若 Y 混着别的分量，翻转不会等幅）。**外展绕局部 Z 完全不动前后分量**：4 个外展档（27.3° / 33.9° / 35.1° / 45.0°）下 `fore` 恒 2.96，`out` 单调 1.35→2.09、`down` 2.62→2.08、√(out²+fore²+down²) 恒 4.18 ⇒ **两个轴在绑定帧里正交**，可以各自独立调幅度。
- **合成顺序必须让两个 delta 都活在绑定帧里**：`want = b->getInitialOrientation() * (qAbd * qFlx)`。这样 §16 里单轴量出来的语义才**直接适用**；反过来写（delta 在父帧、或先乘 initial 再叠第二个 delta）会让第二个轴的意义随第一个轴的幅度漂移。
- ⚠️ **写入是从绑定姿势开始的，不是从当前姿势开始的** ⇒ 接管一条大腿就等于**丢掉坐姿 clip 那 ~90° 屈髋**，屈髋必须自己补回来（只给外展＝叉腿跪姿）。小腿只读不写，**局部**膝弯照旧由姿势轨道提供、跟着大腿一起搬。
- **mask 每帧无条件重上**（`hasBlendMask()` 假时才 `createBlendMask(numBones, 1.0f)`，然后逐骨 `setBlendMaskEntry(handle, 0.0f)`）：`mainState` 可能被引擎换掉，而重建只在真丢了 mask 时发生 ⇒ 便宜且自愈。**handle 是索引不是名字**，写错一个就静默屏蔽了别人的骨头——所以每次接管打一行 `h=(2,7) bones=30` 存证（8 次上马八次全同）。
- ⚠️ **mask 盖不住「正在淡出的另一条 clip」** —— 它挂在坐姿自己的 `Ogre::AnimationState` 上。上马后 ~14 帧站立待机还在承重，它的大腿轨道照旧往手控骨上 rotate ⇒ 那几帧写骨就是 §16 段 1 的 `kept=0.764` 污染重演。**手法：以姿势自己的 `SingleAnimation::weight ≥ 0.5` 当接管闸门**（没到就跳过、已武装就当场复原），与 `PoseLayerPin` 同一道门同一个数。代价是淡出末尾一次瞬切，藏在既有的交叉淡出里看不见。
- **复原的三个触发点**（缺一个就有腿被落下）：闸门掉下去、`Dismount()`、SEH 捕到 AV。实测 8 次上马 / 8 次 `restored on dismount` / 0 次中途 released / 0 次 AV。
- **副作用仍然为零**（第三趟确认）：`wn` 4128 帧全 `(0,0,0)`、姿势权重 4074/4128 恒 1.00、`rel.y` 恒 6.35/6.36 ⇒ 屏蔽两根骨没有动摇 v1.6 的钉权重、逐帧杀 ragdoll，也没有动 `boneLocal`（＝座位表不用搬）。
- ⚠️ **`Character::getRadius()` 不是体宽**：狗族 9.8/9.9（`torsoLen` 8.4/8.5）＞ 野牛 6.7（`torsoLen` 10.3）。当「越大越张腿」的单调代理够用，**当体型闸门会判反**（见 TASK.md P4-0）。

## 17. 骑乘战斗：够到战斗系统的手法与地址（TASK.md P4，2026-08-29 → 08-30，五趟游戏内实测）

**这一节只记「怎么从插件里够到战斗系统」**——手法、地址、偏移、判读陷阱。设计决策与出货形态在 `CLAUDE.md`「骑乘战斗」，被否掉的方案与逐趟原始数字在 `HISTORY.md`。shim 与裸偏移辅助函数全在 `RidingPlugin.cpp:61-195`（那段注释是本节的一等来源，改代码前先读）。

### 17.1 手法：给「进不了编译树」的类写空壳 shim
`kenshi/combat/CombatClass.h` **不能 `#include`**：它把邻居写成 `"Enums.h"` / `"util/hand.h"` / `"util/lektor.h"` / `"util/OgreUnordered.h"`，而它们实际在 `kenshi/` 与 `kenshi/util/` 下 ⇒ 只有往构建脚本再加一条 `/I…\Include\kenshi` 才解析得开（**没加，走 shim**）。`CharStats.h` / `Gear.h` 同样没进树。手法 ＝ 自己声明一个**同名的空 class**（只有非虚成员函数声明，**零数据成员、零虚函数**），调用 mangle 成 KenshiLib.lib 导出的同一个符号、由 RE_Kenshi 在加载时补成真实地址 —— **「链接通过」本身就是签名匹配的证明**，与 §13 的 `AI` 最小 shim 同一招，也同样**不需要逆向 RVA**（RVA 只用于 hook）。四条前提逐条验过：
- **表里每个成员都是非虚的**——对着 lib 自己的 mangled name 核过：**`QEAA`/`QEBA` ＝ public 非虚，虚的会出 `UEAA`**。⇒ 没有 vtable 布局要复现、没有 this 调整。
- **`this` 不需要调整**：`CombatClass` 的唯一基类 `Ogre::GeneralAllocatedObject` 是个空分配器策略（无数据成员、无自己的 vtable）⇒ 类自己的 vptr 落在偏移 0；`CharStats` 同理。`CombatClassAI` 单继承自 `CombatClass`（`CombatClass.h:273`）且 `CombatClass` 是**唯一且第一个**基类 ⇒ AI 子对象从偏移 0 开始，**`CombatClass*` 可以横向 cast 成 `CombatClassAI*`，不加减任何偏移**。对象永远是 `Character::getCombatClass()` 返回的指针**原样当 `this` 传**，我们从不构造/复制/取 sizeof。
- **撞不了名**：`class CombatClass;` 全树只被前向声明（`kenshi/Character.h:33`、`kenshi/CharBody.h:13`），`CombatClass.h` **零处被 include**；`class CombatClassAI` 在整个树里一次都没出现；`CharStats` 只被前向声明（`AI/AI.h:46`、`Character.h:34`、`CharBody.h:14`、`CharMovement.h:221`、`combat/CombatClass.h:19`、`Dialogue.h:273`、`MedicalSystem.h:90`），`Weapon` 同理（`Character.h:39`、`CharStats.h:18`、`Inventory.h:115`、`Item.h:105`、`Animation/AnimationClassHuman.h:5`）⇒ 不会 C2011。
- ⚠️ **头文件标着 `protected` / `private` 的方法，shim 里必须声明成 `public`**：那些注释描述的是**原始游戏源码**，KenshiLib 把它们**全部按 public 修饰**导出。写成 protected 会 mangle 成 `IEAA…` 直接链不上。⇒ `weaponReach` / `isInAttackZone` / `getNearestEnemyInAttackZone` / `setAttackTarget` / `changeState` 都是 public 声明，**别为了跟头文件一致去「修正」它**。
- **`Weapon` 的上行转换要用 `reinterpret_cast`**：`Weapon : Gear`（`Gear.h:41`）、`Gear : Item`（`Gear.h:5`）两级都标注 `offset = 0x0`，但两个类型在本插件里都是**不完整类型**，语言级 upcast 编不过。

### 17.2 RVA 表（`GetRealAddress` 不需要——这些全是直调）
| 符号 | RVA | 备注 |
|---|---|---|
| `CombatClass::_isInCombatMode` | `0x43FCD0` | 与 `combatModeActive`(0x130) 互为对照 |
| `CombatClass::getNumOpponents` / `getNumWaitingAttackers` | `0x2B2B90` / `0x2B2670` | |
| `CombatClass::isInAttackerListH` / `addAttackerH` | `0x664FD0` / `0x6666A0` | |
| `CombatClass::_getAttackTarget` | `0x339E30` | 返回 `hand` |
| `CombatClass::isAttacking` | `0x664CA0` | |
| `CombatClass::weaponReach` | `0x607BA0` | 头里写 protected |
| `CombatClass::isInAttackZone` / `getNearestEnemyInAttackZone` | `0x607CE0` / `0x6090B0` | 头里写 protected |
| `CombatClass::setAttackTarget` / `setAttackTargetHandle` | `0x664E00` / `0x664ED0` | 头里写 protected |
| `CombatClass::getCombatState` / `getBlockStateEnum` | `0x333D30` / `0x664BD0` | |
| `CombatClass::changeState(state, minTime)` | `0x2B25F0` | 头里写 protected |
| **`CombatClassAI::_NV_initCombatMode`** | **`0x667A60`** | `CombatClass.h:286-287`；**真正的派发目标** |
| `CombatClass::initCombatMode`（基类体） | `0x665230` | ⚠️ **不是**派发目标，见 17.4 |
| `CharStats::chooseAttack(range, reach, last, stationary)` | `0x886880` | **reach 是入参**，见 17.4 |
| `CharStats::getBashAnimation(range)` | `0x885C70` | |
| `Weapon::getCategory` | `0x5C71D0` | `Gear.h:49` |
| `AnimationClass::runCombatAnimation(tech, w, "")` | `0x5B6E80` | public，**不用 shim** |
| `AnimationClass::endCombatAnimation` | `0x5B34E0` | public；`Dismount()` 里无条件调 |
| `CombatClass::go(float)` | `0x60C4D0` | **不需要**，见 17.4 |
| `CharacterHuman::sheatheWeapon`（虚 override）/ `_NV_sheatheWeapon` | 头 `0x5CC0A0` → **真实 `0x5CC820`** | `CharacterHuman.h:22-23`；真实入口经 RTTI 虚表槽 `0x2D0` ＋ `+0x780` ＋ 语义三重确认，`.pdata` 精确入口 `prolog=30`，**序言那道门已过（§18.2）**；**派发是纯虚调 ⇒ 骑手必落这个地址、没有基类旁路（§18.6 实锤）**；基类 `Character` 的槽 `0x2D0` 指向 `0x641500`＝`ret 0` **空实现**（旧记的「真实 `0x641510`」是错的，那是隔壁一个无关函数，§18.6） |
| `CharacterHuman::dropWeaponInHands` / `dropWeaponInHandsFake` | `0x5CBFE0` / `0x5C8EE0` | `CharacterHuman.h:49-50`，头里写 protected（shim 要声明成 public，见 17.1 最后一条） |
| `CharacterHuman::leaveSheathEquipped(section, ypos)` | `0x5D1D30` | `CharacterHuman.h:55`，头里写 protected；「把武器留在鞘里」那一路 |
| ⛔ `CombatClass::calculateTargetsInAttackZone` | `0x608020` | **会 AV，永远不要调**，见 17.5 |
- **两个虚表槽**（`Character` 自己的 vtable，按槽位偏移手取函数指针、不进 shim）：`drawWeapon(Item*, const std::string&)` = **`0x3D8`**（人类实现体 `CharacterHuman` @`0x5DB800`），`getThePreferredWeapon()` = **`0x3C8`**（⚠️ 被骑状态下读 NULL，见 17.4）。
- **`Weapon*` 传给 `drawWeapon` 前要 `reinterpret_cast` 成 `Item*`**（理由见 17.1 最后一条）。
### 17.3 裸偏移表（`CombatClass.h` 的数据成员进不了编译树，只能按 `this` 加偏移读）
辅助函数 `CcBool` / `CcInt` / `CcFloat` / `CcPtrSet` / `CcVptrLo` / `CcSetPtr(cc, off, v)`（源码 `:170-195`）。⚠️ **每个裸读都配一个必须与它一致的 API 调用**，偏移一漂就表现为「两者不一致」而不是静默给假数据 —— 这是这张表唯一的自检手段，**加新偏移时照做**。
| 结构 | 偏移 | 字段 | 实测判读 |
|---|---|---|---|
| `CombatClass` | `0x130` | `combatModeActive` | 与 `_isInCombatMode()` **4/4 一致**，可信 |
| `CombatClass` | `0x150` | `currentTechnique` | 战斗前 4/4 读 NULL；**唯一一处 WRITE**（`CcSetPtr`，P4-1d 第 2 级） |
| `CombatClass` | `0x1F0` | `combatState` | 与 `getCombatState()` **4/4 一致**（「不一致」计数 0/4），可信 |
| `CombatClass` | `0x1F4` | `nextMove` | ⛔ **不可用**：战斗模式前读 `1818135763`（＝ASCII 垃圾/未初始化），之后读 8。已从探针里删掉，**别再加回去** |
| `AnimationRequirement` | `0x118` | `_currentCombatTechnique` | 基址是 `rAnim->animationRequirements`，**不是** `AnimationClass*`；与 `CombatClass::currentTechnique(0x150)` 互为对照 |
| `CharacterHuman` | `0x6D8` / `0x6E0` | `weaponInHands` / `weaponInHandsSheathLocation` | 「逻辑上在手里」的槽 ＋ 它离开的那个鞘；P4-3 收鞘写手的下一级把手（`ATTACH_WEAPON`=0） |
| `AnimationClass` slave 子块 | `0xB8` / `0xC0` / `0x110` | `isActionSlave` / `attachRootToMastersBone` / `forcedSlaveLoop` | `forcedSlaveLoop` 是我们自己每帧写的；⚠️ `isActionSlave` **由引擎维护**，见 17.4 |
| `AnimsList`（`getCharacterList()`） | `0xB8` / `0x78` | `allAnims` / `actionAnims` | **boost 布局自检**：不等于这两个值就**拒绝读**（源码 4 处：`:3787` `:3808` `:3841` `:7034`） |
| `CombatTechniqueData` | `0x0` / `0x38` / `0x3C` | `animation` / `initialDistance` / `minDistanceVsStatic` | `animation` 是 `std::string`，与 `AnimationData::dataName` 同一个 ABI 赌注；它自己的头会拖进 `MedicalSystem.h`，所以三个字段全走裸读 |
| `CharMovement` | `0x3B0` / `0x330` | `clickHull` / `dontEverRecreateMe` | P3；`nrc` 全程 0 ⇒ `restore()` 不需要 |
| `AbstractMovementBase` | `+0xC4` | `pos` | public，是 `getPosition()` 背后的字段；P3 剩下那一半的候选写点 |

### 17.4 实测语义：谁是把手、谁是从动
- **`vpR = vpM = vpE = 41DB5688`（4/4，`CcVptrLo`）** ⇒ **玩家骑手不是 `CombatClassPlayer`**，它与坐骑、与敌人共用同一个 `CombatClass` 派生类型。⇒ 对 `initCombatMode` 做虚派发落在 **`CombatClassAI::initCombatMode`@0x667A60**，不是基类体 `0x665230`。⚠️ **P4-1c 非虚地调基类体也是「有效的」**（`cma` 0→1、`rTgt` 0→1、`cst` 3→4 一直保持到日志末尾），换成 AI 那个是**按语义取正确**，不是修 bug —— 别把它当成「原来那样是错的」。
- **`initCombatMode` 就是那个把手**（P4-1c 273 行 P3CMB：`cm`/`cmM`/`rTgt` 三个字段都是 `{0:62, 1:211}`，切换点逐帧对齐）。
- ⚠️ **`aCM`（`AnimationClass` 侧的战斗模式）是从动，不是把手**：引擎自己每帧在推骑手的战斗状态机 ⇒ 旧的第 0 级 `rAnim->setCombatMode` 已删，`go(float)@0x60C4D0` 也**不需要**。
- ⚠️ **`isActionSlave`(0xB8) 由引擎维护** ⇒ 想从插件侧改它，另一把钥匙是 `restore()` 那个被 `pickupObject` destroy 掉的 `CharMovement` —— 而这**与 P3 剩下那一半是同一个岔口、同一笔代价**（交出「骑手不被坐骑碰撞体推挤」）。**要开就两件事一起衡量，别为其中一件单独开**（TASK.md P3 / P4-1j）。
- **骑手手里没有武器**：`wpn=0`（4/4，`getCurrentWeapon()` 是**已拔出**测试）、`aCW=5`（＝`SKILL_UNARMED`）而敌人 `eWpn=1`。⇒ P4-1d 的第 0 级是 `drawWeapon`。⚠️ **取「拔哪把」的来源后来被改过一次**：`getThePreferredWeapon()`（vtable `0x3C8`）**在被骑状态下读 NULL**（上马那一刻武器已收到背上），所以主体改成 `Inventory::getPrimaryWeapon()` → 退 `getSecondaryWeapon()` —— 它们问的是**装备槽**，拔没拔都答得出。`Inventory.h` **能干净 include**（邻居是 `Enums.h`/`util/lektor.h`/`Item.h`，都在 `kenshi/` 里），**不需要 shim**。
- **`drawWeapon` 不被「被骑」拒绝**（P4-1e-2，2026-08-30：**12/12 `post=1`**）⇒ 「骑着拔不出刀」不是权限问题，而是**约 14 帧之后有人把它收回背上**，且 `cma=1` 全程为 1 ⇒ **那个第二写手绑的是「被骑/被携」状态、不是战斗状态**（第一次收鞘可以用「战斗结束自动收」解释，重复收不行）。P4-3 的第一步就是给这个写手点名。⚠️ **别先写「每帧重新拔」**：那正是 HISTORY §B 那三轮伺服的形状（对每帧覆写做写入端补偿）。现有探针已按这个纪律写好——`drawWeapon` 门控在 `getCurrentWeapon()==NULL`、限 `kDrawTryBudget=12` 次、间隔 `kDrawTryGap=10` 帧，**计数器本身就是用来判「一次性状态转换 vs 每帧覆写」的**。
- ⚠️ **「骑着看不到别的动作」不是武器侧的证据**：`kRidePose` 带 `wholeBodyAllLayer` 且 `PoseLayerPin` 把它钉在 1.0，P2-1b-1 实测这会把**任何**其他 clip 压在 `w=0.000`。⇒ **看不见是设计使然**，判读武器/攻击时必须从 `wpn=`/`post=`/读回值上判，不能从「屏幕上有没有动作」上判。
- **收鞘 API 清单（2026-08-30 纯头文件检索，给 P4-3 第一步用；⚠️ 全部未验，一个都还没被调过或 hook 过）**：`CharacterHuman::sheatheWeapon()`@`0x5CC0A0`（**带 `_NV_` 桩** ⇒ 想**主动**收鞘可以直调，与 `_NV_ragdollModeUT` 同一机制）、`dropWeaponInHands`@`0x5CBFE0`、`dropWeaponInHandsFake`@`0x5C8EE0`、`leaveSheathEquipped`@`0x5D1D30`，配 17.3 的 `weaponInHands`(0x6D8) / `weaponInHandsSheathLocation`(0x6E0)。
  - **它能答的问题只有一个**：hook `sheatheWeapon` 打调用方返回地址 ⇒ **给那第二个写手点名**（P4-3 第一道门）。**两条前提已于 2026-08-30 静态查清（§18）**：
    - ~~①头里的 RVA 不可靠 ⇒ hook 前先看 `GetRealAddress(0x5CC0A0)` 那里的序言~~ —— **前半句与 hook 无关，我当初写重了**：`startPlugin()` 的 7 个 `AddHook` 全部走 `GetRealAddress(&Symbol)`，源码里没有任何字面 RVA ⇒ 头注释错不错影响不到 hook 装得上装不上。真实风险只有序言那一个。
    - ②**`AddHook` 无跳板、搬 5 字节不校验指令边界** —— 这条**仍然成立，而且已经查过并通过**：真实入口 **`0x5CC820`**（＝`0x5CC0A0 + 0x780`，RTTI 虚表槽 `0x2D0` 与语义双重确认），是 `.pdata` **精确入口**、`prolog=30`、头字节 `40 53 48 83 EC 60` —— **与在产 hook `AnimationClassHuman::_NV_update`@`0x5C5750`（`40 53 48 83 EC 20`）逐字节同形**，只差 `sub rsp` 的立即数。⚠️ 顺带纠正 §13 那条「安全目标＝纯 push/sub 序言」的读法：**两个在产 hook 的 5 字节都切在指令中间**（`4C 8B DC|57|48 81 EC…` 与 `40 53|48 83 EC 20`）⇒ **mid-instruction 不构成否决**，AddHook 实战容忍它。
    - ⇒ **序言这道门已开**；还没验的只剩「骑手这条路是不是真的走人类 override」，那要进游戏。
  - ⚠️ **也不许拿它当修法**：主动 `sheatheWeapon` 是收鞘、不是拔刀；而「每帧重新 `drawWeapon`」正是上一条禁的那个形状（HISTORY §B 的写入端补偿）。**这一条的用途止于「点名」。**
  - ⚠️ **别把基类那个当成同一个函数**：`Character::sheatheWeapon()`@`0x640D80`（vtable `0x2D0`，`Character.h:354`）与人类 override `0x5CC0A0` 是**两个地址**；hook 错那个 ＝ 骑手这条路上一次都不触发，看起来像「没有第二个写手」。（同样的坑在 `_NV_ragdollModeUT` 上已经踩过一次，见 §12。）
- **`ch=0`（`chooseAttack` 43/43 不给招式）已从谜团降级为后果**：无武器角色没有武器招式可挑。⇒ 修的顺序是**先把武器放回手里**，再回来问战斗层。
- **`weaponReach()` 是唯一与距离无关的那个事实**：P4-1c 的四次读发生在 `d=39.92/19.38/15.61/19.27`，所以 `inZ=0` 同样可以用「太远」解释；最终由 P4-1d 的 `reach=10.50` 对 `d=8.32-9.10` 排除距离。⚠️ **`chooseAttack` 的 reach 是入参**，不是它自己去问 —— 所以这条链上 `weaponReach()` 的值必须先对，否则挑出来的招式跟着错。
- **`chAnim` ＝ `CombatTechniqueData::animation`(0x0) ＝ 人类挥砍 clip 的「记录名」**，正好答掉 P4-3 的前提①（人形表里没有 attack ⇒ 它挂在 per-technique 数据上）。⚠️ **解析只许走 `FindAnimData()`（`allAnims.find()`），永远不许 `getAnimationData()`** —— 后者是 `operator[]` 语义，查不到就往引擎表里插一条永久 NULL（见 §15 与 CLAUDE.md 那条硬约束）。

### 17.5 ⛔ 禁止调用 / 禁止 hook（都已实测，别再试）
- **`CombatClass::calculateTargetsInAttackZone`@0x608020 ——「调用即 AV」**（P4-1c 那趟：第 0/1/2 级全部跑完并打出正常数据，加上这一句就在它里面炸）。**最可能的原因是它解引用 `currentTechnique`(0x150)，而战斗前那里 4/4 读到 NULL**。函数声明已从 shim 里**整条删掉**（源码 `:168` 留了一行墓碑注释），⚠️ **别为了「拿攻击区目标列表」把它加回来** —— 那份信息走 `getNearestEnemyInAttackZone()@0x6090B0` + `isInAttackZone()@0x607CE0`，两个都实测安全。
- 另外两个**禁止 hook 的地址**（§0 与 CLAUDE.md 都有，这里只提醒它们与 P3/P4 撞在一起）：`beingCarriedUpdate` RVA `0x5B5200`、`updateAnimationTransforms` RVA `0x5B0E30`。**「谁在把 `rMove` 拖回 carry 槽」的头号嫌疑正是前者**，而它恰好禁止 hook ⇒ P3 剩下那一半只能从**写入侧**解决（每帧 `teleportCollisionHull` 或直写 `pos`+0xC4），不能从拖回侧拦。

### 17.6 判读与编译陷阱
- **SEH 纪律**：所有探针都在 `__try/__except` 里（/EHsc 下 C++ `catch` **接不住** SEH AV）。⚠️ `__try` 不能与需要展开的对象共处一帧（C2712）⇒ 全部写成 `XxxImpl()` ＋ 外层 `Xxx()` 包壳这一对；`CombatProbe` / `gCmbBudget` 就是这个形状，**仍在源码里，下一轮 P4-3 直接复用**。
- **探针预算是必须的，不是礼貌**：`gCmbBudget` 限次 + `debugContinuous` 门控。战斗探针一行要打十几个字段，无预算会在一次交战里把 6.7MB 级日志刷满，而**判读那份日志只能靠脚本**（记忆规则：大文件绝不整读）。
- **判读顺序：先看「两个来源是否一致」，再看数值**。这一节所有裸偏移都成对配了 API（`0x130` 配 `_isInCombatMode()`、`0x1F0` 配 `getCombatState()`、`0x150` 配 `_currentCombatTechnique`(0x118)）。**不一致 ⇒ 偏移漂了，先修偏移，别去解释数值。**
- ⚠️ **「读到 0」经常有两个解释，要先找到那个与距离无关的字段**：P4-1c 的 `inZ=0` 既可以是「不在攻击区」也可以是「距离太远」，只有 `reach` 是不含距离的。**每加一个新探针字段，先问它会不会被距离/时机解释掉。**
- **shim 只能直调非虚方法**（§13 那条：虚函数要复现 vtable 布局，空壳做不到）。⇒ 本节两条路各走各的：**非虚的**（`_NV_initCombatMode` 等整张表）直接声明 + 链接器从导出桩解析；**虚的**（`drawWeapon` / `getThePreferredWeapon`）**按槽位偏移手动从 vtable 里取函数指针**（`0x3D8` / `0x3C8`），不进 shim。⚠️ 所以 17.1 那条「`CombatClass*` 横向 cast 成 `CombatClassAI*` 不加偏移」是**必需前提**——我们调的是派生类的函数体本身，不是让引擎替我们派发。
- **`hand`（`GameObjectHandle`）能进编译树、可以直接用**（`_getAttackTarget()` 返回它，`.getCharacter()` 取回指针）；⚠️ **它取回的指针照样可能悬空**（会话中读档），所以战斗侧拿到的每个 `Character*` 仍要过 `CharacterLooksLive`。

---

## 18. 静态 PE 定址：header RVA → 真实 RVA / `.pdata` 入口判定 / RTTI 取虚表（2026-08-30）

**这一节把 §13「KenshiLib 头文件 RVA 注释不可靠」从一句警告变成可执行的判定流程。** 全部是**纯文件检查**——不进游戏、不构建、不注入，所以任何时候都能跑。工具进仓库：`tools\ke_pe.py`（库）＋ `tools\hook_probe.py`（驱动，`--delta` / `--vslot` / 直接给 RVA）。

**先记住这条**：`startPlugin()` 里 7 个 `AddHook` **全部**传 `KenshiLib::GetRealAddress(&Symbol)`，源码里一个字面 RVA 都没有 ⇒ **头里的 RVA 注释错不错，跟 hook 能不能装上无关**。RVA 只在**离线判断「这个入口能不能 hook」**时才需要（＝看序言），以及做静态语义确认时。

### 18.1 `header RVA + 0x780` = 真实 RVA —— 只对一族符号成立
`Kenshi_x64.exe`（1.0.65，36718592 B，imagebase `0x140000000`）实测（`hook_probe.py`，14 个符号）：
- **+0x780 精确命中 `.pdata` 入口 7 个**：`AnimationClass::_NV_update`→`0x5B68C0`、`AnimationClassHuman::_NV_update`→`0x5C5750`、`beingCarriedUpdate`→`0x5B5980`、`AnimationClassHuman::_NV_ragdollModeUT`→`0x5BA050`、`CharacterHuman::sheatheWeapon`→`0x5CC820`、`dropWeaponInHands`→`0x5CC760`、`leaveSheathEquipped`→`0x5D24B0`。
- **另一族要 +0x790**：`getFacingDirection`→`0x2AE320`（+0x780 处是**纯 `CC`**，这一条站得住）、`dropWeaponInHandsFake`；⚠️ **另两条已被 18.6 推翻**：~~`Character::sheatheWeapon`→`0x641510`~~、~~`updateAnimationTransforms`→`0x5B15C0`~~ —— 它们 +0x780 处是 `C2 00 00`，而**那不是填充，那是个 3 字节 `ret 0` 的叶子函数**（＝空虚函数体）。基类 `sheatheWeapon` 的虚表槽实测就指向 `0x641500` 那个空桩，往后挪 0x10 挪到的 `0x641510` 是个跟武器无关的转发函数。**修正后的判据**：+0x780 处**纯 `CC`** ⇒ 才往后找 0x10；+0x780 处 `C2 00 00`（`classify` 报 `leaf` 而不是 `padding`）⇒ **它本身就是函数，别挪**。
- ⚠️ **5 个符号 +0x780 落在别的函数中部（MID+224 / 944 / 2368 / 720 / 416），真实地址未知**：`GameWorld::_NV_mainLoop_GPUSensitiveStuff`、`PlayerInterface::newPlayerTaskSelectedCharacters`、`ContextMenuGUI::show`、`InputHandler::loadConfig`、`DatapanelGUI::addCustomLine`。**这五个恰好都是生产环境里 hook 得好好的**（走 `GetRealAddress`）⇒ **「delta 对不上」不等于「hook 有问题」**，别去为它们找地址。
- `--delta` 的投票（±0x4000 全域暴搜）里 `+0x780` 拿 **7/14**，第二名 5/14 ⇒ **它是启发式，不是定律**。**硬规则：候选地址必须自己落在 `.pdata` 的精确入口上**，delta 只用来生成候选。换 exe 版本就重跑 `hook_probe.py --delta` 重新推。

四条独立证据（当初怎么定下 0x780）：①±0x4000 暴搜投票；②RTTI 取 `CharacterHuman` 虚表槽 `0x2D0` → `0x5CC820` = `0x5CC0A0 + 0x780`；③它把 `beingCarriedUpdate` 的头值 `0x5B5200` 映到 `0x5B5980`——**那正是早年靠实测找到的地址**；④语义确认（见 18.3）。

### 18.2 `.pdata` 是唯一权威的「这是不是函数入口」神谕
exe 的异常目录（data directory #3）里有 **77108 条 `RUNTIME_FUNCTION`**（`begin/end/unwind` 三个 dword）。排序后二分即可回答：**`entry`（精确入口）/ `mid+N`（在别的函数里面 N 字节）/ `leaf`（不在表里——小叶子函数不需要 unwind 数据）/ `padding`（`CC`）**。`UNWIND_INFO` 还白送 **`SizeOfProlog`**、flags（`0x4`=CHAININFO）、frame register。
- ⚠️ **`leaf` 不等于「地址错了」**，`padding` 才是强信号（往后 0x10 找）。`getFacingDirection`@`0x2AE320` 确实是真·leaf（不在 `.pdata` 里、无调用无栈帧），⚠️ **但「一个小 getter」这个说法是错的、已被反编译推翻**：`8B 42 08` 只是它的第一条指令，整个函数 **323 字节**、往对象里写 8 个字段 ⇒ **它是不是 `getFacingDirection` 现在存疑**（见 §18.9 ②）。它「全程直调、从没出问题」是真的，但那是因为直调走 `GetRealAddress`、**从不依赖这个 RVA**。
- **这套解析器的正确性由 `0x5B5980` 反过来证明**：第一遍扫 15 个头 RVA 全部 MID，唯一报 `ENTRY` 的就是那个**早已实测可用**的地址。

**序言＝hook 的唯一真实风险**（`KenshiLib::AddHook` 搬 5 字节、**无跳板**，§13）。判定手法 ＝ **拿生产环境的正对照比形状**，别去追求「5 字节正好是指令边界」：
| 目标 | 真实 RVA | prolog | 头 16 字节 | 5 字节切在 |
|---|---|---|---|---|
| ✅ `AnimationClass::_NV_update`（在产） | `0x5B68C0` | 56 | `4C 8B DC 57 48 81 EC D0 00 00 00 …` | **指令中间**（边界 3/4/11） |
| ✅ `AnimationClassHuman::_NV_update`（在产） | `0x5C5750` | 6 | `40 53 48 83 EC 20 48 8B D9 …` | **指令中间**（边界 2/6） |
| `CharacterHuman::sheatheWeapon` | `0x5CC820` | 30 | `40 53 48 83 EC 60 48 C7 44 24 20 …` | 同上（边界 2/6） |
⇒ **两个在产 hook 都是 mid-instruction 切的，所以 mid-instruction 本身不构成否决**。`sheatheWeapon` 与其中一个**逐字节同形**（只差 `sub rsp` 的立即数）⇒ **§17.4 那条「hook 前先看序言」的门已经过了**（见 18.4）。

### 18.3 虚函数：RTTI → COL → 虚表 → ILT 跳板
`hook_probe.py --vslot <Class> <slot>`。手法：找 `.?AV<Class>@@` 字符串 → TypeDescriptor = 串 RVA − 16 → 在 `.rdata` 找 `_RTTICompleteObjectLocator`（`sig==1` 且 `pTypeDescriptor==td` 且 `pSelf==col`）→ 找**指向该 COL 的 8 字节指针** → **虚表 = 该位置 + 8**。实测：`CharacterHuman` 虚表 `0x16F2848`（COL `0x183E498`，thisOffset=0，~134 槽）、`Character` 虚表 `0x16F9EB8`。
- ⚠️ **槽里装的是 /INCREMENTAL 链接跳板**（`E9 rel32`），必须跟一跳：`CharacterHuman` 槽 `0x2D0` → `0x302B5` `E9 66 C5 59 00` → **`0x5CC820`**。
- ⚠️ 同一次走查里 `Character` 槽 `0x2D0` 解出 `0x641500`＝`C2 00 00`＋填充。当时记的「真入口在 `0x641510`」**是错的、已由 18.6 推翻**：`0x641500` 就是基类那个虚函数的**全部函数体**（`ret 0`，空实现），`0x641510` 是隔壁一个无关函数。

**语义确认（不看反编译也能给函数「验明正身」）**：扫函数体里的 `lea rip-rel` → 落在 `.rdata`/`.data` 的可打印串，再数成员位移常量。实测 `0x5CC820` 体内两次读 `[this+0x6D8]`（`weaponInHands`）、两次读 `[this+0x6E0]`（`weaponInHandsSheathLocation`）、引用串 `"hands"`；`leaveSheathEquipped`@`0x5D24B0` 引用 `"hip"/"back"/"back2"/"sheath"`；`dropWeaponInHands`@`0x5CC760` 只有 28 字节、读一次 `[this+0x6D8]`。⇒ 三个地址与 §17.3 那两个裸偏移**互相印证**。

### 18.4 `KenshiLib::GetRealAddress`@`0x2510` 已完全解码 —— 但它拿不到任何地址
（走过一次的死路，留档免得再走。）`KenshiLib.dll` 里 `GetRealAddress` 把传进来的桩指针与窗口 **[0x156D6, 0x2344C]** 比较，`sub` 后除以 **stride 6**（`.rdata` `0x256E0` 那个 dword），再 `mov rax,[rcx+rax*8+0xD2000]`（rcx＝image base）读一张 **8 字节槽数组**：
```
0x255D lea rax,[rip+0x13172] -> 0x156D6      ; 桩窗口起点
0x2564 lea rcx,[rip+0x1DEE1] -> 0x2344C      ; 桩窗口终点
0x2578 mov ecx,[rip+0x23162] -> 0x256E0 = 6  ; stride
0x258D mov rax,[rcx+rax*8+0xD2000]           ; 槽数组（.data）
```
**共 9449 个桩**（窗口跨度 `0xDD76` 能被 6 整除，三个探针符号余数都是 0）。⚠️ **数组在文件里全 0，且 `.text` 里对 `0xD2000` 只有这一处引用（读）** ⇒ 它由 RE_Kenshi 加载时填，**静态文件里不存在任何已烘进去的真实地址**。这条路作废，改用 exe 自己的 `.pdata`（18.2），后者反而更强——它还顺带给序言长度。

### 18.5 关于 `beingCarriedUpdate` 的两条**存疑**记录（⛔ 禁 hook 不变）
新证据与旧记录对不上，**但一个字都不构成解禁**（那条禁令来自实测崩溃，仍然有效；§17.5 / CLAUDE.md）：
- 旧记录：「RVA `0x5B5200` 落在无关构造函数中部；真实 `0x5B5980` 是 /LTCG 死副本」。**新证据**：`0x5B5980` = `0x5B5200 + 0x780`，与 7 个符号同一个 delta，且是 `.pdata` 精确入口（prolog=51）⇒ 它更像**就是那个函数本体**，而 `0x5B5200` 只是没加 delta 的头值。
- 旧记录：「序言 5 个 1 字节 push → 搬 5 字节致栈不平衡」。**新证据**：`0x5B5980` 的字节是 `40 55 | 53 | 56 | 57 | 41 54 …`（`push rbp` 是 2 字节，因为带 REX）⇒ **5 字节处正好是指令边界**，这个解释与字节对不上。真正的崩因未知。
⇒ **处置：当成「原因待查的实测禁令」**。要重新审它必须有新的实测，不能靠这两条静态观察。

### 18.6 调用图方向：直调还是虚调 —— 决定 hook 该坐在哪个地址（2026-08-30）
**18.2 回答「这是不是真入口」，回答不了「引擎是怎么走到它的」**，而后者才决定 hook 装哪儿：
- 只有 `call/jmp [reg+槽]` 这类**间接**站点 ⇒ 调用是虚的 ⇒ 一个 `CharacterHuman` 接收者**必然**落在人类 override 上，hook override 就够。
- 有 `E8 rel32` **直调基类函数体**的站点 ⇒ 某处调用是非虚/限定调用、**绕过** override ⇒ 只 hook override 那条路一次都不会触发。

工具 `tools\callers.py`（纯离线，扫 exe）三种模式：
| 模式 | 找什么 | 编码 |
|---|---|---|
| `callers.py 0x<rva> [...]` | 直调站点 | `.text` 里每个 `E8`/`E9` + rel32，算 `site+5+rel` 是否命中目标 |
| `callers.py --vcall 0x<槽偏移>` | 间接站点 | `[REX] FF /2`(call) `/4`(jmp) 且 `mod=10` ⇒ modrm 后跟 disp32（`rm==100` 时先跳 SIB） |
| `callers.py --ptr 0x<rva> [...]` | **8 字节 VA 引用** | 直接搜 `<Q` 打包的 `base+rva`；⚠️ `findall()` 给的是**文件偏移**，要过 `pe.rva()` 换回来 |

**`--ptr` 是决定性的那一条腿**：光有「0 个直调站点」不够——调用完全可以走一个存在数据里的函数指针。「0 直调 ＋ 唯一的指针引用就是它自己那个 ILT 跳板、各一次、都在 `.rdata`」才等于**虚表是唯一入口**。

**实测：`sheatheWeapon` 是纯虚调，hook `0x5CC820` 就是对的**（TASK.md 原先记「必须进游戏才能验」，这条把它离线结掉了）：
```
callers.py 0x5CC820 0x641500 0x641510   ->  各 1 个直调站点，全部是 jmp 且 "NOT in any .pdata function"
                                             0x302B5 / 0x4B8D0 / 0x3535A  = 它们各自的 ILT 跳板
callers.py 0x302B5 0x3535A 0x4B8D0      ->  0 个直调站点（＝没人 E8 调跳板）
callers.py --ptr 0x5CC820 0x641500 0x641510 -> 0 / 0 / 0 个指针引用（函数体本身没被任何数据引用）
callers.py --ptr 0x302B5 0x4B8D0        ->  各 1 个，都在 .rdata：0x16F2B18 / 0x16FA188
```
- **`--ptr` 的两个 `.rdata` 命中独立复现了 18.3 的虚表算术**：`0x16F2848`(CharacterHuman 虚表)`+0x2D0 = 0x16F2B18`、`0x16F9EB8`(Character)`+0x2D0 = 0x16FA188`。两条完全不同的推导（RTTI 走查 vs 全镜像搜指针）落在同一对地址上 ⇒ **槽号 `0x2D0` 与两张虚表都是对的**。
- ⇒ 派发路径只有 `虚表槽 → ILT 跳板 → 函数体`。骑手动态类型是 `CharacterHuman` ⇒ 永远落 `0x5CC820`。**没有「基类旁路」这回事，不用怕 hook 错地址。**
- **顺带实锤：基类那个 override 是个空实现。** `Character` 槽 `0x2D0` → 跳板 `0x4B8D0` → **`0x641500` = `C2 00 00`（`ret 0`）**，前后都是 `CC`，16 字节对齐，不在 `.pdata`（叶子，合法）。而 18.1/18.3/§17.4 记的「基类真入口 `0x641510`」是**另一个函数**，31 字节、`prolog=6`：
  ```
  40 53              push rbx
  48 83 EC 20        sub  rsp,0x20
  48 8B 89 F8 02 00 00   mov rcx,[rcx+0x2F8]
  48 8B DA           mov  rbx,rdx
  48 8B 01 FF 50 40  call [[rcx]+0x40]        ; 对 +0x2F8 那个子对象发虚调
  48 8B C3 … C3      mov  rax,rbx / ret       ; 返回第二个参数
  ```
  两个参数、返回第二个参数、读 `[this+0x2F8]`——**跟武器一个字都不沾**（对比 18.3 的语义确认：真的 `sheatheWeapon`@`0x5CC820` 读 `[this+0x6D8]`/`[this+0x6E0]` 并引用串 `"hands"`）。⇒ 它只是**恰好**落在 `.pdata` 入口上的启发式假阳性，见 18.1 修正后的判据。
- ⚠️ **别去 hook `0x641500`**：只有 3 字节，`AddHook` 要搬 5 字节（无跳板，§13）；而且 hook 一个空实现本来就毫无意义。

**两条硬限制（结论只能写到这个份上）：**
- ⚠️ **这是静态扫描**，只看得见**编码在本镜像里**的调用站点：看不见存在数据里的函数指针（`--ptr` 只补了「有没有人存它的地址」这一半，存了之后怎么调看不见）、也看不见别的模块（KenshiLib / RE_Kenshi / 其它插件）发起的调用。**「0 个直调」＝「`.text` 里没有直调」，不等于「没人直调」。**
- ⚠️ **逐字节扫描会有假站点**：一段更长指令（或 `.text` 里嵌的数据）中间凑出 `E8 rel32` 的形状也会被报出来。**判据是 `in <function>`**——真站点必落在某个 `.pdata` 函数内部；`NOT in any .pdata function` 的命中要么是 ILT 跳板（本例就是），要么是噪声。
- ⚠️ **`--vcall 0x2D0` 那 ~20 个间接站点不能单独当证据**：disp `0x2D0` 不是类专属的，任何类的第 0x5A 个槽都是这个位移。它只能说明「有人在按这个槽做虚调」。

⚠️ **这一节结的是「hook 哪个地址」，不是「第二个收刀写手是谁」**。后者仍然要进游戏（那本来就是 P4-3 那个探针的用途），也仍然守 §17.4 的「用途止于点名」——**不许每帧重新拔刀**（HISTORY §B 写入端补偿的老坑）。





### 18.7 成员偏移扫描：收刀/拔刀那条流水线的静态全图（2026-08-30）

工具 `callers.py --field <成员偏移>`（谁碰这个成员）与 `--calls <rva> [depth]`（这个函数直调了谁）。**手法**：mod=10 disp32 + 白名单 opcode，区分读/写。⚠️ **成员偏移不是类专属的** —— `0x6D8` 的 54 个站点里有一批落在 7000-24000 字节的巨型函数中，那些几乎肯定是别的类恰好也有个 `0x6D8` 字段。⚠️ **`reg=='sib'` 且 modrm 后紧跟 `24` ＝ `[rsp+disp]` 栈帧，不是成员**（54 → 36 个真站点）。**校准**：先拿 §18.3 已记的三处事实对表（`0x5CC820` 读+写 `0x6D8`、`dropWeaponInHands`@`0x5CC760` 单次读、`leaveSheathEquipped`@`0x5D24B0` 三次读），三处全部逐一命中，扫描器可信。

**结构事实（新，全部静态可复现）**
- `Character` 成员：`0x6D0` `naturalWeapon`（`Sword*`，KenshiLib 头文件里有名字）、`0x6D8` `weaponInHands`、**`0x6E0` 是一个 `std::string`**（MSVC SSO 布局：buf/ptr@+0、size@+0x10、cap@+0x18；两个写手清它都是「size=0 ＋ `*buf=0`」的手法，不是指针赋值）、`0x6F0` 一个指针 ＝ 重挂那一步的门。
- **`Character` 虚表 `+0x198` → `0x5DBD80`（495 B，两个类同一个实现、人类没 override）＝「按位置串把武器挂回去」那一手**：串字面量 `"back"` / `"back2"` / `"backpack_attach"`，随后调 `0x535D50`（Appearance 挂载族；头文件把 `Appearance::attachItem(Item*, const std::string&)` 记成 `0x5355D0`，同一邻域）。**它自己一次都不碰 `0x6D8`/`0x6E0`**，位置串是参数传进来的。
- `+0x1A8` → 人类 `0x5CA740`（264 B）/ 基类 `0xD2040`；`+0x2D0` → 人类 `0x5CC820` / 基类 `0x641500`（`ret 0`，§18.6）。

**`CharacterHuman::sheatheWeapon`@`0x5CC820` 全解码**（274 B，`this`=rbx）：
1. 栈上建一个 `std::string("hands")`（`0x69D10` ＝ `std::string::assign`）
2. `animation(+0x448)->appearance(+0xE8)->detachItem(&"hands")`（`0x52E0F0`）—— **从「手」这个挂点上摘下来**
3. `animation->isHuman()`（虚表 `+0x20`，头文件确认）→ `0x51C9C0(human,0,0,0)`
4. `stats(+0x450)` 重算（`0x897F30`）
5. **`if (weaponInHands && weaponInHands != naturalWeapon && [this+0x6F0]) this->virt_0x198(&weaponInHandsSheathLocation)`** ← 真正「挂回背上」的那一步
6. `weaponInHands = NULL`；`weaponInHandsSheathLocation = ""`

⇒ **收刀不是一个动作，是三件事**：摘 `hands` 挂点 ＋ 按串挂回鞍位 ＋ 清两个字段；且对 `naturalWeapon`（拳/爪）直接跳过第 5 步。**对 P4-3 步骤② 的直接收益：挂载不用猜 `ATTACH_WEAPON` 那类枚举 —— 挂点是字符串**（`"hands"` 在手上、`"back"`/`"back2"` 在背上），**入口是 `Character` 虚表 `+0x198`**。

**`dropWeaponInHands`@`0x5CC760`（含尾巴 chunk `0x5CC77C`，合计 168 B）同族但挂点不同**：`if(!weaponInHands) return;` → `this->virt_0x1A8(weaponInHands)` → `appearance->detachItem(&weaponInHandsSheathLocation)`（⚠️ 摘的是**鞍位**不是 `hands`）→ 位置串赋空（`lea rdx,<空串 0x167C3C0>` + `0x69D10`）→ stats 重算 → `weaponInHands=NULL` → `isHuman()` → `0x51C9C0`。

**候选集（P4-3 步骤①「第二个收刀写手」）** —— `0x6D8` 的 36 个非栈站点里 12 个是写，落在 Character 邻域（`0x5C0000`-`0x5E0000`）的小函数只有这几条：

| 函数 | 长度 | 对 `0x6D8` | 对 `0x6E0` | 直调者 |
|---|---|---|---|---|
| `0x5C8150` | 78 | 写（byte imm） | 写 | 只有 `0x581770` |
| `0x5CBA10` | 66 | 写（存寄存器 ＝ **setter**） | 写 | 只有 `0x581770` |
| `0x5CBA60` | 83 | — | 读+写 | |
| `0x5CC760`(+`0x5CC77C`) | 168 | 读 + 写 NULL | 两次 lea | 已知 `dropWeaponInHands` |
| `0x5CC820` | 274 | 读 + 写 NULL | lea | 已知 `sheatheWeapon`（纯虚调，§18.6） |
| `0x5D0BFC` | 278 | — | 写 | |
| `0x5DC560` | 233 | 读 + 写 imm | — | |
| `0x5E6AB0` | 113 | — | 读+写 | |

剩下的写手全在 7000-24000 B 的巨型函数里（`0x77090` / `0x3CBFA0` / `0x816D90` / `0x841A50` / `0x8AB950` / `0xE876F0`），偏移撞车的可能性远大于「真是 Character 方法」。

**三条负面结果（记下来省重复劳动）**
1. **`0x5CC77C` 不是函数**：它是 `0x5CC760` 的第二条 `.pdata` 记录（0 直调者 ＋ 0 指针引用，且 `0x5CC760` 的代码直接流进去）。⇒ `--field` 打的 `in 0x…` 是 **chunk**，候选集必须按**逻辑函数**去重。
2. **KenshiLib 头文件给 `Character::sheatheWeapon` 的 `0x640D80` 同样是错的**：那个地址落在指令中部、不在 `.pdata`、0 直调者、0 指针引用；而且**整个 `0x630000`-`0x650000` 区间没有任何一处访问 `0x6D8`**。⇒ 这是「基类实现就是 `0x641500` 那个 `ret 0`」的第三条独立证据（前两条见 §18.6）。
3. **`beingCarriedUpdate`@`0x5B5980` 的 6 个直调被调方里 3 个已认出是库函数**：`0x69D10` ＝ `std::string::assign`（`sheatheWeapon`/`dropWeaponInHands` 都在用）、`0xED64F8`/`0xED6C80` ＝ CRT（`sheatheWeapon` 收尾也在用）。真正属于引擎的只有 `0x51D510` / `0x5B1A30` / `0x5B26E0` 三个。⇒ §18.6 记的「深度 3 前向可达集与写手集交集为空」**比原来说的更弱**：那棵子树几乎全是库代码，有意思的边都走虚表、静态看不见。

⚠️ **`--vcall 0x2D0` 定不了案，而且比 §18.6 估的更糟：实测 38 个站点、33 个不同宿主函数**。静态没办法给接收者定类型，所以「谁在 `drawWeapon` 之后 ~14 帧调收刀」仍然只能进游戏（探针/断点）。**本节结的是「这条流水线长什么样、候选集有多大」，不是「第二个写手已点名」**；用途仍止于点名，⚠️ **不许每帧重新拔刀**（HISTORY §B 写入端补偿的老坑）。

### 18.8 `Appearance::attachItem` 是**重载**，两个重载是两个函数 —— 拔刀走的是三参那个（2026-08-31）

**为什么这一条必须写下来**：P4-3 第 2 步的探针要答的问题是「被携态下 `attachItem("hands")` 到底被调了没有」，而**这种探针把「没打日志」读成「没调」**。§18.7:602 顺手记下的 `0x535D50` 是**收鞘侧**那条路的被调方；**拔刀根本不经过它** ⇒ 只 hook 它，每一次真拔刀都是静默，探针会**自己造出**它本该去挣的那个「没调」结论。

**两个重载，各自独立的精确入口（delta 都是 `+0x780`）**

| 头文件签名 | 头 RVA | 真实入口 | `.pdata` | prolog | 首 5 字节 |
|---|---|---|---|---|---|
| `attachItem(Item*, const std::string& slot)` | `0x5355D0` | **`0x535D50`** | ENTRY | 18 | `48 89 5C 24 10` |
| `attachItem(Item*, const std::string& mesh, const std::string& slot)` | `0x5356D0` | **`0x535E50`** | ENTRY | 24 | `48 89 5C 24 10` |

两条序言的前 5 字节都是**一条完整的**位置无关指令 `mov [rsp+0x10], rbx`（REX.W `48` ＋ `89` /r ＋ modrm `5C`（mod=01 disp8、reg=rbx、rm=100→SIB）＋ SIB `24`（base=rsp、无 index）＋ disp8 `10`）——**没有 rip 相对操作数、没有相对跳转** ⇒ 比两个在产正控制还干净（`AnimationClassHuman::_NV_update`@`0x5C5750` 的第 5 字节切在 `sub rsp,0x20` 中间，照样在跑；判据见 §18.2「别追求切在指令边界」）。

**调用方分裂（这就是全部结论）**

- **三参 `0x535E50` ← 拔刀侧**：`CharacterHuman::drawWeapon`（头 `0x5DB800` ＋0x780 ＝ **`0x5DBF80`**，ENTRY prolog=45，序言 `40 55 53 56 57 41 54 41 55 41 56 48 8D 6C 24 D9`）站点 `0x5DC1D4`；`leaveSheathEquipped`@`0x5D24B0` 站点 `0x5D2709`。⚠️ `leaveSheathEquipped` **自己也是 `drawWeapon` 的被调方** ⇒ 拔刀侧是三参族这件事有**两条独立确认**。
- **二参 `0x535D50` ← 收鞘侧**：虚表 `+0x198` 的「按位置串挂回去」`0x5DBD80` 站点 `0x5DBE8A`（§18.7:602 记的就是这一条）；`0x5D0BC7` 站点 `0x5D0CE0`；另有两处在 `0xCFDD0` 里（`0xD0048` / `0xD013C`，与武器无关的邻域）。
- **`--calls 0x5DBF80` 的 12 个被调方里有 `0x535E50` 与 `0x5D24B0`，没有 `0x535D50`。**

⚠️ **两个重载不能靠 hook 一个覆盖**：`--calls 0x535D50` 的 7 个被调方是 `0x6A6F0`(leaf) / `0x52D850` / `0x52E0F0`(`detachItem(slot)`) / `0x5358A0`(`createAttachedObject`) / `0x542090` / `0x544400` / `0x544BA0` —— **它是完整实现、不是转发桩，一次都不调 `0x535E50`**。⇒ 探针**两个都要 hook**，日志带 `ov=2/3` 区分，这样「静默」就只剩一种读法。

⚠️ **取槽名的寄存器按重载不同**（x64 调用约定，`this` 占 RCX）：二参 ＝ RCX=this / RDX=item / **R8=&slot**；三参 ＝ RCX=this / RDX=item / R8=&mesh / **R9=&slot**。搞错就会把网格名当槽名去比 `"hands"`，那又是一次静默。

**方法论：`callers.py` 打在真入口上只看见一个站点，那大半是 ILT 跳板，必须再追一跳。** 实测 `0x535D50` 只有 1 个直调者、而它是个 `jmp`（`jmp NOT in any .pdata function`）；追过去 `0x42163` → 4 个真站点。三参那边 `0x1760C` → 2 个真站点。**别把「1 个站点」读成「几乎没人调」。**

**顺带定死的两处逻辑函数边界**（MSVC 把一个逻辑函数拆成多条 `.pdata` 记录，`prolog=0` 的是续块，见 §18.7 负面结果 1）：T6 那个收刀站点 `0x5D051C`（`hook_probe` 报 `MID+298`）落在续块 `0x5D03F2-0x5D076D` 里，其入口是 **`0x5D0217`**（prolog=8）⇒ 逻辑函数 ≈ `0x5D0217-0x5D0779`；二参挂载站点 `0x5D0CE0` 落在续块 `0x5D0BFC-0x5D0D12`，入口是 **`0x5D0BC7`**（prolog=5）。

**T6 那一趟点名出来的第二个站点：`0x5CE183`**（2026-09-01 从 `TASK.md`／`TEST_REQUIRED.md` 收容进来 —— 此前本手册完全没有它，改无处改）。它和 `0x5D051C` 一样是**调用方里的返回地址、不是函数入口**（`site=` 的模块名都是 `kenshi_x64.exe` ⇒ RVA 有效）。实测配比：`0x5D051C` `real=16`（真把刀从手里拿走 16 次、gap 中位数 22 帧、重复出现）＋ `0x5CE183` `real=3`，`over=0` ⇒ 12 条站点表没满、**没有第三个真写手被挤掉**。⚠️ 想改这两个数字前先看 `TEST_REQUIRED.md` 的 T6 判据，它们是那五条验收里最吃重的两条的证据。

**delta 的三重佐证（这一族不是靠「+0x780 一般对」蒙的）**：①挂载族七个符号 ＋0x780 全部落在**互不相同**的精确入口上（没有别名到同一个地址）；②语义交叉验证 —— `detachItem(const std::string&)` 头 `0x52D970` ＋0x780 ＝ **`0x52E0F0`**，正是 §18.7:607 早就解码出来的 `sheatheWeapon` 第 2 步被调方；③`0x5DBD80` 传着位置串调 `0x535D50`，与头文件里那个二参签名对得上。

**挂载族全表（七个互不相同的精确入口，`hook_probe.py` 的 `--- P4-3 weapon API ---` 组里已常驻，无参跑一次即复现）**

| 符号 | 头 RVA | 真实入口 | prolog |
|---|---|---|---|
| `attachItem`（二参） | `0x5355D0` | `0x535D50` | 18 |
| `attachItem`（三参） | `0x5356D0` | `0x535E50` | 24 |
| `detachItem(slot)` | `0x52D970` | `0x52E0F0` | 43 |
| `detachItem(Item*)` | `0x52DB10` | `0x52E290` | 20 |
| `getAttachedEntity(slot)` | `0x52D1E0` | `0x52D960` | 15 |
| `createAttachedObject` | `0x535120` | `0x5358A0` | 50 |
| `createPhysicsAttachment` | `0x5357A0` | `0x535F20` | 55 |

⚠️ **序言这道门只答「hook 装得上」，不答「该不该调」**：本节全部是静态事实，**用途仍止于点名**，⚠️ **不许由此变成「我们自己去调 `attachItem` 把武器挂上去」**（对绝对覆写做写入端补偿 ＝ HISTORY §B 那三轮伺服的老路）。

### 18.9 反编译器就位（Ghidra 12.1.3）＋ 它当场推翻的两条（2026-09-01）

**工具**：Ghidra 12.1.3 ＋ Temurin JDK 21 便携装在 `D:\KenshiModDev\revtools\`（仓库外、无需管理员、不改系统环境）。`kenshi_x64.exe` 的 auto-analysis **506 秒**跑完并存成工程 `revtools\ghidra-projects\kenshi`（462 MB，**一次性开销、别重跑**），得 **169,169 个函数**。接了 MCP 桥 `pyghidra-mcp`；不经 MCP 也能查：`revtools\scripts\DecompAt.java`（按地址反编译）/ `FindSym.java`（按子串搜符号）走 `analyzeHeadless -process kenshi_x64.exe -noanalysis -readOnly -postScript`。⚠️ 脚本**只能写 Java**（这份 Ghidra 没按 PyGhidra 模式启动，`.py` 一律报 `Ghidra was not started with PyGhidra`）。⚠️ 搜符号必须用 `getName(true)` 取**全限定名**，`getName()` 会全 0 命中（类名在 namespace 里）。

**新能力：RTTI 类名是活的，`ke_pe.py` 的虚表库现在有名字了。** 6,290 个 `vftable` 标签 ＋ 18,989 条 RTTI 记录，且用的就是我们笔记里的类名：`AbstractMovementBase::vftable`@`0x1416FCBA8`、`CharMovement`@`0x1416FCC88`、`AnimationClass`@`0x1416F10E8`、`AnimationClassHuman`@`0x1416F1268`、`AnimationClassAnimal`@`0x1416F4588`、`AnimationClassBase`@`0x1416FA888`、**`PhysicsHullT`**@`0x1416DE808`（§13 记的「只前向声明」这条限制到此解除），`AnimationClassBase::SingleAnimation` 与 `::AnimationLayer` 也在。**Ogre 的方法名是真名**（`Ogre::AnimationState::setWeight` 直接出现在反编译结果里）⇒ 凡是调进 Ogre 的地方不用再猜。⚠️ **Kenshi 自己的方法名一个都没有**（`getFacingDirection` / `updateAnimationTransforms` / `beingCarriedUpdate` 全 0 命中）——stripped 是实的，RTTI 只给类名不给方法名。

**① `0x5B15C0` 的函数体不是「每帧更新动画变换」** —— §18.1:507 已按字节把 `updateAnimationTransforms→0x5B15C0` 划掉，反编译从语义上第二次否掉它。Ghidra 认这是**精确入口**（`EXACT_ENTRY true`）但整个函数只有 **83 字节**：

```c
*(undefined4 *)(param_1 + 0x54) = 0x3f800000;          // = 1.0f
if (*(AnimationState **)(param_1 + 0x28) != NULL) {
    Ogre::AnimationState::setWeight(..., 0.0);
    Ogre::AnimationState::setEnabled(..., false);
    if (*(char *)(param_1 + 0x68) != '\0') Ogre::AnimationState::setDisableTranslation(..., true);
}
*(undefined8 *)(param_1 + 0x28) = 0;                    // 断开那个 AnimationState
```

＝**拆掉一个动画槽位**，不是遍历骨骼的变换更新。`+0x28` 是 `Ogre::AnimationState*`、`+0x54` 是个 float、`+0x68` 是个 bool ⇒ 这个布局像 `AnimationClassBase::SingleAnimation`（它的 RTTI 与虚表现在都能查，见上）。⚠️ **这只是「函数体与名字不符」，不是「已确认它是别的函数」**，更**不构成解禁依据** —— `updateAnimationTransforms` 的真实入口**仍未定位**（`RidingPlugin.cpp` 的 `DISABLED HOOKS` 块写的 "The real entry is still unlocated" 才是对的口径）。

**② `getFacingDirection@0x2AE320` 不是「一个小 getter」，§18.2:515 那句要改。** 那里写它是「`8B 42 08 …` 一个小 getter」——`8B 42 08`（`mov eax,[rdx+8]`）只是它的**第一条指令**。Ghidra 认这里是精确入口、**323 字节**，反编译出来是：把 `[rdx+8..0x18]` 5 个 dword 拷进 `[rcx+0x158..0x168]`，写 `[rcx+0x125]`/`[rcx+0x128]`，再把几个 float 夹进 `[rcx+0x110..0x11c]`，带 ~7 个参数。**一个返回朝向的 const getter 不会往对象里写 8 个字段** ⇒ **`+0x790 → getFacingDirection` 这一条现在存疑**（它是 `+0x790` 那一族仅剩的两根支柱之一）。对得上的部分：`0x2ADB90`（头值）与 `0x2AE310`（`+0x780`）在 Ghidra 里**都没有函数** ⇒ §18.1:507「+0x780 处是纯 `CC`」与「头 RVA 不可靠」两条都被独立确认。

⚠️ **本节全是反编译读数 ＝ 静态事实，不是实测。** 反编译能证伪「这个地址是那个函数」，**不能**证明「换成这个地址 hook 就不崩」。禁 hook 令是实测结论、原样有效（`doc.md`「禁止注册的危险 hook 地址」节）。


## 19. 离线读 FCS 数据文件 + Ogre `.skeleton`（2026-08-31，全程没进游戏）

**为什么**：TASK.md 把 P4-3 前提②「挥砍 clip 的层与 `whole` 位」标成必须进游戏。可这两个字段是 **FCS `ANIMATION`(24) 记录的字段**，记录就存在 `data\*.base` / `*.mod` 里 ⇒ **静态可读**。工具 `tools\gamedata.py`（数据文件）与 `tools\skelanims.py`（骨架资产），两个都离线、都进仓库。

### 19.1 容器格式 —— **一种格式，四个文件**（没有「v16 记录格式 vs v17 记录格式」这回事）
```
header  [u32 fileVer][...][str depFiles]  BE 67 4C 00  [u32 recordCount]  <record>*
record  [u32 size][u32 itemType][u32 id][str name][str stringID][u32 flag] <body>
<body>  [u32 n](key,u8)* bools   [u32 n](key,f32)* floats  [u32 n](key,i32)* ints
        [u32 n](key,3f)* vec3    [u32 n](key,4f)*  vec4    [u32 n](key,str)* strings
        [u32 n](key,str)* filenames
        [u32 nRefCat]   ([str category][u32 n]([str targetSID][i32 v0][i32 v1][i32 v2])*)*
        [u32 nInstance] ([str name][str targetSID][3f pos][4f rot][u32 0])*
str = [u32 len][bytes]（utf-8）
```
- `BE 67 4C 00` 是**锚**：`recordCount` 紧跟其后，记录 0 再紧跟其后。
- ⚠️ **`size` 只有 `rebirth.mod` 填了**（含自身那 4 字节），另三个文件恒 0 ⇒ **走不了 `size` 步进**。能走的是 `<body>` 自定界：`off = bodyEnd` 对四个文件的每一条记录都正好落在下一条的 `size` 字段上。`rebirth.mod` 8571/8571 条 `bodyEnd == p + size`，等于白送一次交叉验证。
- **refcat 在 instance 前面，尾部没有多余的 u32**。
- `flag` 的**低半字节**才是 add/modify（0 或 2 ＝ 本文件引入这个 stringID，1 或 3 ＝ 修改一条已有的），高位是**按文件分族**的：`.base`/`Newwworld` 带 `0x80000000`，`rebirth`/`Dialogue` 是 `0x10/0x11/0x13` 外加 `0x40/0x60/0x70/0x80/0x90` 几族 ⇒ 拿整个 u32 去比会全军覆没。⚠️ **就算只看半字节它也不权威**：`rebirth.mod` 有 1375 条半字节 1/3 的记录，其 stringID 在更早的文件里根本不存在；`Dialogue.mod` 有 1005 条半字节 0 的记录反而确实盖着别人。⇒ **合并只能靠「这个 stringID 我见过没有」，绝不能靠 flag**（`union()` 就是这么做的）。
- ⚠️ **删除不是一个 flag 值**，是 body 里的 bool `REMOVED = 1`（104 rebirth / 26 Newwworld / 88 Dialogue）。
- 记录 `name` **可以是空的**（四个文件里 3 条）；`stringID` **不一定是 `<数字>-<文件名>`**（`PLAYER_WEAPONS` 这种手写的合法）⇒ 校验只能校到「可打印」，别写更紧的正则。
- 自检（`python tools\gamedata.py --verify`，改过格式代码就跑它）：四个文件全部 **header 数目相符、错 0、`size` 不符 0、终点正好落在 EOF**（9399 / 8571 / 2511 / 39077 条）。

### 19.2 两条必须的格式事实 —— 缺任一条，读出来的表就是错的
1. **覆盖记录的 body 是「部分」的**：`walk upper` 在 `gamedata.base` 里 61 个字段，在 `rebirth.mod` 的覆盖里只有 **4** 个。所以 `out[sid] = r` 会把没被重申的字段全部丢掉 —— 实测就是这么让 **124 条 `ANIMATION` 里 68 条完全没有 `layer`** 的。⇒ **必须按 key 合并**（`_merge()`：值/文件名/refcat 三块分别 update，refcat 以「类别」为粒度整表替换）。
2. **引擎跳过带 `disabled = 1` 的记录**：union 里 type-24 共 **124** 条，其中 **7** 条 `disabled`，扣掉这 7 条 ＝ **117 ＝ UPPER 79 / OVERLAY 21 / LOWER 17、`whole` 49**，与 §15 的游戏内 `allAnims` **逐个数字相同**。⇒ 这条既是必要的过滤，也是**整套离线解码可信的判据**：一个纯静态解析器精确复现了一次运行时枚举。那 7 条留档：`guard katana high` / `guard 1` / `guard 3`（upper）、`sidestep--`（lower）、`stand 1 sword noarms`（all）、`MA idle2 lower skill`（upper）、`aimwalk`（upper）。
- `layer` 是**字符串**：`all` / `upper` / `overlay` / `lower` / `tail`，其中 **`all` ⇔ 运行时 `lay=UPPER` ＋ `whole`**（拿 §15 的实测表对过）。`category` 是 int：0 NORMAL / 3 CARRIED / 4 SWIM。
- 载入顺序 ＝ `gamedata.base` → `rebirth.mod` → `Newwworld.mod` → `Dialogue.mod`，后者按 stringID 赢。⚠️ **单文件扫描既是下限、又会读错**：`gamedata.base` 只有 124 条 type-24 里的 75 条（117 条可达记录里的 69 条），`rebirth.mod` 另加 51 条**并且覆盖了 base 那 75 条里的 71 条** —— 和 `race_id.py` 同一个坑。

### 19.3 前提②的答案 ＝ **负面结论：那两个字段对 43 of 44 根本不存在**
`COMBAT TECHNIQUE`(17) 与动画的连接 ＝ 它的**字符串字段 `anim name`**（没有 reference category，技能唯一的 refcat 是 `events` ＝ 音效）。两条实测（`python tools\gamedata.py --tech` ＋ `python tools\skelanims.py --tech`）：
- **44 条技能里 43 条的 `anim name` 查不到任何记录** —— 既不是 `ANIMATION`(24) 的记录名、也不是 `ANIMAL ANIM`(5) 的记录名、也不是任何别的记录的 `anim name` 字段值（三种方式各查过一遍才认的）。那些名字是**裸 clip 名**：`chop left` / `blk right` / `attack1` / `dodgeback` / `ma chudan`…
- **同一批 clip 全部真实存在，作为 `.skeleton` 里的裸轨道**：44 条技能指向 **30 个不同的 clip 名**，**30/30 都找得到轨道**。27 个是人形 clip（`male_skeleton` 与 `female_skeleton` 各一份），另 3 个是动物用的 `attack1`/`attack2`/`attack3`（分别出现在 23 / 13 / 3 个动物骨架里）。`male_skeleton.skeleton` 自己有 **30 根骨、174 条轨道**（骨数与 §16 那份 30 根清单相符 ＝ 解析器对得上）。
- ⇒ **`layer` 与 `wholeBodyAllLayer` 是 RECORD 字段，不是轨道属性**。clip 有轨道而没有记录 ⇒ **这两个字段在 `data\` 里压根没被作者填过**，不是「我没找到」。**前提②在离线侧有答案，答案是「无从可读」。**
- **唯一的例外**：`Downward cut static` → `chop down static` **有**记录 ＝ **UPPER、不带 `whole`**（`loop, is action, uses right arm, uses left arm, delete tail, big stumble`）。一条孤例，别拿它推广到另外 43 条。

### 19.4 对 P4-3 的直接后果（TASK.md 步骤 3 按原样走不通）
TASK.md 写的是「让 `chooseAttack` 交出技能 → 读 `tech->animation`(0x0) 的**记录名** → 用 `FindAnimData()` 查它的层与 `whole` 位」。⚠️ **那个字段里装的是 clip 名，对 43/44 而言 `allAnims.find()` 必然 miss** ⇒ 这一步查不出层、也查不出 `whole`，**因为没有东西可查**。
- ✅ **`FindAnimData()` 这道守卫必须留着，而且现在更要紧**：miss 时返回 NULL 是**正确行为**；同一个名字直接送进 `getAnimationData()`（`operator[]` 语义）就会往引擎的 `allAnims` 里永久插一条空指针（§15 / CLAUDE.md 硬约束）。**攻击 clip 名是「一定 miss」的那一类，绝不许绕过它。**
- **推论（与 §15 的游戏内结果一致）**：人形表里 `attacks=0`、`ANIM_ATTACKS`/`ANIM_COMBAT` 各 0 条，而 `guard 1h` 那类**架势**在表里 —— 现在知道为什么了：**架势有记录，挥砍没有**。攻击的播放路径不经过记录表，所以它也不可能携带 `wholeBodyAllLayer`。
- ⚠️ **剩下的是纯运行时问题，静态答不了、别在文档里假装答了**：①一条**没有记录**的 clip 被请求时落在哪个层、走的是哪个 API；②我们钉着 `w=1.0` 的 `kRidePose`（UPPER＋`whole`）会不会像 P2-1b-1 那样把它压在 `w=0.000`。⇒ P4-3 的路线判断**不变**：手控骨 + blend mask 仍是唯一已证可用的杠杆（clip 压不动手控骨）。

### 19.5 Ogre `.skeleton`（`[Serializer_v1.80]`）—— 长度字段**不可信**
```
[u16 0x1000][str version]                  str 以 '\n' 结尾、没有长度字段
chunk = [u16 id][u32 recordedLength] <payload>
0x1010 BLENDMODE  [u16]
0x2000 BONE       [str name][u16 handle][3f pos][4f rot] (+[3f scale] 仅当非单位)
0x3000 BONE_PARENT[u16 handle][u16 parent]
0x4000 ANIMATION  [str name][f32 秒]  然后嵌套 0x4010 / 0x4100 / 0x4110
0x4100 TRACK      [u16 boneHandle]    然后嵌套 0x4110
0x4110 KEYFRAME   [f32 t][4f rot][3f xlat] (+[3f scale])
0x5000 ANIMATION_LINK [str skeletonName][f32 scale]
```
⚠️ **`o += recordedLength` 的平铺步进会当场脱轨**：Ogre 自己的 `calcBoneSize()` **漏算了名字字符串**，所以每条 BONE 恰好少报 `nameLen+1`（`male_skeleton` 骨 0 报 36、实占 42）。实测第一版就是这样只读出 1 根骨、0 条动画，然后在骨名中间把 `id=0x3304 len=1060439305` 当成 chunk 头。⇒ **按上面的布局结构化步进**，长度只在可信处使用：BONE 的 `36 vs 48` 用来判断有没有那个可选的 scale 三元组，KEYFRAME（计算式里没有字符串）可以直接按长度跳。

### 19.6 边界与用法
⚠️ **本节全部是「授权侧 / 资产侧」静态事实，不是运行时状态。** P1 早就证明两者会分叉（`crawl idle down` 表里写 `spd 0/0/0 play=0.00`，活体上 `t01` 每帧照推）。静态可以用来**挑候选**、读**静态属性**（层、flags、技能指名哪条 clip、clip 存不存在、时长）；**绝不能用来预测运行时权重、时序或混合结果**。
- `python tools\gamedata.py` ＝ itemType 直方图 ／ `--verify` 自检 ／ `--anim [子串]` ANIMATION 表 ／ `--tech` 技能→记录 ／ `--rec <子串>` 整条记录 ／ `--refs <子串>` 引用类别 ／ `--type <N>`。
- `python tools\skelanims.py` ＝ 人形骨架的骨数＋全部轨道 ／ `--find <子串>` 过滤 ／ `--skel <文件名子串>` 换骨架 ／ `--list` 全部 `.skeleton` 的轨道数 ／ `--tech` 上面那张交叉核对表（它 `import gamedata`）。
- 附带结论：type-112 `base animations`（`1533847-gamedata.base`）**不靠引用枚举任何东西**（0 个 refcat，只有两个 `.skeleton` 文件名字段）；所有名为 `animations` 的 refcat 都挂在 **itemType 76** 记录上。
