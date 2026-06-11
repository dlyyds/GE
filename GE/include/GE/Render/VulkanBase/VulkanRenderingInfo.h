#pragma once

#include <vulkan/vulkan.hpp>
#include <array>
#include <cstdint>

#include "Debug/Assert.h"

namespace GE {

/// Dynamic rendering 辅助类。
/// 封装 vk::RenderingInfo，用固定数组替代 vector 避免每帧堆分配。
/// 最大支持 4 个 color attachment（1 color + 3 resolve 的典型场景）。
class VulkanRenderingInfo {
public:
    static constexpr uint32_t MAX_COLOR_ATTACHMENTS = 4;

    VulkanRenderingInfo() = default;

    void SetRenderArea(vk::Offset2D offset, vk::Extent2D extent) {
        m_RenderArea = vk::Rect2D{offset, extent};
    }

    void SetRenderArea(int32_t x, int32_t y, uint32_t width, uint32_t height) {
        m_RenderArea = vk::Rect2D{{x, y}, {width, height}};
    }

    void SetLayerCount(uint32_t count) { m_LayerCount = count; }

    void AddColorAttachment(vk::ImageView imageView,
                            vk::AttachmentLoadOp loadOp,
                            vk::AttachmentStoreOp storeOp,
                            vk::ClearValue clearValue = {},
                            vk::ImageLayout layout = vk::ImageLayout::eColorAttachmentOptimal) {
        GE_CORE_ASSERT(m_ColorAttachmentCount < MAX_COLOR_ATTACHMENTS,
                       "Exceeded max color attachments (max 4)");
        m_ColorAttachments[m_ColorAttachmentCount] = vk::RenderingAttachmentInfo{
            .imageView = imageView,
            .imageLayout = layout,
            .loadOp = loadOp,
            .storeOp = storeOp,
            .clearValue = clearValue,
        };
        m_ColorAttachmentCount++;
    }

    void SetDepthAttachment(vk::ImageView imageView,
                            vk::AttachmentLoadOp loadOp = vk::AttachmentLoadOp::eClear,
                            vk::AttachmentStoreOp storeOp = vk::AttachmentStoreOp::eDontCare,
                            vk::ImageLayout layout = vk::ImageLayout::eDepthStencilAttachmentOptimal) {
        vk::ClearValue clearValue;
        clearValue.depthStencil = vk::ClearDepthStencilValue{1.0f, 0};
        m_DepthAttachment = vk::RenderingAttachmentInfo{
            .imageView = imageView,
            .imageLayout = layout,
            .loadOp = loadOp,
            .storeOp = storeOp,
            .clearValue = clearValue,
        };
        m_HasDepth = true;
    }

    void Begin(vk::CommandBuffer cmd) const {
        vk::RenderingInfo info{
            .renderArea = m_RenderArea,
            .layerCount = m_LayerCount,
            .colorAttachmentCount = m_ColorAttachmentCount,
            .pColorAttachments = m_ColorAttachments.data(),
            .pDepthAttachment = m_HasDepth ? &m_DepthAttachment : nullptr,
        };
        cmd.beginRendering(info);
    }

    static void End(vk::CommandBuffer cmd) { cmd.endRendering(); }

    /// 重置 attachment 计数器，允许对象重用。
    void Reset() {
        m_ColorAttachmentCount = 0;
        m_HasDepth = false;
    }

private:
    vk::Rect2D m_RenderArea{{0, 0}, {0, 0}};
    uint32_t m_LayerCount = 1;
    std::array<vk::RenderingAttachmentInfo, MAX_COLOR_ATTACHMENTS> m_ColorAttachments{};
    uint32_t m_ColorAttachmentCount = 0;
    vk::RenderingAttachmentInfo m_DepthAttachment{};
    bool m_HasDepth = false;
};

} // namespace GE
