# Vulkan 渲染器架构问题分析

> 基于对现有代码的全面审查，记录当前渲染架构的设计缺陷、性能瓶颈和改进方向。
> 撰写时间：2026/06/09

---

## 1. 渲染器是"大泥球"单例

### 问题

`Renderer2D` 承接了完全不相干的多种职责：

```
Renderer2D
├── 持有 VulkanInstance            // 全局对象，应独立
├── 持有 VulkanDevice              // 全局对象，应独立
├── 持有 VulkanSwapchain           // 窗口相关，应独立
├── 持有 RingBuffers[]             // 帧资源管理
├── BeginScene/EndScene            // 帧生命周期管理
├── Draw(Mesh, Material)           // 单个 Draw Call
├── CreateDefaultPipeline          // 管线工厂方法
└── 暴露 GetInstance/GetDevice/GetQueue/GetVkDevice/GetVkGpu...
```

**后果**：
- 没有层次隔离，想加延迟渲染管线无处下手
- 暴露了 7 个转发访问器，内部实现完全泄露到外部
- 单例模式导致测试困难、初始化顺序耦合

### 改进方向

```
RendererContext           ← 持有 Instance/Device
  └── Swapchain           ← 窗口/交换链管理
Renderer                  ← 抽象接口
  ├── ForwardRenderer     ← 前向渲染实现
  └── DeferredRenderer    ← 延迟渲染实现
    └── RenderCommandList ← Command Buffer 录制抽象
```

---

## 2. Frame Overlap 被完全破坏

### 问题

帧循环关键路径：

```cpp
Application::Run()
  → Swapchain::BeginFrame()
    → AcquireNextImage()
      → CheckResize()                          // 每帧调用
        → getSurfaceCapabilitiesKHR()           // WSI 内核调用
        → device.waitIdle()                     // ★ 全设备停顿！
  → 录制 CommandBuffer
  → Swapchain::EndFrame()
    → submit()
    → presentKHR()
```

**影响**：
- `device.waitIdle()` 强制 CPU 等待 GPU **所有队列** 完成
- GPU 在处理帧 N 时 CPU 在空等 → **完全没有帧重叠**
- 60fps 下浪费约 **16ms 的 CPU-GPU 并行潜力**
- `getSurfaceCapabilitiesKHR()` 每次进出内核，只有窗口 resize 时才需要

### 改进方向

- 改用 per-frame fence 等待（`WaitAndResetFence` 已经实现了，但 `CheckResize` 中仍然用了 `waitIdle`）
- resize 检测改为惰性标志：`Present()` 返回 `eErrorOutOfDateKHR` 时设 `m_NeedsResize`
- 仅在 resize 时查询 surface capabilities

---

## 3. 没有批处理系统（立即模式绘制）

### 问题

```cpp
void Renderer2D::Draw(const Mesh &mesh, const Material &material, ...) {
    material.pipeline.Bind(cmd);             // 每 Draw Call 绑管线
    cmd.setCullMode(...);                     // 每 Draw Call 设动态状态
    cmd.setFrontFace(...);
    cmd.bindVertexBuffers(...);
    cmd.bindIndexBuffer(...);
    cmd.bindDescriptorSets(...);
    cmd.drawIndexed(...);
}
```

每笔 Draw Call 完整设置所有管线状态，没有按材质排序和批处理。

**影响**：
- 1000 个物体、3 种材质 → 1000 次 pipeline bind（实际只需 3 次）
- 状态切换是 GPU 开销大户，管线切换尤其昂贵

### 改进方向

```
按材质排序 → BindPipeline → Draw(物体1), Draw(物体2), Draw(物体3)...
                       ↓ 换材质
                   BindPipeline → Draw(物体4), Draw(物体5)...
```

---

## 4. Material 设计过于刚性

### 问题

```cpp
struct Material {
    VulkanPipeline pipeline;                    // 一个管线
    VulkanDescriptorPool descriptorPool;
    std::vector<VulkanDescriptorSet> descriptorSets;  // 每个 swapchain image 一个
};
```

- Material 和 Pipeline 强绑定——换参数就得换管线
- Descriptor Set 布局固定：`binding 0 = UBO(dynamic)` + `binding 1 = combined image sampler`
- 无法表达 PBR 等复杂材质（需要 roughness/metalness/normal/ao 多张纹理）
- 没有材质参数缓存，颜色/粗糙度等参数只能在 UBO 中修改

**后果**：
- 扩展新材质类型必须改 `CreateDefaultPipeline` 和 `UniformData`
- DescriptorSet 数量 = Material 数量 × Swapchain Image 数量 → 平方级增长

### 改进方向

```
Material
├── ShaderHandle              ← 引用着色器
├── ParamBlock                ← 参数块（color, roughness, metalness...）
├── TextureSlots[]            ← 纹理槽（albedo, normal, mr...）
└── PipelineHandle            ← 引用管线（同类型共享）

Pipeline 由 Shader + VertexLayout + BlendState 组合决定
DescriptorSet 改为按需分配，不再预分配 N × imageCount 份
```

---

## 5. 没有资源管理系统

### 现状

| 资源类型 | 管理方式 | 问题 |
|----------|----------|------|
| 纹理 | `VulkanImage::LoadFromFile()` 直接调用 | 重复加载同文件创建多个 GPU Image |
| 着色器 | `.spv` 文件直接加载 | 硬编码路径，不支持热重载 |
| 网格 | 手动创建 `VulkanBuffer` | 无缓存、无引用计数 |
| 管线 | 每次创建，不保存 | 程序重启后重新编译 |

### 改进方向

- `TextureCache` —— 按路径去重，引用计数，异步加载
- `ShaderLibrary` —— 按名称管理，支持 #include 预处理和热重载
- `MeshCache` —— 模型文件解析后缓存顶点/索引数据
- `PipelineCache` —— 序列化/反序列化 `VkPipelineCache` 加速启动

---

## 6. Ring Buffer 对齐浪费

### 问题

```cpp
// VulkanRingBuffer 默认 alignment = 256
// UniformData 实际大小 = 144 字节
// 每笔分配浪费 = 256 - 144 = 112 字节
// 利用率仅 56%
```

4MB Ring Buffer 的理论容量：
- 256 对齐：4MB / 256 = **16384** 次 Draw
- 144 对齐：4MB / 144 = **29127** 次 Draw

**差 1.77 倍**。

### 改进方向

从 `VkPhysicalDeviceLimits::minUniformBufferOffsetAlignment` 动态查询对齐值：
- NVIDIA: 64 字节（浪费率 144→192 = 25%）
- AMD: 16 字节（浪费率 144→144 = 0%）
- 回退值设 64

---

## 7. VulkanRenderingInfo 每帧堆分配

### 问题

```cpp
class VulkanRenderingInfo {
    std::vector<vk::RenderingAttachmentInfo> m_ColorAttachments;  // 每帧 push_back 触发堆分配
};
```

`BeginScene()` 中创建 `VulkanRenderingInfo` 并调用 `AddColorAttachment()`，`push_back` 每次分配/释放内存。

**影响**：帧率抖动 + 堆碎片，虽然每次只分配几十字节，但在渲染热路径上累积不可忽视。

### 改进方向

```cpp
static constexpr uint32_t MAX_COLOR_ATTACHMENTS = 4;

std::array<vk::RenderingAttachmentInfo, MAX_COLOR_ATTACHMENTS> m_ColorAttachments;
uint32_t m_ColorAttachmentCount = 0;
```

---

## 8. Per-Frame 资源管理粗暴

### 问题

Swapchain resize 时整个 `m_PerFrame` 被销毁重建：

```cpp
void VulkanSwapchain::CheckResize() {
    m_Device.waitIdle();
    for (auto &pf : m_PerFrame)
        pf.Destroy(m_Device);     // 销毁 fence/command pool/semaphore
    m_PerFrame.clear();
    // ... resize ...
    m_PerFrame.resize(m_Images.size());  // 重新创建
    for (auto &per_frame : m_PerFrame)
        per_frame.Init(m_Device, ...);   // 重新分配
}
```

实际上 `vkResetCommandPool` 足够重置 command buffer，不需要销毁重建整个 per-frame 对象。

### 改进方向

- Per-frame 对象分离 command pool 与其他资源
- Resize 时只重建 swapchain image views，不重建 fence/pool/semaphore
- 仅当 image count 变化时才增减 per-frame 数组

---

## 9. 同步结构依赖隐式约定

### Semaphore 回收池

```cpp
vk::Semaphore acquire_semaphore;
if (m_RecycledSemaphores.empty())
    acquire_semaphore = m_Device.createSemaphore(...);
else {
    acquire_semaphore = m_RecycledSemaphores.back();
    m_RecycledSemaphores.pop_back();
}
```

池中 semaphore 何时可安全重用？——**依赖隐式约定**：只有 `WaitAndResetFence` 后才回收，但没有代码层面的强制保证。

### Wait Stage 过于保守

```cpp
vk::PipelineStageFlags wait_stage = {vk::PipelineStageFlagBits::eTopOfPipe};
```

`eTopOfPipe` 是最早的 pipeline stage，强制 GPU 在 acquire semaphore 信号到达前不做任何工作。改为 `eColorAttachmentOutput` 可以让 GPU 提前开始 vertex 处理。

---

## 10. Descriptor Set 分配膨胀

### 问题

每个 Material 创建 N 个 Descriptor Set（N = swapchain image count）：

```cpp
m_DescriptorSets.reserve(imageCount);
for (uint32_t i = 0; i < imageCount; i++) {
    m_DescriptorSets.push_back(VulkanDescriptorSet{});
    m_DescriptorSets.back().Init(device, pool, layout);
    m_DescriptorSets.back().WriteBuffer(0, uniformBufferInfos[i]);
    m_DescriptorSets.back().WriteImage(1, textureView, sampler);
}
```

如果有 10 个材质、3 个 swapchain images → 30 个 Descriptor Set。如果 Descriptor Pool 用 `VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT`，销毁时逐个 free 的开销也不小。

### 改进方向

- 全局统一管理 Descriptor Set Pool，按需分配
- 使用 Descriptor Update Template (`VK_EXT_descriptor_indexing`) 批量更新
- 或采用 Bindless Descriptor 方案（VK_EXT_descriptor_indexing 的 `PARTIALLY_BOUND`）

---

## 11. 缺失的关键架构组件

| 组件 | 必要性 | 说明 |
|------|--------|------|
| **渲染管线抽象** | 高 | 当前无法切换 Forward/Deferred 渲染模式 |
| **Command Buffer 录制抽象** | 高 | 裸用 `vk::CommandBuffer`，无自动 Barrier 管理 |
| **资源 Cache** | 高 | 纹理/着色器/网格无去重、无引用计数、无异步加载 |
| **Depth/Stencil 支持** | 高 | 当前完全没有深度测试，无法做正确 3D 渲染 |
| **场景渲染器** | 中 | 当前手动传 mesh/matrix，没有 ECS 场景遍历 |
| **后处理系统** | 中 | 需要可串联的 PostProcessStack |
| **多线程渲染** | 中 | 所有渲染工作在主线程，CPU 端有大量空闲 |
| **Pipeline Cache** | 低 | 每次启动重新创建所有管线 |

---

## 12. 着色器系统欠缺

### 问题

- 着色器路径硬编码：`"mesh.vert.spv"`、`"assets/shaders/glsl"`
- 不支持着色器热重载（修改后需重启程序）
- 不支持 `#include` 预处理
- 没有着色器反射（手动维护 binding 布局，与 shader 代码容易不一致）

### 改进方向

- `Shader` 类按名称管理，抽象 `.spv` 加载细节
- 文件监控 + 热重载回调
- 集成 SPIRV-Reflect 自动解析输入/输出/binding
- CMake 集成 glslc 编译所有 shader（当前已初步实现）

---

## 优先级排序

| 优先级 | 问题 | 影响 | 改动量 |
|--------|------|------|--------|
| **P0** | `waitIdle` 破坏 frame overlap | CPU-GPU 并行完全丧失 | 小 |
| **P0** | Renderer2D 职责膨胀 | 无法扩展架构 | 大（需重构） |
| **P1** | 无批处理 | GPU 状态切换过多 | 中 |
| **P1** | Ring Buffer 对齐浪费 | 44% 容量损失 | 小 |
| **P1** | Material 过度刚性 | 无法表达复杂材质 | 中 |
| **P1** | 无 Depth/Stencil | 无法做 3D 深度测试 | 中 |
| **P2** | RenderingInfo 每帧堆分配 | 帧率抖动 | 小 |
| **P2** | 纹理/着色器无 Cache | 重复加载、无热重载 | 中 |
| **P2** | Per-frame resize 粗暴 | 资源重建开销 | 小 |
| **P3** | Semaphore 回收隐式约定 | 潜在同步 Bug | 小 |
| **P3** | WaitStage 保守 | GPU pipeline 轻微停顿 | 小 |
| **P3** | Descriptor Set 膨胀 | 内存冗余 | 中 |

---

*本文档是对现有渲染架构的客观分析，记录了需要改进的设计问题。每项的改进方向作为参考，具体实现方案见 [renderer-roadmap.md](renderer-roadmap.md)。*
