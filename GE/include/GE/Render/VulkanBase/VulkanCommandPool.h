#pragma once

#include <vector>
#include <vulkan/vulkan.hpp>

namespace GE {

class VulkanDevice;

/// Vulkan CommandPool 封装，管理 command pool 及其分配的 command buffers。
/// 参考 Vulkan-Samples CommandPoolBase 设计，适配 GE 引擎风格。
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
    [[nodiscard]] VulkanDevice   &GetDevice() const { return m_Device; }
    [[nodiscard]] uint32_t        GetQueueFamilyIndex() const { return m_QueueFamilyIndex; }

    /// 从 pool 分配一个 command buffer。
    vk::CommandBuffer RequestCommandBuffer(vk::CommandBufferLevel level = vk::CommandBufferLevel::ePrimary);

    /// 重置整个 pool（所有分配的 command buffer 立即失效）。
    void ResetPool();

private:
    VulkanDevice   &m_Device;
    vk::CommandPool m_Handle          = nullptr;
    uint32_t        m_QueueFamilyIndex = 0;

    std::vector<vk::CommandBuffer> m_PrimaryCommandBuffers;
    uint32_t                       m_ActivePrimaryCount   = 0;
    std::vector<vk::CommandBuffer> m_SecondaryCommandBuffers;
    uint32_t                       m_ActiveSecondaryCount = 0;
};

} // namespace GE
