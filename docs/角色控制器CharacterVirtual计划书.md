# 角色控制器 CharacterVirtual 计划书

> 状态：**计划中（未开工）**
> 目标：给可操控角色一套基于 Jolt `CharacterVirtual` 的控制器——自带重力积分、地面/斜坡检测、自动上楼（Stair Walk）、贴地吸附（Stick to Floor），并能推动动态体、通过 inner body 无缝接入现有碰撞事件管线（OnCollision*/OnTrigger*）。
> 前置：Jolt 已封装（`PhysicsWorld.cpp`，Fixed 60Hz + 累加器，`MAX_SUBSTEPS=5`）；碰撞事件阶段 B 已落地（ContactListener → 环形缓冲 → `Scene::StepPhysics` 派发 Lua）；`RigidBodyComponent` 三态与 Kinematic 同步链路已确认（`PhysicsWorld.cpp:685`）。Jolt 侧 `CharacterVirtual` 头/源均在 `GE/third_party/JoltPhysics/Jolt/Physics/Character/`，已确认可用。
> 关联：`物理系统增强计划书.md`（阶段 F 预留了本能力；阶段 C Edit/Play 分离落地后需一并纳入快照）、`Lua脚本系统计划书.md`（API 注入风格）、`脚本输入系统计划书.md`

---

## 0. 一句话架构

```
脚本(character.set_move/jump) → CharacterControllerComponent(WishVelocity/JumpRequested)
        │
        ▼
PhysicsWorld::UpdateCharacters 合成速度(重力积分引擎侧) → CharacterVirtual::ExtendedUpdate(每物理子步)
        │                                            └ inner body = kinematic 代理刚体(Player 层, userData=entity)
        ▼
CharacterVirtual 位置 → TransformComponent；IsGrounded → 组件回写
        ▼
inner body 接触 → 既有 ContactListener → CollisionEvent → Lua OnCollision*/OnTrigger* 免费获得
```

**与 Kinematic 的关系**：角色实体**不挂 RigidBodyComponent**，避免与 `SyncKinematicTransformsToBodies` 的 MoveKinematic 抢占位移。CharacterVirtual 自管位置，渲染读 TransformComponent。

---

## 1. 现状与关键对齐点

| 项 | 现状 | 本次改动 |
|---|---|---|
| 刚体同步 | Kinematic = Transform→Jolt `MoveKinematic`（PhysicsWorld.cpp:685） | 不动；角色走独立 CharacterVirtual 通道 |
| 碰撞事件 | ContactListener → 实体级事件 → Lua（PhysicsWorld.cpp:736 / Scene.cpp:400） | **复用**：角色靠 inner body 自动获得事件 |
| 脚本 API | `transform.*`/`input.*` 等注入、作用于 activeEntity（ScriptEngine.cpp:123-338） | 新增 `character.*` |
| 编辑器组件 | `TryAddComponent<T>` 弹窗 + `DrawComponent<T>` 面板 + Rebuild 链路（SceneHierarchyPanel.cpp:901/939/1578） | 加一枚 Character Controller，改动量小 |
| 序列化 | `.scene` 序列/反序列化组件（SceneSerializer.cpp:838/1345） | 加 CharacterControllerComponent 段落 |

**inner body 的两个已验证事实**（读 Jolt 源码确认）：
- inner body 创建时 `settings.mUserData = inUserData`（CharacterVirtual.cpp:147）→ 传 entity 即让碰撞事件免费映射回实体，`CollectCollisionEvents`（PhysicsWorld.cpp:736）无需改动。
- 析构自动 `RemoveBody + DestroyBody`（CharacterVirtual.cpp:163）→ PhysicsWorld 只存 `unique_ptr<CharacterVirtual>` 即可，生命周期简洁。

---

## 2. 组件定义（`GE/include/GE/Scene/Components.h`）

```cpp
struct CharacterControllerComponent {
    // 配置（参与 .scene 序列化）
    float Radius        = 0.35f;  ///< 胶囊半径（米）
    float Height        = 1.80f;  ///< 胶囊总高（含两端半球，米）
    float MaxSlopeAngle = 45.0f;  ///< 可上坡最大倾角（度）
    float MaxJumpSpeed  = 5.0f;   ///< 跳跃初速（m/s，character.jump 使用）

    // 运行时（不参与序列化）
    bool      IsInitialized = false;        ///< CharacterVirtual 已创建
    glm::vec3 WishVelocity  = {0, 0, 0};    ///< 脚本每帧写入的水平期望速度（引擎只看 x/z）
    bool      JumpRequested = false;        ///< 脚本置位，贴地时首个子步消费一次
    bool      IsGrounded    = false;        ///< 引擎每子步回写（贴地/斜坡/悬空）
};
```

挂载约定：实体 = `TransformComponent` + `CharacterControllerComponent`，**不挂 RigidBodyComponent**。初始 Transform.Translation 即角色「脚底」位置（CharacterVirtual 的 mPosition 语义，见 §3 形状构造）。

---

## 3. PhysicsWorld 集成（`GE/include/GE/Physics/PhysicsWorld.h` + `PhysicsWorld.cpp`）

### 3.1 成员与生命周期（镜像 RigidBody 的延迟创建模式）

```cpp
// PhysicsWorld.h 私有成员
std::unordered_map<entt::entity, std::unique_ptr<JPH::CharacterVirtual>> m_Characters;
std::vector<entt::entity> m_PendingCharacters;   ///< 延迟创建列表

// 公开方法（Scene 回调调）
void RequestCreateCharacter(entt::entity);
void DestroyCharacter(entt::entity);
void RebuildCharacter(entt::entity);   ///< 参数变化：销毁重建（编辑器面板用）
```

- `RequestCreateCharacter`：入 pending，参考 `ProcessPendingBodies`（PhysicsWorld.cpp:466）延迟到下次 `Step` 创建，取当前 Transform 作初始位置/旋转。
- `DestroyCharacter`：从 map erase → CharacterVirtual 析构自动清理 inner body。
- `RebuildCharacter`：erase → 重新 RequestCreate。

### 3.2 形状构造（胶囊，底部对齐原点）

`CharacterBaseSettings` 硬约束：**shape 底部必须在局部 `(0,0,0)`**（CharacterBase.h:48）。Jolt `CapsuleShape(halfHeightOfCylinder, radius)` 总高 = 2·柱身半高 + 2·半径：

```cpp
float cylHalf = std::max(cc.Height * 0.5f - cc.Radius, 0.0f);   // 柱身半高 = H/2 - R
JPH::CapsuleShape capsule(cylHalf, cc.Radius);
JPH::RotatedTranslatedShapeSettings shifted(
    JPH::Vec3(0.0f, cc.Height * 0.5f, 0.0f),   // 上移 H/2 → 底部落回原点
    JPH::Quat::sIdentity(), &capsule);
```

### 3.3 CharacterVirtual 配置与创建

```cpp
JPH::CharacterVirtualSettings settings;
settings.mShape = shiftedShape;                 // 上述胶囊
settings.mInnerBodyShape  = settings.mShape;    // 启用 inner body（§问题 1）
settings.mInnerBodyLayer  = (JPH::ObjectLayer)CollisionLayer::Player;
settings.mUp              = JPH::Vec3::sAxisY();
settings.mMaxSlopeAngle   = JPH::DegreesToRadians(cc.MaxSlopeAngle);
settings.mPredictiveContactDistance = 0.1f;
settings.mCharacterPadding          = 0.02f;
auto *cv = new JPH::CharacterVirtual(&settings, ToJoltVec3(tc.Translation),
                                     ToJoltQuat(tc.Rotation),
                                     (JPH::uint64)static_cast<entt::id_type>(entity),
                                     m_PhysicsSystem.get());
```

### 3.4 步进集成（`PhysicsWorld::Step`）

```cpp
while (m_Accumulator >= FIXED_TIMESTEP && subSteps < MAX_SUBSTEPS) {
    m_PhysicsSystem->Update(FIXED_TIMESTEP, 1, m_TempAllocator.get(), m_JobSystem.get());
    UpdateCharacters(FIXED_TIMESTEP);           // 新增：角色每物理子步推进
    m_Accumulator -= FIXED_TIMESTEP;
}
// …既有 CollectCollisionEvents() + SyncBodiesToTransforms()
SyncCharacterTransformsToComponents();          // 新增：CharacterVirtual 位置 → TransformComponent
```

顺序依据：角色应在物理 Update **之后**查询世界（看到动态体最新位置），与 Jolt CharacterVirtualTest 的 OnPostPhysicsUpdate 时序一致。

### 3.5 `UpdateCharacters` 速度合成（每子步、每个角色）

**重力由引擎积分**——Jolt 原文明确 "it's your own responsibility to apply gravity to the character velocity"（CharacterVirtual.h:386），脚本只负责水平输入。镜像 Jolt 示例的速度公式：

```cpp
const auto gs = cv->GetGroundState();
const JPH::Vec3 up(0,1,0);
const JPH::Vec3 vert(0, cv->GetLinearVelocity().GetY(), 0);
const JPH::Vec3 groundVel = cv->GetGroundVelocity();
JPH::Vec3 vel;
if (gs == JPH::CharacterBase::EGroundState::OnGround
    && (vert - groundVel).Dot(up) < 0.1f) {
    vel = groundVel;                            // 贴地：跟随地面速度
    if (cc.JumpRequested) { vel += up * cc.MaxJumpSpeed; cc.JumpRequested = false; }
} else {
    vel = vert;                                 // 悬空：保留垂直速度
}
vel += ToJoltVec3(m_Gravity) * dt;              // 重力（每子步）
vel += ToJoltVec3(cc.WishVelocity);             // 脚本水平输入
cv->SetLinearVelocity(vel);
cv->ExtendedUpdate(dt, ToJoltVec3(m_Gravity), extSettings,
                   *m_BroadPhaseLayerFilterObj, objFilter, sAllHit, sAllHit, *m_TempAllocator);
cc.IsGrounded = (gs == JPH::CharacterBase::EGroundState::OnGround);
cc.JumpRequested = false;                        // 未贴地也清：空中按压不缓冲，防落地自动跳
```

- `ExtendedUpdate` = Update + StickToFloor + WalkStairs 一站式（CharacterVirtual.h:451），默认 `ExtendedUpdateSettings` 即可（上楼高度 0.4m、贴地下探 0.5m、上探 0.4m）。
- 过滤：自定义 `ObjectLayerFilter` 复用现有 `EngineObjectLayerPairFilter`（Player 对 X，Trigger 不物理碰撞，与现状语义一致）；BroadPhaseLayer 过滤复用现有 `EngineObjectVsBroadPhaseLayerFilter`；Body/Shape 用 Jolt 默认 `sAllHit`。

### 3.6 `SyncCharacterTransformsToComponents`

遍历 `view<TransformComponent, CharacterControllerComponent>`，把 `GetPosition()/GetRotation()` 写回 `tc.Translation/tc.Rotation`（与 `SyncBodiesToTransforms` 同风格，PhysicsWorld.cpp:660）。渲染读 TransformComponent，auto 拿到最末子步位置。

---

## 4. 脚本 API（`GE/src/Scene/ScriptEngine.cpp`，作用于 activeEntity）

```lua
character.set_move(x, z)          -- 水平期望速度（m/s）；脚本组合 input.* 计算后每帧调用
character.jump()                  -- 贴地则起跳（速度 = MaxJumpSpeed）
character.get_grounded()          -- bool（IsGrounded 回读）
character.get_velocity()          -- 当前真实速度（可选）
character.get_ground_normal_y()   -- 地面法线 Y（可选）
```

实现：注入 `lua["character"]` 表（同 transform.* 模式，ScriptEngine.cpp:170），set_move/jump 写实体 `CharacterControllerComponent.WishVelocity/JumpRequested`，查询读回组件字段。新增 `assets/scripts/character_demo.lua` 示例：

```lua
-- character_demo.lua：WASD 平移 + 空格跳跃（垂直速度由引擎积分，脚本只管水平）
M.PUBLIC_FIELDS = { speed = { type = "number", default = 4.0 } }
function M.OnUpdate(self, ts)
    local speed = public.get("speed") or 4.0
    local dx, dz = 0, 0
    if input.is_held(Key.W) then dz = dz - 1 end
    if input.is_held(Key.S) then dz = dz + 1 end
    if input.is_held(Key.A) then dx = dx - 1 end
    if input.is_held(Key.D) then dx = dx + 1 end
    character.set_move(dx * speed, dz * speed)
    if input.just_pressed(Key.Space) then character.jump() end
end
```

注意：`set_move` 是**世界空间**水平向。若要相机相对移动，脚本按相机 yaw 旋转输入（不在本次范围，可在验收后追加）。

---

## 5. 序列化（`GE/src/Scene/SceneSerializer.cpp`）

- 序列：`CharacterControllerComponent` 段落，写 Radius/Height/MaxSlopeAngle/MaxJumpSpeed（跳过运行时字段），风格对齐 RigidBodyComponent 段落（SceneSerializer.cpp:838）。
- 反序列：`entity.AddComponent<CharacterControllerComponent>()` + 填四字段（对齐 SceneSerializer.cpp:1345）。
- 无组件场景不受影响（旧 `.scene` 向后兼容）。

---

## 6. 编辑器（`GE_Editor/src/Panels/SceneHierarchyPanel.cpp`）

- 添加组件弹窗加一行：`TryAddComponent<CharacterControllerComponent>("Character Controller");`（对齐 SceneHierarchyPanel.cpp:939）。
- 组件面板：`DrawComponent<CharacterControllerComponent>("Character Controller", entity, ...)`（对齐 SceneHierarchyPanel.cpp:901）+ `DrawCharacterControllerComponent`：Radius/Height/MaxSlopeAngle 改动调 `physicsWorld->RebuildCharacter`，MaxJumpSpeed 改动轻量同步（不改形状可不重建），风格对齐 `DrawRigidBodyComponent`（SceneHierarchyPanel.cpp:1578）。

---

## 7. 验收

- 编辑器给角色实体加 Character Controller + character_demo.lua：WASD 移动、空格起跳、悬空下落加速、落地 `get_grounded()==true`。
- 斜坡：≤45° 能走上并站稳；>45° 无法往上爬（IsSlopeTooSteep 生效）。
- inner body 生效：角色能推动 Dynamic 箱子（mMaxStrength）；射线/Overlap 能命中角色；脚本 `OnCollisionEnter` 在角色碰墙时触发（走现有管线）。
- 回归：场景内既有 Kinematic/Dynamic 行为不变（角色实体无 RigidBodyComponent，两条同步循环不受影响）。

---

## 8. 风险与开放问题

1. **inner body 是 kinematic 代理**：动态体（如头上落下的箱子）会被角色挡住而非推开——对「玩家」是期望行为；若未来要「角色不挡只推」，关 `mCanReceiveImpulses`/运动学阻挡，单开讨论。
2. **角色 vs 角色的接触**：CharacterVirtual 不在 broadphase，两角色互碰需 `CharacterVsCharacterCollision` + `CharacterContactListener`。inner body 虽能让两角色互相「挡住」，但碰撞事件要走字符间通道——**初版不做**，某侧角色之间的 OnCollision 待后续单。
3. **多子步下的跳跃消费**：`JumpRequested` 首个子步消费即清（§3.5 每子步末尾清），不会一帧双跳；空中按压不缓冲。
4. **编辑/运行分离未落地**：现有物理在编辑态也跑（物理增强计划书阶段 C 未开工），角色在编辑态同样活动——与 Dynamic 现状一致；阶段 C 落地时把角色快照/重建纳入 Play/Stop 回滚。
5. **Transform 单向性**：运行时只 CharacterVirtual→Transform；编辑器拖拽改 Transform 不会 teleport 角色，需 Rebuild（或后续加 `TeleportCharacter`）。
6. **固定步长 vs 渲染帧**：位置取最末子步，与动态体一致；渲染插值留待阶段 F。

---

## 9. 涉及文件

| 文件 | 改动 |
|---|---|
| `GE/include/GE/Scene/Components.h` | +`CharacterControllerComponent` |
| `GE/include/GE/Physics/PhysicsWorld.h` | +前向声明 / 字符容器 / 方法声明 |
| `GE/src/Physics/PhysicsWorld.cpp` | +Character 头文件、§3 实现 |
| `GE/include/GE/Scene/Scene.h` | +`OnCharacterControllerDestroyed` 声明 |
| `GE/src/Scene/Scene.cpp` | 注册 on_destroy + `OnComponentAdded` 特化 |
| `GE/src/Scene/ScriptEngine.cpp` | +`character.*` API 注入 |
| `GE/src/Scene/SceneSerializer.cpp` | 序列/反序列化 |
| `GE_Editor/src/Panels/SceneHierarchyPanel.cpp` | 添加菜单 + 面板 |
| `assets/scripts/character_demo.lua` | 示例脚本 |

## 10. 里程碑

- **M1**：PhysicsWorld 集成跑通（C++ 侧用固定输入验证移动/重力/落地）。
- **M2**：CharacterControllerComponent + 生命周期 + 编辑器面板 + 序列化。
- **M3**：`character.*` 脚本 API + character_demo.lua 示例，验收逐条过。