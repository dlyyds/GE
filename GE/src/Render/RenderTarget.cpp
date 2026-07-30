/**
 * @file RenderTarget.cpp
 * @brief RenderTarget 实现。
 */

#include "Render/RenderTarget.h"
#include "Debug/Assert.h"
#include "Render/VulkanBase/VulkanDevice.h"
#include "Render/VulkanBase/VulkanCommon.h"

namespace GE {

// ============================================================================
// 构造函数 / 析构函数
// ============================================================================

RenderTarget::RenderTarget(VulkanDevice &device,
                           const RenderTargetDesc &desc,
                           std::unique_ptr<VulkanImageView> swapchainView)
    : m_Device(device), m_Desc(desc), m_SwapchainView(std::move(swapchainView)) {
    CreateResources();
}

RenderTarget::~RenderTarget() {
    DestroyResources();
}

RenderTarget::RenderTarget(RenderTarget &&other) noexcept
    : m_Device(other.m_Device),
      m_Desc(other.m_Desc),
      m_SwapchainView(std::move(other.m_SwapchainView)),
      m_MSAAColorImage(std::move(other.m_MSAAColorImage)),
      m_MSAAColorView(std::move(other.m_MSAAColorView)),
      m_DepthImage(std::move(other.m_DepthImage)),
      m_DepthView(std::move(other.m_DepthView)),
      m_DepthResolveImage(std::move(other.m_DepthResolveImage)),
      m_DepthResolveView(std::move(other.m_DepthResolveView)) {
}

// ============================================================================
// 访问器
// ============================================================================

VulkanImageView &RenderTarget::GetColorResolveView() {
    if (HasMSAA() && m_MSAAColorView != nullptr) {
        return *m_MSAAColorView;
    }
    return *m_SwapchainView;
}

const VulkanImageView &RenderTarget::GetColorResolveView() const {
    if (HasMSAA() && m_MSAAColorView != nullptr) {
        return *m_MSAAColorView;
    }
    return *m_SwapchainView;
}

VulkanImageView &RenderTarget::GetDepthView() {
    GE_CORE_ASSERT(m_DepthView != nullptr, "深度附件未创建，请检查 enableDepth 配置");
    return *m_DepthView;
}

const VulkanImageView &RenderTarget::GetDepthView() const {
    GE_CORE_ASSERT(m_DepthView != nullptr, "深度附件未创建，请检查 enableDepth 配置");
    return *m_DepthView;
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
    m_SwapchainView.reset();
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

    m_DepthImage = std::make_unique<VulkanImage>(
        m_Device,
        VulkanImageBuilder(m_Desc.extent.width, m_Desc.extent.height)
            .with_format(depthFormat)
            .with_sample_count(m_Desc.sampleCount)  // MSAA 时与颜色附件相同采样数
            .with_usage(usage));

    // VulkanImageView 构造函数会自动根据格式推断 aspect mask：
    // - depth-only 格式（D16/D32F）→ eDepth
    // - depth-stencil 格式（D24S8/D32FS8）→ eDepth | eStencil
    m_DepthView = std::make_unique<VulkanImageView>(
        *m_DepthImage,
        vk::ImageViewType::e2D,
        depthFormat,
        0, 0, 1, 1);  // baseMip, baseArray, mipLevels, arrayLayers
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
