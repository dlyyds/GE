#pragma once

#include <vulkan/vulkan.hpp>

namespace GE {

class VulkanCommandPool;

/// Vulkan CommandBuffer 封装，参考 Vulkan-Samples CommandBuffer 设计。
/// 管理单个 command buffer 的生命周期（分配 → 录制 → 回收）。
class VulkanCommandBuffer {
public:
    explicit VulkanCommandBuffer(VulkanCommandPool &pool,
                                 vk::CommandBufferLevel level = vk::CommandBufferLevel::ePrimary);

    VulkanCommandBuffer(const VulkanCommandBuffer &) = delete;

    VulkanCommandBuffer(VulkanCommandBuffer &&other) noexcept;

    VulkanCommandBuffer &operator=(const VulkanCommandBuffer &) = delete;

    VulkanCommandBuffer &operator=(VulkanCommandBuffer &&) = delete;

    ~VulkanCommandBuffer();

    /// 开始录制 command buffer。
    void Begin(vk::CommandBufferUsageFlags flags,
               VulkanCommandBuffer *primary_cmd_buf = nullptr);

    /// 结束录制。
    void End();

    /// 重置 command buffer。
    void Reset();

    [[nodiscard]] vk::CommandBuffer GetHandle() const { return m_Handle; }
    [[nodiscard]] vk::CommandBufferLevel GetLevel() const { return m_Level; }
    [[nodiscard]] VulkanCommandPool &GetPool() const { return m_Pool; }

private:
    VulkanCommandPool &m_Pool;
    vk::CommandBuffer m_Handle = nullptr;
    vk::CommandBufferLevel m_Level = vk::CommandBufferLevel::ePrimary;
};

} // namespace GE
