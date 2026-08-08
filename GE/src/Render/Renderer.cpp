#include "Render/Renderer.h"
#include "Render/Renderer2D.h"
#include "Render/Renderer3D.h"
#include "Render/TextureManager.h"
#include "Render/MaterialManager.h"

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
        std::vector<vk::SurfaceFormatKHR>{
            {vk::Format::eR8G8B8A8Srgb, vk::ColorSpaceKHR::eSrgbNonlinear},
            {vk::Format::eB8G8R8A8Srgb, vk::ColorSpaceKHR::eSrgbNonlinear},
        });

    // 3. 准备 RenderContext（内部创建 RenderFrames，启用深度缓冲）
    m_RenderContext->Prepare(1, true);

    // 4. 初始化纹理 / 材质管理器（在渲染器之前，渲染器可能依赖它们）
    auto &device    = m_VulkanContext->GetDevice();
    auto &resCache  = device.GetResourceCache();
    m_TextureManager = std::make_unique<TextureManager>(device, resCache);
    m_MaterialManager = std::make_unique<MaterialManager>(*m_TextureManager);

    // 5. 初始化 2D 精灵渲染器
    m_2DRenderer = std::make_unique<Renderer2D>();

    // 6. 初始化 3D 网格渲染器
    m_3DRenderer = std::make_unique<Renderer3D>();
}

Renderer::~Renderer() {
    GE_CORE_INFO("Renderer Shutdown");

    // 1. 等待 GPU 完成所有未完成的工作
    WaitIdle();

    // 2. 按构造逆序销毁
    m_3DRenderer.reset();
    m_2DRenderer.reset();
    m_MaterialManager.reset();
    m_TextureManager.reset();
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

    // 0. 重置每帧渲染统计
    m_Stats = {};

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

    // 3. 提交 + 结束帧（present + 清理）
    {
        ZoneScopedN("EndFrame");
        m_RenderContext->EndFrame(m_ActiveFrameCmd->GetHandle());
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

void Renderer::SetPresentMode(vk::PresentModeKHR present_mode) {
    if (!m_RenderContext) {
        return;
    }
    WaitIdle();
    m_RenderContext->UpdateSwapchain(present_mode);
    GE_CORE_INFO("Present mode changed");
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

Renderer3D &Renderer::Get3DRenderer() {
    GE_CORE_ASSERT(Get().m_3DRenderer, "Renderer3D not initialized!");
    return *Get().m_3DRenderer;
}

TextureManager &Renderer::GetTextureManager() {
    GE_CORE_ASSERT(Get().m_TextureManager, "TextureManager not initialized!");
    return *Get().m_TextureManager;
}

MaterialManager &Renderer::GetMaterialManager() {
    GE_CORE_ASSERT(Get().m_MaterialManager, "MaterialManager not initialized!");
    return *Get().m_MaterialManager;
}

const RendererStats &Renderer::GetStats() {
    return Get().m_Stats;
}

void Renderer::AddStats2D(uint32_t drawCalls, uint32_t triangles) {
    m_Stats.drawCalls2D += drawCalls;
    m_Stats.triangles2D += triangles;
}

void Renderer::AddStats3D(uint32_t drawCalls, uint32_t triangles) {
    m_Stats.drawCalls3D += drawCalls;
    m_Stats.triangles3D += triangles;
}

void Renderer::AddBatches3D(uint32_t batches) {
    m_Stats.batches3D += batches;
}

} // namespace GE
