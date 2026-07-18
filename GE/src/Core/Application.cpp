#include "Core/Application.h"
#include "Core/GEInput.h"
#include "Core/GEWindow.h"
#include "Core/Log.h"

#include "Core/KeyCodes.h"
#include "Core/MouseCodes.h"
#include "Core/Timestep.h"
#include "Debug/Assert.h"
#include "ImGui/ImGuiLayer.h"

#include "Render/VulkanBase/VulkanImage.h"
#include "Render/VulkanBase/VulkanRenderingInfo.h"

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

    // 2. 创建 Swapchain（匹配 HPPSwapchain 构造函数）
    auto &dev = m_VulkanContext->GetDevice();
    m_Swapchain = std::make_unique<VulkanSwapchain>(
        dev, m_VulkanContext->GetSurface(),
        vk::PresentModeKHR::eMailbox,
        std::vector<vk::PresentModeKHR>{vk::PresentModeKHR::eMailbox, vk::PresentModeKHR::eFifo},
        std::vector<vk::SurfaceFormatKHR>{
            {vk::Format::eR8G8B8A8Srgb, vk::ColorSpaceKHR::eSrgbNonlinear},
            {vk::Format::eB8G8R8A8Srgb, vk::ColorSpaceKHR::eSrgbNonlinear},
        },
        vk::Extent2D{m_Window->GetWidth(), m_Window->GetHeight()});

    // 3. 创建 swapchain image views + per-frame 资源
    {
        auto &images = m_Swapchain->GetImages();
        auto imageCount = images.size();
        auto vkDevice = dev.GetHandle();
        auto queueIndex = dev.GetQueueByFlags(vk::QueueFlagBits::eGraphics, 0).GetFamilyIndex();

        // ImageViews
        m_SwapchainImageViews.reserve(imageCount);
        for (auto &img : images) {
            m_SwapchainImageViews.push_back(image_utils::CreateView(
                vkDevice, img.GetHandle(), vk::ImageViewType::e2D, m_Swapchain->GetFormat()));
        }

        // PerFrame（command pool / fence / semaphores）
        m_PerFrame.resize(imageCount);
        for (auto &pf : m_PerFrame) {
            pf.Init(vkDevice, queueIndex);
        }
    }

    // 5. 初始化渲染器（RingBuffer 等）
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

    // 4. 关闭渲染器（释放 RingBuffer）

    // 5. 销毁 per-frame 资源
    auto vkDevice = m_VulkanContext->GetVkDevice();
    for (auto v : m_SwapchainImageViews)
        vkDevice.destroyImageView(v);
    m_SwapchainImageViews.clear();

    for (auto &pf : m_PerFrame)
        pf.Destroy(vkDevice);
    m_PerFrame.clear();

    for (auto sem : m_RecycledSemaphores)
        vkDevice.destroySemaphore(sem);
    m_RecycledSemaphores.clear();

    // 6. 销毁 Swapchain（unique_ptr 析构自动触发 VulkanSwapchain 析构）
    m_Swapchain.reset();

    // 7. 销毁 Vulkan 上下文（unique_ptr 析构自动触发 VulkanContext::Destroy）
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
            auto vkDevice = m_VulkanContext->GetVkDevice();
            auto queue = m_VulkanContext->GetVkQueue();
            auto &swapchain = *m_Swapchain;

            // 1. 准备 acquire semaphore（优先从回收池取）
            vk::Semaphore acquireSem;
            if (m_RecycledSemaphores.empty()) {
                acquireSem = vkDevice.createSemaphore(vk::SemaphoreCreateInfo{});
            } else {
                acquireSem = m_RecycledSemaphores.back();
                m_RecycledSemaphores.pop_back();
            }

            // 2. Acquire next image
            auto [result, imageIndex] = swapchain.AcquireNextImage(acquireSem);
            if (result == vk::Result::eErrorOutOfDateKHR || result == vk::Result::eSuboptimalKHR) {
                m_RecycledSemaphores.push_back(acquireSem);
                RecreateSwapchain();
                m_Window->OnUpdate();
                continue;
            }
            if (result != vk::Result::eSuccess) {
                m_RecycledSemaphores.push_back(acquireSem);
                m_Window->OnUpdate();
                continue;
            }

            // 3. Per-frame 同步 + 交换 acquire semaphore
            auto &pf = m_PerFrame[imageIndex];
            pf.WaitAndResetFence(vkDevice);
            pf.ResetCommandPool(vkDevice);

            vk::Semaphore oldSem = pf.TakeAcquireSemaphore();
            if (oldSem)
                m_RecycledSemaphores.push_back(oldSem);
            pf.GiveAcquireSemaphore(acquireSem);

            // 4. Begin command buffer + layout transition
            auto cmd = pf.GetCommandBuffer();
            cmd.begin(vk::CommandBufferBeginInfo{.flags = vk::CommandBufferUsageFlagBits::eOneTimeSubmit});
            image_utils::TransitionLayout(cmd, swapchain.GetImages()[imageIndex].GetHandle(),
                                          vk::ImageLayout::eUndefined, vk::ImageLayout::eColorAttachmentOptimal);

            // 5. 存储帧状态（供 Renderer / ImGuiLayer 通过 Application 访问）
            m_CurrentCmd = cmd;
            m_CurrentImageIndex = imageIndex;

            // 6. OnUpdate（内部调用 Renderer::BeginScene + draw + EndScene）
            for (auto &layer : m_LayerStack)
                layer->OnUpdate(timestep);

            // 7. ImGui
            ImGuiLayer::Begin();
            for (auto &layer : m_LayerStack)
                layer->OnImGuiRender();
            ImGuiLayer::End();

            // 8. Transition to present + end command buffer
            image_utils::TransitionLayout(cmd, swapchain.GetImages()[imageIndex].GetHandle(),
                                          vk::ImageLayout::eColorAttachmentOptimal, vk::ImageLayout::ePresentSrcKHR);
            cmd.end();

            // 9. Submit
            {
                vk::PipelineStageFlags waitStage = vk::PipelineStageFlagBits::eColorAttachmentOutput;
                vk::Semaphore acquire = pf.GetAcquireSemaphore();
                vk::Semaphore release = pf.GetReleaseSemaphore();
                vk::Fence fence = pf.GetSubmitFence();

                vk::SubmitInfo submitInfo{
                    .waitSemaphoreCount = 1,
                    .pWaitSemaphores = &acquire,
                    .pWaitDstStageMask = &waitStage,
                    .commandBufferCount = 1,
                    .pCommandBuffers = &cmd,
                    .signalSemaphoreCount = 1,
                    .pSignalSemaphores = &release,
                };
                queue.submit(submitInfo, fence);
            }

            // 10. Present
            {
                vk::Semaphore release = pf.GetReleaseSemaphore();
                auto swapchainHandle = swapchain.GetHandle();
                vk::PresentInfoKHR presentInfo{
                    .waitSemaphoreCount = 1,
                    .pWaitSemaphores = &release,
                    .swapchainCount = 1,
                    .pSwapchains = &swapchainHandle,
                    .pImageIndices = &imageIndex,
                };
                try {
                    auto presentResult = queue.presentKHR(presentInfo);
                    if (presentResult == vk::Result::eErrorOutOfDateKHR || presentResult == vk::Result::eSuboptimalKHR) {
                        RecreateSwapchain();
                    }
                } catch (vk::OutOfDateKHRError &) {
                    RecreateSwapchain();
                }
            }

            // 11. 清除帧状态
            m_CurrentCmd = nullptr;
            m_CurrentImageIndex = ~0u;
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

void Application::RecreateSwapchain() {
    auto vkDevice    = m_VulkanContext->GetVkDevice();
    auto windowWidth = m_Window->GetWidth();
    auto windowHeight = m_Window->GetHeight();

    if (windowWidth == 0 || windowHeight == 0) {
        m_Minimized = true;
        return;
    }
    m_Minimized = false;

    // 等待 GPU 完成所有未完成的工作
    vkDevice.waitIdle();

    // 1. 销毁旧 image views
    for (auto v : m_SwapchainImageViews)
        vkDevice.destroyImageView(v);
    m_SwapchainImageViews.clear();

    // 2. 使用重建构造函数创建新 swapchain（沿用旧 swapchain 的参数，仅更新 extent）
    auto newSwapchain = std::make_unique<VulkanSwapchain>(
        *m_Swapchain,
        vk::Extent2D{windowWidth, windowHeight});
    m_Swapchain = std::move(newSwapchain);

    // 3. 为新 swapchain images 创建 image views
    auto &images = m_Swapchain->GetImages();
    m_SwapchainImageViews.reserve(images.size());
    for (auto &img : images) {
        m_SwapchainImageViews.push_back(image_utils::CreateView(
            vkDevice, img.GetHandle(), vk::ImageViewType::e2D, m_Swapchain->GetFormat()));
    }

    // 4. image count 变化时调整 per-frame 资源数组
    if (m_PerFrame.size() != images.size()) {
        for (auto &pf : m_PerFrame)
            pf.Destroy(vkDevice);
        m_PerFrame.clear();

        auto &dev = *m_VulkanContext;
        auto queueIndex = dev.GetDevice().GetQueueByFlags(vk::QueueFlagBits::eGraphics, 0).GetFamilyIndex();
        m_PerFrame.resize(images.size());
        for (auto &pf : m_PerFrame)
            pf.Init(vkDevice, queueIndex);
    }

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
