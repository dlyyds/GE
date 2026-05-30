#include "Core/Application.h"
#include "Core/GEInput.h"
#include "Core/GEWindow.h"
#include "Core/Log.h"

#include "Core/KeyCodes.h"
#include "Core/MouseCodes.h"
#include "Core/Timestep.h"
#include "Debug/Assert.h"
#include "ImGui/ImGuiLayer.h"

#include <Events/ApplicationEvent.h>

#include <GLFW/glfw3.h>
#include <functional>
#include <memory>
#include <regex>

namespace GE {
Application *Application::s_Instance = nullptr;

Application::Application(const std::string &name, ApplicationCommandLineArgs args)
    : m_CommandLineArgs(args) {
    GE_PROFILE_FUNCTION();

    GE_CORE_ASSERT(!s_Instance, "Application already exists!");
    s_Instance = this;
    //  WindowProps p("title", 1000, 700);
    m_Window = Window::Create(WindowProps(name, 1600, 900));
    m_Window->SetEventCallback(GE_BIND_EVENT_FN(Application::OnEvent));

    m_ImGuiLayer = CreateRef<ImGuiLayer>();
    PushOverlay(m_ImGuiLayer);

}

Application::~Application() {
    GE_PROFILE_FUNCTION();

    GE_CORE_INFO("Application Shoutdown");
}

void Application::Run() {
    GE_PROFILE_FUNCTION();

    while (m_Running) {
        const auto time = static_cast<float>(glfwGetTime());
        Timestep timestep = time - m_LastFrameTime;

        // 帧率计算
        {
            m_FrameTimeAccumulator += timestep;
            m_FrameCount++;

            // 每累计 0.2 秒更新一次 FPS（平滑显示）
            if (m_FrameTimeAccumulator >= 0.2f) {
                m_FPS = (float)m_FrameCount / m_FrameTimeAccumulator;

                // 重置
                m_FrameTimeAccumulator = 0.0f;
                m_FrameCount = 0;

                //    GE_CORE_INFO("fps: {0}", m_FPS);
            }
        }

        m_LastFrameTime = time;

        if (!m_Minimized) {
            for (auto &layer : m_LayerStack)
                layer->OnUpdate(timestep);
        }

        ImGuiLayer::Begin();
        for (auto &layer : m_LayerStack)
            layer->OnImGuiRender();
        ImGuiLayer::End();

        m_Window->OnUpdate();
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

bool Application::OnWindowResized(const WindowResizeEvent &e) {
    if (e.GetWidth() == 0 || e.GetHeight() == 0) {
        m_Minimized = true;
        return false;
    }
    m_Minimized = false;
    return false;
}

bool Application::OnWindowClose(WindowCloseEvent &e) {
    m_Running = false;
    return true;
}

void Application::PushLayer(const Ref<Layer> &layer) {
    GE_PROFILE_FUNCTION();
    m_LayerStack.PushLayer(layer);
    layer->OnAttach();
}

void Application::PushOverlay(const Ref<Layer> &layer) {
    GE_PROFILE_FUNCTION();
    m_LayerStack.PushOverlay(layer);
    layer->OnAttach();
}


} // namespace GE
