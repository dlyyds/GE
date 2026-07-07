#include "../../../include/GE/Render/VulkanBase/VulkanPerFrame.h"

#include <cassert>

namespace GE {

void VulkanPerFrame::Init(vk::Device device, uint32_t graphicsQueueIndex) {
    m_SubmitFence = device.createFence(vk::FenceCreateInfo{.flags = vk::FenceCreateFlagBits::eSignaled});

    vk::CommandPoolCreateInfo cmd_pool_info{
        .flags = vk::CommandPoolCreateFlagBits::eTransient,
        .queueFamilyIndex = graphicsQueueIndex,
    };
    m_CommandPool = device.createCommandPool(cmd_pool_info);

    vk::CommandBufferAllocateInfo cmd_buf_info{
        .commandPool = m_CommandPool,
        .level = vk::CommandBufferLevel::ePrimary,
        .commandBufferCount = 1,
    };
    m_CommandBuffer = device.allocateCommandBuffers(cmd_buf_info)[0];

    m_ReleaseSemaphore = device.createSemaphore(vk::SemaphoreCreateInfo{});
    // m_AcquireSemaphore is created on first AcquireNextImage — starts as nullptr.
}

void VulkanPerFrame::WaitAndResetFence(vk::Device device) {
    if (m_SubmitFence) {
        vk::Result result = device.waitForFences(m_SubmitFence, true, UINT64_MAX);
        assert(result == vk::Result::eSuccess);
        device.resetFences(m_SubmitFence);
    }
}

void VulkanPerFrame::ResetCommandPool(vk::Device device) {
    if (m_CommandPool) {
        device.resetCommandPool(m_CommandPool);
    }
}

void VulkanPerFrame::Destroy(vk::Device device) {
    if (m_ReleaseSemaphore)
        device.destroySemaphore(m_ReleaseSemaphore);
    if (m_AcquireSemaphore)
        device.destroySemaphore(m_AcquireSemaphore);
    if (m_CommandPool)
        device.destroyCommandPool(m_CommandPool);
    if (m_SubmitFence)
        device.destroyFence(m_SubmitFence);
    m_SubmitFence = nullptr;
    m_CommandPool = nullptr;
    m_CommandBuffer = nullptr;
    m_AcquireSemaphore = nullptr;
    m_ReleaseSemaphore = nullptr;
}

} // namespace GE
