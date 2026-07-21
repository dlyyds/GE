#include "Core/Application.h"
#include "Core/GEInput.h"
#include "Core/GEWindow.h"
#include "Core/Log.h"

#include "Core/KeyCodes.h"
#include "Core/MouseCodes.h"
#include "Core/Timestep.h"
#include "Debug/Assert.h"
#include "ImGui/ImGuiLayer.h"

#include "Render/VulkanBase/VulkanRenderingInfo.h"
#include "Render/VulkanBase/VulkanRenderContext.h"
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
    m_Window->SetEventCallback(GE_BIND_EVENT_FN(Application::OnEvent));

    // 1. 初始化 Vulkan 上下文（构造中完成 Instance → Surface → Device → VMA）
    m_VulkanContext = std::make_unique<VulkanContext>(*m_Window);

    // 2. 创建 RenderContext（内部创建 Swapchain 和 RenderFrame 管理）
    auto &dev = m_VulkanContext->GetDevice();
    m_RenderContext = std::make_unique<VulkanRenderContext>(
        dev, m_VulkanContext->GetSurface(), *m_Window,
        vk::PresentModeKHR::eMailbox,
        std::vector<vk::PresentModeKHR>{vk::PresentModeKHR::eMailbox, vk::PresentModeKHR::eFifo},
        std::vector<vk::SurfaceFormatKHR>{
            {vk::Format::eR8G8B8A8Srgb, vk::ColorSpaceKHR::eSrgbNonlinear},
            {vk::Format::eB8G8R8A8Srgb, vk::ColorSpaceKHR::eSrgbNonlinear},
        });

    // 3. 准备 RenderContext（内部创建 RenderFrames）
    m_RenderContext->Prepare();

    m_ImGuiLayer = CreateRef<ImGuiLayer>();
    PushOverlay(m_ImGuiLayer);

}

Application::~Application() {

    GE_CORE_INFO("Application Shoutdown");

    // 1. 等待 GPU 完成所有未完成的工作，然后才能安全释放资源
    m_VulkanContext->GetVkDevice().waitIdle();

    // 2. Detach 所有层（层中的 Material/Mesh/Texture 持有 GPU 资源）
    m_LayerStack.Clear();
    m_ImGuiLayer.reset();

    // 3. 关闭渲染器（释放 RingBuffer）

    // 4. 销毁 RenderContext（内部销毁 RenderFrames 和 Swapchain）
    m_RenderContext.reset();

    // 6. 销毁 Vulkan 上下文（unique_ptr 析构自动触发 VulkanContext::Destroy）
    m_VulkanContext.reset();

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
            // 1. Begin frame — 自动 acquire next image，处理 surface 变化
            {
                ZoneScopedN("BeginFrame");
                m_ActiveFrameCmd = m_RenderContext->Begin();
                m_ActiveFrameCmd->Begin(vk::CommandBufferUsageFlagBits::eOneTimeSubmit);
            }

            // 2. Transition to color attachment
            {
                ZoneScopedN("TransitionToColor");
                auto &swapchain = m_RenderContext->GetSwapchain();
                auto &img = swapchain.GetImages()[m_RenderContext->GetActiveFrameIndex()];
                image_utils::TransitionLayout(m_ActiveFrameCmd->GetHandle(), img.GetHandle(),
                                              vk::ImageLayout::eUndefined,
                                              vk::ImageLayout::eColorAttachmentOptimal);
            }

            // 3. OnUpdate（内部调用 Renderer::BeginScene + draw + EndScene）
            {
                ZoneScopedN("OnUpdate");
                for (auto &layer : m_LayerStack)
                    layer->OnUpdate(timestep);
            }

            // 4. ImGui
            {
                ZoneScopedN("ImGuiRender");
                ImGuiLayer::Begin();
                for (auto &layer : m_LayerStack)
                    layer->OnImGuiRender();
                ImGuiLayer::End();
            }

            // 5. Transition to present + end command buffer
            {
                ZoneScopedN("TransitionToPresent");
                auto &swapchain = m_RenderContext->GetSwapchain();
                auto &img = swapchain.GetImages()[m_RenderContext->GetActiveFrameIndex()];
                image_utils::TransitionLayout(m_ActiveFrameCmd->GetHandle(), img.GetHandle(),
                                              vk::ImageLayout::eColorAttachmentOptimal,
                                              vk::ImageLayout::ePresentSrcKHR);
                m_ActiveFrameCmd->End();
            }

            // 6. Submit + present
            {
                ZoneScopedN("SubmitAndPresent");
                m_RenderContext->SubmitAndPresent(m_ActiveFrameCmd->GetHandle());
            }

            m_ActiveFrameCmd.reset();
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

void Application::RecreateSwapchain() {
    auto vkDevice = m_VulkanContext->GetVkDevice();
    auto windowWidth = m_Window->GetWidth();
    auto windowHeight = m_Window->GetHeight();

    if (windowWidth == 0 || windowHeight == 0) {
        m_Minimized = true;
        return;
    }
    m_Minimized = false;

    // 等待 GPU 完成所有未完成的工作
    vkDevice.waitIdle();

    // 使用 RenderContext 重建 swapchain（内部更新 RenderFrames 的 RenderTarget）
    m_RenderContext->UpdateSwapchain(vk::Extent2D{windowWidth, windowHeight});

    GE_CORE_INFO("Swapchain recreated: {}x{}", windowWidth, windowHeight);
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

void Application::PushLayer(const Ref<Layer> &layer) {

    m_LayerStack.PushLayer(layer);
    layer->OnAttach();
}

void Application::PushOverlay(const Ref<Layer> &layer) {

    m_LayerStack.PushOverlay(layer);
    layer->OnAttach();
}


} // namespace GE
