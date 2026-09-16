#pragma once

#include "Core/GEWindow.h"

#include <SDL3/SDL_events.h>

#include <utility>

struct SDL_Window;

namespace GE {

/**
 * @brief 基于 SDL3 的窗口实现 —— **桌面与 Android 共用同一份**。
 *
 * 取代原先的 `GlfwWindow`（`Platform/Windows/`）。不再按平台分目录，因为 SDL 已经把
 * 窗口、输入、生命周期在两端归一：同一个 `SDL_Window`、同一套 `SDL_EVENT_*`、
 * 同一套 `SDL_Scancode`，所以不存在"两个平台各一份实现"的需要。
 *
 * 事件模型与 GLFW 的关键差异：GLFW 是**回调**（`glfwSetKeyCallback` 等），SDL 是
 * **轮询**（`SDL_PollEvent`）。故事件翻译集中在 `ProcessEvents()`，`OnUpdate()`
 * 委托给它；`Application::Run()` 里原本"渲染后调 window->OnUpdate()"的时序不变。
 *
 * DPI：`GetWidth/GetHeight` 返回**像素**尺寸（`SDL_GetWindowSizeInPixels`），因为
 * 引擎拿它当 swapchain 的 extent 用。GLFW 的 `glfwGetWindowSize` 返回的是逻辑尺寸，
 * 所以这是本次换库在**高 DPI 显示器上的一处刻意行为变更**（属于修正，不是回归）：
 * 100% 缩放下两者相同，>100% 时 Vulkan 现在会按真实像素渲染。逻辑尺寸另见
 * `GetContentScaleFactor`。
 */
class SdlWindow final : public Window {
public:
    explicit SdlWindow(const WindowProperties &props);

    ~SdlWindow() override;

    /// 每帧推进：轮询并翻译 SDL 事件
    void OnUpdate() override;

    /// 处理所有底层窗口事件（与 OnUpdate 等价，SDL 侧二者同源）
    void ProcessEvents() override;

    [[nodiscard]] bool ShouldClose() override;

    void Close() override;

    [[nodiscard]] uint32_t GetWidth() const override;

    [[nodiscard]] uint32_t GetHeight() const override;

    void SetEventCallback(const EventCallbackFn &callback) override { m_EventCallback = callback; }

    void SetRawPlatformEventHook(RawPlatformEventHook hook) override {
        m_RawEventHook = std::move(hook);
    }

    void SetVSync(VsyncMode mode) override;

    [[nodiscard]] VsyncMode GetVSync() const override;

    void SetResizable(bool resizable) override;

    [[nodiscard]] bool IsResizable() const override;

    [[nodiscard]] WindowMode GetWindowMode() const override;

    void SetTitle(const std::string &title) override;

    void SetWindowMode(WindowMode mode) override;

    Extent Resize(const Extent &new_extent) override;

    void SetMaximized(bool maximized) override;

    [[nodiscard]] void *GetNativeWindow() const override;

    void SetCursorMode(CursorMode mode) override;

    [[nodiscard]] glm::vec2 GetCursorPosition() const override;

    VkSurfaceKHR CreateVulkanSurface(VkInstance instance) override;

    VkSurfaceKHR CreateVulkanSurface(VkInstance instance, VkPhysicalDevice physical_device) override;

    [[nodiscard]] float GetDpiFactor() const override;

    [[nodiscard]] float GetContentScaleFactor() const override;

    [[nodiscard]] bool GetDisplayPresentInfo(VkDisplayPresentInfoKHR *info,
                                             uint32_t src_width, uint32_t src_height) const override;

private:
    void Init(const WindowProperties &props);

    void Shutdown();

    /// 翻译单个 SDL 事件并派发到 m_EventCallback
    void HandleEvent(const SDL_Event &event);

    /// 从窗口重新读取像素尺寸并写入 properties.extent
    void SyncExtentFromWindow();

private:
    SDL_Window *m_Window = nullptr;

    EventCallbackFn m_EventCallback;

    /// 原始 SDL 事件观察者（供 ImGui 平台后端取用），见 Window::SetRawPlatformEventHook
    RawPlatformEventHook m_RawEventHook;

    /// SDL 的 ShouldClose 语义需自己维护（GLFW 有 glfwWindowShouldClose）
    bool m_ShouldClose = false;
};

} // namespace GE
