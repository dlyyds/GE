#pragma once

#include <memory>
#include <vector>
#include <vulkan/vulkan.hpp>

namespace GE {

class VulkanCommandBuffer;
class VulkanDevice;
class VulkanRenderFrame;

class VulkanCommandPool {
public:
    VulkanCommandPool(VulkanDevice &device, uint32_t queue_family_index,
                      vk::CommandPoolCreateFlags flags = vk::CommandPoolCreateFlagBits::eTransient);

    VulkanCommandPool(const VulkanCommandPool &) = delete;

    VulkanCommandPool(VulkanCommandPool &&other) noexcept;

    VulkanCommandPool &operator=(const VulkanCommandPool &) = delete;

    VulkanCommandPool &operator=(VulkanCommandPool &&other) = delete;

    ~VulkanCommandPool();

    [[nodiscard]] vk::CommandPool GetHandle() const { return m_Handle; }
    [[nodiscard]] VulkanDevice &GetDevice() const { return m_Device; }
    [[nodiscard]] uint32_t GetQueueFamilyIndex() const { return m_QueueFamilyIndex; }

    /// 设置关联的 RenderFrame（CommandBuffer 通过此指针访问 RenderFrame 的 RequestDescriptorSet）。
    void SetRenderFrame(VulkanRenderFrame *frame) { m_RenderFrame = frame; }

    /// 获取关联的 RenderFrame（可能为 nullptr）。
    [[nodiscard]] VulkanRenderFrame *GetRenderFrame() const { return m_RenderFrame; }

    /// 从 pool 分配一个 command buffer，返回引用（pool 为唯一所有者）。
    VulkanCommandBuffer &RequestCommandBuffer(vk::CommandBufferLevel level = vk::CommandBufferLevel::ePrimary);

    /// 重置整个 pool，所有 command buffer 回到初始状态并可复用。
    void ResetPool();

private:
    VulkanDevice &m_Device;
    vk::CommandPool m_Handle = nullptr;
    uint32_t m_QueueFamilyIndex = 0;

    /// 关联的 RenderFrame 指针（可选，用于 descriptor set 请求）。
    VulkanRenderFrame *m_RenderFrame = nullptr;

    std::vector<std::unique_ptr<VulkanCommandBuffer> > m_PrimaryCommandBuffers;
    uint32_t m_ActivePrimaryCount = 0;
    std::vector<std::unique_ptr<VulkanCommandBuffer> > m_SecondaryCommandBuffers;
    uint32_t m_ActiveSecondaryCount = 0;
};

} // namespace GE
