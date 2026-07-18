/**
 * @file RenderTarget.cpp
 * @brief RenderTarget 实现。
 */

#include "Render/VulkanBase/RenderTarget.h"
#include "Render/VulkanBase/VulkanDevice.h"
#include "Render/VulkanBase/VulkanCommon.h"

namespace GE {

// ============================================================================
// 构造函数 / 析构函数
// ============================================================================

RenderTarget::RenderTarget(VulkanDevice &device,
                           const RenderTargetDesc &desc,
                           vk::ImageView swapchainView)
    : m_Device(device), m_Desc(desc), m_SwapchainView(swapchainView) {
    GE_CORE_ASSERT(swapchainView != nullptr, "Swapchain image view 不能为 null");
    CreateResources();
}

RenderTarget::RenderTarget(VulkanDevice &device,
                           const RenderTargetDesc &desc)
    : m_Device(device), m_Desc(desc), m_SwapchainView(nullptr) {
    GE_CORE_ASSERT(desc.colorFormat != vk::Format::eUndefined,
                   "离屏 RenderTarget 必须指定 colorFormat");
    CreateResources();
}

RenderTarget::~RenderTarget() {
    DestroyResources();
}

RenderTarget::RenderTarget(RenderTarget &&other) noexcept
    : m_Device(other.m_Device),
      m_Desc(other.m_Desc),
      m_SwapchainView(other.m_SwapchainView),
      m_MSAAColorImage(std::move(other.m_MSAAColorImage)),
      m_MSAAColorView(std::move(other.m_MSAAColorView)),
      m_DepthImage(std::move(other.m_DepthImage)),
      m_DepthView(std::move(other.m_DepthView)),
      m_DepthResolveImage(std::move(other.m_DepthResolveImage)),
      m_DepthResolveView(std::move(other.m_DepthResolveView)) {
    other.m_SwapchainView = nullptr;
}

// ============================================================================
// 重建
// ============================================================================

void RenderTarget::Recreate(const RenderTargetDesc &newDesc,
                            vk::ImageView newSwapchainView) {
    DestroyResources();

    m_Desc = newDesc;
    if (newSwapchainView != nullptr) {
        m_SwapchainView = newSwapchainView;
    }

    CreateResources();
}

// ============================================================================
// 配置 VulkanRenderingInfo
// ============================================================================

void RenderTarget::SetupRenderingInfo(VulkanRenderingInfo &renderInfo,
                                      const vk::Rect2D &renderArea) const {
    renderInfo.Reset();
    renderInfo.SetRenderArea(renderArea);
    renderInfo.SetLayerCount(m_Desc.layerCount);

    // --- 颜色附件 ---
    if (HasMSAA()) {
        // MSAA：多采样缓冲作为主附件，resolve 到 swapchain
        GE_CORE_ASSERT(m_MSAAColorView != nullptr, "MSAA 颜色缓冲未创建");
        GE_CORE_ASSERT(m_SwapchainView != nullptr, "MSAA 模式下需要 swapchain view 作为 resolve 目标");

        renderInfo.AddColorAttachmentWithResolve(
            m_MSAAColorView->GetHandle(),   // 多采样缓冲
            m_SwapchainView,                // resolve 目标（单采样 swapchain）
            m_Desc.colorLoadOp,
            m_Desc.colorStoreOp,
            m_Desc.colorClearValue,
            vk::ImageLayout::eColorAttachmentOptimal,
            vk::ImageLayout::eColorAttachmentOptimal,
            vk::ResolveModeFlagBits::eAverage);
    } else {
        // 非 MSAA：直接写入 swapchain（或离屏纹理）
        vk::ImageView colorView = m_SwapchainView != nullptr
                                      ? m_SwapchainView
                                      : m_MSAAColorView->GetHandle();
        renderInfo.AddColorAttachment(
            colorView,
            m_Desc.colorLoadOp,
            m_Desc.colorStoreOp,
            m_Desc.colorClearValue);
    }

    // --- 深度/模板附件 ---
    if (m_Desc.enableDepth && m_DepthView != nullptr) {
        vk::ClearDepthStencilValue clearDS = m_Desc.depthClearValue.depthStencil;

        if (m_Desc.enableStencil) {
            // 共享 depth/stencil attachment
            renderInfo.SetDepthStencilAttachment(
                m_DepthView->GetHandle(),
                m_Desc.depthLoadOp,
                m_Desc.depthStoreOp,
                m_Desc.stencilLoadOp,
                m_Desc.stencilStoreOp,
                clearDS);
        } else {
            // 仅深度
            if (HasMSAA() && m_DepthResolveView != nullptr) {
                // MSAA 深度 + resolve（未来扩展）
                renderInfo.SetDepthAttachmentWithResolve(
                    m_DepthView->GetHandle(),
                    m_DepthResolveView->GetHandle(),
                    m_Desc.depthLoadOp,
                    m_Desc.depthStoreOp,
                    clearDS);
            } else {
                renderInfo.SetDepthAttachment(
                    m_DepthView->GetHandle(),
                    m_Desc.depthLoadOp,
                    m_Desc.depthStoreOp,
                    clearDS);
            }
        }
    }
}

void RenderTarget::SetupRenderingInfo(VulkanRenderingInfo &renderInfo) const {
    vk::Rect2D renderArea{{0, 0}, m_Desc.extent};
    SetupRenderingInfo(renderInfo, renderArea);
}

// ============================================================================
// 访问器
// ============================================================================

vk::ImageView RenderTarget::GetColorResolveView() const {
    if (HasMSAA() && m_MSAAColorView != nullptr) {
        return m_MSAAColorView->GetHandle();
    }
    return m_SwapchainView;
}

vk::ImageView RenderTarget::GetDepthView() const {
    return m_DepthView != nullptr ? m_DepthView->GetHandle() : nullptr;
}

// ============================================================================
// 内部资源管理
// ============================================================================

void RenderTarget::CreateResources() {
    // --- MSAA 颜色缓冲 ---
    if (m_Desc.enableMSAA && m_Desc.sampleCount != vk::SampleCountFlagBits::e1) {
        CreateMSAAColorBuffer();
    }

    // --- 深度/模板缓冲 ---
    if (m_Desc.enableDepth || m_Desc.enableStencil) {
        CreateDepthBuffer();
    }
}

void RenderTarget::DestroyResources() {
    m_DepthResolveView.reset();
    m_DepthResolveImage.reset();
    m_DepthView.reset();
    m_DepthImage.reset();
    m_MSAAColorView.reset();
    m_MSAAColorImage.reset();
    // m_SwapchainView 为外部引用，不销毁
}

// ============================================================================
// MSAA 颜色缓冲
// ============================================================================

void RenderTarget::CreateMSAAColorBuffer() {
    GE_CORE_ASSERT(m_Desc.colorFormat != vk::Format::eUndefined,
                   "MSAA 颜色缓冲需要指定 colorFormat");

    vk::ImageUsageFlags usage = vk::ImageUsageFlagBits::eColorAttachment;
    // 如果需要作为输入附件（如某些延迟渲染场景），添加 eInputAttachment
    // usage |= vk::ImageUsageFlagBits::eInputAttachment;

    m_MSAAColorImage = std::make_unique<VulkanImage>(
        m_Device,
        VulkanImageBuilder(m_Desc.extent.width, m_Desc.extent.height)
            .with_format(m_Desc.colorFormat)
            .with_sample_count(m_Desc.sampleCount)
            .with_usage(usage));

    m_MSAAColorView = std::make_unique<VulkanImageView>(
        *m_MSAAColorImage,
        vk::ImageViewType::e2D,
        m_Desc.colorFormat);
}

// ============================================================================
// 深度/模板缓冲
// ============================================================================

void RenderTarget::CreateDepthBuffer() {
    vk::Format depthFormat = (m_Desc.depthFormat != vk::Format::eUndefined)
                                 ? m_Desc.depthFormat
                                 : PickDepthFormat();

    GE_CORE_ASSERT(depthFormat != vk::Format::eUndefined, "无法找到合适的深度格式");

    vk::ImageUsageFlags usage = vk::ImageUsageFlagBits::eDepthStencilAttachment;
    // 如果需要采样深度（如阴影贴图），添加 eSampled
    // usage |= vk::ImageUsageFlagBits::eSampled;

    // 确定 aspect mask
    vk::ImageAspectFlags aspect = vk::ImageAspectFlagBits::eDepth;
    if (is_depth_stencil_format(static_cast<VkFormat>(depthFormat))) {
        aspect |= vk::ImageAspectFlagBits::eStencil;
    }

    m_DepthImage = std::make_unique<VulkanImage>(
        m_Device,
        VulkanImageBuilder(m_Desc.extent.width, m_Desc.extent.height)
            .with_format(depthFormat)
            .with_sample_count(m_Desc.sampleCount)  // MSAA 时与颜色附件相同采样数
            .with_usage(usage));

    m_DepthView = std::make_unique<VulkanImageView>(
        *m_DepthImage,
        vk::ImageViewType::e2D,
        depthFormat,
        0, 0, 1, 1);  // baseMip, baseArray, mipLevels, arrayLayers
    // 注意：VulkanImageView 构造函数内部会根据格式自动选择 aspect，
    // 但这里我们显式处理 depth/stencil 格式
}

vk::Format RenderTarget::PickDepthFormat() const {
    if (m_Desc.enableStencil) {
        // 需要深度+模板
        return get_suitable_depth_format(m_Device.GetGpu().GetHandle(), false);
    } else {
        // 仅深度
        return get_suitable_depth_format(m_Device.GetGpu().GetHandle(), true);
    }
}

} // namespace GE
