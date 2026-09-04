/**
 * @file RenderGraph.h
 * @brief 渲染图（RenderGraph）核心：声明式 Pass 顺序执行 + 自动屏障。
 *
 * 理念：各 Pass 只声明「读哪些资源、写哪些资源」，RenderGraph 按声明序执行
 * （声明序即执行序），并自动插入全部图像布局转换与内存屏障。
 *
 * v1（阶段1）范围：
 *   - 仅 Raster pass（动态渲染 beginRendering/endRendering）
 *   - 同步用 vk::ImageMemoryBarrier2（pipelineBarrier2）
 *   - 图按帧实例化、帧末析构；跨帧布局记忆由 VulkanImage 的 layout 字段承载
 *
 * 用法（计划书 §6A.6 接线示意）：
 * @code
 *   RenderGraph graph("FrameGraph");
 *   RenderGraphBuilder builder(graph);
 *   // 每帧：导入外部图 → AddPass 声明读写 → Compile() → Execute(cmd, frame)
 * @endcode
 */

#pragma once

#include "Render/RenderGraph/RenderPassDesc.h"
#include "Render/RenderGraph/RenderGraphTypes.h"

#include <vulkan/vulkan.hpp>

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace GE {

class VulkanCommandBuffer;
class VulkanImageView;
class VulkanRenderFrame;
class VulkanDevice;
class VulkanImage;

// ============================================================================
// RenderGraph — 图本体
// ============================================================================

/// 渲染图：pass 序列 + 资源表。
/// 不拥有任何 GPU 资源；只编排同步。
/// 声明序即执行序：pass 必须按依赖序声明（读某资源须声明在其写者之后），
/// 框架不自动重排、也不做环检测。依赖建图与拓扑排序已移除（v2 保留声明）。
class RenderGraph {
public:
    /// @param name 图名（调试/日志）
    explicit RenderGraph(std::string name);
    ~RenderGraph();  // 定义于 cpp：虚拟资源池含 VulkanImage/View 不完整类型

    RenderGraph(const RenderGraph &) = delete;
    RenderGraph &operator=(const RenderGraph &) = delete;

    // ========================================================================
    // 资源与 Pass 注册（Builder 门面调用，或直接调用）
    // ========================================================================

    /// 导入一张外部图像视图，登记进资源表。返回资源句柄。
    /// @param view 调用方持有的 VulkanImageView（裸指针，不拥有；要求其生命周期 ≥ 本次执行）。
    ///            布局跨帧记忆经 view->get_image() 读写在其 VulkanImage 上。
    /// @param name 可选资源名（调试）；缺省时回退到图像的 debug name / "external"。
    ResourceHandle Import(VulkanImageView *view, const std::string &name = {});

    /// 创建帧内虚拟图像资源（v1 仅登记描述，不分配 GPU 内存）。返回句柄。
    ResourceHandle CreateVirtualResource(const RenderGraphResourceDesc &desc);

    /// 追加一个 Pass 声明。返回其下标。
    uint32_t AddPass(RenderPassDesc &&desc);

    // ========================================================================
    // 帧 WSI 图登记（收尾布局管理）
    // ========================================================================

    /// 标记某外部资源为本帧 WSI swapchain 图。执行结束后其布局不写回
    /// VulkanImage（WSI 图每帧由 acquire 决定、跨帧记忆无意义，且可能已随
    /// 重建失效）。参见计划书 §4.6 的取舍。
    void SetFrameSwapchain(ResourceHandle handle);

    // ========================================================================
    // 编译与执行
    // ========================================================================

    /// 校验 pass 声明自洽。编译前读写声明必须已完成。可重复调用。
    bool Compile();

    /// 清空本帧全部资源与 pass 声明，恢复到可重新构建的初始态。
    /// 供宿主（Renderer）每帧复用同一图对象（每帧重建的替代，避免重复分配）。
    void Reset();

    /// 按声明序把屏障与各 pass 的命令录制到 cmd（本帧已 Begin）。
    /// @param cmd   目标 command buffer（当前帧）
    /// @param frame 当前帧（帧池缓冲/描述符来源，透传给 execute 回调）
    void Execute(VulkanCommandBuffer &cmd, VulkanRenderFrame &frame);

    // ========================================================================
    // 虚拟资源池（阶段2 基建；见《渲染图虚拟资源池计划书》）
    // ========================================================================

    /// 注入池分配所需 device（Renderer 构造时调用；首次 Execute 前必须已设）。
    void SetDevice(VulkanDevice *device);

    /// 设置帧在途数（= swapchain 图像数），用于虚拟资源 LRU 淘汰的安全销毁判定。
    void SetFramesInFlight(uint32_t framesInFlight);

    /// 释放全部虚拟资源池条目（Renderer 析构体里、device 销毁前显式调用）。
    void Shutdown();

    // ========================================================================
    // 只读访问（调试 / 日志 / 断言）
    // ========================================================================

    const std::string &GetName() const { return m_Name; }
    bool IsCompiled() const { return m_Compiled; }
    size_t GetPassCount() const { return m_Passes.size(); }

    /// 资源表行：每行对应一个资源（外部或虚拟）。
    struct ResourceRecord {
        std::string name;                                  ///< 资源名（调试）
        ResourceType type = ResourceType::Image;           ///< v1 恒 Image
        VulkanImageView *external = nullptr;               ///< 外部导入的图 view（经其 image 读写布局记忆）
        VulkanImageView *pooledView = nullptr;             ///< 虚拟资源本帧解析出的池视图（external 路径为 null）
        RenderGraphResourceDesc virtualDesc{};             ///< 虚拟资源描述（v1 不分配）
        bool isFrameSwapchain = false;                     ///< 是否本帧 WSI 图
    };

    /// 获取资源表（调试/访问）。
    const std::vector<ResourceRecord> &GetResources() const { return m_Resources; }

    /// 按句柄查资源记录（越界/无效返回 nullptr）。
    ResourceRecord *FindResource(ResourceHandle handle);
    const ResourceRecord *FindResource(ResourceHandle handle) const;

private:
    friend class RenderGraphBuilder;

    /// 从 pass 声明中查某资源的「写用法」（用于推导跨 pass 屏障的源侧掩码）。
    /// 找不到时返回 ColorAttachment（理论上每个被依赖的资源都来自某写者，不会缺失）。
    ResourceUsage FindUsageInPass(uint32_t passIndex, ResourceHandle handle) const;

    /// 帧内虚拟资源池条目（跨 Reset 存活；LRU 上限淘汰，见计划书 §3）。
    /// 须先于 AcquirePoolEntry 声明：成员函数签名引用嵌套类型需先声明。
    struct PooledImage {
        RenderGraphResourceDesc desc;                ///< 匹配键：format/extent/samples
        vk::ImageUsageFlags      usage = {};         ///< 匹配键：pass 声明推导的用法（防跨帧漂移）
        std::unique_ptr<VulkanImage>     image;      ///< VMA RAII 图像（含跨帧 layout）
        std::unique_ptr<VulkanImageView> view;       ///< 供 MRT / 采样的视图
        bool busy = false;                           ///< 本帧已被某句柄占用
        uint32_t lastUsedFrameIndex = 0;             ///< LRU 淘汰（当前帧号 - lastUsed > framesInFlight 才可销毁）
    };

    /// 解析本帧全部虚拟资源：推导 usage、从池分配/复用视图。Execute 开头调用。
    void ResolveVirtualResources();

    /// 从池取一个匹配 (desc, usage) 的空闲条目（无则新建）。返回条目指针。
    PooledImage *AcquirePoolEntry(const RenderGraphResourceDesc &desc, vk::ImageUsageFlags usage);

    /// LRU 上限淘汰：超限时淘汰最久未用且无在途引用（可安全销毁）的空闲条目。
    void EvictPoolEntries();

    // --- 成员 ---
    std::string m_Name;
    bool m_Compiled = false;

    std::vector<ResourceRecord> m_Resources;   ///< 资源表（句柄 = 下标 + 1）
    std::vector<RenderPassDesc> m_Passes;      ///< 节点（按声明序）

    VulkanDevice *m_Device = nullptr;          ///< 池分配入口（SetDevice 注入，首次 Execute 前必须已设）
    uint32_t m_FramesInFlight = 1;             ///< 帧在途数（LRU 淘汰安全阈值，默认单帧保守）
    uint32_t m_FrameIndex = 0;                 ///< 单调帧计数（Execute 递增，作 LRU 时间戳）

    std::vector<PooledImage> m_VirtualPool;          ///< 虚拟资源池（跨 Reset 存活）
};

// ============================================================================
// RenderGraphBuilder — 构建门面
// ============================================================================

/// 向图追加资源与 Pass 的一层薄封装；生命周期与 RenderGraph 绑定，
/// 使用方每帧用它把「导入 / AddPass」拼装好，最后 graph.Compile()/Execute()。
class RenderGraphBuilder {
public:
    /// @param graph 目标图（要求存活至 Execute 结束）
    explicit RenderGraphBuilder(RenderGraph &graph) : m_Graph(graph) {}

    /// 导入外部图像视图（转发到 RenderGraph::Import）。
    ResourceHandle Import(VulkanImageView *view, const std::string &name = {}) {
        return m_Graph.Import(view, name);
    }

    /// 创建虚拟图像资源（转发）。
    ResourceHandle CreateVirtualResource(const RenderGraphResourceDesc &desc) {
        return m_Graph.CreateVirtualResource(desc);
    }

    /// 新建 pass 并返回引用供填充。
    RenderPassDesc &AddPass(const std::string &name, PassType type = PassType::Raster) {
        RenderPassDesc desc;
        desc.name = name;
        desc.type = type;
        m_Graph.m_Passes.emplace_back(std::move(desc));
        return m_Graph.m_Passes.back();
    }

private:
    RenderGraph &m_Graph;
};

} // namespace GE
