#pragma once


#include <vulkan/vulkan.hpp>

#include <cstddef>
#include <cstring>

namespace GE {

class VulkanBuffer {
public:
    VulkanBuffer() = default;

    ~VulkanBuffer() = default;

    VulkanBuffer(const VulkanBuffer &) = delete;

    VulkanBuffer &operator=(const VulkanBuffer &) = delete;

    void Init(vk::Device device, vk::PhysicalDevice gpu, vk::DeviceSize size,
              vk::BufferUsageFlags usage, vk::MemoryPropertyFlags memory_properties);

    void Destroy();

    /// Upload data to the GPU buffer.
    /// The buffer must have been created with HOST_VISIBLE | HOST_COHERENT
    /// so that the persistent mapping is immediately visible to the GPU.
    void Upload(const void *data, vk::DeviceSize size) const;

    [[nodiscard]] vk::Buffer GetBuffer() const { return m_Buffer; }
    [[nodiscard]] vk::DeviceSize GetSize() const { return m_Size; }

private:
    vk::Device m_Device = nullptr;
    vk::Buffer m_Buffer = nullptr;
    vk::DeviceMemory m_Memory = nullptr;
    void *m_MappedData = nullptr;   ///< Persistent mapping, valid after Init.
    vk::DeviceSize m_Size = 0;
};

} // namespace GE
