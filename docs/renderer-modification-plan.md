# Vulkan 渲染器修改方案

> 基于架构问题分析文档的具体修改方案。每个修改项包含：目标文件、修改内容、风险等级。
> 按优先级分组，建议按顺序执行。

---

## 第一优先级：性能杀手（改动量小，收益最大）

### 1.1 消除 `device.waitIdle()` — 改用 per-frame fence

**目标**：`GE/src/Render/VulkanBase/VulkanSwapchain.cpp`

**问题**：`CheckResize()` 中使用 `device.waitIdle()` 全设备停顿，破坏 CPU-GPU 帧重叠。

**修改内容**：

```cpp
// 修改前
void VulkanSwapchain::CheckResize() {
    auto surface_properties = m_Gpu.getSurfaceCapabilitiesKHR(m_Surface);
    if (surface_properties.currentExtent.width == m_Dimensions.width &&
        surface_properties.currentExtent.height == m_Dimensions.height) {
        return;
    }
    m_Device.waitIdle();  // ← 全设备停顿
    // ... 销毁重建 ...
}

// 修改后
void VulkanSwapchain::CheckResize() {
    // 查询 surface capabilities（仅在 resize 时调用）
    auto surface_properties = m_Gpu.getSurfaceCapabilitiesKHR(m_Surface);
    if (surface_properties.currentExtent.width == m_Dimensions.width &&
        surface_properties.currentExtent.height == m_Dimensions.height) {
        return;
    }

    // 改为等待所有 in-flight per-frame fence（只等正在用的帧，不等全设备）
    for (auto &pf : m_PerFrame) {
        if (pf.GetSubmitFence()) {
            m_Device.waitForFences(pf.GetSubmitFence(), true, UINT64_MAX);
            m_Device.resetFences(pf.GetSubmitFence());
        }
    }
    // ... 销毁重建 ...
}
```

另外，移除 `AcquireNextImage()` 中的每帧 `CheckResize()` 调用：

```cpp
// 修改前
vk::Result VulkanSwapchain::AcquireNextImage(uint32_t *image) {
    CheckResize();  // ← 每帧调用
    // ...
}

// 修改后
vk::Result VulkanSwapchain::AcquireNextImage(uint32_t *image) {
    if (m_NeedsResize) {
        RecreateSwapchain();
        m_NeedsResize = false;
    }
    // ...
}
```

在 `Present()` 中检测 `eErrorOutOfDateKHR` 时设标志：

```cpp
vk::Result VulkanSwapchain::Present(uint32_t index) {
    // ...
    try {
        result = m_Queue.presentKHR(present);
    } catch (vk::OutOfDateKHRError &) {
        m_NeedsResize = true;  // ← 改为设标志
        result = vk::Result::eErrorOutOfDateKHR;
    }
    return result;
}
```

**风险**：中。需要确保 `RecreateSwapchain` 中的所有正确性路径。

---

### 1.2 Ring Buffer 对齐从硬编码改为动态查询

**目标**：
- `GE/src/Render/VulkanBase/VulkanRingBuffer.cpp`
- `GE/include/GE/Render/VulkanBase/VulkanRingBuffer.h`

**问题**：硬编码 256 字节对齐导致 44% 容量浪费，UniformData 实际只用了 144 字节。

**修改内容**：

```cpp
// RingBuffer.h
class VulkanRingBuffer {
public:
    // 新增：Init 接受对齐参数
    void Init(VmaAllocator allocator, vk::DeviceSize totalSize,
              vk::DeviceSize alignment = 256);

    vk::DeviceSize GetAllocateAlignment() const { return m_Alignment; }

private:
    vk::DeviceSize m_Alignment = 256;  // 新增成员
};

// RingBuffer.cpp
void VulkanRingBuffer::Init(VmaAllocator allocator, vk::DeviceSize totalSize,
                            vk::DeviceSize alignment) {
    m_Alignment = alignment;
    m_TotalSize = totalSize;
    m_CurrentOffset = 0;
    // ... 其余不变 ...
}

// Allocate 中使用 m_Alignment 替代硬编码
vk::DeviceSize VulkanRingBuffer::Allocate(vk::DeviceSize size, vk::DeviceSize alignment) {
    vk::DeviceSize align = alignment ? alignment : m_Alignment;  // 参数可覆盖默认
    vk::DeviceSize alignedOffset = (m_CurrentOffset + align - 1) & ~(align - 1);
    // ...
}
```

**在 `Renderer2D::Init()` 中查询设备属性并传入**：

```cpp
void Renderer2D::Init(Window &window) {
    // ...
    vk::PhysicalDeviceProperties props = m_Device.GetGpu().getProperties();
    vk::DeviceSize alignment = props.limits.minUniformBufferOffsetAlignment;
    // 安全回退
    if (alignment == 0) alignment = 64;

    m_RingBuffers.reserve(imageCount);
    for (uint32_t i = 0; i < imageCount; i++) {
        m_RingBuffers.emplace_back();
        m_RingBuffers.back().Init(m_Device.GetVmaAllocator(), RING_BUFFER_SIZE, alignment);
    }
}
```

**风险**：低。纯数值变更，不改变逻辑。

---

### 1.3 VulkanRenderingInfo 改用固定数组

**目标**：`GE/include/GE/Render/VulkanBase/VulkanRenderingInfo.h`

**问题**：`std::vector` 每帧 `push_back` 触发堆分配。

**修改内容**：

```cpp
class VulkanRenderingInfo {
public:
    static constexpr uint32_t MAX_COLOR_ATTACHMENTS = 4;

    void AddColorAttachment(vk::ImageView imageView,
                            vk::AttachmentLoadOp loadOp,
                            vk::AttachmentStoreOp storeOp,
                            vk::ClearValue clearValue = {},
                            vk::ImageLayout layout = vk::ImageLayout::eColorAttachmentOptimal) {
        GE_CORE_ASSERT(m_ColorAttachmentCount < MAX_COLOR_ATTACHMENTS,
                       "Exceeded max color attachments");
        m_ColorAttachments[m_ColorAttachmentCount] = vk::RenderingAttachmentInfo{
            .imageView = imageView,
            .imageLayout = layout,
            .loadOp = loadOp,
            .storeOp = storeOp,
            .clearValue = clearValue,
        };
        m_ColorAttachmentCount++;
    }

    void Begin(vk::CommandBuffer cmd) const {
        vk::RenderingInfo info{
            .renderArea = m_RenderArea,
            .layerCount = m_LayerCount,
            .colorAttachmentCount = m_ColorAttachmentCount,
            .pColorAttachments = m_ColorAttachments.data(),
        };
        cmd.beginRendering(info);
    }

    void Reset() { m_ColorAttachmentCount = 0; }  // 新增：每帧开始时调用

private:
    vk::Rect2D m_RenderArea{{0, 0}, {0, 0}};
    uint32_t m_LayerCount = 1;
    std::array<vk::RenderingAttachmentInfo, MAX_COLOR_ATTACHMENTS> m_ColorAttachments;
    uint32_t m_ColorAttachmentCount = 0;
};
```

**风险**：低。纯数据结构替换，接口不变。

---

### 1.4 Wait Stage 从 eTopOfPipe 改为 eColorAttachmentOutput

**目标**：`GE/src/Render/VulkanBase/VulkanSwapchain.cpp` 第 143 行

**问题**：`eTopOfPipe` 是 GPU pipeline 最早期阶段，GPU 必须等待 acquire semaphore 才能开始任何工作。

**修改内容**：

```cpp
// 修改前
vk::PipelineStageFlags wait_stage = {vk::PipelineStageFlagBits::eTopOfPipe};

// 修改后
vk::PipelineStageFlags wait_stage = {vk::PipelineStageFlagBits::eColorAttachmentOutput};
```

这样 GPU 可以在等待 acquire semaphore 的同时，提前完成 vertex shader、tessellation、geometry shader 等前期工作。

**风险**：低。标准做法。

---

## 第二优先级：代码健壮性（消除未定义行为内存泄漏）

### 2.1 GlfwWindow Rule of Five

**目标**：`GE/include/GE/Platform/Windows/GlfwWindow.h`

**问题**：管理 `GLFWwindow*` 原始指针，但使用默认拷贝语义——浅拷贝会导致同一个窗口指针被 `glfwDestroyWindow` 两次。

**修改内容**：

```cpp
class GlfwWindow : public Window {
public:
    GlfwWindow(const WindowProps &props);
    ~GlfwWindow() override;

    // 禁止拷贝
    GlfwWindow(const GlfwWindow &) = delete;
    GlfwWindow &operator=(const GlfwWindow &) = delete;

    // 启用移动
    GlfwWindow(GlfwWindow &&other) noexcept
        : m_Window(std::exchange(other.m_Window, nullptr)) {}

    GlfwWindow &operator=(GlfwWindow &&other) noexcept {
        if (this != &other) {
            if (m_Window) glfwDestroyWindow(m_Window);
            m_Window = std::exchange(other.m_Window, nullptr);
        }
        return *this;
    }

private:
    GLFWwindow *m_Window = nullptr;
};
```

或在类前加 `GE_NO_COPY` 宏，若项目中有此类宏。

**风险**：低。

---

### 2.2 Base.h 移除全局 `<Windows.h>` 包含

**目标**：`GE/include/GE/Core/Base.h`

**问题**：在基础头文件中全局包含 `<Windows.h>`，导致每个源文件都引入数千个 Win32 宏（`min`/`max`/`ERROR` 等），严重污染命名空间。

**修改内容**：

```cpp
// Base.h 中移除：
// #ifdef GE_PLATFORM_WINDOWS
//     #include <Windows.h>
// #endif

// 如果确实需要 Windows 类型，改用前向声明：
// struct HWND__;  // 前向声明 HWND（如果需要）
// using HWND = HWND__*;
```

然后逐个检查哪些 .cpp 文件编译失败，在这些 .cpp 文件中各自添加：

```cpp
#ifdef GE_PLATFORM_WINDOWS
    #define WIN32_LEAN_AND_MEAN
    #include <Windows.h>
#endif
```

**风险**：中。需要逐个 .cpp 排查 Windows API 调用，但一次做完后续不再受污染。

---

### 2.3 Application 析构顺序加固

**目标**：`GE/src/Core/Application.cpp`

**问题**：当前析构顺序是 `waitIdle → Clear() → Shutdown()`，但 `Shutdown()` 后如果 Layer 的 `Ref` 还在使用则可能访问已销毁资源。

**修改内容**：

```cpp
Application::~Application() {
    GE_PROFILE_FUNCTION();
    GE_CORE_INFO("Application Shoutdown");

    // 1. 先 Detach 所有层（层中可能持有渲染资源的引用）
    m_LayerStack.Clear();

    // 2. 清除 ImGuiLayer 引用（确保在 Shutdown 前释放）
    m_ImGuiLayer.reset();

    // 3. 等待 GPU 完成
    Renderer2D::Get().GetVkDevice().waitIdle();

    // 4. 关闭渲染器
    Renderer2D::Get().Shutdown();

    // 重置单例指针
    s_Instance = nullptr;
}
```

**风险**：低。仅调整调用顺序。

---

### 2.4 添加 null 检查

**目标**：
- `GE/src/Render/Renderer2D.cpp`（`Draw` 中 `GetMappedData()`）
- `GE/src/Render/VulkanBase/VulkanRingBuffer.cpp`（`Allocate` 结果）

**修改内容**：

```cpp
// Renderer2D::Draw() 中
void Renderer2D::Draw(...) {
    // ...
    void *mapped = ringBuffer.GetMappedData();
    if (!mapped) {
        GE_CORE_ERROR("RingBuffer mapped data is null");
        return;
    }
    std::memcpy(static_cast<char *>(mapped) + offset, &data, sizeof(data));
    // ...
}
```

```cpp
// RingBuffer::Allocate 溢出时，Release 模式也应有保护
vk::DeviceSize VulkanRingBuffer::Allocate(vk::DeviceSize size, vk::DeviceSize alignment) {
    vk::DeviceSize align = alignment ? alignment : m_Alignment;
    vk::DeviceSize alignedOffset = (m_CurrentOffset + align - 1) & ~(align - 1);

    if (alignedOffset + size > m_TotalSize) {
        GE_CORE_ERROR("VulkanRingBuffer out of memory: {0} + {1} > {2}",
                      alignedOffset, size, m_TotalSize);
        return 0;  // 返回 0 让调用方处理
    }

    m_CurrentOffset = alignedOffset + size;
    return alignedOffset;
}
```

**风险**：低。

---

## 第三优先级：编译期优化（减少依赖膨胀）

### 3.1 Application.h 移除不必要的 Renderer2D.h

**目标**：`GE/include/GE/Core/Application.h` + `GE/src/Core/Application.cpp`

**问题**：`Application.h` 包含了 `Render/Renderer2D.h`，导致任何包含 Application.h 的文件都拉入完整的 Vulkan 类型系统。

**修改内容**：

```cpp
// Application.h 中移除：
// #include "Render/Renderer2D.h"

// 改为前向声明：
namespace GE {
    class Renderer2D;  // 前向声明
}

// 在 Application.cpp 中保留原有 #include
```

**风险**：低。标准做法。

### 3.2 GlfwWindow.h 清理头文件泄漏

**目标**：`GE/include/GE/Platform/Windows/GlfwWindow.h`

**问题**：头文件中 `#define GLFW_EXPOSE_NATIVE_WIN32` + `#include <GLFW/glfw3native.h>` 导致所有包含者都引入 Win32 API。

**修改内容**：

```cpp
// GlfwWindow.h 中移除：
// #define GLFW_EXPOSE_NATIVE_WIN32
// #include <GLFW/glfw3native.h>

// 同时移除多余的 forward declaration：
// struct GLFWwindow {};

// 将上述内容移到 GlfwWindow.cpp 中
```

**风险**：低。

---

## 第四优先级：架构改进（需要更多设计）

### 4.1 新增 TextureCache 纹理缓存

**目标**：新建 `GE/include/GE/Render/TextureCache.h` + `GE/src/Render/TextureCache.cpp`

**设计**：

```cpp
class TextureCache {
public:
    // 按文件路径加载纹理，若已缓存则直接返回
    VulkanImage *Load(const std::string &filepath,
                      VmaAllocator allocator, vk::Queue queue, uint32_t qfi);

    // 移除引用计数归零的纹理
    void GC();

    // 清空所有纹理
    void Clear();

private:
    struct TextureEntry {
        VulkanImage image;
        uint32_t refCount = 0;
    };
    std::unordered_map<std::string, std::unique_ptr<TextureEntry>> m_Cache;
};
```

### 4.2 新增 ShaderLibrary 着色器库

**目标**：新建 `GE/include/GE/Render/ShaderLibrary.h` + `GE/src/Render/ShaderLibrary.cpp`

**设计**：

```cpp
class ShaderLibrary {
public:
    // 按名称注册着色器
    void Add(const std::string &name, VulkanPipeline &&pipeline);

    // 按名称获取管线（若不存在则从文件加载）
    VulkanPipeline *Get(const std::string &name);

    // 热重载：重新编译所有着色器
    void ReloadAll();

private:
    std::unordered_map<std::string, VulkanPipeline> m_Shaders;
};
```

### 4.3 添加 Depth/Stencil 支持

**目标**：`Renderer2D::BeginScene` 中创建 depth attachment，Swapchain 中管理 depth image

**修改**：

```cpp
// Renderer2D 新增 depth 资源
VulkanImage m_DepthImage;

// BeginScene 中添加 depth attachment
VulkanRenderingInfo render_info;
render_info.SetRenderArea(0, 0, dim.width, dim.height);
render_info.AddColorAttachment(...);
render_info.SetDepthAttachment(m_DepthImageView);  // 新增 depth 支持
```

需要在 `VulkanRenderingInfo` 中新增 depth attachment 字段：

```cpp
std::optional<vk::RenderingAttachmentInfo> m_DepthAttachment;

void SetDepthAttachment(vk::ImageView imageView, ...) {
    m_DepthAttachment = vk::RenderingAttachmentInfo{...};
}

void Begin(vk::CommandBuffer cmd) const {
    vk::RenderingInfo info{
        .renderArea = m_RenderArea,
        .layerCount = m_LayerCount,
        .colorAttachmentCount = m_ColorAttachmentCount,
        .pColorAttachments = m_ColorAttachments.data(),
        .pDepthAttachment = m_DepthAttachment ? &(*m_DepthAttachment) : nullptr,
    };
}
```

---

## 第五优先级：清理项

### 5.1 CMake 修复 Scene 源文件遗漏

**目标**：`CMakeLists.txt` 第 43-53 行

**修改**：

```cmake
file(GLOB GE_SRC
        ${CMAKE_SOURCE_DIR}/GE/src/*.cpp
        ${CMAKE_SOURCE_DIR}/GE/src/Platform/Windows/*.cpp
        ${CMAKE_SOURCE_DIR}/GE/src/Core/*.cpp
        ${CMAKE_SOURCE_DIR}/GE/src/FileSystem/*.cpp
        ${CMAKE_SOURCE_DIR}/GE/src/ImGui/*.cpp
        ${CMAKE_SOURCE_DIR}/GE/src/Render/*.cpp
        ${CMAKE_SOURCE_DIR}/GE/src/Render/VulkanBase/*.cpp
        ${CMAKE_SOURCE_DIR}/GE/src/Scene/*.cpp          # ← 新增
        ${CMAKE_SOURCE_DIR}/GE/third_party/stb/stb_image.cpp
)
```

同时移除未使用的 `BGFX_DIR` 和 `BGFX_BUILD_DIR` 变量：

```cmake
# 移除第 55 行和 81 行：
# set(BGFX_DIR "F:/yxy/c++lib/bgfx")
# set(BGFX_BUILD_DIR "${BGFX_DIR}/.build/win64_vs2022/bin")
```

**风险**：低。

### 5.2 ImGuizmo 改为条件编译

**目标**：`CMakeLists.txt` 第 36 行

**修改**：

```cmake
# ImGuizmo 仅在 Editor 启用时编译
option(GE_BUILD_EDITOR "Build the editor application" OFF)
if (GE_BUILD_EDITOR)
    add_subdirectory(GE/third_party/ImGuizmo)
endif()
```

**风险**：低。

### 5.3 输出目录区分配置

**目标**：`CMakeLists.txt` 第 16-18 行

**修改**：

```cmake
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_SOURCE_DIR}/bin/$<CONFIG>)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_SOURCE_DIR}/bin/$<CONFIG>)
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_SOURCE_DIR}/bin/$<CONFIG>)
```

**风险**：低。

---

## 执行顺序汇总

```
Round 1 — 性能修复（2-4 小时）
├── 1.1 消除 waitIdle
├── 1.2 Ring buffer 对齐
├── 1.3 RenderingInfo 固定数组
└── 1.4 Wait stage 优化

Round 2 — 健壮性（2-3 小时）
├── 2.1 GlfwWindow Rule of Five
├── 2.2 Base.h 清理 Windows.h
├── 2.3 析构顺序加固
└── 2.4 null 检查

Round 3 — 编译期（1-2 小时）
├── 3.1 Application.h 依赖裁剪
└── 3.2 GlfwWindow.h 泄漏清理

Round 4 — CMake 清理（1 小时）
├── 5.1 Scene 源文件 + 移除 bgfx
├── 5.2 ImGuizmo 条件编译
└── 5.3 输出目录区分配置

Round 5 — 新功能（按需排期）
├── 4.1 TextureCache
├── 4.2 ShaderLibrary
└── 4.3 Depth/Stencil
```
