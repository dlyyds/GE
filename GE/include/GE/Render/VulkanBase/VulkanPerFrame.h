#pragma once

#include <vulkan/vulkan.hpp>

namespace GE {

class VulkanPerFrame {
public:
    VulkanPerFrame() = default;

    ~VulkanPerFrame() = default;

    VulkanPerFrame(const VulkanPerFrame &) = delete;

    VulkanPerFrame &operator=(const VulkanPerFrame &) = delete;

    VulkanPerFrame(VulkanPerFrame &&) = default;

    VulkanPerFrame &operator=(VulkanPerFrame &&) = default;

    void Init(vk::Device device, uint32_t graphicsQueueIndex);

    void Destroy(vk::Device device);

    [[nodiscard]] vk::Fence GetSubmitFence() const { return m_SubmitFence; }
    [[nodiscard]] vk::CommandBuffer GetCommandBuffer() const { return m_CommandBuffer; }
    [[nodiscard]] vk::Semaphore GetAcquireSemaphore() const { return m_AcquireSemaphore; }
    [[nodiscard]] vk::Semaphore GetReleaseSemaphore() const { return m_ReleaseSemaphore; }

    bool HasSubmitFence() const { return m_SubmitFence != nullptr; }

    void WaitAndResetFence(vk::Device device);

    void ResetCommandPool(vk::Device device);

    // Acquire semaphore transfer (ownership moves to/from the swapchain's recycled pool)
    vk::Semaphore TakeAcquireSemaphore() {
        vk::Semaphore s = m_AcquireSemaphore;
        m_AcquireSemaphore = nullptr;
        return s;
    }

    void GiveAcquireSemaphore(vk::Semaphore sem) { m_AcquireSemaphore = sem; }

private:
    vk::Fence m_SubmitFence = nullptr;
    vk::CommandPool m_CommandPool = nullptr;
    vk::CommandBuffer m_CommandBuffer = nullptr;
    vk::Semaphore m_AcquireSemaphore = nullptr;
    vk::Semaphore m_ReleaseSemaphore = nullptr;
};

} // namespace GE
