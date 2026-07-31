#include "Render/VulkanBase/VulkanCommandPool.h"
#include "Render/VulkanBase/VulkanCommandBuffer.h"
#include "Render/VulkanBase/VulkanDevice.h"

#include <stdexcept>
#include <utility>

namespace GE {

VulkanCommandPool::VulkanCommandPool(VulkanDevice &device, uint32_t queue_family_index,
                                     vk::CommandPoolCreateFlags flags) :
    m_Device(device),
    m_QueueFamilyIndex(queue_family_index) {
    m_Handle = device.GetHandle().createCommandPool(vk::CommandPoolCreateInfo{
        .flags = flags,
        .queueFamilyIndex = queue_family_index,
    });
}

VulkanCommandPool::VulkanCommandPool(VulkanCommandPool &&other) noexcept :
    m_Device(other.m_Device),
    m_Handle(std::exchange(other.m_Handle, nullptr)),
    m_QueueFamilyIndex(std::exchange(other.m_QueueFamilyIndex, 0)),
    m_RenderFrame(std::exchange(other.m_RenderFrame, nullptr)),
    m_PrimaryCommandBuffers(std::move(other.m_PrimaryCommandBuffers)),
    m_ActivePrimaryCount(std::exchange(other.m_ActivePrimaryCount, 0)),
    m_SecondaryCommandBuffers(std::move(other.m_SecondaryCommandBuffers)),
    m_ActiveSecondaryCount(std::exchange(other.m_ActiveSecondaryCount, 0)) {
}

VulkanCommandPool::~VulkanCommandPool() {
    if (m_Handle) {
        // 先释放所有 command buffer（unique_ptr 析构会调用 freeCommandBuffers）
        m_PrimaryCommandBuffers.clear();
        m_SecondaryCommandBuffers.clear();

        // 再销毁 pool（此时不再有活跃的 command buffer）
        m_Device.GetHandle().destroyCommandPool(m_Handle);
    }
}

VulkanCommandBuffer &VulkanCommandPool::RequestCommandBuffer(vk::CommandBufferLevel level) {
    auto &pool = (level == vk::CommandBufferLevel::ePrimary) ? m_PrimaryCommandBuffers : m_SecondaryCommandBuffers;
    auto &activeCount = (level == vk::CommandBufferLevel::ePrimary) ? m_ActivePrimaryCount : m_ActiveSecondaryCount;

    // 如果有回收的 command buffer，直接复用
    if (activeCount < pool.size()) {
        return *pool[activeCount++];
    }

    // 否则分配新的
    auto cmd = std::make_unique<VulkanCommandBuffer>(*this, level);
    auto &ref = *cmd;
    pool.push_back(std::move(cmd));
    activeCount++;
    return ref;
}

void VulkanCommandPool::ResetPool() {
    if (m_Handle) {
        m_Device.GetHandle().resetCommandPool(m_Handle);
    }
    m_ActivePrimaryCount   = 0;
    m_ActiveSecondaryCount = 0;
}

} // namespace GE
