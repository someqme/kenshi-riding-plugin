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

- carry：`pickupObject` 0x5CFF90、`getPickedUp` 0x5CED90、**`getDropped(bool ragdollHim, bool hull)` 0x5CC420（被携方放下 handler，下马核心）**、`dropCarriedObject` 0x5CDA60、`_carryMode` 0x5CE1E0、`setCarryMode` 0x51C8D0、`beingCarriedUpdate` 0x5B5980（死副本，勿 hook）；CharMovement `destroy`/`restore` 0x65F6C0/0x661810（pickupObject 对被携方 destroy movement，getDropped 经 restore 复活——rev4/6 根因链）
- ragdoll：`Character::ragdollMode` 0x5CB5E0（**排队**+恢复回调，骑乘勿用）、`CharacterHuman::postRagdollCallback` 0x5CBAB0、`AnimationClass::ragdollModeUT` 0x5B9290（虚，vtable +0x50）/ **`AnimationClassHuman::_NV_ragdollModeUT` 0x5B98D0（人类 override，骑手用这个）**；`Character::isRagdoll` 0x7D01E0、`Character::isDown` 0x28D910（疑为「只出放倒」菜单的门）；`RagdollPart`(Enums.h:1001) `NONE/WHOLE/RIGHT_ARM/LEFT_ARM=4/HEAD=8/RIGHT_LEG=0x10/LEFT_LEG=0x20/CARRY_MODE=0x800/ARMS=6/LEGS=0x30/ALL=0xFFFF8000`
- **人类骑手要调 override 版、且走 `_NV_` 导出桩**：`AnimationClassHuman` 覆写了 `ragdollModeUT`（0x5B98D0 ≠ 基类 0x5B9290），虚分派语义正确但依赖 KenshiLib 头**复现 vtable 顺序**——而头里 RVA/offset 注释已知不可靠（人类头把该 override 的 vtable offset 写成 0x0）。故 `static_cast<AnimationClassHuman*>(pAnim)->_NV_ragdollModeUT(...)`：导出桩加载时解析真实地址（= `_NV_update` hook 同一机制），单继承 offset 0x0 指针无需调整。人类判定沿用插件既有的 `!isAnimal()`（`AnimationClass::isHuman()` 是纯虚、无 `_NV_` 桩，不可直调）。
- Character：`_isBeingCarried` 0x3D4、`isCarryingSomething` 0x348、`carryingObject` 0x380、`animation` 0x448、`ai` 0x650；`getAI` 0x268220、`setDestination(Vec3,bool)` 0x5C8E30、`getPosition`(virt) 0x5CDF00、`getRadius` 0x5C7C30（非虚，导出桩直调）
- AI：`isTargetInRangeForOrders(RootObject*)` 0x5997B0（⚠️ 语义=「可否对目标下指令」，对玩家恒 true，**非捡起距离，勿用于到达判定**）、`isTargetInRangeForOrders(Vec3)` 0x597010、`isWithinGivenRange(hand,Vec3,float)` 0x5A2900、`addGoal(TaskType,RootObjectBase*)` 0x5C88A0（备选：若 setDestination 驱动不动，改用它的目标机制）
- CharMovement：`pos` 0xC4、`destination` 0x0DC、`movementMode` 0x378；vtable 槽 `getPosition`=0x40、`isDestinationReached`=0x60、`halt`=0x98（AbstractMovementBase 段）
- ContextMenuGUI：`contextMenuTarget` 0xA8、`optionsList` 0xF8；`ContextMenu::orders`(lektor<int>) = 与 optionsList 子项平行的 order 数组
- 输入/设置窗口（未实现的自定义按键用，见 §14）：`InputHandler::addCommand` 0x363130、`InputHandler::loadConfig` 0x361F80、`controlEnabled` 偏移 0xD0、`DataPanelLine_KeyConfig` 0x3E7740、`OptionsWindow::getSingleton` 0x4067B0（`keysDatapanel` 0x100）、`resetAllKeys` 0x3E7030、`saveOptions` 0x3EC570、`DatapanelGUI::createLine` 0x6FC270（protected → 需 shim）

## 13. RE 工具经验（2026-08-27 从 `CLAUDE.md` 迁入）

- **inline hook 崩溃**：hook 引擎只搬 5 字节不校验指令边界。安全目标=纯 push/sub 序言，或补 replay 跳板（KenshiLib::AddHook 无）。纯 push 序列被跳过同样栈崩。
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
