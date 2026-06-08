//
// Created by Lenovo on 2026/6/8.
//

#include "Render/VulkanBase/VulkanRingBuffer.h"
#include "Debug/Assert.h"

#include <cstring>

namespace GE {

void VulkanRingBuffer::Init(VmaAllocator allocator, vk::DeviceSize totalSize) {
    m_TotalSize = totalSize;
    m_CurrentOffset = 0;

    m_Buffer.Init(allocator, totalSize,
                  vk::BufferUsageFlagBits::eUniformBuffer,
                  VMA_MEMORY_USAGE_AUTO,
                  VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT |
                  VMA_ALLOCATION_CREATE_MAPPED_BIT);

    m_MappedData = m_Buffer.GetMappedData();
}

void VulkanRingBuffer::Destroy() {
    m_Buffer.Destroy();
    m_MappedData = nullptr;
    m_TotalSize = 0;
    m_CurrentOffset = 0;
}

void VulkanRingBuffer::Reset() {
    m_CurrentOffset = 0;
}

vk::DeviceSize VulkanRingBuffer::Allocate(vk::DeviceSize size, vk::DeviceSize alignment) {
    // 将当前偏移按对齐值向上取整
    vk::DeviceSize alignedOffset = (m_CurrentOffset + alignment - 1) & ~(alignment - 1);

    GE_CORE_ASSERT(alignedOffset + size <= m_TotalSize,
                   "VulkanRingBuffer out of memory");

    m_CurrentOffset = alignedOffset + size;
    return alignedOffset;
}

} // namespace GE
