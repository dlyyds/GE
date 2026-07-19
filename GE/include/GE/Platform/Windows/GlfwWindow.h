#pragma once

#include "Core/GEWindow.h"

#define GLFW_INCLUDE_VULKAN
#include "GLFW/glfw3.h"
#define GLFW_EXPOSE_NATIVE_WIN32
#include <GLFW/glfw3native.h>


struct GLFWwindow {
};

namespace GE {


class GlfwWindow final : public Window {
public:
    explicit GlfwWindow(const WindowProperties &props);

    ~GlfwWindow() override;

    void OnUpdate() override;

    void ProcessEvents() override;

    bool ShouldClose() override;

    void Close() override;

    [[nodiscard]] uint32_t GetWidth() const override { return properties.extent.width; }
    [[nodiscard]] uint32_t GetHeight() const override { return properties.extent.height; }

    [[nodiscard]] void *GetGlfwWindow() const override {
        return m_Window;
    }

    [[nodiscard]] void *GetNativeWindow() const override {
        return glfwGetWin32Window(m_Window);
    }

    VkSurfaceKHR CreateVulkanSurface(VkInstance instance) override;
    VkSurfaceKHR CreateVulkanSurface(VkInstance instance, VkPhysicalDevice physical_device) override;

    [[nodiscard]] std::vector<const char *> GetRequiredSurfaceExtensions() const override;

    void SetEventCallback(const EventCallbackFn &callback) override {
        m_EventCallback = callback;
    }

    void SetVSync(VsyncMode mode) override;

    [[nodiscard]] VsyncMode GetVSync() const override;

    void SetResizable(bool resizable) override;

    [[nodiscard]] bool IsResizable() const override;

    [[nodiscard]] WindowMode GetWindowMode() const override;

    Extent Resize(const Extent &new_extent) override;

    [[nodiscard]] float GetDpiFactor() const override;

    [[nodiscard]] float GetContentScaleFactor() const override;

    bool GetDisplayPresentInfo(VkDisplayPresentInfoKHR *info,
                               uint32_t src_width, uint32_t src_height) const override;

private:
    void Init(const WindowProperties &props);

    void Shutdown();

private:
    GLFWwindow *m_Window;

    EventCallbackFn m_EventCallback;
};

} // namespace GE
