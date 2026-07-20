#pragma once

#include <vulkan/vulkan.hpp>

#include "Render/VulkanBase/VulkanResourceBase.h"

namespace GE {

class VulkanCommandPool;

/// Vulkan CommandBuffer 封装，参考 Vulkan-Samples CommandBuffer 设计。
/// 继承 VulkanResourceBase<vk::CommandBuffer>，获得句柄管理 + 调试命名支持。
class VulkanCommandBuffer : public VulkanResourceBase<vk::CommandBuffer> {
public:
    /// 从 pool 分配新的 command buffer。
    explicit VulkanCommandBuffer(VulkanCommandPool &pool,
                                 vk::CommandBufferLevel level = vk::CommandBufferLevel::ePrimary);

    /// 包装已有的 command buffer handle（由 pool 预分配）。
    VulkanCommandBuffer(VulkanCommandPool &pool,
                        vk::CommandBufferLevel level,
                        vk::CommandBuffer handle);

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

    [[nodiscard]] vk::CommandBufferLevel GetLevel() const { return m_Level; }
    [[nodiscard]] VulkanCommandPool     &GetPool() const { return m_Pool; }

private:
    VulkanCommandPool     &m_Pool;
    vk::CommandBufferLevel m_Level = vk::CommandBufferLevel::ePrimary;
};

} // namespace GE
