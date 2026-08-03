#include "Core/Application.h"
#include "Core/GEInput.h"
#include "Core/GEWindow.h"
#include "Core/Log.h"

#include "Core/KeyCodes.h"
#include "Core/MouseCodes.h"
#include "Core/Timestep.h"
#include "Debug/Assert.h"
#include "ImGui/ImGuiLayer.h"

#include "tracy/Tracy.hpp"

#include <Events/ApplicationEvent.h>

#include <GLFW/glfw3.h>
#include <functional>
#include <memory>

namespace GE {
Application *Application::s_Instance = nullptr;

Application::Application(const std::string &name, ApplicationCommandLineArgs args)
    : m_CommandLineArgs(args) {

    GE_CORE_ASSERT(!s_Instance, "Application already exists!");
    s_Instance = this;

    m_Window = Window::Create(WindowProperties(name, 1600, 900));
    m_Window->SetVSync(VsyncMode::ON);
    m_Window->SetEventCallback(GE_BIND_EVENT_FN(Application::OnEvent));

    // 初始化渲染器（内部完成 VulkanContext → RenderContext → Prepare 完整初始化链）
    m_Renderer = std::make_unique<Renderer>(*m_Window);

    m_ImGuiLayer = std::make_shared<ImGuiLayer>();
    m_ImGuiLayer->BlockEvents(true);
    PushOverlay(m_ImGuiLayer);

}

Application::~Application() {

    GE_CORE_INFO("Application Shoutdown");

    // 1. 等待 GPU 完成所有未完成的工作（必须在释放 Layer 的 GPU 资源之前）
    m_Renderer->WaitIdle();

    // 2. Detach 所有层（层中的 Material/Mesh/Texture 持有 GPU 资源）
    m_LayerStack.Clear();
    m_ImGuiLayer.reset();

    // 3. 销毁渲染器（内部再次 waitIdle + 释放所有 Vulkan 资源）
    m_Renderer.reset();

    s_Instance = nullptr;
}

void Application::Run() {

    while (m_Running) {
        ZoneScopedN("MainLoop");
        const auto time = static_cast<float>(glfwGetTime());
        Timestep timestep = time - m_LastFrameTime;

        // 帧率计算
        {
            ZoneScopedN("FPSUpdate");
            m_FrameTimeAccumulator += timestep;
            m_FrameCount++;

            // 每累计 0.2 秒更新一次 FPS（平滑显示）
            if (m_FrameTimeAccumulator >= 0.2f) {
                m_FPS = (float)m_FrameCount / m_FrameTimeAccumulator;

                // 重置
                m_FrameTimeAccumulator = 0.0f;
                m_FrameCount = 0;
            }
        }

        m_LastFrameTime = time;

        if (!m_Minimized) {
            ZoneScopedN("RenderFrame");

            // 1. Begin frame — acquire + begin cmd + layout → ColorAttachment
            m_Renderer->BeginFrame();

            // 2. OnUpdate
            {
                ZoneScopedN("OnUpdate");
                for (auto &layer : m_LayerStack)
                    layer->OnUpdate(timestep);
            }

            // 3. ImGui
            {
                ZoneScopedN("ImGuiRender");
                ImGuiLayer::Begin();
                for (auto &layer : m_LayerStack)
                    layer->OnImGuiRender();
                ImGuiLayer::End();
            }

            //4. End frame — layout → Present + end cmd + submit + present
            m_Renderer->EndFrame();
        }
        m_Window->OnUpdate();
        FrameMark;
    }

}

void Application::OnEvent(Event &e) {
    GE_PROFILE_FUNCTION();
    EventDispatcher dispatcher(e);
    dispatcher.Dispatch<WindowCloseEvent>(GE_BIND_EVENT_FN(Application::OnWindowClose));
    dispatcher.Dispatch<WindowResizeEvent>(GE_BIND_EVENT_FN(Application::OnWindowResized));

    for (auto it = m_LayerStack.rbegin(); it != m_LayerStack.rend(); ++it) {
        if (e.Handled)
            break;
        (*it)->OnEvent(e);
    }
}

void Application::Close() { m_Running = false; }

void Application::SetPresentMode(VsyncMode mode) {
    if (!m_Renderer) {
        return;
    }

    // 将 VsyncMode 映射到 Vulkan 呈现模式（与 VulkanRenderContext 构造中的逻辑一致）
    vk::PresentModeKHR present_mode;
    switch (mode) {
    case VsyncMode::ON:
        present_mode = vk::PresentModeKHR::eFifo;
        break;
    case VsyncMode::OFF:
        present_mode = vk::PresentModeKHR::eMailbox;
        break;
    case VsyncMode::Default:
    default:
        present_mode = vk::PresentModeKHR::eMailbox;
        break;
    }

    // 同步更新 Window 的 vsync 属性，保持状态一致
    m_Window->SetVSync(mode);

    m_Renderer->SetPresentMode(present_mode);
}

void Application::RecreateSwapchain() {
    auto windowWidth = m_Window->GetWidth();
    auto windowHeight = m_Window->GetHeight();

    if (windowWidth == 0 || windowHeight == 0) {
        m_Minimized = true;
        return;
    }
    m_Minimized = false;

    // 委托给 Renderer 重建 swapchain
    m_Renderer->RecreateSwapchain(windowWidth, windowHeight);
}

bool Application::OnWindowResized(const WindowResizeEvent &e) {
    if (e.GetWidth() == 0 || e.GetHeight() == 0) {
        m_Minimized = true;
        return false;
    }
    m_Minimized = false;
    RecreateSwapchain();
    return false;
}

bool Application::OnWindowClose(WindowCloseEvent &e) {
    m_Running = false;
    return true;
}

void Application::PushLayer(const std::shared_ptr<Layer> &layer) {

    m_LayerStack.PushLayer(layer);
    layer->OnAttach();
}

void Application::PushOverlay(const std::shared_ptr<Layer> &layer) {

    m_LayerStack.PushOverlay(layer);
    layer->OnAttach();
}


} // namespace GE
