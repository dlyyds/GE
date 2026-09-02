# imgui-node-editor 0.9.3 API 参考

> 本文档根据 `GE/third_party/imgui-node-editor-0.9.3/` 自带示例与头文件整理，供 ASM 节点图编辑器（见《动画状态机节点图编辑器计划书》）以及后续所有基于该库的图界面开发使用。
> 主要来源：`examples/{simple,basic-interaction,blueprints,widgets,canvas}-example/`、`imgui_node_editor.h`、`imgui_canvas.h`。
> 命名空间：`namespace ed = ax::NodeEditor;`（正文省略 `ed::` 前缀）。Canvas 在 `ImGuiEx::Canvas`。

---

## 0. 全景：一个编辑器实例的生命周期

```
1. ed::Config config; 填配置（设置文件、节点状态回调…）
2. ed::EditorContext* ctx = ed::CreateEditor(&config);
3. 每帧：
   ed::SetCurrentEditor(ctx);
   if (ed::Begin("画布标题", size)) {
       提交数据：ed::BeginNode / BeginPin / Link …
       处理交互：ed::BeginCreate / BeginDelete / 菜单 …
   }
   ed::End();
   ed::SetCurrentEditor(nullptr);
4. 结束：ed::DestroyEditor(ctx);
```

核心思路：**本库不持有你的数据**。每帧你把节点/引脚/连线的 ID 与几何提交给编辑器（`BeginNode`/`BeginPin`/`Link`），编辑器返回交互意图（要建连线？要删东西？选中了谁？），由你据此增删自己的数据结构。ID 是唯一纽带。

---

## 1. 上下文管理与全局状态

| API | 说明 |
|---|---|
| `EditorContext* CreateEditor(const Config* config = nullptr)` | 创建编辑器上下文。一个上下文对应一个独立图（保存独立的画布位置、缩放、节点布局） |
| `void DestroyEditor(EditorContext* ctx)` | 销毁上下文，`OnStop`/窗口关闭时调用 |
| `void SetCurrentEditor(EditorContext* ctx)` | 设定"当前编辑器"。`Begin`/`End` 期间的所有调用作用于此上下文 |
| `EditorContext* GetCurrentEditor()` | 取当前编辑器 |
| `const Config& GetConfig(EditorContext* ctx = nullptr)` | 取配置，`nullptr` 表示当前编辑器 |

**要点**：
- 多个面板共存时（如编辑器里同时开几个图），用 `SetCurrentEditor` 切换当前上下文。忘记设回 `nullptr` 只影响帧内，但规范做法是帧尾设回。
- `CreateEditor` 只要 `config` 里给了 `SettingsFile`，画布平移/缩放与节点坐标就会自动落盘，无需手写序列化。

### 1.1 `Config` 配置

| 字段 | 类型/默认 | 说明 |
|---|---|---|
| `SettingsFile` | `const char*`，`"NodeEditor.json"` | 编辑器自带状态（视口、节点位置等）的落盘文件名 |
| `UserPointer` | `void*` | 透传给各回调的指针（示例传 `this`） |
| `SaveSettings` / `LoadSettings` | 回调 | 全局设置读写（默认走 SettingsFile，传了可接管） |
| `SaveNodeSettings` / `LoadNodeSettings` | 回调 | **节点自定义数据**的读写（见 §1.2） |
| `BeginSaveSession` / `EndSaveSession` | 回调 | 一次批量保存会话的开关（可借此做原子写文件） |
| `CustomZoomLevels` | `ImVector<float>` | 自定义缩放档位 |
| `CanvasSizeMode` | `CanvasSizeMode::FitVerticalView` | 画布大小变化时视口适配方式（FitVerticalView / FitHorizontalView / CenterOnly） |
| `DragButtonIndex` | `0` | 拖动画布用的鼠标键（0 左 1 右 2 中） |
| `SelectButtonIndex` | `0` | 框选用的鼠标键 |
| `NavigateButtonIndex` | `1` | 导航（平移）用鼠标键 |
| `ContextMenuButtonIndex` | `1` | 右键菜单触发键 |

```cpp
ed::Config config;
config.SettingsFile   = "ASMGraph.json";
config.UserPointer    = this;
config.CanvasSizeMode = ed::CanvasSizeMode::FitVerticalView;
m_Context = ed::CreateEditor(&config);
```

### 1.2 节点自定义状态：`SaveNodeSettings` / `LoadNodeSettings`

用于把节点附加数据（比如 Blueprint 节点的"状态字符串"）随编辑器一起持久化。回调签名（`NodeId` 与 `SaveReasonFlags` 见 §8、§1.3）：

```cpp
config.SaveNodeSettings = [](ed::NodeId nodeId, const char* data, size_t size, ed::SaveReasonFlags reason, void* userPointer) -> bool
{
    auto self = static_cast<Example*>(userPointer);
    auto node = self->FindNode(nodeId);
    if (!node) return false;
    node->State.assign(data, size);   // 把回调收到的 data 存进自己的节点结构
    self->TouchNode(nodeId);          // 高亮该节点（见 §2.3 Flow）
    return true;
};

config.LoadNodeSettings = [](ed::NodeId nodeId, char* data, void* userPointer) -> size_t
{
    auto self = static_cast<Example*>(userPointer);
    auto node = self->FindNode(nodeId);
    if (!node) return 0;
    if (data != nullptr)              // data 为空 = 问大小
        memcpy(data, node->State.data(), node->State.size());
    return node->State.size();
};
```

配套 API：`void RestoreNodeState(NodeId nodeId)`——让编辑器重新对该节点执行一次 `LoadNodeSettings`（示例中"恢复保存的状态"按钮用）。
保存原因位 `SaveReasonFlags`：`None/Navigation/Position/Size/Selection/AddNode/RemoveNode/User`，可按位与判断保存时机。

---

## 2. 核心绘制 API

### 2.1 画布开闭

| API | 说明 |
|---|---|
| `void Begin(const char* id, const ImVec2& size = ImVec2(0, 0))` | 开始编辑器画布；`size` 传 `(0,0)` 表示占满剩余区域 |
| `void End()` | 结束画布。**必须配对**，否则下帧异常 |

```cpp
ed::SetCurrentEditor(m_Context);
ed::Begin("ASM Graph", ImVec2(0, 0));
// …提交节点/引脚/连线 + 处理交互…
ed::End();
ed::SetCurrentEditor(nullptr);
```

### 2.2 节点 / 引脚 / 连线

| API | 说明 |
|---|---|
| `void BeginNode(NodeId id)` | 开始绘制一个节点。**必须配合 `ImGui::PushID(id.AsPointer())` 使用**（多个节点共享 ID 栈时防止冲突） |
| `void EndNode()` | 结束节点 |
| `void BeginPin(PinId id, PinKind kind)` | 开始引脚。`PinKind::Input` 输入 / `PinKind::Output` 输出。引脚内部可以放任意 ImGui 控件 |
| `void EndPin()` | 结束引脚 |
| `void PinRect(const ImVec2& a, const ImVec2& b)` | 手动指定引脚的可点击/吸附矩形（默认为引脚内控件包围盒） |
| `void PinPivotRect(const ImVec2& a, const ImVec2& b)` | 指定连线锚点矩形（连线起止点） |
| `void PinPivotSize(const ImVec2& size)` | 设置连线锚点尺寸 |
| `void PinPivotScale(const ImVec2& scale)` | 连线锚点缩放 |
| `void PinPivotAlignment(const ImVec2& alignment)` | 连线锚点对齐（`(1,0.5)` 右中，用于右侧输出引脚） |
| `void Group(const ImVec2& size)` | 在节点内放置一块可交互区域（注释节点、分组框用） |
| `bool Link(LinkId id, PinId startPinId, PinId endPinId, const ImVec4& color = 白, float thickness = 1.0f)` | 提交一条连线。参数顺序固定为 **输入引脚 → 输出引脚** |
| `void Flow(LinkId linkId, FlowDirection dir = Forward)` | 让连线出现流动动画（数据流/执行流指示）。`FlowDirection::Forward/Backward` |

**简单节点写法**：

```cpp
ed::BeginNode(nodeA_Id);
    ImGui::Text("Node A");
    ed::BeginPin(nodeA_InputPinId, ed::PinKind::Input);
        ImGui::Text("-> In");
    ed::EndPin();
    ImGui::SameLine();
    ed::BeginPin(nodeA_OutputPinId, ed::PinKind::Output);
        ImGui::Text("Out ->");
    ed::EndPin();
ed::EndNode();
```

**连线写法**：每帧为每条存活的连线调用一次 `ed::Link(id, inputPinId, outputPinId)`。

```cpp
for (auto& link : m_Links)
    ed::Link(link.Id, link.InputId, link.OutputId, link.Color, 2.0f);
```

**线框技巧**（连线起点贴引脚、控制方向）：
- `ed::PushStyleVar(ed::StyleVar_SourceDirection, ImVec2(0,1))` + `TargetDirection(0,-1)`：竖排节点间连线上下走。
- `ed::PushStyleVar(ed::StyleVar_LinkStrength, 0.0f)`：连线完全拉直（紧贴引脚方向）。
- `PinPivotAlignment` 控制连线锚在引脚内左右/上下位置。

### 2.3 节点上的图标与装饰

blueprints 示例提供了一套可复用的图标绘制工具（`examples/blueprints-example/utilities/`），节点标题、引脚图标都靠它：

```cpp
// ax::Drawing::IconType
enum class IconType : ImU32 { Flow, Circle, Square, Grid, RoundSquare, Diamond };

// ax::Widgets::Icon —— 在节点内画一个引脚类型图标，返回前占位 Dummy(size)
void Icon(const ImVec2& size, IconType type, bool filled,
          const ImVec4& color = 白, const ImVec4& innerColor = 黑);
```

用法（blueprints `DrawPinIcon`）：按引脚类型选 IconType，`filled` 表示实心，`connected` 状态决定内圆是否填充，alpha 随悬停/合法性变化。

```cpp
ax::Widgets::Icon(ImVec2(m_PinIconSize, m_PinIconSize), iconType,
                  connected, color, ImColor(32, 32, 32, alpha));
```

### 2.4 注释 / 分组框 / 分组标题

| API | 说明 |
|---|---|
| `bool BeginGroupHint(NodeId nodeId)` | 开始"分组提示"（注释节点上方的标题浮标）。返回 true 才画内容 |
| `void EndGroupHint()` | 结束 |
| `ImVec2 GetGroupMin()` / `GetGroupMax()` | 分组框的包围盒（用于定位标题） |
| `ImDrawList* GetHintForegroundDrawList()` / `GetHintBackgroundDrawList()` | 在分组提示的前/背景层绘制（浮标不随节点缩放） |
| `ImDrawList* GetNodeBackgroundDrawList(NodeId nodeId)` | 节点背景绘制层（在 ImGui 内容之下画背景/描边） |

```cpp
ed::BeginNode(node.ID);
    ImGui::TextUnformatted(node.Name.c_str());
    ed::Group(node.Size);          // 注释节点用 Group 撑起可选中/可拖拽的矩形区域
ed::EndNode();

if (ed::BeginGroupHint(node.ID)) {
    auto min = ed::GetGroupMin();
    ImGui::SetCursorScreenPos(min - ImVec2(-8, ImGui::GetTextLineHeightWithSpacing() + 4));
    ImGui::TextUnformatted(node.Name.c_str());
    auto drawList = ed::GetHintBackgroundDrawList();
    // 用 drawList 画标题背景框…
    ed::EndGroupHint();
}
```

### 2.5 节点位置 / 尺寸 / 层级

| API | 说明 |
|---|---|
| `void SetNodePosition(NodeId id, const ImVec2& pos)` | 设置节点坐标（**首帧摆放、新建节点、重新布局必用**） |
| `ImVec2 GetNodePosition(NodeId id)` | 取节点坐标 |
| `ImVec2 GetNodeSize(NodeId id)` | 取节点尺寸（需已 `BeginNode`/`EndNode` 提交过） |
| `void SetGroupSize(NodeId id, const ImVec2& size)` | 设置分组（注释节点）尺寸 |
| `void CenterNodeOnScreen(NodeId id)` | 把节点居中到视口 |
| `void SetNodeZPosition(NodeId id, float z)` / `GetNodeZPosition` | 节点 Z 序（大者在上） |

**首帧摆放惯用法**：用一个 `firstFrame` 标记，仅首帧对所有节点调用 `SetNodePosition`（后续交给用户拖拽 + 设置文件持久化）。

```cpp
if (m_FirstFrame) {
    ed::SetNodePosition(nodeA_Id, ImVec2(10, 10));
    ed::SetNodePosition(nodeB_Id, ImVec2(210, 60));
}
// 之后每帧正常 BeginNode/EndNode 提交即可
```

### 2.6 画布导航 / 坐标换算

| API | 说明 |
|---|---|
| `void NavigateToContent(float duration = -1)` | 缩放到刚好容纳全部内容（`-1` 用样式的 ScrollDuration）。「重新布局/回到内容」按钮用 |
| `void NavigateToSelection(bool zoomIn = false, float duration = -1)` | 缩放到选中内容 |
| `float GetCurrentZoom()` | 当前缩放 |
| `ImVec2 ScreenToCanvas(const ImVec2& pos)` / `CanvasToScreen` | 屏幕坐标 ↔ 画布坐标（节点坐标是画布坐标） |
| `ImVec2 GetScreenSize()` | 当前编辑器视口屏幕尺寸 |

---

## 3. 交互：创建 / 删除

### 3.1 建连 / 建节点（`BeginCreate` 块）

创建动作用一个块包裹：`BeginCreate()` 返回 true 表示用户正在拖新东西；块内用 `QueryNewLink` / `QueryNewNode` 询问意图，`AcceptNewItem` / `RejectNewItem` 决定收不收；`EndCreate()` 收尾。

| API | 说明 |
|---|---|
| `bool BeginCreate(const ImVec4& color = 白, float thickness = 1.0f)` | 开始创建流程（可指定连线配色） |
| `bool QueryNewLink(PinId* startId, PinId* endId)` | 用户拖出的候选连线。**返回值语义**：`(input,output)` 哪种有效不确定，需自校验（见下） |
| `bool QueryNewNode(PinId* pinId)` | 用户从空白处拖出（想创建节点） |
| `bool AcceptNewItem(const ImVec4& color, float thickness = 1.0f)` | **用户松手确认**（返回 true）时才真正落库；带配色版可给吸附高亮 |
| `void RejectNewItem(const ImVec4& color, float thickness = 1.0f)` | 拒绝这次创建（视觉上把线变红/变细） |
| `void EndCreate()` | 结束创建流程 |

**QueryNewLink 的三种情形**（决定你校验的写法）：
- `input 有效, output 无效`：用户从输入引脚开始拖 → 输出引脚是你接下来要允许的目标。
- `input 无效, output 有效`：用户从输出引脚开始拖。
- 两者都有效：候选落到了某个引脚上，此时做**合法性校验**。

标准校验写法（blueprints）：把方向统一成 输出→输入，再按类型/种类过滤，非法就 `RejectNewItem` 并提示。

```cpp
if (ed::BeginCreate(ImColor(255,255,255), 2.0f)) {
    ed::PinId startPinId = 0, endPinId = 0;
    if (ed::QueryNewLink(&startPinId, &endPinId)) {
        // 统一成 输出→输入
        if (startPinId.Kind == PinKind::Input) { std::swap(startPin, endPin); std::swap(startPinId, endPinId); }
        if (endPin->Kind == startPin->Kind)        { ed::RejectNewItem(ImColor(255,0,0), 2.0f); }
        else if (endPin->Type != startPin->Type)   { ed::RejectNewItem(ImColor(255,128,128), 1.0f); }
        else if (ed::AcceptNewItem(ImColor(128,255,128), 4.0f)) {
            m_Links.emplace_back(Link(GetNextId(), startPinId, endPinId));
        }
    }
    ed::PinId pinId = 0;
    if (ed::QueryNewNode(&pinId)) {
        if (ed::AcceptNewItem()) { /* 打开"新建节点"菜单 */ }
    }
}
ed::EndCreate();
```

**注意**：只有 `AcceptNewItem` 返回 true 才是用户真正松手，此时才写入你的数据结构；否则每帧都会重新进入 `QueryNewLink`。

### 3.2 删除（`BeginDelete` 块）

| API | 说明 |
|---|---|
| `bool BeginDelete()` | 开始删除流程 |
| `bool QueryDeletedLink(LinkId* linkId, PinId* start = nullptr, PinId* end = nullptr)` | 枚举待删连线；可带出起止引脚 |
| `bool QueryDeletedNode(NodeId* nodeId)` | 枚举待删节点 |
| `bool AcceptDeletedItem(bool deleteDependencies = true)` | 同意删除（默认连带删依赖） |
| `void RejectDeletedItem()` | 拒绝删除 |
| `void EndDelete()` | 结束删除流程 |

删除来源：用户按 Del 键（+ 选中了节点/连线）、或你在右键菜单里调 `DeleteNode`/`DeleteLink`（见 §5）。删除是**请求式**的——编辑器只会询问，实际从你的数据里清除由你完成。

```cpp
if (ed::BeginDelete()) {
    ed::NodeId nodeId = 0;
    while (ed::QueryDeletedNode(&nodeId))
        if (ed::AcceptDeletedItem()) { /* 从 m_Nodes 里 erase(nodeId) */ }

    ed::LinkId linkId = 0;
    while (ed::QueryDeletedLink(&linkId))
        if (ed::AcceptDeletedItem()) { /* 从 m_Links 里 erase(linkId) */ }
}
ed::EndDelete();
```

### 3.3 拖出建节点时弹出的菜单（配合 Suspend/Resume）

用户从空白/引脚拖出 → `QueryNewNode` → `AcceptNewItem` → 弹"新建节点"右键菜单。**菜单必须画在画布坐标之外**，因此用 `Suspend`/`Resume` 包住（见 §4）：

```cpp
if (ed::AcceptNewItem()) {
    createNewNode  = true;
    newNodeLinkPin = FindPin(pinId);
    ed::Suspend();
    ImGui::OpenPopup("Create New Node");
    ed::Resume();
}
// 之后在 Suspend 块内画 BeginPopup("Create New Node") 菜单
```

新建节点时若 `newNodeLinkPin` 非空（从某引脚拖出），自动连一条能连上的线：

```cpp
if (auto startPin = newNodeLinkPin) {
    auto& pins = startPin->Kind == PinKind::Input ? node->Outputs : node->Inputs;
    for (auto& pin : pins)
        if (CanCreateLink(startPin, &pin)) { /* 建 Link(startPin, &pin) */ break; }
}
```

---

## 4. 弹出画布（Suspend / Resume）与画布外 UI

节点内部只能画常规 ImGui 控件，**弹窗/右键菜单/悬浮工具提示这类"覆盖层"必须用 `Suspend`/`Resume` 退出画布坐标空间**，否则会画到错误位置或崩溃。

| API | 说明 |
|---|---|
| `void Suspend()` | 退出画布虚拟坐标，回到正常 ImGui 屏幕空间 |
| `void Resume()` | 重新进入画布坐标。**必须与 Suspend 配对**（可嵌套） |
| `bool IsSuspended()` | 当前是否处于挂起状态 |

```cpp
ed::Suspend();
if (do_popup) { ImGui::OpenPopup("popup_button"); do_popup = false; }
if (ImGui::BeginPopup("popup_button")) { /* 菜单项… */ ImGui::EndPopup(); }
if (do_tooltip) ImGui::SetTooltip("I am a tooltip");
ed::Resume();
```

**节点内控件注意**（widgets 示例总结，与图库自身 API 无关但很关键）：
- 输入框类控件（`InputText`/`InputFloat`/`DragFloat`）**必须**配合 `ed::EnableShortcuts(!io.WantTextInput)` 关闭编辑器快捷键，否则输入时画布会飞。
- 下拉/列表/`BeginPopup` 类必须走 Suspend/Resume 延迟弹出。
- `CollapsingHeader`/`TreeNode` 需用 ImGui 列（`BeginColumns` + `SetColumnWidth`）限定宽度才能正常显示。
- 可滚动 Child 窗口、自动弹 Tooltip 的图形控件（如 Plot）在节点内**不可用**。

---

## 5. 右键菜单

| API | 说明 |
|---|---|
| `bool ShowNodeContextMenu(NodeId* nodeId)` | 当前右键命中的节点 ID（未命中返回 false） |
| `bool ShowPinContextMenu(PinId* pinId)` | 右键命中的引脚 |
| `bool ShowLinkContextMenu(LinkId* linkId)` | 右键命中的连线 |
| `bool ShowBackgroundContextMenu()` | 右键落在空白处 |

这些查询也要放在 `Suspend`/`Resume` 内，随后 `ImGui::OpenPopup` 弹真正的菜单。

```cpp
ed::Suspend();
if (ed::ShowNodeContextMenu(&contextNodeId))
    ImGui::OpenPopup("Node Context Menu");
else if (ed::ShowBackgroundContextMenu())
    ImGui::OpenPopup("Create New Node");
ed::Resume();

ed::Suspend();
if (ImGui::BeginPopup("Node Context Menu")) {
    if (ImGui::MenuItem("Delete"))
        ed::DeleteNode(contextNodeId);   // 触发删除流程，见 BeginDelete
    ImGui::EndPopup();
}
ed::Resume();
```

配套的强制删除 API：
- `bool DeleteNode(NodeId nodeId)` / `bool DeleteLink(LinkId linkId)`：直接请求删除（等价于用户在图上按 Del）。之后仍需在 `BeginDelete` 块里 `AcceptDeletedItem` 真正清掉数据。

---

## 6. 选择 / 高亮 / 悬停查询

### 6.1 选择

| API | 说明 |
|---|---|
| `bool HasSelectionChanged()` | 本帧选择是否有变化（用于刷新侧栏等） |
| `int GetSelectedObjectCount()` | 选中对象总数（节点+连线） |
| `int GetSelectedNodes(NodeId* nodes, int size)` | 填充选中的节点 ID 数组，返回数量 |
| `int GetSelectedLinks(LinkId* links, int size)` | 填充选中的连线 ID |
| `bool IsNodeSelected(NodeId id)` / `IsLinkSelected(LinkId id)` | 单个查询 |
| `void ClearSelection()` | 清空全部选中 |
| `void SelectNode(NodeId id, bool append = false)` / `SelectLink` | 追加/单选地选中 |
| `void DeselectNode(NodeId id)` / `DeselectLink` | 取消选中 |

```cpp
// 读取选中，先按总数 resize 再截断（blueprints ShowLeftPane 惯用法）
std::vector<ed::NodeId> nodes;  nodes.resize(ed::GetSelectedObjectCount());
std::vector<ed::LinkId> links;  links.resize(ed::GetSelectedObjectCount());
int nodeCount = ed::GetSelectedNodes(nodes.data(), (int)nodes.size());
int linkCount = ed::GetSelectedLinks(links.data(), (int)links.size());
nodes.resize(nodeCount); links.resize(linkCount);
```

### 6.2 悬停 / 点击查询

| API | 说明 |
|---|---|
| `NodeId GetHoveredNode()` / `PinId GetHoveredPin()` / `LinkId GetHoveredLink()` | 悬停对象 |
| `NodeId GetDoubleClickedNode()` / `PinId …` / `LinkId …` | 双击对象 |
| `bool IsBackgroundClicked()` / `IsBackgroundDoubleClicked()` | 空白处单击/双击 |
| `ImGuiMouseButton GetBackgroundClickButtonIndex()` | 点击的鼠标键（无则 -1） |
| `bool GetLinkPins(LinkId id, PinId* start, PinId* end)` | 取连线两端引脚（某端可传 nullptr） |
| `bool PinHadAnyLinks(PinId id)` | 引脚是否曾有过连线 |
| `bool HasAnyLinks(NodeId id)` / `HasAnyLinks(PinId id)` | 节点/引脚当前是否有连线 |
| `int BreakLinks(NodeId id)` / `BreakLinks(PinId id)` | 断开全部连线，返回断开条数 |
| `bool IsActive()` | 当前帧编辑器是否处于交互状态 |

---

## 7. 快捷键

编辑器内置 复制/剪切/粘贴/复制拖拽 等快捷键处理，但**动作要不要执行由你决定**（回调式）：

| API | 说明 |
|---|---|
| `void EnableShortcuts(bool enable)` | 开关快捷键（输入框聚焦时应关，见 §4） |
| `bool AreShortcutsEnabled()` | 查询是否开启 |
| `bool BeginShortcut()` | 开始快捷键处理块；返回 false 表示没有快捷键动作 |
| `bool AcceptCut()` / `AcceptCopy()` / `AcceptPaste()` / `AcceptDuplicate()` / `AcceptCreateNode()` | 依次询问"用户按下了哪个快捷键"，返回 true 的那个就是要处理的 |
| `int GetActionContextSize()` | 快捷键作用对象数 |
| `int GetActionContextNodes(NodeId* nodes, int size)` / `GetActionContextLinks` | 作用对象 ID 列表 |
| `void EndShortcut()` | 结束快捷键块 |

```cpp
if (ed::BeginShortcut()) {
    if (ed::AcceptCopy()) {
        int n = ed::GetActionContextSize();
        ed::NodeId* ids; // 需要调用方分配：GetActionContextNodes(ids, n)
        // 把选中的节点复制到你的剪贴板数据结构…
    }
}
ed::EndShortcut();
```

---

## 8. ID 类型

节点/引脚/连线用三个强类型 ID，底层都是 `uintptr_t`：

```cpp
struct NodeId final : SafePointerType<NodeId> {};   // 可隐式构造自整数/指针
struct PinId  final : SafePointerType<PinId>  {};
struct LinkId final : SafePointerType<LinkId> {};
```

常用成员/操作：
- `NodeId(123)`、`NodeId(void*)` 构造；`id.AsPointer()` 取回 `void*`。
- `operator bool()`：`id` 是否有效（`0` 为无效）。`FindPin(id)` 前应先判 `if (!id) return nullptr;`。
- 相等/不等比较运算符；可作 `std::map` 键——`ed::NodeId` 是强类型无默认 `<`，示例自定义 `NodeIdLess` 用 `lhs.AsPointer() < rhs.AsPointer()` 排序。
- 连续 ID：`int uniqueId = 1;` 自增即可（`BeginNode(uniqueId++)`）。真实项目常用数据指针做 ID。

**多实体共享同一上下文时，NodeId 必须全局唯一。** 本仓库 ASM 图的编码方案（详见《动画状态机节点图编辑器计划书》§2.2）：三个 ID 都是 64 bit 位域，按**命名空间**严格分区，区间互不相交，**绝对不冲突**。

```
实体 id (32 bit)  |  命名空间 (16 bit)  |  下标 (16 bit)
└──── 高 32 位 ────┘  └── 16 位字段 ────┘  └─ 低 16 位 ─┘
```

| ID 类型 | 命名空间 | 位布局 `(实体id<<32) \| (命名空间<<16) \| 下标` | 区间（同实体） |
|---|---|---|---|
| ANY 节点 | `0` | `实体id<<32` ｜ `0` ｜ `0` | `0x0000_0000` |
| 状态节点 | `1` | `实体id<<32` ｜ `1` ｜ `状态下标` | `0x0001_0000 ~ 0x0001_FFFF` |
| 输出引脚 | `2` | `实体id<<32` ｜ `2` ｜ `状态下标` | `0x0002_0000 ~ 0x0002_FFFF` |
| 输入引脚 | `3` | `实体id<<32` ｜ `3` ｜ `状态下标` | `0x0003_0000 ~ 0x0003_FFFF` |
| 连线 LinkId | — | 转换下标 `j`（每帧按 `transitions` 重建） | — |

```cpp
// 状态节点：实体 5、状态 7
EncodeNodeId(5, 7)          →  0x00000005_0001_0007
// 输出引脚：同一实体同一状态，仅命名空间不同
EncodePinId(5, 7, true)     →  0x00000005_0002_0007
```

- 命名空间 `1/2/3` 把状态节点 / 输出引脚 / 输入引脚分成三段区间，**哪怕状态下标相同，NodeId 与 PinId 也互不相等**。
- ANY 虚拟节点单独占命名空间 `0`，不挤占状态下标；其输出引脚仍走输入/输出引脚区（命名空间 `2`）。
- 状态下标用 `0xFFFF` 掩码封顶（16 bit），单实体状态数上限 65536，多实体靠高 32 位实体 id 隔离。

---

## 9. 样式（Style）

### 9.1 读取 / 重置

| API | 说明 |
|---|---|
| `Style& GetStyle()` | 取当前样式（`GetStyle().Colors[StyleColor_Bg]`、`NodePadding`、`LinkStrength` 等） |
| `const char* GetStyleColorName(StyleColor idx)` | 颜色名（样式编辑面板遍历用） |

```cpp
auto& style = ed::GetStyle();
style = ed::Style();                      // 一键重置为默认
auto bg = ed::GetStyle().Colors[ed::StyleColor_NodeBg];
```

### 9.2 栈式覆盖（作用域内临时改）

与 ImGui 的 Push/Pop 同思路，成对使用：

| API | 说明 |
|---|---|
| `void PushStyleColor(StyleColor idx, const ImVec4& color)` / `PopStyleColor(int count = 1)` | 颜色 |
| `void PushStyleVar(StyleVar var, float value)` / `(var, ImVec2)` / `(var, ImVec4)` / `PopStyleVar(int count = 1)` | 变量（重载按类型） |

```cpp
ed::PushStyleColor(ed::StyleColor_NodeBg,     ImColor(128, 128, 128, 200));
ed::PushStyleColor(ed::StyleColor_NodeBorder, ImColor(32,  32,  32,  200));
ed::PushStyleVar(ed::StyleVar_NodePadding,    ImVec4(0, 0, 0, 0));
ed::PushStyleVar(ed::StyleVar_NodeRounding,   5.0f);
ed::PushStyleVar(ed::StyleVar_SourceDirection, ImVec2(0.0f, 1.0f));
ed::BeginNode(node.ID); … ed::EndNode();
ed::PopStyleVar(7);      // 数量与 Push 对应
ed::PopStyleColor(4);
```

### 9.3 颜色与变量清单

`StyleColor`（枚举，`StyleColor_Count` 结尾）：`Bg, Grid, NodeBg, NodeBorder, HovNodeBorder, SelNodeBorder, NodeSelRect, NodeSelRectBorder, HovLinkBorder, SelLinkBorder, HighlightLinkBorder, LinkSelRect, LinkSelRectBorder, PinRect, PinRectBorder, Flow, FlowMarker, GroupBg, GroupBorder`。

`StyleVar`（枚举，`StyleVar_Count` 结尾），常用项与取值类型：

| 变量 | 类型 | 默认 | 用途 |
|---|---|---|---|
| `NodePadding` | `ImVec4` | (8,8,8,8) | 节点内边距 |
| `NodeRounding` | `float` | 12 | 节点圆角 |
| `NodeBorderWidth` / `HoveredNodeBorderWidth` / `SelectedNodeBorderWidth` | `float` | 1.5/3.5/3.5 | 描边宽 |
| `HoveredNodeBorderOffset` / `SelectedNodeBorderOffset` | `float` | 0 | 悬停/选中描边外扩 |
| `PinRounding` / `PinBorderWidth` | `float` | 4/0 | 引脚圆角/描边 |
| `LinkStrength` | `float` | 100 | 连线弯曲度 |
| `SourceDirection` / `TargetDirection` | `ImVec2` | (1,0) / (-1,0) | 出线/进线方向 |
| `ScrollDuration` | `float` | 0.35 | 导航动画时长 |
| `FlowMarkerDistance` / `FlowSpeed` / `FlowDuration` | `float` | 30/150/2 | 流动动画参数 |
| `PivotAlignment` / `PivotSize` / `PivotScale` | `ImVec2` | (0.5,0.5)/(0,0)/(1,1) | 连线锚点 |
| `PinCorners` | `float` | 全圆角 | 引脚圆角角位（ImDrawFlags） |
| `PinRadius` / `PinArrowSize` / `PinArrowWidth` | `float` | 0/0/0 | 引脚箭头样式 |
| `GroupRounding` / `GroupBorderWidth` | `float` | 6/1 | 分组框 |
| `HighlightConnectedLinks` / `SnapLinkToPinDir` | `float` | 0/0 | 高亮相连连线 / 连线贴引脚方向 |

---

## 10. 其他查询 / 工具

| API | 说明 |
|---|---|
| `int GetNodeCount()` | 自 `Begin()` 以来提交的节点数 |
| `int GetOrderedNodeIds(NodeId* nodes, int size)` | 按绘制顺序填充节点 ID，返回实际个数（画"序号徽标"用，见 blueprints `ShowOrdinals`） |

```cpp
int n = ed::GetNodeCount();
std::vector<ed::NodeId> ordered(n);
ed::GetOrderedNodeIds(ordered.data(), n);
for (auto& id : ordered) {
    auto p = ed::CanvasToScreen(ed::GetNodePosition(id));   // 画布→屏幕坐标画徽标
    // drawList->AddText(...)
}
```

---

## 11. Canvas（`ImGuiEx::Canvas`）——通用无限画布

`imgui_canvas.h` 提供不依赖节点语义的**通用缩放画布**：平移、缩放、子坐标空间。节点编辑器内部就是用它实现的。适合：网格背景、时间线、拓扑查看器等。

| API | 说明 |
|---|---|
| `bool Begin(const char* id / ImGuiID id, const ImVec2& size)` | 进入画布。返回 false = 不可见可跳过；true 则必须 `End()`。`size` 任一分量为 0 表示该轴撑满剩余区域 |
| `void End()` | 离开画布 |
| `void SetView(const ImVec2& origin, float scale)` / `SetView(const CanvasView&)` | 设定视口（原点 + 缩放） |
| `void CenterView(const ImVec2& canvasPoint)` | 居中到某点（不改缩放） |
| `CanvasView CalcCenterView(const ImVec2&)` | 计算视口（不应用） |
| `void CenterView(const ImRect& rect)` / `CalcCenterView(const ImRect&)` | 居中并缩放以容纳某矩形 |
| `void Suspend()` / `Resume()` / `bool IsSuspended()` | 退出/重入画布坐标（同上，可嵌套） |
| `ImVec2 FromLocal(const ImVec2&)` / `FromLocalV` | 画布坐标 → 屏幕（点/向量） |
| `ImVec2 ToLocal(const ImVec2&)` / `ToLocalV` | 屏幕 → 画布 |
| `const ImRect& Rect()` | 画布控件包围盒（`Begin` 后可查） |
| `const ImRect& ViewRect()` | 当前可见区域（画布坐标） |
| `const CanvasView& View()` / `ViewOrigin()` / `ViewScale()` | 当前视口 |
| `ImRect CalcViewRect(const CanvasView&)` | 计算某视口的可见区域 |

```cpp
static ImGuiEx::Canvas canvas;
if (canvas.Begin("##mycanvas", ImVec2(0, 0))) {   // 占满剩余
    auto origin = canvas.ViewOrigin();
    auto scale  = canvas.ViewScale();

    // 中键拖拽平移
    if (ImGui::IsItemHovered() && ImGui::IsMouseDragging(1, 0.0f))
        canvas.SetView(origin + ImGui::GetMouseDragDelta(1, 0.0f) * scale, scale);
    // 滚轮缩放
    //   scale = clamp(scale * (1 + io.MouseWheel * k), min, max); canvas.SetView(origin, scale);

    // 画布坐标下画内容（ImDrawList 自动按视口裁剪）
    ImGui::Text("Hello World!");
    canvas.End();
}
```

**要点**：
- 进入画布后 `ImGui::GetCursorScreenPos()` 原点为画布左上角，鼠标输入自动换算到画布空间，控件照常工作。
- 所有 ImDrawList 绘制都在画布坐标（含缩放）。
- `CenterView(ImRect)` 常用来在初始化时"一次性把内容缩放到可见"。

---

## 12. 示例对照表（想抄哪段看哪段）

| 需求 | 看哪个示例 | 位置 |
|---|---|---|
| 最小可用编辑器 | `simple-example` | 全部 |
| 建连 / 删连完整流程 | `basic-interaction-example` | `OnFrame` 第 2 部分 |
| 节点内控件陷阱 + 延迟弹窗 + InputText 快捷键 | `widgets-example` | 各节点 Demo + 文件头注释 |
| 复杂节点布局（Header/Middle/Input/Output） | `blueprints-example` + `utilities/builders.*` | `BlueprintNodeBuilder` |
| 拖出空白建节点 + 右键菜单（节点/引脚/连线/空白） | `blueprints-example` | `OnFrame` `#if 1` 块 |
| 选中读取、左侧节点列表、样式编辑面板 | `blueprints-example` | `ShowLeftPane` / `ShowStyleEditor` |
| 节点状态持久化（Save/LoadNodeSettings） | `blueprints-example` | `OnStart` config 回调 |
| 注释/分组框 + 标题浮标 | `blueprints-example` | `NodeType::Comment` 段 |
| 连线动画 `Flow` | `blueprints-example` | `ShowLeftPane` "Show Flow" |
| 通用缩放画布、平移、标尺 | `canvas-example` | 全部 |
| 引脚类型图标绘制 | `blueprints-example/utilities/{drawing,widgets}.*` | `DrawPinIcon` |
| 序号徽标（GetOrderedNodeIds） | `blueprints-example` | `ShowOrdinals` 段 |

---

## 13. 常见坑速查

1. `Begin`/`End`、`BeginNode`/`EndNode`、`BeginPin`/`EndPin`、`Suspend`/`Resume`、`BeginCreate`/`EndCreate`、`BeginDelete`/`EndDelete` 全部**严格配对**，漏一个下帧就崩。
2. 数据是"每帧全量提交"：节点忘提交、连线条目没 `Link()`，编辑器就当它不存在。
3. `BeginNode` 前必须 `ImGui::PushID(nodeId.AsPointer())`，否则多个节点内控件 ID 冲突。
4. 弹窗/菜单/Tooltip 一律包 `Suspend`/`Resume`。
5. `InputText` 等输入控件聚焦时调 `ed::EnableShortcuts(false)`，失焦调回 true。
6. 删除是请求式：编辑器只发问，数据要自己删。
7. 节点 ID 需跨实体唯一；要持久化布局就用 `Config.SettingsFile`。
