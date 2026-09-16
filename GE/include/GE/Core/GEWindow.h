#pragma once

#include "Base.h"
#include "Events/Event.h"

#include <optional>
#include <vulkan/vulkan.h>
#include <glm/glm.hpp>


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

/**
 * 光标模式。
 *
 * `Disabled` 对应「锁定并隐藏」：光标从窗口坐标中脱离、只上报相对位移，用于自由
 * 视角相机持续转向。原先两层各自直接调 GLFW 的 `glfwSetInputMode(GLFW_CURSOR, …)`
 * 与 `glfwGetCursorPos`（GameLayer / SceneLayer 各一份），现收编到窗口接口。
 */
enum class CursorMode {
    Normal, ///< 光标可见、可自由移动
    Disabled ///< 光标隐藏并锁定（相对位移模式）
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

    /**
     * 原始平台事件观察者（可选）。
     *
     * ImGui 的 SDL3 后端**没有** GLFW 后端那种「自己装回调」的机制 —— 原先用的是
     * `ImGui_ImplGlfw_InitForOther(window, true)`，由浮后端接管回调链。SDL 是轮询模型，
     * 必须逐事件喂 `ImGui_ImplSDL3_ProcessEvent`。为了不让窗口层反向依赖 ImGui，
     * 这里开一个通用钩子：实参是 `const SDL_Event *`，以 `const void *` 传递，
     * 避免把平台类型写进这个平台无关的接口（与 `GetNativeWindow()` 返回 `void *` 同风格）。
     *
     * 钩子在**引擎自身的事件翻译之前**调用，与 GLFW 版的先后顺序一致：ImGui 先据此
     * 算出 WantCaptureMouse / WantCaptureKeyboard，引擎再照常收到全部事件。
     */
    using RawPlatformEventHook = std::function<void(const void *)>;

    virtual void SetRawPlatformEventHook(RawPlatformEventHook hook) = 0;

    virtual void SetVSync(VsyncMode mode) = 0;

    [[nodiscard]] virtual VsyncMode GetVSync() const = 0;

    virtual void SetResizable(bool resizable) = 0;

    [[nodiscard]] virtual bool IsResizable() const = 0;

    [[nodiscard]] virtual WindowMode GetWindowMode() const = 0;

    /// 设置窗口标题（运行时可改：如运行时按 game.cfg 应用标题）
    virtual void SetTitle(const std::string &title) = 0;

    /// 切换窗口模式（窗口化 ↔ 全屏）。进入全屏时尺寸对齐主显示器当前视频模式。
    virtual void SetWindowMode(WindowMode mode) = 0;

    /// 尝试调整窗口大小，返回实际尺寸
    virtual Extent Resize(const Extent &new_extent) = 0;

    /// 设置窗口最大化状态（true 最大化，false 恢复）
    virtual void SetMaximized(bool maximized) = 0;

    /// 底层原生窗口句柄。SDL 侧一律是 `SDL_Window*`（桌面与 Android 一致）。
    /// 需要更深层的句柄（如 Win32 HWND、Android ANativeWindow）请自行用
    /// `SDL_GetWindowWMInfo` / `SDL_GetWindowProperties` 转换。
    [[nodiscard]] virtual void *GetNativeWindow() const = 0;

    /// 设置光标模式：Normal 释放、Disabled 锁定并隐藏（自由视角相机用）
    virtual void SetCursorMode(CursorMode mode) = 0;

    /// 当前光标在窗口坐标中的位置（像素）。
    /// 注意 CursorMode::Disabled 下绝对坐标不再有意义，此时应改用 InputState 的鼠标增量。
    [[nodiscard]] virtual glm::vec2 GetCursorPosition() const = 0;

    /// 创建 Vulkan 表面
    virtual VkSurfaceKHR CreateVulkanSurface(VkInstance instance) = 0;

    virtual VkSurfaceKHR CreateVulkanSurface(VkInstance instance, VkPhysicalDevice physical_device) = 0;

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
