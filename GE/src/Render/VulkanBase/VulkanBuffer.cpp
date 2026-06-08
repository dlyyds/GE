//
// Created by Lenovo on 2026/6/3.
//

#define VULKAN_HPP_DISPATCH_LOADER_DYNAMIC 1
#include "Render/VulkanBase/VulkanBuffer.h"
#include "Render/VulkanBase/VulkanDevice.h"

#include "Debug/Assert.h"

#include <cassert>
#include <stdexcept>

namespace GE {

void VulkanBuffer::Init(vk::Device device, vk::PhysicalDevice gpu, vk::DeviceSize size,
                        vk::BufferUsageFlags usage, vk::MemoryPropertyFlags memory_properties) {
    m_Device = device;
    m_Size = size;

    vk::BufferCreateInfo buffer_info{.size = size, .usage = usage, .sharingMode = vk::SharingMode::eExclusive};
    m_Buffer = device.createBuffer(buffer_info);

    vk::MemoryRequirements mem_req = device.getBufferMemoryRequirements(m_Buffer);

    uint32_t memory_type = VulkanDevice::FindMemoryType(gpu, mem_req.memoryTypeBits, memory_properties);

    vk::MemoryAllocateInfo alloc_info{.allocationSize = mem_req.size, .memoryTypeIndex = memory_type};
    m_Memory = device.allocateMemory(alloc_info);

    device.bindBufferMemory(m_Buffer, m_Memory, 0);

    // Persistent map for efficient host → GPU uploads.
    m_MappedData = device.mapMemory(m_Memory, 0, mem_req.size);
}

void VulkanBuffer::Destroy() {
    if (m_Device) {
        if (m_MappedData)
            m_Device.unmapMemory(m_Memory);
        if (m_Memory)
            m_Device.freeMemory(m_Memory);
        if (m_Buffer)
            m_Device.destroyBuffer(m_Buffer);
    } else {
        GE_CORE_ERROR("m_Device is nullptr");
    }
    m_MappedData = nullptr;
    m_Buffer = nullptr;
    m_Memory = nullptr;
    m_Size = 0;
    m_Device = nullptr;
}

void VulkanBuffer::Upload(const void *data, vk::DeviceSize size) const {
    GE_CORE_ASSERT(m_MappedData && size <= m_Size);

    std::memcpy(m_MappedData, data, static_cast<size_t>(size));
}

} // namespace GE
