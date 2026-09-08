#include "Core/Application.h"
#include "Core/GEInput.h"
#include "Core/GEWindow.h"
#include "Core/Log.h"

#include "Core/KeyCodes.h"
#include "Core/MouseCodes.h"
#include "Core/Timestep.h"
#include "Debug/Assert.h"

#include "Debug/Profiler.h"

#include <Events/ApplicationEvent.h>

#include <GLFW/glfw3.h>
#include "Audio/AudioContext.h"
#include <chrono>
#include <functional>
#include <memory>
#include <thread>

namespace GE {
Application *Application::s_Instance = nullptr;

Application::Application(const std::string &name, ApplicationCommandLineArgs args)
    : m_CommandLineArgs(args) {

    GE_CORE_ASSERT(!s_Instance, "Application already exists!");
    s_Instance = this;

    m_Window = Window::Create(WindowProperties(name, 1600, 900));
    m_Window->SetVSync(VsyncMode::OFF);

    // 音频后端：全局一份，与 Vulkan 解耦（失败不阻塞启动，仅打日志）。
    m_AudioContext = std::make_unique<Audio::AudioContext>();
    if (!m_AudioContext->Init()) {
        GE_CORE_WARN("AudioContext init failed; audio features disabled");
        m_AudioContext.reset();
    }
    m_Window->SetEventCallback(GE_BIND_EVENT_FN(Application::OnEvent));

    // 初始化渲染器（内部完成 VulkanContext → RenderContext → Prepare → ImGui 初始化）
    m_Renderer = std::make_unique<Renderer>(*m_Window);

    // ImGui 已归并 Renderer：把"遍历各 Layer 的 OnImGuiRender"作为回调注入，
    // 由 Renderer::EndFrame 在 ImGui Begin 之后、上屏之前逐帧调用。
    m_Renderer->SetFrameUI([this]() {
        for (auto &layer : m_LayerStack)
            layer->OnImGuiRender();
    });

    m_Window->SetMaximized(true);
}

Application::~Application() {

    GE_CORE_INFO("Application Shoutdown");

    // 1. 等待 GPU 完成所有未完成的工作（必须在释放 Layer 的 GPU 资源之前）
    m_Renderer->WaitIdle();

    // 2. Detach 所有层（层中的 Material/Mesh/Texture 持有 GPU 资源）
    m_LayerStack.Clear();

    // 3. 销毁渲染器（内部再次 waitIdle + 释放 ImGui 与所有 Vulkan 资源）
    m_Renderer.reset();

    // 4. 销毁音频上下文（在 Window 销毁前，Render/Asset 已释放资源）
    m_AudioContext.reset();

    s_Instance = nullptr;
}

void Application::Run() {

    while (m_Running) {
        GE_PROFILE_SCOPE("MainLoop");
        const auto frameStart = std::chrono::steady_clock::now();
        const auto time = static_cast<float>(glfwGetTime());
        Timestep timestep = time - m_LastFrameTime;

        // 帧率计算
        {
            GE_PROFILE_SCOPE("FPSUpdate");
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
            GE_PROFILE_SCOPE("RenderFrame");

            // 1. Begin frame — acquire + begin cmd + layout → ColorAttachment
            m_Renderer->BeginFrame();

            // 2. OnUpdate
            {
                GE_PROFILE_SCOPE("OnUpdate");
                for (auto &layer : m_LayerStack)
                    layer->OnUpdate(timestep);
            }

            // 3. End frame — layout → Present + end cmd + submit + present
            //    （ImGui 已归并 Renderer：其 UI 提交回调在 EndFrame 内部被调用）
            m_Renderer->EndFrame();
        }
        m_Window->OnUpdate();
        GE_PROFILE_FRAME_MARK();

        // Frame rate lock: if this frame finished faster than the target interval,
        // sleep until the ideal frame boundary so the next timestep includes the sleep.
        if (m_FrameRateLimit > 0.0f) {
            GE_PROFILE_SCOPE("FrameRateLimit");
            const auto targetFrameTime = std::chrono::duration<float>(1.0f / m_FrameRateLimit);
            const auto targetDuration = std::chrono::duration_cast<std::chrono::steady_clock::duration>(targetFrameTime);
            const auto elapsed = std::chrono::steady_clock::now() - frameStart;
            if (elapsed < targetDuration) {
                std::this_thread::sleep_until(frameStart + targetDuration);
            }
        }
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

void Application::SetFrameRateLimit(float fps) {
    m_FrameRateLimit = fps;
    if (fps > 0.0f) {
        GE_CORE_INFO("Frame rate locked to {0:.1f} FPS", fps);
    } else {
        GE_CORE_INFO("Frame rate limit disabled");
    }
}

void Application::SetPresentMode(VsyncMode mode) {
    if (!m_Renderer) {
        return;
    }

    // 将 VsyncMode 映射到 Vulkan 呈现模式（与 VulkanRenderContext 构造中的逻辑一致）
    vk::PresentModeKHR present_mode;
    switch (mode) {
    case VsyncMode::ON: present_mode = vk::PresentModeKHR::eFifo;
        break;
    case VsyncMode::OFF: present_mode = vk::PresentModeKHR::eMailbox;
        break;
    case VsyncMode::Default:
    default: present_mode = vk::PresentModeKHR::eMailbox;
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
