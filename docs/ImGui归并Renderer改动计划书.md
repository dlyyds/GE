# ImGui 归并 Renderer 模块改动计划书

- 日期：2026-09-03
- 状态：待评审
- 涉及范围：`GE` 引擎模块（`GE/src/ImGui`、`GE/include/GE/ImGui`、`GE/src/Core`、`GE/src/Render`、顶层 `CMakeLists.txt`）

## 1. 背景与现状

### 1.1 目标
把 ImGui 从"挂在 Application 上的独立 Layer"重构为**由 Renderer 统一初始化、持有并驱动的渲染子系统**，消除 ImGui 侧通过 `Application` 中转拿 Renderer 资源的迂回路径。

### 1.2 现状耦合

- 物理位置：ImGui 代码独立成目录 `GE/src/ImGui`、`GE/include/GE/ImGui`，是引擎的一个孤立模块。
- 生命周期：`Application` 构造里 `new ImGuiLayer` 并 `PushOverlay`；主循环里手动调用 `ImGuiLayer::Begin() / End()`；析构里在 `Renderer` 之前清理 Layer。
- 数据/资源获取：`ImGuiLayer` 内部绕道 `Application::GetVulkanContext() / GetSwapchain() / GetFrameCmd() / GetFrameImageView()`，而这些方法**只是转发 `Renderer` 的静态方法**（见 `Application.h:66-75`）。也就是说 ImGui 拿到的全部渲染资源本就归 `Renderer` 所有。
- 额外依赖点：
  - `Application::Get().GetWindow().GetGlfwWindow()`（平台窗口句柄，Layer 的 `Window` 来源）
  - `Renderer::GetAssetManager().ResolvePath(AssetPaths::Fonts + "/OpenSans-Regular.ttf")`（字体路径解析）
  - `app.GetFPS()`（帧率面板数据源，非渲染依赖）

### 1.3 业务方影响面

- 引擎侧的 `Application`、各 `Layer`；编辑器侧的各 Layer / Panel 不直接感知 ImGui 生命周期，只通过 `Layer::OnImGuiRender` 提交 UI。归并需保住这条"每帧在帧内提交 UI"的通道。
- ImGui 的最终上屏是画在 **swapchain 颜色附件** 上的（`ImGuiLayer::End` 用 `eLoad` 保持场景），而 2D/3D 场景现走离屏 RenderTarget（`Renderer3D` / `SceneViewport`）。**归并后 ImGui 仍应画在主 swapchain image**，只是执行入口从 Application 挪进 Renderer 的帧流程。
- 编辑器 `SceneViewport` 用 `Renderer::GetSwapchain().GetFormat()` 取颜色格式（`SceneViewport.cpp:26`），与 ImGui 归并无冲突（ImGui 仍在 swapchain 上屏，不产生新的格式源）。

## 2. 现状梳理结论

下面三个判断直接决定改动范围，实现前需先确认（见 §5）：

1. **整目录物理迁移动**：`GE/src/ImGui`、`GE/include/GE/ImGui` 整体迁入 `GE/src/Render/ImGui`、`GE/include/GE/Render/ImGui`。引擎内仅 `ImGuiLayer` 直接 include 这两个路径（§1.3 grep 结果：只有 `Application.h/.cpp`、`ImGuiLayer.cpp` 引用），改动面小。
2. **`Renderer` 具备构造 ImGui 的全部前置资源**：`Renderer` 构造链为 `VulkanContext → RenderContext → AsyncUploadManager → AssetManager`（`Renderer.cpp:22-56`）。ImGui 需要的 swapchain 格式 / device / queue 等都已就绪。
3. **主循环的 ImGui 调度段**（`Application.cpp:99-106`）**可整体下沉进 `Renderer::EndFrame`**：该段位于 `BeginFrame()` 之后、`EndFrame()` 之前，且只有 `Renderer` 持有"当前活动 command buffer"与"是否最小化"两态的语义。

## 3. 目标架构

### 3.1 模块归属

```
GE/src/Render/ImGui/ImGuiLayer.h/cpp   ← Renderer 内部私有实现（不进 Renderer 公共头）
GE/include/GE/Render/ImGui/            ← 若需要暴露给外界的 ImGui API 再放公共头
```

依赖方向变为单向：

```
Editor/App Layers ──OnImGuiRender──▶ Renderer ──持有/驱动──▶ ImGuiLayer(内部)
        │                                                    ▲
        └──────────── Layer::OnImGuiRender 反调 ──────────────┘
```

- Renderer 不再依赖 Application、不再依赖 Window 类型之外的应用层抽象（若 §5 判定取窗方式允许，只依赖 `Window` 接口）。
- ImGuiLayer 的 Vulkan / swapchain / cmd buffer / image view / 字体路径全部改为来自 `Renderer` 自身成员。
- `Application::GetVulkanContext/GetSwapchain/GetFrameCmd/GetFrameImageView/GetFrameImageIndex/GetRenderContext` 六个转发方法不再被 ImGui 使用（是否删除看 §6.1，其他业务方仍可能在使用）。

### 3.2 生命周期

- `Renderer` 构造尾部（`AssetManager` 之后）：`m_ImGuiLayer = std::make_unique<ImGuiLayer>(*this)`，内部完成 ImGui context / 字体 / Vulkan 与 GLFW backend 初始化。
- `Renderer` 析构、`WaitIdle()` 之后销毁 ImGuiLayer（与现在"先 Layer 后 Renderer"的顺序一致，保证 DescriptorPool 等先于 device 释放）。
- swapchain 重建时由 `Renderer::RecreateSwapchain` 回调 ImGui 做尺寸/资源刷新（见 §4.3 的 swapchain-resize 处理）。

### 3.3 帧驱动

ImGui 不再由 Application 调用，改为 `Renderer` 内部编排。公共入口保持 `Layer::OnImGuiRender` 通道不变：

- 现在：`Application::Run` 在主循环里手动 `ImGuiLayer::Begin/End`（静态）。
- 之后：`Renderer::EndFrame()` 内部依次 `BeginFrame → 各Layer OnImGuiRender(经RenderHook) → EndFrame`。`Renderer` 需暴露一个**帧内 UI 提交回调**，让 `Application`（或未来的编辑器/工具宿主）把 `m_LayerStack` 中每个 `Layer` 的 `OnImGuiRender()` 传进来。

### 3.4 模块化打包与宿主解耦（视 §5 目标取舍）

- Renderer 持有 ImGui 相关依赖：`imgui`、`imgui_impl_vulkan`、`imgui_impl_glfw`、`ImGuizmo`（可选）。构建上 `GE` 静态库仍统一链接这些库，但把依赖关系收敛到 `GE/src/Render/ImGui` 一层。
- 若希望 `Renderer` 不依赖具体 GLFW/Vulkan 的 ImGui backend，则进一步抽出薄接口（§6.3）；本轮默认**不抽接口**，因为引擎自身后端就是 GLFW+Vulkan，接口化属过度设计。

## 4. 实施步骤

### Step 1 — 物理迁移与收口
1. 移动 `GE/include/GE/ImGui/ImGuiLayer.h` → `GE/include/GE/Render/ImGui/ImGuiLayer.h`
2. 移动 `GE/src/ImGui/ImGuiLayer.cpp` → `GE/src/Render/ImGui/ImGuiLayer.cpp`
3. 更新 `.cpp` 内 include 为 `Render/ImGui/ImGuiLayer.h`（依赖 `#include "ImGui/ImGuiLayer.h"` 的只有 `Application.h/.cpp` 与 `ImGuiLayer.cpp` 自身，见 grep）。
4. 顶层 `CMakeLists.txt`：`GE_SRC` 的 `GE/src/ImGui/*.cpp` 改为 `GE/src/Render/ImGui/*.cpp`（§顶部 108-123）。
5. 检查 include 目录：`GE/include` 与 `GE/include/GE` 已在公共 include 路径中，`Render/ImGui/` 子路径自动可被解析。

### Step 2 — Renderer 内部挂载 ImGui
在 `Renderer.h/.cpp` 增加私有成员与实现：

```cpp
// Renderer.h（private 区）
friend class ImGuiLayer;
std::unique_ptr<ImGuiLayer> m_ImGuiLayer;          // Renderer 内部持有
LayerRenderCallback  m_UIRenderCallback = nullptr; // Application 注入的帧内 UI 提交

// 供 ImGuiLayer 内部读取，等价于现在的 Application 转发
Window       &GetWindowRef() const { return m_Window; }
AssetManager &GetAssetManagerRef() const { ... }
```

- `Renderer` 构造尾部：`m_ImGuiLayer = std::make_unique<ImGuiLayer>(*this);`
- `Renderer` 析构 `WaitIdle()` 后：`m_ImGuiLayer.reset();`
- 新增 `void SetUIRenderCallback(LayerRenderCallback cb)`：把"渲染一帧 ImGui"封装成一个 `std::function<void()>`，由 `Application` 注入（内容为 `for(auto& layer : m_LayerStack) layer->OnImGuiRender();`）。

### Step 3 — ImGuiLayer 改造（依赖反转）
把对 `Application` 的静态调用全部换成对 `Renderer` 成员/引用的访问：

| 现状（ImGuiLayer.cpp） | 改造后 |
|---|---|
| `Application::Get().GetWindow().GetGlfwWindow()` | `m_Renderer.GetWindowRef().GetGlfwWindow()` |
| `Application::GetVulkanContext()` | `m_Renderer.GetVulkanContext()`（或 Renderer 直接持有 ctx 引用传给 ImGui） |
| `Application::GetSwapchain()` | `m_Renderer.GetSwapchain()` |
| `Application::GetFrameCmd()` | `m_Renderer.GetFrameCmd()` |
| `Application::GetFrameImageView()` | `m_Renderer.GetFrameImageView()` |
| `Renderer::GetAssetManager().ResolvePath(...)` | `m_Renderer.GetAssetManagerRef().ResolvePath(...)` |
| `app.GetFPS()` | 帧率面板需另行注入 FPS 数据（见 §4.4） |

- `ImGuiLayer` 从"继承 `Layer`、被 Push 到 LayerStack"变为"被 `Renderer` 独占持有的对象"——是否保留 `Layer` 派生可讨论，核心是**不再进入 LayerStack，不参与 Application 的 OnUpdate/OnEvent 遍历**。
- 事件处理 `OnEvent`（`WantCaptureMouse/Keyboard` 阻断）从"ImGui 自己拦截"调整为**显式查询**：`Renderer::WantCaptureImGuiInput()` 返回 ImGuiIO 的捕获状态，由 Application/Editor 的输入分发决定是否继续向下派发（改由 `Application::OnEvent` 内联判断，见 §4.5）。

### Step 4 — 主循环调度下沉
把 `Application.cpp:99-106` 的 ImGui 块挪进 `Renderer::EndFrame()`：

```cpp
// Renderer::EndFrame() 内部（在 layout→Present 转换之前）
{
    if (m_ImGuiLayer && m_UIRenderCallback) {
        m_ImGuiLayer->BeginFrameImGui();            // 原 ImGuiLayer::Begin（NewFrame）
        m_UIRenderCallback();                        // 各 Layer 提交 UI
        m_ImGuiLayer->RenderImGui(swapchainView);    // 原 ImGuiLayer::End（Render + 上屏）
    }
}
// 之后原有 TransitionToPresent / End / submit/present
```

`Application::Run()` 删掉该段，只保留 `m_Renderer->BeginFrame() / EndFrame()`。

### Step 5 — 收尾清理
- 删除 `Application::GetFrameCmd/GetFrameImageView/...` 等只被 ImGui 使用的方法（若确无其他调用方，见 §6.1）。
- `Application.h` 去掉 `#include "ImGui/ImGuiLayer.h"`，`m_ImGuiLayer` 改为经由 Renderer 持有；`Application.cpp` 对应删除 `new / reset`、`PushOverlay`（需考虑编辑器对 ImGuiLayer 的直接引用是否还存在）。
- 运行冒烟：Sandbox + GE_Editor 均能启动、主视口渲染、ImGui 菜单/统计/视口图正常。

### Step 6 — 验证项
- Debug 与 Release 各构建一次（MSVC + Ninja，`build.bat`），无编译/链接错误。
- 运行 Sandbox：ImGui 统计面板与场景正常、无 validation layer 报错。
- 运行 GE_Editor：DockSpace、Hierarchy、SceneViewport、ResourcePanel、ASMGraphLayer、Gizmo 均正常，resize 主窗口时 ImGui 与离屏视口同步正确。
- 中断/切后台窗口，确认 `WantCaptureMouse/Keyboard` 逻辑不劣化（尤其编辑场景拖动时 gizmo 与视口事件冲突）。

## 5. 待确认事项

1. **宿主解耦程度**：Renderer 是否允许在构造时拿到 `Window`（现构造就收 `Window&`），但需要 **FPS / 最小化 / 输入** 这些"宿主状态"的注入接口。若严格禁止 Renderer 知道"Application"，需抽象一个 `RendererHost` 接口（见 §6.3）。建议：**本阶段保留 Renderer 依赖 Window，FPS 通过 `SetFrameInfo(fps, minimized)` 注入**，不为脱离 Application 而过度抽象。
2. **渲染目标**：ImGui 上屏目标是 swapchain color attachment（现状）。若未来希望 ImGui 也画到独立视口纹理再合成，属于另一需求，本轮不展开。
3. **静态 Layer 渲染回调**：`Layer::OnImGuiRender` 的遍历由 Application 闭包注入 Renderer；编辑器若不走 Layer 机制，可改用直接调用 `Renderer::GetImGuiLayer()->NewFrame()/Render()` 两段式（对编辑器透明，编辑器本就引用 Renderer）。
4. **文件组织**：目录用 `Render/ImGui`（模块风格）还是 `Render/Backends`/`Render/UI`？先确认项目命名偏好；本计划默认 `Render/ImGui`。

## 6. 风险与备选

### 6.1 静态转发方法去留
`Application::GetFrameCmd/GetFrameImageView/GetVulkanContext/GetSwapchain` 是否还有业务调用方，决定能否删除。若仍有，建议保留并标注 `[[deprecated]]`，待全部迁移后清理（grep 目前仅 ImGuiLayer 使用，删除风险低，但需在编辑器侧再全量搜一次）。

### 6.2 初始化顺序与 swapchain 重建
`Renderer::RecreateSwapchain` 改变 swapchain image 数量/格式时，ImGui 的 `MinImageCount/ImageCount`、descriptor pool、image view 缓存需同步刷新。现状 swapchain 重建只由 `Application` resize 触发；归并后务必在 `RecreateSwapchain` 内回调 `ImGuiLayer::OnSwapchainRecreated()`，否则窗口拖动 resize 后 ImGui 上屏可能引用过期 image view。

### 6.3 为彻底脱离 Application 抽 RendererHost 接口（备选）
若确定 Renderer 不得依赖任何"宿主"抽象，则引入：

```cpp
struct RendererHost {
    virtual Window     &GetWindow() = 0;
    virtual float       GetFPS() = 0;
    virtual bool        IsMinimized() = 0;
    virtual void        SetImGuiBlockEvents(bool) = 0;  // 若输入分发仍归宿主
    virtual void        ForEachLayerUIRender(std::function<void()>) = 0;
    virtual ~RendererHost() = default;
};
```

`Application` 实现 `RendererHost` 并在构造 Renderer 时注入。**收益**：Renderer 完全可脱离 Application 单独测试 / 复用。**代价**：多一层虚接口与适配代码。本轮建议不做，仅在文档标注此演进方向。

### 6.4 帧率数据源
`Application::GetFPS()` 是 Application 主循环统计值（`Application.cpp:70-82`），非渲染数据。归并后 ImGui 渲染统计面板的 FPS 显示应改为宿主注入（如 `SetFrameInfo` 每帧传入），而不是让 Renderer 反向拉取 Application。

### 6.5 OnEvent 输入阻断迁移
ImGui 的输入捕获（`WantCaptureMouse/Keyboard`）目前由 `ImGuiLayer::OnEvent` 在 LayerStack 逆序遍历中起作用。归并后 Application 不再 push ImGui 到 LayerStack，需在 `Application::OnEvent` 前置判断：

```cpp
if (Renderer::Get().WantCaptureImGuiInput())
    if (e.IsInCategory(EventCategoryMouse|EventCategoryKeyboard))
        return; // 或不再向下派发
```

该行为要与现状逐帧一致，编辑器的 GizmoController 大量依赖鼠标在视口内的事件，需回归测试。

## 7. 变更清单（File Touch List）

| 文件 | 变更 |
|---|---|
| `GE/include/GE/ImGui/ImGuiLayer.h` | 移动到 `GE/include/GE/Render/ImGui/ImGuiLayer.h` |
| `GE/src/ImGui/ImGuiLayer.cpp` | 移动到 `GE/src/Render/ImGui/ImGuiLayer.cpp` |
| `GE/include/GE/Render/Renderer.h` | 增加 `m_ImGuiLayer`、帧回调、供 ImGui 访问的成员方法与 `RecreateSwapchain` 内刷新 |
| `GE/src/Render/Renderer.cpp` | 构造尾部创建 ImGuiLayer；析构清理；`EndFrame` 内接入 ImGui 帧调度；`RecreateSwapchain` 回调 |
| `GE/include/GE/Core/Application.h` | 删除 `#include ImGuiLayer.h`、`m_ImGuiLayer`；删除不再使用的转发方法 |
| `GE/src/Core/Application.cpp` | 主循环删 ImGui 块；`PushOverlay/OnAttach/OnDetach` 对应简化；输入阻断上移 |
| `CMakeLists.txt`（根） | `GE/src/ImGui/*.cpp` → `GE/src/Render/ImGui/*.cpp` |

## 8. 验收标准

- [ ] `GE` 库内部不再有 `src/ImGui` 之外的目录，ImGui 源码收在 `Render/ImGui`。
- [ ] `ImGuiLayer.cpp` 不再 include `Core/Application.h`，不再调用任何 `Application::` 静态方法。
- [ ] `Renderer` 的生命周期内自动初始化/驱动 ImGui，`Application` 主循环无 ImGui 专属代码段。
- [ ] 编辑器的 UI（DockSpace/视口/面板/图编辑器）与渲染统计功能与重构前一致。
- [ ] 主窗口 resize、切最小化、帧率显示均正常，validation layer 无新报错。
