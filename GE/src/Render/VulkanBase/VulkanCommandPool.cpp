#include "../../../include/GE/Render/VulkanBase/VulkanCommandPool.h"

#include <stdexcept>

namespace GE {

VulkanCommandPool::VulkanCommandPool(vk::Device device, uint32_t queue_family_index,
                                     vk::CommandPoolCreateFlags flags) :
    m_Device(device),
    m_QueueFamilyIndex(queue_family_index) {
    m_Handle = device.createCommandPool(vk::CommandPoolCreateInfo{
        .flags = flags,
        .queueFamilyIndex = queue_family_index,
    });
}

VulkanCommandPool::VulkanCommandPool(VulkanCommandPool &&other) noexcept :
    m_Device(std::exchange(other.m_Device, nullptr)),
    m_Handle(std::exchange(other.m_Handle, nullptr)),
    m_QueueFamilyIndex(std::exchange(other.m_QueueFamilyIndex, 0)),
    m_PrimaryCommandBuffers(std::move(other.m_PrimaryCommandBuffers)),
    m_ActivePrimaryCount(std::exchange(other.m_ActivePrimaryCount, 0)),
    m_SecondaryCommandBuffers(std::move(other.m_SecondaryCommandBuffers)),
    m_ActiveSecondaryCount(std::exchange(other.m_ActiveSecondaryCount, 0)) {
}

VulkanCommandPool::~VulkanCommandPool() {
    if (m_Handle) {
        // Command buffers are freed automatically when the pool is destroyed
        m_Device.destroyCommandPool(m_Handle);
    }
}

vk::CommandBuffer VulkanCommandPool::RequestCommandBuffer(vk::CommandBufferLevel level) {
    auto &pool = (level == vk::CommandBufferLevel::ePrimary) ? m_PrimaryCommandBuffers : m_SecondaryCommandBuffers;
    auto &activeCount = (level == vk::CommandBufferLevel::ePrimary) ? m_ActivePrimaryCount : m_ActiveSecondaryCount;

    // 如果有回收的 command buffer，直接复用
    if (activeCount < pool.size()) {
        return pool[activeCount++];
    }

    // 否则分配新的
    vk::CommandBufferAllocateInfo allocInfo{
        .commandPool = m_Handle,
        .level = level,
        .commandBufferCount = 1,
    };
    auto cmd = m_Device.allocateCommandBuffers(allocInfo)[0];
    pool.push_back(cmd);
    activeCount++;
    return cmd;
}

void VulkanCommandPool::ResetPool() {
    if (m_Handle) {
        m_Device.resetCommandPool(m_Handle);
    }
    m_ActivePrimaryCount   = 0;
    m_ActiveSecondaryCount = 0;
}

} // namespace GE
