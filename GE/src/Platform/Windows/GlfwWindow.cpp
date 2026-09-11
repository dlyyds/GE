#include "pch.h"

#include "Events/ApplicationEvent.h"
#include "Events/KeyEvent.h"
#include "Events/MouseEvent.h"

#include "Core/Log.h"
#include "Platform/Windows/GlfwWindow.h"

#define GLFW_INCLUDE_VULKAN
#include "GLFW/glfw3.h"

#include "Core/GEInput.h"
#include "Debug/Assert.h"


namespace GE {

static uint8_t s_GLFWWindowCount = 0;

namespace {
/// 按窗口模式选目标监视器（nullptr = 窗口化：Default / Headless / FullscreenStretch）。
/// 无边框全屏额外把尺寸对齐到显示器当前视频模式；纯全屏交给 GLFW 挑最接近的视频模式。
GLFWmonitor *ResolveMonitorForMode(WindowMode mode, Extent &extent) {
    if (mode == WindowMode::Fullscreen) {
        return glfwGetPrimaryMonitor();
    }
    if (mode == WindowMode::FullscreenBorderless) {
        GLFWmonitor *monitor = glfwGetPrimaryMonitor();
        if (const GLFWvidmode *videoMode = glfwGetVideoMode(monitor)) {
            extent = {static_cast<uint32_t>(videoMode->width),
                      static_cast<uint32_t>(videoMode->height)};
        }
        return monitor;
    }
    return nullptr;
}
} // namespace

GlfwWindow::GlfwWindow(const WindowProperties &props) {
    GE_PROFILE_FUNCTION();
    Init(props);
}

GlfwWindow::~GlfwWindow() { Shutdown(); }

static void GLFWErrorCallback(int error, const char *description) {
    GE_CORE_ERROR("GLFW Error ({0}): {1}", error, description);
}

void GlfwWindow::Init(const WindowProperties &props) {
    GE_PROFILE_FUNCTION();

    properties = props;

    if (s_GLFWWindowCount == 0) {
        GE_PROFILE_SCOPE("glfwInit");
        int success = glfwInit();
        GE_CORE_ASSERT(success, "Could not initialize GLFW!");
        GE_CORE_INFO("Initializing GLFW");
        glfwSetErrorCallback(GLFWErrorCallback);
    }
    ++s_GLFWWindowCount;

    // 设置窗口提示
    glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
    glfwWindowHint(GLFW_RESIZABLE, properties.resizable ? GLFW_TRUE : GLFW_FALSE);

    // 全屏模式（无边框模式会把尺寸对齐到显示器当前视频模式）
    GLFWmonitor *monitor = ResolveMonitorForMode(properties.mode, properties.extent);

    {
        GE_PROFILE_SCOPE("glfwCreateWindow");
        m_Window = glfwCreateWindow(
            static_cast<int>(properties.extent.width),
            static_cast<int>(properties.extent.height),
            properties.title.c_str(),
            monitor, nullptr);

        GE_CORE_INFO("Creating window {0} ({1}, {2})",
                     properties.title, properties.extent.width, properties.extent.height);
    }

    glfwSetWindowUserPointer(m_Window, this);

    // 垂直同步
    SetVSync(properties.vsync);

    glfwSetWindowSizeCallback(m_Window, [](GLFWwindow *window, int width, int height) {
        auto &self = *(GlfwWindow *)glfwGetWindowUserPointer(window);
        self.properties.extent.width = static_cast<uint32_t>(width);
        self.properties.extent.height = static_cast<uint32_t>(height);
        WindowResizeEvent event(width, height);
        self.m_EventCallback(event);
    });

    glfwSetWindowCloseCallback(m_Window, [](GLFWwindow *window) {
        auto &self = *(GlfwWindow *)glfwGetWindowUserPointer(window);
        WindowCloseEvent event;
        self.m_EventCallback(event);
    });

    glfwSetKeyCallback(m_Window,
                       [](GLFWwindow *window, int keyCode, int scancode, int action, int mods) {
                           auto &self = *(GlfwWindow *)glfwGetWindowUserPointer(window);
                           KeyCode key = static_cast<KeyCode>(keyCode);
                           switch (action) {
                           case GLFW_PRESS: {
                               KeyPressedEvent event(key, 0);
                               self.m_EventCallback(event);
                               break;
                           }
                           case GLFW_RELEASE: {
                               KeyReleasedEvent event(key);
                               self.m_EventCallback(event);
                               break;
                           }
                           case GLFW_REPEAT: {
                               KeyPressedEvent event(key, 1);
                               self.m_EventCallback(event);
                               break;
                           }
                           }
                       });

    glfwSetCharCallback(m_Window, [](GLFWwindow *window, uint32_t keycode) {
        auto &self = *(GlfwWindow *)glfwGetWindowUserPointer(window);
        KeyTypedEvent event(static_cast<KeyCode>(keycode));
        self.m_EventCallback(event);
    });

    glfwSetMouseButtonCallback(m_Window, [](GLFWwindow *window, int button, int action, int mods) {
        auto &self = *(GlfwWindow *)glfwGetWindowUserPointer(window);

        switch (action) {
        case GLFW_PRESS: {
            MouseButtonPressedEvent event(static_cast<MouseCode>(button));
            self.m_EventCallback(event);
            break;
        }
        case GLFW_RELEASE: {
            MouseButtonReleasedEvent event(static_cast<MouseCode>(button));
            self.m_EventCallback(event);
            break;
        }
        }
    });

    glfwSetScrollCallback(m_Window, [](GLFWwindow *window, double xOffset, double yOffset) {
        auto &self = *(GlfwWindow *)glfwGetWindowUserPointer(window);
        MouseScrolledEvent event((float)xOffset, (float)yOffset);
        self.m_EventCallback(event);
    });

    glfwSetCursorPosCallback(m_Window, [](GLFWwindow *window, double xPos, double yPos) {
        auto &self = *(GlfwWindow *)glfwGetWindowUserPointer(window);
        MouseMovedEvent event((float)xPos, (float)yPos);
        self.m_EventCallback(event);
    });
}

void GlfwWindow::Shutdown() {
    glfwDestroyWindow(m_Window);
    if (--s_GLFWWindowCount == 0) {
        GE_CORE_INFO("Terminating GLFW");
        glfwTerminate();
    }
}

void GlfwWindow::OnUpdate() {
    glfwPollEvents();
}

void GlfwWindow::ProcessEvents() {
    glfwPollEvents();
}

bool GlfwWindow::ShouldClose() {
    return glfwWindowShouldClose(m_Window) != GLFW_FALSE;
}

void GlfwWindow::Close() {
    glfwSetWindowShouldClose(m_Window, GLFW_TRUE);
}

void GlfwWindow::SetVSync(VsyncMode mode) {
    properties.vsync = mode;
}

VsyncMode GlfwWindow::GetVSync() const {
    return properties.vsync;
}

void GlfwWindow::SetResizable(bool resizable) {
    properties.resizable = resizable;
    glfwSetWindowAttrib(m_Window, GLFW_RESIZABLE, resizable ? GLFW_TRUE : GLFW_FALSE);
}

bool GlfwWindow::IsResizable() const {
    return properties.resizable;
}

WindowMode GlfwWindow::GetWindowMode() const {
    return properties.mode;
}

Extent GlfwWindow::Resize(const Extent &new_extent) {
    glfwSetWindowSize(m_Window,
                      static_cast<int>(new_extent.width),
                      static_cast<int>(new_extent.height));
    return {GetWidth(), GetHeight()};
}

void GlfwWindow::SetMaximized(bool maximized) {
    if (maximized) {
        glfwMaximizeWindow(m_Window);
    } else {
        glfwRestoreWindow(m_Window);
    }
}

void GlfwWindow::SetTitle(const std::string &title) {
    properties.title = title;
    glfwSetWindowTitle(m_Window, title.c_str());
}

void GlfwWindow::SetWindowMode(WindowMode mode) {
    if (mode == properties.mode) {
        return;
    }

    if (mode == WindowMode::Fullscreen || mode == WindowMode::FullscreenBorderless ||
        mode == WindowMode::FullscreenStretch) {
        GLFWmonitor *monitor = glfwGetPrimaryMonitor();
        if (!monitor) {
            GE_CORE_WARN("SetWindowMode: 无法获取主显示器，保持窗口化");
            return;
        }
        // 尺寸/刷新率取显示器当前视频模式；GLFW 会自行切到该模式
        const GLFWvidmode *videoMode = glfwGetVideoMode(monitor);
        const int width = videoMode ? videoMode->width : static_cast<int>(properties.extent.width);
        const int height = videoMode ? videoMode->height : static_cast<int>(properties.extent.height);
        const int refreshRate = videoMode ? videoMode->refreshRate : GLFW_DONT_CARE;
        glfwSetWindowMonitor(m_Window, monitor, 0, 0, width, height, refreshRate);
    } else {
        // 窗口化：沿用当前尺寸（全屏期间 properties.extent 已被显示器尺寸刷新），居中摆放
        const int width = static_cast<int>(properties.extent.width);
        const int height = static_cast<int>(properties.extent.height);
        int x = 0, y = 0;
        if (GLFWmonitor *monitor = glfwGetPrimaryMonitor()) {
            if (const GLFWvidmode *videoMode = glfwGetVideoMode(monitor)) {
                x = (videoMode->width - width) / 2;
                y = (videoMode->height - height) / 2;
            }
        }
        glfwSetWindowMonitor(m_Window, nullptr, x, y, width, height, GLFW_DONT_CARE);
    }

    properties.mode = mode;
}

float GlfwWindow::GetDpiFactor() const {
    auto *monitor = glfwGetWindowMonitor(m_Window);
    if (!monitor) {
        monitor = glfwGetPrimaryMonitor();
    }
    float xscale, yscale;
    glfwGetMonitorContentScale(monitor, &xscale, &yscale);
    return xscale;
}

float GlfwWindow::GetContentScaleFactor() const {
    float xscale, yscale;
    glfwGetWindowContentScale(m_Window, &xscale, &yscale);
    return xscale;
}

bool GlfwWindow::GetDisplayPresentInfo(VkDisplayPresentInfoKHR *info,
                                       uint32_t src_width, uint32_t src_height) const {
    (void)info;
    (void)src_width;
    (void)src_height;
    // 默认实现：不提供额外呈现信息
    return false;
}

VkSurfaceKHR GlfwWindow::CreateVulkanSurface(VkInstance instance) {
    if (instance == VK_NULL_HANDLE || !m_Window) {
        return VK_NULL_HANDLE;
    }
    VkSurfaceKHR surface;
    VkResult err = glfwCreateWindowSurface(instance, m_Window, nullptr, &surface);
    if (err != VK_SUCCESS) {
        return VK_NULL_HANDLE;
    }
    return surface;
}

VkSurfaceKHR GlfwWindow::CreateVulkanSurface(VkInstance instance, VkPhysicalDevice physical_device) {
    (void)physical_device;
    // GLFW 创建表面不需要 physical_device，直接委托
    return CreateVulkanSurface(instance);
}

std::vector<const char *> GlfwWindow::GetRequiredSurfaceExtensions() const {
    uint32_t count;
    const char **extensions = glfwGetRequiredInstanceExtensions(&count);
    if (!extensions) {
        return {};
    }
    return {extensions, extensions + count};
}

} // namespace GE
