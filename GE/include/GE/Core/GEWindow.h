#pragma once

#include "Base.h"
#include "Events/Event.h"

#include <optional>
#include <Vulkan/vulkan.h>


namespace GE {
class GraphicsContext;

// 窗口尺寸
struct Extent {
    uint32_t width;
    uint32_t height;
};

// 窗口模式
enum class WindowMode {
    Headless,
    Fullscreen,
    FullscreenBorderless,
    FullscreenStretch,
    Default
};

// 垂直同步模式（三态）
enum class VsyncMode {
    OFF,
    ON,
    Default
};

// 窗口完整属性
struct WindowProperties {
    std::string title = "Game Engine";
    WindowMode mode = WindowMode::Default;
    bool resizable = true;
    VsyncMode vsync = VsyncMode::Default;
    Extent extent = {1280, 720};

    explicit WindowProperties(std::string t = "Game Engine",
                              uint32_t w = 1280,
                              uint32_t h = 720)
        : title(std::move(t)), extent{w, h} {
    }
};

// 可选属性（用于部分更新）
struct OptionalWindowProperties {
    std::optional<std::string> title;
    std::optional<WindowMode> mode;
    std::optional<bool> resizable;
    std::optional<VsyncMode> vsync;
    std::optional<uint32_t> width;
    std::optional<uint32_t> height;
};

// 基于桌面系统的窗口接口
class Window {
public:
    using EventCallbackFn = std::function<void(Event &)>;

    virtual ~Window() = default;

    /// 每帧更新（处理事件、交换缓冲区等）
    virtual void OnUpdate() = 0;

    /// 处理所有底层窗口事件（可与 OnUpdate 分离调用）
    virtual void ProcessEvents() = 0;

    /// 检查窗口是否应关闭
    virtual bool ShouldClose() = 0;

    /// 请求关闭窗口
    virtual void Close() = 0;

    [[nodiscard]] virtual uint32_t GetWidth() const = 0;

    [[nodiscard]] virtual uint32_t GetHeight() const = 0;

    [[nodiscard]] Extent GetExtent() const { return {GetWidth(), GetHeight()}; }

    // 窗口属性
    virtual void SetEventCallback(const EventCallbackFn &callback) = 0;

    virtual void SetVSync(VsyncMode mode) = 0;

    [[nodiscard]] virtual VsyncMode GetVSync() const = 0;

    virtual void SetResizable(bool resizable) = 0;

    [[nodiscard]] virtual bool IsResizable() const = 0;

    [[nodiscard]] virtual WindowMode GetWindowMode() const = 0;

    /// 尝试调整窗口大小，返回实际尺寸
    virtual Extent Resize(const Extent &new_extent) = 0;

    [[nodiscard]] virtual void *GetNativeWindow() const = 0;

    [[nodiscard]] virtual void *GetGlfwWindow() const = 0;

    /// 创建 Vulkan 表面
    virtual VkSurfaceKHR CreateVulkanSurface(VkInstance instance) = 0;

    virtual VkSurfaceKHR CreateVulkanSurface(VkInstance instance, VkPhysicalDevice physical_device) = 0;

    /// 获取窗口所需的 Vulkan 表面扩展名列表
    [[nodiscard]] virtual std::vector<const char *> GetRequiredSurfaceExtensions() const = 0;

    /// DPI 缩放因子
    [[nodiscard]] virtual float GetDpiFactor() const = 0;

    /// 内容缩放因子（适用于异构窗口坐标与像素坐标的系统）
    [[nodiscard]] virtual float GetContentScaleFactor() const = 0;

    /// 获取显示呈现信息（用于全屏、显示设备等场景）
    virtual bool GetDisplayPresentInfo(VkDisplayPresentInfoKHR *info,
                                       uint32_t src_width, uint32_t src_height) const = 0;

    [[nodiscard]] const WindowProperties &GetProperties() const { return properties; }

    static std::unique_ptr<Window> Create(const WindowProperties &props = WindowProperties{});

protected:
    WindowProperties properties;
};

} // namespace GE
