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

GlfwWindow::GlfwWindow(const WindowProps &props) {
    GE_PROFILE_FUNCTION();
    Init(props);
}

GlfwWindow::~GlfwWindow() { Shutdown(); }

static void GLFWErrorCallback(int error, const char *description) {
    GE_CORE_ERROR("GLFW Error ({0}): {1}", error, description);
}

void GlfwWindow::Init(const WindowProps &props) {
    GE_PROFILE_FUNCTION();
    m_Data.Title = props.Title;
    m_Data.Width = props.Width;
    m_Data.Height = props.Height;

    if (s_GLFWWindowCount == 0) {
        GE_PROFILE_SCOPE("glfwInit");
        // TODO: glfwTerminate on system shutdown
        int success = glfwInit();
        GE_CORE_ASSERT(success, "Could not initialize GLFW!");
        GE_CORE_INFO("Initializing GLFW");
        glfwSetErrorCallback(GLFWErrorCallback);
    }
    ++s_GLFWWindowCount;
    {
        GE_PROFILE_SCOPE("glfwCreateWindow");

        //glfwWindowHint(GLFW_OPENGL_DEBUG_CONTEXT, GLFW_TRUE);
        glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
        m_Window = glfwCreateWindow((int)props.Width, (int)props.Height, m_Data.Title.c_str(),
                                    nullptr, nullptr);

        GE_CORE_INFO("Creating window {0} ({1}, {2})", props.Title, props.Width, props.Height);
    }

    glfwSetWindowUserPointer(m_Window, &m_Data);

    // 垂直同步
    SetVSync(true);

    glfwSetWindowSizeCallback(m_Window, [](GLFWwindow *window, int width, int height) {
        WindowData &data = *(WindowData *)glfwGetWindowUserPointer(window);
        data.Width = width;
        data.Height = height;
        WindowResizeEvent event(width, height);
        data.EventCallback(event);
    });

    glfwSetWindowCloseCallback(m_Window, [](GLFWwindow *window) {
        WindowData &data = *(WindowData *)glfwGetWindowUserPointer(window);
        WindowCloseEvent event;
        data.EventCallback(event);
    });

    glfwSetKeyCallback(m_Window,
                       [](GLFWwindow *window, int keyCode, int scancode, int action, int mods) {
                           WindowData &data = *(WindowData *)glfwGetWindowUserPointer(window);
                           KeyCode key = static_cast<KeyCode>(keyCode);
                           switch (action) {
                           case GLFW_PRESS: {
                               KeyPressedEvent event(key, 0);
                               data.EventCallback(event);
                               break;
                           }
                           case GLFW_RELEASE: {
                               KeyReleasedEvent event(key);
                               data.EventCallback(event);
                               break;
                           }
                           case GLFW_REPEAT: {
                               KeyPressedEvent event(key, 1);
                               data.EventCallback(event);
                               break;
                           }
                           }
                       });

    glfwSetCharCallback(m_Window, [](GLFWwindow *window, uint32_t keycode) {
        WindowData &data = *(WindowData *)glfwGetWindowUserPointer(window);

        KeyTypedEvent event(static_cast<KeyCode>(keycode));
        data.EventCallback(event);
    });

    glfwSetMouseButtonCallback(m_Window, [](GLFWwindow *window, int button, int action, int mods) {
        WindowData &data = *(WindowData *)glfwGetWindowUserPointer(window);

        switch (action) {
        case GLFW_PRESS: {
            MouseButtonPressedEvent event(static_cast<MouseCode>(button));
            data.EventCallback(event);
            break;
        }
        case GLFW_RELEASE: {
            MouseButtonReleasedEvent event(static_cast<MouseCode>(button));
            data.EventCallback(event);
            break;
        }
        }
    });

    glfwSetScrollCallback(m_Window, [](GLFWwindow *window, double xOffset, double yOffset) {
        WindowData &data = *(WindowData *)glfwGetWindowUserPointer(window);

        MouseScrolledEvent event((float)xOffset, (float)yOffset);
        data.EventCallback(event);
    });

    glfwSetCursorPosCallback(m_Window, [](GLFWwindow *window, double xPos, double yPos) {
        WindowData &data = *(WindowData *)glfwGetWindowUserPointer(window);

        MouseMovedEvent event((float)xPos, (float)yPos);
        data.EventCallback(event);
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

void GlfwWindow::SetVSync(bool enabled) {
    m_Data.VSync = enabled;
}

bool GlfwWindow::IsVSync() const {
    return m_Data.VSync;
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

} // namespace GE
