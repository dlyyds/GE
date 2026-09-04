# Filament 渲染后端可行性计划书

> 状态：**可行性分析 + 方案设计（待评审）**
> 目标：评估引入 Google Filament 作为第二渲染后端、可"与现有 Vulkan 渲染器切换"的可行性，给出分档路线与推荐档位。
> 前置：无（本计划书仅分析 + 设计，不含代码改动）
> 关联：`项目架构总览.md`、`Renderer3D扩展路线图-材质系统与批处理.md`（现渲染器演进路径）

---

## 1. 一句话结论

**"增加 Filament 后端并与现有渲染器切换"在本项目里不是"加一个后端"，而是一次渲染子系统抽象重构。** 其成本取决于"切换"的粒度，从"独立进程验证（低）"到"编辑器内运行时切换（极高）"跨度巨大。推荐先做**独立 target 的验证型接入**，用最低成本回答 Filament 是否值得深度集成；全后端抽象（B 档）作为后续立项。

---

## 2. 现状盘点：耦合在哪里（实证）

### 2.1 调用面是干净的（好消息）

高层渲染语义与后端无关，场景层只依赖"提交一个网格 + 材质 + 变换"：

| 位置 | 调用内容 | 说明 |
|---|---|---|
| `Scene::RenderMeshes3D`（Scene.cpp:768-905） | `r3d.BeginScene(view,proj,viewPos) → SetSkinJointBuffer → DrawSkinnedSubMesh / DrawSubMesh(transform, mesh, submesh, material, color) → EndScene` | 语义面：只传 `Mesh*` + `Material*` + 矩阵 + tint |
| `Scene::RenderSprites2D`（Scene.cpp:907） | `r2d.BeginScene / DrawSprite / EndScene` | 2D 面与 3D 同构 |
| `MeshRendererComponent` | 持有 `Mesh*` / `Material*` / `Color` / `materialOverrides`（子网格→材质） | 纯逻辑引用 |

这段语义天然可被另一个后端"重新实现"——**接口换名不换意**。

### 2.2 GPU 资源持有层是深度 Vulkan 耦合（真正的分界线）

| 类型 | 内部实质 | 位置 |
|---|---|---|
| `Mesh` | 持有 `std::unique_ptr<VulkanBuffer>` 顶点/索引缓冲，绘制靠 `vkCmdBindVertexBuffers` + `vkCmdDrawIndexed` | Mesh.h:460 |
| `Material` | 槽位数组存 `Texture*` + 标量参数 dict | Material.h:281 |
| `Texture` | 内部就是 `VulkanImage` + `VulkanImageView` + `VulkanSampler`，`GetDescriptorInfo()` 返回 `vk::DescriptorImageInfo` | Texture.h:332-358 |
| `Renderer` | 成员即 `VulkanContext`/`VulkanRenderContext`；静态方法直接暴露 `GetVulkanContext()`/`GetRenderContext()`/`GetFrameCmd()`/`GetSwapchain()` | Renderer.h:156-183 |
| `RenderTarget` | Vulkan 附件集合（颜色/深度/MSAA），构造收 `VulkanDevice` | RenderTarget.h |

结论：**GPU 资源类型本身就是 Vulkan 句柄的载体**，同一种类型不可能同时装两种后端的资源。要支持两后端并存，必须先把"资源外壳（子网格、材质槽位、AABB）"与"GPU 资源实现"拆开——这正是成本大头。

### 2.3 编辑器 / ImGui / 主循环的锁定

| 位置 | 依赖 | 影响 |
|---|---|---|
| `ImGuiLayer`（GE/src/ImGui/ImGuiLayer.cpp） | `imgui_impl_vulkan` 后端叠帧 | Filament 官方无 imgui 后端；编辑器 UI 全在 Vulkan 上 |
| `SceneViewport`（SceneViewport.cpp:42） | 离屏 `RenderTarget` + `ImGui_ImplVulkan_AddTexture` | "渲染到视口"直接绑死 Vulkan 纹理 |
| `Application` 主循环（Application.cpp:90-109） | `Renderer::BeginFrame → OnUpdate → ImGui::Begin/End → EndFrame` | 帧流、present、swapchain 全在 Renderer 内部 |
| `Window`（GEWindow.h:109） | `CreateVulkanSurface` 直接返回 `VkSurfaceKHR` | 窗口抽象含 Vulkan 方法 |

**推论**：想让编辑器内某个视口或场景实时切后端，等价于给 ImGui 后端、视口纹理桥、帧流都加抽象——这是最高成本档。真正的可切换点是**独立进程（不同 EXE target）**。

### 2.4 唯一现成的后端无关入口：`MeshData`

`MeshData`（Mesh.h:235）是纯 CPU 载荷（vertices/indices/subMeshes/materialData/aabb/skinIndex），顶点结构 `Vertex` 80B（position/normal/uv/tangent/joints/weights）是公开契约。格式解析（OBJ/glTF/gemesh）与 GPU 上传在此解耦。**这是给 Filament 喂几何数据的唯一现成接缝**——Filament 侧自建自己的 GPU 缓冲即可，不需动现有加载器。

---

## 3. 档位拆解

### 档位 A（低）：独立进程验证 —— 推荐先行

**形式**：新增独立 target（如 `Sandbox_Filament` EXE，或 Filament 版冒烟层），**只**在它内部接入 Filament。

**内容**：
- Filament 源码入库 third_party（含其驱动的 .so/.dll 分发，Windows 用它的 desktop 后端）
- 独立 `FilamentContext`：Engine/SwapChain（经 GLFW 窗口 surface）/Renderer/View/Camera/Scene
- 从现有 `MeshData` 装配 `filament::VertexBuffer`/`IndexBuffer` + `RenderableManager` + `MaterialInstance`（先用 Filament 内置 lit/unlit）
- 逐帧驱动自己的一套简易主循环（不碰 Application/Renderer 单例）

**能回答的问题**：Filament 在这台 Windows + MSVC 环境能否稳定构建运行、能否吃同一批资产（glTF/纹理）、输出画质与自研 Vulkan 的差异观感。这是决定是否值得投入 B 档的成本前提。

**成本**：中等（纯新增，零改动现有代码）；不满足"编辑器里切换"，只满足"另一条可跑的通路"。

### 档位 B（高）：渲染后端抽象层（真正的"可切换"）

**形式**：为"提交场景→出帧"这一层定义后端无关接口，现有 Vulkan 渲染器与 Filament 各实现一份。

**必经重构（成本清单）**：
1. `Mesh`/`Material`/`Texture` 拆出"资源外壳 + GPU 实现 pimpl"——现有 Vulkan 资源收进 impl，接口面（子网格/槽位/AABB）留在外壳。**牵动全部既有调用方与序列化。**
2. 定义 `IRenderer3D`/`IRenderer2D`（BeginScene/Draw*/EndScene）+ 后端无关的资源类型（句柄式 meshHandle/materialHandle）。
3. `Renderer` 单例收口对 `VulkanContext`/`GetFrameCmd()` 的静态裸暴露，改为后端接口；swapchain/frame 语义后移。
4. 帧流与 ImGui：ImGui 保持 Vulkan 后端则 UI 层仍锁 Vulkan；要 Filament 画 3D、ImGui 叠 UI，需在两者间做**图像桥**（Filament 离屏 render target → 纹理 → ImGui 采样），并新增一套 ImGui render pass 喂给 Filament。这是独立子工程。
5. Filament 侧把引擎自研在 Vulkan 上的能力重映射：PBR 材质/IBL/蒙皮/透明排序/批处理 → Filament 的 `Material`/`IndirectLight`/`SkinningBuffer`/`View` 语义。**等于用 Filament 重写一遍渲染路径**，现有 shader、管线、批处理让位。

**成本**：全引擎渲染层 2-3 个月级 + 双份维护；收益：真·双后端可切换。

### 档位 C（不推荐为起点）：编辑器内运行时热切

在 B 档之上再让 ImGui/编辑器 UI 也切后端。受 §2.3 三重锁定制约，等于再造一套 UI 渲染栈，建议直接排除为 v1 目标。

---

## 4. 关键风险与已知限制

| 项 | 说明 |
|---|---|
| **Filament 资产管线** | Filament 用 filamat 材质编译、自带模型/纹理管线；与引擎现有 .gemesh/GLSL/MaterialData 体系并存，需要桥接，不存在"一键吞掉现有资产" |
| **能力重映射而非复用** | IBL 三图、蒙皮、透明双 pass、排序批处理在 Filament 里各有内置语义，但**移植=重写语义映射**，现有 Vulkan 自研不回收 |
| **版本与平台** | Filament 桌面后端走自有 driver 分发，需核对该发行版对 Windows + 驱动的最低要求；版本号与 CMake 接入方式以引入时官方 release 为准 |
| **双渲染器互不可见** | 两后端产出的 GPU 资源无法互相引用；任何"切换"都要重建场景的 GPU 表示，运行期切换必然有顿挫与状态迁移成本 |
| **维护负担** | B 档后每条渲染改动要过两套后端，测试面翻倍 |

---

## 5. 推荐路线（若立项）

1. **阶段 0**：third_party 入库 Filament（vendor 静态/动态分发 + 顶层 CMake 开关 `GE_ENABLE_FILAMENT`，默认 OFF）。不改现有 GE 目标。
2. **阶段 1（= 档位 A）**：新增 `Sandbox_Filament` target：GLFW 窗口 + Filament 全链 + 从 `MeshData` 装配 Renderable。验收：同一 glTF/OBJ 资产在自研 Vulkan 与 Filament 下都能出帧，几何一致。
3. **阶段 2（评审门槛）**：基于阶段 1 的实测（构建耗时/驱动兼容/画质/帧率），决定是否进入档位 B。若进入，先做 §3-B 清单的第 1 项资源 pimpl 拆分（该步对现有架构亦有独立价值）。
4. **不在 v1**：编辑器内热切、ImGui 跨后端桥。

---

## 6. 验收标准（针对推荐档位 A）

- `Sandbox_Filament` 能在 Windows + 本机 GPU 上独立运行出帧，不触碰现有 `GE`/`GE_Editor` 构建产物
- 同一份 glTF（如 `assets/models/adamHead/adamHead.gltf`）或 OBJ 经现有解析 → `MeshData` → Filament Renderable，几何/材质观感与 Vulkan 通路一致
- 顶层 `GE_ENABLE_FILAMENT` 默认 OFF 时现有构建链路零变化

---

## 7. 与现有方案的衔接

- `项目架构总览.md`：本计划书是渲染模块"单 Vulkan 后端 → 可插拔后端"的探索立项；档位 B 的第一步（资源 pimpl 拆分）不破坏现有加载/序列化协议。
- `Renderer3D扩展路线图-材质系统与批处理.md`：该路线图继续演进自研 Vulkan 渲染器，与 Filament 立项**并行不冲突**；双轨运行是档位 A 的隐含前提。
- `Mesh类拆分重构方案.md`：`MeshData` 作为后端无关接缝的前提已在此落实。
