#include "../../../include/GE/Render/VulkanBase/VulkanCommandBuffer.h"
#include "../../../include/GE/Render/VulkanBase/VulkanCommandPool.h"

#include <cassert>
#include <utility>

namespace GE {

VulkanCommandBuffer::VulkanCommandBuffer(VulkanCommandPool &pool, vk::CommandBufferLevel level) :
    m_Pool(pool),
    m_Level(level) {
    auto device = pool.GetHandle().getDevice();

    vk::CommandBufferAllocateInfo alloc_info{
        .commandPool        = pool.GetHandle(),
        .level              = level,
        .commandBufferCount = 1,
    };

    m_Handle = device.allocateCommandBuffers(alloc_info).front();
}

VulkanCommandBuffer::VulkanCommandBuffer(VulkanCommandBuffer &&other) noexcept :
    m_Pool(other.m_Pool),
    m_Handle(std::exchange(other.m_Handle, nullptr)),
    m_Level(std::exchange(other.m_Level, vk::CommandBufferLevel::ePrimary)) {
}

VulkanCommandBuffer::~VulkanCommandBuffer() {
    if (m_Handle) {
        auto device = m_Pool.GetHandle().getDevice();
        device.freeCommandBuffers(m_Pool.GetHandle(), m_Handle);
    }
}

void VulkanCommandBuffer::Begin(vk::CommandBufferUsageFlags flags,
                                VulkanCommandBuffer *primary_cmd_buf) {
    vk::CommandBufferBeginInfo begin_info{.flags = flags};

    if (m_Level == vk::CommandBufferLevel::eSecondary) {
        assert(primary_cmd_buf && "A primary command buffer must be provided for secondary command buffers");

        // When secondary, the inheritance info would be set here
        // For now, just begin without inheritance (caller sets up via extended API if needed)
        vk::CommandBufferInheritanceInfo inheritance{};
        begin_info.pInheritanceInfo = &inheritance;
    }

    m_Handle.begin(begin_info);
}

void VulkanCommandBuffer::End() {
    m_Handle.end();
}

void VulkanCommandBuffer::Reset() {
    m_Handle.reset(vk::CommandBufferResetFlagBits::eReleaseResources);
}

} // namespace GE
