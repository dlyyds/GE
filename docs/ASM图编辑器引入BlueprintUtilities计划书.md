# ASM 节点图引入 Blueprint 图标工具 + 节点布局 Builder 实现计划书

> 状态：**计划中（未开工）**。前置：ASM 节点图面板（`GE_Editor/src/Panels/ASMGraphPanel.cpp`）已
> 落地阶段 B 只读渲染，节点/引脚图标目前是**手绘原语**（`DrawStateIcon`/`DrawPinIcon`），并已修复
> 连线标签误用 `ed::GetHintBackgroundDrawList()` 崩溃（改用 `ImGui::GetWindowDrawList()`，不回退）。
> 仓库自带的 `imgui-node-editor` 0.9.3 的 blueprints 示例提供一套**可复用 UI 工具**（`utilities/`）：
> 类型化图标（`drawing`/`widgets`）与 Blueprint 风格节点布局（`builders` 的 `BlueprintNodeBuilder`）。
> 目标：把这两类工具引入工程，并让 ASM 图的状态节点/ANY 节点用上 Builder 的 Header+Input/Output 结构
> 与 `IconType` 图标 —— 让节点外观向 blueprints 示例对齐，手绘图标代码删除。
> 定位：**纯编辑器 UI 侧改造**。不改 `AnimStateMachineComponent` 数据模型、不改求值内核、不改序列化；
> 不改 imgui-node-editor 本身；只新增 imgui 的一小段布局 API、新增一组工具源文件、重写 ASM 面板节点渲染。

---

## 0. 一句话架构

```
blueprints 的 utilities（NodeEditor 示例自带，依赖 imgui 的 Spring/BeginVertical/BeginHorizontal 布局）
    ├─ 移植 imgui "栈布局引擎"(fork 1.84 的完整实现) → 引擎 imgui 1.92.8（新增公共 API，作为 imgui 一部分）
    ├─ 拷贝 drawing/widgets（图标工具，纯 ImDrawList，原样保留 namespace ax::）
    └─ 拷贝 builders（BlueprintNodeBuilder，保留接口与 namespace ax::NodeEditor::Utilities，改掉贴图分支为纯色色条）
ASMGraphPanel 改用 util::BlueprintNodeBuilder 画节点 + ax::Widgets::Icon 画引脚图标
```

**关键前置依赖（务必先理解）**：blueprints 的 `builders.cpp`/`widgets.cpp` 依赖 imgui 的
`Spring`/`BeginHorizontal`/`BeginVertical`/`EndHorizontal`/`EndVertical` 栈布局 API。这套 API
**不在**本项目 vendored imgui（1.92.8 WIP）里 —— 引擎现只有 `LayoutType`/`BackupLayout` 等
枚举壳与字段残留（imgui 官方「开发中未完成的水平/垂直布局」半成品），没有 `ImGuiLayout` 结构、
没有 Spring、没有可用的布局实现。真正的完整实现只存在于 node-editor 仓库自带的
`external/imgui/`（一个基于 imgui 1.84 WIP 的 fork）。因此**第一步必须把该布局引擎移植进引擎 imgui**。

---

## 1. 现状盘点与差距定位

| 能力 | 现状（ASMGraphPanel 手绘） | blueprints utilities 提供 | 差距 |
|---|---|---|---|
| 引脚类型图标 | `DrawPinIcon` 手绘圆环/圆点 | `Widgets::Icon` + `IconType{Flow,Circle,Square,Grid,RoundSquare,Diamond}` | 图标形状/风格统一、可表达"转换/事件/布尔"等语义 |
| 状态图标 | `DrawStateIcon` 手绘实/空心圆 | 同一 Icon 工具（或继续用语义色点） | 换 IconType 表达当前/初始 |
| 节点结构 | `BeginNode` + 手排 `BeginGroup`/`SameLine` | `BlueprintNodeBuilder`（Header 色条 + 输入列/输出列 + Spring 弹性） | 标题色条、左右引脚对齐、列式排布 |
| 布局能力 | 无（手排叠 ImGui 基本原语） | `Spring`/`BeginHorizontal`/`BeginVertical` | **需先移植布局引擎** |

**复用对象（上游源，均只读不修改原库）**：
- `GE/third_party/imgui-node-editor-0.9.3/examples/blueprints-example/utilities/`
  → `drawing.{h,cpp}`、`widgets.{h,cpp}`、`builders.{h,cpp}`
- `GE/third_party/imgui-node-editor-0.9.3/external/imgui/`（fork imgui 1.84 WIP）
  → 栈布局引擎的实现源（`imgui.h`/`imgui_internal.h`/`imgui.cpp` 内 `[SECTION] STACK LAYOUT]` 及
  6 处核心接入点）

---

## 2. 总体设计

### 2.1 imgui 布局引擎移植方案（阶段 1，独立可验收）

**移植范围**：把 fork imgui 1.84 的「栈布局引擎」忠实移植进引擎 imgui 1.92.8。采用
「**整块搬 + 6 处核心接入**」策略（fork 布局引擎算法自包含在一个 SECTION 内，核心接入点少而集中）。

**（A）`imgui_internal.h` 新增（引擎内部结构，供 imgui.cpp 使用）**
1. `typedef int ImGuiLayoutItemType;` + `enum ImGuiLayoutItemType_ { _Item, _Spring }`（fork 143-144、873-876）。
   `ImGuiLayoutType` 枚举壳已存在（1.92.8 1135-1138），保留复用。
2. `struct ImGuiLayoutItem`（48B，fork 1194-1220：Type/MeasuredBounds/SpringWeight/SpringSpacing/
   SpringSize/CurrentAlign/CurrentAlignOffset/VertexIndexBegin/VertexIndexEnd + 构造）。
3. `struct ImGuiLayout`（fork 1222-1257：Id/Type/Live/Size/CurrentSize/MinimumSize/MeasuredSize/
   Items/CurrentItemIndex/ParentItemIndex/Parent/FirstChild/NextSibling/Align/Indent/StartPos/StartCursorMaxPos + 构造）。
4. `ImGuiWindowTempData` 增 4 字段（fork 1904-1906，插到现有 `LayoutType`/`ParentLayoutType` 附近）：
   `ImGuiLayout* CurrentLayout;`、`ImGuiLayoutItem* CurrentLayoutItem;`、
   `ImVector<ImGuiLayout*> LayoutStack;`。
   （`ImGuiWindow` 的 `BackupLayout` 字段 1.92.8 已有，属于官方半成品，与本移植无冲突。）
5. 前置声明：`ImGuiLayout`/`ImGuiLayoutItem` 需在 `struct ImGuiWindow` 完整定义前声明
   （`ImGuiWindowTempData` 内含 `ImGuiLayout*` 指针，前置声明即可）。

**（B）`imgui.h` 新增公共 API（供 builders.cpp 调用）**
放在 `SameLine()` 附近，仿 fork（imgui.h 450-458）：
```cpp
IMGUI_API void BeginHorizontal(const char* str_id, const ImVec2& size = ImVec2(0,0), float align = -1.0f);
IMGUI_API void BeginHorizontal(const void* ptr_id, const ImVec2& size = ImVec2(0,0), float align = -1.0f);
IMGUI_API void EndHorizontal();
IMGUI_API void BeginVertical(const char* str_id, const ImVec2& size = ImVec2(0,0), float align = -1.0f);
IMGUI_API void BeginVertical(const void* ptr_id, const ImVec2& size = ImVec2(0,0), float align = -1.0f);
IMGUI_API void EndVertical();
IMGUI_API void Spring(float weight = 1.0f, float spacing = -1.0f);
```
（fork 还有 int/ptr 重载与 `SuspendLayout`/`ResumeLayout`；ASM 场景用不到 int/ptr/Suspend，**按需最小引入**：
仅搬上述 7 个。但 `builders.cpp`/`blueprints` 正文只用 str_id/void* 重载，7 个足够；int 重载如 blueprints 用到再补。）

**（C）`imgui.cpp` 新增布局引擎实现**
把 fork `imgui.cpp` 的 `[SECTION] STACK LAYOUT]`（fork 7946-8636，约 690 行）整块搬入，
含：
- 前置 static 声明（fork 917-933：`FindLayout/CreateNewLayout/BeginLayout/EndLayout/PushLayout/
  PopLayout/BalanceLayoutSprings/BalanceChildLayouts/BalanceLayoutItemsAlignment/
  BalanceLayoutItemAlignment/CalculateLayoutSize/GenerateLayoutItem/
  CalculateLayoutItemAlignmentOffset/TranslateLayoutItem/BeginLayoutItem/EndLayoutItem/AddLayoutSpring`）。
  注意 fork 里这些是 `static ... ImGui::`（声明于文件顶部、定义于 STACK LAYOUT 段）—— 移植时
  对应 1.92.8 的声明风格（1.92.8 用 `static void ImGui::Xxx()` 无前置声明，需核对）。**忠实搬算法**，
  签名/static 关键字/函数体与 fork 一致；仅做 1.84→1.92.8 的 API 适配（如 `IM_FLOOR`、`ImFloor`
  命名差异、`GetItemRectMin` 等是否同名）。定义内调用的 helper（如 `SignedIndent`）也一并搬。
- 公共 API：`BeginHorizontal`/`EndHorizontal`/`BeginVertical`/`EndVertical`/`Spring`（fork 8563-8620）。

**（D）imgui.cpp 核心函数接入点（关键，逐条对照 fork）**

| # | 核心函数 | fork 改动 | 1.92.8 落点 | 说明 |
|---|---|---|---|---|
| 1 | `ImGuiWindow::~ImGuiWindow()` | fork 2955-2959：析构时遍历 `DC.Layouts` 并 `IM_DELETE` 每个 `ImGuiLayout*` | 1.92.8 ~4700 | 释放持久 layout（跨帧存储于 `window->DC.Layouts`） |
| 2 | `ImGui::Begin()` | fork 6385-6390：每帧把 `window->DC.Layouts` 中 layout 标 `Live=false`（帧初预标死，运行时 `BeginLayout` 置回） | 1.92.8 ~7793 title-bar 收尾后 | 帧生命周期管理 |
| 3 | `ImGui::End()` | fork 7415-7416：断言 layout 栈空（mismatch 检测） | 1.92.8 ~8765 | BeginHorizontal/Vertical 未配对的兜底断言 |
| 4 | `ImGui::ItemSize()` | fork 7472-7474：`layout_type` 读 `window->DC.CurrentLayout` 决定横/竖测量；加 `layout_type` 分支（fork 7476+，把竖直/水平两分支的测量改成按布局类型走） | 1.92.8 ~12050 | **关键**：普通 item 在 horizontal 布局里横向累加、在 vertical 布局里纵向累加 |
| 5 | `ImGui::ItemAdd()` | fork 7568-7569：尾段 `if (DC.CurrentLayoutItem) CurrentLayoutItem->MeasuredBounds.Max = ImMax(..., bb.Max)` | 1.92.8 ~11912 尾段 | **关键**：每个 item 的包围盒并入当前 layout item，撑开测量尺寸 |
| 6 | `ImGuiWindowTempData`/存储 helper | fork 用 `window->DC.Layouts`（`ImGuiStorage`）按 id 存 `ImGuiLayout*` | 需新增字段 | 见（A）；另有 `window->DC.LayoutType`/`ParentLayoutType` 赋值在 1.92.8 `Begin()`（8552-8553 已是官方代码） |

> 还需核对 fork 里 `NewFrame()` 是否对布局栈有额外清理（g 全局 reset 部分），以及 `Render()` 是否有布局
> 相关收尾；若有则一并搬。移植后必须跑全量 Editor 验证无回归（见 §7）。

**验收（阶段 1 独立可验）**：`build.bat` 编译通过；Editor 普通流程无回归；写一个 10 行的临时验证
（可选，或直接用 builders 跑通阶段 2 后一起验）用 `BeginVertical`+`Spring`+`BeginHorizontal` 画一组
排布不崩、排布符合预期。**布局引擎改动影响整个 imgui，是本计划最大风险面，优先单独验收。**

### 2.2 utilities 图标工具接入（阶段 2）

拷贝 `utilities/{drawing,drawing,widgets,widgets}.{h,cpp}` → `GE_Editor/src/NodeEditorUtils/`：
- `drawing.{h,cpp}`：`namespace ax::Drawing`，`IconType` 枚举 + `DrawIcon(...)`。纯 ImDrawList 手绘，
  不依赖布局引擎，**原样拷贝不改**。
- `widgets.{h,cpp}`：`namespace ax::Widgets`，`Icon(...)`（光标定位 + `IsRectVisible` + `Dummy`）。
  纯 ImGui 基本 API，**原样拷贝不改**。
- 两者都 `#define IMGUI_DEFINE_MATH_OPERATORS` + include `<imgui.h>`；drawing.cpp 额外 include
  `<imgui_internal.h>`（`ImRect`/`ImColor`）。引擎 imgui 1.92.8 均支持。
- LICENSE 头保留（MIT/public-domain 双许可，注明来源）。

### 2.3 builders 的 BlueprintNodeBuilder 适配（阶段 3）

拷贝 `builders.{h,cpp}` → `GE_Editor/src/NodeEditorUtils/`，**保留接口与命名空间**
（`namespace ax::NodeEditor::Utilities` 的 `BlueprintNodeBuilder`：`Begin/End/Header/EndHeader/
Input/EndInput/Middle/Output/EndOutput`），让别的面板日后可复用 blueprints 调用约定。改动点：
1. **Header 色条不用贴图**：原 `End()` 里 `HeaderTextureId` 分支画纹理贴图（blueprints 用
   `BlueprintBackground.png`）。我们没有该贴图 → 改为当 `HeaderTextureId==nullptr` 时用
   `HeaderColor` 纯色 `AddRectFilled` 上半圆角色条（读 `ed::GetNodeBackgroundDrawList(CurrentNodeId)`）。
   构造默认 `HeaderTextureId=nullptr` 即走纯色路径。
2. `#if IMGUI_VERSION_NUM > 18101` 圆角分支：引擎 imgui 1.92.8 走新分支（`ImDrawFlags_RoundCornersTop`），
   无需改。
3. `PushStyleVar(StyleVar_NodePadding,...)`、`ed::GetNodeBackgroundDrawList` 等接口 0.9.3 可用，不改。
4. `builders.cpp` 依赖布局引擎（Spring/横竖列），因此**必须阶段 1 完成后**才能编译。

### 2.4 ASMGraphPanel 节点渲染改造（阶段 4）

`ASMGraphPanel.{h,cpp}`（`GE_Editor/src/Panels/`）。
- **删除** `DrawStateIcon`/`DrawPinIcon` 手绘（声明+定义）。
- **状态节点**改 `util::BlueprintNodeBuilder`：
  - 依据 `isCurrent`（当前态）/`isInitial`（初始态）/悬停/选中 决定 `Header` 颜色：
    当前=绿、初始=橙、普通=中性深灰；悬停/选中叠加提亮/加粗描边。
  - `builder.Begin(id)` → `builder.Header(color)` + 标题（名称 + ▶/★/`stateTime`，沿用现有文本逻辑）
    → `builder.EndHeader()` → 左 `builder.Input(inPin)` 输入图标+文本 → `builder.EndInput()`
    → 右 `builder.Output(outPin)` 输出图标+文本 → `builder.EndOutput()` → `builder.End()`。
- **ANY 虚拟节点**：独立段，同样 Builder + 蓝色 Header，标注"全局"，输出画 Flow/圆点图标。
- **引脚图标**：`ax::Widgets::Icon`——输入（接收转换）`Circle` 空心灰；输出（发出转换）`Circle`
  实心蓝；ANY 输出用蓝 `Flow`/`RoundSquare`。图标语义沿用现有：当前态高亮、初始态点标记可并入 Header/文字。
- **连线**：`ed::Link` 逻辑与配色不变；连线标签仍用 `ImGui::GetWindowDrawList()`（不回退崩溃修复）。
  标签中点 y 近似（现在 `+32` 硬编码）改为按 Builder 节点实际尺寸（`ed::GetNodeSize` 或 Header 高）取，
  x 维持现有两侧近似。

---

## 3. 分阶段实施与退出标准

### 阶段 1：imgui 栈布局引擎移植
- 1.1 产出 fork 相对上游 1.84 的精确 diff 清单（尤其 6 处核心接入点 + STACK LAYOUT 段），
      核对 1.92.8 对应落点。
- 1.2 `imgui_internal.h`：加 `ImGuiLayoutItemType`/`ImGuiLayoutItem`/`ImGuiLayout` + `CurrentLayout`/
      `CurrentLayoutItem`/`LayoutStack` 字段 + 前置声明。
- 1.3 `imgui.h`：加 7 个公共布局 API 声明。
- 1.4 `imgui.cpp`：搬 STACK LAYOUT 段（static 声明 + 定义 + 公共 API）+ 6 处核心接入。
- 1.5 `build.bat` 编译 + Editor 全量回归（普通场景/窗口/列表面板/ASM 图不含 Builder 现状先不崩）。
**退出标准**：编译通过；Editor 常规流程正常；布局 API 可调用（临时或随阶段 2 验证）。

### 阶段 2：拷贝 drawing/widgets 图标工具
- 2.1 新建 `GE_Editor/src/NodeEditorUtils/`，拷贝 4 文件（原样，含 LICENSE）。
- 2.2 顶 CMake 的 `GE_Editor` 源列表加入 `drawing.cpp`/`widgets.cpp`（不含 builders，阶段 1 完成后再加）。
- 2.3 编译验证（drawing/widgets 不依赖布局，此时即可过）。
**退出标准**：图标工具编译通过；临时用 `Widgets::Icon` 画一个验证不崩、形状正确（可随阶段 4 一起看）。

### 阶段 3：拷贝并适配 builders
- 3.1 拷贝 `builders.{h,cpp}`。
- 3.2 改 Header 贴图分支 → 纯色色条。
- 3.3 CMake 加入 `builders.cpp`；编译（此时依赖阶段 1 布局引擎）。
**退出标准**：builders 编译通过。

### 阶段 4：ASMGraphPanel 用 Builder + Icon 重写节点渲染
- 4.1 删手绘图标；改状态节点/ANY 节点为 Builder 结构。
- 4.2 连线/标签保留；标签中点 y 用节点实际尺寸。
**退出标准**：见 §7 全部验收。

### 阶段 5：收尾
- 5.1 文档更新（本计划书状态 → 已落地）。
- 5.2 全量回归 + 提交（每次改代码后按项目惯例自动暂存提交）。

---

## 4. 后续路线图（本计划范围外）

| 能力 | 触发 | 说明 |
|---|---|---|
| 布局引擎从 fork 换成 imgui 官方将来正式版 | imgui 官方合并 Stack Layout 后 | 目前 1.92.8 只有半成品壳；届时可切官方 API，删本地移植 |
| 其它面板用 BlueprintNodeBuilder | 需要 blueprints 风格节点 | 工具已就位（NodeEditorUtils），直接复用 |

---

## 5. 新增/修改文件清单

| 文件 | 类型 | 改动 |
|---|---|---|
| `GE/third_party/imgui/imgui.h` | 修改 | 新增 7 个公共布局 API 声明（阶段 1） |
| `GE/third_party/imgui/imgui_internal.h` | 修改 | 新增 `ImGuiLayout`/`ImGuiLayoutItem` 结构 + TempData 3 字段 + 前置声明（阶段 1） |
| `GE/third_party/imgui/imgui.cpp` | 修改 | 新增 STACK LAYOUT 实现段 + 6 处核心接入点（阶段 1） |
| `GE_Editor/src/NodeEditorUtils/drawing.{h,cpp}` | 新增 | 拷贝 blueprints utilities（图标原语） |
| `GE_Editor/src/NodeEditorUtils/widgets.{h,cpp}` | 新增 | 拷贝 blueprints utilities（Icon 控件） |
| `GE_Editor/src/NodeEditorUtils/builders.{h,cpp}` | 新增 | 拷贝 + 改 Header 纯色色条 |
| `GE_Editor/src/Panels/ASMGraphPanel.{h,cpp}` | 修改 | 删手绘图标；用 Builder 画节点 + `Widgets::Icon` 画引脚（阶段 4） |
| `CMakeLists.txt` | 修改 | `GE_Editor` 源列表加 6 个 cpp |
| `docs/ASM图编辑器引入BlueprintUtilities计划书.md` | 新增 | 本文档 |

---

## 6. 风险与对策

| 风险 | 等级 | 对策 |
|---|---|---|
| **imgui 内核改动回归**（布局引擎影响全部 imgui 渲染） | **高** | 阶段 1 单独验收；移植点精确对照 fork diff；改动收敛在 STACK LAYOUT 段 + 6 处；Editor 全量回归 |
| fork imgui 1.84 与引擎 1.92.8 API 漂移（`ImFloor`/`IM_FLOOR`、函数签名、`ImGuiItemAddFlags` 等） | 中 | 逐处核对 1.92.8 等价写法；只搬算法不搬版本差异 |
| 1.92.8 官方 LayoutType 半成品与移植引擎并存冲突 | 中 | 只新增 `ImGuiLayout*` 系列字段；`LayoutType`/`ParentLayoutType`（官方 dock/轴向用）**不覆盖**，仅 `ItemSize` 读 `CurrentLayout` 分支前置判断非空 |
| 布局引擎顶点平移（`TranslateLayoutItem` 改 drawlist vtx）依赖 drawlist 状态 | 中 | 忠实搬 fork 实现与调用时机；在节点编辑器内验证排布正确性 |
| builders 与图标 namespace 冲突（`ax::` 是否被别处占用） | 低 | 先 grep 全工程确认无同名冲突再落 |
| 引擎 imgui 与 `GE` 静态库/`Sandbox` 共用 | 低 | imgui 是共享库，改动对 Sandbox 也生效——回归时一并看 |

---

## 7. 验证标准 / 交付验收

1. **构建**：`build.bat`（MSVC + Ninja）编译通过，无新增错误。三个可执行（`Sandbox`/`GE_Editor`/`gemesh`）
   中受 imgui 影响者均正常。
2. **阶段 1 回归**：改 imgui 后先跑既有 Editor 全流程——场景视口渲染、停靠窗口、列表面板
   （含 ASM 列表式 `DrawAnimStateMachine`）、层次面板、Gizmo 拖拽、Play 运行，均无异常/无布局错乱。
3. **阶段 4 主验收**（`assets/scenes/2.scene` FPS 角色，四状态四转换）：
   - 状态节点有 **Header 色条**（圆角卡片），标题区含名称 + ▶/★/stateTime；左侧输入引脚、右侧输出引脚
     用 `IconType` 图标（输入空心圆、输出实心圆，ANY 输出 Flow/蓝）。
   - ANY 虚拟节点蓝色 Header、标"全局"，来自它（`*→jump`/`*→run`/`*→idle`）的连线从 ANY 输出引脚出发。
   - 当前状态节点 Header 绿、初始状态 ★；Play 时 jump 触发瞬间该节点高亮、stateTime 递增。
   - 缩放/平移画布、拖动节点：布局不塌陷、图标不错位、连线跟随引脚。
   - 转换带条件/混合时长时连线标签仍显示且不崩（回归崩溃修复）。
   - 多实体切换坐标不串；重启后位置保持（`asm_graph.json`）。
4. **回归**：列表式 ASM 面板、ASM 运行时求值、场景序列化、Lua `anim.*` 均不变。
5. **图标视觉**：形状清晰、风格与 blueprints 一致；无 DrawList 层级错乱（图标不被节点/连线盖错序）。

---

## 8. 决策记录（编码前拍板）

**8.1 布局引擎来源 —— ✅ 已定：从 node-editor 自带 fork imgui 1.84 移植，不用 imgui 官方半成品**
- 引擎 imgui 1.92.8 只有 `LayoutType`/`BackupLayout` 壳 + 注释"FIXME: in development, not exposed"，
  无可用实现。blueprints 依赖的完整 `Spring`/`BeginHorizontal` 只存在于 fork 1.84。因此移植 fork 的
  STACK LAYOUT 段 + 6 处核心接入。

**8.2 移植粒度 —— ✅ 已定：整块搬 STACK LAYOUT + 最小核心接入；公共 API 按需最小（7 个）**
- 不搬 fork 与布局无关的其它改动；不搬 `SuspendLayout`/`ResumeLayout`/int 重载（ASM/builders 未用到，
  需要时再补）。算法忠实、逐行搬，适配只做版本差异。

**8.3 builders Header —— ✅ 已定：改纯色色条（不用贴图），保留类接口与 namespace**
- 仓库无 `BlueprintBackground.png` 资源管线；用 `HeaderColor` 纯色 + `AddRectFilled` 上半圆角即可，
  并保留无 Header 分支（ANY 简化场景）。未来要贴图再走 `HeaderTextureId` 分支。

**8.4 图标语义 —— ✅ 已定：沿用 ASM 语义配色，换用 IconType 形状**
- 输入(收转换)=空心圆，输出(发转换)=实心圆/Flow；ANY=蓝 Flow。当前态/初始态用 Header 色与文字标记表达。

---

## 9. 与既有链路的关系（改动面收敛）

```
GE/third_party/imgui   ← 阶段1 新增布局 API（唯一"内核"改动，全程序共享，回归需覆盖 Sandbox）
   ▲ 链接
GE_Editor/src/NodeEditorUtils/{drawing,widgets,builders}   ← 阶段2/3 新工具源（仅 Editor 编译）
   ▲ include/调用
ASMGraphPanel 节点渲染（重写，Builder+Icon）   ← 阶段4，纯 Editor 视图层
AnimStateMachineComponent / 求值内核 / 序列化 / Lua   ← 零改动
```

本计划改动面上游至共享 imgui（风险最高、最先单独验收），下游止于 ASM 面板的节点绘制（不触数据与运行时）。
`GE` 引擎库不感知 `NodeEditorUtils`；布局 API 虽加在 imgui 内但对既有调用是纯增量（新增 API 不改变
既有函数行为；接入点均为"非空则额外记录"的前置判断）。
