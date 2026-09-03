#include "Render/Renderer.h"
#include "Render/Renderer2D.h"
#include "Render/Renderer3D.h"
#include "Render/AssetManager.h"
#include "Render/TextureManager.h"
#include "Render/MaterialManager.h"
#include "Render/MeshManager.h"
#include "Render/ImGui/ImGuiLayer.h"

#include "Core/GEWindow.h"
#include "Core/Log.h"
#include "Debug/Assert.h"
#include "Render/VulkanBase/VulkanImage.h"
#include "Render/VulkanBase/VulkanRenderingInfo.h"
#include "Render/VulkanBase/VulkanRenderFrame.h"

#include "Debug/Profiler.h"

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

    // 4. 初始化统一资源管理器（在渲染器之前，渲染器可能依赖它）
    auto &device    = m_VulkanContext->GetDevice();
    auto &resCache  = device.GetResourceCache();

    // 4a. 创建异步上传管理器（后台线程解码 + GPU 上传，主线程每帧 Poll 回收）
    m_AsyncUpload = std::make_unique<AsyncUploadManager>(device);

    m_AssetManager = std::make_unique<AssetManager>(device, resCache, *m_AsyncUpload);

    // 5. 初始化 2D 精灵渲染器
    m_2DRenderer = std::make_unique<Renderer2D>();

    // 6. 初始化 3D 网格渲染器
    m_3DRenderer = std::make_unique<Renderer3D>();

    // 7. 初始化 ImGui（归并 Renderer）：底层 Vulkan / swapchain / AssetManager 均已就绪。
    //    失败不阻断渲染（无 UI 的应用仍可运行）。
    m_ImGuiLayer = std::make_unique<ImGuiLayer>(*this);
    m_ImGuiLayer->OnAttach();
}

Renderer::~Renderer() {
    GE_CORE_INFO("Renderer Shutdown");

    // 1. 等待 GPU 完成所有未完成的工作
    WaitIdle();

    // 1a. 先销毁 ImGui（其 DescriptorPool / backend 依赖 device，须先于 VulkanContext）
    if (m_ImGuiLayer) {
        m_ImGuiLayer->OnDetach(); // 关闭 ImGui Vulkan/GLFW backend 并销毁 context
        m_ImGuiLayer.reset();
    }

    // 2. 按构造逆序销毁
    m_3DRenderer.reset();
    m_2DRenderer.reset();
    m_AssetManager.reset();
    m_AsyncUpload.reset(); // 持有 device 引用，须在 VulkanContext 之前销毁
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
    GE_PROFILE_SCOPE("Renderer::BeginFrame");

    // 0. 重置每帧渲染统计
    m_Stats = {};

    // 0a. 回收已完成的后台上传（fence 置位的槽位），使后台解码/上传完成的资源本帧可见
    if (m_AsyncUpload) {
        m_AsyncUpload->Poll();
    }

    // 1. Acquire next image + 获取 command buffer
    m_ActiveFrameCmd = &m_RenderContext->Begin();

    // 2. Begin command buffer
    m_ActiveFrameCmd->Begin(vk::CommandBufferUsageFlagBits::eOneTimeSubmit);

    // 3. Transition to color attachment layout
    {
        GE_PROFILE_SCOPE("TransitionToColor");
        auto &swapchain = m_RenderContext->GetSwapchain();
        auto &img = swapchain.GetImages()[m_RenderContext->GetActiveFrameIndex()];
        image_utils::TransitionLayout(m_ActiveFrameCmd->GetHandle(), img.GetHandle(),
                                      vk::ImageLayout::eUndefined,
                                      vk::ImageLayout::eColorAttachmentOptimal);
    }

    // 4. 清屏到暗色背景（动态渲染）。
    //    场景现已离屏渲染（不再直接写 swapchain），这里为 ImGui 先清一个
    //    干净底色，避免残留/未定义内容。其它仍直接渲染到 swapchain 的层会覆盖它。
    {
        auto &frame     = m_RenderContext->GetActiveFrame();
        auto &swapchain = m_RenderContext->GetSwapchain();
        auto extent     = swapchain.GetExtent();

        vk::ClearValue clearValue;
        clearValue.color = std::array<float, 4>{0.1f, 0.1f, 0.15f, 1.0f};

        VulkanRenderingInfo renderInfo;
        renderInfo.SetRenderArea(0, 0, extent.width, extent.height);
        renderInfo.AddColorAttachment(
            frame.GetRenderTarget().GetSwapchainView().GetHandle(),
            vk::AttachmentLoadOp::eClear,
            vk::AttachmentStoreOp::eStore,
            clearValue);
        renderInfo.Begin(m_ActiveFrameCmd->GetHandle());
        VulkanRenderingInfo::End(m_ActiveFrameCmd->GetHandle());
    }

    return *m_ActiveFrameCmd;
}

void Renderer::EndFrame() {
    GE_PROFILE_SCOPE("Renderer::EndFrame");

    GE_CORE_ASSERT(m_ActiveFrameCmd, "No active frame command buffer!");

    // 0. ImGui 帧（归并后由 Renderer 驱动）：Begin → 各 Layer UI → 上屏。
    //    必须在 swapchain image 仍为 ColorAttachmentOptimal 时绘制，
    //    故置于 layout 转换到 Present 之前。
    if (m_ImGuiLayer) {
        GE_PROFILE_SCOPE("ImGuiRender");
        ImGuiLayer::Begin();
        // 各宿主 Layer 的 UI（含 DockSpace 宿主，须先建 DockSpace，故统计窗口在其后画）
        if (m_FrameUI) {
            m_FrameUI();
        }
        // Renderer 自带 UI（渲染统计面板）：在 DockSpace 建好后停靠并绘制
        m_ImGuiLayer->OnImGuiRender();
        m_ImGuiLayer->End();
    }

    // 1. Transition to present layout
    {
        GE_PROFILE_SCOPE("TransitionToPresent");
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
        GE_PROFILE_SCOPE("EndFrame");
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

    if (m_ImGuiLayer) {
        m_ImGuiLayer->OnSwapchainRecreated();
    }

    GE_CORE_INFO("Swapchain recreated: {}x{}", width, height);
    return true;
}

void Renderer::SetPresentMode(vk::PresentModeKHR present_mode) {
    if (!m_RenderContext) {
        return;
    }
    WaitIdle();
    m_RenderContext->UpdateSwapchain(present_mode);
    if (m_ImGuiLayer) {
        m_ImGuiLayer->OnSwapchainRecreated();
    }
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

AssetManager &Renderer::GetAssetManager() {
    GE_CORE_ASSERT(Get().m_AssetManager, "AssetManager not initialized!");
    return *Get().m_AssetManager;
}

AsyncUploadManager &Renderer::GetAsyncUploadManager() {
    GE_CORE_ASSERT(Get().m_AsyncUpload, "AsyncUploadManager not initialized!");
    return *Get().m_AsyncUpload;
}

TextureManager &Renderer::GetTextureManager() {
    return GetAssetManager().GetTextureManager();
}

MaterialManager &Renderer::GetMaterialManager() {
    return GetAssetManager().GetMaterialManager();
}

MeshManager &Renderer::GetMeshManager() {
    return GetAssetManager().GetMeshManager();
}

const RendererStats &Renderer::GetStats() {
    return Get().m_Stats;
}

void Renderer::SetFrameUI(std::function<void()> callback) {
    m_FrameUI = std::move(callback);
}

void Renderer::SetFrameInfo(float fps) {
    m_FPS = fps;
}

float Renderer::GetFPS() {
    return Get().m_FPS;
}

Window &Renderer::GetWindowRef() {
    return Get().m_Window;
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
