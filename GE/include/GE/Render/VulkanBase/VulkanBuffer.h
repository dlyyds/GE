#pragma once

#include <vulkan/vulkan.hpp>
#include "vk_mem_alloc.h"

#include <cstddef>
#include <cstring>

namespace GE {

/// GPU buffer backed by VMA-managed memory.
/// Supports host-visible (mapped) and device-local allocations.
class VulkanBuffer {
public:
    VulkanBuffer() = default;

    ~VulkanBuffer() = default;

    VulkanBuffer(const VulkanBuffer &) = delete;

    VulkanBuffer &operator=(const VulkanBuffer &) = delete;

    /// Create a buffer and allocate memory via VMA.
    /// @param allocator    the VMA allocator (from VulkanDevice).
    /// @param size         buffer size in bytes.
    /// @param usage        buffer usage flags (e.g. eVertexBuffer | eTransferDst).
    /// @param memory_usage VMA memory usage hint (default: VMA_MEMORY_USAGE_AUTO).
    /// @param flags        VMA allocation creation flags.
    ///                     Default: HOST_ACCESS_SEQUENTIAL_WRITE | MAPPED
    ///                     (host-visible, persistently mapped).
    void Init(VmaAllocator allocator, vk::DeviceSize size,
              vk::BufferUsageFlags usage,
              VmaMemoryUsage memory_usage = VMA_MEMORY_USAGE_AUTO,
              VmaAllocationCreateFlags flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT |
                                               VMA_ALLOCATION_CREATE_MAPPED_BIT);

    /// Destroy the buffer and free its memory via VMA.
    void Destroy();

    /// Upload data to the GPU buffer.
    /// The buffer must have been created with the MAPPED flag set
    /// so that the persistent mapping is immediately visible to the GPU.
    void Upload(const void *data, vk::DeviceSize size) const;

    [[nodiscard]] vk::Buffer GetBuffer() const { return m_Buffer; }
    [[nodiscard]] vk::DeviceSize GetSize() const { return m_Size; }

private:
    VmaAllocator m_Allocator = nullptr;
    VmaAllocation m_Allocation = nullptr;
    vk::Buffer m_Buffer = nullptr;
    void *m_MappedData = nullptr;   ///< Persistent mapping, valid if MAPPED flag was used.
    vk::DeviceSize m_Size = 0;
};

} // namespace GE
