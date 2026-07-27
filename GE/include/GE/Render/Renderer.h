#pragma once

#include "Core/Base.h"
#include "Render/VulkanBase/VulkanContext.h"
#include "Render/VulkanBase/VulkanRenderContext.h"
#include "Render/VulkanBase/VulkanCommandBuffer.h"

namespace GE {

class Window;

/**
 * @brief 渲染器：统一管理 Vulkan 资源和帧渲染流程。
 *
 * Renderer 是 Application 和渲染层之间的中间层，负责：
 * - 持有 VulkanContext（全局 Vulkan 运行时）
 * - 持有 VulkanRenderContext（帧管理器 / swapchain / RenderFrame 池）
 * - 管理当前帧 command buffer
 * - 每帧 Begin/End 流程（含 image layout 转换）
 * - swapchain 重建
 * - 提供静态访问方法供 Layer / ImGui 使用
 */
class Renderer {
public:
    /**
     * @brief 构造 Renderer，内部完成 VulkanContext → RenderContext → Prepare 完整初始化链。
     * @param window 主窗口引用（用于创建 surface 和 swapchain）
     */
    explicit Renderer(Window &window);

    ~Renderer();

    Renderer(const Renderer &) = delete;

    Renderer &operator=(const Renderer &) = delete;

    Renderer(Renderer &&) = delete;

    Renderer &operator=(Renderer &&) = delete;

    // ========================================================================
    // 帧循环接口（由 Application 主循环调用）
    // ========================================================================

    /// 开始一帧：acquire next image + begin command buffer + layout → ColorAttachmentOptimal。
    VulkanCommandBuffer &BeginFrame();

    /// 结束一帧：layout → PresentSrcKHR + end command buffer + submit + present。
    void EndFrame();

    // ========================================================================
    // Swapchain 管理
    // ========================================================================

    /**
     * @brief 重建 swapchain（窗口 resize 时调用）。
     * @param width  新宽度
     * @param height 新高度
     * @return 是否成功重建（宽高为 0 时返回 false）
     */
    bool RecreateSwapchain(uint32_t width, uint32_t height);

    /// 等待 GPU 空闲。
    void WaitIdle();

    // ========================================================================
    // 静态访问方法（供 Layer / ImGui / 外部使用）
    // ========================================================================

    /// 获取全局单例。
    static Renderer &Get();

    /// 访问 Vulkan 全局上下文。
    static VulkanContext &GetVulkanContext();

    /// 访问帧管理器。
    static VulkanRenderContext &GetRenderContext();

    /// 访问 swapchain（const 引用）。
    static const VulkanSwapchain &GetSwapchain();

    /// 当前帧的 command buffer。
    static VulkanCommandBuffer &GetFrameCmd();

    /// 当前帧的 image index。
    static uint32_t GetFrameImageIndex();

    /// 当前帧的 swapchain image view。
    static VulkanImageView &GetFrameImageView();

private:
    /// Vulkan 全局上下文（Instance / PhysicalDevice / Surface / Device / VMA）。
    std::unique_ptr<VulkanContext> m_VulkanContext;

    /// 帧管理器（swapchain / RenderFrame 池 / 提交呈现）。
    std::unique_ptr<VulkanRenderContext> m_RenderContext;

    /// 当前帧的 command buffer（每帧由 BeginFrame 设置，EndFrame 后重置）。
    std::shared_ptr<VulkanCommandBuffer> m_ActiveFrameCmd;

    /// 窗口引用。
    Window &m_Window;

    /// 静态单例。
    static Renderer *s_Instance;
};

} // namespace GE
