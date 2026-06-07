#pragma once

#include <vulkan/vulkan.hpp>
#include <vector>

namespace GE {

class VulkanRenderingInfo {
public:
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
        m_ColorAttachments.push_back(vk::RenderingAttachmentInfo{
            .imageView = imageView,
            .imageLayout = layout,
            .loadOp = loadOp,
            .storeOp = storeOp,
            .clearValue = clearValue,
        });
    }

    void Begin(vk::CommandBuffer cmd) const {
        vk::RenderingInfo info{
            .renderArea = m_RenderArea,
            .layerCount = m_LayerCount,
            .colorAttachmentCount = static_cast<uint32_t>(m_ColorAttachments.size()),
            .pColorAttachments = m_ColorAttachments.data(),
        };
        cmd.beginRendering(info);
    }

    static void End(vk::CommandBuffer cmd) { cmd.endRendering(); }

private:
    vk::Rect2D m_RenderArea{{0, 0}, {0, 0}};
    uint32_t m_LayerCount = 1;
    std::vector<vk::RenderingAttachmentInfo> m_ColorAttachments;
};

} // namespace GE
