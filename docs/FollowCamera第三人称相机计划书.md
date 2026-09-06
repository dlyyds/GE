# FollowCamera 第三人称相机计划书

> 状态：**计划中（未开工）**
> 目标：在现有 `FollowCameraComponent` 上增加**完整第三人称跟随相机**，并支持**第一人称 ↔ 第三人称运行时切换**。
> 前置：`FollowCameraComponent` 已提供 `EyeOffset/YawSpeed/PitchSpeed/InvertY` 等字段；`Scene::UpdateFollowCamera` 已每帧把主相机钉到角色；`Camera` 已具备 `FPS/Orbit` 两套姿态与 `GetYaw/GetPitch/GetForward`；`InputState` 已有 `justPressed/GetScrollDelta`；Jolt `PhysicsSystem` 已封装在 `Physics::PhysicsWorld` 内。
> 关联：`docs/第一人称跟随角色相机计划书.md`（第一人称基线）、`GE/include/GE/Scene/Components.h`、`GE/src/Scene/Scene.cpp`、`GE/src/Scene/SceneSerializer.cpp`、`GE_Editor/src/Panels/SceneHierarchyPanel.cpp`。

---

## 0. 一句话架构

```
鼠标增量 → yaw/pitch（与现有 FPS 逻辑共用同一套更新）
滚轮     → 第三人称当前距离（Distance）
按键 V   → 第一人称 / 第三人称切换

第三人称每帧：
  target     = 角色脚底 + 绕 yaw 旋转的 TargetOffset（胸/头高锚点）
  desiredPos = target - Forward*当前距离 + Right*ShoulderOffset
  safePos    = 对 target→desiredPos 做物理射线/球体查询，取最近安全点
  camPos     = 对 safePos 做平滑阻尼
  cam.SetPosition(camPos); cam.SetYawPitch(yaw, pitch)
```

角色移动方向**仍沿用现有脚本行为**：脚本读 `camera.get_yaw()` 把 WASD 旋转到相机朝向系；第三人称只改相机位置/距离，不改变移动语义。

---

## 1. 需求

### 1.1 功能需求

- **两种视图模式**：第一人称（默认，行为与现状完全一致）、第三人称（相机在角色身后/斜后方，看向角色身上锚点）。
- **运行时切换**：默认按 `V` 在第一/第三人称之间切换；可在编辑器里改按键或关闭切换。
- **第三人称视角控制**：
  - 鼠标控制 yaw/pitch，相机围绕角色锚点转动；
  - 滚轮控制距离（zoom），在 `MinDistance~MaxDistance` 内钳位；
  - 支持过肩（shoulder）水平偏移，可配置左右肩偏移量；
  - 相机看向角色身体锚点（默认胸口/头高），横向偏移为 0 时目标位于屏幕中心。
- **相机防穿墙**：玩家背后有障碍物时相机被拉近，障碍物消失后平滑恢复到设定距离。
- **序列化**：第三人称配置随 `.scene` 落盘；旧场景缺字段时用默认值兼容。
- **编辑器面板**：可编辑模式、切换键、锚点、距离、过肩、碰撞、平滑等参数。
- **脚本 API**：提供 `camera.get_mode/set_mode/get_distance/set_distance/toggle` 等只读/控制 API。

### 1.2 非目标（初版不做）

- 相机切换过渡动画（初版硬切，切换瞬间保留 yaw/pitch；后续可加平滑过渡）。
- 侧翻转肩快捷键（初版只暴露 `ShoulderOffset` 配置，可由脚本改符号实现）。
- 第三人称瞄准辅助（aim assist）、准星瞄准线、命中判定。
- 角色模型的近平面/裁剪处理（初版仅靠 `CollisionRadius` 和近裁剪面尽量规避）。

---

## 2. 组件字段设计

在 `GE/include/GE/Scene/Components.h` 中新增枚举与字段：

```cpp
enum class FollowCameraViewMode : uint8_t {
    FirstPerson = 0,   ///< 第一人称（默认）
    ThirdPerson = 1,   ///< 第三人称
};
```

在 `FollowCameraComponent` 上扩展（`KeyCode` 需在 `Components.h` 引入 `Core/KeyCodes.h`）：

```cpp
struct FollowCameraComponent {
    // —— 总开关 ——
    bool Enabled = true;

    // —— 视图模式（可序列化）——
    FollowCameraViewMode StartMode = FollowCameraViewMode::FirstPerson; ///< Play 启动时使用的模式
    bool  ToggleEnabled = true;      ///< 是否允许运行时切换
    KeyCode ToggleKey = Key::V;      ///< 切换键（默认 V）

    // —— 共用：鼠标视角（沿用现有字段）——
    glm::vec3 EyeOffset = {0.0f, 1.65f, 0.0f}; ///< 第一人称视点偏移（角色局部系）
    float YawSpeed   = 0.10f;
    float PitchSpeed = 0.10f;
    float MinPitch   = -89.0f;
    float MaxPitch   = 89.0f;
    bool  InvertY    = false;

    // —— 第三人称 ——
    glm::vec3 TargetOffset   = {0.0f, 1.60f, 0.0f}; ///< 相机看向的角色局部锚点（脚底起抬高）
    float Distance           = 3.50f; ///< 目标距离（滚轮会运行时修改 CurrentDistance，不改此基值）
    float MinDistance        = 1.00f;
    float MaxDistance        = 12.0f;
    float ShoulderOffset     = 0.00f; ///< 过肩水平偏移（>0 右肩、<0 左肩；0 = 正中跟拍）
    bool  CollisionEnabled   = true;  ///< 是否启用相机防穿墙
    float CollisionRadius    = 0.25f; ///< 相机球体半径（卡碰撞用）
    float CollisionMargin    = 0.05f; ///< 离障碍物额外余量
    float Smoothing          = 8.0f;  ///< 位置/距离阻尼系数（越大越跟手）
    float ZoomSpeed          = 0.40f; ///< 滚轮一格改变的距离

    // —— 运行时（不参与 .scene 序列化）——
    FollowCameraViewMode CurrentMode = FollowCameraViewMode::FirstPerson; ///< Play 当前模式
    float CurrentDistance = -1.0f;   ///< 当前平滑距离；首次进入第三人称初始化为 Distance
    glm::vec3 CurrentPos  = {0,0,0}; ///< 当前相机位置（用于平滑跟随）

    FollowCameraComponent() = default;
    FollowCameraComponent(const FollowCameraComponent &) = default;
    explicit FollowCameraComponent(const glm::vec3 &eyeOffset) : EyeOffset(eyeOffset) {}
};
```

### 设计说明

- **不新增 `Camera::Mode`**：FollowCamera 继续把主相机放在 `Camera::Mode::FPS`，引擎侧手动算位置。这样 `GetYaw/GetPitch/GetForward` 语义与第一人称一致，脚本 `camera.get_yaw()`、`fps_character_demo.lua` 的 WASD 移动逻辑无需改动。
- `EyeOffset` 继续用于第一人称；`TargetOffset` 只用于第三人称，两者可独立配置。
- `CurrentMode/CurrentDistance/CurrentPos` 是运行时态，**不落盘**；每次 `Play()` 用 `StartMode/Distance` 初始化，避免运行时切换/缩放污染存档。

---

## 3. `UpdateFollowCamera` 流程改造

现有函数 `Scene::UpdateFollowCamera()` 增加以下阶段，原有第一人称路径保持等价：

```
1. 取主相机 + FollowCamera 角色（现状代码）
2. 切换处理：
   若 ToggleEnabled 且 m_InputState.JustPressed(ToggleKey)
      → CurrentMode 翻转 FirstPerson/ThirdPerson
3. 鼠标视角 → yaw/pitch（现状代码，两模式共用）
4. 按 CurrentMode 更新相机位置：
   FIRST ：cam.SetPosition(foot + rotateYaw(EyeOffset))      // 现状逻辑不变
   THIRD ：调用 UpdateThirdPersonCamera(cam, tc, fcc)
5. cam.SetYawPitch(yaw, pitch)（现状逻辑）
```

### 第三人称位置推导

以相机 yaw/pitch 得到前向 `F` 和右向 `R`：

```cpp
const glm::vec3 F = glm::normalize(glm::vec3{
    -std::cos(pitch)*std::sin(yaw),
     std::sin(pitch),
    -std::cos(pitch)*std::cos(yaw) });   // 与 Camera::GetForward() 同语义
const glm::vec3 R = glm::normalize(glm::cross(F, glm::vec3{0,1,0}));

const float yawRad = glm::radians(cam.GetYaw());
const glm::vec3 target = tc.Translation + rotateYaw(fcc.TargetOffset, yawRad);
const glm::vec3 desired = target - F * fcc.CurrentDistance + R * fcc.ShoulderOffset;
```

### Zoom（滚轮）

```cpp
if (CurrentMode == ThirdPerson) {
    fcc.CurrentDistance += m_InputState.GetScrollDelta() * fcc.ZoomSpeed;
    fcc.CurrentDistance = clamp(fcc.CurrentDistance, fcc.MinDistance, fcc.MaxDistance);
}
```

### 防穿墙与平滑

```cpp
glm::vec3 safePos = desired;
if (fcc.CollisionEnabled) {
    // 从 target 向 desired 做最近碰撞查询；忽略玩家自身碰撞体（见 §4）
    float fraction = m_PhysicsWorld->CameraRaycastClampedFraction(
        target, desired - target, length(desired - target), playerEntity);
    safePos = target + (desired - target) * fraction;
    // 沿 -F 方向让出 CollisionRadius + CollisionMargin
    const glm::vec3 inward = normalize(target - desired);
    safePos -= inward * (fcc.CollisionRadius + fcc.CollisionMargin);
}

// 位置平滑：避免碰到墙瞬间镜头跳变、离开墙后慢慢复位
const float alpha = 1.0f - std::exp(-fcc.Smoothing * dt);
fcc.CurrentPos = mix(fcc.CurrentPos, safePos, alpha);
cam.SetPosition(fcc.CurrentPos);
```

首次进入第三人称时 `CurrentPos = safePos`、`CurrentDistance = Distance`。

### Play 初始化

`Scene::Play()` 中 FollowCamera 初始化追加：

```cpp
fcc.CurrentMode   = fcc.StartMode;
fcc.CurrentDistance = fcc.Distance;
fcc.CurrentPos    = tc.Translation + fcc.EyeOffset; // 先钉身边，首帧第三人称再初始化到 safePos
```

---

## 4. 物理相机射线

### 新增接口（`Physics::PhysicsWorld`）

```cpp
/// 相机防穿墙：返回 target→direciton 方向上的最近安全 fraction [0,1]。
/// ignoreEntity 里的刚体会被排除（玩家自身不挡相机）。
float CameraRaycastClampedFraction(
    const glm::vec3 &origin,
    const glm::vec3 &direction,
    float maxDistance,
    entt::entity ignoreEntity);
```

### Jolt 实现要点

- 使用 `m_PhysicsSystem->GetNarrowPhaseQuery().CastRay(...)`，收集最近命中。
- 用 Jolt `RayCast` + `ClosestHitCollisionCollector`。
- 用 `SpecifiedBodyFilter` 忽略 `ignoreEntity` 对应刚体的 `BodyID`（若该实体有 `RigidBodyComponent.IsInitialized`）。
- `CharacterVirtual` 不是 broadphase body，玩家自身默认不会挡；`ignoreEntity` 用于兜底有刚体的玩家。
- 返回命中 fraction；未命中返回 `1.0f`。
- 初版每帧只对“挂 FollowCamera 的玩家 + 主相机”执行一次，开销可忽略。

---

## 5. 序列化

`SceneSerializer.cpp` 的 FollowCamera 段落扩写（全部带默认值，兼容旧 `.scene`）：

```cpp
fcNode["StartMode"]         = (fcc.StartMode == FirstPerson) ? "First" : "Third";
fcNode["ToggleEnabled"]     = fcc.ToggleEnabled;
fcNode["ToggleKey"]         = fcc.ToggleKey;
fcNode["TargetOffset"]      = SerializeVec3(fcc.TargetOffset);
fcNode["Distance"]          = fcc.Distance;
fcNode["MinDistance"]       = fcc.MinDistance;
fcNode["MaxDistance"]       = fcc.MaxDistance;
fcNode["ShoulderOffset"]    = fcc.ShoulderOffset;
fcNode["CollisionEnabled"]  = fcc.CollisionEnabled;
fcNode["CollisionRadius"]   = fcc.CollisionRadius;
fcNode["CollisionMargin"]   = fcc.CollisionMargin;
fcNode["Smoothing"]         = fcc.Smoothing;
fcNode["ZoomSpeed"]         = fcc.ZoomSpeed;
```

反序列化保持“缺省即默认”，并继续接受旧 `FirstPersonCamera` 节点（`FollowCamera` 优先）。

---

## 6. 编辑器面板

`SceneHierarchyPanel::DrawFollowCameraComponent` 分组展示：

- **总开关**：`Enabled`
- **模式**：`StartMode` 下拉（First/Third）、`ToggleEnabled`、`ToggleKey`
- **第一人称**：`EyeOffset`
- **第三人称**：
  - `TargetOffset`（DragVec3）
  - `Distance` + `Min/Max`
  - `ShoulderOffset`
  - `CollisionEnabled`、`CollisionRadius`、`CollisionMargin`
  - `Smoothing`、`ZoomSpeed`
- **共用鼠标**：`YawSpeed/PitchSpeed/MinPitch/MaxPitch/InvertY`
- Play 态可显示当前运行时模式（只读提示）。

---

## 7. 脚本 API

在 `ScriptEngine.cpp` 的 `camera` 表扩展读写入口：

| API | 返回值/副作用 |
|---|---|
| `camera.get_mode()` | `"first"` / `"third"` |
| `camera.set_mode(mode)` | 传入 `"first"`/`"third"`，切换 `CurrentMode` |
| `camera.toggle()` | 在第一/第三人称之间翻转 |
| `camera.get_distance()` | 当前第三人称距离 |
| `camera.set_distance(d)` | 钳位到 `[MinDistance, MaxDistance]` |
| `camera.toggle_enabled()` | 关闭/恢复切换键 |

脚本 `fps_character_demo.lua` 无需改动（它只读 `camera.get_yaw()` 旋转 WASD）。

---

## 8. 调试叠加

`DebugDrawLayer` 中的 `DrawFollowCamera` 相关标记可扩展为：

- 第一人称：保留现有 `EyeOffset` 橙色十字。
- 第三人称：
  - 角色锚点 `TargetOffset`（绿色小标记）；
  - 目标相机位置（灰色空心圈）；
  - 当前相机位置（橙色实心圈）；
  - 命中点（若发生碰撞，红色短横线）。

初版也可只保留现有眼睛标记，查看相机轨迹用日志/截图辅助验证。

---

## 9. 验收标准

1. **默认回归**：不新增配置时，`StartMode=First`，第一人称行为与改动前完全一致。
2. **切换**：Play 中按 `V` 可在第一/第三人称之间即时切换，视角 yaw/pitch 不跳变。
3. **第三人称环绕**：鼠标左右转视角，相机绕角色水平转动；上下转视角，相机在俯仰方向绕角色转动。
4. **Zoom**：滚轮在 `MinDistance~MaxDistance` 内缩放，极限处不越界。
5. **防穿墙**：角色背对墙时相机被拉近，墙消失后相机平滑回到设定距离。
6. **锚点**：`ShoulderOffset=0` 时角色锚点大致位于屏幕中心；左右肩偏移生效。
7. **序列化**：保存/加载场景后模式、距离、碰撞等配置一致；旧场景不丢组件。
8. **脚本**：`camera.set_mode("third")` 可切第三人称；WASD 移动方向仍随相机 yaw 旋转。

---

## 10. 风险与开放问题

1. **相机平滑穿透瞬时**：碰撞发生后若平滑系数低，可能帧间短暂贴墙；默认 `Smoothing=8` 可缓解。
2. **动态障碍**：射线查询的是当前帧碰撞体位置；快速移动的物体可能让相机一帧穿插，后续帧被修正。
3. **自遮挡**：角色头/肩模型过大时可能遮挡屏幕中央；初版用 `ShoulderOffset` + 碰撞半径缓解，不处理模型剔除。
4. **近平面裁剪**：第三人称离角色很近时角色模型可能被近裁剪面切开；后续可加“相机所在网格缩放/熔断”。
5. **ToggleKey 序列化**：`KeyCode` 直接落盘，若后续键位枚举变化需迁移；初版接受。
6. **性能**：每帧仅一个玩家、一条射线，Jolt 宽相位查询成本可忽略；多玩家时按玩家数量线性增长。

---

## 11. 里程碑

- **M1（姿势计算）**：第三人称位置/朝向推导、滚轮 Zoom、模式切换；`GE`/编辑器可编译运行。
- **M2（物理防穿墙）**：`PhysicsWorld::CameraRaycastClampedFraction` + 平滑阻尼；背墙/脱墙验证。
- **M3（资产与 UI）**：组件序列化、编辑器面板、`camera.*` 脚本 API、调试标记。
- **M4（回归）**：旧场景兼容、第一人称回归、`fps_character_demo.lua` 双模式验收。

---

## 12. 改动面清单

| 文件 | 改动 |
|---|---|
| `GE/include/GE/Scene/Components.h` | 新增 `FollowCameraViewMode`，扩展 `FollowCameraComponent` |
| `GE/include/GE/Core/KeyCodes.h` 关联 | `Components.h` 引入 `Core/KeyCodes.h` |
| `GE/src/Scene/Scene.cpp` | `UpdateFollowCamera` 分支、切换、第三人称位置/平滑/滚轮；`Play()` 运行时初始化 |
| `GE/include/GE/Physics/PhysicsWorld.h` | 新增相机射线接口声明 |
| `GE/src/Physics/PhysicsWorld.cpp` | Jolt `CastRay` 封装 |
| `GE/src/Scene/SceneSerializer.cpp` | FollowCamera 新字段序列化 + 默认值反序列化 |
| `GE/src/Scene/ScriptEngine.cpp` | `camera.*` 模式/距离控制 |
| `GE_Editor/src/Panels/SceneHierarchyPanel.cpp` | 面板分组编辑新字段 |
| `GE_Editor/src/DebugDrawLayer.cpp` | 第三人称锚点/相机/命中标记（可选） |
| `docs/FollowCamera第三人称相机计划书.md` | 本文档 |

---