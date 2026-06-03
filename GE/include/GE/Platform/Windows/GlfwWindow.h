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
    explicit GlfwWindow(const WindowProps &props);

    ~GlfwWindow() override;

    void OnUpdate() override;

    [[nodiscard]] uint32_t GetWidth() const override { return m_Data.Width; }
    [[nodiscard]] uint32_t GetHeight() const override { return m_Data.Height; }

    [[nodiscard]] void *GetGlfwWindow() const override {
        return m_Window;
    }

    [[nodiscard]] void *GetNativeWindow() const override {
        return glfwGetWin32Window(m_Window);
    }

    VkSurfaceKHR CreateSurface(VkInstance instance, VkPhysicalDevice physicalDevice) override;

    // Window attributes
    void SetEventCallback(const EventCallbackFn &callback) override {
        m_Data.EventCallback = callback;
    }

    void SetVSync(bool enabled) override;

    [[nodiscard]] bool IsVSync() const override;

private:
    virtual void Init(const WindowProps &props);

    virtual void Shutdown();

private:
    GLFWwindow *m_Window;

    struct WindowData {
        std::string Title;
        uint32_t Width, Height;
        bool VSync;

        EventCallbackFn EventCallback;
    };

    WindowData m_Data;
};

} // namespace GE