//
// Created by Lenovo on 2026/6/3.
//

#include "Render/VulkanBase/VulkanBuffer.h"
#include "Debug/Assert.h"

#include <cstring>

namespace GE {

void VulkanBuffer::Init(VmaAllocator allocator, vk::DeviceSize size,
                        vk::BufferUsageFlags usage, VmaMemoryUsage memory_usage,
                        VmaAllocationCreateFlags flags) {
    m_Allocator = allocator;
    m_Size = size;

    VkBufferCreateInfo buffer_info{};
    buffer_info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    buffer_info.size = size;
    buffer_info.usage = static_cast<VkBufferUsageFlags>(usage);
    buffer_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

    VmaAllocationCreateInfo alloc_info{};
    alloc_info.usage = memory_usage;
    alloc_info.flags = flags;

    VmaAllocationInfo vma_alloc_info;
    VkBuffer buffer;
    vmaCreateBuffer(allocator, &buffer_info, &alloc_info, &buffer, &m_Allocation, &vma_alloc_info);

    m_Buffer = buffer;
    m_MappedData = vma_alloc_info.pMappedData;
}

void VulkanBuffer::Destroy() {
    if (m_Allocator && m_Buffer) {
        vmaDestroyBuffer(m_Allocator, static_cast<VkBuffer>(m_Buffer), m_Allocation);
    }
    m_Allocator = nullptr;
    m_Allocation = nullptr;
    m_Buffer = nullptr;
    m_MappedData = nullptr;
    m_Size = 0;
}

void VulkanBuffer::Upload(const void *data, vk::DeviceSize size) const {
    GE_CORE_ASSERT(m_MappedData && size <= m_Size);

    std::memcpy(m_MappedData, data, static_cast<size_t>(size));
}

} // namespace GE
