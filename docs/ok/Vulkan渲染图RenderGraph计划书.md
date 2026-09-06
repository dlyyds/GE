# Vulkan 渲染图（Render Graph）计划书

> 状态：**阶段1 骨架已落地（S1）**，S2–S5 待做
> 目标：引入渲染图抽象 —— 各 Pass 只声明「读取哪些资源、写入哪些资源」，由渲染图按声明序自动插入全部内存屏障与图像布局转换。
> 前置：Vulkan 1.3（动态渲染 core，VulkanContext.cpp:159）、已有多通道框架（Renderer2D / Renderer3D / ImGuiLayer）
> 关联：`RenderTarget.h`、`VulkanRenderingInfo.h`、`VulkanCommandBuffer.h`、`Filament渲染后端可行性计划书.md`
>
> 实现进展（相对早期设计稿的偏离已同步进正文）：
> - **声明序即执行序**：依赖建图与拓扑排序已移除（原 §5.1/§5.2）。依赖边恒朝声明序前方延伸，Kahn+最小堆的拓扑输出恒等于声明序，故直接按声明序执行，pass 必须按依赖序声明（读某资源须声明在其写者之后），框架不重排、不做环检测。
> - **布局记忆下沉 VulkanImage**：`ImageViewResource` 封装已移除（原 §4.6/§6A）。Vulkan 布局本质 per-image，跨帧布局记忆用 VulkanImage 上的单变量 `currentLayout`（帧首直接读、帧末覆盖，NSDMI 默认 `eUndefined`）。

---

## 1. 现状盘点：屏障与布局转换是手工、零散、脆弱的

| 位置 | 手工同步 | 问题 |
|---|---|---|
| `Renderer.cpp` BeginFrame 布局转换（Renderer.cpp:99） | swapchain 图 `eUndefined → eColorAttachmentOptimal` | `eUndefined` 语义 = **丢弃旧内容**；每帧早于清屏、晚于 acquire 都各做一次 |
| `Renderer.cpp` EndFrame 布局转换（Renderer.cpp:139） | swapchain 图 `eColorAttachmentOptimal → ePresentSrcKHR` | 假定 3D 一定画完才轮到 ImGui 写；两者都直写 swapchain 时顺序依赖靠「谁先被调用」维持 |
| `Renderer2D / Renderer3D` 离屏路径 | 场景先离屏渲染，ImGui 再把离屏颜色图当纹理贴到 swapchain | **布局转换在 Renderer3D 离屏 ImageView 注册进 ImGui 的那一段手工发生**，2D / 3D / UI 三层之间只有隐式时序约定 |
| `VulkanImage.cpp:251` 起 | 已用 `PipelineStageFlagBits2 / AccessFlagBits2` | 引擎**已能触达同步2 原语**（VulkanImage 内部在用），证明可用性 |
| `image_layout_transition`（VulkanCommon.cpp:246） | 依据 old/new layout **查表自动推导** stage/access（VulkanCommon.cpp:302/326） | 已有「布局 → 语义」映射，正是渲染图自动屏障需要的最小基础 |

**结论**：同步逻辑散落在各渲染器 + 帧循环的硬编码时序里。每加一个 Pass（离屏、后处理、阴影、计算），就多一组手写布局转换，且在正确的位置猜对顺序。渲染图把「**正确**」收进一个可推演、可断言的地方。

> 判断：本项目 **不需要真正引入一个外部渲染图库**。需求规模 = 单 command buffer、单 graphics 队列、动态渲染、帧池资源（现有 RenderTarget / 每帧独立描述符天然满足）。自研一个「轻量、以正确性为纲」的渲染图即可，无需 FrameGraph 的持久资源 / 跨帧分析复杂度。

---

## 2. 目标与不做什么

### 2.1 目标

1. **声明式 Pass**：`AddPass(...)` 里说清读谁、写谁、写完后谁的布局得变成什么，以及 render target 的 load/store/clear。
2. **自动同步**：各 Pass 按声明序执行（声明序即执行序），Execute 为每条跨 Pass 的资源依赖自动插入屏障（图像布局转换 + 内存屏障 + 阶段/访问掩码）。
3. **自动布局终点**：Pass 描述里给出**写后布局**（采样态 / 呈现态），Execute 负责前置布局转换 + Pass 末尾的收尾转换，Pass 内部只写 `VulkanRenderingInfo` 里的目标布局，不写任何 `vkCmdPipelineBarrier`。
4. **顺序由声明决定，不是自动推导**：v1 图不重排 pass（早期「依赖图定顺序下界 + 自动拓扑」的设计已移除，见顶部实现进展）。Pass 必须按依赖序声明；读某资源须声明在其写者之后。这是 v1 取舍 —— 图负责在给定顺序下做对同步，不负责替你排顺序。

### 2.2 明确不做什么（划清边界，防蔓延）

| 不做 | 原因 |
|---|---|
| **不做资源生命周期 / 分配** | RenderTarget 与 Image 继续由各子系统（SceneViewport、EnvironmentMap、TextureManager）自己持有。图只引用、不拥有（`std::vector<ResourceHandle>` 语义） |
| **不做跨帧复用分析** | 现有 `VulkanRenderFrame`（帧池、每帧重置描述符/缓冲）已管理帧间资源。图按帧重建，零持久状态 |
| **不做持久 Pass / 跨帧复用 Pass** | 每帧构建是明确取舍：**正确性优先、开销可忽略**。~10 pass 的收集 + 屏障生成在微秒量级 |
| **不做自动并行 / 队列分派** | 引擎单 graphics 队列 + 单命令缓冲。图产出线性命令流即可。跨队列作为未来扩展记录（见 §9.2） |
| **不强制计算队列 / 渲染图没理由只服务 raster** | **阶段1 仅 raster；阶段2 引入 compute pass 支持（dispatch + 图像存储 + 缓冲读写）**。核心任务本质不变：仍是资源的 R/W 声明 |
| **不做多 pass 合帧** | 不隐藏 `beginRendering / endRendering`，一张「渲染图」里可以出现多次 beginRendering —— 一个 pass = 一次 dynamic rendering |

---

## 3. 为什么每帧重建（渲染图里的关键取舍）

持久的渲染图（记录 Pass 与纹理，帧间可复用的图结构）在 C++ 侧通常得不偿失：

- 它的核心收益在「**资源分配与别名**」（一个物理图像在帧内被多个虚拟纹理复用）。而本引擎的 RenderTarget **不是帧内虚拟的**：SceneViewport 的离屏目标是跨帧常驻对象。图内无资源分配可优化，持久化只剩纯开销。
- 持久图必须处理 **图随内容变化**（分辨率、环境是否启用、材质 alphaMode 数量）时的重建与失效；每帧重建直接免疫。

**代价是 CPU 时间**：每帧收集 pass 访问 + 生成屏障。声明序执行后连依赖建图与拓扑排序都省了（v1 图是「声明序顺序执行 + 运行期屏障」的薄层），~10 pass 场景每帧构建开销相比 command buffer 录制可忽略。

**中间路线（记录于未来扩展 §9.1）**：把「Pass 的**结构**」（哪个 pass 依赖哪张纹理、谁写入）与「Pass 的**参数**」（UBO 内容、网格列表、clear color）分开。参数是每帧的，结构是**按需重算**的（key 命中复用）。

---

## 4. 接口设计总览

> 完整接口一览放在本章；核心算法与逐 Pass 流程分别在 §5、§6。引用现有类名（VulkanRenderFrame / RenderTarget / VulkanCommandBuffer）确保与现状可拼装。

```cpp
namespace GE {

// ============================================================================
// 资源引用句柄
// ============================================================================

/// 一张图内唯一的资源引用。对「帧内虚拟资源」与「外部资源」一视同仁。
/// 图不持有资源；写入者引用一个外部资源时，会告诉图它的图像布局与 usage。
struct RenderGraphResourceDesc {
    vk::Format   format          = vk::Format::eUndefined; ///< 图像格式（未知则用创建者提供）
    vk::Extent2D extent          = {};                     ///< 尺寸
    vk::SampleCountFlagBits samples = vk::SampleCountFlagBits::e1;
    bool         transient       = false;                  ///< 帧内临时（未来别名优化用，v1 保留字段）
};

enum class ExternalResourceType { Image, Buffer };

// ============================================================================
// 外部图像：布局跨帧记忆已下沉到 VulkanImage
// ============================================================================

// 【设计变更】早期设计稿的 ImageViewResource 封装已移除：Vulkan 布局本质是
// per-image，跨帧布局记忆直接在 VulkanImage 上用单变量承载（见 §4.2）：
//   VulkanImage::set_layout / get_layout，NSDMI 默认 eUndefined。
// 外部资源在图中只登记为 VulkanImageView*（经 get_image() 读写其 image 的布局）。

// ============================================================================
// 附件 / 输入描述（声明式 Pass 的核心）
// ============================================================================

/// 图内资源——具体是哪张外部图、如何转成 image view（可能需创建视图）由实现层决定。
/// 它把「Image X 作为颜色附件，加载 clear，存 store」这一语义性声明带进图。
enum class ResourceUsage {
    ColorAttachment,
    DepthStencilAttachment,
    ResolveSrc,            ///< 作为颜色附件的 resolve 来源（通常不单独出现）
    Input,                 ///< 作为片元/采样输入
    ShaderRead,            ///< 采样纹理（读写语义的读侧）
    ShaderWrite,           ///< 图像存储（写侧，阶段2 计算）
    Transfer,              ///< 传输（拷贝/清空）
};

/// 附件声明：一次 load 的清晰语义。
struct AttachmentDesc {
    ResourceHandle   resource;            ///< 引用哪种资源（帧内虚拟或外部图像）
    ResourceUsage    usage;               ///< 用途
    vk::AttachmentLoadOp  loadOp  = vk::AttachmentLoadOp::eLoad;
    vk::AttachmentStoreOp storeOp = vk::AttachmentStoreOp::eStore;
    vk::ClearColorValue   clearColor{};   ///< 附件的清除值（仅 loadOp == eClear 有意义）
};

/// 图像输入声明。
struct ImageInputDesc {
    ResourceHandle   resource;            ///< 图内资源句柄（虚拟或外部图像）
    vk::ImageLayout  layout   = vk::ImageLayout::eShaderReadOnlyOptimal;  ///< 期望读取布局
};

// ============================================================================
// Pass
// ============================================================================

enum class PassType {
    Raster,          ///< 动态渲染 pass
    Compute,         ///< 计算 pass（阶段2）
    Transfer,        ///< 传输 pass：清空/拷贝（可选，阶段2 顺带）
};

/// 一个 Pass 所需的一切。由 AddPass 填满。
struct RenderPassDesc {
    std::string name;                          ///< 调试名（RenderDoc 与日志）
    PassType    type = PassType::Raster;

    // --- 附件（raster pass） ---
    std::vector<AttachmentDesc> colorAttachments;
    AttachmentDesc depthAttachment;            ///< 可选
    vk::Rect2D   renderArea{};                 ///< 渲染区域；无效 = 用颜色附件 extent

    // --- 输入与资源读写（所有 pass 共用） ---
    std::vector<ImageInputDesc> readImages;    ///< 采样读取（含 color resolve 前的不写路径）
    std::vector<ImageInputDesc> writeImages;   ///< 本 pass 会写入（图要转成目标布局）

    /// 绑定到本 pass 的动态数据（UBO 内容 / clearColor / push constants），参数部分每帧独立。
    void *userData = nullptr;                  ///< pass 执行回调时透传
    std::function<void(PassExecuteContext &)> execute;   ///< 录制命令的回调（见 §5.3）
};

// ============================================================================
// 图构建器 —— 顶层门面
// ============================================================================

class RenderGraphBuilder {
public:
    explicit RenderGraphBuilder(RenderGraph &graph);

    /// 引用一个外部图像视图，注册其「帧首布局 → 图期望布局」与「写后布局」。
    /// 布局跨帧记忆经 view->get_image() 读写在其 VulkanImage 上。
    ResourceHandle Import(VulkanImageView *view, const std::string &name = {});

    /// 创建帧内虚拟资源（图内部管理生命周期，适合做中间缓冲）。
    /// v1 提供：临时图像（alias 等留待未来）。
    ResourceHandle CreateVirtualResource(const std::string &name, const RenderGraphResourceDesc &desc);

    /// 开始记录一个 pass，返回其描述符（后续用附件/输入绑定填充）。
    RenderPassDesc &AddPass(const std::string &name, PassType type = PassType::Raster);

    /// 便捷宏式入口：一个子渲染 pass（raster 的一种，见 §5.1 示例）。
    // 接口见文档 §5.1。

    // --- 结束记录并编译 ---
    RenderGraph &Build();
};

} // namespace GE
```

### 4.1 资源句柄与「虚拟 vs 外部」

- `ResourceHandle` 是渲染图内部一张 **索引表** 的键。外部导入的每个 `VulkanImageView*` 占一行，虚拟资源占一行。图不拥有任何资源，只持有「引用 + 布局流转期望」。
- **虚拟资源**：中间结果（不常驻）。v1 可用场景 = 一次完整 frame 链中的中间纹理（如后处理 ping-pong、SSAO buffer、光照分量的离屏缓冲）。
- **外部资源**：Pass 里要读要写、但所有权在别处的图（SceneViewport 离屏目标、swapchain 图、IBL 采样贴图等）。**这是本引擎 v1 的主流**。

### 4.2 布局与访问：外部图帧首布局从哪来

`vk::ImageLayout` 描述**单个**状态。但 swapchain 图每帧首尾布局固定（`Undefined / PresentSrc`），中途若是 `ColorAttachmentOptimal`，一次 pass 完后要回 `Present`。而普通离屏颜色图帧首可能正是 `ShaderReadOnlyOptimal`（上一帧末被 UI 采样过）。**图必须知道「外部图现在停在哪个布局」，才能安排本帧首尾转换。**

实现方案（相对早期设计稿的 `ExternalImageState` 三态枚举、`ImageViewResource` 两段式封装，落地时收敛为 VulkanImage 单变量）：
- **跨帧记忆**：VulkanImage 持一个 `currentLayout`，帧末图把最终停靠布局写回（`set_layout`）；帧首直接读它作为起点。这正是「上一帧决定本帧起点」的载体，已覆盖大多数情况。
- **无记忆起点**：`currentLayout` 的 NSDMI 默认值是 `eUndefined`。首帧 / swapchain 重建 / 从未进图时，直接以默认值为帧首起点 —— 对「新内容未定义、需丢弃型转换」的图像（swapchain 图）是正确的。
- **非 Undefined 起点的图像**（已上传停在采样态、深度图等）：持有方须在创建后、进图前调 `set_layout(其真实布局)`，否则会被误判为 Undefined（丢弃内容）。S2 接线时对进图的采样纹理须设 SRV；swapchain 图默认即可。

「内容可否丢弃」由图像初始 layout 是否为 `eUndefined` 表达，不再需要独立枚举。

这样一个外部图**可以**先被 pass A 当采样读、再被 pass B 当颜色附件写、最后回采样态 —— 图自动补齐 A→B、B→末态的转换。

### 4.3 属性（usage）声明决定「谁先谁后」

图把「读」与「写」分层，跨 pass 同步靠运行期的 `lastWriterPass` 单链（见 §5.4）推导：

| 声明 | 依赖（运行时推导） | 效果 |
|---|---|---|
| **只读**（`readImages` 里的图） | 读依赖上一个写它的 pass | 前驱写完后才开始（屏障 src = 该资源最近写者的输出） |
| **写**（附件） | 写依赖上一个写它的 pass（写-写串行） | 前驱整个 pass 结束后才开始 |
| **读写混合** | 同 pass 内访问按声明收集、句柄去重（先声明者为准） | v1 不在 pass 内部做读写重叠分析；渲染反馈循环（同 pass 既当附件又当采样）由调用方自担（§4.3 注） |

**声明序约束（v1 关键规则）**：pass 必须按依赖序声明 —— 读某资源须声明在其写者之后。若把读者声明在写者之前，图不会自动重排，读者会读到「图像当前 layout 对应的旧内容」（无写者前置时直接以帧首 layout 为起点）。这是 v1 取舍：图负责在给定顺序下做对同步，不负责替你排顺序。

**隐含假设（必须写进注释）**：同一张图在**同一 pass 里既当附件又当输入**属于未定义用法（动态渲染不支持读自己的附件；传统 renderpass 才支持 input attachment 回读）。v1 已不做编译期拦截（随 m_PassAccess 一起移除），依赖调用方自担。

### 4.4 屏障插入的粗粒度边界

图在**整个 frame 层**粒度做同步 —— 屏障只可能出现在 **pass 边界**，每个 pass 内部（用户回调里）不可能出现屏障，且每个 pass 前有且仅有一个汇总屏障：

```
Barrier 合并上游对该资源的所有写入（对读）；Barrier 前一个写者等后一个写者（对写）
```

**理由**（针对 raster 路径）：颜色附件写入的「end-of-pass」，恰好与动态渲染的结束点一致；深度附件同理。因此：

> 跨 pass 的同步可以安全地落在「上一个写者结束」处 —— 这正好就是 pass 的 `endRendering` 之后。**在 pass 边界同步 = 在 pass 内完全不需屏障**。这也让 `execute` 回调里**禁止**任何 `vkCmdPipelineBarrier` 成为规则而非约定。

### 4.5 写后布局（finalLayout）与采样端点

Pass 描述中：颜色附件 + `storeOp == eStore` 时，可以给一个**写后布局**（`finalLayout`，默认 `eColorAttachmentOptimal`）。图在这个 pass 之后、下一个读它/呈现它的 pass 之前，自动安排到该布局。采样纹理声明（`readImages`）的布局是**采样期望布局**（通常 `eShaderReadOnlyOptimal`）。图在这些约束间做最小时序匹配。

### 4.6 运行期外部资源状态回写（这是把正确性「具象化」的关键）

图做同步时需要知道「外部图上一帧结束时停在哪」。若只让图持有其内部布局（以帧为单位），**图无法知道自己上一帧干了什么**（图对象可能每帧重建）。

因此 Execute 收尾把本帧每个外部资源的最终停靠布局，**写回其 VulkanImage 的 `currentLayout`**（`rec.external->get_image().set_layout(layout)`）。这样即便 `RenderGraph` 每帧新建，它引用的**同一批** `VulkanImage` 实例跨帧记住了自己的布局。这是自研渲染图里最容易被低估的一环 —— 帧间布局必须由「外部图像（VulkanImage）」承接，而不是图本身。WSI swapchain 图例外（`isFrameSwapchain`），其布局由 acquire 决定、不写回。

---

## 5. 核心设计：声明序顺序执行 + 逐 pass 前置屏障

> 早期设计稿的「编译器三阶段」（依赖图 §5.1 → 拓扑排序 §5.2 → 屏障 §5.4）在落地时简化为两步：**依赖建图与拓扑排序已移除**。原因：依赖边只朝声明序前方延伸（只找 curPass 之前的最近写者），故 Kahn+最小堆的拓扑输出恒等于声明序 `0..n-1`，拓扑排序对执行不产生任何重排。因此 v1 直接按声明序遍历 pass，运行期用 `lastWriterPass` 单链做跨 pass 同步（见 §5.2）。

帧首调用 `graph.Compile()`(现为恒 true 的轻量校验)，`graph.Execute()` 按声明序逐 pass：收集本 pass 资源访问 → 生成前置屏障 → 打开动态渲染 → 调 execute 回调 → 收尾。

### 5.0 数据结构

```cpp
// RenderGraph 成员：
std::vector<ResourceRecord> m_Resources;   ///< 资源表（句柄 = 下标 + 1）
std::vector<RenderPassDesc> m_Passes;      ///< pass 节点（按声明序）

// 资源表行：每行对应一个资源（外部或虚拟）。
struct ResourceRecord {
    std::string        name;               ///< 资源名（调试）
    VulkanImageView   *external = nullptr; ///< 外部导入的图 view（经其 image 读写布局记忆）
    RenderGraphResourceDesc virtualDesc;   ///< 虚拟资源描述（v1 不分配）
    bool               isFrameSwapchain;   ///< 是否本帧 WSI 图
};
```

pass 存于 `m_Passes`（按声明序 = 执行序）；跨 pass 同步不需要「边」，而是运行期逐资源的 `lastWriterPass` 单链（见 §5.2）。

### 5.1 声明序即执行序（pass 的约束）

v1 图**不重排 pass**：`Execute` 从 0 到 n-1 按声明序遍历。因此：

1. pass 必须按依赖序声明 —— **读某资源须声明在其写者之后**；
2. 读者声明在写者之前 = 读者以「帧首 layout / 无写者前置」为起点，读到旧内容（不报错，是合法语义「读外部已就绪内容」）；
3. 不做环检测 —— 声明序天然无环，无依赖边可检；
4. 无依赖的 pass 保持声明序串行执行，但彼此不插入屏障。

### 5.2 逐资源最近写者（跨 pass 同步的载体）

运行时维护 `lastWriterPass`（资源句柄 → 本帧最近写它的 pass 下标）。对每个 pass 的每个资源访问（附件=写、采样=读）：

- **有前置写者**（`lastWriterPass` 命中）→ 在该 pass 前插一条屏障：src = 前置写者的写阶段/写访问，dst = 本次读/写阶段/访问。写-写也经此串行。
- **无前置写者**（帧内首次访问）→ 需布局转换时插一条 srcStage=`eTopOfPipe` 的屏障（等待此前全部命令；对 Undefined 丢弃型起点安全）。
- 布局转换合并进屏障；本 pass 若写该资源，推进 `lastWriterPass[h] = pi`。

### 5.3 Execute 逐 pass 流程

**输入：`m_Passes`（声明序）+ 当前帧 `VulkanCommandBuffer`。**

对每个 pass（按声明序下标 pi）：

```
1. 收集本 pass 的资源访问（颜色附件 → 深度 → 采样，句柄去重，先声明者为准）
2. 前置屏障：对每个访问生成 barrier（见 §5.2），合并为一次 pipelineBarrier2
3. 录制本 pass：
   - 生成动态渲染信息：colorAttachment 为资源 X → imageView = X.view；
       imageLayout = X 目标布局；loadOp/storeOp = 声明；loadOp=clear 填 clearValue
   - 调 execute 回调（图已打开动态渲染，回调内录制绘制命令）
4. 收尾：endRendering；若本 pass 写某资源，记入 lastWriterPass
```

**帧尾布局回写**：Execute 结束后，把每个外部资源（非 WSI 图）的最终停靠布局写回其 VulkanImage（`set_layout`），供下一帧作起点（§4.6）。

**execute 回调上下文** —— pass 需要拿到：命令缓冲 + 一组绑定资源句柄映射（纹理 / 缓冲 / UBO 帧分配）。这些来自**帧池**（`VulkanRenderFrame`），与现有 Renderer2D/3D 的帧内分配一致：

```cpp
struct PassExecuteContext {
    VulkanCommandBuffer &cmd;
    VulkanRenderFrame   &frame;         ///< 本帧（用其缓冲/描述符池）
    // 绑定好的描述符 / 句柄查询由实现层提供（例如一张表：logical resource → image view / buffer）
};
```

### 5.4 自动屏障（解决「屏障在哪、掩码是什么」）

运行时按声明序扫描，对每个资源的跨 pass 访问序列维护同步源（即 §5.2 的 `lastWriterPass`，配合每资源帧内布局状态）：

```
每资源运行期状态（Execute 内的局部映射）：
    vk::ImageLayout currentLayout;          ///< 该资源当前的布局（随执行推进）
    bool            layoutValid;
    uint32_t        lastWriterPass;         ///< 本帧最后一次写它的 pass（跨 pass 屏障的源）
```

屏障源侧掩码由 `FindUsageInPass(lastWriterPass, 资源)` 查最近写者的写用法推导。

| 前一动作 | 需要 | 屏障属性来源 |
|---|---|---|
| 读依赖（下个 pass 读） | `srcStage=写者输出阶段` `srcAccess=写访问` `dstStage=读者输入阶段` `dstAccess=读访问` | **属性由使用**（写者 = 附件写入 COLOR_ATTACHMENT_OUTPUT；读者 = 采样 FRAGMENT_SHADER_READ）|
| 写依赖（下个 pass 写） | `srcStage=写者输出阶段` `srcAccess=写访问` `dstStage=写者输入阶段` `dstAccess=写访问` | 两者都来自 **该资源的写使用**（附件写入 → COLOR_ATTACHMENT_OUTPUT，_WRITE）|

**布局转换合并进同一条屏障**：`oldLayout = 上一状态`，`newLayout = 下个 pass 对该资源的期望布局`。这正是把 §4.5 的 finalLayout 意图落到运行期。

**为什么这是「自动」的**：开发者从不写 `vk::PipelineStageFlags` / `vk::AccessFlags` / 布局。只要给每个 **资源用法**（`ColorAttachment` / `DepthStencil` / `Input`/`ShaderRead` / `ShaderWrite`/`Transfer`）**一张内置的表**，把「用法 → 输出阶段、写访问；用法 → 输入阶段、读访问」写好，屏障掩码与布局就全由推导生成。

**共享同一命令缓冲的保证**：因执行序内所有屏障命令都录在**同一条 command buffer** 上，无需处理跨 command buffer 的信号量/事件；需要的只是内存屏障本身的 stage/access/layout。

**隐藏的下限**：如果开发者把一个资源**既当附件又当输入**（同一 pass 内，渲染反馈循环），动态渲染不支持，v1 不再做编译期断言拦截（随依赖建图一起移除），依赖调用方遵守 §4.3 的声明序约束。

---

## 6. 与现状拼装：如何把这套东西接进现在的渲染器

### 6.1 建设蓝图：阶段 1 最小闭环

**目标**：把现有的「离屏 3D 场景 → 编辑器视口展示」拆成三段 Pass，由图自动同步，行为与现状逐帧等价。

```
Pass "Scene3D"        （颜色附件 = 视口离屏图，深度附件 = 视口深度；清屏；写后布局 = ShaderReadOnlyOptimal）
  └─ 读：环境的 IBL / 天空盒贴图、材质贴图（这些是常驻采样纹理）
Pass "Scene2D"        （在视口颜色附件之上叠加世界精灵；loadOp = Load）
Pass "UIPass"         （把 3D+2D 合成结果当采样输入画到 swapchain；另有 UI 精灵直写 swapchain）
```

对应现状：`SceneViewport.cpp` 创建离屏 RenderTarget（外部图），`SceneLayer.cpp` 里 2D/3D 先离屏、再喂给 ImGui 上屏。**图把它变成三个显式 Pass** —— 现状的「2D/3D/UI 谁先谁后的时序依赖」变成**按声明序排列的 pass 序列**（图保证每 pass 前置屏障正确）。

**迁移顺序（每一步都是「可运行 + 行为不变」的增量）**：

| 步骤 | 内容 | 验收 |
|---|---|---|
| 1 | `RenderGraph` / `RenderGraphBuilder` 骨架 + `RenderPassDesc` + Execute（声明序逐 pass 屏障 + 动态渲染 + 空回调） | 空图编译执行不崩 |
| 2 | 「离屏 3D + 2D → 视口」改为两张 pass 声明 | 编辑器视口画面与迁移前一致 |
| 3 | swapchain 合成 pass：3D 结果当采样、UI 直写 | ImGui 覆盖在场景之上，顺序正确 |
| 4 | 把 `Renderer.cpp` BeginFrame/EndFrame 的 swapchain 布局转换吸收进图的「首尾终点」 | 行为等价、屏障由推导生成 |

每步对照现有场景：ImGui 场景（有离屏视口）与无编辑器纯渲染路径都要回归。

---

## 6A. 阶段 1 第一版实现计划（类划分 / 文件位置 / 对接接口）

> 本节给"阶段1 代码长什么样"的**逐文件**答案。与 §6.1 的阶段步序对应，每步结束都是可运行、行为与现状等价的增量。

### 6A.1 新文件清单（阶段1，全部在 `GE` 引擎侧）

| 文件 | 内容 | 依赖 |
|---|---|---|
| `GE/include/GE/Render/RenderGraph/RenderGraphTypes.h` | 纯数据结构：`ResourceHandle`、`ResourceType`、`ResourceUsage`、`AttachmentDesc`、`ImageInputDesc`、`PassType`、`RenderGraphResourceDesc` | `Render/VulkanBase/VulkanCommon.h`（vk 类型） |
| `GE/include/GE/Render/RenderGraph/RenderPassDesc.h` | `RenderPassDesc`（一个 pass 的全部声明 + `execute` 回调）+ `PassExecuteContext` | `RenderGraphTypes.h`、`Render/VulkanBase/VulkanCommandBuffer.h` |
| `GE/include/GE/Render/RenderGraph/RenderGraph.h` | `RenderGraph`（图本体：pass 序列 + 资源表）+ `RenderGraphBuilder`（Import / AddPass / Compile / Execute） | `RenderPassDesc.h`、`RenderGraphTypes.h`（`VulkanImageView` 仅前向声明） |
| `GE/src/Render/RenderGraph/RenderGraph.cpp` | Execute：声明序逐 pass 收集访问 → 生成屏障 → 命令录制；帧首/帧尾布局经 VulkanImage 读写 | `VulkanImageView.h` |
| 布局记忆（不单独成文件） | 跨帧布局下沉到 `VulkanImage` 的 `currentLayout`（`set_layout` / `get_layout`），不新增封装 | `VulkanImage.h` |
| 对现有文件**零改动**（见 6A.4 说明） | — | — |

> 位置遵循现状约定：头文件在 `GE/include/GE/Render/**`，实现同名映射到 `GE/src/Render/**`（对照 `Render/` 下已有 `RenderTarget.h / VulkanRenderingInfo.h` 的平铺风格）。阶段1 不新建子命名空间，类型一律 `GE::`，避免给渲染器加一层作用域噪音。

### 6A.2 类划分与责任边界

```
┌─────────────────────────────────────────────────────────────────┐
│ RenderGraph（每帧实例化，帧末析构；由 Renderer 拥有）                │
│  · 成员：pass 序列（按声明序）、资源表（每行持 VulkanImageView*）    │
│  · Compile() ：轻量校验（声明序即执行序，恒通过）                   │
│  · Execute() ：按声明序逐 pass：前置屏障 → 动态渲染 → execute 回调   │
│               帧末把布局写回各 VulkanImage（WSI 图除外）            │
└─────────────────────────────────────────────────────────────────┘
```

| 类 | 责任 | 不做什么 |
|---|---|---|
| `RenderGraphBuilder` | 导入外部图 view / 创建虚拟资源 / 逐个 `AddPass` 填充声明 | 不持有任何 GPU 对象 |
| `RenderGraph` | 运行期：按声明序逐 pass 收集访问、生成前置屏障、驱动动态渲染与回调；帧首/帧末经 VulkanImage 读写跨帧布局 | 不做资源分配 / 跨帧复用 / 别名 / 依赖重排 |
| `VulkanImage`（既有类，加两字段） | **跨帧**记住"停在哪个布局"（`currentLayout`），供图帧首作起点、帧末写回 | 不参与 pass 声明 |
| `PassExecuteContext` | 透传本 pass 需要的：cmd、frame、绑定句柄表 | 不直接持渲染器引用 |
| 屏障工具（匿名命名空间函数） | 依据 §7 资源用法表生成同步2 掩码（`vk::ImageMemoryBarrier2`） | 不进公共 API |

**关键**：图只认 `VulkanImageView*`（外部）与虚拟资源句柄，**不识别也不持有 `RenderTarget`**。现在 2D/3D 绑定渲染目标用的是 `Renderer3D::SetRenderTarget(RenderTarget*)`（SceneLayer.cpp:196/197），迁移后由**调用方**把目标**拆成若干 ImageView 导入**并挂到 pass 附件声明 —— 谁要把场景画进某张图，谁负责 import 它。这样图对"渲染目标"无感知，天然同时支持 swapchain 目标与 SceneViewport 离屏目标。

### 6A.3 RenderPassDesc.execute 的两层（承接 2D/3D 的接口）

现有 2D/3D 录制不能整个塞进 execute 回调 —— 它们的 `DrawSprite`/`DrawMesh` 在**主线程逐实体遍历时**收集（Scene.cpp:843），帧内集中上报；而 execute 只在**图执行那一刻**调用一次。二者频次不同。**第一版以两种 execute 共存收尾**：

| 形式 | 适用 | 对应现状 |
|---|---|---|
| **Lazy execute**（**第一版默认**）：`execute` 里调用"该 pass 的采集函数"，采集函数立即在同一 command buffer 上录制 draw | 网格/精灵绘制本就可整体推迟到 EndScene 再录制（`CollectBatches` 已在 EndScene 前把所有批次备好） | `Renderer3D::EndScene()` 中"批次上传 → BeginDynamicRendering → 逐批 Draw"整段搬进回调 |
| **Immediate 子pass**：先用 `BeginScene` 采集、`EndScene` 落空到本 pass（等效现在"采集完即画"） | 图结构稳定后、ImGui 绘制等一次绘制的场景 | 现状 `SceneLayer.cpp` 里先 `OnUpdate` 后 `OnImGuiRender` 的次序 |

> 两层的**分界线在"采集与录制能否分离"**：能分离（3D：提交命令前已 `SortMeshes` 且批次齐全）→ Lazy；不能（2D UI 精灵先要主线程拿相机矩阵与 Viewport 尺寸，再到 pass 绘制）→ 仍按帧序调用，只是落点变成 pass。**这层是阶段1 最大的设计分叉**：先把 Lazy 打通并让 2D/3D 无行为变化地落在同一条命令缓冲，是把 ImGui / 2D / 3D 并排进图的最小前提。

### 6A.4 现有类的改动点（接口适配，非重写）

| 类 | 改动 | 理由 |
|---|---|---|
| `Renderer2D` / `Renderer3D` | 各加一组"录制落到指定 cmd"的重载；`EndScene` 内 `Renderer::GetFrameCmd()` 改为可注入 cmd（或新增 `EndScene(cmd)`） | 现状硬编码全局 cmd（Renderer2D.cpp / Renderer3D.cpp 均 `GetFrameCmd`）。图执行序要求"图说往哪录就往哪录" |
| `RenderTarget` | **阶段1 不改**（`GetColorView()` 等已暴露 ImageView）；若要 pass 直接消费它，由 `RenderGraphBuilder` 用其 view `Import` 进图 | 避免把图的资源表耦合进 RenderTarget |
| `Renderer` | 持有 `RenderGraph m_Graph`（每帧 new + 帧末析构）或由 Application 建图传入；`BeginFrame` 后先 `graph.BeginFrame(swapchain图)` | swapchain 的布局转换从此入图（替代 Renderer.cpp:99/139 的手工两处） |
| `Renderer::BeginFrame / EndFrame` | 屏障逻辑迁移至图，仅保留 acquire/submit/present | Renderer.cpp:99-107、139-147 两条手工转换删去 |
| `VulkanCommandBuffer` | 提供 `Raw()` 访问裸 `vk::CommandBuffer`（已有 `GetHandle()`），不新加 API | 屏障生成与动态渲染已能经 handle 录制 |
| `ImGuiLayer` | **阶段1 不动**（仍走 `Application::GetFrameCmd()` 直写）。阶段1 收尾若 pass 化 ImGui，用 `CommandBufferSubmission` 期序守卫；完全 pass 化放阶段2 | ImGui 有自己的命令提交约定，抢在这步做会拖慢阶段1 |

### 6A.5 逐步骤迁移明细（替换 §6.1 表格的验收文字）

| 步骤 | 代码动作 | 行为验收 |
|---|---|---|
| **S1** 骨架 | 建 `RenderGraphTypes / RenderPassDesc / RenderGraph(+Builder)`；Execute 实现声明序逐 pass：收集访问 → 前置屏障 → 动态渲染 → **空 execute 回调**；跨帧布局用 `VulkanImage::currentLayout` | 空图 `Compile`+`Execute` 不崩；无 RenderGraph 实例时零行为变化 |
| **S2** 视口 pass 化 | `SceneLayer.cpp` 里"先 3D 后 2D 画进离屏目标"改为：add 两个 raster pass（颜色附件 = 视口离屏图的 view；Scene3D 带深度附件 + `eClear`；Scene2D 附件 `eLoad` 不写深度）。`Renderer3D::EndScene` 落进 pass cmd | **编辑器视口画面与 S1 前逐帧一致**（这是本方案最重要的验收红线） |
| **S3** 视口 → 采样 | 新增"合成 pass"：把离屏颜色图（`ShaderReadOnlyOptimal`）当采样画到 swapchain，UI 精灵与视口图同 pass；ImGui 仍直写 swapchain | ImGui 面板能正确叠在场景上（含视口图），顺序无错乱 |
| **S4** 吸收 swapchain | `Renderer::BeginFrame` 的 `Undefined→ColorAttachmentOptimal`、`EndFrame` 的 `ColorAttachmentOptimal→PresentSrc` 改由图首尾态承接（swapchain 图 `currentLayout` 默认 `eUndefined`，末帧帧尾写回 `ePresentSrcKHR`，见 §4.2/§4.6） | 纯渲染与编辑器双路径都无回归；屏障数量不增 |
| **S5** 回归 | 对拍：编辑器 + 无编辑器两条路径，GPU 上 barrier 数 ≤ 迁移前 | 见 §6.4 断言，RenderDoc 目检 |

**要点**：阶段1 不做 compute/transfer、不做虚拟资源实例、不做 ImGui pass 化。同步已提前到同步2（见 §7 附注）。pass 必须按依赖序声明（§4.3）；执行遇问题先查 §6.4 断言。

### 6A.6 帧循环接线（现状 → 图版）

现状（Application.cpp:90-109 + 各 Layer）：

```
BeginFrame()                // 内部已手工转 swapchain → ColorAttachmentOptimal
  layer->OnUpdate           // SceneLayer: SetRenderTarget(离屏) → OnUpdate3D(3D+2D画离屏) → SetRenderTarget(null)
  ImGui Begin/OnImGuiRender/End   // ImGuiLayer::End 直写 GetFrameCmd → swapchain
EndFrame()                  // 手工转 swapchain → PresentSrc
```

图版（阶段1 S2–S4 后的目标形态）：

```
RenderGraph &g = renderer.BeginGraphFrame()          // 取代原手工布局转换
  SceneLayer  addPass(Scene3D, 附件=视口图+深度, cmd 由 g 提供)
  SceneLayer  addPass(Scene2D, 附件=视口图 eLoad)
  UIPass      addPass(合成,    输入=视口图采样)
  ImGuiLayer  addPass(ImGui,  直写 swapchain / loadOp=eLoad)   // S3 后
g.Execute()                                             // 顺序+屏障全在图内
EndFrame()                                              // 仅 submit + present
```

### 6A.7 待定项（写进代码注释，防止实现时走偏）

1. **2D/3D 的 `SetRenderTarget`**：阶段1 保留现状（Override + 复位），只是多一层"哪个 pass 在录它"。完全移除（pass 不再由 2D/3D 选目标，而由 addPass 声明）放阶段2。**这是中间态，别为它发明接口**。
2. **ImGui 与 2D/3D 命令提交是否仍走同一 `GetFrameCmd`**：S3 前保持现状，让合成 pass 与 ImGui 共用同一条命令缓冲是阶段1 的临时约束；真正拆命令缓冲（多 submit）属跨队列演进，不在此期。
3. **执行序只服务"能画对"，不服务"并行"**：本阶段产出线性命令流，两个无依赖 pass 仍串行录制，只是不再互相插入屏障 —— 这已是阶段1 的收益上限，并行留给未来。

### 6.2 迁移中的 2D / 3D 责任

图本身不做绘制，只做**编排**。`Renderer2D::EndScene` 与 `Renderer3D::EndScene` 内部的绘制逻辑（清屏、管线绑定、批量提交）整体**搬进 execute 回调**，语义保持不变：

- 回调从 `PassExecuteContext` 拿 `cmd` / `frame`（即现在 `Renderer::GetFrameCmd()` / `GetActiveFrame()` 的对象）—— 唯一变化是「何时调用、往哪个附件写」由图决定。
- **2D/3D 不再各自调用 `Renderer::GetFrameCmd()` 拿到的全局 command buffer**，改为图传入的、属于本 pass 的那一份（现阶段还是同一条命令缓冲）。
- 2D/3D 对 `GetRenderContext()` 的依赖（帧 UBO 分配、描述符）保持不变。

**半透明批的先后**（`Renderer3D.cpp` §透明计划）：同一「Scene3D」pass 内不透明段与透明段保持现状顺序 —— 因为透明是**段**不是 pass。若未来透明需要独立 pass（单独深度附件、后处理介入），那时图自然承接：新增一个 pass 节点、读不透明深度即可。

### 6.3 swapchain 与外部资源的接入

swapchain 图像：`VulkanImage` 的 `currentLayout` 默认 `eUndefined`（引擎现在 BeginFrame 用 `eUndefined → eColorAttachmentOptimal` 是**丢弃型转换**），frame 末图把 `ColorAttachmentOptimal / ShaderReadOnlyOptimal → PresentSrcKHR` 作为 **收尾转移**（Execute 帧末写回 swapchain 图所在 VulkanImage 的 `currentLayout`，等价于现在 `Renderer.cpp:146` 的手工转换；但 WSI 图例外不写回，见 §4.6）。运行时 layout 由 VulkanImage 的 `currentLayout` 管理 —— 不再有「BeginFrame 手工转、EndFrame 手工转」这种全局命令位置依赖。

### 6.4 错误报告（这一节让自研渲染图值得）

**断言是调试期回报最高的资产**。v1 保留的检查（随声明序执行简化的取舍）：

1. **漏绑定附件**：color attachment 声明了，但 execute 回调没往该 view 写东西 → 编译期难查，运行时加一次校验：回调前后查询该图像 usage 状态（有 `vkCmdPipelineBarrier` 在回调里就告警）。
2. **写后没定义终点**：资源被写、被读，但**从没**安排回目标布局 → 图形帧末该布局永远停在这 → 下一次使用时错。

> 已移除的早期断言（随依赖建图/拓扑排序删去）：
> - **同 pass 内读写自依赖**（渲染反馈循环）拦截、**循环依赖**检测 —— 依赖边与 `m_PassAccess` 收集已删，v1 靠 §4.3 的声明序约束（读须声明在写者之后）由调用方自担。
>
> 仅凭「自动屏障」不足以证明图值得引入；真正价值在**错误能定位到具体 Pass + 具体资源**。RenderDoc 里每一帧只剩一个明确形状：pass 用同一条命令缓冲录制、屏障在 pass 边界只有一个汇总屏障。

---

## 7. 资源用法表（属性 → 阶段 / 访问 / 布局的最小真值表）

Execute 从资源用法推导掩码。映射表放在引擎一处（渲染图内部，单一来源），**替代**现在散落在各渲染器与 `image_layout_transition` 里的查表。

| 用法（Usage） | 输出阶段 | 写访问 | 输入阶段 | 读访问 | 布局 |
|---|---|---|---|---|---|
| `ColorAttachment` | COLOR_ATTACHMENT_OUTPUT | COLOR_ATTACHMENT_WRITE | COLOR_ATTACHMENT_OUTPUT | COLOR_ATTACHMENT_READ | ColorAttachmentOptimal |
| `DepthStencilAttachment` | EARLY_FRAGMENT / LATE_FRAGMENT | DEPTH_STENCIL_ATTACHMENT_WRITE | EARLY/LATE_FRAGMENT | DEPTH_STENCIL_ATTACHMENT_READ | DepthStencilAttachmentOptimal |
| `Input`（片元输入/采样） | FRAGMENT_SHADER | SHADER_READ | FRAGMENT_SHADER | SHADER_READ | ShaderReadOnlyOptimal |
| `ShaderRead`（通用读） | — | SHADER_READ | VERTEX/FRAGMENT/COMPUTE | SHADER_READ | ShaderReadOnlyOptimal |
| `ShaderWrite`（阶段2） | COMPUTE | SHADER_WRITE | COMPUTE | SHADER_WRITE | General |
| `Transfer`（阶段2） | TRANSFER | TRANSFER_WRITE | TRANSFER | TRANSFER_READ | TransferSrc/DstOptimal |

颜色附件「读」阶段仅在写后采样才需要（如后处理读回）；基础绘制 pass 读的是深度附件（早期片元测试）与采样纹理 —— 上表已覆盖。

> 同步已统一到同步2（`VulkanImage.cpp` 的既有范式）：RenderGraph 用 `vk::ImageMemoryBarrier2` + `cmd.pipelineBarrier2`，stage/access 直接携带在结构体内，无需在命令提交处再推导合并（计划书初稿把同步2 列为阶段2 升级点，S1 落地时因引擎已全用同步2 而提前完成）。

---

## 8. 性能特征（为什么这层几乎不花钱）

| 项 | 说明 |
|---|---|
| **CPU 开销** | 无依赖建图 / 拓扑排序（已移除），每帧 Execute 仅为每个 pass 收集访问并生成屏障，~10 pass 级开销远 < 10μs，相比录制命令缓冲的几十~上百 μs 可忽略 |
| **命令缓冲额外命令** | 布局转换只在 **pass 边界** 需要；同一资源跨 pass 的同步用运行期 `lastWriterPass` 合并成一次屏障。现状的手工屏障数**只会减少** |
| **不增加 GPU 往返** | 全部同步在单 command buffer 内，无额外 submit / 等待 |
| **同步2** | S1 已用 `pipelineBarrier2`，stage/access 在结构体内、可精细合并 | 已落地，无额外成本 |

---

## 9. 路线图

### 阶段 1：骨架 + 最小闭环（目标：行为等价迁移，预计为本次实现主体）

**步骤定义见 §6A.5（S1–S5），每个 S 步结束是可运行、行为与现状等价的增量。** 文件与类见 §6A.1–6A.4。

- [x] S1：`RenderGraph` 数据结构 + `RenderPassDesc` / `AttachmentDesc` 声明；`RenderGraphBuilder`：Import / AddPass / Compile / Execute
  - 已落地文件：`RenderGraphTypes.h` / `RenderPassDesc.h` / `RenderGraph.h` / `RenderGraph.cpp`（见 §6A.1）+ 根 `CMakeLists.txt` 登记源码目录
  - 已实现：Import / CreateVirtualResource / AddPass / Compile（轻量）/ Execute（声明序逐 pass 前置屏障 + 动态渲染录制 + 帧尾布局写回 VulkanImage）；空图可编译执行
  - 设计简化（相对初稿）：依赖建图 + 拓扑排序已移除（声明序即执行序，§5 开头）；`ImageViewResource` 封装已移除、布局记忆下沉 VulkanImage 单变量（§4.2/§4.6）
  - 尚未接线：任意 Scene 渲染路径仍走原 Renderer2D/3D（S1 只交付骨架，零行为变化）
- [ ] S2：迁移「离屏 3D + 2D → 视口展示」到 Scene3D / Scene2D 两 raster pass（§6A.5）；2D/3D 录制落到 pass cmd（§6A.4）
- [ ] S3：合成 pass（视口图 → swapchain 采样），UI 精灵同 pass
- [ ] S4：吸收 swapchain 首尾布局（替代 Renderer.cpp 两处手工转换，§6A.5）
- [ ] S5：回归对拍（编辑器 + 无编辑器两路径），资源用法表（§7）与编译期/运行期断言（§6.4），RenderDoc 验证 pass 边界只有一个汇总屏障

**S1 遗留（实现时记录，S2 起解决）**：
- 虚拟资源不产生屏障（无真实图，阶段2 支持）
- 附件 `finalLayout`（写后转采样/呈现布局）尚未接线 —— 当前渲染一律停在写布局，S4 的收尾转换补上
- `renderArea` 未指定时以 1x1 占位并告警 —— S2 由调用方填好
- 屏障源阶段在首次访问（无前置写者）时用 `eTopOfPipe` 保守等待此前全部命令 —— 首次丢弃型（Undefined 起点）正确，首次保留型（跨帧 Load）依赖持有方把 VulkanImage 的 `currentLayout` 设成真实起点（见 §4.2）

**验收**：
- 编辑器视口（场景 3D + 世界精灵 + ImGui UI）渲染结果与**迁移前逐帧等价**
- 无编辑器纯渲染路径回归
- RenderDoc 中布局转换在 pass 边界、命名清晰、数量 ≤ 现状手工屏障

### 阶段 2：compute / transfer pass + 后处理
- [ ] `PassType::Compute` 支持（dispatch + 图像存储读写，补 ShaderWrite 行）
- [ ] 可选 `Transfer`（清空 / 拷贝）
- [ ] 接入首个后处理链（色调映射 / 泛光 ping-pong）验证多 pass 级联

### 阶段 3（可选）：结构缓存
- [ ] pass 结构按 key 缓存、每帧只跑参数更新（§3 中间路线）

---

## 10. 未来预留（不阻塞 v1）

- **跨队列**：图目前只产出「同一条命令缓冲的线性命令流」。若将来出现 transfer / compute 专用队列，需把 barrier 升级为 `semaphore`（队列间）—— 接口层面变化，但 Pass 声明方式不变（读写关系不依赖队列）。
- **帧内资源别名**：虚拟资源支持 alias（同帧两个虚拟纹理复用物理内存），是持久图时代的主要内存收益。v1 仅预留 `transient` 标志。
- **RenderPass / 传统 renderpass**：本项目已用动态渲染，不回流。
