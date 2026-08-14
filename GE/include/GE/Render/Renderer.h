#pragma once

#include "Core/Base.h"
#include "Render/VulkanBase/VulkanContext.h"
#include "Render/VulkanBase/VulkanRenderContext.h"
#include "Render/VulkanBase/VulkanCommandBuffer.h"

namespace GE {

class Window;
class Renderer2D;
class Renderer3D;
class AssetManager;
class TextureManager;
class MaterialManager;
class MeshManager;

/**
 * @brief 渲染统计（每帧由 BeginFrame 重置），2D / 3D 分开统计。
 */
struct RendererStats {
    uint32_t drawCalls2D = 0; ///< 本帧 2D 批处理提交的 draw call 数量
    uint32_t triangles2D = 0; ///< 本帧 2D 绘制三角形数量（近似：顶点数 / 3）
    uint32_t drawCalls3D = 0; ///< 本帧 3D 网格提交的 draw call 数量
    uint32_t triangles3D = 0; ///< 本帧 3D 绘制三角形数量（近似：索引数 / 3）
    uint32_t batches3D   = 0; ///< 本帧 3D 排序后的批次数（同材质连续分组，用于观察合批收益）

    /// 本帧 draw call 总数。
    uint32_t TotalDrawCalls() const { return drawCalls2D + drawCalls3D; }

    /// 本帧三角形总数。
    uint32_t TotalTriangles() const { return triangles2D + triangles3D; }
};

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

    /**
     * @brief 设置呈现模式（运行时切换垂直同步）。
     * @param present_mode  目标呈现模式
     */
    void SetPresentMode(vk::PresentModeKHR present_mode);

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

    /// 访问 2D 精灵渲染器。
    static Renderer2D &Get2DRenderer();

    /// 访问 3D 网格渲染器。
    static Renderer3D &Get3DRenderer();

    /// 访问统一资源管理器。
    static AssetManager &GetAssetManager();

    /// 访问纹理管理器。
    static TextureManager &GetTextureManager();

    /// 访问材质管理器。
    static MaterialManager &GetMaterialManager();

    /// 访问网格管理器。
    static MeshManager &GetMeshManager();

    /// 获取本帧渲染统计（draw call / 三角形数量）。
    static const RendererStats &GetStats();

private:
    friend class Renderer2D;
    friend class Renderer3D;

    /// 记录 2D 批量绘制产生的 draw call 与三角形数量（供 Renderer2D 调用）。
    void AddStats2D(uint32_t drawCalls, uint32_t triangles);

    /// 记录 3D 网格绘制产生的 draw call 与三角形数量（供 Renderer3D 调用）。
    void AddStats3D(uint32_t drawCalls, uint32_t triangles);
    void AddBatches3D(uint32_t batches);
    /// Vulkan 全局上下文（Instance / PhysicalDevice / Surface / Device / VMA）。
    std::unique_ptr<VulkanContext> m_VulkanContext;

    /// 帧管理器（swapchain / RenderFrame 池 / 提交呈现）。
    std::unique_ptr<VulkanRenderContext> m_RenderContext;

    /// 当前帧的 command buffer（每帧由 BeginFrame 设置，EndFrame 后置空；由 RenderContext 所有）。
    VulkanCommandBuffer *m_ActiveFrameCmd = nullptr;

    /// 2D 精灵渲染器。
    std::unique_ptr<Renderer2D> m_2DRenderer;

    /// 3D 网格渲染器。
    std::unique_ptr<Renderer3D> m_3DRenderer;

    /// 全局资源管理器（持有纹理 / 材质 / 网格子管理器 + 资源根路径）。
    std::unique_ptr<AssetManager> m_AssetManager;

    /// 窗口引用。
    Window &m_Window;

    /// 本帧渲染统计（每帧 BeginFrame 重置）。
    RendererStats m_Stats;

    /// 静态单例。
    static Renderer *s_Instance;
};

} // namespace GE
