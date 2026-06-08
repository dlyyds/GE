# GameEngine 渲染器架构设计

> 基于现有代码分析，对渲染器架构进行分层设计，明确演进路线。

---

## 目录

1. [现状分析](#1-现状分析)
2. [总体架构分层](#2-总体架构分层)
3. [Layer 1: VulkanBase（现状 + 微调）](#3-layer-1-vulkanbase现状--微调)
4. [Layer 2: RenderCommand + RendererAPI](#4-layer-2-rendercommand--rendererapi)
5. [Layer 3: Renderer2D 重构](#5-layer-3-renderer2d-重构)
6. [Layer 4: RenderGraph（远期）](#6-layer-4-rendergraph远期)
7. [演进路线图](#7-演进路线图)
8. [附录：数据流对比](#8-附录数据流对比)

---

## 1. 现状分析

### 当前架构

```
Application
  └─ Renderer2D（单例）
       ├─ VulkanInstance
       ├─ VulkanDevice (+ VMA)
       ├─ VulkanSwapchain (+ VulkanPerFrame × N)
       ├─ VulkanBuffer        ← 共享 UBO，每帧被多次覆写
       └─ 直接调用 vulkan API
```

### 已有的优点

| 特性 | 说明 |
|---|---|
| Vulkan 1.3 dynamic rendering | 没有 VkRenderPass 对象，简洁高效 |
| VMA 管理内存 | `VulkanBuffer`、`VulkanImage` 均已集成 VMA |
| 持久映射 | `MAPPED_BIT` + memcpy，免去每帧 map/unmap |
| Dynamic states | viewport / scissor / cullMode / frontFace / topology 全动态设置 |
| Swapchain 封装 | `BeginFrame` / `EndFrame` 逻辑清晰，每帧资源管理完善 |
| 组件分离 | Camera、Mesh、Material 各司其职 |

### 当前瓶颈

| 问题 | 影响 |
|---|---|
| **单 UBO，每帧覆写** | 一帧只能画一个物体，`Draw()` 多次调用只有最后生效 |
| **没有 RendererAPI 抽象层** | 硬编码 Vulkan，无法切换到其他后端 |
| **Renderer2D 职责过重** | 既做 API 入口，又直接写 command buffer，还管理资源 |
| **固定管线 layout** | 只有 binding 0 = UBO + binding 1 = texture，无法扩展 |
| **无管线缓存** | 每次创建新管线都重复构造，性能浪费 |
| **无 render pass 编排** | 所有物体画进同一个颜色附件，无法做多 pass 渲染 |

---

## 2. 总体架构分层

渲染器分为 **四层架构**，逐层抽象递增：

```
┌──────────────────────────────────────────────────────────────┐
│  Layer 4:  Scene / RenderGraph                               │
│  （场景管理、render pass 编排、可见性裁剪、光照）              │
│                                                              │
│  职责：决定 "画什么" 以及 "什么顺序画"                        │
├──────────────────────────────────────────────────────────────┤
│  Layer 3:  Renderer2D / Renderer3D                           │
│  （高层渲染 API：BeginScene / Draw / EndScene,                │
│   材质系统、着色器变体管理、管线缓存）                         │
│                                                              │
│  职责：决定 "用什么材质画"                                    │
├──────────────────────────────────────────────────────────────┤
│  Layer 2:  RenderCommand / RendererAPI                       │
│  （抽象 API：DrawIndexed / BindPipeline / SetUniform,          │
│   可切换后端：Vulkan ↔ OpenGL ↔ Metal 的回溯点）               │
│                                                              │
│  职责：决定 "怎么调用 GPU"                                    │
├──────────────────────────────────────────────────────────────┤
│  Layer 1:  VulkanBase（现有代码）                             │
│  （Instance, Device, Swapchain, Pipeline, Buffer, Image,      │
│   Sampler, DescriptorPool/Set — 薄封装，不包含业务逻辑）       │
│                                                              │
│  职责：封装 Vulkan 对象生命周期                               │
└──────────────────────────────────────────────────────────────┘
```

### 依赖方向

```
Layer 4 → Layer 3 → Layer 2 → Layer 1
```

每一层只依赖它的下一层，**不允许跨层调用**。

---

## 3. Layer 1: VulkanBase（现状 + 微调）

### 现有文件清单

```
GE/
├── include/GE/Render/VulkanBase/
│   ├── VulkanInstance.h
│   ├── VulkanDevice.h
│   ├── VulkanSwapchain.h
│   ├── VulkanPipeline.h
│   ├── VulkanBuffer.h
│   ├── VulkanImage.h
│   ├── VulkanSampler.h
│   ├── VulkanDescriptorPool.h
│   ├── VulkanDescriptorSet.h
│   ├── VulkanPerFrame.h
│   └── VulkanRenderingInfo.h
└── src/Render/VulkanBase/
    ├── VulkanInstance.cpp
    ├── VulkanDevice.cpp
    ├── VulkanSwapchain.cpp
    ├── VulkanPipeline.cpp
    ├── VulkanBuffer.cpp
    ├── VulkanImage.cpp
    ├── VulkanSampler.cpp
    ├── VulkanDescriptorPool.cpp
    ├── VulkanDescriptorSet.cpp
    ├── VulkanPerFrame.cpp
    └── VmaImpl.cpp
```

### 现状评价

这一层做得很扎实：薄封装、不泄露业务逻辑、合理使用 RAII。**建议保持，只做少量补充。**

### 需要补充的内容

```cpp
// ① VulkanPipeline 支持多 DescriptorSetLayout
// 当前只能绑定一个 layout，应改为支持数组
struct VulkanPipelineCreateInfo {
    std::vector<vk::DescriptorSetLayout> descriptorSetLayouts;
    // ...
};

// ② 添加 VulkanComputePipeline（为 GPU culling 做准备）
class VulkanComputePipeline {
    vk::Pipeline m_Pipeline;
    vk::PipelineLayout m_Layout;
    void Init(vk::Device device, const std::vector<uint32_t> &spirv,
              vk::DescriptorSetLayout layout);
    void Bind(vk::CommandBuffer cmd);
};

// ③ VulkanBuffer 添加非 MAPPED 支持
// 目前只有 HOST_VISIBLE 路径
// 需要支持 DEVICE_LOCAL +  staging buffer 上传
enum class BufferType {
    HostVisible,  // 现有逻辑
    DeviceLocal   // 新增：纯显存，通过 staging 上传
};
```

### 不应放在 Layer 1 的逻辑

- ❌ 管线缓存（属于 Layer 3）
- ❌ 材质系统（属于 Layer 3）
- ❌ 着色器加载/反射（属于 Layer 3 的 ShaderLibrary）
- ❌ UBO 数据上传时机（属于 Layer 3）

---

## 4. Layer 2: RenderCommand + RendererAPI

### 4.1 设计目标

- 将 Vulkan 调用封装在抽象接口之后
- 允许未来添加 OpenGL / Metal 后端
- 支持 command buffer 录制优化（sorting、合并）

### 4.2 RenderCommand

```cpp
// GE/include/GE/Render/RenderCommand.h

enum class CommandType : uint8_t {
    BindPipeline,
    SetUniform,
    BindVertexBuffer,
    BindIndexBuffer,
    BindDescriptorSet,
    SetPushConstant,
    Draw,
    DrawIndexed,
    DrawInstanced,
    SetViewport,
    SetScissor,
};

struct RenderCommand {
    CommandType type;
    union {
        struct { PipelineHandle pipeline; } bindPipeline;
        struct { uint32_t slot; uint32_t size; uint32_t offset; } setUniform;
        struct { BufferHandle buffer; uint32_t binding; } bindVertexBuffer;
        struct { BufferHandle buffer; IndexType type; } bindIndexBuffer;
        struct { DescriptorSetHandle set; uint32_t setIndex; } bindDescriptorSet;
        struct { uint32_t offset; uint32_t size; void *data; } pushConstant;
        struct { uint32_t vertexCount; uint32_t instanceCount; } draw;
        struct { uint32_t indexCount; uint32_t instanceCount;
                 uint32_t firstIndex; int32_t vertexOffset; } drawIndexed;
    };
};
```

### 4.3 RendererAPI

```cpp
// GE/include/GE/Render/RendererAPI.h

class RendererAPI {
public:
    virtual ~RendererAPI() = default;

    // === 生命周期 ===
    virtual void Init(const Window &window) = 0;
    virtual void Shutdown() = 0;

    // === 帧生命周期 ===
    virtual void BeginFrame() = 0;
    virtual void EndFrame() = 0;

    // === Command buffer 录制 ===
    virtual void BeginRenderPass(RenderPassInfo &info) = 0;
    virtual void EndRenderPass() = 0;
    virtual void ExecuteCommands(const RenderCommand *cmds, uint32_t count) = 0;
    virtual void Flush() = 0;

    // === 资源工厂 ===
    virtual Ref<VertexBuffer> CreateVertexBuffer(
        const void *data, size_t size, BufferUsage usage) = 0;
    virtual Ref<IndexBuffer> CreateIndexBuffer(
        const void *data, size_t size, IndexType type) = 0;
    virtual Ref<Pipeline> CreatePipeline(const PipelineSpec &spec) = 0;
    virtual Ref<Texture2D> CreateTexture2D(const TextureSpec &spec) = 0;
    virtual Ref<UniformBufferSet> CreateUniformBufferSet(
        size_t size, uint32_t framesInFlight) = 0;

    // === 查询 ===
    virtual RendererCapabilities &GetCapabilities() = 0;
};
```

### 4.4 Vulkan 后端实现

```cpp
// GE/src/Render/Backends/VulkanRendererAPI.cpp

class VulkanRendererAPI : public RendererAPI {
    VulkanInstance m_Instance;
    VulkanDevice m_Device;
    VulkanSwapchain m_Swapchain;

    void ExecuteCommands(const RenderCommand *cmds, uint32_t count) override {
        // 遍历 command 数组，翻译为 Vulkan 调用
        for (uint32_t i = 0; i < count; i++) {
            switch (cmds[i].type) {
                case CommandType::DrawIndexed:
                    m_CurrentCmdBuffer.drawIndexed(
                        cmds[i].drawIndexed.indexCount,
                        cmds[i].drawIndexed.instanceCount,
                        cmds[i].drawIndexed.firstIndex,
                        cmds[i].drawIndexed.vertexOffset,
                        0
                    );
                    break;
                // ...
            }
        }
    }
};
```

### 4.5 Command Sorting（可选优化）

在 `ExecuteCommands` 之前，可以对 command buffer 按 pipeline handle 排序，减少 state change：

```cpp
void VulkanRendererAPI::Flush() {
    // 按 (pipeline, descriptorSet, buffer) 排序
    std::sort(m_PendingCommands.begin(), m_PendingCommands.end(),
        [](const RenderCommand &a, const RenderCommand &b) {
            return a.sortKey < b.sortKey;
        });
    ExecuteCommands(m_PendingCommands.data(), m_PendingCommands.size());
    m_PendingCommands.clear();
}
```

> **注意**：排序会改变 draw call 顺序，只适用于不透明物体。
> 透明物体需要按从远到近排序，不能混用这个逻辑。

---

## 5. Layer 3: Renderer2D 重构

### 5.1 当前问题

`Renderer2D` 目前混合了三种职责：

| 职责 | 目前位置 | 目标位置 |
|---|---|---|
| Frame lifecycle | `Renderer2D` | `Renderer2D`（保留） |
| 直接写 command buffer | `Renderer2D::Draw` | 下放给 `RendererAPI` |
| 资源管理（某个 buffer） | `Renderer2D` | 拆分到 `RenderResource` |

### 5.2 目标结构

```
Renderer2D（用户看到的 API）
  ├─ SceneRenderer     ← 封装一帧的渲染流程
  │    ├─ 管理 RenderGraph 节点
  │    └─ 管理 per-frame 资源池
  ├─ MaterialSystem    ← 材质和着色器管理
  │    ├─ PipelineCache
  │    ├─ ShaderLibrary
  │    └─ DescriptorSetManager
  └─ RenderResource    ← 资源缓存
       ├─ MeshPool
       ├─ TexturePool
       └─ BufferPool（环形 buffer，替代单 UBO）
```

### 5.3 核心改进：Per-Frame Ring Buffer

#### 问题

当前每帧只能画一个物体：

```cpp
void Renderer2D::Draw(...) {
    memcpy(m_UniformBuffer.m_MappedData, &ubo, sizeof(ubo)); // ← 每次覆盖！
    // 如果 Draw 了 3 个物体，只有最后一个能画对
}
```

#### 解决方案

```cpp
struct PerFrameData {
    VulkanBuffer ringBuffer;  // 64KB ~ 几 MB
    uint32_t currentOffset = 0;
    void *mappedData = nullptr;

    void Init(VmaAllocator allocator, vk::DeviceSize totalSize) {
        ringBuffer.Init(allocator, totalSize,
            vk::BufferUsageFlagBits::eUniformBuffer |
            vk::BufferUsageFlagBits::eTransferDst,
            VMA_MEMORY_USAGE_AUTO,
            VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT |
            VMA_ALLOCATION_CREATE_MAPPED_BIT);
        mappedData = ringBuffer.GetMappedData();
    }

    void *Allocate(size_t size, size_t alignment = 256) {
        // 对齐到 256 字节（minUniformBufferOffsetAlignment）
        size_t alignedOffset = (currentOffset + alignment - 1) & ~(alignment - 1);
        GE_CORE_ASSERT(alignedOffset + size <= ringBuffer.GetSize(),
                       "Ring buffer out of memory");

        currentOffset = alignedOffset + size;
        return static_cast<char *>(mappedData) + alignedOffset;
    }

    void Reset() { currentOffset = 0; }
};

class Renderer2D {
    // 每个 swapchain image 一个 PerFrameData
    std::vector<PerFrameData> m_FrameData;

    void Draw(...) {
        PerFrameData &frame = m_FrameData[m_CurrentFrameIndex];
        void *dst = frame.Allocate(sizeof(UBOData));
        memcpy(dst, &ubo, sizeof(UBOData));

        // 使用 VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC
        // offset 通过 dynamic offset 传入
        cmd.bindDescriptorSets(
            ..., 1, &set,
            1, &frame.currentOffset - sizeof(UBOData) // dynamic offset
        );
    }
};
```

#### 内存需求估算

```
假设：一帧最多 10000 个 draw call
每个 UBO 大小：128 bytes（对齐到 256）
每帧所需：10000 × 256 = 2.56 MB

每个 PerFrameData 分配 4 MB 绰绰有余
3 个 swapchain image → 总共 12 MB
```

### 5.4 PipelineCache

```cpp
// GE/include/GE/Render/MaterialSystem.h

struct PipelineKey {
    std::string vertexShader;
    std::string fragmentShader;
    VertexLayout vertexLayout;
    BlendState blendState;
    DepthState depthState;
    uint32_t renderPassHash;

    bool operator==(const PipelineKey &other) const {
        return vertexShader == other.vertexShader
            && fragmentShader == other.fragmentShader
            && vertexLayout == other.vertexLayout
            && blendState == other.blendState
            && depthState == other.depthState
            && renderPassHash == other.renderPassHash;
    }
};

struct PipelineKeyHash {
    size_t operator()(const PipelineKey &key) const {
        size_t h = 0;
        hash_combine(h, key.vertexShader);
        hash_combine(h, key.fragmentShader);
        // ...
        return h;
    }
};

class PipelineCache {
    std::unordered_map<PipelineKey, Ref<Pipeline>, PipelineKeyHash> m_Cache;

    Ref<Pipeline> GetOrCreate(const PipelineSpec &spec) {
        PipelineKey key = MakeKey(spec);
        auto it = m_Cache.find(key);
        if (it != m_Cache.end())
            return it->second;

        auto pipeline = Pipeline::Create(spec);
        m_Cache[key] = pipeline;
        return pipeline;
    }
};
```

### 5.5 ShaderLibrary

```cpp
// GE/include/GE/Render/ShaderLibrary.h

class ShaderLibrary {
    std::unordered_map<std::string, Ref<Shader>> m_Shaders;

    // 从 .spv 文件加载
    Ref<Shader> Load(const std::string &name,
                     const std::string &vertexPath,
                     const std::string &fragmentPath);

    // 从源码字符串加载（将来支持 GLSL 编译）
    Ref<Shader> LoadFromSource(const std::string &name,
                               const std::string &vertexSource,
                               const std::string &fragmentSource);

    Ref<Shader> Get(const std::string &name);
    bool Exists(const std::string &name) const;
};
```

### 5.6 Material 重构

当前 Material 写死了管线 layout 和参数绑定。应该拆分为：

```cpp
// GE/include/GE/Render/Material.h

class Material {
public:
    void SetShader(Ref<Shader> shader);
    void SetTexture(const std::string &name, Ref<Texture2D> texture);
    void SetFloat(const std::string &name, float value);
    void SetVec3(const std::string &name, const glm::vec3 &value);
    void SetVec4(const std::string &name, const glm::vec4 &value);
    void SetMat4(const std::string &name, const glm::mat4 &value);

    void Bind(RenderCommandList &cmdList);
    void Unbind();

private:
    Ref<Shader> m_Shader;
    Ref<Pipeline> m_Pipeline;       // 来自 PipelineCache
    DescriptorSetHandle m_DescriptorSet;

    // 参数暂存区，Bind 时上传
    std::unordered_map<std::string, MaterialParam> m_Params;
};
```

---

## 6. Layer 4: RenderGraph（远期）

### 6.1 什么时候需要

当场景出现多种渲染 pass 时：

- 阴影贴图（shadow mapping）
- G-buffer 渲染（deferred shading）
- 光照计算
- 后处理（bloom、tone mapping、SSAO）

### 6.2 设计思路

```cpp
// GE/include/GE/Render/RenderGraph.h

struct AttachmentDesc {
    std::string name;
    TextureFormat format;
    glm::vec4 clearColor;
    LoadOp loadOp;
    StoreOp storeOp;
};

struct RenderPassNode {
    std::string name;
    std::vector<AttachmentDesc> colorAttachments;
    AttachmentDesc depthAttachment;
    std::vector<std::string> inputs;   // 来自其他 pass 的输出
    std::vector<std::string> outputs;  // 供其他 pass 使用
};

class RenderGraph {
public:
    void AddPass(const RenderPassNode &pass);
    void Build();                     // 拓扑排序，自动推导 barrier
    void Execute(RenderCommandList &cmdList);

private:
    std::vector<RenderPassNode> m_Passes;
    std::unordered_map<std::string, Ref<Texture2D>> m_TransientTextures;

    void InsertBarriers(vk::CommandBuffer cmd,
                        const std::vector<ResourceTransition> &transitions);
};
```

### 6.3 示例：Shadow + Deferred + Lighting

```
RenderGraph:
  [1] ShadowMap
       ├─ 输出: depth_attachment -> shadow_map
       └─ 画: 所有投射阴影的物体

  [2] G-Buffer
       ├─ 输出: color_attachment[0] -> albedo_rt
       │         color_attachment[1] -> normal_rt
       │         color_attachment[2] -> metallic_roughness_rt
       └─ 输入: shadow_map（来自 Pass 1）

  [3] Lighting
       ├─ 输出: color_attachment -> hdr_scene
       └─ 输入: albedo_rt, normal_rt, metallic_roughness_rt（来自 Pass 2）

  [4] PostProcess (Bloom + ToneMapping)
       ├─ 输出: swapchain image
       └─ 输入: hdr_scene（来自 Pass 3）
```

> **这不是现在需要做的事情**。一次只解决一个问题。

---

## 7. 演进路线图

### Phase 1：立即可以做（优先级高）

| # | 改进 | 文件改动范围 | 预估工作量 |
|---|---|---|---|
| 1 | **Ring Buffer 替代单 UBO** | `Renderer2D`、`VulkanBuffer` | 2-3 天 |
| 2 | **提取 RendererAPI 抽象** | 新建 `RendererAPI.h` + `VulkanRendererAPI.cpp` | 3-5 天 |
| 3 | **PipelineCache** | 新建 `PipelineCache` | 1-2 天 |
| 4 | **ShaderLibrary** | 新建 `ShaderLibrary` + 整理现有 `.spv` | 2-3 天 |

#### Phase 1 后的架构

```
Renderer2D
  ├─ m_RendererAPI      (VulkanRendererAPI)
  ├─ m_PipelineCache    (已缓存管线)
  ├─ m_ShaderLibrary    (按 name 加载 shader)
  ├─ m_FrameData[N]     (每个 swapchain image 的 ring buffer)
  └─ BeginScene / Draw / EndScene
       ↳ 不直接调 Vulkan，改调 m_RendererAPI
```

### Phase 2：下一步（优先级中）

| # | 改进 | 原因 |
|---|---|---|
| 5 | Material 参数系统 | 支持不同着色器有不同的 uniform 参数 |
| 6 | Command sorting | 按管线排序 → 最小化 state change |
| 7 | Multi-threaded command recording | 充分利用多核 CPU |

### Phase 3：远期（优先级低）

| # | 改进 |
|---|---|
| 8 | RenderGraph 多 pass 调度 |
| 9 | GPU culling (mesh shader / indirect draw) |
| 10 | Vulkan → OpenGL 双后端 |

---

## 8. 附录：数据流对比

### 当前数据流

```
Application::Run()
  │
  ├─ swapchain.BeginFrame()
  │    ├─ acquire next image
  │    ├─ begin cmd buffer
  │    └─ transition undefined → color-attachment
  │
  ├─ VulkanLayer::OnUpdate()
  │    ├─ Renderer2D::BeginScene(camera, clear)
  │    │    └─ cmd.beginRendering(...)
  │    │
  │    ├─ Renderer2D::Draw(mesh, material, model, color)
  │    │    ├─ memcpy(UBO)          ← 覆写，只能画一个
  │    │    ├─ bindPipeline
  │    │    ├─ bindVertex/IndexBuffer
  │    │    ├─ bindDescriptorSet
  │    │    └─ cmd.drawIndexed
  │    │
  │    └─ Renderer2D::EndScene()
  │         └─ cmd.endRendering()
  │
  ├─ ImGuiLayer::End()
  └─ swapchain.EndFrame()
```

### Phase 1 后的数据流

```
Application::Run()
  │
  ├─ m_RendererAPI->BeginFrame()
  │
  ├─ VulkanLayer::OnUpdate()
  │    ├─ Renderer2D::BeginScene(camera, clear)
  │    │    └─ m_RendererAPI->BeginRenderPass(...)
  │    │
  │    ├─ for each (mesh, material, model, color):
  │    │    ├─ Renderer2D::Draw(mesh, material, model, color)
  │    │    │    ├─ frame.Allocate(UBO_size)  ← ring buffer 推进
  │    │    │    ├─ memcpy(allocated_ptr)      ← 不覆写已有数据
  │    │    │    ├─ m_RendererAPI->DrawIndexed(...)
  │    │    │    └─ ... (绑定的管线来自 PipelineCache)
  │    │    └─ ...
  │    │
  │    └─ Renderer2D::EndScene()
  │         └─ m_RendererAPI->EndRenderPass()
  │
  ├─ ImGuiLayer::End()
  └─ m_RendererAPI->EndFrame()
```

### 关键区别

| 项目 | 当前 | Phase 1 后 |
|---|---|---|
| Draw call 数 | 每帧 1 个 | 每帧任意数量 |
| UBO 管理 | 单个 buffer 覆写 | ring buffer 推进 |
| API 调用 | 直接 Vulkan | 通过 RendererAPI 抽象 |
| Pipeline | 每次手动创建 | 按 key 缓存复用 |
| Shader 加载 | 硬编码路径 | ShaderLibrary 管理 |

---

> **文档版本**: v1.0
> **适用引擎阶段**: 从单物体渲染 → 多物体渲染的过渡期
