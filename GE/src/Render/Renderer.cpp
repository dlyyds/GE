#include "Render/Renderer.h"
#include "Render/Renderer2D.h"

#include "Core/GEWindow.h"
#include "Core/Log.h"
#include "Debug/Assert.h"
#include "Render/VulkanBase/VulkanImage.h"

#include "tracy/Tracy.hpp"

namespace GE {

Renderer *Renderer::s_Instance = nullptr;

Renderer::Renderer(Window &window)
    : m_Window(window) {
    GE_CORE_ASSERT(!s_Instance, "Renderer already exists!");
    s_Instance = this;

    // 1. 初始化 Vulkan 上下文（构造中完成 Instance → Surface → Device → VMA）
    m_VulkanContext = std::make_unique<VulkanContext>(m_Window);

    // 2. 创建 RenderContext（内部创建 Swapchain 和 RenderFrame 管理）
    auto &dev = m_VulkanContext->GetDevice();
    m_RenderContext = std::make_unique<VulkanRenderContext>(
        dev, m_VulkanContext->GetSurface(), m_Window,
        vk::PresentModeKHR::eMailbox,
        std::vector<vk::PresentModeKHR>{vk::PresentModeKHR::eMailbox, vk::PresentModeKHR::eFifo},
        std::vector<vk::SurfaceFormatKHR>{
            {vk::Format::eR8G8B8A8Srgb, vk::ColorSpaceKHR::eSrgbNonlinear},
            {vk::Format::eB8G8R8A8Srgb, vk::ColorSpaceKHR::eSrgbNonlinear},
        });

    // 3. 准备 RenderContext（内部创建 RenderFrames）
    m_RenderContext->Prepare();

    // 4. 初始化 2D 精灵渲染器
    m_2DRenderer = std::make_unique<Renderer2D>();
}

Renderer::~Renderer() {
    GE_CORE_INFO("Renderer Shutdown");

    // 1. 等待 GPU 完成所有未完成的工作
    WaitIdle();

    // 2. 按构造逆序销毁
    m_2DRenderer.reset();
    m_ActiveFrameCmd = nullptr; // 仅为观察指针，实际由 RenderContext 所有
    m_RenderContext.reset();
    m_VulkanContext.reset();

    s_Instance = nullptr;
}

void Renderer::WaitIdle() {
    if (m_VulkanContext) {
        m_VulkanContext->GetVkDevice().waitIdle();
    }
}

VulkanCommandBuffer &Renderer::BeginFrame() {
    ZoneScopedN("Renderer::BeginFrame");

    // 1. Acquire next image + 获取 command buffer
    m_ActiveFrameCmd = &m_RenderContext->Begin();

    // 2. Begin command buffer
    m_ActiveFrameCmd->Begin(vk::CommandBufferUsageFlagBits::eOneTimeSubmit);

    // 3. Transition to color attachment layout
    {
        ZoneScopedN("TransitionToColor");
        auto &swapchain = m_RenderContext->GetSwapchain();
        auto &img = swapchain.GetImages()[m_RenderContext->GetActiveFrameIndex()];
        image_utils::TransitionLayout(m_ActiveFrameCmd->GetHandle(), img.GetHandle(),
                                      vk::ImageLayout::eUndefined,
                                      vk::ImageLayout::eColorAttachmentOptimal);
    }

    return *m_ActiveFrameCmd;
}

void Renderer::EndFrame() {
    ZoneScopedN("Renderer::EndFrame");

    GE_CORE_ASSERT(m_ActiveFrameCmd, "No active frame command buffer!");

    // 1. Transition to present layout
    {
        ZoneScopedN("TransitionToPresent");
        auto &swapchain = m_RenderContext->GetSwapchain();
        auto &img = swapchain.GetImages()[m_RenderContext->GetActiveFrameIndex()];
        image_utils::TransitionLayout(m_ActiveFrameCmd->GetHandle(), img.GetHandle(),
                                      vk::ImageLayout::eColorAttachmentOptimal,
                                      vk::ImageLayout::ePresentSrcKHR);
    }

    // 2. End command buffer
    m_ActiveFrameCmd->End();

    // 3. Submit + present
    {
        ZoneScopedN("SubmitAndPresent");
        m_RenderContext->SubmitAndPresent(m_ActiveFrameCmd->GetHandle());
    }

    // 4. 重置当前帧 command buffer（仅置空观察指针）
    m_ActiveFrameCmd = nullptr;
}

bool Renderer::RecreateSwapchain(uint32_t width, uint32_t height) {
    if (width == 0 || height == 0) {
        return false;
    }

    WaitIdle();
    m_RenderContext->UpdateSwapchain(vk::Extent2D{width, height});

    GE_CORE_INFO("Swapchain recreated: {}x{}", width, height);
    return true;
}

// ========================================================================
// 静态访问方法
// ========================================================================

Renderer &Renderer::Get() {
    GE_CORE_ASSERT(s_Instance, "Renderer not initialized!");
    return *s_Instance;
}

VulkanContext &Renderer::GetVulkanContext() {
    return Get().m_VulkanContext.operator*();
}

VulkanRenderContext &Renderer::GetRenderContext() {
    return Get().m_RenderContext.operator*();
}

const VulkanSwapchain &Renderer::GetSwapchain() {
    return GetRenderContext().GetSwapchain();
}

VulkanCommandBuffer &Renderer::GetFrameCmd() {
    GE_CORE_ASSERT(Get().m_ActiveFrameCmd, "No active frame command buffer!");
    return *Get().m_ActiveFrameCmd;
}

uint32_t Renderer::GetFrameImageIndex() {
    return GetRenderContext().GetActiveFrameIndex();
}

VulkanImageView &Renderer::GetFrameImageView() {
    return GetRenderContext().GetActiveFrame().GetRenderTarget().GetSwapchainView();
}

Renderer2D &Renderer::Get2DRenderer() {
    GE_CORE_ASSERT(Get().m_2DRenderer, "Renderer2D not initialized!");
    return *Get().m_2DRenderer;
}

} // namespace GE
