# Lua 脚本系统计划书（sol2 · 文件脚本 · 可序列化 · 编辑器接入）

> 状态：**阶段 A/B/C 已完成**（阶段 A：Lua 运行时端到端；阶段 B：序列化 + 编辑器接入；阶段 C：API 补齐 + 限频禁用 + 热重载收口 + public 字段 C4）。前置已完成：ECS（EnTT）、现有 `ScriptComponent`（std::function 回调框架）、`Scene::OnUpdate3D` 时序、`SceneSerializer`（YAML）、`SceneHierarchyPanel` 组件面板、`on_destroy` 组件监听模式（物理清理在用）。
> 目标：把当前「C++ 手写 lambda、无法序列化、编辑器无法挂载、零实际使用」的脚本占位框架，升级为**真正可用的 Lua 脚本系统**——脚本是独立的 `.lua` 文件、挂到实体、可保存加载、可在编辑器里管理、可热重载、坏脚本不拖垮引擎。
> 定位：纯场景层/玩法层特性，**渲染层与着色器零改动**；三个阶段各自独立可回退，每阶段结束可编译可运行。

---

## 0. 一句话架构

```
实体 ──► ScriptComponent{ScriptPath, Enabled} ──► Scene::ScriptEngine（共享 sol::state）
                                                       │
                 dofile(路径) 按路径缓存 ──► 行为表（OnCreate / OnUpdate / OnDestroy）
                 每实体建实例表（metatable→行为表）──► 每实体独立状态字段
                 热重载 = 清缓存重读文件 → 重建实例、重跑 OnCreate
```

`ScriptComponent` 只存数据（脚本路径 + 开关），运行时逻辑全部收进 `ScriptEngine` 管理器（Scene 持有，仿 `PhysicsWorld`/`AnimationClipManager` 的归属方式）；Lua 通过一个共享的 `sol::state` 注入固定 API；每实体一份"实例表"保存独立状态，行为函数（含每帧 `OnUpdate`）从共享行为表通过 metatable 继承。输入不进事件分发——Lua 与 C++ 脚本同源，在 `OnUpdate` 内经注入的 `input.*` 查询场景输入快照。

---

## 1. 现状盘点与差距定位

| 能力 | 现状 | 本次改动 |
|---|---|---|
| 脚本语言 | C++ 手写 `std::function` lambda，无法外部编写 | **Lua 5.4 + sol2**，脚本是独立文件 |
| 序列化 | `SceneSerializer.cpp:849/1277` 注释「std::function 无法持久化」被跳过 | 只落 `ScriptPath` + `Enabled` 两个字段 |
| 编辑器挂载 | `DrawAddComponentPopup`（`SceneHierarchyPanel.cpp:498`）14 种组件无 Script | 加 Script 项 + 属性面板完整编辑 UI |
| 实际使用 | 代码库 `AddComponent<ScriptComponent>` 零匹配 | 端到端验证 + `assets/scripts/` 示例脚本 |
| 热重载 | 无（改逻辑要重编整个引擎） | Reload 按钮 + `Ctrl+R` 全局重载 |
| 输入到达脚本 | 已实现：`Scene::OnEvent`（`Scene.cpp:852`）写 `InputState` 快照，C++ 脚本 `OnUpdate` 内查询 | Lua 同源：注入 `input.*` 查询 API，无独立事件回调 |
| 错误隔离 | 无（C++ lambda 崩一次就崩全场） | `sol::protected_function` + 日志 + 跳过实例 + 限频禁用 |

本次做上表全部行；其余能力（public 字段反射、动画事件订阅等）在 §4 路线图定位，不提前开工。

---

## 2. 总体设计

### 2.1 `ScriptComponent` 重构 —— 纯数据，可序列化

输入快照改造后 `ScriptComponent` 已收敛为 `{OnUpdate, Enabled}`（六个输入回调已删除），本次再移除 `OnUpdate`，只留序列化载荷：

```cpp
struct ScriptComponent {
    std::string ScriptPath;   ///< assets/scripts/ 下相对路径（含 .lua 后缀）；唯一序列化载荷
    bool Enabled = true;
};
```

- 空路径 = 未挂脚本，Scene 直接跳过。
- `OnUpdate` 移交 `ScriptEngine` 接管（每帧生成 `Timestep` 调 Lua），组件不再持有 `std::function`。动画事件回调（`AnimationComponent::eventCallback`）属另一语义，见路线图 D-2 衔接。

### 2.2 `ScriptEngine` —— 运行时管理器

新增 `GE/include/GE/Scene/ScriptEngine.h` + `GE/src/Scene/ScriptEngine.cpp`。Scene 在构造时持有 `ScriptEngine m_ScriptEngine;`，生命周期随 Scene（拷贝/移动与 `PhysicsWorld` 同类处理）。

```cpp
class ScriptEngine {
public:
    void Init(const std::string &scriptsBaseDir);      // 建 sol::state、注入 API、设 package.path（assets/scripts/）
    void Shutdown();                                   // 清实例/行为缓存（随 Scene 析构）

    void OnComponentAdded(Entity);                     // Scene::OnComponentAdded<ScriptComponent> 调：挂载/换路径
    void OnUpdate(Timestep, Entity);                   // 每帧（ScriptEngine 持 Scene*，input.* 查询经快照）
    void OnEntityDestroyed(entt::entity);              // 清理实例 + 调 OnDestroy

    void Reload(const std::string &path);              // 热重载单脚本（清缓存 + 重建使用它的实例）
    void ReloadAll();                                  // 热重载所有已加载脚本

private:
    sol::state m_Lua;                                  ///< 全场景共享一个 Lua 状态
    std::map<std::string, sol::table>  m_Behaviors;    ///< 路径 → 行为表缓存
    std::map<entt::entity, sol::table> m_Instances;    ///< 实体 → 实例表（独立状态）
    std::string m_BaseDir;                             ///< assets/scripts/
    Scene      *m_Scene = nullptr;                     ///< 反查输入快照等场景级状态（Init 注入）
    // …EnsureBehavior / MakeInstance / CallHook 等私有实现
};
```

**行为表 / 实例表机制（metatable 单继承）**：

- **行为表**：`EnsureBehavior(path)` 对每个脚本文件 `dofile` 一次并缓存。约定脚本顶层返回一个 table，可选字段 `OnCreate(self, entity)` / `OnUpdate(self, ts)` / `OnDestroy(self)`。
- **实例表**：`MakeInstance(path)` 新建空 table 并 `setmetatable(instance, { __index = behavior })`。于是实例上的变量赋值落在实例表（**每实体独立状态**），读函数/未赋值字段经 `__index` 走共享行为表 —— 即「class + instance」。
- **隔离**：所有调用走 `sol::protected_function_result`（异常捕获）；出错 → `GE_CORE_ERROR` 带脚本名/行号 → **本帧跳过该实例**，其它脚本照常。连续出错计数达阈值（如 3 次）→ 自动 `Enabled=false` 并提示（防刷屏，见 2.6/阶段 C）。

### 2.3 Lua API（注入接口）

| 域 | 函数 | 说明 |
|----|------|------|
| `transform` | `get_translation() → x, y, z` | 读 `TransformComponent.Translation` |
| | `set_translation(x, y, z)` | 无 Transform 时 log.error 并返回 false |
| | `get_rotation() → degX, degY, degZ` | quat→欧拉（度数，与编辑器一致） |
| | `set_rotation(degX, degY, degZ)` | 欧拉→quat |
| | `get_scale() / set_scale(x,y,z)` | `TransformComponent.Scale` |
| `input` | `is_held(code)` / `is_mouse_down(btn)` | 读 `Scene::GetInputState()` 快照（与 C++ 脚本同帧，非 `GEInput` 实时态） |
| | `just_pressed(code)` / `just_released(code)` | 单帧边沿 |
| | `mouse_pos() → x, y` / `mouse_delta() → x, y` | 视口坐标 / 帧间增量 |
| | `scroll() → n` | 本帧滚轮累计 |
| `log` | `info(w)` / `warn(w)` / `error(w)` | → spdlog |
| `entity` | `get_tag() → string` | `TagComponent` |
| | `has_component("MeshRenderer") → bool` | 字符串→组件名查表 |
| `public` | `get(name)` | 读本实体 `ScriptComponent.PublicFields`（按字段类型返回 number/bool/string；未声明→nil，设计见 9.11） |

**MVP 硬决策**：
- **不绑定 glm vec3/flags**——位置/欧拉角用多返回值 `(x,y,z)`，避免引入 vec3 userdata 与 glsl 混淆（路线图 D 再做数学类型）。
- **不暴露 entt 注册表/裸指针**——v1 脚本不创建/销毁实体、不跨实体改组件（路线图 D-3 补）。脚本只作用于**挂载它的实体**。

### 2.4 输入接入 —— 查询快照，不设事件回调

输入快照改造（`docs/脚本输入系统计划书.md`）后，`Scene::OnEvent`（`Scene.cpp:852`）不再向脚本分发输入、只写 `InputState` 快照。Lua 与 C++ 脚本同源：`ScriptEngine` 持 `Scene*`，注入的 `input.*` C 函数读 `Scene::GetInputState()`，脚本在 `OnUpdate` 里查询：

```lua
function M.OnUpdate(self, ts)
    local dt = ts
    if input.is_held(Key.W)            then move_forward() end  -- 按住
    if input.just_pressed(Key.Space)   then log.info("jump") end -- 单帧边沿
    if input.mouse_delta() ~= 0        then turn() end
end
```

- 无 `OnInput`、无消费短路（现有引擎已无该语义）；脚本间无先后、同帧同输入。
- `Key.*`/`Mouse.*` 常量由引擎以字符串键码表注入（与 `Core/KeyCodes.h` 一致）。
- 帧首 `m_InputState.BeginFrameInput()`（`Scene.cpp:508`）先于 `UpdateScripts`，Lua 拿到的 `held/just_pressed` 天然是**本帧**语义。

### 2.5 序列化

- **写**（`SceneSerializer.cpp` 每个实体节点内）：
  ```yaml
  Script:
    ScriptPath: scripts/mover.lua
    Enabled: true
  ```
- **读**：组件创建后由 `Scene::OnComponentAdded<ScriptComponent>` 调 `m_ScriptEngine.OnComponentAdded(entity)` 预加载行为表。脚本文件缺失 → `GE_CORE_WARN`，组件保留但 `Enabled=false`（不打断加载流程）。
- **兼容**：旧场景无 `Script` 键 → 跳过（现状行为，无回退问题）。

### 2.6 错误处理与热重载语义

- **错误隔离**：`protected_function` 捕获 + 日志；坏实例本帧跳过；连续报错 3 次自动禁用（限频）。
- **热重载**：`Reload(path)` = 清 `m_Behaviors[path]` → 对每个使用它的实例：先 `OnDestroy`，改绑行为表新地址，再重跑 `OnCreate`。**实例已有字段保留**（玩家位置/计数器不清零），行为变化即时生效。未加载的脚本无需动作。
- **生命周期**：实体销毁（`Scene::DestroyEntity`，`Scene.cpp:148`）→ `m_Registry.destroy(handle)` 触发 `on_destroy<ScriptComponent>`（仿 `Scene.cpp:129` 物理清理的既有模式）→ `ScriptEngine::OnEntityDestroyed` 调 `OnDestroy` 并移除实例。

---

## 3. 分阶段实施

### 阶段 A：Lua 运行时端到端打通（已完成）

**目标**：示例脚本在编辑器视口内每帧驱动实体、输入回调可用、坏脚本不崩引擎。

**A1. vendored 集成**：Lua 5.4 源码拷入 `GE/third_party/lua/`；sol2（≥3.3）拷入 `GE/third_party/sol2/`；`CMakeLists.txt` 把 Lua `*.c` 并入 `GE_SRC` 编译，`target_include_directories(GE PUBLIC .../sol2/include)`。

**A2. `ScriptEngine` 骨架**：`Init(Scene*, baseDir)`（state + 注入 `transform/log/input` API + `package.path`）、`EnsureBehavior`、`MakeInstance`、`OnUpdate`。

**A3. 接线 Scene**：构造 `m_ScriptEngine` 并 `Init("assets/scripts/")`；`UpdateScripts`（`Scene.cpp:545`）改调 `OnUpdate`；`OnComponentAdded<ScriptComponent>` 挂载；`on_destroy<ScriptComponent>` 清理。

**A4. 示例脚本** `assets/scripts/mover.lua`：WASD 平移 + R 旋转 + 空格日志。

**退出标准**：
- 给任意实体挂 mover.lua，编辑器视口内按键位移/旋转即时生效，欧拉换算正确（旋转后 GetLocalMatrix 与手动赋值一致）。
- 语法/运行时错误的脚本 → `GE_CORE_ERROR` 且编辑器**不崩**，其余实例照常。
- 无 Script 组件的既有场景输入/渲染行为与改造前逐帧一致。

---

### 阶段 B：序列化 + 编辑器接入（已完成）

**目标**：脚本成为场景资产一部分，编辑器可视化挂载/管理。

**B1. YAML 读写**（见 2.5）：写 `Script` 键；读预留加载。旧文件缺键照常。

**B2. AddComponent**：`DrawAddComponentPopup`（`SceneHierarchyPanel.cpp:498`）加 `TryAddComponent<ScriptComponent>("Script")`。

**B3. 属性面板** `DrawScriptComponent`：路径文本输入 + 「浏览…」（弹窗列 `assets/scripts/*.lua`）+ `Enabled` 勾选 + 「Reload」按钮 + 状态行（脚本名 / 已加载 / 最近错误）。

**B4. 全局重载**：编辑器 `Ctrl+R` + 顶部菜单项 → `m_ScriptEngine.ReloadAll()`。

**退出标准**：
- 保存→加载后脚本保留、**不重复**创建实例（`m_Instances` 无泄漏）、照常运行。
- 面板改路径即时重挂（旧实例卸载 + 新脚本重载）；填不存在路径 → warn + `Enabled=false`，不崩。
- 既有场景保存→加载（无脚本）行为不回退。

---

### 阶段 C：API 补齐 + 稳健性（已完成）

**目标**：脚本能写更真实玩法逻辑，沙箱可靠。

**C1. API 补齐**：`input` 全量（鼠标位置）、`entity.get_tag` / `entity.has_component`（组件名映射表）。

**C2. 限频禁用**：连续报错 3 次 → 自动 `Enabled=false` + 面板提示（防刷屏）。

**C3. 热重载语义收口**：`Reload` 保留实例字段、只重跑 `OnCreate`（先 `OnDestroy`），行为见 2.6；文档明示「重载 = 换逻辑不换状态」。

**C4. public 字段编辑（已完成，非 D-1）**：脚本声明 `PUBLIC_FIELDS = { speed = {type="number", default=2.0} }`（支持 `number/bool/string`）→ 面板按声明生成输入框，值存进 `ScriptComponent.PublicFields` 随场景序列化。运行时脚本经注入的 `public.get(name)` 实时读**本实体**组件值（读保存值，不并入实例表，重载/面板改动天然即时生效），设计见 **9.11**。

**退出标准**：
- 脚本能读鼠标状态、查询自身组件；public 字段（若做）面板改动即时生效并落盘。
- 热重载：改脚本 → Ctrl+R → 行为变化即现，实例字段保留。
- 持续报错脚本被限频/禁用，日志不刷屏，编辑器长期运行稳定。

---

## 4. 后续路线图（本计划范围外）

不提前开工；按玩法需求触发。排序与定位：

| 编号 | 能力 | 触发时机 / 复杂度 | 与本次关系 |
|---|---|---|---|
| D-1 | ~~public 字段反射编辑~~（已并入 C4 完成） | —— | 本次已实现 number/bool/string；vec3 等复杂类型并入 D-4 |
| D-2 | 脚本订阅动画事件 | 动画增强计划阶段 B 的 `eventCallback` 已留句柄，桥接为 Lua 回调；小 | `ScriptEngine` 发 `sol::protected_function` |
| D-3 | 脚本创建/销毁实体、跨实体操作、消息 | 角色生成/击杀逻辑；中 | 需设计 entity 句柄的 Lua 包装（防悬挂引用） |
| D-4 | 数学类型（vec3/quat userdata） | 复杂移动/射线脚本；中 | 替换 2.3 的多返回值，向后兼容 |
| D-5 | 调试设施（Lua 控制台 / 断点） | 脚本规模上来后；大 | —— |
| D-6 | 发布硬化（包内脚本只读/加密、去 `load`/`require` 权限收紧） | 对外发布时；中 | 依赖 D-4 沙箱 |

**排序逻辑**：D-2/D-1 直接复用本次 `ScriptEngine` 基建，边际成本低；D-3/D-5 是玩法与工具需求驱动；D-6 是发布脏活，正式对外前再处理。

---

## 5. 新增/修改文件清单

| 文件 | 类型 | 改动 |
|---|---|---|
| `GE/third_party/lua/` | 新增 | Lua 5.4.x 官方源码（vendored，随仓库提交） |
| `GE/third_party/sol2/` | 新增 | sol2 ≥3.3 头（vendored，全套 `sol/`） |
| `CMakeLists.txt` | 修改 | Lua `*.c` 并入 GE 编译；sol2 进 PUBLIC include |
| `GE/include/GE/Scene/ScriptEngine.h` + `GE/src/Scene/ScriptEngine.cpp` | 新增 | 运行时管理器（2.2） |
| `GE/include/GE/Scene/Components.h` | 修改 | `ScriptComponent` 重构为 `{ScriptPath, Enabled}`（2.1） |
| `GE/include/GE/Scene/Scene.h` + `GE/src/Scene/Scene.cpp` | 修改 | 持有 `ScriptEngine`；`UpdateScripts` 改派 `ScriptEngine::OnUpdate`；`OnComponentAdded`/`on_destroy` 接线 |
| `GE/src/Scene/SceneSerializer.cpp` | 修改 | `Script` 键 YAML 读写（2.5） |
| `GE_Editor/src/Panels/SceneHierarchyPanel.cpp/.h` | 修改 | AddComponent 加项 + `DrawScriptComponent`（B2/B3） |
| `GE_Editor/src/EditorApp.cpp`（或 DockSpaceLayer） | 修改 | `Ctrl+R` 全局重载 + 菜单项（B4） |
| `assets/scripts/mover.lua` | 新增 | 示例脚本（阶段 A 验收） |
| `assets/scripts/broken.lua` | 新增 | 故意出错脚本（错误隔离验收） |
| `docs/Lua脚本系统计划书.md` | 新增 | 本文档 |

（无需改）渲染层 / 着色器 / `Mesh` / `AnimationClipManager` —— 脚本是场景层特性。

---

## 6. 风险与对策

| 风险 | 等级 | 对策 |
|---|---|---|
| sol2 模板重 → 编译慢、内存高 | 中 | **不进 PCH**，仅 `ScriptEngine.cpp` include；必要时 `SOL_*_SAF*` 宏裁剪未用特性 |
| Lua/sol2 版本兼容（MSVC/C++20） | 低 | Lua 5.4 + sol2 ≥3.3（官方组合），vendored 锁定版本、随仓提交 |
| 坏脚本崩编辑器 | 高 | `protected_function` 全调用点 + 错误日志 + 跳过实例 + 限频禁用（2.6，阶段 C 收口） |
| 热重载丢运行状态 | 中 | 决策「换逻辑不换状态」：保留实例字段、仅重跑 `OnCreate`（§9.8） |
| 脚本路径散乱 / 找不到文件 | 低 | 统一 `assets/scripts/` 基准 + `package.path` + 缺失 warn/禁用（§9.6） |
| 每实体独立实例表内存 | 低 | 实例表只存字段，行为表共享；实体数百级无压力，可后续池化（D） |
| on_destroy 未清理 → 实例泄漏 | 低 | 复用既有的 `on_destroy` 模式（仿 `Scene.cpp:129`），Clear 全部实例兜底 |
| Lua 5.4 与 GE 运行时库不一致 | 低 | 同 `/MD`，vendored 源码随 GE 一起编译，天然一致 |

---

## 7. 验证标准 / 交付验收

1. **构建**：`build.bat`（MSVC + Ninja）编译通过；无新着色器，无需 glslc 干预。
2. **阶段 A**：mover.lua 挂到实体后按键驱动生效；broken.lua 报错但编辑器存活、其它脚本照常；无脚本场景行为逐帧不回退。
3. **阶段 B**：保存→加载脚本保留且实例不泄漏；面板路径/Enabled/Reload 即时生效；坏路径 warn+禁用不崩。
4. **阶段 C**：API 可读鼠标/查组件；热重载换逻辑不换状态；连续报错受限频禁用、日志不刷屏。
5. **回归**：mint / buster_drone / 既有静态场景渲染与输入全部照常。

---

## 8. 测试资产

| 资产 | 用途 |
|---|---|
| `assets/scripts/mover.lua` | 阶段 A 主验收：WASD 平移 / R 旋转 / 空格日志，欧拉换算正确性 |
| `assets/scripts/broken.lua` | 错误隔离验收：语法错 + 运行错各一个，编辑器不崩、日志有脚本名 |
| 现有场景（mint 等）挂 mover.lua | 回归：渲染/输入不回退，序列化往返无泄漏 |

---

## 9. 决策记录（编码前拍板）

**9.1 运行时归属 —— ✅ 已定：`ScriptEngine` 由 Scene 持有，一个共享 `sol::state`**
- 每场景一个 Lua 状态、API 注入一次。不做每实体独立 `lua_State`（开销大、无收益）。随 Scene 生命周期，多场景各持各的。

**9.2 实例机制 —— ✅ 已定：行为表（dofile 缓存）+ 实例表（metatable→行为表）**
- 类同「class + instance」；每实体独立状态字段，行为共享。比「每实体复制函数表」省内存且热重载只需换行为表一个指针。

**9.3 组件形态 —— ✅ 已定：`ScriptComponent` 只留 `ScriptPath` + `Enabled`，纯数据**
- 运行态全放 `ScriptEngine`（组件不持有 `sol::table`，避免组件拷贝/序列化碰 Lua 引用）。

**9.4 API 形状 —— ✅ 已定：不绑 GLM，多返回值 + 度数欧拉角；不暴露 entt/裸指针**
- 脚本与内部分离；欧拉角对齐编辑器习惯。复杂数学留 D-4。

**9.5 输入接入 —— ✅ 已定：不设 `OnInput`，Lua 在 `OnUpdate` 内经 `input.*` 查询 `InputState` 快照**
- 输入快照改造（脚本输入系统计划书 §2.4）后引擎已无事件分发；Lua 与 C++ 脚本同源读 `Scene::GetInputState()`，无消费短路、无脚本先后，天然同帧语义。

**9.6 文件缺失 —— ✅ 已定：`GE_CORE_WARN` + 组件保留 + `Enabled=false`，不打断加载**
- 场景加载遇到坏路径继续走完，绝不因脚本问题卡死编辑器。

**9.7 序列化 —— ✅ 已定：只落 `ScriptPath` + `Enabled`；public 字段（若做）作为 C4 单独扩展**
- 最小载荷先跑通；public 字段反射是新一套（类型表 + 面板 + 序列化），放可选阶段，不进 MVP 序列化格式。

**9.8 热重载语义 —— ✅ 已定：换逻辑不换状态**
- `Reload(path)`：先 `OnDestroy` → 换行为表 → 重跑 `OnCreate`；实例表已有字段**保留**。玩家位置/计数器不清零，行为即时更新。

**9.9 错误策略 —— ✅ 已定：protected_function 捕获 + 跳过实例 + 连续 3 次自动禁用**
- Debug 期也开（`SOL_ALL_SAFETIES_ON`），让坏脚本在编辑器里「灰掉」而不是黑掉会崩。

**9.10 废弃旧回调 —— ✅ 已定：`ScriptComponent` 收敛为 `{ScriptPath, Enabled}`，`OnUpdate` 移交 `ScriptEngine` 接管**
- 六个输入回调和 `OnUpdate` 均无外部使用方；输入查询由注入的 `input.*` 快照 API 承担，按帧回调由 `ScriptEngine::OnUpdate` 统一调 Lua。`AnimationComponent::eventCallback` 是动画语义，保留，D-2 再桥接。

**9.11 public 字段（C4）—— ✅ 已定：运行时经 `public.get(name)` 实时读本实体 `ScriptComponent.PublicFields`，配置不入实例表**
- 脚本顶层 `PUBLIC_FIELDS`（`type` 支持 number/bool/string + `default`）作 schema，面板据此生成输入框；值存组件 map，随场景序列化（`Script.PublicFields`）。
- **不入实例表 ⇒ public 字段是「配置」不是「状态」**：面板改动/场景加载即时生效，与热重载「换逻辑不换状态」天然不冲突，也绝不与脚本自己的实例字段混叠。
- 挂载/热重载时按 schema 补默认：缺失字段补 default，已有值保留，脚本类型变化则重置默认。
- 约定：场景反序列化先 `AddComponent`（触发挂载 OnCreate）再回读 loaded 值，故加载首帧 OnCreate 读到默认、此后 `public.get` 即 loaded 值——可接受，不重挂。

**9.12 限频禁用（C2）—— ✅ 已定：连续 3 次 OnUpdate 报错自动 `Enabled=false`**
- 计数只在 `OnUpdate` 累计（OnCreate/OnDestroy 等其余 hook 只记 lastError 不累计），成功一帧即清零；触发禁用后计数清零，重新勾选/重挂才重新累计 3 次。
- 面板状态行区分「未加载 / 已暂停（连续报错自动禁用）/ 已暂停（手动禁用）/ 已加载」四态，并显示最近错误文本（`ScriptEngine::GetLastError`）。

---

## 10. 与既有链路的关系（改动面收敛）

```
OnUpdate3D（Scene.cpp:506）
  ├─ UpdateScripts ──► ScriptEngine::OnUpdate（脚本每帧最早动 local TRS）
  ├─ StepPhysics、UpdateAnimations（既有，零改动；时序保持脚本优先）
  └─ UpdateWorldTransforms（DFS）──► 蒙皮肤管线 / 渲染（既有，零改动）

OnEvent（Scene.cpp:852）
  └─ 写 InputState 快照（既有，见脚本输入系统计划书）──► C++/Lua 脚本经 input.* 查询，无事件分发

实体销毁：DestroyEntity → m_Registry.destroy ──► on_destroy<ScriptComponent> ──► OnDestroy
          （复用是从 Scene.cpp:129 RigidBody 物理清理迁来的既有 on_destroy 模式）
```

`ScriptComponent` 从「跑不起来的回调占位」降为「指路数据」，运行面收进 `ScriptEngine`；渲染层、`.gemesh/.geanim` 格式、物理、动画全部不动。动画增强计划书 §2.2 提到的「脚本订阅动画事件」在本计划 D-2 落地，届时 `eventCallback` 直接挂到 Lua 回调。

---

> 参考既有文档风格：架构见 `docs/项目架构总览.md` §2.5（ECS/组件表）；动画侧衔接 `docs/动画系统增强计划书.md`（事件回调预留）。本计划书待用户确认后开始阶段 A。