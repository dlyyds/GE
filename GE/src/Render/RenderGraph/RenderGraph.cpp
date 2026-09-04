/**
 * @file RenderGraph.cpp
 * @brief 渲染图实现：声明序顺序执行 + 逐 pass 前置屏障。
 *
 * 同步采用 vk::ImageMemoryBarrier2（pipelineBarrier2，对齐全引擎
 * VulkanImage.cpp 的同步2 范式）。依赖建图与拓扑排序已移除：声明序即执行序，
 * pass 必须按依赖序声明，框架不自动重排、不做环检测。
 *
 * 屏障策略：Execute 按声明序逐 pass 推进，为每个 pass 的跨 pass 资源访问
 * 生成前置屏障。资源布局状态由帧首记忆/初始状态出发随执行序推进。
 */

#include "Render/RenderGraph/RenderGraph.h"

#include "Core/Base.h"
#include "Core/Log.h"
#include "Debug/Assert.h"
#include "Render/VulkanBase/VulkanCommandBuffer.h"
#include "Render/VulkanBase/VulkanDevice.h"
#include "Render/VulkanBase/VulkanImage.h"
#include "Render/VulkanBase/VulkanImageView.h"
#include "Render/VulkanBase/VulkanRenderFrame.h"
#include "Render/VulkanBase/VulkanRenderingInfo.h"

#include <algorithm>
#include <map>

namespace GE {

// ============================================================================
// 资源用法 → 阶段/访问/布局映射（计划书 §7 资源用法表）
// ============================================================================

namespace {

/// 「写」用法对应的写阶段（写入该资源的访问发生在哪条管线阶段）。
vk::PipelineStageFlags2 WriteStage(ResourceUsage usage) {
    switch (usage) {
    case ResourceUsage::ColorAttachment:
        return vk::PipelineStageFlagBits2::eColorAttachmentOutput;
    case ResourceUsage::DepthStencilAttachment:
        return vk::PipelineStageFlagBits2::eEarlyFragmentTests | vk::PipelineStageFlagBits2::eLateFragmentTests;
    case ResourceUsage::ShaderWrite:
        return vk::PipelineStageFlagBits2::eComputeShader;
    case ResourceUsage::TransferDst:
        return vk::PipelineStageFlagBits2::eTransfer;
    default:
        return {};  // 读用法无写阶段
    }
}

/// 「读」用法对应的读阶段（读取该资源的访问发生在哪条管线阶段）。
vk::PipelineStageFlags2 ReadStage(ResourceUsage usage) {
    switch (usage) {
    case ResourceUsage::ColorAttachment:
        return vk::PipelineStageFlagBits2::eColorAttachmentOutput;
    case ResourceUsage::DepthStencilAttachment:
        return vk::PipelineStageFlagBits2::eEarlyFragmentTests | vk::PipelineStageFlagBits2::eLateFragmentTests;
    case ResourceUsage::ShaderRead:
        // v1 覆盖顶点/片元采样（计算留阶段2）
        return vk::PipelineStageFlagBits2::eVertexShader | vk::PipelineStageFlagBits2::eFragmentShader;
    case ResourceUsage::ShaderWrite:
        return vk::PipelineStageFlagBits2::eComputeShader;
    case ResourceUsage::TransferDst:
        return vk::PipelineStageFlagBits2::eTransfer;
    }
    return {};
}

/// 用法对应的「写访问」位。
vk::AccessFlags2 WriteAccess(ResourceUsage usage) {
    switch (usage) {
    case ResourceUsage::ColorAttachment:
        return vk::AccessFlagBits2::eColorAttachmentWrite;
    case ResourceUsage::DepthStencilAttachment:
        return vk::AccessFlagBits2::eDepthStencilAttachmentWrite;
    case ResourceUsage::ShaderWrite:
        return vk::AccessFlagBits2::eShaderWrite;
    case ResourceUsage::TransferDst:
        return vk::AccessFlagBits2::eTransferWrite;
    default:
        return {};
    }
}

/// 用法对应的「读访问」位。
vk::AccessFlags2 ReadAccess(ResourceUsage usage) {
    switch (usage) {
    case ResourceUsage::ColorAttachment:
        return vk::AccessFlagBits2::eColorAttachmentRead;
    case ResourceUsage::DepthStencilAttachment:
        return vk::AccessFlagBits2::eDepthStencilAttachmentRead;
    case ResourceUsage::ShaderRead:
        return vk::AccessFlagBits2::eShaderRead;
    case ResourceUsage::ShaderWrite:
        return {};
    case ResourceUsage::TransferDst:
        return vk::AccessFlagBits2::eTransferRead;
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

/// 构造一条同步2 图像内存屏障（stage/access 一并携带在结构体内）。
vk::ImageMemoryBarrier2 MakeBarrier(vk::PipelineStageFlags2 srcStage,
                                    vk::AccessFlags2 srcAccess,
                                    vk::PipelineStageFlags2 dstStage,
                                    vk::AccessFlags2 dstAccess,
                                    vk::ImageLayout oldLayout,
                                    vk::ImageLayout newLayout,
                                    const VulkanImageView &view) {
    vk::ImageMemoryBarrier2 b;
    b.srcStageMask = srcStage;
    b.srcAccessMask = srcAccess;
    b.dstStageMask = dstStage;
    b.dstAccessMask = dstAccess;
    b.oldLayout = oldLayout;
    b.newLayout = newLayout;
    b.image = view.get_image().GetHandle();
    // 复用 view 已推断的 aspect（VulkanImageView.cpp 按格式正确设置 color/depth/stencil）
    b.subresourceRange = vk::ImageSubresourceRange{
        view.get_subresource_range().aspectMask,
        0, VK_REMAINING_MIP_LEVELS, 0, VK_REMAINING_ARRAY_LAYERS,
    };
    return b;
}

/// 虚拟资源池 LRU 上限（软目标；帧内同时占用的资源数超过此值时不被淘汰，仅超限时裁剪）。
constexpr size_t kVirtualPoolMaxEntries = 8;

} // namespace

// ============================================================================
// RenderGraph
// ============================================================================

RenderGraph::RenderGraph(std::string name) : m_Name(std::move(name)) {}

RenderGraph::~RenderGraph() = default;

// ---------------------------------------------------------------------------
// 虚拟资源池（阶段2 基建；见《渲染图虚拟资源池计划书》）
// ---------------------------------------------------------------------------

void RenderGraph::SetDevice(VulkanDevice *device) {
    m_Device = device;
}

void RenderGraph::SetFramesInFlight(uint32_t framesInFlight) {
    m_FramesInFlight = std::max(1u, framesInFlight);
}

void RenderGraph::Shutdown() {
    m_VirtualPool.clear();  // 须在 device 销毁前调用（Renderer 析构体显式触发）
}

void RenderGraph::ResolveVirtualResources() {
    for (size_t i = 0; i < m_Resources.size(); ++i) {
        auto &rec = m_Resources[i];
        if (rec.external) {
            continue;  // 外部资源不需要池
        }
        if (rec.type != ResourceType::Image) {
            continue;  // v1 仅图像
        }
        const ResourceHandle h = static_cast<ResourceHandle>(i + 1);

        // 从本帧全部 pass 声明推导 usage（并集）：附件=写能力，采样=读能力。
        // 计划书 §3.4：不在 desc 加 usage 字段，pass 声明即唯一事实来源。
        vk::ImageUsageFlags usage = {};
        for (const auto &pass : m_Passes) {
            for (const auto &att : pass.colorAttachments) {
                if (att.resource == h) {
                    usage |= vk::ImageUsageFlagBits::eColorAttachment;
                }
            }
            if (pass.depthAttachment.has_value() && pass.depthAttachment->resource == h) {
                usage |= vk::ImageUsageFlagBits::eDepthStencilAttachment;
            }
            for (const auto &img : pass.readImages) {
                if (img.resource == h) {
                    usage |= vk::ImageUsageFlagBits::eSampled;
                }
            }
        }
        if (usage == vk::ImageUsageFlags{}) {
            GE_CORE_WARN("虚拟资源 [{}] 未被任何 pass 使用，跳过分配", rec.name);
            continue;
        }
        if (rec.virtualDesc.extent.width == 0 || rec.virtualDesc.extent.height == 0) {
            GE_CORE_WARN("虚拟资源 [{}] extent 无效，跳过分配", rec.name);
            continue;
        }

        PooledImage *entry = AcquirePoolEntry(rec.virtualDesc, usage);
        rec.pooledView = entry->view.get();
    }
}

RenderGraph::PooledImage *RenderGraph::AcquirePoolEntry(const RenderGraphResourceDesc &desc,
                                                        vk::ImageUsageFlags usage) {
    // 复用：匹配 (desc, usage) 的空闲条目。usage 并入匹配键，防止同一 desc 跨帧
    // 推导出不同 usage 时复用旧图（其创建态 usage 不含新能力 → 录制期校验失败）。
    for (auto &entry : m_VirtualPool) {
        if (entry.busy) {
            continue;
        }
        if (entry.desc.format != desc.format || entry.desc.extent != desc.extent
            || entry.desc.samples != desc.samples || entry.usage != usage) {
            continue;
        }
        entry.busy = true;
        entry.lastUsedFrameIndex = m_FrameIndex;
        return &entry;
    }

    // 未命中 → 新建。不做初始布局转换：首帧由图的屏障以 eUndefined 起点做丢弃转换。
    GE_CORE_ASSERT(m_Device, "虚拟资源池需要先 SetDevice() 注入 device");
    PooledImage entry;
    entry.desc = desc;
    entry.usage = usage;
    entry.image = std::make_unique<VulkanImage>(
        *m_Device,
        VulkanImageBuilder(desc.extent.width, desc.extent.height)
            .with_format(desc.format)
            .with_sample_count(desc.samples)
            .with_usage(usage));
    entry.view = std::make_unique<VulkanImageView>(
        *entry.image, vk::ImageViewType::e2D, desc.format, 0, 0, 1, 1);
    entry.busy = true;
    entry.lastUsedFrameIndex = m_FrameIndex;
    m_VirtualPool.emplace_back(std::move(entry));
    return &m_VirtualPool.back();
}

void RenderGraph::EvictPoolEntries() {
    // 计划书 §3.6：只淘汰"当前帧号 - lastUsed > framesInFlight"的条目 —— 此时
    // 引用它的所有帧均已完成、其描述符池已被复用重置，可立即销毁，无需退休列表。
    if (m_VirtualPool.size() <= kVirtualPoolMaxEntries) {
        return;
    }
    const uint32_t evictBefore = (m_FrameIndex > m_FramesInFlight)
                                     ? (m_FrameIndex - m_FramesInFlight)
                                     : 0u;
    while (m_VirtualPool.size() > kVirtualPoolMaxEntries) {
        // 在可安全淘汰的条目中取最久未用（LRU）者；无则停（如全部被占用，保持超限）。
        auto best = m_VirtualPool.end();
        for (auto it = m_VirtualPool.begin(); it != m_VirtualPool.end(); ++it) {
            if (it->lastUsedFrameIndex < evictBefore
                && (best == m_VirtualPool.end()
                    || it->lastUsedFrameIndex < best->lastUsedFrameIndex)) {
                best = it;
            }
        }
        if (best == m_VirtualPool.end()) {
            break;
        }
        m_VirtualPool.erase(best);
    }
}

// ---------------------------------------------------------------------------
// 资源与 Pass 注册
// ---------------------------------------------------------------------------

ResourceHandle RenderGraph::Import(VulkanImageView *view, const std::string &name) {
    GE_CORE_ASSERT(view != nullptr, "导入的外部图像视图不能为空");
    ResourceRecord rec;
    // 取名优先级：显式 name → 图像 debug name → 兜底 "external"。
    const std::string &imgDebugName = view->get_image().GetDebugName();
    rec.name = !name.empty() ? name
              : !imgDebugName.empty() ? imgDebugName
                                      : "external";
    rec.type = ResourceType::Image;
    rec.external = view;
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
// Compile：校验 pass 声明（轻量，无依赖图）
// ---------------------------------------------------------------------------

bool RenderGraph::Compile() {
    m_Compiled = true;  // 声明序即执行序，无环可检；空图合法（Execute 为空操作）
    return true;
}

void RenderGraph::Reset() {
    m_Resources.clear();
    m_Passes.clear();
    m_Compiled = false;

    // 虚拟资源池：全部条目标闲（图像保留跨帧复用）；超限时 LRU 淘汰可安全销毁的条目。
    for (auto &entry : m_VirtualPool) {
        entry.busy = false;
    }
    EvictPoolEntries();
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
    ++m_FrameIndex;  // LRU 时间戳（池淘汰安全判定，计划书 §3.6）
    if (m_Passes.empty()) {
        return;  // 空图
    }

    // 解析虚拟资源：推导 usage 并从池分配/复用视图（须先于布局播种与屏障）。
    ResolveVirtualResources();

    // 布局运行期状态（按资源句柄），执行序推进中维护
    std::map<ResourceHandle, PerResourceLayout> layout;
    for (size_t i = 0; i < m_Resources.size(); ++i) {
        const auto &rec = m_Resources[i];
        const ResourceHandle h = static_cast<ResourceHandle>(i + 1);
        PerResourceLayout pl;
        if (rec.external) {
            pl.layout = rec.external->get_image().get_layout();  // 帧首起点 = image 最近停靠/初始
            pl.valid = true;
        } else if (rec.pooledView) {
            // 池化虚拟资源：同一机制，跨帧布局记忆经 VulkanImage 承载。
            pl.layout = rec.pooledView->get_image().get_layout();
            pl.valid = true;
        }
        layout[h] = pl;
    }

    // 每资源最近写它的 pass（跨 pass 屏障的源）；以及本帧是否已有写者
    std::map<ResourceHandle, uint32_t> lastWriterPass;

    // 逐 pass 按声明序执行（声明序即执行序，框架不重排）
    for (uint32_t pi = 0; pi < m_Passes.size(); ++pi) {
        const auto &pass = m_Passes[pi];
        GE_CORE_ASSERT(pass.type == PassType::Raster, "渲染图 v1 仅支持 Raster pass");

        // ---- 收集本 pass 的资源访问（附件=写，采样=读）。按声明序，句柄去重 ----
        // （沿用旧的 AddAccess 语义：颜色附件 → 深度 → 采样，同资源以先声明者为准）
        std::vector<std::pair<ResourceHandle, ResourceUsage>> accesses;
        const auto addAccess = [&](ResourceHandle h, ResourceUsage u) {
            if (h == kInvalidResource || h > m_Resources.size()) {
                return;  // 无效句柄由录制段各自的告警兜底
            }
            const auto dup = std::find_if(accesses.begin(), accesses.end(),
                                          [&](const auto &a) { return a.first == h; });
            if (dup == accesses.end()) {
                accesses.emplace_back(h, u);
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

        // ---- 生成前置屏障 ----
        std::vector<vk::ImageMemoryBarrier2> barriers;

        // 逐资源生成屏障。外部或池化虚拟资源均可参与（池资源本帧已解析出视图）。
        for (const auto &[h, usage] : accesses) {
            const ResourceRecord &rec = m_Resources[h - 1];
            if (!rec.external && !rec.pooledView) {
                continue;
            }
            VulkanImageView &view = rec.external ? *rec.external : *rec.pooledView;
            PerResourceLayout &pl = layout[h];

            // 本 pass 对该资源是写还是读；写则停靠到写后布局，读则要求读布局。
            const bool passWrites = IsWriteUsage(usage);
            vk::ImageLayout target;
            if (passWrites) {
                target = LayoutForWrite(usage);
            } else {
                target = LayoutForRead(usage);
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
                const vk::PipelineStageFlags2 srcStage = WriteStage(writerUsage);
                const vk::PipelineStageFlags2 dstStage =
                    passWrites ? WriteStage(usage) : ReadStage(usage);
                barriers.push_back(MakeBarrier(
                    srcStage, WriteAccess(writerUsage),
                    dstStage, passWrites ? WriteAccess(usage) : ReadAccess(usage),
                    needLayoutChange ? pl.layout : target,   // 无需转换时 old==new==target，仅内存排序
                    target, view));
                if (needLayoutChange) {
                    pl.layout = target;
                    pl.valid = true;
                }
            } else if (pl.valid && pl.layout != target && target != vk::ImageLayout::eUndefined) {
                // 本帧首次访问且需布局转换（无内存依赖）。
                // srcStage=eTopOfPipe 表示"等待此前全部命令完成"，对丢弃型（Undefined 起点）是安全选择。
                const vk::PipelineStageFlags2 dstStage =
                    passWrites ? WriteStage(usage) : ReadStage(usage);
                barriers.push_back(MakeBarrier(
                    vk::PipelineStageFlagBits2::eTopOfPipe, {},
                    dstStage, passWrites ? WriteAccess(usage) : ReadAccess(usage),
                    pl.layout, target, view));
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

        // ---- 提交前置屏障（本 pass 全部合并为一次 pipelineBarrier2） ----
        if (!barriers.empty()) {
            vk::DependencyInfo depInfo{
                .imageMemoryBarrierCount = static_cast<uint32_t>(barriers.size()),
                .pImageMemoryBarriers   = barriers.data(),
            };
            cmd.GetHandle().pipelineBarrier2(depInfo);
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

        // 记录本 pass 实际写入的首个颜色/深度附件视图，随 ctx 透传给 execute 回调。
        // 回调（3D/2D 录制）据此推导附件格式与有无深度，无需自行持有 RenderTarget。
        VulkanImageView *firstColorView = nullptr;
        VulkanImageView *firstDepthView = nullptr;

        for (const auto &att : pass.colorAttachments) {
            const ResourceRecord &rec = m_Resources[att.resource - 1];
            if (!rec.external && !rec.pooledView) {
                GE_CORE_WARN("pass [{}] 颜色附件引用了无效资源，跳过", pass.name);
                continue;
            }
            VulkanImageView &view = rec.external ? *rec.external : *rec.pooledView;
            // 附件渲染布局：必须是写后布局（附件写入发生在 ColorAttachmentOptimal）。
            // 前置屏障已把资源转到该布局；att.finalLayout（写后想再转到的布局）
            // 由 S2 的收尾转换使用，S1 尚未接线，此处不使用。
            const vk::ImageLayout renderLayout = LayoutForWrite(ResourceUsage::ColorAttachment);
            vk::ClearValue cv;
            cv.color = att.clearValue.color;
            rinfo.AddColorAttachment(view.GetHandle(),
                                     att.loadOp, att.storeOp, cv, renderLayout);
            if (!firstColorView) {
                firstColorView = &view;
            }
        }
        if (pass.depthAttachment.has_value()) {
            const auto &datt = *pass.depthAttachment;
            const ResourceRecord &rec = m_Resources[datt.resource - 1];
            if (rec.external || rec.pooledView) {
                VulkanImageView &view = rec.external ? *rec.external : *rec.pooledView;
                vk::ClearDepthStencilValue clearDS{1.0f, 0};
                const vk::ImageLayout renderLayout = LayoutForWrite(ResourceUsage::DepthStencilAttachment);
                rinfo.SetDepthAttachment(view.GetHandle(),
                                         datt.loadOp, datt.storeOp, clearDS, renderLayout);
                firstDepthView = &view;
            }
        }

        PassExecuteContext ctx;
        ctx.cmd = &cmd;
        ctx.frame = &frame;
        ctx.renderArea = renderArea;
        ctx.renderingInfo = &rinfo;
        ctx.colorAttachmentView = firstColorView;
        ctx.depthAttachmentView = firstDepthView;

        rinfo.Begin(cmd.GetHandle());
        pass.execute(ctx);
        VulkanRenderingInfo::End(cmd.GetHandle());

        // ---- 收尾：写后布局转换（AttachmentDesc.finalLayout，S1 预留、S2 接线） ----
        // 附件在动态渲染内停在 LayoutForWrite(usage)（颜色/深度附件态）；若调用方
        // 声明了不同的 finalLayout（如 Scene2D 写后转 ShaderReadOnlyOptimal 供后续
        // 采样），在此追加一条收尾 barrier 并把布局状态推进到 finalLayout，供下一
        // pass 前置屏障与帧末写回使用。finalLayout == eUndefined 或 == 当前停靠布局
        // （默认停在附件态）时不产生转换。
        {
            std::vector<vk::ImageMemoryBarrier2> tailBarriers;
            const auto collectTail = [&](ResourceHandle h, ResourceUsage usage,
                                         vk::ImageLayout finalLayout) {
                if (h == kInvalidResource || h > m_Resources.size()) {
                    return;
                }
                const ResourceRecord &rec = m_Resources[h - 1];
                if ((!rec.external && !rec.pooledView) || finalLayout == vk::ImageLayout::eUndefined) {
                    return;
                }
                auto it = layout.find(h);
                if (it == layout.end() || !it->second.valid) {
                    return;
                }
                PerResourceLayout &pl = it->second;
                if (pl.layout == finalLayout) {
                    return;  // 已停靠，无需转换
                }
                // 收尾 barrier：附件写完成后把资源转到声明布局并使其对后续使用可见。
                // 目标布局决定目的阶段/访问：
                //  - ShaderReadOnlyOptimal（写后供采样）→ 目的 = 着色器读；
                //  - PresentSrcKHR（写后呈现，WSI 帧图）→ 目的 = 队列呈现（BottomOfPipe，
                //    不设访问位），仅需在提交前完成所有附件写。
                const vk::PipelineStageFlags2 dstStage =
                    (finalLayout == vk::ImageLayout::ePresentSrcKHR)
                        ? vk::PipelineStageFlagBits2::eBottomOfPipe
                        : vk::PipelineStageFlagBits2::eVertexShader
                              | vk::PipelineStageFlagBits2::eFragmentShader;
                const vk::AccessFlags2 dstAccess =
                    (finalLayout == vk::ImageLayout::ePresentSrcKHR)
                        ? vk::AccessFlags2{}
                        : vk::AccessFlagBits2::eShaderRead;
                tailBarriers.push_back(MakeBarrier(
                    WriteStage(usage), WriteAccess(usage),
                    dstStage, dstAccess,
                    pl.layout, finalLayout,
                    rec.external ? *rec.external : *rec.pooledView));
                pl.layout = finalLayout;
            };
            for (const auto &att : pass.colorAttachments) {
                collectTail(att.resource, att.usage, att.finalLayout);
            }
            if (pass.depthAttachment.has_value()) {
                const auto &datt = *pass.depthAttachment;
                collectTail(datt.resource, datt.usage, datt.finalLayout);
            }
            if (!tailBarriers.empty()) {
                vk::DependencyInfo depInfo{
                    .imageMemoryBarrierCount = static_cast<uint32_t>(tailBarriers.size()),
                    .pImageMemoryBarriers   = tailBarriers.data(),
                };
                cmd.GetHandle().pipelineBarrier2(depInfo);
            }
        }
    }

    // ---- 收尾：外部资源最终布局写回（WSI 图除外） ----
    for (size_t i = 0; i < m_Resources.size(); ++i) {
        const auto &rec = m_Resources[i];
        if ((!rec.external && !rec.pooledView) || rec.isFrameSwapchain) {
            continue;
        }
        auto it = layout.find(static_cast<ResourceHandle>(i + 1));
        if (it != layout.end() && it->second.valid) {
            VulkanImage &img = rec.external ? rec.external->get_image()
                                            : rec.pooledView->get_image();
            img.set_layout(it->second.layout);
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
