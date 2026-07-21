#include "Render/VulkanBase/VulkanCommandBuffer.h"
#include "Render/VulkanBase/VulkanCommandPool.h"
#include "Render/VulkanBase/VulkanDevice.h"

#include <tracy/Tracy.hpp>

#include <cassert>
#include <utility>

namespace GE {

VulkanCommandBuffer::VulkanCommandBuffer(VulkanCommandPool &pool, vk::CommandBufferLevel level) : VulkanResourceBase(nullptr, &pool.GetDevice()),
                                                                                                  m_Pool(pool),
                                                                                                  m_Level(level) {
    vk::CommandBufferAllocateInfo alloc_info{
        .commandPool = pool.GetHandle(),
        .level = level,
        .commandBufferCount = 1,
    };

    SetHandle(this->GetDevice().GetHandle().allocateCommandBuffers(alloc_info).front());
}

VulkanCommandBuffer::VulkanCommandBuffer(VulkanCommandPool &pool,
                                          vk::CommandBufferLevel level,
                                          vk::CommandBuffer handle) : VulkanResourceBase(handle, &pool.GetDevice()),
                                                                       m_Pool(pool),
                                                                       m_Level(level) {
}

VulkanCommandBuffer::VulkanCommandBuffer(VulkanCommandBuffer &&other) noexcept : VulkanResourceBase(std::move(other)),
                                                                                 m_Pool(other.m_Pool),
                                                                                 m_Level(std::exchange(
                                                                                     other.m_Level, vk::CommandBufferLevel::ePrimary)) {
}

VulkanCommandBuffer::~VulkanCommandBuffer() {
    if (HasHandle()) {
        auto device = this->GetDevice().GetHandle();
        device.freeCommandBuffers(m_Pool.GetHandle(), GetHandle());
    }
}

void VulkanCommandBuffer::Begin(vk::CommandBufferUsageFlags flags,
                                VulkanCommandBuffer *primary_cmd_buf) {
    ZoneScoped;
    vk::CommandBufferBeginInfo begin_info{.flags = flags};

    if (m_Level == vk::CommandBufferLevel::eSecondary) {
        assert(primary_cmd_buf && "A primary command buffer must be provided for secondary command buffers");

        // When secondary, the inheritance info would be set here
        // For now, just begin without inheritance (caller sets up via extended API if needed)
        vk::CommandBufferInheritanceInfo inheritance{};
        begin_info.pInheritanceInfo = &inheritance;
    }

    GetHandle().begin(begin_info);
}

void VulkanCommandBuffer::End() {
    ZoneScoped;
    GetHandle().end();
}

void VulkanCommandBuffer::Reset() {
    ZoneScoped;
    GetHandle().reset(vk::CommandBufferResetFlagBits::eReleaseResources);
}

} // namespace GE
