/**
 * @file RenderGraph.cpp
 * @brief 渲染图实现：依赖建图 → 稳定拓扑排序 → 逐 pass 前置屏障。
 *
 * 对应计划书 §5.1–§5.4。v1 采用经典 vk::ImageMemoryBarrier；同步2 化留待阶段2。
 *
 * 屏障策略（阶段1）：Execute 按执行序逐 pass 推进，为每个 pass 的跨 pass 资源
 * 访问生成前置屏障。资源布局状态由帧首记忆/初始状态出发随执行序推进。
 */

#include "Render/RenderGraph/RenderGraph.h"

#include "Core/Base.h"
#include "Core/Log.h"
#include "Debug/Assert.h"
#include "Render/VulkanBase/VulkanCommandBuffer.h"
#include "Render/VulkanBase/VulkanImage.h"
#include "Render/VulkanBase/VulkanRenderFrame.h"
#include "Render/VulkanBase/VulkanRenderingInfo.h"

#include <algorithm>
#include <map>
#include <queue>

namespace GE {

// ============================================================================
// 资源用法 → 阶段/访问/布局映射（计划书 §7 资源用法表）
// ============================================================================

namespace {

/// 「写」用法对应的写阶段（写入该资源的访问发生在哪条管线阶段）。
vk::PipelineStageFlags WriteStage(ResourceUsage usage) {
    switch (usage) {
    case ResourceUsage::ColorAttachment:
        return vk::PipelineStageFlagBits::eColorAttachmentOutput;
    case ResourceUsage::DepthStencilAttachment:
        return vk::PipelineStageFlagBits::eEarlyFragmentTests | vk::PipelineStageFlagBits::eLateFragmentTests;
    case ResourceUsage::ShaderWrite:
        return vk::PipelineStageFlagBits::eComputeShader;
    case ResourceUsage::TransferDst:
        return vk::PipelineStageFlagBits::eTransfer;
    default:
        return {};  // 读用法无写阶段
    }
}

/// 「读」用法对应的读阶段（读取该资源的访问发生在哪条管线阶段）。
vk::PipelineStageFlags ReadStage(ResourceUsage usage) {
    switch (usage) {
    case ResourceUsage::ColorAttachment:
        return vk::PipelineStageFlagBits::eColorAttachmentOutput;
    case ResourceUsage::DepthStencilAttachment:
        return vk::PipelineStageFlagBits::eEarlyFragmentTests | vk::PipelineStageFlagBits::eLateFragmentTests;
    case ResourceUsage::ShaderRead:
        // v1 覆盖顶点/片元采样（计算留阶段2）
        return vk::PipelineStageFlagBits::eVertexShader | vk::PipelineStageFlagBits::eFragmentShader;
    case ResourceUsage::ShaderWrite:
        return vk::PipelineStageFlagBits::eComputeShader;
    case ResourceUsage::TransferDst:
        return vk::PipelineStageFlagBits::eTransfer;
    }
    return {};
}

/// 用法对应的「写访问」位。
vk::AccessFlags WriteAccess(ResourceUsage usage) {
    switch (usage) {
    case ResourceUsage::ColorAttachment:
        return vk::AccessFlagBits::eColorAttachmentWrite;
    case ResourceUsage::DepthStencilAttachment:
        return vk::AccessFlagBits::eDepthStencilAttachmentWrite;
    case ResourceUsage::ShaderWrite:
        return vk::AccessFlagBits::eShaderWrite;
    case ResourceUsage::TransferDst:
        return vk::AccessFlagBits::eTransferWrite;
    default:
        return {};
    }
}

/// 用法对应的「读访问」位。
vk::AccessFlags ReadAccess(ResourceUsage usage) {
    switch (usage) {
    case ResourceUsage::ColorAttachment:
        return vk::AccessFlagBits::eColorAttachmentRead;
    case ResourceUsage::DepthStencilAttachment:
        return vk::AccessFlagBits::eDepthStencilAttachmentRead;
    case ResourceUsage::ShaderRead:
        return vk::AccessFlagBits::eShaderRead;
    case ResourceUsage::ShaderWrite:
        return {};
    case ResourceUsage::TransferDst:
        return vk::AccessFlagBits::eTransferRead;
    }
    return {};
}

/// 「写」用法对应的布局（写后停靠布局）。
vk::ImageLayout LayoutForWrite(ResourceUsage usage) {
    switch (usage) {
    case ResourceUsage::ColorAttachment:
        return vk::ImageLayout::eColorAttachmentOptimal;
    case ResourceUsage::DepthStencilAttachment:
        return vk::ImageLayout::eDepthStencilAttachmentOptimal;
    case ResourceUsage::ShaderWrite:
        return vk::ImageLayout::eGeneral;
    case ResourceUsage::TransferDst:
        return vk::ImageLayout::eTransferDstOptimal;
    default:
        return vk::ImageLayout::eUndefined;
    }
}

/// 「读」用法期望的布局；未知（0）表示沿用当前布局（如附件读）。
vk::ImageLayout LayoutForRead(ResourceUsage usage) {
    switch (usage) {
    case ResourceUsage::ShaderRead:
        return vk::ImageLayout::eShaderReadOnlyOptimal;
    default:
        return vk::ImageLayout::eUndefined;
    }
}

/// 用法是否为「写」侧（写者产生依赖源）。
bool IsWriteUsage(ResourceUsage usage) {
    switch (usage) {
    case ResourceUsage::ColorAttachment:
    case ResourceUsage::DepthStencilAttachment:
    case ResourceUsage::ShaderWrite:
    case ResourceUsage::TransferDst:
        return true;
    default:
        return false;
    }
}

/// 构造一条经典图像内存屏障（阶段掩码不在此结构内，由 vkCmdPipelineBarrier 单独传入）。
vk::ImageMemoryBarrier MakeBarrier(vk::AccessFlags srcAccess,
                                   vk::AccessFlags dstAccess,
                                   vk::ImageLayout oldLayout,
                                   vk::ImageLayout newLayout,
                                   const VulkanImageView &view) {
    vk::ImageMemoryBarrier b;
    b.srcAccessMask = srcAccess;
    b.dstAccessMask = dstAccess;
    b.oldLayout = oldLayout;
    b.newLayout = newLayout;
    b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = view.get_image().GetHandle();
    // 复用 view 已推断的 aspect（VulkanImageView.cpp 按格式正确设置 color/depth/stencil）
    b.subresourceRange = vk::ImageSubresourceRange{
        view.get_subresource_range().aspectMask,
        0, VK_REMAINING_MIP_LEVELS, 0, VK_REMAINING_ARRAY_LAYERS,
    };
    return b;
}

} // namespace

// ============================================================================
// RenderGraph
// ============================================================================

RenderGraph::RenderGraph(std::string name) : m_Name(std::move(name)) {}

// ---------------------------------------------------------------------------
// 资源与 Pass 注册
// ---------------------------------------------------------------------------

ResourceHandle RenderGraph::Import(ImageViewResource *res) {
    GE_CORE_ASSERT(res != nullptr, "导入的外部图像不能为空");
    ResourceRecord rec;
    rec.name = res->GetName().empty() ? "external" : res->GetName();
    rec.type = ResourceType::Image;
    rec.external = res;
    m_Resources.emplace_back(std::move(rec));
    return static_cast<ResourceHandle>(m_Resources.size());
}

ResourceHandle RenderGraph::CreateVirtualResource(const RenderGraphResourceDesc &desc) {
    ResourceRecord rec;
    rec.name = "virtual";
    rec.type = ResourceType::Image;
    rec.virtualDesc = desc;
    m_Resources.emplace_back(std::move(rec));
    return static_cast<ResourceHandle>(m_Resources.size());
}

uint32_t RenderGraph::AddPass(RenderPassDesc &&desc) {
    GE_CORE_ASSERT(!m_Compiled, "图已编译，不能再追加 pass");
    m_Passes.emplace_back(std::move(desc));
    return static_cast<uint32_t>(m_Passes.size() - 1);
}

void RenderGraph::SetFrameSwapchain(ResourceHandle handle) {
    GE_CORE_ASSERT(handle != kInvalidResource && handle <= m_Resources.size(),
                   "帧 swapchain 句柄越界");
    m_Resources[handle - 1].isFrameSwapchain = true;
}

// ---------------------------------------------------------------------------
// §5.1 依赖建图
// ---------------------------------------------------------------------------

namespace {

/// 一个资源被本帧哪些 pass 读写（声明序收集）。
struct ResourceUseRecord {
    /// (pass 下标, 用法)。按 pass 下标升序。
    std::vector<std::pair<uint32_t, ResourceUsage>> byPass;
    /// 写者 pass 下标（升序）。
    std::vector<uint32_t> writerPasses;
};

} // namespace

void RenderGraph::BuildDependencyEdges() {
    m_Edges.clear();

    std::map<ResourceHandle, ResourceUseRecord> use;
    m_PassAccess.assign(m_Passes.size(), {});

    for (uint32_t p = 0; p < m_Passes.size(); ++p) {
        const auto &pass = m_Passes[p];
        const auto addAccess = [&](ResourceHandle h, ResourceUsage u) {
            if (h == kInvalidResource || h > m_Resources.size()) {
                GE_CORE_ERROR("pass [{}] 引用了无效资源句柄 {}", pass.name, h);
                GE_CORE_ASSERT(false, "pass 引用了无效资源句柄");
            }
            // 同一 pass 内同一资源多次出现：并集语义 —— 任一出现写即记为写。
            auto &accesses = m_PassAccess[p];
            auto it = std::find_if(accesses.begin(), accesses.end(),
                                   [&](const auto &a) { return a.first == h; });
            if (it == accesses.end()) {
                accesses.emplace_back(h, Access{u, IsWriteUsage(u)});
            } else {
                it->second.usage = u;   // 保留最近一次用法（用于掩码推导）
                it->second.write = it->second.write || IsWriteUsage(u);
            }
            auto &r = use[h];
            r.byPass.emplace_back(p, u);
            if (IsWriteUsage(u)) {
                r.writerPasses.push_back(p);
            }
        };

        for (const auto &att : pass.colorAttachments) {
            addAccess(att.resource, att.usage);
        }
        if (pass.depthAttachment.has_value()) {
            addAccess(pass.depthAttachment->resource, pass.depthAttachment->usage);
        }
        for (const auto &img : pass.readImages) {
            addAccess(img.resource, img.usage);
        }
    }

    // 为每个资源建立依赖边：
    //   - 某 pass 读：依赖该资源上一个写者（首次读外部已就绪内容则无依赖）
    //   - 某 pass 写：依赖该资源上一个写者（写-写串行）
    for (const auto &[res, record] : use) {
        (void)res;
        for (const auto &[curPass, usage] : record.byPass) {
            // 找 curPass 之前最近的写者
            auto it = std::upper_bound(record.writerPasses.begin(), record.writerPasses.end(), curPass);
            if (it == record.writerPasses.begin()) {
                continue;  // 没有更早的写者
            }
            --it;
            if (*it == curPass) {
                continue;  // 自己就是写者且无更早写者 → 无前驱
            }
            GraphEdge e;
            e.srcPass = *it;
            e.dstPass = curPass;
            e.resource = res;
            e.usage = usage;
            m_Edges.push_back(e);
        }
    }
}

// ---------------------------------------------------------------------------
// §5.2 稳定拓扑排序（Kahn）
// ---------------------------------------------------------------------------

void RenderGraph::TopologicalSort() {
    m_ExecutionOrder.clear();

    const size_t n = m_Passes.size();
    std::vector<uint32_t> inDegree(n, 0);
    std::vector<std::vector<uint32_t>> adj(n);
    for (const auto &e : m_Edges) {
        if (e.srcPass < n && e.dstPass < n) {
            adj[e.srcPass].push_back(e.dstPass);
            ++inDegree[e.dstPass];
        }
    }

    // 入度为 0 的节点按声明序先进队列（稳定：同层保持声明序）
    std::priority_queue<uint32_t, std::vector<uint32_t>, std::greater<uint32_t>> ready;
    for (uint32_t i = 0; i < n; ++i) {
        if (inDegree[i] == 0) {
            ready.push(i);
        }
    }

    while (!ready.empty()) {
        const uint32_t cur = ready.top();
        ready.pop();
        m_ExecutionOrder.push_back(cur);
        for (const uint32_t nx : adj[cur]) {
            if (--inDegree[nx] == 0) {
                ready.push(nx);
            }
        }
    }
    // 若 < n 说明有环，由 Compile() 检测
}

// ---------------------------------------------------------------------------
// Compile
// ---------------------------------------------------------------------------

bool RenderGraph::Compile() {
    m_Compiled = false;
    m_Edges.clear();
    m_ExecutionOrder.clear();

    if (m_Passes.empty()) {
        m_Compiled = true;  // 空图合法（调试期），Execute 为空操作
        return true;
    }

    BuildDependencyEdges();
    TopologicalSort();

    if (m_ExecutionOrder.size() != m_Passes.size()) {
        GE_CORE_ERROR("渲染图 [{}] 存在循环依赖，无法确定执行序", m_Name);
        GE_CORE_ASSERT(false, "渲染图存在循环依赖");
        return false;
    }

    m_Compiled = true;
    return true;
}

// ---------------------------------------------------------------------------
// Execute：逐 pass 前置屏障 + 命令录制
// ---------------------------------------------------------------------------

namespace {

/// 布局运行期状态。
struct PerResourceLayout {
    vk::ImageLayout layout = vk::ImageLayout::eUndefined;
    bool valid = false;   ///< 布局已知（帧首记忆或已在本帧被转换）
};

} // namespace

ResourceUsage RenderGraph::FindUsageInPass(uint32_t passIndex, ResourceHandle handle) const {
    const auto &pass = m_Passes[passIndex];
    for (const auto &att : pass.colorAttachments) {
        if (att.resource == handle) {
            return att.usage;
        }
    }
    if (pass.depthAttachment.has_value() && pass.depthAttachment->resource == handle) {
        return pass.depthAttachment->usage;
    }
    for (const auto &img : pass.readImages) {
        if (img.resource == handle) {
            return img.usage;
        }
    }
    return ResourceUsage::ColorAttachment;  // 理论不可达
}

void RenderGraph::Execute(VulkanCommandBuffer &cmd, VulkanRenderFrame &frame) {
    GE_CORE_ASSERT(m_Compiled, "渲染图未编译即执行，请先调用 Compile()");
    if (m_Passes.empty()) {
        return;  // 空图
    }

    // 布局运行期状态（按资源句柄），执行序推进中维护
    std::map<ResourceHandle, PerResourceLayout> layout;
    for (size_t i = 0; i < m_Resources.size(); ++i) {
        const auto &rec = m_Resources[i];
        const ResourceHandle h = static_cast<ResourceHandle>(i + 1);
        PerResourceLayout pl;
        if (rec.external) {
            if (rec.external->HasFinalLayout()) {
                pl.layout = rec.external->GetFinalLayout();  // 上一帧落点
            } else {
                pl.layout = rec.external->GetInitialLayout();  // 调用方给的初始布局
            }
            pl.valid = true;
        }
        layout[h] = pl;
    }

    // 每资源最近写它的 pass（跨 pass 屏障的源）；以及本帧是否已有写者
    std::map<ResourceHandle, uint32_t> lastWriterPass;

    // 逐 pass 按执行序
    for (const uint32_t pi : m_ExecutionOrder) {
        const auto &pass = m_Passes[pi];
        GE_CORE_ASSERT(pass.type == PassType::Raster, "渲染图 v1 仅支持 Raster pass");

        // ---- 生成前置屏障 ----
        struct PendingBarrier {
            vk::ImageMemoryBarrier barrier;
            vk::PipelineStageFlags srcStage;
            vk::PipelineStageFlags dstStage;
        };
        std::vector<PendingBarrier> pendings;

        // 逐资源去重：同一 pass 内同一资源出现多次（如附件 + 输入），只生成一次。
        std::vector<ResourceHandle> visited;
        for (const auto &[h, acc] : m_PassAccess[pi]) {
            if (std::find(visited.begin(), visited.end(), h) != visited.end()) {
                continue;
            }
            visited.push_back(h);

            const ResourceRecord &rec = m_Resources[h - 1];
            // 虚拟资源 v1 无真实图，跳过（阶段2 支持）
            if (!rec.external || !rec.external->GetView()) {
                continue;
            }
            VulkanImageView &view = *rec.external->GetView();
            PerResourceLayout &pl = layout[h];

            // 目标布局：本 pass 是否写该资源（acc.write 为并集，任一用法是写即写）。
            const bool passWrites = acc.write;
            const ResourceUsage passUsage = passWrites ? FindUsageInPass(pi, h) : acc.usage;
            vk::ImageLayout target;
            if (passWrites) {
                target = LayoutForWrite(passUsage);
            } else {
                target = LayoutForRead(acc.usage);
                if (target == vk::ImageLayout::eUndefined) {
                    target = pl.layout;  // 附件读沿用当前布局
                }
            }

            const bool hasPriorWriter = lastWriterPass.count(h) > 0;
            if (hasPriorWriter) {
                // 跨 pass：前驱写 → 本次读/写。
                // 内存屏障必须（排序访问）；布局转换仅在需要时（布局不同）。
                const uint32_t writerPi = lastWriterPass[h];
                const ResourceUsage writerUsage = FindUsageInPass(writerPi, h);
                const bool needLayoutChange =
                    pl.valid && pl.layout != target && target != vk::ImageLayout::eUndefined;
                PendingBarrier pb;
                pb.srcStage = WriteStage(writerUsage);
                pb.dstStage = passWrites ? WriteStage(passUsage) : ReadStage(acc.usage);
                pb.barrier = MakeBarrier(WriteAccess(writerUsage),
                                         passWrites ? WriteAccess(passUsage) : ReadAccess(acc.usage),
                                         needLayoutChange ? pl.layout : target,
                                         target, view);
                // 若无需布局转换，oldLayout == newLayout == target，仅做内存排序
                pendings.push_back(std::move(pb));
                if (needLayoutChange) {
                    pl.layout = target;
                    pl.valid = true;
                }
            } else if (pl.valid && pl.layout != target && target != vk::ImageLayout::eUndefined) {
                // 本帧首次访问且需布局转换（无内存依赖）。
                const vk::PipelineStageFlags dstStage =
                    passWrites ? WriteStage(passUsage) : ReadStage(acc.usage);
                PendingBarrier pb;
                // 首次转换无依赖源：eTopOfPipe 表示"等待此前全部命令完成"，
                // 对丢弃型（Undefined 起点）是标准安全选择。
                pb.srcStage = vk::PipelineStageFlagBits::eTopOfPipe;
                pb.dstStage = dstStage;
                pb.barrier = MakeBarrier({},
                                         passWrites ? WriteAccess(passUsage) : ReadAccess(acc.usage),
                                         pl.layout, target, view);
                pendings.push_back(std::move(pb));
                pl.layout = target;
                pl.valid = true;
            } else if (!pl.valid && target != vk::ImageLayout::eUndefined) {
                GE_CORE_WARN("资源 [{}] 布局未知且被 pass [{}] 访问，跳过布局转换",
                             rec.name, pass.name);
            }

            // 布局推进：写成为后续写者的源；读不改变布局（读目标即其停靠布局）。
            if (passWrites) {
                lastWriterPass[h] = pi;
            }
        }

        // ---- 提交前置屏障（本 pass 全部合并为一条 pipelineBarrier） ----
        if (!pendings.empty()) {
            vk::PipelineStageFlags src = {};
            vk::PipelineStageFlags dst = {};
            std::vector<vk::ImageMemoryBarrier> barriers;
            barriers.reserve(pendings.size());
            for (const auto &pb : pendings) {
                src |= pb.srcStage;
                dst |= pb.dstStage;
                barriers.push_back(pb.barrier);
            }
            cmd.GetHandle().pipelineBarrier(src, dst, {}, {}, {}, barriers);
        }

        // ---- 录制本 pass ----
        if (!pass.execute) {
            continue;  // 空 pass（调试期）
        }

        // 渲染区域：声明无效时用 1x1 占位并告警（计划书 S2 由调用方填好）
        vk::Rect2D renderArea = pass.renderArea;
        if (renderArea.extent.width == 0 || renderArea.extent.height == 0) {
            GE_CORE_WARN("渲染图 pass [{}] 未指定 renderArea，使用 1x1 占位", pass.name);
            renderArea = vk::Rect2D{{0, 0}, {1, 1}};
        }

        VulkanRenderingInfo rinfo;
        rinfo.SetRenderArea(renderArea);
        rinfo.SetLayerCount(1);

        for (const auto &att : pass.colorAttachments) {
            const ResourceRecord &rec = m_Resources[att.resource - 1];
            if (!rec.external || !rec.external->GetView()) {
                GE_CORE_WARN("pass [{}] 颜色附件引用了无效资源，跳过", pass.name);
                continue;
            }
            // 附件渲染布局：必须是写后布局（附件写入发生在 ColorAttachmentOptimal）。
            // 前置屏障已把资源转到该布局；att.finalLayout（写后想再转到的布局）
            // 由 S2 的收尾转换使用，S1 尚未接线，此处不使用。
            const vk::ImageLayout renderLayout = LayoutForWrite(ResourceUsage::ColorAttachment);
            vk::ClearValue cv;
            cv.color = att.clearValue.color;
            rinfo.AddColorAttachment(rec.external->GetView()->GetHandle(),
                                     att.loadOp, att.storeOp, cv, renderLayout);
        }
        if (pass.depthAttachment.has_value()) {
            const auto &datt = *pass.depthAttachment;
            const ResourceRecord &rec = m_Resources[datt.resource - 1];
            if (rec.external && rec.external->GetView()) {
                vk::ClearDepthStencilValue clearDS{1.0f, 0};
                const vk::ImageLayout renderLayout = LayoutForWrite(ResourceUsage::DepthStencilAttachment);
                rinfo.SetDepthAttachment(rec.external->GetView()->GetHandle(),
                                         datt.loadOp, datt.storeOp, clearDS, renderLayout);
            }
        }

        PassExecuteContext ctx;
        ctx.cmd = &cmd;
        ctx.frame = &frame;
        ctx.renderArea = renderArea;
        ctx.renderingInfo = &rinfo;

        rinfo.Begin(cmd.GetHandle());
        pass.execute(ctx);
        VulkanRenderingInfo::End(cmd.GetHandle());
    }

    // ---- 收尾：外部资源最终布局写回（WSI 图除外） ----
    for (size_t i = 0; i < m_Resources.size(); ++i) {
        const auto &rec = m_Resources[i];
        if (!rec.external || rec.isFrameSwapchain) {
            continue;
        }
        auto it = layout.find(static_cast<ResourceHandle>(i + 1));
        if (it != layout.end() && it->second.valid) {
            rec.external->RecordFinalLayout(it->second.layout);
        }
    }
}

// ---------------------------------------------------------------------------
// 调试访问
// ---------------------------------------------------------------------------

RenderGraph::ResourceRecord *RenderGraph::FindResource(ResourceHandle handle) {
    if (handle == kInvalidResource || handle > m_Resources.size()) {
        return nullptr;
    }
    return &m_Resources[handle - 1];
}

const RenderGraph::ResourceRecord *RenderGraph::FindResource(ResourceHandle handle) const {
    return const_cast<RenderGraph *>(this)->FindResource(handle);
}

} // namespace GE
