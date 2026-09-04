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
| `AnimationClass::startCombatAnimation(tech, w, "")` | `0x5B7080` | `AnimationClass.h:489` ＝ `run` 的**未试同胞**（隔一行）。⚠️ **一次都没调过**；`run` 的判决见 17.9，这一条是 17.10 留的候选 |
| `AnimationClassBase::playAction(AnimationData*, spd, w0, stumble)` / `playAction(const std::string&, …)` | `0x51E330` / `0x51FC30` | `AnimationClass.h:439-440`。⚠️ 两个**都没调过**；按名那个可能内部走 `getAnimationData()`＝`operator[]`（miss 插永久 NULL，见 19.4）⇒ **不许盲调** |
| `AnimationClassBase::restartAction(const std::string&)` | `0x51C400` | `AnimationClass.h:438`，未调过，同一条 `operator[]` 警告 |
| `AnimationClassBase::runAnimation(const std::string&, spd, blend)` | `0x339D00` | 未调过（我们只用 `AnimationData*` 那个重载 `0x5B7340`），同一条 `operator[]` 警告 |
| `CombatClass::go(float)` | `0x60C4D0` | **不需要**，见 17.4 |
| `CharacterHuman::sheatheWeapon`（虚 override）/ `_NV_sheatheWeapon` | 头 `0x5CC0A0` → **真实 `0x5CC820`** | `CharacterHuman.h:22-23`；真实入口经 RTTI 虚表槽 `0x2D0` ＋ `+0x780` ＋ 语义三重确认，`.pdata` 精确入口 `prolog=30`，**序言那道门已过（§18.2）**；**派发是纯虚调 ⇒ 骑手必落这个地址、没有基类旁路（§18.6 实锤）**；基类 `Character` 的槽 `0x2D0` 指向 `0x641500`＝`ret 0` **空实现**（旧记的「真实 `0x641510`」是错的，那是隔壁一个无关函数，§18.6） |
| `CharacterHuman::dropWeaponInHands` / `dropWeaponInHandsFake` | `0x5CBFE0` / `0x5C8EE0` | `CharacterHuman.h:49-50`，头里写 protected（shim 要声明成 public，见 17.1 最后一条） |
| `CharacterHuman::leaveSheathEquipped(section, ypos)` | `0x5D1D30` | `CharacterHuman.h:55`，头里写 protected；「把武器留在鞘里」那一路 |
| ⛔ `CombatClass::calculateTargetsInAttackZone` | `0x608020` | **会 AV，永远不要调**，见 17.5 |
- **两个虚表槽**（`Character` 自己的 vtable，按槽位偏移手取函数指针、不进 shim）：`drawWeapon(Item*, const std::string&)` = **`0x3D8`**（人类实现体 `CharacterHuman` @`0x5DB800`），`getThePreferredWeapon()` = **`0x3C8`**（⚠️ 被骑状态下读 NULL，见 17.4）。
- **`Weapon*` 传给 `drawWeapon` 前要 `reinterpret_cast` 成 `Item*`**（理由见 17.1 最后一条）。
- ⚠️ **这张表里的数字是头文件注释里的 RVA，不是「真实 RVA」** —— 它们够用是因为**这些全是直调**（写 `rAnim->runCombatAnimation(...)`，地址由 KenshiLib 解析），**没有一个走 `GetRealAddress`**。要拿它们当真实地址（反编译／`.pdata` 判入口）得先按 §18.1 加 `HEADER_RVA_DELTA 0x780` 再验，`getAnimationState` 就是这么对上的（头 `0x51C320` → 真实 `0x51CAA0`，§21.4）。⇒ 表里那五条「未调过」的**风险是编译期链不上，不是运行期跑飞**。
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

### 17.7 ⛔ **玩家包里骑手根本进不了战斗模式** —— 架势／补拔／收刀抑制三件全是诊断态产物（2026-09-02 第十四趟，标的 `D3D4879E…`，**诊断全程 OFF**，两趟骑乘，同一份 DLL）

**这是十四趟里第一次在「玩家会看到的形状」下量骑乘战斗**：DLL 与第十三趟**同一份**（md5 `D3D4879E…`），唯一差别是 `debugContinuous` 全程没开（运行时键，`RidingPlugin.cpp:370` 默认 OFF、`:8468` ＝ Ctrl+小键盘句点）。

| 量 | 第十三趟（诊断 ON，四趟骑乘） | 第十四趟（诊断 OFF，两趟骑乘） |
|---|---|---|
| `P43SUP ride real=`（真抑制掉的收刀） | 134 / 26 / 54 / 0 | **0 / 0** |
| `pass=`（放行的收刀） | 524 / 242 / 0 / 52 | **507 / 189** |
| `P43RD drawn=` | 2 / 1 / 0 / 0 | **0 / 0**（`fail=0 nowpn=0`） |
| `P43HD ride nonnull=/samples=` | 4884/17334、1048/7869、1381/1381、264/1521 | **0/13691**、**1/4402** |
| 肉眼 | 空手上马会拔刀、刀全程在手、打完自己收 | **空手全程没刀；持刀上马也被收；两段打架全程坐姿** |

- **架势一次都没起来，这条是硬的**：`P43RD drawn=0 fail=0 nowpn=0`（三个计数器都无门控、每趟骑乘打印）＋ 第一趟 `nonnull=0`（13691 帧手上全空）⇒ 不可能是补拔第一道守卫 `if (rider->getCurrentWeapon()) return;` 拦的（`RidingPlugin.cpp:3440`）⇒ **补拔函数一次都没被调用** ⇒ **`0→1` 边沿一个都没有**。⚠️ 别拿 `cm=`／`st=` 采样当证据：`kDtLinesSiteH=4` 意味着 12 行带旗标的 `P43DT`/`P43HD` 全落在上马后 0.5 s 内（128.440–128.699、277.365–277.832，两趟骑乘分别到 262.393／321.856），打架时段**没有采样**。
- **拆刀的人还是引擎收刀路径，不是新的第三方**：`hands` 拆卸 189 次全出自**一个**站点 `kenshi_x64.exe+0x5CBDF8`（§18.11 静态表里），和 `pass=189` **一比一对上**（§18.11.1 的结论没被推翻，只是这趟没人去抑制它）。
- **根因 ＝ `RideStanceRaw` 的第二个条件**（`RidingPlugin.cpp:3301`，`rider->isInCombatMode(true, true)`）。第十三趟的时间线把因果钉死：
  - `95.864 STANCE 0 f=6917 cm=0`（已上马、无战斗）
  - `203.828 P41D ai … r cm=0` → **同一帧** `203.828 P41D rung=0 … post rTgt=1 cTgt=1 cm=1 cmM=1 in=1 opp=1`
  - **13 ms 后** `203.841 STANCE 1 f=16701 cm=1 d=37.2` ← 全趟第一个架势边沿；四个边沿全在杠杆开始写之后
  - 杠杆 ＝ `RiderCombatLever`，**`debugContinuous` 门控**（`:8060`）；rung 2 ＝ `cc->setAttackTarget`＋`setAttackTargetHandle`（`:7663`-`:7664`），rung 3 ＝ `rider->attackingYou(enemy,true,false)`。源码自己写明它只在诊断下跑，**因为它写真实游戏状态**（`:593`-`:594`）。
- **为什么玩家包里骑手进不了战斗模式**：上马会**掉 aggro**（`:7596`-`:7598`，2026-08-30 实测），敌人也从不改打骑手（§17 P4-1b 结果：rung 4 无效、`eTgt=2`＝坐骑，33/33）⇒ 骑手既没 attacker 也没攻击目标 ⇒ `isInCombatMode` 恒 false。
- ⇒ **连带失效的不止 P4-3**：P4-2「坐骑护主」的入口是 `rider->getAllAttackers()`（`:8069`），玩家包里同样恒空 ⇒ 那段从不下令；**P4-1「参战资格」的实测证据（P4-1b RESULT，`:596` 起）本来就是「诊断已开」时取的**，不构成玩家包里的证据。⚠️ §18.12.1「修法成立」那条**没有**被推翻 —— 鞘位参数确实是拦路的闸，只是它下游那一整条链在玩家包里从来没被点燃过。
- ✅ **顺路收掉一笔欠账**：上马后一两秒那 7 对手槽抖动（§18.12.1）**是我们自己的梯子造的** —— 诊断关掉后 `gain=0/0`、`loss=0/1`（第十三趟是 14/12/0/11）。
- ⏳ **仍未读到**：`kDrawTryBudget` 的 `left=`（补拔没触发 ⇒ 一行没打）**→ 第十五趟已读到，见下**、`P4-4` 落地姿态（这趟没触发强制下马）。

**修法（用户 2026-09-02 裁定走「只读放宽」；✅ 已实测通过 ＝ 第十五趟，标的 `312832 B` / md5 `E83DB50D17267C7C2DFA67A4BB144D3C`）**
- 架势第②项换成新的 `RideFightIsOn(rider, mount)`，四问取或：`rider->isInCombatMode(true,true)`（原项保留）∥ `mount->isInCombatMode(true,true)` ∥ 坐骑有活的攻击目标 ∥ 坐骑 `getAllAttackers()` 里有活人。**全部是读，一个引擎写都没有** —— 这就是它与 `RiderCombatLever` 的分界。
- ⚠️ **只改第②项不够，这是本节最容易踩的坑**：第③项 `RideNearestThreat` 原本也**只**读骑手的 `getAttackTarget()` 与 `getAllAttackers()`，玩家包里两者同样空 ⇒ 架势会卡在第③项、再废一趟。所以它同时加了 `mount` 参数，候选按**优先级**四档：骑手攻击目标 → 骑手 attackers → **坐骑攻击目标 → 坐骑 attackers**。距离照旧从**骑手**位置量 ⇒ `kRideThreatDist` 与 `RideTwistTargetDeg` 的角度含义不变；四档共用新的 `RideThreatConsider()`（`isDown()`/`isDead()` 同等除名，**并把坐骑本身排除出威胁**，否则玩家攻击自己坐骑时骑手会朝着屁股底下摆架势）。`RideTwistTargetDeg` 也跟着收 `mount`。
- ⚠️ **`mount->isInCombatMode` 是未测量项，不能当修法本体**：没有任何一份日志记过它 —— `P3CMB` 的 `cmM=` 是**骑手**的 `isInCombatMode(true, false)`（`RidingPlugin.cpp:7278`），`M` 那个字母骗人。真正有实测支撑的是**坐骑的攻击目标与 attackers**：第十三趟 `200.497 P3CMB … mAtk=1 … mTgt=1`，`202.263` 已 `mAtk=6`，其间 `cm=0 cmM=0 rTgt=0 rAtk=0` ⇒ **坐骑的账本比杠杆早 3.3 秒就是满的**，而骑手的账本要等杠杆。
- 🆕 判据探针 `P43FT`（**不受 `debugContinuous` 门控**，每趟骑乘一行，紧跟 `P43SUP`）：`ok= noElig= noFight= noThr= far= dmin=` ＝ 三个条件里是哪一个拒绝的 ＋ 见过的最近威胁距离。理由 ＝ `STANCE` 那行是诊断门控的，诊断 OFF 的趟失败了会无从下手。`ridelog.py` 的 `-- T18 --` 一节自动判，并会指出日志是否早于本构建。⚠️ T19 摘探针时它与 `P43DT`/`P43HD` 一起摘。
- **没采纳的候选**：把 rung 3 `rider->attackingYou(enemy,true,false)` 或 rung 2 `cc->setAttackTarget`（`:7663`-`:7664`）搬进出货路径 —— 它写真实游戏状态，副作用范围（AI 仇恨表、玩家自己的命令）没量过。

**✅ 实测结果（第十五趟，2026-09-02，诊断全程 OFF，两趟骑乘，日志 `RE_Kenshi_log_trip15_E83DB50D_diagoff.txt`，16638 B / 199 行）—— 修法成立，本节这条 ⛔ 关闭**
- 肉眼（用户报）：空手上马打一架 ＝ **有攻击架势、会出刀**；持刀上马打一架 ＝ **保持出刀、有攻击架势**。对照第十四趟的「全程没刀、全程坐姿」。
- `P43FT ok=2182` / `2585`（两趟都 `noElig=0`，`dmin=7.6` / `8.4`）⇒ 架势靠**坐骑的账本**起来了，`RideFightIsOn` 不用等骑手的 `isInCombatMode`。
- `P43RD drawn=2` / `1`，`post=1` **3/3**、`sheath '' -> 'back'` **3/3**、`aCW` 全部离开 `SKILL_UNARMED(5)`；`P43SUP real=50` / `66`；`P43HD nonnull=1164` / `1512`（第十四趟是 0/13691 与 1/4402）。
- 🔑 **三个补拔边沿全部 `cm=0`** —— 这正是修法生效的形状：架势第②项已经不再要求骑手的战斗模式。⚠️ `ridelog.py` 里原来那条「every edge fired with cm=1」的 CHECK **必须**因此改成 NOTE（判别用「有没有 `P43FT` 行」，**不能用字节数**：`312832` 与 `_prev_E7613634` / `_prev_544D8C86` 撞号），否则以后每趟都误报一次假故障。
- ✅ **`kDrawTryBudget` 的 `left=` 收掉了**：第一次成功边沿 `left=11`、同趟第二次 `left=10`、第二趟第一次 `left=11`，与裁定一致。
- 🔎 **「坐骑 attacker 列表不清空」这条设计风险确认存在，60u 闸也确认是它的挡板**：`far=2400` / `688`（威胁找到了但超出 `kRideThreatDist=60`）⇒ 打完架的旧敌人真的还留在列表里，但架势没有被它们吊住。
- ⏳ 肉眼四条里「打完收起来」用户这次没提（日志侧 `loss=3`、第二趟末 `last=0000000000000000` 与它一致，但那半只有肉眼能判）。另：第一趟骑乘是**坐骑倒地**结束的（`mount down @292.785`）。
- ⇒ 连带解冻：P4-2「坐骑护主」仍然按 `rider->getAllAttackers()` 下令（`RidingPlugin.cpp:7766`，行号 2026-09-02 复核过；T19 摘探针后旧记的 `:8069` 已挪位），**那条没跟着修** —— 玩家包里骑手的 attacker 列表照旧恒空，所以那段下令到今天还是没派上。⚠️ 但**别把这条读成「坐骑不参战」**：用户 2026-09-02 肉眼报「看到坐骑在打人」，坐骑自己的引擎 AI 被打就反击，那一半不需要我们的代码 ⇒ 这条下令的边际价值很低。要修就照本节的办法把坐骑的账本也读进去。

### 17.8 🔑 **有刀在手之后，`chooseAttack` 会给出真技法，而 `runCombatAnimation` 真的能播** —— 挥砍这条路线的地基（2026-09-02，**离线复判第十三趟日志**，零游戏时间）

标的 ＝ 第十三趟那份日志（`D:\KenshiModDev\RE_Kenshi_log_trip13_D3D4879E.txt`，T16 鞘位修法 ＋ 诊断 ON）。方法 ＝ 有界 grep，**没有整读**。

- ⛔ **`ch=0` 43/43 那条判词已过期，而它自己的注释就写着为什么。** `RidingPlugin.cpp:754-760` 把 `ch=0` 归因为「空手的角色没有武器技法可挑」；T16／T18 把刀塞回手里就取消了这个前提。第十三趟的 `P41D read` 行里 **`ch=1` 连片出现**，技法名与技法自己的距离字段：

  | `anim=` | `init=`（`0x38` initialDistance） | `minS=`（`0x3C` minDistanceVsStatic） |
  |---|---|---|
  | `downward combo` | 10.00 | 10.00 |
  | `bigchopv2` | 25.00 | 20.00 |
  | `chop left-3` | 0.00 | −10.00 |

  ⚠️ 三个名字**都没有 `AnimationData` 记录**（§19；`bigchopv2` / `chop left-3` 另见 `TASK.md` P4-3）⇒ 走 `FindAnimData`／`ClipPin` 必然拿不到入参，这不是缺陷、是那条路的天花板。
- ✅ **rung 3 ＝ `AnimationClass::runCombatAnimation(chTech, 1.0f, "")`（`@0x5B6E80`，§17.2 表）执行过 7 次，带非 NULL 技法，零 AV**：`rung=3` 落在 206.338／210.175／213.864／217.541／379.445／405.453（+1），同帧回读 `tech=1 wpn=1 reach=10.50 cma=1 aCM=1 cst=3/4`，全趟没有一行 `access violation in rung`。**`inZ=0 nearZ=0` 每一次都有** ⇒ 引擎不会替被携带的骑手**派发**一刀，但会替他**播**。
  - ⇒ 它吃 `CombatTechniqueData*` 而不是 clip 名（`+0x0` 就是裸 clip 名，§17.2 的 `CombatTechniqueData` 行）⇒ **这是唯一绕开「43/44 条技法没有记录」的播放路径**。
  - ⚠️ `0x5B6E80` 的函数体**没有反编译过**，所以「它内部怎么找到那条轨道」仍是未知；这一节能说的只是「调了，没炸，引擎状态跟着动了」。
- 🔑 **引擎驱动的一次性 clip 不驱逐循环架势 —— 这与第十趟我们自己钉的那条正好相反。** rung 3 之后 `guard 1h` 仍是 `play=1 w=1.000`，只有 render 侧 `mainState` 从 `1.000` 掉到 `0.909`（`P41K` @206.280 对 @206.600）。读法：`ClipPin` 的全局 `target + others <= 1.02f` 门（`RidingPlugin.cpp:5998`）因为 `others` 抬起来了而拒发 guard 自己的 `setWeight` ⇒ **刀在播，只是压在架势底下**。
  - ⇒ 出货形状必须在挥刀期间**停止主张架势**（第十趟 §U 绕这道门用的同一招），而且**两处**都要停：`HaltAndForceSitPass`（render 侧 `true`）与 `animUpdate` 前置 pass（render 侧 `false`）。§U 只处理了前者，理由是「引擎会把 guard 踢出播放列表」—— 那条对**我们钉的** clip 成立，对**引擎播的** clip 不成立。
- 落地形状与判据在 `TASK.md` P4-3-4；实测欠账在 `TEST_REQUIRED.md` T20。

### 17.9 ⛔ **`runCombatAnimation` 不把技法送进 `rAnim->layer[]`** —— 第十七趟实测，`LegPoseFindHost` 是这条结论的独立探测器（2026-09-02，诊断全程 OFF，四趟骑乘／两场打架，标的 `308224 B` / md5 `635436E9D4A90FE14B5287AAECB086D4`，日志 `RE_Kenshi_log_trip17_635436E9_diagoff.txt`）

**触发条件那一半成立**：`P43SW ride swing=5 tech=17 skip=12 fail=0`（第一趟）＋ `swing=1 tech=20 skip=19`（第四趟），六个窗口，技法名 `bigchopv2`（`init=25.00`，在 `d=21-23` 开）与 `chop left-3`（记录没意见 ⇒ 退 `reach=10.50`，在 `d=9.95`/`10.19` 开）。`runCombatAnimation` 每次都带真技法、**零 AV**。

**但屏幕上什么都没播，而且赔上了一个回归**：用户报「打斗中骑手在牛背上**站直**一会儿」。日志把成因钉到帧 —— **六条 `LEGPOSE released grace=12`，每条在对应窗口开启后 106–159 ms**：

| `P43SW open` | `grace=12` 释放 | 延迟 |
|---|---|---|
| 175.748 | 175.854 | +106 ms |
| 178.256 | 178.377 | +121 ms |
| 184.745 | 184.862 | +117 ms |
| 205.297 | 205.443 | +146 ms |
| 209.798 | 209.919 | +121 ms |
| 422.040 | 422.199 | +159 ms |

`grace=12` 的含义 ＝ `LegPoseFindHost` 连续 **12 帧**找不到宿主（＝ 没有任何一个 `SingleAnimation` 拥有 `AnimationState`）⇒ 跨骑 mask 被交还 ⇒ 引擎自己的 `idle_stand_normal` 接管腿（takeover 行里就写着这个 clip 名）⇒ 骑手站起来。

⇒ **两条结论**：
1. **`runCombatAnimation` 没有把那条技法放进 `rAnim->layer[]` 的 add/remove 列表里**（至少没有以「拥有 `AnimationState` 的 `SingleAnimation`」的形式）。`LegPoseFindHost` 在这里是一个**独立于观感的探测器**，它看到的是一个空舞台。⚠️ 它只扫 `layer[]` 两张表 ⇒ **不能排除**「引擎把战斗动画放在别的通道里」，但屏幕上也确实什么都没有。⚠️ `0x5B6E80` 函数体没有反编译过，所以「为什么没进去」仍然未知。
2. **P4-1i 那条约束的代价被量化了**：「什么都不主张 ⇒ 引擎渲染 BIND 姿势」不只是上半身的事 —— 它会连带触发 `LegPoseFindHost` 的宽限并把腿部 mask 交出去。**撤掉架势只有在有别的真宿主接手时才安全**。§U 当年撤得掉，是因为它钉的是 `mid blow`（有记录 ⇒ 有 state ⇒ 是合法宿主）。
- ✅ 回归侧干净：`straddle takeovers=10 ＝ restored 4 + released 6`、`man=0x00`、`minDot=1.0000`、`dropped=0`、`late=24`；`P43SUP real=374`；`P43RD post=1` **3/3**、`sheath '' -> 'back'` 3/3、`cm=0` 3/3（T18 之后的正常形状）；零 AV。用户报「其他和之前一样」与日志一致。
- ⇒ 下一份构建（`308736 B` / md5 `AF771D99FA0D904691FE38815FCDD9CD`）把窗口播的东西换回 `mid blow`（第十趟实测过的机制），并**顺带把这个问题问到底**：闭窗时对技法名做一次纯读 `getAnimationState()`（§21.4 的调用，`RidingPlugin.cpp:4643` 已在用），日志字段 `ogre=found/absent`。`found` ⇒ 没有记录的裸轨道确实有 state、驱它自己的权重重新成为选项；`absent` ⇒ 这条路彻底关掉。⚠️ §21 对此**没有**表态，只能实测。

### 17.10 ✅ **`mid blow` 当宿主成立，挥砍第一次在玩家包里落地** —— 顺带答出「没有记录的技法轨道**确实**有 `AnimationState`」（2026-09-02 第十八趟，诊断全程 OFF，四趟骑乘／两场打架，标的 `308736 B` / md5 `AF771D99FA0D904691FE38815FCDD9CD`，日志 `RE_Kenshi_log_trip18_AF771D99_diagoff.txt`，34653 B / 402 行）

**肉眼（用户报）**：①**打架时看得见挥砍** —— 但「不像挥砍，有点像右手给左手割腕」＝ 有新动作、不像一刀（成因见下面第二条新事实）；②⛔ **不再「在牛背上站直」**（§17.9 那条病修掉了）；③四条回归（出刀／有攻势／持刀保持／打完收刀）＋ 跨骑腿型**两场都和之前一样**。

**日志侧全过**：`P43SW ride` 四行 `swing=4 / 0 / 4 / 9` ＝ **17 个窗口**，`tech=59` 命名、`skip=42` 距离不合、`noclip=0`（`mid blow` 每次都解析到）、`guardoff` 合计 **5126 帧**（一次性 clip 真的拿到了上身）；🔑 **`grace=` 释放 0 条 / 17 个窗口**；`d<=lim` **17/17**；**零 AV**；straddle `takeovers=4 ＝ restored 4 + released 0`、`man=0x00`、`minDot=1.0000`、`residue=0`、`dropped=0`、`late=14`；`P43RD post=1` **9/9**、`sheath '' -> 'back'` 9/9、`cm=0` 9/9（T18 之后的正常形状）。⚠️ 第二趟骑乘 `swing=0` **不算失败** —— 那趟没打架（按「有打架的趟」读，同 T19）。

**引擎自己挑的技法**：`bigchopv2` ×11（`init=25.00 minS=20.00`，在 `d=20.40–24.95` 开）、`chop left-3` ×6（记录没意见 ⇒ 退 `reach=10.50`，在 `d=7.79–9.80` 开）。

🔑 **新事实一 —— `ogre=found` 17/17：没有 `AnimationData` 记录的裸轨道确实拥有 `Ogre::AnimationState`。**
闭窗时对技法 clip 名做的那次**纯读** `getAnimationState()`（§21.4 的现成调用，`RidingPlugin.cpp:4643` 同一条）对 `bigchopv2` 与 `chop left-3` **每次都非 NULL**。⇒ **§19 那道墙只挡 `FindAnimData` / `ClipPin` / `allAnims`，不挡 Ogre 侧**（那条链按名字走，完全不碰 `allAnims`，§21.1）⇒ 「**直接驱那个 state 自己的权重**」重新成为选项，而且它是唯一能播**引擎自己挑的那一刀**的路。
🔑 **`enabled`/`weight`/`timePosition` 这一趟其实读到了**（探针 `RideSwingProbeState` 一直在印 `en=/w=/t=/len=`，34 行齐全 ＝ 17 open ＋ 17 close），三种形状：
- `en=0 w=1.000 t=0.000 len=2.833` ×22 ＝ `bigchopv2` 的**出厂默认**（Ogre 建好就没人碰过）；
- `en=0 w=1.000 t=0.000 len=1.067` ×2 ＝ `chop left-3` 同上；
- `en=0 w=0.000 t=0.251 len=1.067` ×10 ＝ `chop left-3` **被播到 23% 然后被关掉、权重清零**。这一版**已经没有 `runCombatAnimation` 调用**（§17.9 摘掉了）⇒ 动它的只能是**引擎自己**（骑手下马时的徒手／持刀战斗）。
⇒ 两条读数：**引擎确实驱这些 state**（所以驱得动），而**我们采样的那些时刻它们全是 `en=0`**（所以引擎在骑乘中一次都没启动过它们）。⚠️ 三个字段**不需要新探针**，下一轮别再加一遍。

🔑 **新事实二 —— `mid blow` ≈ 4.8 s，而且窗口之间不重置时间。这就是「像割腕」的成因。**
`prog=` 有两种形状，互相印证出同一个时长：
- `0 → 0.517` 用了 **2.502 s**（open 158.510 → close 161.012 ＝ 撞上 `kRideSwingLenMs` 2500 ms 硬上限）⇒ clip ≈ **4.84 s**；
- `0.518 → 0.900` 用了 **1.828 s**（open 161.020 → close 162.848 ＝ 正常达标闭窗）⇒ clip ≈ **4.79 s**。

⇒ 一个窗口只播这条 clip 的 **~52%**，而且**下一个窗口从上一个停下的地方接着播、不是从 0** ⇒ 屏幕上是一条 4.8 秒通用重击的**任意半截**，有时还是中段 ⇒ 读起来不像挥砍。印出来的 12 条闭窗行：8 条 ≈`0.514–0.518`（撞上限）、3 条 `0.900/0.901`（达标）、1 条 `1.010`。
⇒ 要它像一刀，三个旋钮：**每次开窗把 `timePosition` 归零**、**放大 `kRideSwingLenMs`**、**调 `speed`**。⚠️ 三个都还没动过，没有实测。

**⇒ 下一轮的杠杆按上限排序**（都未实测）：①走 Ogre 侧驱真技法（新事实一打开的路，上限最高 ＝ 真·武器技法）；②`startCombatAnimation`（`0x5B7080`，`run` 的未试同胞，隔一行，成本最低）；③把 `mid blow` 这半截修整齐（归零＋加长＋调速，保底、必定有效但永远是通用重击）。

### 17.11 ✅ 挥砍的**观感**修法：钉着的 `SingleAnimation` 跨窗口不清时间，所以要自己归零（2026-09-02，用户裁定走 §17.10 的杠杆③，标的 `311296 B` / md5 `B69680BCBE1DFB36EE1B7D18E96F820C`，**第十九趟实测通过**）

**机制（§17.10 新事实二的结构解释，不是新实测）**：`ClipPin` 钉的那条 `AnimationClassBase::SingleAnimation`
**活得比窗口长** —— 窗口闭了只是不再每帧请求它，条目还在 `layer[].addList` 里、`currentFrameTime01`
原样冻着。所以第 2 个窗口从 `0.518` 接着 `0.517` 播（实测数字）。由此还有一个**更糟的推论**：如果某个窗口
是「达标闭窗」（`prog ≥ kRideSwingDoneProg`），那么下一个窗口开的时候残留进度**已经满足闭窗条件** ⇒ 它在
**第一帧就闭**、一帧都没播。第十八趟 12 条闭窗行里有 3 条 `0.900/0.901` ⇒ 这个坑真的踩到过。

**修法（三处，都在骑乘战斗那一支内；不碰 `ClipPin` 的 `target + others <= 1.02f` 门、不碰触发条件）**：
1. **`RideSwingRestart()`** —— 每窗**一次**，把条目的 `currentFrameTime` / `currentFrameTime01` 归零、写
   `speed`，并 `mainState->setTimePosition(0)`（只为让**第一帧渲染**就从头开始；引擎下一次 update 会自己从
   `currentFrameTime01` 重算，所以这一笔是保险不是主力）。字段偏移 `speed +0x40` / `currentFrameTime +0x50` /
   `currentFrameTime01 +0x54` 出自 `AnimationClass.h`（`SingleAnimation`），与 `ClipPin` 写的
   `weight/desiredWeight/stillWanted/mainState->setWeight` **同一条脚跟**。
   ⚠️ 找条目**没有**用 `getAnimationPlaying(AnimationData*)`，而是照抄 `ClipPin` 的 `layer` 遍历（只认
   `addList`，`removeList` 上的是要走的尸体）⇒ **不引入新地址、不在出货路径上加新引擎调用**。
   ⚠️ 它必须挂在 **`HaltAndForceSitPass`**：pass 顺序（`RidingPlugin.cpp:7037-7040`）让它跑在读 `prog` 的
   `RideSwingPass` **之前**，否则「第一帧就闭」那条修不掉。
   ⚠️ 闩**按成功**记（`gRideSwingRestartTick = gRideSwingOpenTick` 只在归零成功后写）：第 1 个窗口的条目要等
   `runAnimation` 先建出来，按尝试记会把那一窗的唯一一次归零花在空转上。
2. **常量**：`kRideSwingLenMs` 2500 → 3000、新增 `kRideSwingSpeed = 2.5f`、`kRideSwingMinGapMs` 2000 → 3200。
   ⚠️ **不变式 `kRideSwingMinGapMs > kRideSwingLenMs`** —— 间隔从**开窗尝试**起算，间隔小于上限会让被上限截断的
   窗口在闭窗的下一帧又开，架势永远建不回来（而「架势回不回来」只能看**下一个**窗口的 open，HISTORY §U）。
   `speed` 也**同时**传给 `runAnimation`：它只在**创建**条目时取速度，第 2 个窗口起条目已经存在 ⇒ 两处都写才覆盖两种情形。
   ⚠️ `kRideSwingSpeed` 是**从没验证过的旋钮** —— 这个 DLL 以前每一次 `runAnimation` 都传 1.0f。引擎若把它压回
   1.0，这一趟也不白跑：旋钮①单独就把「任意中段」变成「从起势开始」，闭窗行的 `sp=` 会直接说是哪种。
3. **`RideSwingProbePin()`（纯读）** —— 读**我们钉的那条 clip**（以前所有探针都在读技法侧那条）：`sp=`/`t01=`/
   `pw=`/`psw=`/`oen=`/`olen=`。🔑 **`olen=` 是第一次直接印出 `mid blow` 的真实长度**（§17.10 只能靠两个 `prog`
   读数 ＋ 时间戳推出 ~4.8 s，而上面三个常量全部是按那个推算定的）。开窗行印 `pre`（归零**前**的残留）、闭窗行印
   结果 ＋ `ms=`（窗口时长）/ `rst=`（归零次数，必须 ＝ `swing=`）。
   ⚠️ 它的字段名与技法侧 `ogre=` 探针**故意全部不同名**（`pinst=`/`pw=`/`pt=`/`oen=`/`olen=`），因为两个探针印在
   **同一行**上 —— HISTORY §U 记的就是「一行两个 clip 同名字段、`kv()` 静默返回错的那个」。

**判据**：`tools\ridelog.py` 的 `-- T21 --` 一节（`rst== swing` ／ 零 `ms>=2950` ／ 零 `ms<200` ／
`pw>=0.95`＋`psw=1`＋`oen=1` 全数 ／ 既有回归零变化）。验收与退路写在 `TEST_REQUIRED.md` 的 T21 条。

**实测结果（第十九趟，2026-09-02，诊断全程 OFF，四趟骑乘／三场打架，日志 `RE_Kenshi_log_trip19_B69680BC_diagoff.txt`，41760 B / 487 行）—— 三个旋钮全部成立**：
- 🔑 `rst=14 ＝ swing=14`（每窗一次归零，承重判据）／**零**窗口一帧就闭 ⇒ 上面推的「残留进度让窗口空转」
  这个坑**已经堵死**，`prog` 闭窗 min `0.510` max `0.904`。
- 🔑 **`sp=2.50` 14/14** ⇒ **`kRideSwingSpeed` 是真旋钮**，引擎的 update 不把它压回 1.0
  （上面那句「从没验证过的旋钮」到此定案：验证过了，活的）。
- 🔑 **`olen=4.633 s`** ＝ 第一次直接读到 `mid blow` 的真实长度（§17.10 的 ~4.8 s 推算偏大 0.17 s，
  三个常量按它重算后不需要改）⇒ `sp=2.5` 下 0→90% 需 **1668 ms**，实测窗口 `ms` min 1734 max 3000 avg 1846。
- `pw>=0.95`＋`psw=1` 14/14／`oen=1` 14/14／`ogre=found` 14/14／`grace=` 释放 **0** 条 / 14 窗口／
  `guardoff=2842`／`noclip=0`／零 AV／straddle `takeovers=4 ＝ restored 4`／`minDot=1.0000`／`dropped=0`。
- ⚠️ **13/14 而不是 14/14**：`392.640` 那个窗口撞到 3000 ms 上限时只播到 `prog=0.51`（同一份 clip、同一个
  `sp`，说明起步那一下确实会慢）⇒ 判据里「零窗口 `ms>=2950`」实际有 1 条。不影响结论（肉眼「动作完整了」），
  但**再抬 `kRideSwingLenMs` 必须同抬 `kRideSwingMinGapMs`**。
- ⚠️ 开窗行 `pinst=none` **14/14** 是**预期形状**，不是失败：开窗那一帧条目还没建出来（归零发生在下一帧的
  `HaltAndForceSitPass`），所以「归零前的残留」这个读数在这套 pass 顺序下**永远读不到** —— `rst=` 才是它落地的证据。
- 肉眼：①**动作完整了**，但用户原话「这个动作实在不像挥砍」⇒ 观感缺口不在时间轴上，在 clip 本身（§17.12）；
  ②⛔ 不再「在牛背上站直」；③v1.6 四条 ＋ 跨骑腿型一条都不退。

### 17.12 ✅ **驱动技法自己的 `Ogre::AnimationState`** —— `mid blow` 不是攻击 clip，这是绕开 §19 那道墙的唯一活路（2026-09-02 构建 `312832 B` / md5 `882134675C2FFB514A0C526BE0375E57`，**2026-09-03 第二十趟实测：机制成立**）

**为什么必须换路**：§17.11 把时间轴修满之后，缺口剩下一个 —— 播的东西不对。`mid blow` 是**替身**，
不是攻击动作；而 44 条技法里 **43 条没有 `AnimationData` 记录**（§19）⇒ `FindAnimData`/`ClipPin` 这条路上
**没有第二个候选**可换。⇒ 换 clip 这个方向本身是死的。

**为什么这条路有戏（已实测的地基，不是推测）**：`ogre=found` 第十八趟 17/17 ＋ 第十九趟 14/14 ＝ **31/31**
（§17.10／§19.4／§21.5）—— 没有记录的技法轨道**确实**拥有 `Ogre::AnimationState`，按 clip 名走
`AnimationClassBase::getAnimationState` 就能拿到，**完全不碰 `allAnims`**。⇒ §19 那道墙只挡 `allAnims` 一侧。
另一半地基：Ogre 自己**不推进** state 的时间，谁拿到谁负责驱动 —— 引擎给自己的攻击就是这么干的
（第十八趟抓到它把 `chop left-3` 播到 `t=0.251` 再 disable）。

**手法（`RideSwingDrive` / `RideSwingUndrive`，都在骑乘战斗那一支内）**：窗口内每帧
`f = elapsed / kRideSwingTechMs`（夹到 1.0）→ `setLoop(false)` ＋ `setTimePosition(len*f)` ＋
`setEnabled(true)` ＋ `setWeight(kRideSwingTechW)`。驱**相位**不驱速率 ⇒ `bigchopv2`（2.833 s）与
`chop left-3`（1.067 s）在同一个窗口里都走完整条弧。两个主张点与钉 `mid blow` 的两处**一一对应**
（`HaltAndForceSitPass` 的请求点 ＋ `animUpdate` 前置 pass），闭窗沿与 `Dismount()` 各 `Undrive` 一次。
- ⚠️ **纯加法**：`mid blow` 的 pin／权重／速度／归零**一个字节没动**，它仍然是 host。§17.9 的教训是
  「什么都不主张 ⇒ 引擎渲染 BIND ⇒ 腿部 mask 交还、骑手在坐骑背上站直」⇒ 任何新机制只能加在它旁边。
  ⇒ 这条路失败的代价上限是「多一层混合／看不出变化」，**赌不掉第十八～十九趟已经拿到的东西**。
- ⚠️ `kRideSwingTechW = 1.0f` 是**从没实测过的旋钮**：这个 DLL 从来没写过任何 Ogre state 的权重
  （§21 只用它**读**）。1.0 与 host 的 1.0 并排 ＝ 一半一半的混合，**肉眼看得出来**；压低 host 的权重才是
  拿已过的行为去赌。
- ⚠️ `kRideSwingTechMs = 1700` 不是编的：它是第十九趟窗口时长（1734～1846 ms）的复述。
- 计数器 `drv=`（写成功的帧数）印在闭窗行与每趟汇总行上，与闭窗行既有的 `en=/w=/t=/len=` 一起**把两种失败分开**：
  `drv=0` ＝ 写从来没发生（`ogre=absent` ＝ 按名取 state 拿到 NULL；否则 ＝ SEH 壳吃掉了 AV）；
  `drv>0` ＋ `en=0` ＝ **写进去了、引擎又清掉了** ⇒ 下一级是改到 `animUpdate_orig` **之后**主张（同一个 hook，
  不需要新地址）。**这两条别猜，闭窗行会说。**
- ⚠️ 这一节**答不了**的事：一个 enabled ＋ 有权重的 state 到底有没有到骨架。只有肉眼能答；
  若 `en=1 w=1.000 t≈len` 而肉眼毫无变化 ⇒ Kenshi 自己那张表之外的 state 根本不被应用 ⇒ **这条路到此为止**。

**判据**：`tools\ridelog.py` 的 `-- T22 --` 一节 (B) 半；验收与退路写在 `TEST_REQUIRED.md` 的 T22 条。

#### 实测结果（2026-09-03 第二十趟，诊断全程 OFF，七趟骑乘／多场打架，日志 45374 B / 506 行）

- 日志：`drv>0` 在每个 `swing>0` 的趟上（334/186/453/391/365/108）／闭窗 `en=1` **11/11**／`w=1.000`／
  `t/len = 100%` **11/11**／`ogre=found` 11/11／零 AV／`grace=0`／`noclip=0`／`guardoff=2386`／
  straddle `takeovers=7 ＝ restored 6 + released 1`（released ＝ 骑手被打倒那次的设计行为）／
  `dropped=0 residue=0 late=25`／技法 `bigchopv2` ×9 ＋ `chop left-3` ×3。
- 🔑 **肉眼：state 确实到了骨架** —— 用户原话「这个动作不是挥砍，他是把刀从右手往胸口收，然后刀飘到头顶，
  **整个人缩成一团瞬移到坐骑前面。动作幅度很大**，但是毫无意义」。
  ⇒ ⛔ **上面那条预登记的死刑判据没有发生，发生的是它的反面**：不是「不被应用」，是**应用得毫无约束**。
- ⇒ 由此坐实的两条新事实：
  1. **写一个 Ogre state 的权重是可行的**，1.0 会被真的应用 ⇒ `kRideSwingTechW` 从「从没实测过」升级为
     「已知可用的旋钮」（压低它是软化幅度的手段，见 §17.14 的退路①）。
  2. **没有 `AnimationData` 记录 ⇒ 没有 `SingleAnimation` 条目 ⇒ `LegMaskApply` 结构上碰不到它**
     ⇒ 这条被驱动的轨道是全场**唯一一条没有 blend mask 的贡献者**，root/pelvis/腿全都归它。
     「瞬移到坐骑前面」＝ 地面 clip 的 root motion 没人替它应用 `whole`/`reloc`（§19）；
     「缩成一团」＝ 它与 pin 住的 `mid blow`（同为 1.0）在同一批 spine/arm 骨上叠加。
- ⚠️ 加法纪律在这一趟得到确认：T20/T21 的十四项与 v1.6 四条＋跨骑腿型＋不站直**一条没退**
  （用户原话「v1.6 四条 ＋ 跨骑腿型 ＋ 不在坐骑背上站直，一条都没退」）。
- ⚠️ `ms=3000` 撞上限 **2/11**（`662.297` prog=0.241、`678.678` prog=0.256，同一趟）＝ 与第十九趟 1/14 同一件事，
  **这一轮没动 `kRideSwingLenMs`**（抬它必须同抬 `kRideSwingMinGapMs`）。

### 17.14 ✅ **互补分工：技法守座位、host 让出上半身** —— §17.12 的约束（2026-09-03，标的 `316416 B` / md5 `519F0F445DECE74729B2620CC54C09EC`，**第二十一趟实测通过**）

**要解决的是 §17.12 实测出来的两个症状**，各有一个不同的成因（上一节末尾），所以修法是两半，
且两半都只用**已经背着 straddle 跑过十几趟的同一套机器**：`Ogre::AnimationState::createBlendMask` /
`setBlendMaskEntry` / `destroyBlendMask`，泄漏纪律见 §21.2。

- **技法侧**（`RideSwingDrive` 内，每帧幂等）：`Bip01`、`Bip01 Pelvis`、左右 `Thigh`/`Calf`/`Foot` **八根钉 0**。
  ＝「技法可以要躯干，不许要座位」。`RideSwingUndrive` 交还：我们建的 mask 直接 `destroyBlendMask()`，
  本来就存在的只把这八项写回 1.0（`gRideSwingMaskMine` 记着是哪种）。
- **host 侧**（`LegMaskApply` 内，仅 `RideSwingInFlight` 为真时）：`Bip01 Spine`、`Neck`、`Head`、
  左右 `Clavicle`/`UpperArm`/`Forearm`/`Hand` **十一根钉 0**；`Spine1`/`Spine2` 不进新表，改成既有腿表的
  `ours = spineMask || swingFree`（一个条目只有一个写者）。host 保住腿 —— 那是它唯一撑着的东西（§17.9）。
  ⚠️ **手指两侧都不列** ⇒ 握刀手型仍归 host。
- 两张表**都按名解析**（`getHasBone` ＋ `_getBone()->getHandle()`，与腿表同一处、同一对调用），
  每根骨在 DLL 载入时各打一行 `SWING hold bone` / `SWING free bone`（`has=0` 会静默废掉半个修法）。
  ⚠️ **一个 handle 都不许猜**：这具骨架 30 根骨，`0`/`1` 只是**看起来**像 root/pelvis。
- 自证字段：闭窗行 `hold=`（那一窗写进技法 state 的条目数，应 ＝ 已解析的 hold 骨数）／
  每趟汇总行 `swfree=`（host 让位的帧数，＝ 这一半的 `guardoff`）。
- ⚠️ **这一节的回归风险是「泄漏一个 0.0」，不是崩**：11 根上半身骨现在会在窗口期内被钉 0，
  一趟骑乘若**在窗口里结束**（下马／被打倒／读档），`LegMaskApply` 不会再跑来写回 1.0 ⇒
  `LegMaskRelease` 的引擎侧分支同时补上这 11 项，`LegMaskResidueCount` 也一起审计（`residue=`）。
  技法那张 mask 泄漏的受害者不同：`AnimationState` 属于**这一个实体** ⇒ 泄漏会让**玩家自己的地面攻击**
  root 死掉（挥刀不再向前迈步）⇒ **日志看不见，只有下马在地上挥两刀能看见**。

**判据**：`tools\ridelog.py` 的 `-- T23 --` 一节；验收、四支退路与「不通过时第一个看哪」写在
`TEST_REQUIRED.md` 的 T23 条。

**实测结果（2026-09-03 第二十一趟，诊断全程 OFF，四趟骑乘／两场打架，日志 `RE_Kenshi_log_trip21_519F0F44_diagoff.txt`，29377 B / 329 行）—— 两半都成立，四支预登记退路一支都没用上：**
- 🔑 **肉眼三条**：⛔ 不再「瞬移到坐骑前面」、⛔ 不再「缩成一团」、✅ **下马后在地上挥两刀正常**
  （＝ 技法那张 mask 没有泄漏；这是本节唯一日志看不见的判据，只能这样测）。v1.6 四条 ＋ 跨骑腿型
  ＋ 不站直 ＋ 不突然转向都不退。
- 两张表**按名全数解析**：hold 8/8（handle `Bip01`=0 / `Pelvis`=1 / L·R `Thigh`=2·7 / `Calf`=3·8 /
  `Foot`=4·9）、free 11/11（`Spine`=12 / `Neck`=20 / `Head`=21 / `Clavicle`=15·25 / `UpperArm`=16·26 /
  `Forearm`=17·27 / `Hand`=18·28）。⇒ 那句「30 根骨，`0`/`1` 只是**看起来**像 root/pelvis」这次是对的，
  但**是按名验出来的**，仍然不是可以下次直接照抄的常数。
- 闭窗 `hold=8` **9/9**、每趟 `swfree=` 739/218/594/200（四趟都 >0）、`residue=0 dropped=0 late=16`、
  `minDot=1.0000`、straddle `takeovers=4 ＝ restored 4`、零 AV、`grace=0`、`noclip=0`。
- 顺带两条形状：`rst=9 ＝ swing=9`，且 **9 个窗口全靠 clip 自己结束**（`prog` 0.900–0.905、
  窗口 1687–1750 ms）⇒ §17.12 那个「2/11 撞 3000 ms 上限」这趟没有复现，`kRideSwingLenMs` 不必动。
- ⏳ **剩下的唯一缺口是形状，不是机制**：9/9 个窗口播的都是 `bigchopv2`，肉眼「**双手持刀然后反转刀身
  把刀朝下然后向下刺去**」⇒ 见 §17.15。

### 17.15 ✅ **两次 `chooseAttack`：远的那次管节律、近的那次给动作** —— 机制成立，而它的观感失败**判决了整个「播引擎记录」家族**（2026-09-03，标的 `316928 B` / md5 `64D751F6023F1921055361A18D2EE0F4`，**第二十二趟实测**）

**成因由两个实测事实钉住，都不是推的：**
- `bigchopv2` 的 `init=25.00`（`0x38`，§17.2）是骑手自己 `reach=10.50` 的两倍多，而 §17.12 那一趟
  **同一条 clip、还没有 mask** 时把骑手整个搬到坐骑前面 ⇒ 它是一条**「先冲进去再劈」**的技法，
  **位移就是它一半的动画**。§17.14 把 root/pelvis 遮掉之后，到屏幕上的只剩落地那半段 —— 于是看起来
  就是「刀朝下、向下刺」。
- **坐骑自己的身体把敌人垫开** ⇒ 骑手→敌人的距离**结构性地**落在 12–25（第二十一趟 `dmin=` 12.56 /
  13.89 / 21.40 / 15.47），**永远不在 reach 之内**。拿这个距离去问 `chooseAttack`，引擎的回答本来就该是
  「先把距离拉近」⇒ `bigchopv2` 9/9 **不是引擎选错，是我们问错了距离**。

**修法 ＝ 问两次，一次几何**（`RideSwingChooseImpl`，仍然**零引擎写**，仍在诊断门之外）：
- **GATE** `chooseAttack(d, reach, NULL, false)` —— 它的 `init=`/`minS=` 继续喂开窗的 `lim=` 阶梯，
  所以**节律与第二十一趟一模一样**（9 个窗口 / 3 场打架），这一轮不许顺手改节奏。
- **CLIP** `chooseAttack(min(d, reach), reach, NULL, false)` —— 「已经在一刀之内的人挥哪一下」，即
  **没有步法可丢**的那一条；`min()` 而不是写死 `reach`，所以真的贴身时问的还是真距离。
  第二问返回 NULL 时退回第一问的记录（宁可播老的，也不丢一刀）。
- ⚠️ **两个数都是引擎给的**，没有编任何距离常数 —— 这是这条路线从计划期就背着的禁令。
- ⚠️ **`lim=` 必须继续取自 GATE**：拿真实 d 去比 CLIP 记录的意见是范畴错误（`downward combo` 的
  `init=10.00` 对上结构性的 d=12–25 ⇒ **每个窗口都被拒，`swing=0`，而屏幕上什么都不说**）。
  这是本轮唯一的静默失败模式，`ridelog.py` 的 `-- T24 --` 专门有一条 CHECK 复算那道阶梯。
- 配套（**必要**，不是第二个实验）：`RideSwingDrive` 的相位拟合改成**只压缩、绝不拉伸**
  `fit = min(kRideSwingTechMs, len*1000)`。`chop left-3` 只有 1.067 s，塞进 1700 ms ＝ **0.63 倍慢动作**，
  而这一轮的目的正是开始选这些更短的原地记录；对 `bigchopv2`（2.833 s）是 no-op ⇒ 第二十一趟的数字
  不受影响。短 clip 提前跑完后靠 `setLoop(false)` 停在末帧 ＝ 收势，不是卡住。
- 自证字段：开窗行 `gate='<记录>'` ／ `dq=`（问 CLIP 用的距离）／闭窗行 `fit=`。
  🔑 **`gate=` ＝ `tech=` 全数就是一个干净的负结果**（引擎对两个距离给同一个答案）⇒ 那时只剩
  「自己指定 clip 名」这一杠杆，而名字**只能**取自 §17.10 的实测清单（⚠️ 猜 clip 名在这个项目里从来没对过）。

**判据**：`tools\ridelog.py` 的 `-- T24 --`；验收与五支退路写在 `TEST_REQUIRED.md` 的 T24 条。

**实测结果（2026-09-03 第二十二趟，诊断全程 OFF，三趟骑乘／两场打架，日志 `RE_Kenshi_log_trip22_64D751F6_diagoff.txt`，27492 B）—— 机制这一半全过：**
- **第二问真的换了记录**：`gate=` ≠ `tech=` **6/7**（`bigchopv2` → `downward combo` ×3、→ `chop left-3` ×3；
  第 7 个窗口 `d=7.78` 本来就在 `reach=10.50` 之内 ⇒ 两问同答，那是**正确行为**不是失败）。
  ⇒ 「问错了距离」这个诊断成立：同一个骑手、同一件武器，只把提问距离从 20–22 改成 `min(d,reach)`，
  引擎就从「先冲进去再劈」换成原地技法。
- `dq=min(d,reach)` 7/7、`lim=` 仍取自 GATE 的阶梯 7/7（节律没被改：`swing=7` / 三趟）、
  `fit=min(1700,len*1000)` 7/7（**4/7 按自己的速率播** ⇒ 无拉伸子句真的在用）。
- 回归：零 AV、`grace=0`、`rst=7 ＝ swing=7`、`en=1` 7/7、弧 100% 7/7、`hold=8` 7/7、
  straddle `takeovers=3 ＝ restored 3`、`residue=0 dropped=0 late=18`、`minDot=1.0000`、`post=1` 6/6、`real=554`。
- 🆕 顺路量到 **`downward combo` 的 Ogre 长度 ＝ 1.733 s**（§17.10 的长度清单里原来只有 `bigchopv2` 2.833 s
  与 `chop left-3` 1.067 s）。
- ⛔ **肉眼判决，而且它不只否决这一轮**：「不是劈砍，**平地上的动作在马上用有些放不开，刀砍不出去，
  只能在自己肚子那块拉，动作都挤成一团了**」⇒ 记录换得动、也到得了骨架，但**地面记录是围着一个站立的
  骨盆编的**：肩膀本来该带着身体走，坐着的骨盆把整条弧压在胸腹前。这与「挑哪一条记录」无关 ——
  T20（`mid blow` 替身）／T22（引擎自己挑的）／T24（原地那一条）三种取法都撞同一面墙。
  ⇒ 用户裁定：「**我们要的其实不一定非要和原版一样，只要像骑砍那样挥砍的动作就行**」
  ⇒ 保真度不再是要求 ⇒ **§17.16 自己写**。

### 17.16 ⚠️ **自己写这一刀（第一版：写关节角）** —— 机器全过、参数化错（2026-09-03 **第二十三趟实测**，标的 `321024 B` / md5 `78A3F132728B6C1E9380BF1357A534E7`）

> **实测结论先行（2026-09-03，诊断全程 OFF，三趟骑乘）**：
> **机器这一半全过** —— free 表 2/2 `has=1`、`arm=214/812/794`、51 个样本里最差的 `kept=1.0000`
> （⇒ 我们的写**一帧都没被 clip 盖掉**）、`armback` 9 次全 `man=0x00`＋`minDot=1.0000`
> （⇒ 交还干净、下马后手臂不僵）、三轴跨度 `7.55/9.12/7.64`（手臂只有 6.09 长 ⇒ 手真的在动）、
> 零 AV、straddle `takeovers=3 ＝ restored=3`、`residue=0 dropped=0 late=16`、`noclip=0`、`grace=0`、
> `guardoff=1806`、`rst=9 ＝ swing=9`、`sp=2.50` 8/8、`ogre=found` 9/9、`hdveto=2583` 帧。
> **而肉眼判决：「不是挥砍，是往下戳」，轨迹描出一个 `\`。**
> ⇒ ⛔ **`abd/flx/elb` 这套参数化结构上错了**（成因与修法在 §17.17），
> 但它顺路坐实两件事：① 手写骨这台机器在**手臂**上和在大腿上一样可靠（`kept=1.0000` 51/51）；
> ② `tools\armarc.py --log` 能**离线**定位观感缺口，零游戏时间。
> ⚠️ 下面这一节的「形状」描述保留原样（它是第二十三趟那份二进制的准确记录），
> **唯独「四行数字是量出来的」这句要连着 §17.17 读** —— 量的是**手在骨架空间的位置**，
> 而写下去的是**关节角**，两者只在「父骨不动」时等价，而父骨恰恰在动。

**前提（第二十二趟裁定）**：不再播引擎的任何记录。整个家族的墙已经量清（上一节末尾），
而用户放开了保真度要求 ⇒ 只需要「看得出是一刀」。

**机器是现成的，不是新地基** —— 跨骑腿型从 P2-1b 起就在做同一件事（§16 ＋ §21.2 的泄漏纪律）：
`setManuallyControlled(true)` ＋ **在每一条有权重的 clip 上把这根骨的 blend mask 钉 0** ＋
每帧在**渲染前**那个 pass 里写 ＋ 交还时 `setManuallyControlled(false)` ＋ `reset()` ＋ 读回自证。
⚠️ 两件事缺一不可：手控的骨**能**活过 `Skeleton::reset()`，但**照旧收** `NodeAnimationTrack::applyToNode`
⇒ **只有 mask 能保护它**。

**这一轮的形状**：
- **只两根骨**：`Bip01 R UpperArm`（abduction ＋ flexion）与 `Bip01 R Forearm`（肘）。
  ⚠️⚠️ **free 表（host 让位的集合）与手写表必须完全相同** —— 让位而没人写 ＝ 那根骨渲染成 BIND（§17.9 的病），
  写了而没让位 ＝ 被 clip 盖掉。所以 T23/T24 那 11 根**缩回 2 根**，其余全部还给 host；
  `Bip01 Spine` 退出 free 表，因此腿表里的 `ours = spineMask || swingFree` 也**改回 `spineMask` 单条件**
  （P4-1M 的躯干扭转重新是那两根骨唯一的写手）。这同时是「动作挤成一团」的解药：躯干、颈、头、左臂全归 host。
- **手腕留给 host**（`R Hand` 两张表都不进）⇒ 刀的握持与相对朝向不动（T24 唯一没坏的部分）；手指同理。
- **轴继承、符号实测** —— §16 在**大腿**上反解过这具骨架的四肢约定（local X 沿骨、local Z ＝ abduction、
  local Y ＝ flexion，右腿 +Z 向外、+Y 向前）。手臂是同一具骨架、同一套 Biped 命名 ⇒ **轴**沿用；
  ⚠️ 但**符号不能沿用**：`tools\armarc.py`（这一轮新增，全离线、零游戏时间）读 `male_skeleton.skeleton`
  的 bind 位置／bind 旋转／父子关系，按引擎自己的合成式（`derivedRot = parentDerivedRot * localRot`，
  `localRot = initialRot * qAbd * qFlx`）算出手在骨架空间的轨迹，量出的结论是
  🔑 **右臂两轴都反**：**负 abd ＝ 向上向外、负 flx ＝ 向前**（大腿是右侧 +Z 向外、+Y 向前）。
  ⛔ 第一版弧线照大腿的符号写，`armarc.py` 一跑就看出它**起手往前、"劈"往后** ＝ 整条挥砍是倒着放的
  （那一版 `223DC2D4…` 从没进过游戏；同字节数，**认 md5**）。
  🔑 **顺路量到一个格式事实**：`.skeleton` 的 BONE 块里四元数**按 x,y,z,w 存**，不是 w,x,y,z。
  这条是**实测出来的、不是从上游 Ogre 源码抄的**（§21.5 禁止那样做）：按 w,x,y,z 读，bind 姿势下
  肩→手只有 0.39，而那两根臂骨长 2.849 ＋ 3.244 ⇒ 不可能；按 x,y,z,w 读得到 **6.09 ＝ 直臂之和**，
  这条自证 `armarc.py` 每次运行都印出来。
- **弧线 `kRideSwingArc` 四个键**（线性插值，`kRideSwingArcMs = 1700` ＝ 实测窗口长度 1687–1765），
  ⚠️ **四行数字是量出来的，不是编的**（`armarc.py` 的输出，`out` ＝ 离身、`fore` ＝ 向前、`down` ＝ 低于肩）：
  `t=0.00` 预备 `out 2.15 / fore 1.81 / down 4.61` → `t=0.26` 起手 `5.28 / −1.58 / 0.23`（举到肩高、
  向外、后引）→ `t=0.66` 落刀 `−0.76 / 3.90 / 4.62`（向下向前、掠过坐骑肩前）→ `t=1.00` 回预备。
  三轴跨度 6.13 / 5.48 / 5.01，而整条手臂只有 6.09 长 ⇒ **每个轴上都走了将近一整条手臂**。
  ⚠️ 改这四行必须**同时改 `tools\armarc.py` 里的那一份**，构建过程抓不到两边漂移。
- **两处技法驱动都摘掉** ⇒ `drv=0` / `hold=0` / `fit=0` 从这一版起是**预期形状**（`ridelog.py` 的 T22/T23/T24
  三节都已改成认这条，靠 ride 行有没有 `arm=` 分流）。`RideSwingDrive` 保留但不再被调用，`RideSwingUndrive`
  仍在闭窗与 `Dismount()` 各跑一次。⚠️ **不要把它重新挂上**：技法那张 mask 只让开八根骨，会和手写的手臂打架。
- **自证字段**：`SWING arm`（`t=`/`abd=`/`flx=`/`elb=`/`kept=`/`out=`/`fore=`/`down=`，每 12 个手写帧一条、
  每趟 18 条预算）／`SWING armback`（`man=`/`minDot=`/`seen=`）／ride 行 `arm=`／close 行 `armt=`。
  🔑 `kept=` 是核心自证（我们的写有没有被 clip 盖掉），`out/fore/down` 是方向自证。
  ⚠️ **`kept=` 与 `LEGPOSE` 的 `kept=` 撞名**：`ridelog.py` 那条按形状分流的 `"kept=" in line` 分支
  已加排除项（否则第一帧的 `kept=-1.0000` 会被记成跨骑 mask 失败，同时 T25 一条样本都收不到）。
- ⚠️ **回归风险 ＝ 手臂僵在抬起的姿势**（手控骨的老病，只是换了骨头）：`LegPoseRestoreImpl` 开头
  无条件先 `RideSwingArmRelease()`，那是「一趟在窗口里结束」（下马／被打倒／读档）唯一的出口。

**判据**：`tools\ridelog.py` 的 `-- T25 --`；验收与五支退路写在 `TEST_REQUIRED.md` 的 T25 条。

### 17.17 ✅ **自己写这一刀（第二版：写骨架空间的方向）** —— 参数化这一半对了、观感第一次向前，而 `dot` 判据软失败（2026-09-03，标的 `323072 B` / md5 `A243F1729B366307EF5E229650EDE0F7`，**第二十四趟实测**）

**成因（量出来的，不是推的 —— 全部来自第二十三趟那份日志 ＋ `tools\armarc.py --log`，零游戏时间）**：

`abd/flx/elb` 是**相对 bind 的关节角**，它只把这根骨钉在**父骨**上；而父骨（`Bip01 R Clavicle` → 脊椎）
**归 host**，正被钉住的 `mid blow` 以 `sp=2.50` 扭着。⇒ 屏幕上看到的是
**我们的弧 × 宿主的躯干扫动**，不是我们的弧。三条数字各自独立地指向同一件事：

- 🔑 **同一个窗口里、同一组关节角（−25/10/−55）写了两次，手落在相距 72.3° 的两个地方**
  ⇒ 关节角这个量**不决定手的位置**。
- **`|量到的| / |按关节角预测的|` 全程没离开 1.04** ⇒ **手臂的几何是对的**（骨长、轴、符号都没错），
  错的是**参照系**。
- **窗口之内肩关节自己走了 3.00 / 4.83 / 5.12**（三个窗口）—— 我们弧的"起点"每帧都在跑。
  ⇒ 用户看到的 `\` ＝ 一条被宿主拖着走的、方向不断被重定义的弧 ＝「往下戳」。

**修法（结构性，不是调参）—— 写方向、不写角度**：
每根骨的 local +X **就是骨轴**（bind 子偏移量出来是纯 +X：`UpperArm→Forearm (2.849,0,0)`、
`Forearm→Hand (3.244,0,0)`，bind 肩→手 `6.09` ＝ 两者之和 ⇒ 「把 local +X 瞄向某个方向」
是这具骨架上**唯一**不需要额外约定的原语）。于是：

```
aim  = UNIT_X.getRotationTo(dir)            // dir ＝ 我们要的骨轴方向，骨架空间
want = conj(parentDerivedOrientation) * aim // 把父骨的姿态从等式里除掉
```

⇒ 手最终落在 **`lenFore * dirUpper + lenHand * dirForearm`**，逐分量、**与宿主无关**。
宿主怎么扭躯干，这条弧一动不动。

- ⚠️ **前臂不回读节点** —— 它的父姿态取**上臂自己刚算出的 `aim`**（`qAim[i-1]`），不取
  `Forearm->getParent()->_getDerivedOrientation()`。理由：这个 getter 在**我们链接的这份头文件里
  声明为 `const`**（§21.5 ＝ 只认头文件的可用性），它背后的缓存**可能落后一帧**；
  读到陈旧值 ＝ 把肘悄悄交回给 host。⇒ 全流程**只剩一次节点回读**（上臂的父 ＝ host 驱动的
  `R Clavicle`，无从避免），滞后约 2°，并且**由日志的 `dot=` 直接报出来**。
- ⚠️ **`Quaternion * Vector3` 这个算子在这个工程里从没链接过** ⇒ 旋转后的 X 轴是**手写展开**的
  （`RideQuatXAxis`），共轭也是手写的单位四元数 `(w,−x,−y,−z)`。别改成算子写法去赌链接。
- **坐标约定（沿用日志既有的三个名字）**：骨架空间 +X ＝ 骑手左、+Y ＝ 上、+Z ＝ 前；
  日志印的是 `out = −relX`、`fore = +relZ`、`down = −relY` ⇒ 一个写成 `(out, fore, down)` 的方向
  映射成 `(−out, −down, fore)`。这条映射写在 `RideSwingDirToSkel` 一个地方。

**弧线换成 `kRideSwingArc` 五个键**（每键两个方向：上臂骨轴 ＋ 前臂骨轴，线性插值后重新归一化），
`kRideSwingArcMs` **1700 → 1400**：第二十三趟量到窗口实际长 **1437–1781 ms**，1437 那一窗把 1700 的弧
**切在 `t=0.85`**（日志 `armt=0.85`，闭窗判据的软失败）⇒ 1400 装得进最短的窗，装满之后**保持最后一个键**。
手在骨架空间的落点（`out / fore / down`，由 `armarc.py` 的 `ARC2` 离线算出）：

| 键 | t | 手 out | 手 fore | 手 down |
|---|---|---|---|---|
| 预备 | 0.00 | 1.99 | 3.02 | 4.27 |
| 起手（举到外上、后引） | 0.24 | 3.25 | −1.67 | −4.47 |
| 落刀中段 | 0.50 | 2.74 | 4.55 | 2.73 |
| 劈透 | 0.72 | −1.64 | 3.58 | 4.19 |
| 收回预备 | 1.00 | 1.99 | 3.02 | 4.27 |

三轴跨度 **out 6.20 ／ fore 6.09 ／ down 8.93**（手臂全长 6.09）⇒ 幅度故意给大。
⚠️ **改这五行必须同时改 `tools\armarc.py` 里的 `ARC2`**，构建过程抓不到两边漂移（与 §17.16 同一条）。

**自证字段（`SWING arm` 行新增四个）**：
- 🔑 **`dot=`** ＝ 「按两个方向 ＋ 运行时骨长预测的肩→手」与「量到的肩→手」的夹角余弦。
  **`dot ≥ 0.99` 是整节 T26 的预登记否证条件** —— 它不到 0.99，说明方向没有到骨架，
  这条路线的结构假设就是错的（判读顺序 a/b/c/d 写在 `ridelog.py` 的 `-- T26 --` 里）。
- `want=(out,fore,down)` ＝ 我们**要**的手位（`lenFore*dirUpper + lenHand*dirHand`，不是量到的），
  与既有的 `out/fore/down`（量到的）成对，一眼可比。
- `len=` ＝ 两根骨长**从活骨架读**（预期 `2.85 / 3.24`）。它是 0 ⇒ 骨没解析到，`want=` 全部作废。
- `bx=` ＝ `Bip01 R Hand` 的 derived X 轴，**故意不设判据**：手腕两张表都不进 ⇒ 刀身朝向归 host。
  它只是"刀指哪"的数据。⚠️ 若肉眼报告「手在挥但刀身方向不对」，旋钮是把 `Bip01 R Hand`
  放进**两张表**（free ＋ 手写）或者两张都不放 —— **绝不能只放一张**（§17.16 那条不变量）。

**判据**：`tools\ridelog.py` 的 `-- T26 --`（`dot` 打头）＋ 共用的 `-- T25/T26 --` 一节；
验收、六支「先看哪里」写在 `TEST_REQUIRED.md` 的 T26 条。
⚠️ **T25 那一节的每一条判据第二十三趟都过了，而肉眼不过** ⇒ 过 T25 不等于过 T26。

#### 17.17.1 第二十四趟实测结果（2026-09-03，诊断全程 OFF，两趟骑乘／六个窗口）

**肉眼（用户原话）**：「**有点劈砍的意思了**，我猜测动作奇怪的原因是我们想要的骑砍动作姿势应该是
单手劈砍，但是角色的**右手总是想找左手**因为原版就是双手劈砍的，所以把动作带崩了，如果你需要，
我可以把角色一条胳膊卸掉让你看看他是怎么单手劈砍的。其他和以前一样。」
⇒ **参数化这一半成立**（§17.16 的「往下戳」没有再出现，这是这条路线第一次向前），
⇒ 剩下的缺口**不在右臂**，在**还被 host 驱动的那三处**（成因与修法 ＝ §17.18）。

**机器半（`ridelog.py`，一条一条）**：

| 判据 | 读数 | 判 |
|---|---|---|
| free 表 ＝ 手写表，两根骨都按名解析 | `R UpperArm=26 R Forearm=27` | ✅ 2/2 |
| 每个 `swing>0` 的趟都写了手臂 | `arm=635` / `arm=709` | ✅ |
| 闭窗弧走到最后一个键 | `armt=1.00` **6/6** | ✅ |
| 🔑 我们的写没被 clip 盖掉 | **`kept` 最差 0.9998 / 34 样本** | ✅ |
| 手真的走了 | 量到跨度 `out 6.31 / fore 5.94 / down 9.13` | ✅ |
| 交还干净 | `armback` 6 次、`man!=0: 0`、`minDot=1.0000` | ✅ |
| `len=` ＝ 活骨架读的骨长 | `2.85 / 3.24`（离线 2.849 / 3.244） | ✅ |
| `want=` 跨度 | `6.40 / 6.07 / 8.81`（离线表 6.20 / 6.09 / 8.93） | ✅ |
| 🔑 **`dot ≥ 0.99`（整节的预登记否证条件）** | **稳定帧最差 0.9860 ＝ 9.6°、均值 0.9971 / 34 样本** | ⚠️ **软失败** |
| 回归：straddle | `takeovers=2 ＝ restored 2`、`minDot=1.0000`、`residue=0 dropped=0 late=8` | ✅ |
| 回归：v1.6 四条 | `P43RD drawn=3`／`1`、`fail=0 nowpn=0`、`P43SUP real=283`／`123` | ✅ |
| 回归：零 AV ／ `grace=0` | 0 ／ 0 | ✅ |

**`dot` 那一条软失败的定案（离线算出来的，零游戏时间）**：`ridelog.py` 现在按 `kept<0` 把
**窗口的第一个写入帧**单独报（那一帧读到的缓存还是 host 留下的姿势 ⇒ 它量的是读、不是瞄），
第二十四趟是 **2 个第一帧样本（最差 0.8268）＋ 34 个稳定帧样本（最差 0.9860）**。
⚠️ **这个分离并没有救回判据** —— 0.99 在稳定帧上也没过。同一节新加的判读器把每个失败样本的
**亏损**与「那一点弧上一帧滞后最多值多少」并排印出来：

- `194.787 t=0.61 dot=0.9860 ＝ 9.6°`，而一帧滞后 **≤1.6°**（相邻行 19.5° / 12 帧）⇒ **不是我们这一侧的滞后**；
- `228.629 t=0.11 dot=0.9897 ＝ 8.2°`，一帧滞后 ≤3.7° ⇒ 这一条是读滞后。

⇒ 剩下的唯一嫌疑是**父骨那次回读**（`R Clavicle`，同样陈旧一帧，而它归 host —— 那一趟 host ＝
以 `sp=2.50` 驱动的 `mid blow`）。§17.17 原文估的「滞后约 2°」**偏乐观**，实测最差 9.6°。
⇒ **这给了 §17.18 一条可预登记的方向**：host 换成 1.0 速率的 LOOP 之后，`dot` **应该变好**
（判据与基线写在 `ridelog.py` 的 `-- T27 --` 第 6 节：稳定帧最差不得差于 0.9860 / 9.6°）。

### 17.18 ✅ **窗口不再换宿主：`guard 1h` 全程钉着** —— 「右手总是想找左手」的成因（2026-09-03，标的 `323072 B` / md5 `65DED60CC9FBDAEE0835CBFE6A3D91FF`，**第二十五趟实测：成因判对了、观感又向前一步**，实测结果在 17.18.1）

**用户的判断对，但机制要修正一处**：**右手的位置早就是我们的了**（第二十四趟 `dot=` 均值 0.9920→
稳定帧 0.9971、`kept=0.9998` 34/34 ⇒ 没有任何 clip 在拉右手）。窗口里 `mid blow` 还在驱动的是
**左臂、右手腕、脊椎**这三处 —— 它是六条 `blow` 记录之一，全部 `whole,action,norm,reloc,restrict`
＝ 击倒类重击、**双手动作**（`doc.md:245`）⇒ 左手一直伸过来找一个已经被我们的弧带走的刀柄，
躯干也按双手劈砍的方式扭。**「右手总是想找左手」看到的是左手在找右手。**

**修法 ＝ 减法**：窗口期内**不再把 `mid blow` 换上身**，`guard 1h` 从架势第一帧一直钉到最后。
`guard 1h` ＝ UPPER / LOOP / `weaponTypeFlags` bit `0x04`（**单手**）、**没有 `whole`/`reloc`**
（`doc.md:248`）⇒ 它是唯一能无限期托住战斗上半身、又把腿留给 `LegPosePass` 的记录。
**骨架一帧都不会没有宿主** ⇒ 这同时是 §17.9（第十七趟「在牛背上站直」）最强的那个答案。

**它退役的四件东西**（都是这一轮的设计决定，日志里再出现就是代码被改回去了，`-- T27 --` 第 4 节判）：

| 退役 | 为什么 |
|---|---|
| `rst=`（`RideSwingRestart`） | 没有一次性 clip 可重启了 |
| `kRideSwingSpeed = 2.5f` | LOOP 的播放速率对「一刀完整不完整」没有意义 |
| `kRideSwingDoneProg = 0.90f` ／ `prog=` 作为闭窗判据 | LOOP 的进度是循环的；闭窗改由**我们自己的钟** |
| §U 的 `ClipPin` 绕门法（窗口内不主张 guard） | T27 之后**全场只有一条 clip 要 1.0** ⇒ `target + others <= 1.02f`（`RidingPlugin.cpp:7345`）结构上不再咬。⛔ 那道门本身**一个字节都不动**（`TASK.md:326-334`） |

**窗口时序不变量**（改一个必须重读这一行）：
`kRideSwingArcMs 1400 < kRideSwingWinMs 1650 < kRideSwingLenMs 3000（硬上限，纯保险）< kRideSwingMinGapMs 3200`。
⇒ 弧走完之后还有 ~250 ms 的**落定姿势**（弧保持最后一个键），窗口按 `kRideSwingWinMs` 闭。
⚠️ **弧走完与否看 `armt=`，不看 `ms=`** —— 两个数字本来就不相等。

**`guardoff=` 改名 `hostkeep=`**：同一个计数器（`gRideSwingGuardOff`）、相反的含义
（guard 被**让开**的帧 → 窗口把 guard **留作宿主**的帧）。键名必须换，否则有人会在一份 guard
从不让位的构建上读出「guard 让开了 N 帧」。它同时是 `ridelog.py` 的**分流键**（哪一族判据适用）。

**三个每帧计数器**（`-- T27 --` 第 5 节要求它们互相吻合，第二十四趟 636/635/635 ＝ 开窗那一帧的偏移）：
`hostkeep` 在 `HaltAndForceSitPass`（render 侧，`RidingPlugin.cpp:8042`，**只在这里数**）、
`arm` 在 `RideSwingArmPose`（`:5826`）、`swfree` 在 `LegPosePass`（`:6270`，`msk > 0` 才数）。

⚠️ **`P41K resolve` 那一行不受门控**、每次加载 DLL 在架势第一帧印一次（第二十四趟诊断全程 OFF
的日志里确认在）。T27 之后它的 **guard 那一半是承重的**（`found` ＝ 宿主拿到了），
**blow 那一半只是存证** —— 已经没有任何地方请求 `mid blow` 了，而它仍然 resolve，
这就是 §17.18 那条退路还走得通的凭据（退路 ＝ 把 `want` 的 switch 加回去，一处）。

**判据**：`tools\ridelog.py` 的 `-- T27 --`（按 `hostkeep=` 分流）＋ 共用的 `-- T25/T26 --`；
第 6 节是**这一轮唯一能预登记的数字**：`dot` 稳定帧最差**不得差于** 0.9860 / 9.6°（第二十四趟基线）；
若它**越过 0.99**，T26 的头号判据第一次通过，而第二十四趟那次 CHECK 的成因**回溯确定**为
host 在搅动父骨（⇒ 判读顺序里的嫌疑 (c) 退役）。四支「先看哪里」写在 `TEST_REQUIRED.md` 的 T27 条。

#### 17.18.1 第二十五趟实测结果（2026-09-03，诊断全程 OFF，三趟骑乘／八个窗口）

**肉眼（用户原话）**：「**本来是正手拿刀，动作是正手变反手然后从右侧劈出。只需要正手拿刀劈出就好。
劈砍动作我试了一下，大臂旋转小臂不动，带动小臂，带动刀。其他和以前一样。**」

⇒ 三件事，一件比一件重要：
1. **「其他和以前一样」** ＝ v1.6 四条／跨骑腿型／下马后右臂／不站直／不转向坐骑屁股全部不退；
2. **「从右侧劈出」不再是抱怨** —— 方向与出刀的那一侧**被接受了**，`guard 1h` 换宿主这一步（＝ T27 本身）
   把「左臂过来找刀柄」拿掉了，这是这条路线第二次连续向前；
3. **剩下的两条都指向同一个东西**：`正手变反手` ＝ 握持在弧上飘（成因见下），
   `大臂旋转小臂不动` ＝ 用户给出了**机制**而不是口味 ⇒ §17.19。

**机器半（`ridelog.py`）**：

| 判据 | 读数 | 判 |
|---|---|---|
| `guard 1h` 全程当宿主、权重不掉 | 开窗 `1.000` → 闭窗最差 `pw=1.000`／7、`psw!=1: 0`、`oen!=1: 0` | ✅ |
| 开窗那一刻 guard 已经在播 | 8/8 live | ✅ |
| 窗口按我们自己的钟闭 | `ms=1656×6 / 1657×2`（≈`kRideSwingWinMs 1650`，离 3000 硬顶很远） | ✅ |
| 闭窗弧走到最后一个键 | `armt=1.00` | ✅ |
| 四个退役位全静 | `rst/drv/hold/fit = 0` | ✅ |
| 🔑 我们的写没被 clip 盖掉 | `kept` 最差 **0.9998** | ✅ |
| `dot`（预登记：不得差于 0.9860） | 稳定帧最差 **0.9877 ＝ 9.0°**、均值 **0.9980** / 51 样本 | ✅ **确实变好了** |
| 两次 `chooseAttack` 分工 | `gate=` ≠ `tech=` 6/8（`bigchopv2`→`chop left-3`／`downward combo`…） | ✅ |
| 回归：straddle ／ 零 AV | 干净 ／ 0 | ✅ |
| 闭窗时 guard 的条目还活着 | **7/8** —— `551.968` 一次 `pinst=none` | ⚠️ **硬失败一处** |
| 三个每帧计数器互相吻合 | `559.442` 那趟 `hostkeep=504` vs `arm=536 swfree=536`（另两趟各自相等） | ⚠️ **同一个 bug** |

🔑 **`dot` 那条预登记的方向被证实了**：host 从 `sp=2.50` 的 `mid blow` 换成 1.0 速率的 LOOP 之后，
稳定帧最差 0.9860 → **0.9877**、均值 0.9971 → **0.9980**（样本还多了一半：34 → 51）。
⇒ §17.17.1 那条「嫌疑落在被 host 搅动的父骨」**方向对**，但**量级不够** —— 换掉搅动源只回收了 0.6°，
所以 9° 这个残差**不是** host 速率造成的，`dot` 也就**不再是**下一轮的杠杆（§17.19 因此不动弧的判据、只换模型）。

**那一处硬失败的定案（同一个 bug，两个症状）**：窗口的开窗判据里有架势、**闭窗判据里没有**
⇒ 一场打架在挥砍中途结束（架势落下）时，`HaltAndForceSitPass` 不再把 guard 留作宿主
（`hostkeep` 停在 504），而 `RideSwingArmPose` 与 `LegPosePass` 还按「窗口开着」跑了 32 帧
（`arm/swfree=536`）⇒ 那 32 帧右臂两根骨**从 host 手里被摘掉、而宿主已经不在** ⇒ 正是 §17.9
第十七趟那个「骨架无宿主」的病，只是范围缩到两根骨。闭窗那一行的 `pinst=none` 就是它的签名。
**修法 ＝ 一处**（已落地，`RidingPlugin.cpp:3736-3739`）：把 `stance` 加进**开着**这个谓词本身，
不只是开窗那一次决定。⚠️ **不会因此抖动** —— 那个 pass 读架势用的是 `advance=false`，是黏的。

### 17.19 ✅ **一次刚体旋转：肘关节冻住、握持被带着走** —— 用户给的机制，两个判据都与模型无关（2026-09-04，标的 `323072 B` / md5 `BE9626866EFAA01A45816198E74B3AB0`，**第二十六趟实测：两个见证都过、观感成立 ⇒「挥砍」这条线到此收口**，实测结果在 17.19.1，之后的形状打磨在 **17.19.2**）

用户判词的后半句「**大臂旋转小臂不动，带动小臂，带动刀**」是**机制**，不是口味。而两条抱怨在
退役的那张「每根骨一个方向」表里各有一个**结构性**成因 —— 都不是调参能碰到的：

- **握持为什么会翻**：那张表给每根骨一个**方向**，写入是 `UNIT_X.getRotationTo(dir)` ＝ 落到 `dir`
  上的**最小**旋转 ⇒ 它只钉住骨头的轴，**绕这根轴的滚转**是弧碰巧走到哪就是哪。而真正握着刀的
  `Bip01 R Hand` 按设计在**两张表都不列**（握刀手型归 host）⇒ 它把这份滚转**全额继承**。
  第二十五趟量到的飘量：`bx=` 三轴跨度 1.13 / 1.59 / 1.70 ⇒ **`正手变反手` 来自一个表里没有任何键
  命名过的量**。
- **为什么是小臂在砍**：拿那张表自己的数字算，ready→through 上臂只转了 **27.7°**、小臂扫了 **~124°**
  ＝ 「大臂旋转小臂不动」的**反面**。两个**独立**受键的方向**结构上表达不了**「一根动、另一根被带着」——
  它们之间的关节角是**导出量**，没有任何键在看它。

⇒ **T28 改成写用户描述的那件事**：开窗时**捕获** host 正拿着的整条手臂，然后在骨架空间里绕**一根固定轴**
施加**一次刚体旋转**。小臂的**局部**朝向捕获之后逐字回写 ⇒ **关节角结构上不可能变**；握持是捕获来的
⇒ `正手` 保持 `正手`；上臂带小臂、小臂带刀 —— 因为**刚体旋转一个捕获的姿势本来就是这个效果**。
第二十五趟三个症状里的两个（翻手、小臂主刀）**由构造消掉，不是靠调**；`S(0)=identity`
⇒ 窗口开在**屏幕上已有的那个姿势**上，交接零跳变。

⚠️ **代价写在源码里**：刚体旋转画的是一个**锥面** ⇒ **手最后落在哪里不再由我们选**（伸手距离与肘角
就是 `guard 1h` 当时拿着的那个），只剩两个旋钮：**轴**（在哪个平面里扫）与**角度曲线**（多远、多快）。
这是有意的交换 —— 第二十三～二十五趟花了三趟自由参数化关节角，判词是「往下戳」「正手变反手」，
**两次都是这个模型已经交出去的自由度造成的失败**。

**轴也不是猜的**：取退役那张表**自己**的上臂在 cock 键（out 0.80 / fore −0.30 / down −0.52）与
through 键（0.05 / 0.55 / 0.83）之间扫出的平面的**法向** —— 即用户**已经接受**了方向的那个形状的切面。
叉乘、转到骨架空间、归一化 ⇒ 日志坐标系里 **out −0.05 / fore 0.83 / down −0.55**（前上方）。
**正**角把右手带向下、横过坐骑的脖子；**负**角把它抬出去、越过肩膀。
弧表五个键（`RidingPlugin.cpp:5729` 起，`RideSwingArcAt` ＝ 纯线性插值、**故意不加 easing**，
形状活在键的**时刻**里）：`(0.00, 0)` / `(0.26, −100)` cock / `(0.56, −60)` hang / `(0.78, +45)` through /
`(1.00, 0)` settle ⇒ cock→through **145°**、手的直线弦长 **10.07 单位**、耗时 **308 ms**。
⚠️ **改这五行必须同时改 `tools\armarc.py` 的 `ARC2`**（与 §17.16 同一条，构建抓不到两边漂移）。

**两个判据都与模型无关、而且防滞后**（这是这一轮方法上的进展 —— 前四轮的判据都要么要一份轴常量的
副本、要么会被一帧读滞后污染）：

| 见证 | 量什么 | 为什么滞后污染不了它 | 第二十五趟基线（＝ 退役模型的读数） |
|---|---|---|---|
| 1 · 肘 | `r=` ＝ `|shoulder→hand|` | 绕肩的旋转**改不了长度**，而滞后的样本只是**转过的**样本 | 每窗跨度 **0.868 / 0.617 / 0.848 / 0.829 / 0.768**（臂长 5.42）⇒ 退役模型**确实在屈肘** |
| 2 · 握持 | `angle(bx, arm)` ＝ 握刀手自己的 `+X` 对 shoulder→hand | 两者被**同一个** `S(t)` 带走 ⇒ 刚体旋转下恒定 | 每窗跨度 **52.5 / 27.1 / 54.0 / 30.7 / 46.6°**（全场 1.7…55.7）⇒ `正手变反手` 被量化了 |

⇒ 判据（`ridelog.py` 的 `-- T28 --`，**测前登记**）：`r` 跨度 ≤ 0.05；`angle(bx,arm)` 跨度 ≤ 15°；
弧确实走完（`min(deg) ≤ −95` ＆ `max(deg) ≥ +20`）；`noref=0`（`RidingPlugin.cpp:3252`，想写而两根骨
没解析出来的帧 ⇒ 非零就有骨头被 free 了却没人写 ⇒ 渲染在 BIND）；首帧 `dot ≥ 0.99`；
稳定帧 `dot ≥ 0.9860`；零 `pinst=none`（＝ 上面那处修法的判词）。

⚠️ **`cone=` 是两个不同的数字，别互相对账**：`armarc.py` 印的是轴对**手向量**（用实测开窗姿势算 ＝ 77.0°），
DLL 的 `cone=` 是轴对**上臂自己的 `+X`**、每窗捕获一次（`RidingPlugin.cpp:5884-5893`）。
**是表亲，不是副本**。它存在的唯一理由是接住**唯一一种能通过其他所有判据的失败**：锥角接近 0 或 180 时
「扫过 145°」照样成立而**手几乎不动** ⇒ `-- T28 --` 第 3 节收 `25° ≤ cone ≤ 155°`，
读到界外时该动的旋钮是**轴**、不是弧表。

#### 17.19.1 第二十六趟实测结果（2026-09-04，诊断全程 OFF，四趟骑乘／十三个窗口，日志 `D:\KenshiModDev\RE_Kenshi_log_trip26_BE962686_diagoff.txt`，54410 B）

**肉眼（用户原话）**：「**现在动作已经可以看出来了，不像之前那么难描述。新的动作是侧面张开大臂带动刀，
简单美观。**」⇒ **模型成立**：「张开**大臂**带动刀」＝ 用户自己在第二十五趟给的那条机制（大臂旋转、
小臂被带着、刀被带着）**在屏幕上被读出来了**；「不像之前那么难描述」＝ 前五轮判词都是在描述**畸形**
（「往下戳」「正手变反手」「缩成一团」），这一趟第一次是在描述**动作**。⚠️ 用户没有再提任何回归项。

**机器半（`ridelog.py` 的 `-- T28 --` ＋ `-- T27 --` ＋ `-- T25/T26 --`，48 条 PASS、零 CHECK
除两条「诊断关着 ⇒ 未测量」）**：

| 判据（**全部测前登记**） | 门槛 | 第二十六趟读数 | 第二十五趟基线（退役模型） | 判 |
|---|---|---|---|---|
| 🔑 **见证 1 · 肘** `r=` 每窗跨度 | ≤ 0.05 | **0.000** ×7 窗（`r=5.422` 一动不动） | 0.868 / 0.617 / 0.848 / 0.829 / 0.768 | ✅ **肘关节真的冻住了** |
| 🔑 **见证 2 · 握持** `angle(bx,arm)` 每窗跨度 | ≤ 15° | **0.4…0.7°**（全场 1.4…2.1°） | 52.5 / 27.1 / 54.0 / 30.7 / 46.6° | ✅ **`正手` 保持 `正手`** |
| 弧真的走完 | `min ≤ −95` ＆ `max ≥ +20` | `deg= −99.0 … +42.5` | — | ✅ |
| 锥角在带内 | `25° ≤ cone ≤ 155°` | `88.3 … 127.1°`（每窗捕获） | — | ✅ 接近 90 ＝ 手动得最快 |
| `noref=`（free 了却没人写的帧） | 0 | **0** / 13 个闭窗行 | — | ✅ |
| 首帧 `dot`（交接跳变） | ≥ 0.99 | 最差 **0.9971**（另 4 帧 1.0000） | — | ✅ 窗口开在屏幕上已有的姿势上 |
| 稳定帧 `dot` | ≥ 0.9860 | 最差 **0.9918 ＝ 7.3°**、均值 **0.9984** / 98 样本 | 0.9877 ＝ 9.0°、0.9980 / 51 | ✅ **第一次越过 0.99** |
| **零 `pinst=none`** | 0 | **0** —— 闭窗 13/13 `pinst=live`、`pw=1.000` | 8 窗里 1 次 | ✅ `&& stance` 那处修法成立 |
| 三个每帧计数器吻合 | 相等 | `hostkeep ＝ arm ＝ swfree` **4/4 趟**（1104/290/590/214） | `504` vs `536` ⚠️ | ✅ **同一个 bug 的两个症状都没了** |
| 我们的写没被 clip 盖掉 | — | `kept` 最差 **0.9998** / 98 样本 | 0.9998 | ✅ |
| 闭窗弧走到最后一个键 ／ 窗口按自己的钟闭 | — | `armt=1.00` **13/13**、`ms=1656/1657` | 同 | ✅ |
| 四个退役位全静 | 0 | `rst/drv/hold/fit = 0` | 同 | ✅ |
| 交还干净 | — | `armback` 13 次、`man!=0: 0`、`minDot=1.0000` | 同 | ✅ |
| 手真的走了 | — | 跨度 `out 9.97 / fore 6.78 / down 7.98` | — | ✅ |
| 节律与技法 | — | `swing=13` / 4 趟、`tech=42` 命名、`skip=29` 出界、`noclip=0` | — | ✅ |
| 回归：straddle | `takeovers ＝ restored ＋ released` | `4 ＝ 4 ＋ 0`、`minDot=1.0000`、`residue=0 dropped=0 late=19`、`grace=0` | — | ✅ |
| 回归：v1.6 四条 | — | `P43RD post=1` **4/4** ＋ `sheath '' -> 'back'` 4/4 ＋ `aCW` 脱离 5 4/4、`P43SUP real=694` | — | ✅ |
| 回归：零 AV ／ 不突然转向 | — | 无 AV 行 ／ `hdveto=897/639/1981/1127` | — | ✅ |

🔑 **`dot` 那 9° 残差的归因这一趟才算清楚**（三趟的差分，每次只动一件事）：
`9.6°`（第二十四趟 ＝ 瞄方向表 ＋ `mid blow` 2.5×）→ `9.0°`（第二十五趟 ＝ 同表、host 换成 1.0 速率
LOOP ⇒ **host 只值 0.6°**）→ **`7.3°`（第二十六趟 ＝ 同 host、换模型 ⇒ 模型值 1.7°）**。
⇒ §17.17.1 追的那个「父骨回读陈旧一帧」**上限只剩 7.3°**，而它**不再是杠杆**：两个模型无关的见证
（0.000 / 0.7°）已经把形状判过了，`dot` 只回答「我们的写落在瞄的地方」。⛔ 以后**不要再拿弧表去追 `dot`**。

**离线对照件**（`python tools\armarc.py --log …`，与 `ridelog.py` 各走各的代码路径）：WITNESS 1
每窗跨度 `0.006…0.011`（它按日志里两位小数的分量**重算**长度 ⇒ 这是量化残差，不是分歧；DLL 自己印的
`r=` 是 `5.422` 恒定），WITNESS 3 的模型无关块印出的 `angle(bx,arm)` 与 `ridelog.py` **逐字一致**
（0.4…0.7°）＝ 两份实现互为对账通过。⚠️ armarc 的 WITNESS 2/3 **残差**最差 `11.6°/11.9°`（win3
`t=0.67`）**不是判据**：那是「把 `S(t)` 从实测里除掉之后还剩多少」，按脚本自己的话要**对着帧间隔**读 ——
弧最快那段 `hang→through` ＝ 105° / 308 ms ＝ **341 °/s** ⇒ 30 fps 下一帧 **11.4°**，与残差同量级
⇒ 读滞后，不是翻手（翻手那条是模型无关的 0.7°）。

⇒ **「挥砍」这条线到此收口**：机制（T20/T21）、约束（T23）、形状（T25→T28）三层都实测过了。
剩下的是**发布决定**（这一份要不要替掉 `release\` 里的 v1.6），以及用户若要继续打磨观感，只有两个
旋钮 —— **轴**（在哪个平面里扫）与**角度曲线**（`kRideSwingArc`），⚠️ 两者都在 `tools\armarc.py`
里有镜像（`AXIS` / `ARC2`），**改必须成对改**，构建抓不到两边漂移。

#### 17.19.2 ⬛ 在同一个圆上换窗口：「侧面张开」→「劈下来」（2026-09-04 落地，构建 `323072 B` / md5 `21E0F66D93068F18543A5F5500362BC1`，**从没实测、也不会再测 ⇒ 只作几何档案读，见本节末横幅**，阶段条目 `TASK.md` P4-3-4k）

第二十六趟的判词接受了动作、同时**命名了它的形状**：「侧面张开大臂带动刀」。这一节记的是那句话的
**几何**，以及为什么它能只用角度曲线修掉一半、另一半只能等轴。**全部结论都是离线得到的**
（`python tools\armarc.py` ＋ 模块级探针），没有新逆向、没有进游戏。

**① 「侧面」是圆的位置，不是振幅**。刚体旋转 ⇒ 手在一个**固定圆**上走，半径 `r·sin(cone) = 5.28`
（实测开窗姿势 `r=5.419`、`cone=77.0°`）。圆上 `out` 有一个极大值（**out 5.2**，约在 `−75°`）。
4j 的两个端点 `−100 / +45` **跨在这个极值两侧** ⇒ cock→through 竖直 6.00／横向 7.27（**vert/lat 0.83**）
＝ 横向比竖直还多。沿**同一个圆**上滑到 `−130 / +25`：竖直 **8.11**／横向 **3.74**（**vert/lat 2.17**），
且 `+25` 刚过圆的**最低可达点**（`down` 在 `+18…20°` 到极大 3.73）⇒ 落点带着速度通过最低处。
⚠️ **换轴反而更差**：量过三个候选轴，最好只到 3.04（候选 B ＝ 日志系 `−0.36/0.81/−0.47`）⇒
**这一趟不动轴**（一个变量换最大效果，且 `cone=` 判据的含义保持不变）。

**② 但横向成分删不干净，这是圆的性质**。`−130 → +25` 必须**穿过** `−75` 那个 out 极值 ⇒ 落刀中途
`out` 会鼓到 **5.21**，比两个端点都多 **+2.34**。⇒ **任何角度曲线都碰不到这段**。若第二十七趟仍然
判「侧面」，下一个旋钮就是**轴**，已量好的候选 ＝ **矢状轴**（日志系 ≈ `out 1 / fore 0 / down 0`），
它让 `out` 全程恒定在 1.41（落点误差 ≤ 0.01 单位）。⚠️ 轴同样有 `armarc.py` 镜像。

**③ 第二条缺陷是节奏，也是离线可见的**。4j 的 `−60`「hang」把 145° 里的 40° **提前漏掉**
⇒ 只有 105° 是看得见的那刀；而抬手（弦 8.09 / 364 ms ＝ **22.2 u/s**）几乎等于落刀
（8.38 / 308 ms ＝ **27.2 u/s**）⇒ 眼睛分不出哪一段是动作。新表：cock 130° / 560 ms（17.1 u/s）
→ **停 196 ms** → 155° / 336 ms（弦 **10.31** ＝ **30.7 u/s ＝ 1.80×**）→ **停 112 ms** → 归位 25°。
⚠️ 弦随角度**次线性**增长（接近 180° 时几乎不长）⇒ 角速率 +35% 只换来线速度 +13%，这是这条路的天花板。
停顿之所以是真的：`RideSwingArcAt` **纯线性、故意不加 easing** ⇒ 两个同值键就是一段 hold。

**④ 落刀单调下落**（离线 40 点细扫）：新表落刀段 `down` 只有 **1/40** 处回退（＝ 顶上的平段），
旧表 4/40，且旧表的抬手段自己先下落过一次（`down −2.75 → +0.23`）⇒ 下落被劈成两半。

**⑤ 不变的东西（写下来免得被读成回归）**：轴、骨表（`kRideSwingFreeBones` ＝ 手写的那两根，
`Bip01 R Hand` 两张表都不列）、四条时序不变式（`1400 < 1650 < 3000 < 3200`）、以及**落刀的时刻**
—— through 仍在窗口的 **1092 ms** 处（`t=0.78`），与 4j 逐字相同 ⇒ 与引擎自己那套攻击动画的相位
关系按构造不变。末键仍是 `0` ⇒ 弧走完的 hold 与交还照旧。

**⑥ `dot` 会变松，那是按构造的**。`dot` ＝ 一帧滞后的滞后项 ∝ 角速率，而这一趟**故意**把峰值速率
从 341 提到 **461 °/s**（×1.35）⇒ 缩放期望 `7.3 × 1.35 = 9.9°`，且这是**上界**（7.3 里有 host 的
一份、host 没动）。`ridelog.py` 的门槛因此从 9.6+1.5 改成 **11.0°（0.9816）**，并在两处注释里写明
⛔ **不许为了 `dot` 回调弧表** —— 那等于把这一趟要测的东西撤销掉；真修法是新鲜的父骨回读（§21.5）。

**⑦ 三处镜像**（编译器抓不到漂移）：`RidingPlugin.cpp:5729` 的表、`tools\armarc.py` 的 `ARC2`、
`tools\ridelog.py` 的 T28/T27 判据（`deg=` 门槛 `−120/+15`、`dot` 门槛 0.9816、以及失败出口的指向）。
🆕 **前两处的漂移现在有检查了**：`python tools\armarc.py --mirror` 把 `.cpp` 的 `kRideSwingAxis*` /
`kRideSwingArc` / `kRideSwingArcKeys` / `ArcMs` / `WinMs` **正则解析**出来逐个对，**armarc 的每一个
模式开头都自动跑一遍、不一致就拒绝出报告**（理由：一份悄悄描述着另一条弧的离线报告，比没有报告更坏 ——
从它得出的每个结论都会「对着错的东西正确」）。它顺带核 `kRideSwingArcKeys` 与表长一致 ＝ 那是**编译器
同样抓不到**的越界（`{...}` 初始化不检查那个独立的计数常量）。⚠️ 第三处（ridelog 的门槛）**仍然是手工**：
它是「测前登记」的判据、按定义不能从源码推导出来。
⚠️ armarc 顺手补了 `ARC_MS`/`WIN_MS` 与一段 **per-segment tempo ＋ vert/lat** 的离线打印 ⇒
「改完先看离线表」现在**看得见这一趟的设计目标本身**（vert/lat 就是「劈」与「张开」的分界）。

⚠️⚠️ **这一节从此只作几何档案读**：它描述的构建（`21E0F66D…`）**从没实测过**，而它唯一改的那张
`kRideSwingArc` 与整个 `kRideSwingAxis*` 已随 T28/T29 一起退役（源码里都没有了，见 §17.19.3）
⇒ 别拿它的 `deg=`／`cone=`／vert/lat 数字对现状账，圆的那套几何仍然成立、但屏幕上已经没有那个圆了。

#### 17.19.3 ✅ 播香草自己的曲线：两根骨、delta 形式（2026-09-04 落地，构建 `324096 B` / md5 `960E3EB9C056518518A971D9F18FCAB2`，**已实测通过 ＝ `TEST_REQUIRED.md` ✅ T30**，读数在 §17.19.4，阶段条目 `TASK.md` P4-3-4l）

**① 为什么刚体旋转到顶了 —— 是几何天花板，不是调参问题**。T28 交付了它该交付的（肘按构造冻住、
`正手` 不翻、第二十六趟肉眼「侧面张开大臂带动刀，简单美观」，两个见证远超门槛）。但一次刚体旋转
让手走**一个圆**、并把肘**留在发现它的地方** —— 见证 1 量的正是这件事（`r=` 每窗跨度 **0.000**／13 窗）。
而 Kenshi 自己每一条 `chop` 都把肘弯 **62…80°**（`python tools\skelanims.py --sweep chop`）⇒
眼睛读成「一刀」的形状**不在任何圆上**，任何弧表都到不了。轴与角度曲线是那个模型的全部两个旋钮，
两个都花掉了，里面**没有第三个**。

**② 模型：把香草的曲线当 delta 播**。Ogre 的每个关键帧旋转都是**相对骨骼绑定姿势**的
（`local = bind * key`，§19.7）＝ `Bone::setOrientation` 收的同一个空间 ⇒ `chop down` 自己的肩、肘
曲线可以**离线**从 `male_skeleton.skeleton` 读出来，由**我们的**写手在**我们的** mask 下重放：

```
local_upper(t) = capturedUpperLocal * Xu(t),   Xu(t) = conj(Ku(0)) * Ku(t)   <- kRideSwingBakeUp
local_fore (t) = capturedForeLocal  * Xf(t),   Xf(t) = conj(Kf(0)) * Kf(t)   <- kRideSwingBakeFo
```

⚠️ **这不是第二十二趟判死的那个家族**：没有 `AnimationState` 被驱动、没有 ANIMATION 记录被播、
没有向动画系统要过任何东西 —— **两张 float 表 ＋ 一次 lerp**（`RideSwingBakeAt`，`RidingPlugin.cpp:5750`／`:5782`，
各 27 行）。两张表都**从 identity 开始** ⇒ §17.19「窗口开在屏幕上已有的姿势上」逐字保住。
**绝对形式（`local = bind * K(t)`，香草自己的帧、用户在地面上已经接受的形状）被离线否掉**：
它开窗时离屏幕上的姿势 **33.0° / 19.0°** ＝ 手瞬移 **3.2 单位**，只有交叉淡入能盖住。

**③ 它顺带退役了父骨回读**。local 写入不需要 `conj(parentDerived)` ⇒ 第二十四→二十五→二十六趟
差分出来的那 **7.3°**（「一帧陈旧的 `R Clavicle`」）**没有路径进入姿势**了，只活在日志的 `dot=` 里
（那里还在读）。代价说清楚：切面现在**跟着躯干走**，不再钉在骨架空间 —— 对「劈」这是**正确的符号**
（香草自己就是为此写 local 的），而且是量过的：锚定块显示躯干自己在窗口之间动 **0…39°**。

**④ delta 形式的代价，说在前面**：香草的一刀被 **re-base** 到鞍上 `guard 1h` 正拿着的姿势上 ⇒
它在屏幕上的方向是**那次 re-base 的**方向，不是站姿 clip 的。第二十三趟「往下戳」＝ 曲线量对了、
屏幕上指错方向的先例 ⇒ 方向**在这次构建之前就离线预测过**：

```
python tools\armarc.py --bake "chop down" --ref-log <第二十六趟日志>
```

用每个窗口**第一帧实测的** `bx=`／`bz=` 建正交系锚定（同一行的 `out=`／`fore=`／`down=` 给目标），
六个窗口 `|err| 0.01…0.03` 单位（对 `r=5.42` ≈ **0.5%**），**什么都没拟合**。它预测的一刀
（游戏自己的坐标系）＝ 这一趟的 accept test：

| t | out | fore | down | r | elbow | 是什么 |
|---|---|---|---|---|---|---|
| 0.000 | 1.84 | 3.69 | 3.52 | 5.42 | 126 | 屏幕上已有的姿势 |
| 0.276 | −1.16 | −0.32 | −4.94 | **2.90** | **56** | 抬高**并且折起** —— 折起就是重点，`r` 走掉 2.52 单位 |
| 0.387 | 2.31 | 3.63 | 1.64 | 5.42 | 125 | through，手臂重新展开 |
| 0.690 | 1.79 | 4.11 | 1.38 | 4.46 | 94 | 余势 ＝ 香草的末键 |
| 1.000 | — | — | — | 5.42 | 126 | identity，回到捕获姿势（**唯一一个合成键**） |

落刀 ＝ 竖直 6.58／横向 3.47／前向 3.95（vert/lat **1.90**），155 ms 走弦 **8.43 ＝ 54.5 u/s ＝ T29 的 1.80×**
—— 因为这是**香草的节奏**，不是谁挑的节奏。

**⑤ 唯一另一件动了的事：落刀时刻从 1092 挪到 541 ms**。native tempo 把香草自己的 387 ms 抬手 ＋
155 ms 落刀放在**香草放的地方**，仍在 `kRideSwingArcMs` 里留下 433 ms settle。1092 从来没有对着引擎的
任何东西量过 —— 它是 T29 的 through 键、只为把上一趟的变量钉住。若判「太早」或「和伤害不同步」，
修法**不是**重塑形状，而是 `python tools\armarc.py --bake "chop down" --map lead`：滑动**同一张表**
让落刀回到 1092（它自己会印代价 —— clip 末键去到 `t=1.084` ⇒ 归位被截、交还 pop 回到 ~19°）。

**⑥ 见证换了符号，这是判据设计的核心**。T28 的两个见证是「肘不能动」的样子（`r=` 跨度 0.000、
平的 `angle(bx,arm)`）；T30 必须**跨**（离线一窗内 `r` **2.52** 单位、`elbow` **69°**）。三条只在这里成立的道理：
- 🔑 **无锚定 vs 锚定的分工**。`r`／`elbow`／`grip` 是**无锚定标量**（只由三个 derived 位置与手的 `+X` 决定）
  ⇒ 免疫鞍座 yaw、免疫躯干倾斜，够格进「测前登记」；而**方向**（vert/lat、`out/fore/down`）
  **只在同一套锚定约定内可比** —— T28 的 0.83、T29 的 2.17、T30 的 1.90 各自对自己的锚定成立，
  ⛔ **不许跨趟当成同一把尺子**。
- 🔑 **锚定见证的唯一盲区，与它的补丁**。锚定方向**按构造吸收掉整窗的常量旋转**（合成日志把整条手臂
  pitch 60°，中位滑移仍读 5 ms）⇒ 补丁 ＝ `anchor_tilt` 的 `tilt`（yaw **解释不掉**的那一份，因为绕 up 的
  偏转不动 up）。**经验带 5…40°**，第二十六趟六窗 **14.9 / 19.3 / 19.7 / 19.7 / 19.9 / 24.5**（对应 total 19.5…93.9）
  ＝ 坐姿躯干真的偏站姿文件 ~20° ⇒ 门槛是**绕 20 的一条带、不是 0**。
- 🔑 **时间轴有 ~40 ms 的地板**：`t=%.2f` 把 x 轴量化到 **±7 ms**，采样又落在写入之后一帧（~33 ms）
  ⇒ 40 ms 是**地板**。只是「动得快」的一刀不可能失败它，形状错的一刀不可能通过它。

**⑦ 判据是池化的，不是每窗的 —— 这是采样算术，不是宽容**。`kRideSwingArmLogGap = 12` 个作者帧、
`kRideSwingArmLines = 30` **每趟**（不是每窗）⇒ 一个窗口**不保证**拿到它的第一个作者帧：第二十六趟
13 个窗口里**只有 5 个**有 `kept<0` 那一行。⇒ `r`／`elbow` 跨度按**全部窗口池化**判（门槛 **1.5 单位 / 40°**，
对照 T28 的 FLAT **0.05**），并给「每窗 `dip=NO`」留一条专门出口 ＝ **采样缺口、不是失败**
（`dip` ＝ 那一窗有没有采到折起段：离线 `r ≤ 3.62` 且 `elbow ≤ 72.6` 落在窗口 `t` 的 0.04…0.18）。

**⑧ 握持跨度不再是防翻转证明 —— 这条必须写下来**。`grip` 带 8…45°、离线 1.8…30.2（spread **28.4**），
而第二十五趟那次**真翻转**读 27.1…54.0（跨度 26.9）⇒ **两者重叠** ⇒ 通过它**不证明**没翻。
防翻的保证在**离线**：小臂表是绕骨自身 **−Y** 的**纯铰链** —— 26 个键上 `max |x|,|z| = 0.000000`，
`--mirror` 每次运行都重新推导一遍 ⇒ 轴垂直于骨轴 ⇒ **不诱发绕骨轴的 twist** ⇒ host 驱动的
`Bip01 R Hand`（按设计**两张 mask 表都不列**）保住捕获时的 roll。T28 用**冻肘**买 `正手`，
T30 用**铰链**买 —— 肘因此可以自由，这正是①要的东西。

**⑨ settle 把环闭上（新判据）**：两张表末键都是 identity ⇒ `t=1.00` 的姿势必须**回到**捕获姿势
⇒ 判 `t ≥ 0.99` 与 `t ≤ 0.005` 两组样本的 `r`／`elbow` 差（门槛 **0.25 单位 / 8°**）。
⚠️ **这两个门是量出来的、不是编的**：`0.06 / 0.94` 那一版会让一份**完美**的合成日志自己 CHECK ——
`t=0.02` 已经读到 `r 4.86`（28 ms 走掉 0.56 单位），而末 clip 键（`t=0.690`）到合成 settle 键（`t=1.000`）
之间手臂**还在插值回家**，`t=0.94` 才走了 ~80% ＝ 0.7 单位 `r`。`t=1.00` 可达是因为弧把 `t` 夹在 1、
窗口继续跑到 `kRideSwingWinMs` ⇒ 有 ~250 ms（1-2 个采样）坐在 1.00。
⚠️ 它同时是「host 有没有在我们脚下前进」的探针：离线 `guard 1h` 在自己的键上只把小臂动 **0.1°**
⇒ 读数大 ＝ host 不是那个停着的 guard。

**⑩ `dot=` 从机制降级成诊断，门槛改由日志自己的帧率算**。姿势不再读父骨 ⇒ `dot=` 只活在日志里、
量的是**回读滞后**而不是形状。⚠️ 固定门槛在 30 fps 会**误报**（完美合成日志读均值 0.9685）⇒ 现在
`fps = median(Δf / (Δt × 1.400))`（跳过 `t=1.00` 夹持段那些 `Δt=0`），最差帧门槛 `1400/fps × 1.25`、
均值门槛 `214/fps × 3.0`；在 **90 / 60 / 30 fps** 三档对完美日志全 PASS、对播种 **3×** 滞后的日志全 CHECK。
🔑 **1400 °/s 是瞬时峰值**，不是每键均值：每键均值 **1244 °/s**（`t` 0.055→0.083，38.7 ms 走 48.1°），
而落刀**穿过 `r=2.90`** ⇒ 同样的线速度在短臂上扫过更多角度。中位 **214 °/s**（T29 的落刀是 461 °/s）。

**⑪ 镜像现在由资产推导，不再手抄**：`python tools\armarc.py --mirror` 把 `.cpp` 的两张表
**重新烘一遍**逐值对（含两个 `*Keys` 常量对行数、`ArcMs`／`WinMs`），armarc **每个模式开头自动跑、
不一致就拒绝出报告** ⇒ **严格强于**它取代的 `AXIS`/`ARC2` 手工镜像：**没有任何东西被手抄两遍**，
检查器自己从资产推它那一侧。⚠️ 表是**生成的，不许手改一行**。⚠️ 第三处（`ridelog.py` 的门槛）
**仍然是手工** —— 「测前登记」按定义推不出来。

**⑫ 一个字没动的东西**（写下来免得被读成回归）：四条时序不变式（`1400 < 1650 < 3000 < 3200`）、
`kRideSwingFreeBones` ＝ 我们手写的那两根、`Bip01 R Hand` 两张表都不列、弧走完**只看 `armt=`**、
`noref=` 仍是硬判据（它是 mask 的事、与模型无关，§17.9），以及回归风险仍是**下马后手臂僵在抬起的姿势**
（出口 ＝ `LegPoseRestoreImpl` 开头那次无条件 `RideSwingArmRelease()`）。

#### 17.19.4 ✅ 第二十七趟实测结果（2026-09-04，诊断全程 OFF，三趟骑乘／十一个窗口，日志 `D:\KenshiModDev\RE_Kenshi_log_trip27_960E3EB9_diagoff.txt` / 42442 B，构建 `324096 B` / md5 `960E3EB9…`）

**判决：过。「挥砍」这条线（`TASK.md` P4-3-4）到此收口。** 肉眼原话「**很标准的挥砍**」⇒ 落在
`ridelog.py` 印的四种结局里的第 1 条（*it reads as a sabre cut ⇒ P4-3-4 is DONE*），三个**再生**旋钮
（clip / tempo / form）一个都没用上。第二十六趟那句「侧面张开大臂带动刀」到这一趟变成「标准」
＝ **弯肘正是「张开」与「砍」之间缺的那一段**，也就是 §17.19.3 换模型时要买的东西。

🔑 **最强的一条读数：见证 1 与离线预测逐值吻合。** 池化 `r` 2.92…5.42（跨度 **2.50**）／`elbow`
57…126°（跨度 **69°**），而离线烘出来的表是 2.52 / 70° ⇒ **差 0.02 单位 / 1°**。这一栏同时答掉两件事：
① 两张表整条到了骨头上（不是只有肩那张生效）；② 第二十三趟那道防线（「方向在构建前就离线预测过」）
这次连**幅度**也预测对了。5/5 个采样窗口 `dip=yes`（采到折起段），而 T28 同一栏是 13 窗跨度 **0.000**
＝ 那时的 PASS —— 判据符号确实是翻过来用的。

**其余通过项**：见证 2 `angle(bx,arm)` 最宽 spread 28.3°（离线 28.4°，带 8…45）；settle 闭环 3 个采到
闭窗的窗口全 `+0.00 u / +0°`（门槛 0.25 / 8）⇒ 合成末键真的把手臂送回捕获姿势；**首帧 `dot=1.0000` ×3**
⇒ 开窗零 pop（T28 最好 0.9971）；`armt=1.00` 11/11・`kept` 最差 0.9998 / 76 样本・`noref=0`（11 闭窗）・
`pinst=live` 11/11 ＋ `pw=1.000`・窗口 1656…1657 ms・`rst/drv/hold/fit=0`・`hostkeep ＝ arm ＝ swfree`
3/3 趟・`swing=11` `tech=30` `noclip=0`・`armback` 全 `man=0x00` ＋ `minDot=1.0000`・mask 按名救回 13 个 ＋
`dropped=0` ＋ `grace=0`・straddle 与 v1.6 四条全不退・零 AV・**下马后右臂正常**。

⚠️ **两条软失败（都在测前登记里，都不阻塞）**：

- **三角闭合跨度 0.2300**（门槛 0.02；median **0.9982**、range 0.8970…1.1270）。🔑 median ≈ 1.0000
  ⇒ **不是体型缩放**（缩放会整条偏一个常数，那种情况下 `r` 的门槛才需要跟着缩）；双向散 ±12%
  ⇒ 三个 `_getDerivedPosition()` 不在同一帧（脚本的假设 ＝ 跨了骨架重建，§16）。**它量的是这三个
  诊断读数彼此的自洽性，不是姿势** —— 姿势是我们写进去的两个 local 四元数，不经过它们。
  ⇒ 已知诊断偏差、成因待查；要查就从「两根骨每帧按名重解析」那条路上找读数的时序，⛔ 不动表。
- **`dot=` 最差 0.9413（19.7°）／均值 0.9952 / 76 样本**（143 fps ⇒ 本趟门槛 worst 0.9772、mean 0.9969）。
  这是 §17.19.3 预告过的：T30 写 local、**不读父骨** ⇒ `dot=` 没有路径进入姿势。逐样本分离器把 6 个
  坏样本**全部**判成 `NOT lag on our write`（差额 10.5…19.7°，一帧滞后在那些点只值 2.9…5.3°）
  ⇒ 落在父骨 `R Clavicle` 那一侧的回读。⛔ 不许用「慢化落刀」去改善它 —— 那等于退回这一趟测的 tempo。
  🔑 **归因链到此完结**：9.6°（第二十四趟）→ 9.0°（换 host）→ 7.3°（换模型，T28）→ 本趟 19.7°
  而**观感反而最好** ⇒ `dot=` 与形状已经彻底脱钩，往后只当滞后计。

**这一趟没测到的**：诊断全程 OFF ⇒ P3CMB／DBG／腿部 `kept=`／按键四段报 NOT MEASURED（预期，不是缺口）。

### 17.13 ✅ **打架时坐骑的「行进方向」会与它的身体朝向反向** —— 骑手转向坐骑屁股的成因（2026-09-02 第十九趟用户报告，**2026-09-03 第二十趟实测通过**）

**用户原话**：「人物在牛背上打架的时候会突然转向牛屁股的方向，可能是和之前设定的在动物背上人物朝向和动物
前进方向一致有关系，动物打架时虽然身体的方向没发生变化，但是前进方向可能发生了变化。」—— **这个判断是对的，
而且指到了唯一的写入点。**

**机制（读代码得到，不是新逆向）**：`ApplyRiderOrientation` 的朝向源是坐骑的**每帧位置差**
（`CharMovement::getPosition()` 的 delta），水平长度超过 `kHeadingMoveEps = 0.03`（60 fps 下 ≈1.8 u/s）
才刷新，静止时**保持**上一次，然后按 0.35/帧 nlerp 低通。⇒ 打架时坐骑被打退／自己后撤：**身体没转、
delta 反了**，骑手就跟着朝后坐。日常行走不触发，因为动物指哪走哪。

**修法（否决，不是换源）**：delta 与坐骑**自己的朝向**（`GetMountFacingDirection` ＝
`CharMovement::getFacingDirection()`，**非虚、直调，从 2026-08-28 起就是既有的 facing 源 4**，`RidingPlugin.cpp:2504`）
做同半球判定；反向的那一帧**不采用**、保持上一次好朝向，并计数 `hdveto=`。
- `kHeadingFaceMinDot = 0.0f` ＝ 这里**唯一不是编出来的值**（「delta 与身体指向相反」）。**纯侧移读 dot ≈ 0，
  不被否决** ⇒ 若报告变成「转向侧面」，旋钮就是这个常量（0.25 ≈ 75°）。
- 选否决而不是换 facing 源，是为了**让已经实测过的行为在除这一种情形外的每一帧都保持逐位不变**：
  普通帧上动物指哪走哪 ⇒ 判定恒不触发。
- ⚠️ **`mountHeadingDir` 只有这一个写入点** ⇒ 这一处也同时修好了 `GetMountForward` 的 src=2
  （＝ 座位偏移的前/侧向旋转基准，反向时偏移也跟着镜像）。
- ⚠️ `getFacingDirection` **不会退化**（静止、从没走过一步时也有值），这也是它当年被选作 facing 源 4 的理由；
  它的 header RVA 存疑（§18.9 ②）与这里无关 —— 这里是直调，从不依赖那个 RVA。

**实测结果（2026-09-03 第二十趟，诊断全程 OFF）**：肉眼 ✅ 用户原话「**1.不会突然转向了**」，
且正常骑行／跨骑腿型／座位偏移一条没退（＝ 上面「除这一种情形外逐位不变」那条推理成立）。
日志 `hdveto=` 七趟 1034/69/65/374/828/436/110（合计 **2916** 帧保持了上一次好朝向）⇒ 否决真的在开火。
⚠️ `hdveto=` 永远只是 NOTE：它 ＝「发生过反向 delta」的次数，**不是**「修好了」的证明（真判据是肉眼），
而 `hdveto=0` 也只意味着那一趟没发生反向。




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

- **三参 `0x535E50` ← 拔刀侧**：`CharacterHuman::drawWeapon`（头 `0x5DB800` ＋0x780 ＝ **`0x5DBF80`**，ENTRY prolog=45，序言 `40 55 53 56 57 41 54 41 55 41 56 48 8D 6C 24 D9`）站点 `0x5DC1D4`（✅ **2026-09-01 运行时兑现**：`P43AT` 的 logged `site=0x5DB749` 换算过来正是这一条 call 的下一字节，见 §18.10）；`leaveSheathEquipped`@`0x5D24B0` 站点 `0x5D2709`。⚠️ `leaveSheathEquipped` **自己也是 `drawWeapon` 的被调方** ⇒ 拔刀侧是三参族这件事有**两条独立确认**。
- **二参 `0x535D50` ← 收鞘侧**：虚表 `+0x198` 的「按位置串挂回去」`0x5DBD80` 站点 `0x5DBE8A`（§18.7:602 记的就是这一条）；`0x5D0BC7` 站点 `0x5D0CE0`；另有两处在 `0xCFDD0` 里（`0xD0048` / `0xD013C`，与武器无关的邻域）。
- **`--calls 0x5DBF80` 的 12 个被调方里有 `0x535E50` 与 `0x5D24B0`，没有 `0x535D50`。**

⚠️ **两个重载不能靠 hook 一个覆盖**：`--calls 0x535D50` 的 7 个被调方是 `0x6A6F0`(leaf) / `0x52D850` / `0x52E0F0`(`detachItem(slot)`) / `0x5358A0`(`createAttachedObject`) / `0x542090` / `0x544400` / `0x544BA0` —— **它是完整实现、不是转发桩，一次都不调 `0x535E50`**。⇒ 探针**两个都要 hook**，日志带 `ov=2/3` 区分，这样「静默」就只剩一种读法。

⚠️ **取槽名的寄存器按重载不同**（x64 调用约定，`this` 占 RCX）：二参 ＝ RCX=this / RDX=item / **R8=&slot**；三参 ＝ RCX=this / RDX=item / R8=&mesh / **R9=&slot**。搞错就会把网格名当槽名去比 `"hands"`，那又是一次静默。

**方法论：`callers.py` 打在真入口上只看见一个站点，那大半是 ILT 跳板，必须再追一跳。** 实测 `0x535D50` 只有 1 个直调者、而它是个 `jmp`（`jmp NOT in any .pdata function`）；追过去 `0x42163` → 4 个真站点。三参那边 `0x1760C` → 2 个真站点。**别把「1 个站点」读成「几乎没人调」。**

**~~顺带定死的两处逻辑函数边界~~ —— 前半条已作废，见 §18.10**（MSVC 把一个逻辑函数拆成多条 `.pdata` 记录，`prolog=0` 的是续块，见 §18.7 负面结果 1）：❌ ~~T6 那个收刀站点 `0x5D051C`（`hook_probe` 报 `MID+298`）落在续块 `0x5D03F2-0x5D076D` 里，其入口是 `0x5D0217`（prolog=8）⇒ 逻辑函数 ≈ `0x5D0217-0x5D0779`~~ —— **`0x5D051C` 是 logged 值、不是本文件的 RVA**（要 `+0xA90` ＝ `0x5D0FAC`），所以这段边界是**对一个假地址做的划界，整段无效**。真站点的边界在 §18.10。✅ 二参挂载站点 `0x5D0CE0`（这个是**静态解码出来的**、不经探针 ⇒ 不受 §18.10 影响）落在续块 `0x5D0BFC-0x5D0D12`，入口是 **`0x5D0BC7`**（prolog=5）。

**T6 那一趟点名出来的两个站点：`0x5D051C` 与 `0x5CE183` 都是 logged 运行时值，真站点见 §18.10**（2026-09-01 从 `TASK.md`／`TEST_REQUIRED.md` 收容进来；同日 §18.10 查明要 `+0xA90`）。⇒ 真正的两个收刀写手是 **`0x5D0FA6 in 0x5D0DB0` ＝ `Character::_ragdollMode`**（ragdoll 消息那条路，`real=16`、gap 中位数 22 帧、重复出现 —— 第六趟同一站点 `real=11 noop=657`）与 **`0x5CEC0C in 0x5CE9C0` ＝ `Character::_carryMode(on=true,…)`**（续块 `0x5CEBA0+0x73`，`real=3` ≈ 每次上马一次）。定名与四路证据见 §18.10。`over=0` ⇒ 12 条站点表没满、**没有第三个真写手被挤掉**。⚠️ 想改这两个数字前先看 `TEST_REQUIRED.md` 的 T6 判据，它们是那五条验收里最吃重的两条的证据；⚠️ **别再把 logged `site=` 直接当 RVA 抄进任何文档** —— 抄之前先跑 `callers.py --ret`。

⚠️⚠️ **方法论更正：`prolog != 0` 证不了「这是可调用入口」**（§18.7 负面结果 1 与 `callers.py` 的 `logical_entry()` 原来都把「续块」等同于 `prolog=0`，**错了**）。实测 `0x5CEA40` 自己是一条 `.pdata` 记录、`prolog=40`，但它 **0 个直调站点、0 个指针引用**，Ghidra 也把它并进 `0x5CE9C0` ⇒ MSVC 的 separated/chained 记录**可以带自己的 prolog 大小**。⇒ 判「是不是入口」要**三件一起看**：`.pdata` ENTRY ＋ 有直调站点或指针引用 ＋（能反编译时）反编译器认的 `getFunctionContaining`。`logical_entry()` 的 `prolog=0` 走法只是启发式，**别当判据**。

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

**工具**：Ghidra 12.1.3 ＋ Temurin JDK 21 便携装在 `D:\KenshiModDev\revtools\`（仓库外、无需管理员、不改系统环境）。`kenshi_x64.exe` 的 auto-analysis **506 秒**跑完并存成工程 `revtools\ghidra-projects\kenshi`（462 MB，**一次性开销、别重跑**），得 **169,169 个函数**。接了 MCP 桥 `pyghidra-mcp`；不经 MCP 也能查：`revtools\scripts\DecompAt.java`（按地址反编译）/ `FindSym.java`（按子串搜符号）走 `analyzeHeadless -process kenshi_x64.exe -noanalysis -readOnly -postScript`。⚠️ 脚本**只能写 Java**（这份 Ghidra 没按 PyGhidra 模式启动，`.py` 一律报 `Ghidra was not started with PyGhidra`）。⚠️ 搜符号必须用 `getName(true)` 取**全限定名**，`getName()` 会全 0 命中（类名在 namespace 里）。⚠️ **传参有两个坑**（2026-09-01 各踩一次）：`analyzeHeadless` **吃掉任何以 `-` 开头的 arg**（脚本压根看不见），而且**按 `=` 把 `key=value` 拆成两个 arg** ⇒ `DecompAt.java` 的打印行数上限现在收**裸十进制**（`… -postScript DecompAt.java 0 0x1405D0DB0`，`0` ＝ 不截断），地址一律 `0x` 开头。⚠️ 还有第三个：改完 `.java` 若行为没变，是**编译缓存**没失效 —— 删 `%APPDATA%\ghidra\ghidra_12.1.3_PUBLIC\osgi\compiled-bundles\*\<脚本>.class` 再跑。

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

### 18.10 点名探针打出来的 `site=` **不是本文件的 RVA**：要 `+0xA90`（2026-09-01）

**这一条推翻了前面几节里所有「站点 RVA」的直接读法，先看它再看 §18.8:669/:671。**

⚠️ **状态注记（2026-09-01）：打出这些 `site=` 的两个探针（`CharacterHuman::sheatheWeapon` ×1、`AppearanceBase::attachItem` ×2）已经从插件里摘掉了**（不带探针的干净构建，逐行经过与取回路线见 `TASK.md` X-5）⇒ **新日志里不会再有 `P43SH` / `P43AT` 行**。**本节的地址、换算与定名一个字都不改** —— 它们是记录，不跟着代码删；`ShDescribeAddr`（下面提到的那个减法）也随探针出去了，将来任何新探针要复用它就从那次提交里取回。

探针打的是 `_ReturnAddress() - GetModuleHandle(kenshi_x64.exe)`（当时的 `ShDescribeAddr`，就是个减法，没有别的加工；⚠️ 那个函数已随探针摘掉，原来的行号 `RidingPlugin.cpp:1092` 从此作废）⇒ 那**本该**就是 RVA。实测不是：三个 logged site 在本文件里**全部解码在指令中间**，谁都不是返回地址；而**同一个常数** `+0xA90` 一加，三个全部精确落在「一条 call 的下一字节」上，且那条 call 的目标正是被 hook 的那个函数。

| logged `site=` | `+0xA90` | 落点 | 那条 call | 目标 |
|---|---|---|---|---|
| `0x5DB749`（`P43AT` 三参挂载） | **`0x5DC1D9`** | `0x5DBF80+0x259` | `0x5DC1D4` len 5 `E8` | 跳板 `0x1760C` → **`0x535E50`**（三参 `attachItem`） |
| `0x5D051C`（`P43SH` 频繁那个） | **`0x5D0FAC`** | `0x5D0DB0+0x1FC` | `0x5D0FA6` len 6 `FF /2` | `[reg+0x2D0]` ＝ `sheatheWeapon` 的虚表槽 |
| `0x5CE183`（`P43SH` 次要那个） | **`0x5CEC13`** | `0x5CEBA0+0x73`（续块，逻辑入口 **`0x5CE9C0`**） | `0x5CEC0C` len 7 `REX FF /2` | 同上 `[reg+0x2D0]` |

**为什么这算定案**：①第一行的落点 `0x5DBF80` ＝ `CharacterHuman::drawWeapon`，而 §18.8:659 **早在静态就预言了「三参挂载的调用方是 drawWeapon、站点 `0x5DC1D4`」** —— 运行时独立命中同一个站点，这是**先验预测被兑现**，不是事后对齐；②后两行的 call 都吃 `[reg+0x2D0]`，而 `0x2D0` 正是 `sheatheWeapon` 的槽（§18.7），`--vcall 0x2D0` 那 38 个站点里恰好有这两个；③三个站点一个常数、`0x5D051C` 那个站点在一趟里被走了 668 次而值稳定 ⇒ 不是栈垃圾、不是尾调。

⚠️ **成因未解释。** 已逐个排除：磁盘上第二份 exe（只有一份，36,718,592 B，比日志旧）、`ke_pe` 映射算错（节表 ＋ 代码字节双向核过，`off = ptr + (rva - va)` 教科书式正确）、KenshiLib 头 delta 其实是 `0xA90`（拿 10 个符号试 `+0x310`，9 个落在函数中间 ⇒ **`+0x780` 与 §12/§18 全部照旧有效**）、MinHook 跳板/尾调。⚠️ 因此**范围只到 `0x5CE000-0x5DC000` 这个窗口**（三个锚点都在里面）；出了这个窗口按未验证对待，**每次都要用 `callers.py --ret <logged rva>` 重新验**，那条命令把 raw 与 `+0xA90` 两行并排打出来，"no call instruction ends here => NOT a return address" 就是判据。常数记在 `tools\ke_pe.py` 的 `RUNTIME_RVA_DELTA`。⚠️ **2026-09-02 第十三趟多了一个窗口外的锚点**：`0x535383` 在 `0x535xxx` 也 `+0xA90` 落在一条真的 5 字节 call 上（→ 同一个 `detachItem` 入口，raw 行明确 `NOT a return address`），细节见 **§18.12.1** ⇒ 两个不相交窗口各有锚点，但**成因仍未解释、纪律一字不改**（每个 logged `site=` 都跑 `--ret`）。

**便宜的下一步（要做的时候顺手带上，不值得单独出一个构建）**：arm 那一刻打一行 `ShDescribeAddr((uintptr_t)GetRealAddress(att3))`。它印 `+0x535E50` ⇒ 偏移只在返回地址上；印 `+0x5353C0`（＝ `0x535E50 - 0xA90`）⇒ 整个模块基址口径差了这么多，全 image 一次定死。

✅ **补注（2026-09-02 第十二趟，§18.11.1）：窗口外又添了两个锚点，同一个 `+0xA90`，共 5 个。** `P43DT` 打出的 `0x5CBDF8` 与 `0x535383` 两条 raw 都「不是返回地址」，`+0xA90` 后分别落在 `0x5CC888`（call `0x5CC883` → `0x52E0F0`）与 `0x535E13`（call `0x535E0E` → `0x52E0F0`），两个落点都在 §18.11 那张**静态先做好的**站点表里 ⇒ 又是一次先验预测被兑现。⚠️ **`0x535383` 在 `0x535xxx`、`0x5CBDF8` 在 `0x5CC8xx`，两个都在 `0x5CE000-0x5DC000` 之外** ⇒ 上一段那句「范围只到这个窗口」现在**偏保守**，但**成因仍然未解释**，所以纪律不变：**照旧每条 logged site 都用 `callers.py --ret` 验一次，永远不要手算**。

**顺带定死的语义（`callers.py --strings`，§18.6 的无反编译认脸法）**：`0x5D0DB0` 在收刀那条 call 之后 ~0x20 字节触到 `'female ragdoll'`(`0x5D0FC6`)／`'male ragdoll'`(`0x5D0FCD`) ⇒ **倒地/ragdoll 那条路**；`0x5CEA40` 触到 `'VO_Creature_Die'`(`0x5CEB73`) ⇒ **死亡那条路**。两个都是虚表进入（`0x5D0DB0` 只有 ILT 跳板 `0xC97D` 一个直调者，`0x5CEA40` 零个直调者）。

**✅ 两个宿主已定名定型（2026-09-01，Ghidra ＋ 头文件 ＋ `.pdata` 三路互证）—— 这两个名字就是 P4-3 第 1 步要的答案**

| 真实入口 | 头 RVA | `.pdata` | 头文件签名 |
|---|---|---|---|
| **`0x5D0DB0`** | `0x5D0630` | ENTRY prolog=43 | **`bool Character::_ragdollMode(const Character::RagdollMsg& message)`**（`Character.h:690`，protected） |
| **`0x5CE9C0`**（`0x5CEA40`/`0x5CEBA0` 都是它的续块） | `0x5CE240` | ENTRY prolog=10 | **`void Character::_carryMode(bool on, bool makeRagdoll, bool makeHull)`**（`Character.h:639`） |

四路证据，每一路都独立：①头 RVA **＋0x780** 精确落在两个 ENTRY 上（`hook_probe.py 0x5D0630 0x5CE240`）；②**参数形状对得上签名** —— `_ragdollMode` 反编译成 `(this, char *msg)` 且只读 `msg[0]`(char) 与 `*(uint*)(msg+4)`(mask) ＝ `RagdollMsg` 的两个字段，`_carryMode` 的序言第 13 字节就是 `84 D2`(`test dl,dl`) ＝ 那个 `bool on`，反编译的第一行正是 `if (param_2 != '\0')`；③**常数** —— `_ragdollMode` 里 `uVar2 == 1` / `uVar2 == 0x800` 与 `(uVar2 & 1) || (uVar2 >> 0xb & 1)`，`_carryMode` 第一件事就是 `local[0] = 0x800` ⇒ 正是 `RagdollPart::WHOLE`(1) 与 **`CARRY_MODE`(0x800)**；④**体内第三个符号也按同一 delta 落点** —— 两个函数都调 `0x5CE1E0`，而 `Character::dropCarriedObject(bool,bool)` 头 `0x5CDA60` ＋0x780 ＝ `0x5CE1E0`（ENTRY），调用形状 `(this,1,0)` 也对。顺带第五路：`param_1[0x89]` ＝ `+0x448` ＝ `animation`、`param_1[0x89]+0xE8` ＝ `appearance`，与 §18.7 从 `sheatheWeapon` 体内解出来的布局逐字吻合。

**收刀发生在哪一条语句上（这才是修法要看的东西，⚠️ 只是描述、不是许可）**：两个函数都有同一段拆除序列，收刀是其中一条 —— `dropCarriedObject(true,false)` → `…(param_1[200], 0)` → `param_1[0xc9]->vtbl+0x60` → 两个 `0x5074A0`/`0x50D1B0` → `0x674830(param_1[0x50],1)` → **`this->vtbl[0x2D0]()` ＝ `sheatheWeapon`** → `…(param_1[200])`。
- `_ragdollMode` 里这段被 **`if (mask & WHOLE) || (mask & CARRY_MODE)`** 罩着（`0x5D0FA6` 就在里面），之后才去播 `'male ragdoll'`/`'female ragdoll'`。
- `_carryMode` 里这段是 `on != 0` 那一支的尾巴（`0x5CEC0C`，续块 `0x5CEBA0`），`'VO_Creature_Die'` 只在 `part == WHOLE` 时播 ⇒ 死亡是那一支的**另一个**用法，不是收刀的条件。

⇒ **P4-1e-2 那句「绑在被携态而不是战斗状态上」现在有机制了**：骑乘本身就跑在 `RagdollPart::CARRY_MODE` 上，而 `_carryMode(on=true,…)` 的尾巴与 `_ragdollMode(mask ∋ CARRY_MODE)` 的拆除段**都无条件收刀**。日志侧的配比也对得上：`_carryMode` 站点 `real=3`（≈ 每次上马一次），`_ragdollMode` 站点 `real=11..16`、间隔中位 22 帧（≈ ragdoll 消息的节律）。
⚠️ **仍然只是点名。** 两个都是**非虚成员函数、都有头文件符号** ⇒ 理论上 `GetRealAddress(&Character::_ragdollMode)` 能 hook，但**要不要动、动哪一端还没定**，`HISTORY.md` §B 那条「不许对绝对覆写做写入端补偿」照旧；而 `_carryMode` / `_ragdollMode` 是**引擎自己维护被携态的两条主干**，在它们身上改行为的风险远超「少收一次刀」。⚠️ 反编译读数不是实测（§18.9 结尾那条）。


### 18.11 `AppearanceBase::detachItem` 的静态全图 —— P4-3 第 2 步第 3 个探针的靶子（2026-09-02，全程静态）

第十一趟把 P4-3 第 2 步顶成唯一卡点：**数据层有刀、屏幕上没刀**（`post=1` 4/4、`wih=0->1`、`aCW` 离开 `SKILL_UNARMED`、`wpn=1` 52/56），而第六趟已证 `attachItem(item, mesh, "hands")` 真的跑过（`hands=12` / `other=0` / `slot0='hands'`）。⇒ 剩下要点名的是**挂上去之后谁把它撤掉**，靶子就是 `detachItem`。

| 真实入口 | 头 RVA | `.pdata` | 头文件签名（`Appearance.h`） |
|---|---|---|---|
| **`0x52E0F0`** | `0x52D970` | ENTRY prolog=43 | `bool AppearanceBase::detachItem(const std::string& slot)`（:71） |
| `0x52E290` | `0x52DB10` | ENTRY prolog=20 | `bool AppearanceBase::detachItem(Item* item)`（:70） |
| `0x52D960` | `0x52D1E0` | ENTRY prolog=15 | `AttachedEntity* AppearanceBase::getAttachedEntity(const std::string& slot) const`（:69） |

**① 只 hook 一个重载 —— 这是对 §18.8「两个重载都要 hook，否则沉默有两种读法」的偏离，靠静态事实撑，不是图省事**：
```
python tools\callers.py 0x52E0F0 0x52E290     -> 各 1 个直调站点，都是自己的 ILT 跳板（0x1E88F / 0x44026）
python tools\callers.py 0x1E88F 0x44026       -> 0x1E88F 有 14 个站点；0x44026 有 0 个
python tools\callers.py --ptr 0x52E0F0 0x52E290 -> 两个都 0 个指针宽引用 ⇒ 都不在任何虚表里
```
⇒ `Item*` 那个重载**在 exe 里没有任何可达调用方**（跳板零引用 ＋ 不在虚表 ⇒ 也不可能被间接叫到）。所以「`slot` 侧沉默」只有一种读法。

**② `detachItem(slot)` 的 14 个静态站点（`0x1E88F` 跳板侧），归属逐条查明 —— 探针一有命中就能当场归因**：

| 站点 | 所属函数 | 是谁 |
|---|---|---|
| `0xD0053` / `0xD011F` | `0xCFDD0` | 未定名 |
| `0x5324D4` | `0x532490` | `Appearance::attachItem_Hair` |
| **`0x535E0E`** | `0x535D50` | **`attachItem` 二参：写槽前先清槽** |
| **`0x535ECF`** | `0x535E50` | **`attachItem` 三参：写槽前先清槽（拔刀走的就是这个，§18.8）** |
| `0x5C8294` | `0x5C8272` | 未定名续块 |
| `0x5CA517` | `0x5CA4A0` | `CharacterAnimal::dropItem` |
| `0x5CA7B7` | `0x5CA740` | `CharacterHuman::dropItem` |
| `0x5CC7B2` | `0x5CC77C` | `dropWeaponInHands` 尾块（§18.7） |
| `0x5CC883` | `0x5CC820` | `CharacterHuman::sheatheWeapon`（**已被 P4-3-2 抑制**） |
| `0x5D0D74` | `0x5D0D52` | 未定名续块 |
| `0x5D2632` | `0x5D24B0` | `leaveSheathEquipped` |
| `0x5DC416` | `0x5DC310` | `Character::unequipItem`（虚，有 `_NV_`） |
| `0x5DC5F5` | `0x5DC560` | `CharacterHuman::unequipItem`（虚，有 `_NV_`） |

⚠️ **两个 `attachItem` 站点 ＝ 自噪声，同时也是这个探针自带的阳性对照**：每次成功拔刀**必然**从 `attachItem` 体内产生一条 `hands` 拆卸。⇒ 站点表里**只有这两条**是「没人拆过手槽」这个有意义的答案，**不是探针失败**。
⚠️ 两个 `unequipItem` 是「非 `attachItem` 站点」里最强的候选（虚 ＋ `_NV_` ＋ 直接调 `detachItem`）；落在 `0x5C8272` / `0x5D0D52` 这两个未定名续块上则还要再查一次归属。

**③ hook 安全性（`KenshiLib::AddHook` 固定拷 5 字节、无 trampoline）**：`0x52E0F0` 序言 `48 8B C4 | 56 | 57 | 41 54 …` ＝ `mov rax,rsp` ＋ `push rsi` ＋ `push rdi` ＝ **三条完整的位置无关指令，刚好在第 5 字节边界收口**（第 6 字节起是 `41 54` ＝ `push r12`）⇒ 比出货件里 `AnimationClassHuman::_NV_update` 那一刀更干净。三个都是**非虚成员函数**，`GetRealAddress` 的「虚函数不行」不适用；重载歧义用带类型的成员指针局部量解决（`typedef bool (AppearanceBase::*DetSlotFn)(const std::string&);`），**源码里不出现任何 RVA 字面量**。

⚠️ **只许点名**（`TASK.md` P4-3 第 2 步的闸门）。两个答案都不许当场变成「我们自己去调 `attachItem`」＝ 对绝对覆写做写入端补偿（`HISTORY.md` §B）。
⚠️ 全静态，**反编译／PE 读数不是实测**（§18.9 结尾）。

### 18.11.1 运行时结果 —— **没有第三方写手**（2026-09-02 第十二趟，标的 `4E223D95…`，两趟骑乘，肉眼 A 不过／B 过）

⚠️ **这一节原来的标题写的是「卡点是渲染侧」，那半个结论已被 §18.12.1 推翻** —— 真卡点是 `drawWeapon` 的第 2 个参数（空串被 `leaveSheathEquipped` 的白名单挡掉），修好之后**渲染侧一个字节都没改就正常了**。⇒ 本节仍然有效的部分是**「`hands` 拆卸只有 `sheatheWeapon` 一个站点、没有第三方写手」** 这条排除，以及下面那张字段对照表（它正是找到鞘位差异的地方）。「转渲染侧」那个箭头**别再跟着走**。

| | 空手上马（`first=0`，肉眼**不过**：刀留在背上） | 先拔刀再上马（`first=7EA3A218`，肉眼过） |
|---|---|---|
| `hands` 拆卸站点 | `sites=1`，**全部** `0x5CBDF8` ×293 | `0x5CBDF8` ×107 |
| `attachItem` 自清槽 | **一条都没有** | `0x535383` ×1，摘的是 **`back`** |
| 同趟 `P43SUP pass=`（放行的收鞘） | **293** | **107** |
| 手槽占用 `nonnull/samples` | 6540 / 14885（44%） | 1398 / 3869 |
| `swap` / `appswap` / `nullapp` | 0 / 0 / 0 | 0 / 0 / 0 |

- **两趟的 `hands` 拆卸数 ＝ 放行数，一个不多一个不少**（293＝293、107＝107），被抑制的那 286 次一条拆卸都没产生 ⇒ **手槽的唯一写手就是我们已经在抑制的 `sheatheWeapon`**。「≥3 个站点 ＝ 第三方写手」那条读法**被排除**。
- 换算一律走 `callers.py --ret`，**没有手算**（§18.10）：
```
python tools\callers.py --ret 0x5CBDF8 -> raw 不是返回地址；+0xA90 = 0x5CC888 in 0x5CC820+0x68，call 0x5CC883 -> 0x52E0F0
python tools\callers.py --ret 0x535383 -> raw 不是返回地址；+0xA90 = 0x535E13 in 0x535D50+0xC3，call 0x535E0E -> 0x52E0F0
```
  两条落点正是上表的 `0x5CC883`（`sheatheWeapon`）与 `0x535E0E`（`attachItem` 二参自清槽）⇒ **§18.11 的站点表被运行时兑现两条**，同时给 §18.10 的 delta 添了两个**窗口外**的锚点（见 §18.10 补注）。
- ⚠️ **那条「自带阳性对照」要打折**：预言是「每次成功拔刀必然从 `attachItem` 体内产生一条 `hands` 拆卸」，空手那趟（5 次 `post=1` ＋ 12 级 `P41E` 梯子）**一条都没有** ⇒ 三参的清槽**带条件**（我们的拔刀门控在手槽为空时才开，空槽无须清）。阳性对照实际兑现在**二参**那条上：收鞘把刀挂回 `back` 之前先清 `back`。⇒ 结论不受影响（拆卸数与放行数仍逐趟对齐），但**别再拿「站点表只剩那两条 ＝ 没人拆手槽」当判据**。
- ⇒ **抑制期内手槽是持续被占的**（空手那趟 44% 的帧），没有拆卸、没有 Appearance 重建（`appswap=0`/`swap=0`/`nullapp=0`），刀却在屏幕上留在背上 ⇒ **P4-3 第 2 步的「谁撤掉」这一问已答完：没人撤。剩下的是渲染侧。**
- ✅ **本趟最有价值的一条 —— `weaponInHandsSheathLocation`（`0x6E0`，§17.3）在两趟里不一样**：肉眼过的那趟 `sh='back'`；不过的那趟 5/5 条 `P43RD` 全是 `sheath '' -> ''`。而我们调的是 `rider->drawWeapon(sw, std::string())` ＝ **第 2 个参数传空串**，`drawWeapon`@`0x5DBF80` 的被调方里就有 `leaveSheathEquipped`@`0x5D24B0`（§18.8:661；它引用 `"hip"/"back"/"back2"/"sheath"`），而 `sheatheWeapon` 第 5 步正是拿这个字段当位置串把刀挂回去（§18.7:615）。⇒ **假设**：空串让「把刀从鞘里取出来」那一步没执行，背上那份挂载原地不动，手槽里那份只存在于数据层。
  - ⚠️ **是假设、不是实测**。要坐实需要两条：①静态解 `drawWeapon`@`0x5DBF80` 怎么用第 2 个参数（以及引擎自己的调用方传什么，`--vcall 0x3D8`）；②运行时把 `getAttachedEntity` 从 `"hands"` 扩到 `"back"/"back2"/"hip"/"sheath"`，看背上那份是否与手槽**同时**非空。
  - ✅ 这条**不违反 `HISTORY.md` §B**：改的是我们传给引擎自家 API 的参数，不是对每帧覆写做写入端补偿。
  - ✅✅ **①已做完，假设坐实 ⇒ 见 §18.12**；②不必再做（要确认的那件事 `P43DT` 现成就能打：修好以后每次拔刀会多出一条 `slot='back'` 的拆卸）。

### 18.12 `drawWeapon` 的第 2 个参数 ＝ **鞘位**；`leaveSheathEquipped` 只认 `"hip"`/`"back"`，空串 ＝ 整段不执行（2026-09-02，Ghidra 反编译，§18.11.1 那条假设**已坐实**）

**一句话**：`drawWeapon` 把第 2 个参数原样交给 `leaveSheathEquipped`@`0x5D24B0`，后者开头是一张**白名单** —— 只有 `"hip"`（`0x16C0580`）与 `"back"`（`0x16C0584`）能过，且长度必须**精确相等**；别的字符串（**包括空串**）一律 `goto` 收尾、清串、**什么都不做**。被跳过的那一段正是「把刀从背上摘下来」：

| 被跳过的动作（`0x5D24B0` 体内） | 地址 | 逐条对上的症状 |
|---|---|---|
| `detachItem(appearance, loc)` | `0x52E0F0`，站点 `0x5D2632`（§18.11:788 早就列了这一条） | 刀的 mesh 一直挂在背上 ⇒ 肉眼「刀还在背上」 |
| `weaponInHandsSheathLocation = loc` | 写 `Character+0x6E0`（§17.3），站点 `0x5D26CE` | `P43RD` 5/5 全是 `sheath '' -> ''` |
| `attachItem(app, weaponInHands, <"sheath" 网格>, loc)` | 三参 `0x535E50`（§18.8），站点 `0x5D2709` | 背上不会出现空鞘 |

⇒ **`std::string()` 就是 T15 那个「渲染侧卡点」的成因**：三条症状一条不漏地对上白名单外那一跳，不是巧合。

**`drawWeapon`@`0x5DBF80`（901 B）的形状**（`param_1`＝`this`／`param_2`＝`Item* wpn`／`param_3`＝鞘位串）

```c
if (!wpn) { this->vt[0x428](); wpn = this->m_0x6D0; }
if (this->weaponInHands /*0x6D8*/) {
    if (this->weaponInHands == wpn) { loc.clear(); return 1; }   // ⚠️ 啥也没干就 return 1
    this->vt[0x2d0]();                                          // sheatheWeapon（§18.7）
}
if ((c = FUN_747BB0(this->m_0x2E8, loc)) && *(int*)(c+0xB8)) {   // loc 当 KEY 被读一次
    if (!FUN_74C650(this->m_0x2E8, wpn)) { loc.clear(); return 0; }
    loc.assign(wpn + 0xE8);                                     // 命中就用 Item 自己的串顶掉
}
*(char*)(wpn + 0x129) = 1;
mesh = wpn->vt[0x18]()->map_0x1F8["bare sword"] + 0x28;         // 网格名从游戏数据取
if (wpn->vt[0x2d8]()) mesh = wpn->vt[0x18]()->map_0x1F8["mesh"] + 0x28;
if (mesh.size != 0)                                             // ⚠️ 只看网格名空不空
    attachItem3(*(void**)(this->m_0x448 + 0xE8), wpn, mesh, "hands");
... this->weaponInHands = wpn; AK::SoundEngine::SetSwitch("Weapons", ...);
if (wpn != this->m_0x6D0) leaveSheathEquipped(this, copy_of(loc), (int)*(int*)(wpn+0xE0));
loc.clear(); return 1;                                          // 每条返回路径都清
```

- ✅ **挂 `"hands"` 那一步只被「网格名非空」门控，跟第 2 个参数无关** ⇒ 这就是为什么空串照样 `post=1`、手槽照样被占（§18.11.1 的 `nonnull=6540/14885`），而背上那份原地不动。**数据层成功、渲染层没变**，两件事在源码里是两条独立的门。
- ⚠️ **KenshiLib 头文件的 `const std::string&` 是错的**：`param_3` 在**每一条**返回路径上都被就地清空（`_Myres=0xf`／`_Mysize=0`／`buf[0]=0`），命中上面那个 `assign` 时还会被整串顶掉。它是个**被消费的入参**，不是 const 引用。
- ⚠️ **传进去的串必须 ≤ 15 字符**：清空走的是 `if (0xf < _Myres) operator_delete(_Ptr)` —— 堆串会让**游戏的**分配器去 free **我们的**缓冲。`"hip"`/`"back"` 落在 MSVC 的内部缓冲里，这条分支是死代码。
- ⚠️ **`weaponInHands == wpn` 时 `drawWeapon` 直接 `return 1`**，一件事都不做。⇒ 读 `post=` 时别把 1 当「这次真拔了」。
- ⚠️ **`drawWeapon` 体内自己会经 vtable `+0x2D0` 调 `sheatheWeapon`**（手上已有*另一把*时）—— 那正是 P4-3-2 抑制器坐的位置。当前骑乘路径进不到这一支（手槽逻辑上是空的），但改抑制器之前要想到这条。

**`leaveSheathEquipped`@`0x5D24B0`（718 B）的形状**（`param_3` ＝ `(int)*(int*)(Item+0xE0)`）

```c
if (loc != "hip" && loc != "back") goto done;      // ← 空串死在这里
if (loc == "back" && param_3 > 0) loc = "back2";   // ⚠️ "back2" 是它自己推的
if (this->weaponInHands /*0x6D8*/) {
    detachItem(app, loc);
    sheathMesh = weaponInHands->vt[0x18]()->map_0x1F8["sheath"] + 0x28;
    this->weaponInHandsSheathLocation /*0x6E0*/ = loc;
    if (sheathMesh.size != 0) attachItem3(app, this->weaponInHands, sheathMesh, loc);
}
done: loc.clear();
```

- ⚠️⚠️ **永远不要传 `"back2"`**：白名单只认 3 字节的 `"hip"` 与 4 字节的 `"back"`，`"back2"` 长度不等 ⇒ 直接掉进 `goto done`，回到今天这个 bug。要 `"back2"` 就传 `"back"`，让它自己按 `Item+0xE0` 推。

**为什么 `--vcall 0x3D8` 答不了「引擎自己传什么」（负面结果，省重复劳动）**

- `--vcall 0x3D8` ＝ **72 个站点**，比 §18.7 的 `0x2D0`（38 个）更糟；抽查 `0x5D02A2 in 0x5D00B0+…` 反编译出来是 `(**(code**)(*plVar7+0x3d8))(plVar7)` ——**单参**、返回值当指针判空 ⇒ 那是别的类的同偏移槽。静态给不了接收者类型，这条路封死。
- `callers.py 0x5DBF80` ＝ 1 个直调站点，且是 ILT 跳板 `0x3C0E7`；**追一跳后 `0x3C0E7` 有 0 个直调者** ⇒ 引擎对 `drawWeapon` 的调用**全部**走虚表，静态看不见任何一个实参。
- ⇒ **鞘位的真值只能从运行时拿**，而它已经拿到了：肉眼过的那趟（引擎自己拔的刀）`sh='back'`（§18.11.1）—— 同一个角色、同一把刀，引擎用的就是 `"back"`。
- `callers.py 0x408D1`（`leaveSheathEquipped` 的 ILT）→ 唯一站点 `0x5DC2A1 in 0x5DBF80+0x321` ⇒ **`drawWeapon` 是它唯一的调用方**，白名单挡掉的就是整条鞘位流水线。

**复现口令**（都离线）
```
python tools\callers.py --strings 0x5DBF80        -> 'bare sword' / 'mesh' / 'hands' / 'Weapons'
python tools\callers.py --strings 0x5D24B0        -> 'hip'(0x16C0580) / 'back'(0x16C0584) / 'back2' / 'sheath'
python tools\callers.py 0x5DBF80 0x5D24B0         -> 各 1 个站点，都是 ILT 跳板
python tools\callers.py 0x408D1                   -> 0x5DC2A1 in 0x5DBF80+0x321
analyzeHeadless … -postScript DecompAt.java 0x1405DBF80 0x1405D24B0 0x140535E50 0
```
⚠️ **方法论**：`--field 0x6E0` 把 `0x5DC6F5/0x5DC727/0x5DC745`（`in 0x5DC650`）列成 `lea r`，看着像鞘位的读者，其实是 `FUN_791BF0(this+0x6E0)` 那类**取地址当参数**的用法。`--field` 的 `lea` 行只说明「这里取了这个成员的地址」，**别读成读/写鞘位**。

### 18.12.1 运行时确认 —— **修法成立，白名单那道闸是唯一的闸**（2026-09-02 第十三趟，标的 `D3D4879E…`，四趟骑乘，`TEST_REQUIRED.md` T16 已关）

**肉眼（真判据，两半都过）**：半 A 空手上马 ＝ **「打架时会自动掏刀，刀全程在手上，打完会自己收起来」**；半 B 先拔刀上马 ＝ 和以前一样刀在手上（对照没坏）。⇒ 症状从「数据层有刀、屏幕上没刀」直接消失，**不需要任何渲染侧改动**。

**日志侧的三条自证**（`python tools\ridelog.py`，全在 `RE_Kenshi_log_trip13_D3D4879E.txt`）

| 字段 | 第十二趟（空串） | 第十三趟（`"back"`） | 读法 |
|---|---|---|---|
| `P43RD` 的 `sheath` | `'' -> ''` **5/5** | **`'' -> 'back'` 3/3** | `weaponInHandsSheathLocation`（§17.3）终于被写进去了 |
| `P43DT` 的 `slot='back'` 拆卸 | **一条都没有** | **`0x5D1BA7` other=39** | 就是 §18.12 说被跳过的那一段现在跑了 |
| `P43DT` 的 detach 站点数 | 1（只有 `hands`） | **3** | 多出来的两个都是 `slot0=back` |

**两个新站点由 `callers.py --ret` 换算，不是手算**（§18.10 那条红线）：
- `0x5D1BA7` → `+0xA90` ＝ `0x5D2637 in 0x5D24B0+0x187`，call site **`0x5D2632`**（len 5）→ `0x52E0F0` entry ⇒ **正是 §18.11:788 记的、§18.12 判定被空串跳过的那次 `detachItem(app, loc)`。** 39 次 ＝ 每次真拔刀一次。
- `0x535383` → `+0xA90` ＝ `0x535E13 in 0x535D50+0xC3`，call site **`0x535E0E`** → `0x52E0F0` entry ⇒ 二参 `attachItem` 的自清槽（§18.11 的「自带正控制」），40 次 ＝ 每次收鞘挂回 `back` 前先清 `back`。39/40 近乎相等 ＝ **一个平衡的拔/收循环**，差的那一次是上马时的 `_carryMode` 收鞘。

⇒ **`leaveSheathEquipped` 的白名单是唯一的闸**：它一开，引擎自己的拔刀/收刀循环就正常了（`P43HD gain=37 / loss=38` 对上 39 次 `back` 拆卸，而我们自己只贡献 3 次架势边沿补拔 ＋ 10 级 `P41E` rung 0）⇒ **屏幕上看到的那把刀绝大多数是引擎自己拔的**，我们只是把它一直缺的那个参数补上了。⚠️ **这条只由「次数」支持，时间不支持** —— 见下一段。

⚠️ **「引擎按战斗节律拔/收」这条读法不成立。** 能看到时间的那些 `P43HD` 边沿（DLL 侧预算 `kHdLossLines=8`／趟，所以只看得到每趟最早的几条）**全部落在 t≈95.9–97.6**，也就是**上马之后一两秒、离战斗窗口还很远**（同趟 `P41D` 的 `read` 窗口是 `203.828 .. 415.986`）：7 对 `gain`/`loss`，每对只持续 **29–330 ms**，而且**每次的 Entity 指针都不一样**（`016D6710` / `016E5FB0` / `016DF9D0` / `01702590` / `016F6E90` / `7E4A1700` / `17FD6C2C0`），全程 `st=0 cm=0 bc=1`。⇒ 两条推论：
- **手槽占用（以及 `weaponInHands` 的 0↔1 边沿）不是「引擎认为在打架」的信号，不能拿来当 `TASK.md` P4-3 第 3 步的触发条件。** 亚秒级抖动 ＋ 每轮换一个 Entity，接上去只会每秒钟触发好几次。
- **这一串的归属分不清**：候选是我们自己的 `kDrawTryBudget` 梯子（12 次／趟，已知诊断开着时**在上马那一刻就烧完**）与引擎，诊断是开着的 ⇒ 只能靠**不带诊断的那一趟**（`TEST_REQUIRED.md` T17）来分。**次数那半照旧成立**（39 次里我们最多贡献 3＋10＝13 次），所以「大多数是引擎拔的」按次数仍然对；被推翻的只是「按战斗节律」这个读法。

⚠️ **`+0xA90` 因此多了一个窗口外的锚点。** §18.10 原来的范围只到 `0x5CE000-0x5DC000`（三个锚点都在里面），现在 `0x535383` 在 `0x535xxx` 也 `+0xA90` 落在一条**真的 5 字节 call**上、而且目标就是同一个 `detachItem` 入口（raw 那一行明确 `NOT a return address`）⇒ 两个不相交的窗口各有锚点。**成因照旧未解释**，纪律不变：**每个 logged `site=` 都要跑 `callers.py --ret` 重验，永远不许手算。**

**旧不变量照旧成立**：`0x5CBDF8`（＝ `0x5CC883 in CharacterHuman::sheatheWeapon`，§18.11.1）的 `hands=818` **精确等于**同趟四段 `P43SUP pass=` 之和（524＋242＋0＋52＝818），被抑制的 `real=214` 一条都没漏 ⇒ 抑制器与拆手槽的账仍然是一对一的（第十二趟是 293＝293／107＝107）。

## 19. 离线读游戏数据（FCS 记录 / Ogre 资产）—— TASK.md P4-3 前提②

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
- ✅✅ **运行时确认（2026-09-01 第六趟，`P41D` 判读层，312832 B）**：`chooseAttack` 交出的名字正是这一族裸 clip 名（`flykick` ×21 / `megakick` ×13 / `shoteiL` ×12 / `ma chudan` ×3 / `ma 2strike` ×2，`ch=1` 出现在 107 次读里的 51 次），其中 **5 个报 `ABSENT in allAnims`** ⇒ §19.3 的离线结论在引擎侧成立。
  - ⚠️⚠️ **同一趟还量到一个新事实：引擎自己已经把其中一批种成了 NULL。** 13 行 `P41D clip key='<名>' ptr=0000000000000000 UNREADABLE`（`shoteiL` / `flykick` …）—— 那个调用点只有 `allAnims.find()` **命中**才走到 ⇒ **键在表里、值是空指针 ＝ 中毒条目**。能造成它的只有 `getAnimationData()` 的 `operator[]` 语义，而插件这一侧一个字节都没送进去（`FindAnimData()` 返回 `mi->second`、调用方全部指针判空）⇒ **是引擎的战斗代码自己拿技法 clip 名查过一次**。
  - ⇒ **判读纪律**：同一份日志里 `ABSENT`（还没人查过）与 `find` 命中却 `ptr=0`（已经被查过、留下了毒）会**并存**，两者都**不是**「记录存在」。⇒ 「靠 `layer`/`wholeBodyAllLayer` 判层」这条路在运行时也确认没有 ⇒ 只能钉上去实测。**这条同时是 §15 那道 `FindAnimData()` 硬约束的活体证据：连引擎自己都在污染这张表。**
- 🔑 **这道墙的范围已经量清了（2026-09-02 第十八趟，§17.10）：它只挡 `allAnims` 一侧，不挡 Ogre 一侧。** 同一批技法 clip 名（`bigchopv2` / `chop left-3`）送进 `getAnimationState()`（按名走 Ogre 侧、完全不碰 `allAnims`，§21.1/§21.4）**17/17 全部非 NULL** ⇒ **没有 `AnimationData` 记录的裸 `.skeleton` 轨道照样拥有 `Ogre::AnimationState`**。⇒ 本节「查不出层、因为没有东西可查」照旧成立（`FindAnimData`/`ClipPin` 这条路永远够不到它们），但**「这些 clip 根本没法播」从来不是本节的结论，现在更明确不是** —— 驱那个 state 自己的权重是活着的候选，见 §17.10。

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
⚠️ **少报的只有 BONE 一种**：`ANIMATION` 的长度**把自己的名字算进去了**（`aimcrossH` 报 13940 ＝ 6 ＋ 10（名字）＋ 4（秒）＋ 30 轨 ×（6 ＋ 2 ＋ 12 键 × 38），逐字对得上）⇒ **别把 BONE 那条补正套到 ANIMATION 上**：套了就晚 10 字节，把名字的 ASCII 当 chunk 头读成 `id=0x6F68 len=685167`，8 MB 的文件读成「`bones=30 animations=1`」。这两个坑都不碰结构化走法，`skelanims.py` 一个容器长度都不用。

### 19.6 边界与用法
⚠️ **本节全部是「授权侧 / 资产侧」静态事实，不是运行时状态。** P1 早就证明两者会分叉（`crawl idle down` 表里写 `spd 0/0/0 play=0.00`，活体上 `t01` 每帧照推）。静态可以用来**挑候选**、读**静态属性**（层、flags、技能指名哪条 clip、clip 存不存在、时长）；**绝不能用来预测运行时权重、时序或混合结果**。
- `python tools\gamedata.py` ＝ itemType 直方图 ／ `--verify` 自检 ／ `--anim [子串]` ANIMATION 表 ／ `--tech` 技能→记录 ／ `--rec <子串>` 整条记录 ／ `--refs <子串>` 引用类别 ／ `--type <N>`。
- `python tools\skelanims.py` ＝ 人形骨架的骨数＋全部轨道 ／ `--find <子串>` 过滤 ／ `--skel <文件名子串>` 换骨架 ／ `--list` 全部 `.skeleton` 的轨道数 ／ `--tech` 上面那张交叉核对表（它 `import gamedata`）／**`--bones` 骨号＋父骨＋bind 四元数** ／ **`--sweep [子串]` 每条 clip 的肩扫角与肘弯角** ／ **`--track "<clip>" [--bone <骨名>]` 一根骨的整条曲线**（后三个是 2026-09-04 加的，见 §19.7；⚠️ 这个文件里只有**一个** parser，别再写第二份 —— `armarc.py` 那份 `read_bones()` 是历史遗留、只读 bind、内部四元数序是 `(w,x,y,z)` 而这里是磁盘序 `(x,y,z,w)`，两边搬数字要转）。
- 附带结论：type-112 `base animations`（`1533847-gamedata.base`）**不靠引用枚举任何东西**（0 个 refcat，只有两个 `.skeleton` 文件名字段）；所有名为 `animations` 的 refcat 都挂在 **itemType 76** 记录上。

### 19.7 ✅ vanilla 的挥砍曲线可以离线读出来，而且和 `RideSwingArmPose` 写的是**同一个空间**（2026-09-04）

- 🔑 **键值是「相对 bind 的增量」，`local = bind * key`**（Ogre 播骨架轨道 ＝ `Skeleton::reset()` 回 bind ＋ 每轨 `Node::rotate(key, TS_LOCAL)`）。⚠️ **这条是在文件里实测的、不是抄上游源码**（§21.5 禁把上游源码当第二真相源）：`guard 1h` 里 `Bip01 L Toe0Nub` 的 3 个键**逐个都是单位四元数**，而这根骨的 bind 是很显眼的 `(0,1,0,0)` ＝ 180° ⇒ 要是存的绝对 local，键就该是 `(0,1,0,0)`。⇒ **`Bone::setOrientation` 要的正是 `bind * key`** ⇒ 曲线**不用换空间**就能进我们的手写器（§17.19 的两根骨、我们的 mask），这和第二十二趟判死的那一族（驱引擎的 `AnimationState` 走动画系统）**不是同一条路**。
- 🔑 **文件 ＝ 运行时那些 clip 的来源**（跨源对账，两个数字）：`mid blow` 在文件里 `length=4.633 s`，与第十九趟在游戏里读到的 `olen=4.633 s`（§17.11）**逐字相同**；骨号也对得上日志 free 表的 `R UpperArm=26 / R Forearm=27`（另：`R Hand=28`、`Bip01 Prop2=29` ＝ 武器挂点、是 `R Hand` 的子骨）。`guard 1h` ＝ 0.067 s / 3 键的准静态姿势 clip，正是我们钉的那个宿主。
- **male 174 条 / female 166 条**（`female_skeleton.skeleton` 9416189 B），同名 clip 的形状一致（`chop down`：male 167.6°/69.2°、female 167.5°/68.9°，差别只是采样密度 26 键 vs 30 键）⇒ 从哪一份烘都一样。
- **形状盘点**（`python tools\skelanims.py --sweep`；肩 ＝ 上臂离**本 clip 首键**的最大角，肘 ＝ 小臂的同一个量）：`chop down` 0.967 s **167.6° / 69.2°**、`heavy downcut` 1.633 s 105.9°/75.9°、`bigchopv2` 2.833 s 105.9°/76.0°、`chop left` 1.067 s 102.7°/61.8°、`desperate attack` 1.667 s、`heavy swing` 4.000 s。`chop down` 的整条肩曲线（`--track "chop down"`）＝ 0→167.6° 用 271 ms（≈620 °/s）、再回到起点另一侧 19° 用 232 ms、剩下一半 clip 停在 ~19° 收势。
- 🔑 **肘那一列全都不是 0，而 T28 模型的肘结构上恒为 0**（小臂 local 每帧逐字回写，§17.19）⇒ 这是现模型摸不到的形状；`kRideSwingArc` 六键 ＋ 一根轴（155° / 461 °/s）能对上肩的量级，**对不上肘**。⇒ 「只剩轴一个旋钮」（CLAUDE.md）之外还有第三条：**把曲线烘进 `RideSwingArmPose`**，两根骨各一张表，`X(t) = conj(K(0)) * K(t)` 施加在窗口开时捕获的姿势上 ⇒ `X(0)=identity` ⇒ §17.19 那条「窗口开在屏幕上已有的姿势上」的性质原样保住。
- ⚠️ **仍然是资产侧读数**（§19.6 那条纪律照旧）：它给形状与时长，**不给**层、`whole`、混合权重，也不预言在马背上混出来什么样。烘不烘、烘哪条 ＝ `TASK.md` P4-3-4 的下一个决定，不是本节的结论。

---

## 20. 例本：工坊 2994497775 的 `ride.dll` 怎么做骑乘（**静态读二进制，2026-09-01，没进游戏、没读 hook 体**）

**身份**：`D:\steam\steamapps\workshop\content\233860\2994497775` ＝「帝国风云（版本1.9.4.6.2）」（`<mod>2333</mod>`，最后更新 2026-08-31），是一份 **RE_Kenshi 插件 mod**：
`RE_Kenshi.json` ＝ `{ "Plugins" : [ "plugin/KenshiExtensionPlugin.dll", "2333.dll","ride.dll" ] }`。
`ride.dll` 163840 B，唯一导出 `?startPlugin@@YAXXZ`（ride.dll+0x1230），PDB 路径
`C:\Users\HP\KenshiLib_Examples_deps\ride\x64\Release\ride.pdb` ⇒ **和我们同一个底座**（KenshiLib 示例工程 ＋ MSVCR100/VS2010 CRT）。
用户实测过它：「用人的骨架做了一匹马把人扛起来，扛起来的人物可以战斗」。

⚠️⚠️ **本节全部是第三方二进制的静态读数 ＝ 线索，不是证据。** 拿它做任何改动仍要过原来三道门（静态核地址 → 编译 → 进游戏实测），和 §19.6 同一条纪律。**hook 体一个都没反编译**，凡标「静态推断」的都待核。

### 20.1 它挂了 12 个钩子，一个都不在我们的禁用名单上
**手法**（和我们不同，值得记）：`GetModuleHandleA("KenshiLib.dll")` ＋ `GetProcAddress(<C++ 修饰名>)` → `KenshiLib::GetRealAddress` → `KenshiLib::AddHook`。⇒ 名字是**运行时字符串**，二进制里躺着完整修饰名 ⇒ **hook 清单可以静态点名**（12 个 `GetRealAddress` 站点：2 个在 `startPlugin` 里直接 `AddHook`，另 10 个走同一个包装 ride.dll+0x11400）。虚函数（`hitByMeleeAttack` / `update` / `mainLoop_GPUSensitiveStuff` / `operate`）只能这么拿地址，这也解释了为什么它们不在静态导入表里。

| 安装点（ride.dll RVA） | 钩住的 KenshiLib 导出 |
|---|---|
| `startPlugin` 0x1230 | `TitleScreen::_CONSTRUCTOR`（延后初始化）、`GameWorld::mainLoop_GPUSensitiveStuff(float)`（**每帧驱动**） |
| 0x2ED0 | `Character::hitByMeleeAttack(...)`、`MedicalSystem::addWound(...)` |
| 0x3410 | `Character::dropCarriedObject(bool,bool)` |
| 0x3470 | `UseableStuff::operate(Character*,float)` |
| 0x36E0 | `CharBody::_move(RootObjectBase*,const Vector3&)`、`HavokCharacter::calculateAvoidanceVector(...)`、**`CombatMovementController::combatMovementAnimationUpdate(const Vector3&,const Vector3&,AnimationClass*,bool)`**、`CharMovement::update(float)` |
| 0x3800 | `GameWorld::resetGame()` |
| 0x3920 | `Character::_startStumble(CutDirection,Damages&,GameData*,Character*)` |

### 20.2 三条「它不做什么」—— 对我们信息量最大的部分
1. **完全没有 Ogre `AnimationState` / blend mask**：`OgreMain_x64.dll` 只导入 21 个符号，动画侧只有 `Node::setPosition` / `Node::setOrientation` / **`OldBone::setManuallyControlled`**（3 个调用点），刷新靠**主动调** `AppearanceBase::forceUpdateAnimationTransforms`。⇒ 它**没有我们 T8 那一类 mask 泄漏面**（我们的 `gLegMasked[]` / `LegMaskRelease` 是自选负担，不是骑乘的必需品）。
2. **没挂我们永久禁的那两个**（`beingCarriedUpdate` / `updateAnimationTransforms`，§0）：每帧活儿挂在 `GameWorld::mainLoop_GPUSensitiveStuff`，变换刷新是主动调而不是挂钩子 ⇒ **一条独立的结构旁证：那两个钩子确实不是必需品。**
3. **全身骨都手控，不止腿**：`.rdata` 里 22 根骨名 —— `Bip01 Pelvis` / `Spine`/`Spine1`/`Spine2`/`Spine3` / `Neck` / `Head` / 左右 `Clavicle`/`UpperArm`/`Forearm`/`Hand` / 左右 `Thigh`/`Calf`/`Foot`。⇒ 手控骨在生态里是**常规手法**，不是我们独有的偏方（与 §16 一致）。
- 输入只有 `USER32!GetAsyncKeyState` 一个 ⇒ 没有键位设置界面，我们 §14 那条（键位进原版设置菜单）比它完整。

### 20.3 座位底座 ＝ 引擎原生 carry ＋ 队伍改派
静态导入里成套出现：`Character::pickupObject` / `getPickedUp` / `carryModeT(bool,bool,bool)` / `isBeingCarried` / `dropCarriedObject` / `getDropped` / `hand`（`isValid` / `getCharacter` / `toString`）/ `CharBody::getCurrentSubject` / `Character::setBedMode`。⇒ **它不自建座位，直接让「马」扛人**（＝ 用户看到的样子），我们 §2-§4 那套 carry 逆向对它是同一片地。
队伍侧：`ActivePlatoon::getSquadLeader` / `setSquadLeader` / `addCharacterAt` / `Character::setSquadMemberType` / `getPlatoon` ＋ 存值键字符串 `mount_old_squad_type` ⇒ 上马时把骑手改派进坐骑的队伍、下马时按记下来的旧值还原。位置侧：`teleport`（两个重载）/ `get`/`setTerrainHeightPosition` / `getCurrentFloor` / `getBoneWorldPosition` / `CharMovement::trackAnimationMovement` ＋ `isTrackingAnimationMode` / `AbstractMovementBase::setDesiredSpeed`。

### 20.4 坐姿疑似走「换 `idle stance` 授权引用」而不是每帧压骨（**静态推断，待核**）
`.rdata` 里有三个字符串 `idle stance`（＝ 引用类别名）、`idle_stand_normal`（＝ 目标 stringID 形状）、`races`，配合导入的 `GameData::getReferenceList` / `GameDataReference::getPtr` / `GameDataContainer::getDataOfType` / `Character::get`/`setAppearanceData`，再配合存值键 `mount_old_idle_stance`（＝ 上马前的旧值，下马还原）。
⇒ 读法是：**它换的是角色授权侧的站姿引用，让引擎自己去播那条 clip**，不是像我们那样每帧把骨压过去。⚠️ 只有字符串 ＋ 导入的组合，**hook 体没读 ⇒ 不算定论**。
⇒ **对我们的意义（也只是意义，不是方案）**：如果站姿引用真能在运行时换，那 P2 的坐姿有可能不必靠 mask ⇒ T8 那条泄漏面有机会整块消失。要动之前必须先反编译它的 hook 体，再走三道门。

### 20.5 战斗侧七个钩子逐个对到我们的阶段上
- **`CombatMovementController::combatMovementAnimationUpdate`** ＝ 我们路线 A 每天在打的那一层（`guard 1h` 抢躯干、`pose=` 权重、TWIST）的**正规缝**。我们从来没在这里挂过钩子 ⇒ **这是本节最值得跟进的一条。**
- `CharMovement::update`（虚）＋ `CharBody::_move` ＝ 移动/跟随的两个接缝（我们现在靠别处的每帧回调）。
- **`Character::_startStumble`** ＝ 抗打断。我们 P4-4 走的是「被击倒就强制下马」，它多一个选择：**把踉跄本身吃掉**。
- **`HavokCharacter::calculateAvoidanceVector`** ＝ 让坐骑与骑手不互推（我们的「人粘在鞍座上」是靠别的办法维持的）。
- `Character::hitByMeleeAttack`（虚）＋ `MedicalSystem::addWound` ＋ 导入的 `Damages::multiply` ⇒ **伤害分流/缩放**（谁吃这一刀、吃多少）；我们 P3 是自己那一套。
- `UseableStuff::operate`（虚）＝ 交互式上马；`GameWorld::resetGame` ＝ 读档/新游戏时清状态（我们该照抄这个习惯）。

### 20.6 还没读的与怎么读
- **没读**：`ride.dll` 的 hook 体（下一步：Ghidra，先读 `combatMovementAnimationUpdate` 与 `mainLoop_GPUSensitiveStuff` 两个 hook，再读 20.4 那条 `idle stance` 路径）、`2333.dll`（45568 B，看着是这份 mod 自己的玩法插件）、`plugin\KenshiExtensionPlugin.dll`（1229312 B ＝ 第三方框架 KEP，带 `kep_settings.json` 与 ja_JP/ru_RU 的 gettext `.mo`）。
- **怎么读（可复现）**：`ride.dll` 的清单是纯 PE 静态读 —— 导入表 ＋ `.rdata` 里的修饰名字符串 ＋ `FF 15` 间接调用站点回指 IAT ＋ `.pdata` 归属。⚠️ 它的名字是 **KenshiLib 导出名**、不是 exe 地址 ⇒ 本节**故意不写任何 `kenshi_x64.exe` 地址**；真要落到 RVA 得走 §18 那条 header RVA ＋ `HEADER_RVA_DELTA` 的换算，而且换算结果只许写在本文件里。表格里的 `0x1230` 一类**是 `ride.dll` 自己的 RVA**，别拿去和 §12 的表比。

## 21. `Ogre::AnimationState` 的生命周期 —— TASK.md T8 的 (a)/(b) fork **定案：(b)**（2026-09-01；定案本身全程静态，修法与结论已由第八趟实地确认，见 21.4）

**一句话**：`dropped` 不是「那个对象死了」，它就是「那条 clip 停了」。⇒ 我们清零的大腿 handle **留在一个活着的对象上**，泄漏真实、按 `(角色, clip)` 计、跨趟累积。

### 21.1 四条静态事实（都可复现）

1. **取 state 只有一条路，而且是查表、不是创建。** `0x51CAA0`（104 B、Ghidra `EXACT_ENTRY true`）＝ `AnimationNameIDMapper::getSingleton()` → `getAnimationID(name)` → `Entity::hasAnimationState(id)` → `Entity::getAnimationState(id)`，没有就返回 NULL。**`param_1 + 0xA8` ＝ `Ogre::Entity*`**（宿主是 `AnimationClass` 一族）。孪生体 `0x51CB10`（89 B）同形，但走 `+0xB8` 那个带虚表的载体、虚槽 `+0x1D8`。另有两个不带 `has` 检查的取法：`0x845B60+0x2F`、`0x84B890` 里三处。
2. **`SingleAnimation` 绑定 ＝ `0x5B3090`（186 B）**：`+0x28` 非 0 就直接返回（已绑）；否则按名字查（`+0x38` ＝ 宿主 `AnimationClass`，而 `param_1` 自己就被当 `std::string*` 传进去 ⇒ **`SingleAnimation+0x00` ＝ clip 名**），存进 `+0x28`，再 `setLoop`/`setWeight(0)`/`setTimePosition`/`setEnabled(true)`。
3. **解绑 ＝ §18.9 ① 那个 `0x5B15C0`（83 B）**：`setWeight(0)` → `setEnabled(false)` →（`+0x68` 时）`setDisableTranslation(true)` → `+0x28 = 0`。**没有 destroy，也没有碰 blend mask。** 2 与 3 正好是一对，`+0x28` 是同一个字段。
4. **exe 对 `Ogre::AnimationState` 的全部导入 ＝ 8 个方法，全是属性存取**（`setWeight` 20 站点 / `setEnabled` 15 / `setDisableTranslation` 11 / `getLength` 17 / `setTimePosition` 10 / `setLoop` 5 / `getTimePosition` 2 / `getAnimationName` 2）。**`blendmask` 在 2542 条导入里 0 命中**，也没有任何 ctor/dtor、`createAnimationState`/`destroyAnimationState`/`removeAnimationState`。⇒ Kenshi **不可能**创建或销毁一个 state，也**不可能**清掉一个 blend mask ⇒ **往 state 上装 mask 的只有我们自己**。（导入是链接期事实 ⇒ 「导入表里没有」对跨模块 API 是**证明不存在**，不是「没找到」。查法：`python tools\callers.py --import "(?i)blendmask"`。）

### 21.2 定案：`dropped` 的成因就是 clip 停了

`LegMaskRelease` 判 live 的方式是「某 layer 的 `addList`/`removeList` 里有 `SingleAnimation::mainState == st`」。而 21.1(3) **一停 clip 就把 `mainState` 写 0** —— 那条 `SingleAnimation` 还在表里、state 也还活在实体的 `AnimationStateSet` 里，只是 `mainState` 不再等于它 ⇒ 我们**必然**查不到 ⇒ 记 `dropped`。所以第六趟那个「7 个里 6 个查不到」根本不是异常，是**停了的 clip 的数量**。

⇒ **(b) 成立**：mask 留在活对象上；而且按 21.1(2)，下次播同一条 clip 会用**同一个 animation ID** 从**同一个实体**取回**同一个对象**，我们清零的大腿 entry 原样在上面。

### 21.3 (a) 那一支剩下的唯一出口 —— 不在骑乘路径上

一个 state 只会随实体那整套 set 一起重建，能做这件事的只有：

- **`Entity::_initialise(bool)`**，全 exe **2 个真站点**：`0x449930+0x16`（该函数 123 B，实参 **`false`** ⇒ 已初始化的实体**不**重建）与 `0x44BF40+0x5CB`（该函数触 `'queued'`/`'preloaded'`/`'load_id'`/`'loaded'`/`'Kenshi_ProgressBarFill'` ⇒ **资源装载/进度条**那条路）。
- **`shareSkeletonInstanceWith`**（`0x52D480+0x86`、`0x84B890+0x16B`）与 **`stopSharingSkeletonInstance`**（`0x52DC90+0x292`、`0x531F40+0x89`）—— 都作用在**副实体**上：`0x52DC90` 触 `'overlap items'`、`0x531F40` 触 `'Hair'`、`0x84B890` 触 `'idle stance'`/`'Posture'`/`'Neck position'`/`'portrait offset x'`/`'RTT_Portrait'` ＝ **立绘 RTT**（顺带：§20.4 说例本换的那个 `'idle stance'` 授权键，Kenshi 自己在立绘路径上也读）。
- ⚠️ **`_deinitialise` / `destroyEntity` / `destroyMovableObject` / `~Entity` 一条都没导入** ⇒ 实体的死法全在 OgreMain 内部，静态看不见。

⇒ 上/下马不碰这些站点；但**角色实体一旦被重建或卸载，我们那张跟踪表的指针会整体悬空**。这就是修法必须自带活性判据、而不能直接解引用旧指针的理由。

### 21.4 修法因此有了确定的对象（✅ **已实现、已部署、已实测通过** —— 第八趟，2026-09-01）

不再要求「在 `addList`/`removeList` 里查得到」，改成**按名字回取、再比对指针**：装 mask 时抄下 clip 名 ⇒ 交还时用 `AnimationNameIDMapper::getSingleton()` ＋ `getAnimationID(name)` ＋ `Entity::hasAnimationState(id)` ＋ `Entity::getAnimationState(id)` 重新取一次（**四个都是 OgreMain 导出，不需要任何 exe 地址、不需要新 hook**）：

- 取回**同一个指针** ⇒ 对象活着、还是那条 clip ⇒ **可以安全清 mask**；
- 取回 NULL 或换了指针 ⇒ 那套 set 被重建过 ⇒ 无事可做，丢掉是对的。

宿主实体从 `AnimationClass + 0xA8` 取（21.1(1)；若 KenshiLib 头里已有字段名就用头里的）。⚠️ 这**只是交还我们自己装的 mask**，不是往骨头上多写一次 —— 写入端补偿照旧禁止（`HISTORY.md` §B）。

**实现（2026-09-01，312832 B / md5 `E7613634783EDE5E948573B0C6ED3285`）**：上面那四步**一个都不用自己拼** —— 它们整套就是 21.1(1) 那个 `0x51CAA0`，而这个函数在 KenshiLib 头里**是有声明的**（`AnimationClassBase::getAnimationState(const std::string&)`，header RVA `0x51C320` ＋ `HEADER_RVA_DELTA 0x780` ＝ `0x51CAA0` ⇒ **同一个函数，双向对上**）。⇒ 落地成三处改动，都在 `RidingPlugin.cpp`：①跟踪表多一列 `gLegMaskedName[64]`（装 mask 时从 `SingleAnimation::animName` 抄，即 21.1(2) 那个查表用的同一个字符串）；②`LegMaskRelease` 的 live 判据加第二条路 —— 路 1（两张表）失败时才问路 2 `rAnim->getAnimationState(clip) == st`，并计数 `gLegMaskLate`；③交还日志多一个 `late=` 字段。⚠️ **`body`（0xA8）为 NULL、名字为空、或回取到 NULL/别的指针一律照旧丢弃**，从不解引用旧指针；名字超 64 字符会被截断 ⇒ 退化成旧的 `dropped`，不会误判。⚠️ **`late` 是自证字段**：修法前每次交还 `dropped=2..4`，修法后健康形状是 `dropped=0 late=2..4`；**`late=0` 说明救援一次都没受力，此时 `dropped=0` 不构成证据**（`tools\ridelog.py` 会照这个三分法直接打结论）。


**实测（第八趟，2026-09-01，同一份 md5 `E7613634…`，5 次上马 5 次下马）—— 两半都过，本节从此是实测结论不只是静态推理**：5 次交还**全部 `dropped=0`**、`late` 合计 **12**（4/2/2/2/2），`man=0x00` / `minDot=1.0000` / `residue=0` 照旧，`takeovers=5 restored=5 released=0`、`kept` 90/90 全 `1.0000`；肉眼「大腿张开，跑步时大腿跟着摆，很正常」⇒ 第七趟那个「大腿并着打不开、只有小腿在动、一直不恢复」不复现。⚠️ **`late=12` 才是这条实测的要害**：路 1（扫两张活表）在同一批交还里对这 12 张 mask **全部失败** ⇒ 21.2 的定案（clip 一停 `mainState` 就被置空、对象却活着）在运行时得到独立确认，「(b) 是真泄漏」不再只靠行为侧推。

### 21.5 顺带的后果

- **原计划为分 (a)/(b) 而做的那一次构建 ＋ 一趟骑乘不用做了**（TASK.md 那条 fork 已改判）。⚠️ `TEST_REQUIRED.md` 后来还是开了一格 —— 但那是 **T11 ＝ 验 21.4 修法本身**（`dropped=0 late>0` ＋ 肉眼大腿张开），**不是**分 (a)/(b) 的那一格；**T11 已于第八趟通过、条目已删，那份文件现在零条待测**。
- **「泄漏的是不是起步/转向那类短命 clip」这个问题与 (a)/(b) 脱钩了**：不管哪条 clip，mask 都留着。⇒ 起步那一下「轻微夹腿」的候选解释变成「**上一趟留在起步/转向 clip 上的 mask**」；第七趟把它推到了尽头（宿主 clip 有两次是 `run lower`，被清零之后症状从「轻微、暂态」升级成「大腿一直打不开、只有小腿在动」＝ **(b) 的行为侧实证**）⇒ 走了 21.4 的第二条路：**直接上修法看症状消不消** —— **第八趟给出判决：消了**（见 21.4 的实测段）。
- ⚠️ **全节是反编译 ＋ 导入表读数 ＝ 静态事实，不是实测**（§18.9 末尾那句照旧有效）。⚠️ **Kenshi 的 OgreMain 是改过的**（`AnimationNameIDMapper`、`Entity::hasAnimationState(uint)` 按 **ID** 取而不是按名字、`OldSkeletonInstance`）⇒ **别拿上游 Ogre 源码当第二真相源**。
- 🔑 **§21 一直没敢断言的那一条，2026-09-02 第十八趟量到了：「没有 `AnimationData` 记录的 clip 有没有 `AnimationState`」＝ 有。** 17/17 次 `getAnimationState('bigchopv2' / 'chop left-3')` 全部非 NULL（§17.10）。⇒ 21.1 那条链**不只是 mask 交还的救援路**，它同时是**够到那 43 条没有记录的技法 clip 的唯一已知入口**。⚠️ 只证明「state 存在」，**不**证明引擎在骑乘中启动过它：34 行读数里 `enabled` **全是 0**，其中 10 行是 `chop left-3` 被播到 `t=0.251` 之后被关掉、权重清零（那一版已无 `runCombatAnimation` ⇒ 动它的是引擎自己的地面战斗）⇒ **引擎驱得动这些 state，但骑乘中一次都没驱**（形状全表见 §17.10）。
- **工具**：新增 `python tools\callers.py --import <regex>`（列匹配的导入符号 ＋ 它们的 `FF 15`/`FF 25` 站点，底层 `ke_pe.imports()`）。⚠️ 只对**跨模块**调用有效；Kenshi 自己的函数在本 image 里，这条看不见。

