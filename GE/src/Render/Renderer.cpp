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
#include "Render/VulkanBase/VulkanRenderFrame.h"

#include "Debug/Profiler.h"

namespace GE {

Renderer *Renderer::s_Instance = nullptr;

Renderer::Renderer(Window &window, const std::filesystem::path &assetRoot)
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
    auto &device = m_VulkanContext->GetDevice();
    auto &resCache = device.GetResourceCache();

    // 4a. 渲染图虚拟资源池注入分配入口与帧在途数（GBuffer 等瞬态资源走池分配）。
    m_FrameGraph.SetDevice(&device);
    m_FrameGraph.SetFramesInFlight(
        static_cast<uint32_t>(m_RenderContext->GetRenderFrames().size()));

    // 4b. 创建异步上传管理器（后台线程解码 + GPU 上传，主线程每帧 Poll 回收）
    m_AsyncUpload = std::make_unique<AsyncUploadManager>(device);

    m_AssetManager = std::make_unique<AssetManager>(device, resCache, *m_AsyncUpload);

    // 4c. 资源根必须在任何资产消费者之前就位：下方 ImGui 初始化会解析字体路径。
    m_AssetManager->SetAssetRoot(assetRoot);

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
    m_FrameGraph.Shutdown();    // 释放虚拟资源池（VMA 图像，须先于 VulkanContext/device 销毁）
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

    // 0a. 回收已完成的后台上传（fence 置位的槽位），使后台解码/上传完成的资源本帧可见
    if (m_AsyncUpload) {
        m_AsyncUpload->Poll();
    }

    // 1. Acquire next image + 获取 command buffer
    m_ActiveFrameCmd = &m_RenderContext->Begin();

    // 2. Begin command buffer
    m_ActiveFrameCmd->Begin(vk::CommandBufferUsageFlagBits::eOneTimeSubmit);

    // 3. 清空本帧渲染图（供各 Layer 在 OnUpdate 里重新构建 Scene pass 声明）。
    // Execute 统一延后到 EndFrame（ImGui 上屏前）。
    m_FrameGraph.Reset();

    // 3a. Import 一次 swapchain 颜色视图并缓存句柄：EndFrame 的 UIPass 与运行时的
    //     场景 pass（SetSceneToBackbuffer）共用同一个资源记录。若两处各自 Import，
    //     同一张图会有两条布局记录，后者会按帧首布局（Undefined）对一个已处于
    //     ColorAttachmentOptimal 的图插屏障 → 非法旧布局。
    m_FrameColorHandle = m_FrameBuilder.Import(&GetFrameImageView(), "Swapchain");

    return *m_ActiveFrameCmd;
}

void Renderer::EndFrame() {
    GE_PROFILE_SCOPE("Renderer::EndFrame");

    GE_CORE_ASSERT(m_ActiveFrameCmd, "No active frame command buffer!");

    auto &activeFrame = m_RenderContext->GetActiveFrame();

    // ── 恒为帧图路径：ImGui 作为帧图最后一张 UIPass ──────────────────
    // 1. UI 内容构建（纯 CPU）：Begin → 各 Layer 提交 ImGui 命令(含视口图 Image)
    //    → EndUI 结束 CPU 侧帧数据。命令录制延后到 UIPass execute 回调，
    //    故必须在 Execute 之前完成本段。
    if (m_ImGuiLayer) {
        GE_PROFILE_SCOPE("ImGuiBuild");
        ImGuiLayer::Begin();
        if (m_FrameUI) {
            m_FrameUI();
        }
        m_ImGuiLayer->EndUI();
    }

    // 2. 注册 UIPass：颜色附件 = swapchain 当前帧 view（BeginFrame 已 Import 并缓存句柄）。
    //    默认 eClear 承接清屏职责，为 UI 画一个干净暗底；运行时（场景直画背缓冲）改
    //    eLoad，保留场景结果。execute 内裸录 RenderDrawData，收尾 finalLayout 转 PresentSrc。
    //    无条件注册，保证帧图恒有内容。
    auto &b = GetFrameGraphBuilder();
    ResourceHandle hSwapchain = m_FrameColorHandle;
    m_FrameGraph.SetFrameSwapchain(hSwapchain);
    const auto extent = m_RenderContext->GetSwapchain().GetExtent();
    RenderPassDesc &uiPass = b.AddPass("UIPass");
    uiPass.renderArea = vk::Rect2D{{0, 0}, {extent.width, extent.height}};
    AttachmentDesc color;
    color.resource = hSwapchain;
    color.usage = ResourceUsage::ColorAttachment;
    color.loadOp = m_SceneToBackbuffer ? vk::AttachmentLoadOp::eLoad
                                       : vk::AttachmentLoadOp::eClear;
    color.storeOp = vk::AttachmentStoreOp::eStore;
    color.clearValue.color = {0.1f, 0.1f, 0.15f, 1.0f};
    color.finalLayout = vk::ImageLayout::ePresentSrcKHR;
    uiPass.colorAttachments.push_back(color);
    uiPass.execute = [](PassExecuteContext &ctx) {
        ImGuiLayer::DrawUI(*ctx.cmd);
    };
    m_Stats = {};
    // 3. 统一 Execute：声明序即执行序（Scene3D → Scene2D → UIPass）。Scene2D 收尾
    //    把离屏视口图转 ShaderReadOnly，UIPass 采样之并画 UI 到 swapchain，最后
    //    由 UIPass finalLayout 收尾转 PresentSrc（不再有手工 →PresentSrc 段）。
    // 埋点单独立块：Tracy 的 ZoneScopedN 在所在作用域声明固定名局部变量
    // （___tracy_scoped_zone），本函数开头已有一个 GE_PROFILE_SCOPE，同作用域
    // 再展开一次即为重定义（C2374）。花括号限定作用域同时也让计时范围恰好落在
    // Compile + Execute 上。
    {
        GE_PROFILE_SCOPE("FrameGraphExecute");
        m_FrameGraph.Compile();
        m_FrameGraph.Execute(*m_ActiveFrameCmd, activeFrame);
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

VulkanImageView &Renderer::GetFrameDepthView() {
    return GetRenderContext().GetActiveFrame().GetRenderTarget().GetDepthView();
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
