# 根位移（Root Motion）接入计划书

> 状态：**计划中（未开工）**
> 目标：把动画自带的根位移（根骨骼 translation 键帧）接入现有 `CharacterVirtual` 角色控制器——位移来源从键盘 `set_move` 换成动画自身，物理仍负责碰撞滑动/贴地/自动上楼，渲染与插值零改动。
> 前置：角色控制器已落地（`CharacterControllerComponent` + `PhysicsWorld::UpdateCharacters` + `ExtendedUpdate`，见 `角色控制器CharacterVirtual计划书.md`）；动画系统已落地（`AnimationSystem` 采样写局部 TRS + `Scene::UpdateWorldTransforms` DFS 重算 world，见 `骨骼动画实现计划书.md`）；蒙皮已落地（`Scene::UpdateSkins`，见 `骨骼蒙皮实现计划.md`）。
> 关联：`角色控制器CharacterVirtual计划书.md`（消费端所在）、`骨骼动画实现计划书.md`（生产端所在）、`动画状态机ASM计划书.md`（根位移角色切状态仍走现有 `character.get_velocity()` 回读，不新增 API）

---

## 0. 一句话架构

```
动画系统(渲染帧)               物理(固定子步 60Hz)             渲染
UpdateAnimations 写根骨骼局部TRS    │                          │
        │                        ▼                          │
Scene::UpdateRootMotion(新)   UpdateCharacters(改)            │
  提取局部位移增量 ──累积──▶  RootMotionDelta(信箱) ──换算速度──▶ ExtendedUpdate
  就地化:根骨骼局部归 base        （每子步消费并清零）            │
        ▼                           │ 碰撞滑动/贴地/上楼         ▼
  下帧 DFS 用清零后的值         CharacterVirtual 位置(真相) → InterpolateTransforms → Transform.Translation
```

**一句话**：位置真相始终在 CharacterVirtual；动画根位移只是**驱动输入**（转成期望速度喂进去），不是直接改位置。

---

## 1. 现状与关键对齐点

| 项 | 现状 | 本次改动 |
|---|---|---|
| 位移来源 | 脚本 `character.set_move` → `WishVelocity`（键盘 WASD） | 根位移角色改为动画驱动；`WishVelocity` 停用 |
| 物理推进 | `UpdateCharacters` 合成速度 → `ExtendedUpdate`（PhysicsWorld.cpp:612） | 在速度合成处叠加"根位移换算速度" |
| 位置归属 | CharacterVirtual 位置 → `SyncCharacterTransformsToComponents` → 渲染插值 | **不动**；根位移只喂速度，位置仍归物理 |
| 动画写姿态 | `AnimationSystem::UpdateAnimations` 写目标实体局部 TRS（AnimationComponents.h:138 `channelTargets`；AnimationSystem.cpp:380） | 采样后提取根骨骼局部增量 + 就地化（归 base） |
| 世界矩阵 | `Scene::UpdateWorldTransforms` DFS 每帧重算（Scene.cpp:745） | 就地化发生在 DFS **之前**，本帧即生效，无残帧 |
| 帧时序 | 脚本→物理→相机→动画→DFS→蒙皮→渲染（Scene.cpp:722） | 生产端插在"动画之后、DFS 之前" |
| 脚本 API | `character.*` 只读回读 `get_velocity()` 等 | 不新增；ASM 切状态沿用 velocity 回读 |
| 编辑器 | `DrawCharacterControllerComponent` 面板（SceneHierarchyPanel.cpp） | 加 UseRootMotion / RootBoneNodeIndex / ZeroRootBoneLocal 三控件 |
| 序列化 | `CharacterControllerComponent` 段落（SceneSerializer.cpp） | 加 3 个配置字段（运行时字段不入序列化） |

**两个已验证的代码事实**：
- 物理消费点已经存在天然位置：`UpdateCharacters` 速度合成末段（PhysicsWorld.cpp:652-653：重力增量 + `WishVelocity`）——根位移在这里加一行即可。
- 动画最终姿态含过渡混合（`BlendAndApplyTransition` 写回局部 TRS，AnimationSystem.cpp:357）。**读最终姿态的根骨骼局部值** = 跨 clip 混合/切状态时根位移天然连续，不需要单独解析某条 clip 通道。

---

## 2. 设计决策

### 2.1 根位移是"输入"，不是"位置"

禁止脚本直接 `Transform.Translation += rootDelta`：会被 `SyncCharacterTransformsToComponents` / `InterpolateTransforms` 覆盖，且绕过碰撞滑动/贴地/上楼。根位移**必须转成期望速度**喂给物理，物理兜底。

### 2.2 位移精确兑现：`v = Δ / FIXED_TIMESTEP`

物理积分公式 `位移 = 速度 × 时间`。根位移（米）累计在 `RootMotionDelta`，每个物理子步换算成 `v = RootMotionDelta / FIXED_TIMESTEP`，`ExtendedUpdate` 用 `dt = FIXED_TIMESTEP` 积分后：
```
实际位移 = (Δ / FIXED_TIMESTEP) × FIXED_TIMESTEP = Δ   ← 严格等于动画根位移（不被墙削之前）
```
两个 `FIXED_TIMESTEP` 一乘一除抵消，保证**物理走出的距离 = 动画根位移**（被墙/坡削去的部分正是期望的碰撞滑动）。

### 2.3 就地化（ZeroRootBoneLocal）——烘焙 vs 原地

- **原地动画**（根骨骼 translation 键帧 ≈ 0）：根位移增量 ≈ 0，就地化无副作用。
- **烘焙动画**（根骨骼 translation 键帧很大，骨架整体跟着走）：若只提取增量喂物理、不处理根骨骼局部值，骨架**视觉上又走一份** → 双重移动。
- 标准解法：提取增量后把根骨骼局部 `Translation` 归回 **base**（base = 无根位移的绑定值，默认 `{0,0,0}`），位移完全交给角色实体走。骨架钉在角色原点，动画只提供姿态 + 位移增量。

### 2.4 提取方式：局部增量（主） / 世界增量差（备）

| 方式 | 优点 | 缺点 |
|---|---|---|
| **A. 局部增量（选为主）**：读根骨骼局部 `Translation` 相对 base 的差，再绕朝向转世界 | 无双重计数、无需 DFS、无 prev 状态、就地化零残帧 | 假设根骨骼局部 XZ ≈ 角色水平（模型已转正）；倾斜根需换旋转换算 |
| B. 世界增量差：`根骨骼世界增量 − 角色实体世界增量` | 无轴假设，读最终世界姿态 | 需两份 prev 世界状态；就地化发生在 DFS 后会造成一帧残影，须把就地化挪到 DFS 前又与"世界增量"自洽冲突 |

选 A。倾斜根（Z-up 转正等）的旋转换算降级为"父节点世界旋转 × 局部增量"（用上帧 DFS 结果，滞后一帧无感），列风险 §11.4。

### 2.5 根位移方向模式：全根位移 / 半根位移

根位移水平移动的**方向归谁**，分两种互斥模式（`RootDirMode`，参与序列化）：

| 模式 | 方向来源 | 速率来源 | 键盘对移动 | 无输入时 | 转向 |
|---|---|---|---|---|---|
| **全根位移** `Anim`（默认） | 动画 | 动画 | 无效 | 照常走 | 需 root yaw / 相机通道（M3） |
| **半根位移** `Input` | 输入（WishVelocity） | 动画 | 决定方向 | 停下 | `FaceMovement` 自动 |

- **全根位移**：过场、技能、击退、爬墙等"动画牵着走"的场景；`WishVelocity` 被忽略。
- **半根位移**：日常方向自由操控的移动；**速率取 `|RootMotionDelta|`，方向取 `normalize(WishVelocity)`**，且 `FaceMovement` 照常工作（方向=输入），顺带解决"根位移角色没有转向驱动"的问题（§11.5 的坑自然消失）。
- 两模式前提一致：**动画必须真带根位移**（烘焙键帧）；原地动画 `|Δ|≈0`，半根位移模式下角色原地踏步——原地动画请沿用键盘 `set_move`（§11.6）。

**模式归属（M4 升级路径）**：严格说，全/半/无根位移是**动画状态**的属性，不是整个实体的属性——同一角色日常走路想半跟、被击退想全跟、待机想无根位移。M1/M2 用实体级 `RootDirMode` 当默认值；M4 把 `AnimStateDef.rootMotionMode`（None/Anim/Input）纳入，`EnterState` 时写入 `cc->UseRootMotion`/`cc->RootDirMode` 运行时值（与既有 `loop`/`speed` 进状态写入同构，AnimationComponents.h:213），消费端只读运行时值，实体字段退化为默认/兜底。详见 §7 与 §13 M4。

---

## 3. 组件定义新增字段（`GE/include/GE/Scene/Components.h`）

```cpp
struct CharacterControllerComponent {
    // …既有字段不动…

    // ---- 新增：根位移（配置，参与 .scene 序列化）----
    bool  UseRootMotion     = false;  ///< 总开关：开着则水平位移改由动画根位移驱动（方向模式见 §2.5）
    int   RootBoneNodeIndex = -1;     ///< 承载根位移的骨骼 glTF nodeIndex（与 AnimationChannel.nodeIndex 同系；-1=未配置）
    bool  ZeroRootBoneLocal = true;   ///< 就地化：提取后把根骨骼局部 Translation 归 base（烘焙动画防双倍移动）

    /// 根位移水平方向模式（§2.5）：Anim=全根位移（方向也来自动画）；Input=半根位移（方向来自 WishVelocity）。
    /// 实体级字段 = 默认值；M4 起可被 ASM 状态进入时覆盖（AnimStateDef.rootMotionMode）或脚本 character.* 覆盖。
    enum class RootMotionDir : uint8_t { Anim = 0, Input = 1 };
    RootMotionDir RootDirMode = RootMotionDir::Anim;

    // ---- 新增：根位移（运行时，不参与序列化）----
    glm::vec3 RootMotionDelta  = {0,0,0}; ///< 信箱：动画系统每渲染帧累加水平位移（米），物理子步消费并清零
    glm::vec3 RootBoneBaseLocal = {0,0,0}; ///< 根骨骼无根位移的绑定局部位置（就地化归位用；默认原点）
};
```

- `RootBoneNodeIndex` 用 glTF nodeIndex 而非实体句柄：nodeIndex 由源文件解析、跨序列化稳定；实体句柄序列化不稳定。
- `RootMotionDelta` / `RootBoneBaseLocal` 为运行时字段，沿既有 `IsGrounded`/`WishVelocity` 等"运行时不入序列化"惯例（组件头注释已声明，见 Components.h:617）。

---

## 4. 生产端：`Scene::UpdateRootMotion`（新增）

### 4.1 调用位置

插在 `Scene::OnUpdate3DSimulation`（Scene.cpp:722）的 `UpdateAnimations(ts)` 与 `UpdateWorldTransforms()` **之间**：

```cpp
void Scene::OnUpdate3DSimulation(Timestep ts) {
    // …①脚本 ②物理 ③跟随相机…
    UpdateAnimations(ts);           // 现有：采样写各骨骼局部 TRS
    UpdateRootMotion();             // 新增：提取根位移增量 + 就地化（DFS 之前，本帧即生效）
    UpdateWorldTransforms();        // 现有：DFS 重算 world（读到的是就地化后的值）
    UpdateSkins();
}
```

### 4.2 根骨骼解析（nodeIndex → 实体）

根骨骼实体 = 当前 active clip 里 `nodeIndex == RootBoneNodeIndex` 的通道对应的 `channelTargets[ci]`（AnimationComponents.h:138）。量级为几条通道，每帧解析可忽略；同骨架各 clip 目标实体一致，过渡期取 active clip 即可：

```cpp
static entt::entity ResolveRootBone(entt::registry &reg, entt::entity e, int nodeIndex) {
    auto *ac = reg.try_get<AnimationComponent>(e);
    if (!ac || ac->clips.empty()) return entt::null;
    const auto &inst = ac->clips[ac->active];
    const auto *clip = inst.clip.get();
    if (!clip) return entt::null;
    for (size_t ci = 0; ci < clip->channels.size(); ++ci) {
        if (clip->channels[ci].nodeIndex == nodeIndex && ci < inst.channelTargets.size())
            return inst.channelTargets[ci];
    }
    return entt::null;
}
```

### 4.3 提取 + 就地化

```cpp
void Scene::UpdateRootMotion() {
    auto view = m_Registry.view<TransformComponent, CharacterControllerComponent>();
    for (auto entity : view) {
        auto &cc = view.get<CharacterControllerComponent>(entity);
        if (!cc.UseRootMotion)
            continue;

        const entt::entity rootBone = ResolveRootBone(m_Registry, entity, cc.RootBoneNodeIndex);
        auto *rootTC = m_Registry.try_get<TransformComponent>(rootBone);
        if (rootTC == nullptr)
            continue; // 未配置/未解析：跳过（可 GE_CORE_WARN 一次）

        // ① 提取：动画刚写入的局部 Translation 相对 base 的增量 = 纯动画根位移（局部系）
        const glm::vec3 localDelta = rootTC->Translation - cc.RootBoneBaseLocal;

        // ② 转世界：按角色当前朝向旋转（M1 假设根骨骼局部 XZ ≈ 角色水平，见 §11.4）
        const glm::vec3 up(0.0f, 1.0f, 0.0f);
        const glm::vec3 worldDelta = glm::angleAxis(cc.FacingYaw, up) * localDelta;

        // ③ 累加进信箱（只取水平；y 交给引擎重力）
        cc.RootMotionDelta.x += worldDelta.x;
        cc.RootMotionDelta.z += worldDelta.z;

        // ④ 就地化：根骨骼局部归 base → 骨架钉在角色原点，位移只由角色实体走
        if (cc.ZeroRootBoneLocal)
            rootTC->Translation = cc.RootBoneBaseLocal;
    }
}
```

`RootBoneBaseLocal` 默认 `{0,0,0}`；若资产根骨骼带常量偏移（非原点绑定），首次运行可从该骨骼在 clip t=0 的根通道采样值刷新（`SampleVec3Channel`，AnimationSystem.h:27），M2 兜底。

### 4.4 旋转（可选，M3）

若动画根骨骼还带旋转键帧（root yaw），把局部旋转相对 base 的 yaw 增量累计进 `cc->FacingYaw`，并设 `FaceMovement = false`（否则自动转向与动画抢朝向）。初版**不做**，见风险 §11.5。

---

## 5. 消费端：`PhysicsWorld::UpdateCharacters`（改，`GE/src/Physics/PhysicsWorld.cpp`）

在速度合成末段（PhysicsWorld.cpp:652-653，重力增量之后、`ExtendedUpdate` 之前）改为：

```cpp
vel += ToJoltVec3(m_Gravity) * cc->GravityScale * dt;   // 重力（每子步，原有）

if (cc->UseRootMotion) {
    const glm::vec2 rm(cc->RootMotionDelta.x, cc->RootMotionDelta.z); // 动画根位移（水平）
    const float rmLen = glm::length(rm);                              // 速率 |Δ|
    if (cc->RootDirMode == CharacterControllerComponent::RootMotionDir::Input) {
        // 半根位移：速率给动画、方向给输入（FaceMovement 顺带脸朝输入方向）
        const glm::vec2 wish(cc->WishVelocity.x, cc->WishVelocity.z);
        const float wishLen = glm::length(wish);
        if (wishLen > 0.01f && rmLen > 1e-6f) {
            vel.x += (wish.x / wishLen) * (rmLen / FIXED_TIMESTEP);   // 方向=输入
            vel.z += (wish.z / wishLen) * (rmLen / FIXED_TIMESTEP);   // 大小=动画
        }
        // 无输入 → 不叠加（停下；要"无输入仍被动画推着走"用全根位移兜底，§11.6）
    } else {
        // 全根位移：方向 + 速率都来自动画
        vel += ToJoltVec3(cc->RootMotionDelta) / FIXED_TIMESTEP;
    }
    cc->RootMotionDelta = {0.0f, 0.0f, 0.0f};                         // 消费即清零
} else {
    vel += ToJoltVec3(cc->WishVelocity);                              // 原有：键盘水平输入
}
cv->SetLinearVelocity(vel);
cv->ExtendedUpdate(dt, ToJoltVec3(m_Gravity), extSettings, …);
```

- **贴地分支顺序不变**：`vel = groundVel`（整体替换）→ 重力 → 根位移。根位移叠加在移动平台地面速度之上 → "平台带 + 自身走"正确合成。
- **竖直始终归引擎重力**：根位移只喂 x/z，跳跃/下落手感不变，`GravityScale`/`MaxJumpSpeed` 照常生效。
- `FIXED_TIMESTEP = 1/60`（PhysicsWorld.h:293）。清零点在物理子步（引擎侧），不在渲染帧——这正是与"渲染帧直接 set_move"的帧率错位区分开的关键。
- `SyncCharacterTransformsToComponents` / `InterpolateTransforms` **零改动**：位置仍按 m_PreviousPhysicsPosition → m_PhysicsPosition → alpha 插值。

---

## 6. 时序与帧率对齐

- **渲染帧**（可能 120/144Hz）：`UpdateRootMotion` 把本帧根位移增量累进 `RootMotionDelta`。
- **物理子步**（固定 60Hz，`MAX_SUBSTEPS=5`）：`UpdateCharacters` 首个子步消费并清零。
- **一帧滞后**：帧 N 动画累积的增量 → 帧 N+1 的 `StepPhysics` 才被物理消费。被 `m_PreviousPhysicsPosition/m_PhysicsPosition + alpha` 插值抹平，视觉无感。
- **帧率错位安全**：若某帧物理没跑子步（累加器不够），增量攒着，下一个子步一次性兑现，**距离始终精确**（只可能更"块状"，由插值平滑）。
- **卡顿安全**：渲染增量持续累积、物理按 60Hz 消耗 → 总位移 = 总动画位移，不漂移。

---

## 7. 与 ASM / 键盘输入的关系

- 根位移角色的键盘输入按模式分工：**全根位移**忽略 `WishVelocity`（脚本停调 `set_move` 防残留）；**半根位移**脚本**继续调 `character.set_move`**——引擎消费端用它的**方向**、动画给**速率**（§2.5/§5）。`FaceMovement` 半根位移下照常生效（方向=输入）；全根位移下因 `WishVelocity` 被忽略而失去转向驱动（§11.5，M3 补 root yaw / 相机通道）。
- 模式可随 ASM 状态切换（M4）：`AnimStateDef.rootMotionMode`（None/Anim/Input）在 `EnterState` 时写入 `cc->UseRootMotion`/`cc->RootDirMode` 运行时值——日常状态半跟、受击/技能状态全跟、待机状态无根位移，作者在编辑器 ASM 状态面板配，脚本不用每帧管。切换瞬间的过渡混合期沿用目标状态的模式（与 §7"取 active clip 解析根骨骼"一致）。
- ASM 切状态（idle/walk/run）**沿用现有回读**：`character.get_velocity()` 返回物理真实速度（含根位移驱动后的速度），`anim.set("speed", hspd)` 不变（见 `character_asm_demo.lua:40`）——顶墙被挡时真实速度≈0 → 动画回 idle，行为与键盘驱动一致。
- 跳跃/贴地：`character.get_grounded()` 照常回读，根位移不影响重力/贴地判定。

---

## 8. 编辑器（`GE_Editor/src/Panels/SceneHierarchyPanel.cpp`）

在 `DrawCharacterControllerComponent` 面板（对齐现有 Draw 风格）加：
- **Use Root Motion** 复选框 → `cc.UseRootMotion`（即时生效，无需重建）。
- **Root Bone Node Index** 整数输入 → `cc.RootBoneNodeIndex`（-1 = 未配置；可加提示"填模型根骨骼的 nodeIndex"）。
- **Zero Root Bone Local** 复选框 → `cc.ZeroRootBoneLocal`。
- **Root Dir Mode** 下拉 → `cc.RootDirMode`（Anim 全根位移 / Input 半根位移，§2.5）。

改动均**不触发 `RebuildCharacter`**（只影响 `Scene::UpdateRootMotion` 生产端，不动物理胶囊形状）。

---

## 9. 序列化（`GE/src/Scene/SceneSerializer.cpp`）

`CharacterControllerComponent` 段落新增四字段：`UseRootMotion` / `RootBoneNodeIndex` / `ZeroRootBoneLocal` / `RootDirMode`（枚举按整数序列化）；运行时字段（`RootMotionDelta`/`RootBoneBaseLocal`）不入序列化。旧 `.scene` 无此段落字段 → 反序列化用默认值（UseRootMotion=false），向后兼容。

---

## 10. 验收

- 烘焙动画角色：挂 Character Controller + AnimationComponent + `UseRootMotion=true` + 填对 nodeIndex → 角色按动画根位移行走，**骨架不双倍移动**（就地化生效）。
- 碰撞：根位移顶着墙走 → 角色沿墙滑动、不穿模；`get_velocity()` 水平≈0 → ASM 回 idle。
- 台阶/斜坡：根位移被台阶挡住 → `ExtendedUpdate` 自动上楼；下坡 → 贴地吸附，无浮空。
- 精确性：无碰撞空地上走 N 秒，角色位移 ≈ 动画根位移累计（±1 子步内）。
- 半根位移：按 WASD → 角色沿输入方向走、步幅速度跟动画走；松开 → 停下；`get_velocity()` 水平≈动画速率、方向≈输入；脸朝输入方向（FaceMovement）。
- 全根位移：键盘无效，角色严格按动画根位移走（过场式）；有键盘输入也不偏航。
- 回归：`UseRootMotion=false` 角色行为与现状完全一致（键盘驱动不受影响）；渲染插值/相机跟随无抖动。

---

## 11. 风险与开放问题

1. **脚部滑步（根位移固有代价）**：被墙/坡顶住时"脚在走、身体没动"。这不是 bug；想消除需脚部 IK 或"受阻停播移动动画"，**初版不做**。
2. **根骨骼解析依赖 active clip 的目标集**：跨 clip 混合时不同 clip 若目标集不一致（同骨架多 clip 目标集一致，决策 9.3 已保证），根骨骼实体可能不同 → 就地化可能作用错实体。已由"同骨架目标集一致"的既有约定覆盖，异常模型留待观察。
3. **base 捕获**：默认 `{0,0,0}`，带常量偏移的资产首次运行可能"沉入地板"或"抬高一截"。M2 用根通道 t=0 采样刷新 base 兜底。
4. **根骨骼局部轴 ≠ 角色水平**（Z-up 模型转正、根骨骼倾斜）：M1 按"已转正、局部 XZ ≈ 世界水平"假设；不满足时旋转换算换"父节点世界旋转 × 局部增量"（上帧 DFS 结果），或退到世界增量差方案 B。
5. **根位移旋转（root yaw）**：初版只做平移、旋转仍由 `FaceMovement`/相机驱动；动画带 root yaw 时会有"转向与朝向打架"现象，M3 补 yaw 通道。
6. **半根位移的边界**（方向模式已正式纳入 §2.5/§5，此处列剩余风险）：①原地动画（`|Δ|≈0`）在半根位移下原地踏步——该模式本就要求动画带根位移，原地动画请沿用键盘 `set_move`；②侧向/斜向 clip（横移类）被强制成输入方向会"月球漫步"，缓解是把根位移投影到模型前向轴只取前进速率；③无输入时半根位移不动，若需"没输入仍被动画推着走"（过场/被击退），用全根位移兜底。
7. **`RootMotionDelta` 在编辑态**：物理 Edit/Play 分离已落地（阶段 C），停态不步进 → 信箱只累不消费，Play 起始 `ResetAccumulator` 时需同步清空各角色 `RootMotionDelta`，防上次 Play 残留。

---

## 12. 涉及文件

| 文件 | 改动 |
|---|---|
| `GE/include/GE/Scene/Components.h` | `CharacterControllerComponent` +5 字段（§3） |
| `GE/include/GE/Scene/Scene.h` | +`UpdateRootMotion` 声明 |
| `GE/src/Scene/Scene.cpp` | +`Scene::UpdateRootMotion`（§4）+ 插入 `OnUpdate3DSimulation` 时序 |
| `GE/src/Physics/PhysicsWorld.cpp` | `UpdateCharacters` 消费端（§5）+ 停态清信箱 |
| `GE/src/Scene/SceneSerializer.cpp` | 序列/反序列化 3 配置字段（§9） |
| `GE_Editor/src/Panels/SceneHierarchyPanel.cpp` | +3 控件（§8） |
| `GE/include/GE/Scene/AnimationComponents.h` | +`AnimStateDef.rootMotionMode`（M4） |
| 编辑器 ASM 状态面板 | 状态节点加 rootMotionMode 配置（M4） |
| `assets/scripts/character_asm_demo.lua` | （可选）根位移示例角色的 ASM 样例 |

## 13. 里程碑

- **M1（生产端 + 消费端跑通）**：`UpdateRootMotion` 提取/就地化 + `UpdateCharacters` 消费；C++ 侧用一个带根位移键帧的模型验证"位移精确、无双倍移动、顶墙滑动"。
- **M2（配置 + 编辑器 + 序列化）**：4 配置字段面板/落盘（含 `RootDirMode` 半根位移模式）+ base 捕获兜底；验收逐条过。
- **M3（可选增强）**：root yaw 通道、倾斜根旋转换算兜底。
- **M4（模式随状态走）**：`AnimStateDef.rootMotionMode`（None/Anim/Input）+ `EnterState` 写运行时 `UseRootMotion`/`RootDirMode` + 编辑器 ASM 状态面板配置；消费端只读运行时值，实体字段退化为默认值。
