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

namespace GE {
Application *Application::s_Instance = nullptr;

Application::Application(const std::string &name, ApplicationCommandLineArgs args)
    : m_CommandLineArgs(args) {
    GE_PROFILE_FUNCTION();

    GE_CORE_ASSERT(!s_Instance, "Application already exists!");
    s_Instance = this;

    m_Window = Window::Create(WindowProps(name, 1600, 900));
    m_Window->SetEventCallback(GE_BIND_EVENT_FN(Application::OnEvent));

    // 1. 初始化 Vulkan 上下文（构造中完成 Instance → Surface → Device → VMA）
    m_VulkanContext = std::make_unique<VulkanContext>(*m_Window);

    // 2. 创建 Swapchain（格式/呈现模式使用默认优先级列表）
    auto &dev = m_VulkanContext->GetDevice();
    m_Swapchain = std::make_unique<VulkanSwapchain>(
        dev.GetHandle(), dev.GetGpu().GetHandle(), m_VulkanContext->GetSurface(),
        dev.GetQueue(), dev.GetGraphicsQueueIndex(),
        std::vector<vk::SurfaceFormatKHR>{
            {vk::Format::eR8G8B8A8Srgb, vk::ColorSpaceKHR::eSrgbNonlinear},
            {vk::Format::eB8G8R8A8Srgb, vk::ColorSpaceKHR::eSrgbNonlinear},
        },
        std::vector<vk::PresentModeKHR>{
            vk::PresentModeKHR::eMailbox,
            vk::PresentModeKHR::eFifo,
        },
        VulkanSwapchainProperties{.extent = {m_Window->GetWidth(), m_Window->GetHeight()}});

    // 3. 初始化资源管理器（纹理缓存等）
    m_ResourceManager.Init(m_VulkanContext->GetVmaAllocator(),
                           m_VulkanContext->GetVkQueue(),
                           m_VulkanContext->GetGraphicsQueueIndex());

    // 4. 初始化渲染器（RingBuffer 等）
    //  Renderer::Get().Init(m_VulkanContext, m_Swapchain);

    m_ImGuiLayer = CreateRef<ImGuiLayer>();
    PushOverlay(m_ImGuiLayer);

}

Application::~Application() {
    GE_PROFILE_FUNCTION();
    GE_CORE_INFO("Application Shoutdown");

    // 1. 等待 GPU 完成所有未完成的工作，然后才能安全释放资源
    m_VulkanContext->GetVkDevice().waitIdle();

    // 2. Detach 所有层（层中的 Material/Mesh/Texture 持有 GPU 资源）
    m_LayerStack.Clear();
    m_ImGuiLayer.reset();

    // 3. 关闭资源管理器（释放纹理等 GPU 资源）
    m_ResourceManager.Shutdown();

    // 4. 关闭渲染器（释放 RingBuffer）
    Renderer::Get().Shutdown();

    // 5. 销毁 Swapchain（unique_ptr 析构自动触发 VulkanSwapchain 析构）
    m_Swapchain.reset();

    // 6. 销毁 Vulkan 上下文（unique_ptr 析构自动触发 VulkanContext::Destroy）
    m_VulkanContext.reset();

    s_Instance = nullptr;
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
            if (m_Swapchain->BeginFrame()) {
                for (auto &layer : m_LayerStack)
                    layer->OnUpdate(timestep);

                ImGuiLayer::Begin();
                for (auto &layer : m_LayerStack)
                    layer->OnImGuiRender();
                ImGuiLayer::End();

                m_Swapchain->EndFrame();
            }
        }
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
