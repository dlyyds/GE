# 动画状态机 ASM 实现计划书

> 状态：**计划中（未开工）**。前置已完成：动画系统增强阶段 A~C 全部落地——键帧采样缓存（`ClipInstance::keyHints`）、动画事件（区间法 + loop 回绕）、`PlayClip(idx, blendSec)` 交叉淡化（双路 eval + `blendBuffer` α 混合）。脚本侧已具备 Lua 运行时（sol2）与 `input.*`/`transform.*` 等按实体注入的 API 表。
> 目标：把「编辑器/脚本**手动** `PlayClip` 切片段」升级为「**状态图 + 参数 + 条件**自动切片段」。对应增强计划书 §4 路线图 D-2（做了它才能做 D-3 分层动画）。
> 定位：ASM 只回答「**何时**切、**切到哪个** clip、**用什么过渡**」；实际切换仍走现有 `PlayClip` 交叉淡化与事件机制，**采样/蒙皮/DFS/渲染层零改动**。分四阶段各自独立可回退，每阶段结束可编译可运行。

---

## 0. 一句话架构

```
状态图（AnimStateMachineComponent，挂动画实体）：状态列表 + 转换表 + 参数表
每帧（仅 playing）：累加 stateTime → 按声明序找第一条满足条件的导出转换
  → 命中：EnterState(to) = 应用目标状态 speed/loop + PlayClip(to.clip, blendSec)
参数（float / bool / trigger）由脚本（anim.set / set_bool / trigger）或其它系统填写
编辑器手动切 clip 视为外部覆盖：若 ASM 在跑则同时关停 ASM
```

**三个"不改"**：不改 `UpdateAnimations` 的采样/事件/过渡实现；不改 `PlayClip`（保持 ASM 无感知，避免重入歧义）；不改蒙皮/DFS/渲染/着色器。

---

## 1. 现状盘点与差距定位

| 能力 | 现状 | ASM 差距 |
|---|---|---|
| 切 clip | 编辑器下拉 / 脚本 `PlayClip(idx, blendSec)` **手动**切换 | 需「按条件自动切」：速度、落地、血量、按键触发 |
| 交叉淡化 | `PlayClip` 双路混合已落地（阶段 C） | ASM 只决定**何时**调它，复用即可 |
| 播放循环/倍速 | `AnimationComponent.loop` / `speed` 全局字段 | 需**每状态**可覆盖（一体态循环、死亡态播一次） |
| 事件 | 阶段 B 按 clip 触发回调 | 不变；ASM 切换仍走事件 |
| 参数/条件 | 无 | 纯新（float/bool/trigger 参数 + 比较条件 + 驻留时间） |
| 图编辑 | 无（仅 clip 下拉） | 新增列表式编辑器（节点图形态记路线图 D-11） |

本次只做上表后四行；节点图编辑器、分层、混合空间等仍在 §4 路线图，不提前开工。

---

## 2. 总体设计

### 2.1 归属与数据模型

**归属**：新组件 `AnimStateMachineComponent`，**与 `AnimationComponent` 同实体**（挂在带骨架的角色上）。不并入 `AnimationComponent`——动画内核与"玩法切换策略"分离，序列化 / 编辑器面板各自独立，可单独启用停用。

```cpp
/// 单个状态：指到一条 clip + 播放参数覆盖（进入状态时应用到 ac）
struct AnimStateDef {
    std::string name;      ///< 状态名（序列化稳定标识，编辑器/转换引用）
    std::string clipName;  ///< 目标 AnimationClip::name（与 AnimationClipManager 键无关；解析为 clips 下标）
    bool loop = true;      ///< 进入时写 ac.loop（false = 播一次，配合「播完」条件离开）
    float speed = 1.0f;    ///< 进入时写 ac.speed
};

/// 条件节点：各类型取其一（其余字段默认无效）
struct AnimCondition {
    enum class Type : uint8_t { FloatCmp = 0, Bool = 1, StateTime = 2, StateEnded = 3 };
    enum class Cmp : uint8_t { Greater, GreaterEq, Less, LessEq, NearEq, Not };
    Type type = Type::FloatCmp;
    std::string param;     ///< FloatCmp/Bool：参数名
    Cmp cmp = Cmp::Greater;
    float value = 0.0f;    ///< FloatCmp 阈值 / StateTime 秒 / NearEq 用同值 + 内置 eps
    bool expect = true;    ///< Bool 期望值
};

/// 转换边：from → to（from 空 = 全局 ANY，任意状态可退出）
struct AnimTransitionDef {
    size_t from = SIZE_MAX; ///< 源状态下标；SIZE_MAX = ANY（全局转换）
    size_t to = 0;          ///< 目标状态下标
    float blendSec = 0.25f; ///< 复用阶段 C 过渡时长
    std::vector<AnimCondition> conditions; ///< 全满足（AND）才触发；空 = 恒真
};

/// 运行时参数表（不序列化；初值由脚本 OnCreate 填写）
struct AnimStateMachineComponent {
    bool enabled = false;         ///< 总开关；editor/脚本可开
    std::string initialState;      ///< 启用时进入的状态名（空 = 取第一个状态）
    std::vector<AnimStateDef> states;
    std::vector<AnimTransitionDef> transitions;

    // ---- 运行时状态（不序列化）----
    size_t current = SIZE_MAX;    ///< 当前状态下标（SIZE_MAX = 未进入）
    float stateTime = 0.0f;       ///< 当前状态已驻留秒数（求值累加，EnterState 清零）

    // ---- 参数 ----
    std::unordered_map<std::string, float> floats;          ///< 连续参数（速度、方向、血量）
    std::unordered_map<std::string, bool> bools;            ///< 开关参数（on_ground、in_air）
    std::unordered_set<std::string> triggers;               ///< 一次性脉冲，求值读到即消费

    AnimStateMachineComponent() = default;
    AnimStateMachineComponent(const AnimStateMachineComponent &) = default;
};
```

> **clip 引用用名字不用下标**：下标随模型重排/增删会错位；`AnimationClip::name`（如 `"...Skeleton|char_walk"`）稳定可辨识。解析在装配/反序列化时做，缓存下标到 `states[].clipName` 对应关系；解析失败的状态取「无效」，被转换引用时跳过并 WARN。

### 2.2 参数来源：脚本桥

沿用 ScriptEngine 现有「按当前实体注入 API 表」模式（`lua["input"]/["transform"]/["entity"]`），新增一张 `lua["anim"]` 表（阶段 C 实施）：

```lua
-- 脚本 OnCreate 里可初始化：
anim.set("speed", 0.0)          -- float 参数
anim.set_bool("on_ground", true) -- bool 参数
-- OnUpdate 里按输入/物理驱动：
anim.set("speed", ...)
anim.set_bool("on_ground", body:on_ground())
if input.just_pressed(Key.Space) and on_ground then anim.trigger("jump") end
```

```cpp
// ScriptEngine.cpp BindScriptingApi 增：
animT["set"]    = [&eng](const std::string &name, float v) { /* 当前实体 ASM.floats[name] = v */ };
animT["set_bool"]= [&eng](const std::string &name, bool v){ /* ASM.bools[name] = v */ };
animT["trigger"] = [&eng](const std::string &name) { /* ASM.triggers.insert(name) */ };
animT["get"]    = [&eng](const std::string &name) -> float { /* 读 floats/state_time 回退 */ };
```

**trigger 自动消费**：`trigger("jump")` 置入集合；下一次求值读到即从集合移除——天然「按采样跳一次」，脚本无需手动复位。`get` 额外回退到 `stateTime`（脚本想自己读驻留时长用）。

### 2.3 条件求值 & 转换选择

**条件**（一个转换下多条，AND）：
- `FloatCmp`：`floats[param]` 与 `value` 比较；`NearEq` 用内置 `eps=0.01f`（`|a-b|<=eps`）。参数缺省按 0 参与比较。
- `Bool`：`bools[param] == expect`（缺省按 false）。
- `StateTime`：`stateTime >= value`（驻留时间下限，防触发后立刻回跳的"振铃"）。
- `StateEnded`：当前 active clip **非循环已到末尾**，即 `ac.loop==false && ac.time >= clip.duration - eps`。用于「死亡动画播完回 idle」等一次性状态。

**选择**：按 `transitions` 声明序遍历；过滤 `from==current || from==SIZE_MAX(ANY)`；**第一条全部条件满足**的即触发（声明序 = 优先级）。都不满足则保持当前状态。

```cpp
// 求值（阶段 B，UpdateAnimations 内每实体、仅 ac.playing 时）
if (asm.enabled && asm.current != SIZE_MAX) {
    ac.loop  = asm.states[asm.current].loop;   // 状态参数持续生效（也防外改）
    ac.speed = asm.states[asm.current].speed;
    asm.stateTime += ts.GetSeconds();
    for (const AnimTransitionDef &t : asm.transitions) {
        if (t.from != SIZE_MAX && t.from != asm.current) continue;
        if (!EvalConditions(asm, ac, t.conditions)) continue;
        if (t.to != asm.current && ValidState(asm.states[t.to]))
            EnterState(asm, ac, t.to, t.blendSec);
        break; // 声明序首达优先
    }
}

// 进入状态
void EnterState(AnimStateMachineComponent &asm, AnimationComponent &ac, size_t to, float blend) {
    asm.current = to;
    asm.stateTime = 0.0f;
    ac.loop  = asm.states[to].loop;
    ac.speed = asm.states[to].speed;
    ac.PlayClip(ClipIndexOf(asm.states[to].clipName), blend); // 复用阶段 C 过渡
}
```

### 2.4 运行时机与手动覆盖

- **求值位置**：`UpdateAnimations` 每实体处理时、在「推进时间轴」**之前**——求值若触发 `EnterState` 已把 active/time/过渡改好，本帧推进与混合即刻按新目标走，语义干净。
- **PlayClip 保持 ASM 无感知**（决策 9.5）：`PlayClip` 不读、不写 ASM。"手动覆盖 = 关停 ASM" 由**调用侧**落实：
  - 编辑器 clip 下拉：若 `asm.enabled`，先 `asm.enabled=false`（面板日志提示）再 `PlayClip`。
  - 脚本显式换片段：新增 `anim.play(idx, blendSec)` 或约定「脚本调 `PlayClip` 也关停 ASM」。v1 只提供 `anim` 参数表，**不给脚本直调 PlayClip**，消除歧义；要硬覆盖走编辑器或后续扩展。
- **启用时进入初始状态**：`enabled=true` 瞬间若 `current==SIZE_MAX`，用 `initialState`（空则取状态 0）走 `EnterState(…, 0)`（硬切），随后正常求值。关停则 `current=SIZE_MAX`，回到手动 `PlayClip` 控制。
- **暂停**（`ac.playing==false`）：跳过求值（参数也不变，本就不会触发），`stateTime` 冻结。

### 2.5 序列化

`.scene` 增 `AnimStateMachine` 节点。**参数不落盘**（运行时玩法态，初值由脚本 `OnCreate` 重填）；`current/stateTime` 不落盘（反序列化重建组件后 `current=SIZE_MAX`，首次 `enabled=true` 时从 initial 进入）。

```yaml
AnimStateMachine:
  Enabled: true
  Initial: idle
  States:
    - { Name: idle, Clip: "player_..._Skeleton|char_idle", Loop: true,  Speed: 1.0 }
    - { Name: walk, Clip: "player_..._Skeleton|char_walk", Loop: true,  Speed: 1.0 }
    - { Name: dead, Clip: "player_..._Skeleton|char_die",  Loop: false, Speed: 1.0 }
  Transitions:
    - { From: idle, To: walk, BlendSec: 0.25, Conditions: [ { Type: FloatCmp, Param: speed, Cmp: Greater, Value: 0.1 } ] }
    - { From: walk, To: idle, BlendSec: 0.25, Conditions: [ { Type: FloatCmp, Param: speed, Cmp: LessEq,  Value: 0.1 } ] }
    - { From: "*", To: dead, BlendSec: 0.10, Conditions: [ { Type: Bool, Param: dead, Expect: true } ] }
    - { From: dead, To: idle, BlendSec: 0.30, Conditions: [ { Type: StateEnded } ] }
```

反序列化：states/transitions 按序建立并**解析 clipName→clips 下标**（`AnimationComponent.clips` 内按 `clip->name` 查，未命中状态置无效 + WARN）；`From/To` 先用名字串序列化，加载后转下标，转换里引用无效状态时跳过容错。

### 2.6 编辑器面板（阶段 D，列表式，非节点图）

在 Animation 组件面板下方增"动画状态机"分栏（`DrawAnimStateMachine`）：

- **总开关 + 初始状态**：Checkbox「状态机」、初始状态下拉。开态下原「播放片段」下拉区块被"运行中：当前状态 X / stateTime Ns"占位替代，**手动切 clip 自动关停 ASM**（决策 9.5 落实处）。
- **参数调试行**：脚本参数在此只读预览；另加**一组调试滑条/开关**直接写 `floats/bools`（驱动测试，不依赖脚本）——验证求值与回跳的主力手段。
- **状态表**：每行「名字输入 + clip 下拉 + Loop 勾选 + Speed 输入 + 删除」，「添加状态」按钮；当前状态加标识。
- **转换表**：每行「From 下拉（含 ANY）+ To 下拉 + 过渡时长 + 删除」，点开可编辑该行**条件列表**（每条件一行：类型下拉 + 参数输入 + 比较下拉 + 阈值输入 + 删除）。新版是纯列表：从/到/条件都明确，避免节点图复杂度。

---

## 3. 分阶段实施

### 阶段 A：数据模型 + 序列化

- A1. `Components.h`：`AnimStateDef/AnimCondition/AnimTransitionDef/AnimStateMachineComponent`（见 2.1）。
- A2. `SceneSerializer.cpp`：序列化 / 反序列化 `AnimStateMachine`（见 2.5），旧场景无节点 → 缺省组件（`enabled=false`）照常。
- A3. `OnComponentAdded<AnimStateMachineComponent>` 空特化（跟随主模板 static_assert 惯例）。

**退出标准**：加挂组件保存→加载后状态/转换/条件逐字段一致；旧场景加载不回归。

### 阶段 B：运行时求值驱动 PlayClip

- B1. `Scene.cpp`：`UpdateAnimations` 内接入求值（见 2.3 伪码）+ `EvalConditions`/`ResolveClipIndex`/`EnterState` 抽函数 + `stateTime`。
- B2. 编辑器联动：手动切 clip 关停 ASM（在 `PlayClip` 调用处统一处理，与 2.4 一致）。
- B3.（可选）`anim.get` 回退读到 `stateTime` 前先实现 C 段的参数表，供脚本读。

**退出标准**：制造 2~3 个状态（同一 clip 不同相位/速度）+ 参数条件，编辑器调试滑条驱动能正确进入/停留在目标状态；声明序优先、ANY 转换、StateTime/StateEnded 均正确；关闭 ASM 后行为与现状逐帧一致。

### 阶段 C：Lua 脚本桥

- C1. `ScriptEngine.cpp`：注入 `lua["anim"]` 表（`set/set_bool/trigger/get`，见 2.2）。
- C2. 样例脚本：`OnUpdate` 读 `input`/物理状态写参数、`OnAnimationEvent` 收事件。
- C3. 参数运行 RL 确认：`get` 回退 `stateTime`。

**退出标准**：脚本 OnCreate 初始化 + OnUpdate 驱动参数 → 状态机按输入切换；trigger 一次脉冲跳一次不重复触发；无参数表时报错/缺省不崩。

### 阶段 D：编辑器 ASM 面板

- D1. `SceneHierarchyPanel.cpp`：`DrawAnimStateMachine` 分栏（2.6），含状态/转换/条件编辑 + 调试滑条 + 当前状态显示。

**退出标准**：面板增删改状态/转换/条件即时生效；开/关 ASM、选初始状态正确；当前状态与 stateTime 每帧刷新；面板编辑可回读保存。

---

## 4. 后续路线图（本计划范围外）

| 编号 | 能力 | 触发时机 / 复杂度 | 与本次关系 |
|---|---|---|---|
| D-11 | 节点图 ASM 编辑器（ImNodes 类） | 图复杂度上来后；大 | 基于阶段 D 数据模型，仅换编辑界面 |
| D-3 | 分层动画（layer 权重 + 关节 mask） | 上下半身分离动作时；大 | **依赖本 ASM**（多状态并行），勿提前 |
| D-4 | 混合空间 1D/2D（速度参数→连续插值） | 移动轴确立后；中 | 可把某状态替换成混合空间节点 |
| D-7 | Root Motion | 走跑循环带位移时；中 | ASM 自动切换与 root 位移配合 |
| D-12 | 转换优先级/权重显式化、条件 OR 组、参数重置策略 | 玩法复杂后；小-中 | 扩展 2.3 求值，不换架构 |

**排序逻辑**：本计划先落地求值内核与脚本桥；节点图/分层是换皮与叠加，等真实角色需求再动。

---

## 5. 新增/修改文件清单

| 文件 | 类型 | 改动 |
|---|---|---|
| `GE/include/GE/Scene/Components.h` | 修改 | `AnimStateDef/AnimCondition/AnimTransitionDef/AnimStateMachineComponent`（阶段 A） |
| `GE/src/Scene/SceneSerializer.cpp` | 修改 | `AnimStateMachine` 序列化/反序列化（A2） |
| `GE/src/Scene/Scene.cpp` | 修改 | 求值接入 + `EvalConditions/EnterState/...`（B1） |
| `GE/src/Scene/ScriptEngine.cpp` | 修改 | 注入 `lua["anim"]` 参数表（C1） |
| `GE_Editor/src/Panels/SceneHierarchyPanel.cpp` | 修改 | ASM 面板（D1）+ 手动切 clip 关停 ASM（B2） |
| `assets/scripts/asm_demo.lua` | 新增 | 阶段 C 验收样例脚本 |
| `docs/动画状态机ASM计划书.md` | 新增 | 本文档 |
| （无需改）渲染层 / 蒙皮 / DFS / 着色器 / `.geanim` | — | ASM 纯场景层播放策略 |

---

## 6. 风险与对策

| 风险 | 等级 | 对策 |
|---|---|---|
| 状态引用的 clip 名字解析失配（模型重导入/改名） | 中 | 名字序列化稳定；解析失败状态置无效 + WARN，被引用时跳过不崩 |
| 手动切 clip 与 ASM 抢 `active` | 中 | 决策 9.5：手动 = 外部覆盖、关停 ASM；由调用侧落实，`PlayClip` 保持无感知 |
| 条件漏配/临界触发导致同帧回跳（振铃） | 中 | 提供 `StateTime` 最小驻留条件；（可选）转换求值用「进入后至少 1 帧」冷却 |
| trigger 消费时序（本帧未求值就复位/漏跳） | 低 | trigger 放集合、求值读到即删，天然脉冲；验收做精确次数断言 |
| 多状态引用同一 clip 互转看不出变化 | 低 | 正常（clip 相同但 phase/speed 不同仍过渡）；验收说明靠 speed/相位差异 |
| 大量状态/转换的表单编辑繁琐、易配错 | 中 | 阶段 D 提供参数调试滑条驱动闭环；错误引用即时 WARN；节点图记为 D-11 |
| 序列化旧场景无 `AnimStateMachine` 节点 | 低 | 缺省 `enabled=false` 照常 |

---

## 7. 验证标准 / 交付验收

1. **构建**：`build.bat`（MSVC + Ninja）编译通过。
2. **阶段 A**：ASM 挂组件序列化往返逐字段一致；旧场景加载回归通过。
3. **阶段 B**：编辑器调试滑条驱动多状态进入/停留/退出正确；ANY 全局转换、声明序优先、`StateTime`、`StateEnded` 行为正确；关停 ASM 后与现状逐帧一致。
4. **阶段 C**：脚本写参数 → 状态机按输入切换；trigger 一次性不重不漏；无脚本/无参数时缺省不崩。
5. **阶段 D**：面板增删改即时生效、当前状态/`stateTime` 刷新、保存回读一致。
6. **回归**：未启用 ASM 的既有场景（mint/lacrimosa/单片段）渲染、播放、事件、阶段 C 过渡全部照常。

---

## 8. 测试资产

| 资产 | 用途 |
|---|---|
| 现有单 clip 模型（mint/lacrimosa） | 阶段 A/B 数据与求值正确性（多状态引用**同一 clip** 不同 speed/相位；参数回调验证切换日志）。注意视觉差异有限，配合编辑器 debug 滑条与日志断言 |
| **多动画角色 glTF（mixamo 类，仍需物色）** | idle/walk/run/dead 三角的真视觉主验收（复用阶段 C 过渡）。未到位前先用「参数驱动的同一 clip 双状态」做功能验证 |
| mint 既有事件 | 回归：ASM 切换后事件仍按 clip 触发（不回归阶段 B） |

---

## 9. 决策记录（编码前拍板）

**9.1 ASM 归属 —— ✅ 已定：独立组件 `AnimStateMachineComponent`（与 AnimationComponent 同实体）**
- 动画内核（采样/过渡/事件）与"玩法切换策略"分离；序列化、编辑器面板、启用开关各自独立；去掉它 = 回到手动 `PlayClip`，完全无影响。

**9.2 状态→clip 引用 —— ✅ 已定：用 `AnimationClip::name`（序列化稳定），加载解析为下标并缓存**
- 下标随模型重排错位；名字可辨识。解析失败状态置无效 + WARN。

**9.3 参数 —— ✅ 已定：float/bool/trigger 三类，存 ASM 组件、不序列化；trigger 自动消费**
- 参数是玩法态（初值脚本 `OnCreate` 重填）。trigger 用集合 + 读到即删实现一次性脉冲。调试滑条直写同一参数表，编辑器驱动与脚本驱动等价。

**9.4 条件与优先级 —— ✅ 已定：单转换多条件 AND；转换声明序 = 优先级（首达优先）；`from` 空 = ANY 全局转换**
- 提供 `FloatCmp / Bool / StateTime / StateEnded` 四种。`StateEnded` 语义 = `ac.loop==false && time>=duration-eps`（一次性状态播完离开的通道）。

**9.5 PlayClip 与 ASM 的关系 —— ✅ 已定：`PlayClip` 保持 ASM 无感知；"手动覆盖=关停 ASM"由调用侧落实**
- 避免重入歧义：ASM 内部可自由调 `PlayClip`（它就是阶段 C 那套过渡）。编辑器下拉先关停 ASM 再切；v1 不向脚本暴露裸 `PlayClip`，只有 `anim` 参数表，硬覆盖留给编辑器。
- `enabled=true` 时进入初始状态（`initialState` 空取状态 0）硬切，随后正常求值。

**9.6 驻留与回跳 —— ✅ 已定：`StateTime` 最小驻留由用户条件显式配；不做隐式冷却**
- 默认允许同帧进出（显式条件比隐式规则可预期）；需要时配 `StateTime >= 0.2s` 即可，避免"框架替你猜"。

**9.7 暂停语义 —— ✅ 已定：`playing=false` 时跳过求值，`stateTime` 冻结**
- 与阶段 C 过渡的暂停冻结行为一致；编辑器暂停预览时不跳转。

**9.8 编辑器形态 —— ✅ 已定：阶段 D 做列表式面��；节点图记 D-11**
- 列表式的 From/To/条件都明确、可保存，先满足"能配能用"；节点图是换编辑壳，不改变数据模型。

**9.9 `speed`/`loop` —— ✅ 已定：由状态在**进入时**与应用时**双重写入** `ac.speed/ac.loop`
- 进入时写入保证换状态生效；求值期每帧再写一次防外部（如脚本直接改 ac）漂移。状态全量指定，不做"继承/覆盖"两级语义。

---

## 10. 与既有链路的关系（改动面收敛）

```
UpdateAnimations（写局部 TRS，既有）
  ├─ 求值接入（阶段 B，仅 ASM 已启用时）：参数→条件→EnterState→PlayClip(to, blendSec)
  ├─ 阶段 C 过渡机制：复用，ASM 不重写采样/混合
  ├─ 阶段 B 事件：不变，切换后新 clip 的事件照常按区间法触发
  → UpdateWorldTransforms / UpdateSkins / 蒙皮肤管线 / 渲染 既有，零改动
```

`OnUpdate3D` 时序不变（脚本先于动画，参数先写好）：`脚本→物理→[ASM 求值 + 动画推进]→DFS→蒙皮→渲染`。`.geanim` 二进制格式不动（ASM 是场景/玩法数据，不进烘焙格式）。因此本计划是纯场景层播放策略增强，与既有"动画是蒙皮之上的薄层"定位一致。