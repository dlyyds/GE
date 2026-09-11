#pragma once

#include "Core/Base.h"
#include "Render/VulkanBase/VulkanContext.h"
#include "Render/VulkanBase/VulkanRenderContext.h"
#include "Render/VulkanBase/VulkanCommandBuffer.h"
#include "Render/AsyncUploadManager.h"
#include "Render/RenderGraph/RenderGraph.h"

#include <filesystem>
#include <functional>

namespace GE {

class Window;
class Renderer2D;
class Renderer3D;
class AssetManager;
class TextureManager;
class MaterialManager;
class MeshManager;
class ImGuiLayer;

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
     * @param assetRoot 资源根目录。构造尾部会初始化 ImGui 并加载字体（需解析资源路径），
     *                  故资源根必须在构造时就位，不能构造后再 SetAssetRoot。
     */
    explicit Renderer(Window &window, const std::filesystem::path &assetRoot);

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
    // 场景直画背缓冲（运行时播放器用；编辑器不用）
    // ========================================================================

    /**
     * @brief 场景 pass 是否直接渲染到 swapchain 背缓冲。
     *
     * 默认 false（编辑器）：帧图末尾的 UIPass 用 eClear 清屏，场景画在离屏视口，
     * 由 UIPass 采样视口图并叠 UI。
     *
     * 置 true（运行时，无编辑器 UI）：场景 pass 直接写背缓冲，UIPass 改用 eLoad
     * 保留场景结果（仅在其上叠 ImGui 内容，通常为空）。须在首帧 EndFrame 之前设置，
     * 与 GetFrameSwapchainHandle() 配套使用。
     */
    void SetSceneToBackbuffer(bool enabled) { m_SceneToBackbuffer = enabled; }

    /// @return 本帧 swapchain 颜色附件资源句柄（BeginFrame 时 Import 并缓存）。
    /// 需要把场景画进背缓冲的宿主（运行时 GameLayer）应复用它，而非自行再 Import
    /// 一次同一张视图——否则同一图像会有两个资源记录各自维护布局，产生非法旧布局屏障。
    [[nodiscard]] ResourceHandle GetFrameSwapchainHandle() const { return m_FrameColorHandle; }

    /// 本帧 swapchain 渲染目标的深度附件视图（随 swapchain 重建）。
    /// 帧渲染目标以 enable_depth=true 创建，深度图 usage 含 eSampled，当附件或采样源皆可。
    static VulkanImageView &GetFrameDepthView();

    // ========================================================================
    // ImGui / UI 调度（Renderer 内部集成）
    // ========================================================================

    /**
     * @brief 注入每帧 UI 提交回调（由宿主在构造后调用）。
     *
     * 归并 ImGui 后 Application 不再遍历 LayerStack 驱动 UI；改为把
     * “遍历各 Layer 的 OnImGuiRender” 封装成回调注入 Renderer，
     * 由 EndFrame 在 ImGui Begin 之后、上屏之前调用。
     *
     * @param callback 每帧 UI 提交函数（通常在 ImGui::NewFrame 之后执行窗口绘制）
     */
    void SetFrameUI(std::function<void()> callback);

    // ========================================================================
    // 帧渲染图（RenderGraph）编排
    // ========================================================================

    /**
     * @brief 获取本帧的渲染图构建器（宿主每帧在 OnUpdate 里向它注册 pass）。
     *
     * Renderer 持有帧图对象，每帧 BeginFrame 末尾 Reset；各 Layer 在 OnUpdate
     * 中经此 builder Import 外部图像 / AddPass 声明读写，命令录制延后到
     * Renderer::EndFrame 里统一 Execute。编辑器的离屏视口 Scene3D/Scene2D 两
     * pass 由此注册；EndFrame 恒追加 UIPass（写 swapchain），故帧图恒非空。
     */
    RenderGraphBuilder &GetFrameGraphBuilder() { return m_FrameBuilder; }

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

    /// 访问主窗口（ImGuiLayer 初始化/上屏需取 GLFW window 与尺寸）。
    /// 常规业务方优先走 Window 引用，勿经此访问。
    [[nodiscard]] static Window &GetWindowRef();

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

    /// 访问资源异步上传管理器（后台线程解码上传，主线程每帧 Poll 回收）。
    static AsyncUploadManager &GetAsyncUploadManager();

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
    friend class ImGuiLayer;   ///< ImGuiLayer 需访问窗口与帧资源（内部组件，不设公共转发）

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

    /// 本帧 swapchain 颜色附件的资源句柄（BeginFrame 里 Import 一次，EndFrame 复用）。
    /// 缓存而非每处各自 Import：同一张视图只能有一个资源记录，否则布局跟踪会打架。
    ResourceHandle m_FrameColorHandle = kInvalidResource;

    /// 场景是否直画背缓冲（见 SetSceneToBackbuffer）。false = 编辑器行为（UIPass 清屏）。
    bool m_SceneToBackbuffer = false;

    /// 帧渲染图对象 + 构建器（每帧 BeginFrame 末尾 Reset，EndFrame Execute）。
    /// 宿主（各 Layer）在 OnUpdate 经 GetFrameGraphBuilder() 注册 pass。
    RenderGraph m_FrameGraph{"FrameGraph"};
    RenderGraphBuilder m_FrameBuilder{m_FrameGraph};

    /// 2D 精灵渲染器。
    std::unique_ptr<Renderer2D> m_2DRenderer;

    /// 3D 网格渲染器。
    std::unique_ptr<Renderer3D> m_3DRenderer;

    /// 全局资源管理器（持有纹理 / 材质 / 网格子管理器 + 资源根路径）。
    std::unique_ptr<AssetManager> m_AssetManager;

    /// 资源异步上传管理器（后台线程解码+上传，主线程每帧 BeginFrame 调 Poll 回收）。
    std::unique_ptr<AsyncUploadManager> m_AsyncUpload;

    /// 窗口引用。
    Window &m_Window;

    // -- ImGui 集成（归并后由 Renderer 持有并驱动）--

    /// ImGui 运行时组件（Vulkan/GLFW backend + UI 上下文）。
    std::unique_ptr<ImGuiLayer> m_ImGuiLayer;

    /// 每帧 UI 提交回调（宿主注入：遍历各 Layer 的 OnImGuiRender）。
    std::function<void()> m_FrameUI;

    /// 本帧渲染统计（每帧 BeginFrame 重置）。
    RendererStats m_Stats;

    /// 静态单例。
    static Renderer *s_Instance;
};

} // namespace GE
