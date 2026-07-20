/**
 * @file VulkanRenderingInfo.cpp
 * @brief VulkanRenderingInfo 实现，包括从 RenderTarget 构造的工厂方法。
 */

#include "Render/VulkanBase/VulkanRenderingInfo.h"
#include "Render/VulkanBase/RenderTarget.h"

namespace GE {

// ============================================================================
// 工厂方法：从 RenderTarget 构造
// ============================================================================

VulkanRenderingInfo VulkanRenderingInfo::FromRenderTarget(const RenderTarget &rt,
                                                          const vk::Rect2D &renderArea) {
    VulkanRenderingInfo info;
    info.SetRenderArea(renderArea);
    info.SetLayerCount(rt.GetDesc().layerCount);

    const auto &desc = rt.GetDesc();

    // --- 颜色附件 ---
    if (rt.HasMSAA()) {
        // MSAA：多采样缓冲作为主附件，resolve 到 swapchain
        vk::ImageView msaaView = rt.GetColorResolveView().GetHandle();
        vk::ImageView swapchainView = rt.GetSwapchainView().GetHandle();

        info.AddColorAttachmentWithResolve(
            msaaView,                       // 多采样缓冲
            swapchainView,                  // resolve 目标（单采样 swapchain）
            desc.colorLoadOp,
            desc.colorStoreOp,
            desc.colorClearValue,
            vk::ImageLayout::eColorAttachmentOptimal,
            vk::ImageLayout::eColorAttachmentOptimal,
            vk::ResolveModeFlagBits::eAverage);
    } else {
        // 非 MSAA：直接写入 swapchain（或离屏纹理）
        vk::ImageView colorView = rt.GetSwapchainView().GetHandle()
                                      ? rt.GetSwapchainView().GetHandle()
                                      : rt.GetColorResolveView().GetHandle();
        info.AddColorAttachment(
            colorView,
            desc.colorLoadOp,
            desc.colorStoreOp,
            desc.colorClearValue);
    }

    // --- 深度/模板附件 ---
    if (desc.enableDepth && rt.GetDesc().enableDepth) {
        vk::ClearDepthStencilValue clearDS = desc.depthClearValue.depthStencil;

        if (desc.enableStencil) {
            // 共享 depth/stencil attachment
            info.SetDepthStencilAttachment(
                rt.GetDepthView().GetHandle(),
                desc.depthLoadOp,
                desc.depthStoreOp,
                desc.stencilLoadOp,
                desc.stencilStoreOp,
                clearDS);
        } else {
            // 仅深度
            // 注意：RenderTarget 目前支持 MSAA 深度 + resolve（未来扩展），
            // 此处暂不处理 m_DepthResolveView，后续可扩展
            info.SetDepthAttachment(
                rt.GetDepthView().GetHandle(),
                desc.depthLoadOp,
                desc.depthStoreOp,
                clearDS);
        }
    }

    return info;
}

VulkanRenderingInfo VulkanRenderingInfo::FromRenderTarget(const RenderTarget &rt) {
    vk::Rect2D renderArea{{0, 0}, rt.GetExtent()};
    return FromRenderTarget(rt, renderArea);
}

} // namespace GE