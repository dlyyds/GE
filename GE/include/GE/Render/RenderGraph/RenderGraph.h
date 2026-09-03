/**
 * @file RenderGraph.h
 * @brief 渲染图（RenderGraph）核心：声明式 Pass 自动依赖 / 排序 / 屏障。
 *
 * 理念：各 Pass 只声明「读哪些资源、写哪些资源」，RenderGraph 自动推导依赖
 * 与执行顺序，并自动插入全部图像布局转换与内存屏障。
 *
 * v1（阶段1）范围：
 *   - 仅 Raster pass（动态渲染 beginRendering/endRendering）
 *   - 同步用经典 vk::ImageMemoryBarrier（对齐全引擎现有 image_layout_transition）
 *   - 图按帧实例化、帧末析构；跨帧布局记忆由 ImageViewResource 状态机承载
 *
 * 用法（计划书 §6A.6 接线示意）：
 * @code
 *   RenderGraph graph("FrameGraph");
 *   RenderGraphBuilder builder(graph);
 *   // 每帧：导入外部图 → AddPass 声明读写 → Compile() → Execute(cmd, frame)
 * @endcode
 */

#pragma once

#include "Render/RenderGraph/ImageViewResource.h"
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
class VulkanRenderFrame;

// ============================================================================
// RenderGraph — 图本体
// ============================================================================

/// 渲染图：节点（pass）+ 资源表 + 依赖边 + 每资源布局状态机。
/// 不拥有任何 GPU 资源；只编排同步与执行序。
class RenderGraph {
public:
    /// @param name 图名（调试/日志）
    explicit RenderGraph(std::string name);
    ~RenderGraph() = default;

    RenderGraph(const RenderGraph &) = delete;
    RenderGraph &operator=(const RenderGraph &) = delete;

    // ========================================================================
    // 资源与 Pass 注册（Builder 门面调用，或直接调用）
    // ========================================================================

    /// 导入一张外部图像，登记进资源表。返回资源句柄。
    /// @param res 调用方持有的 ImageViewResource（裸指针，不拥有；要求其生命周期 ≥ 本次执行）
    ResourceHandle Import(ImageViewResource *res);

    /// 创建帧内虚拟图像资源（v1 仅登记描述，不分配 GPU 内存）。返回句柄。
    ResourceHandle CreateVirtualResource(const RenderGraphResourceDesc &desc);

    /// 追加一个 Pass 声明。返回其下标。
    uint32_t AddPass(RenderPassDesc &&desc);

    // ========================================================================
    // 帧 WSI 图登记（收尾布局管理）
    // ========================================================================

    /// 标记某外部资源为本帧 WSI swapchain 图。执行结束后其布局不写回
    /// ImageViewResource（WSI 图每帧由 acquire 决定、跨帧记忆无意义，且可能已随
    /// 重建失效）。参见计划书 §4.6 的取舍。
    void SetFrameSwapchain(ResourceHandle handle);

    // ========================================================================
    // 编译与执行
    // ========================================================================

    /// 编译：依赖建图 → 稳定拓扑排序 → 检测环。编译前读写声明必须已完成。
    /// 可重复调用。
    bool Compile();

    /// 按编译得到的执行序，把屏障与各 pass 的命令录制到 cmd（本帧已 Begin）。
    /// @param cmd   目标 command buffer（当前帧）
    /// @param frame 当前帧（帧池缓冲/描述符来源，透传给 execute 回调）
    void Execute(VulkanCommandBuffer &cmd, VulkanRenderFrame &frame);

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
        ImageViewResource *external = nullptr;             ///< 外部导入时非空
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

    // --- 编译内部子步骤 ---
    void BuildDependencyEdges();     ///< §5.1：由读写声明建依赖边
    void TopologicalSort();          ///< §5.2：稳定拓扑排序（Kahn）

    /// 从 pass 声明中查某资源的「写用法」（用于推导跨 pass 屏障的源侧掩码）。
    /// 找不到时返回 ColorAttachment（理论上每个被依赖的资源都来自某写者，不会缺失）。
    ResourceUsage FindUsageInPass(uint32_t passIndex, ResourceHandle handle) const;

    // --- 成员 ---
    std::string m_Name;
    bool m_Compiled = false;

    std::vector<ResourceRecord> m_Resources;   ///< 资源表（句柄 = 下标 + 1）
    std::vector<RenderPassDesc> m_Passes;      ///< 节点（按声明序）
    std::vector<GraphEdge>      m_Edges;       ///< 依赖边（§5.1 产出）
    std::vector<uint32_t>       m_ExecutionOrder;  ///< 拓扑序（pass 下标）

    /// pass 下标 → 该 pass 访问的资源集（{句柄, 用法, 是否写}），Compile 时构建，
    /// Execute 用它计算每个资源在 pass 内的期望布局与首访转换。
    struct Access {
        ResourceUsage usage = ResourceUsage::ShaderRead;
        bool write = false;
    };
    std::vector<std::vector<std::pair<ResourceHandle, Access>>> m_PassAccess;
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

    /// 导入外部图像（转发到 RenderGraph::Import）。
    ResourceHandle Import(ImageViewResource *res) { return m_Graph.Import(res); }

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
