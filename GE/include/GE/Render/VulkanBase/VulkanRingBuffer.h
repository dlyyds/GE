#pragma once

#include <vulkan/vulkan.hpp>
#include "vk_mem_alloc.h"
#include "Render/VulkanBase/VulkanBuffer.h"

namespace GE {

/// 环形缓冲区：一个大的持久映射 buffer，
/// 每帧逐个分配每个 draw call 的 UBO 数据空间。
/// 每帧开始调用 Reset()，每个 draw call 调用 Allocate()。
class VulkanRingBuffer {
public:
    VulkanRingBuffer() = default;

    ~VulkanRingBuffer() = default;

    VulkanRingBuffer(const VulkanRingBuffer &) = delete;

    VulkanRingBuffer &operator=(const VulkanRingBuffer &) = delete;

    VulkanRingBuffer(VulkanRingBuffer &&) = default;

    VulkanRingBuffer &operator=(VulkanRingBuffer &&) = default;

    /// 初始化 ring buffer。
    /// @param alignment 每次分配的对齐值，传入 minUniformBufferOffsetAlignment。
    void Init(VmaAllocator allocator, vk::DeviceSize totalSize,
              vk::DeviceSize alignment = 64);

    void Destroy();

    void Reset();

    /// 分配 `size` 字节的空间，返回从 buffer 起始位置的字节偏移。
    /// 返回值会按 m_Alignment 对齐。
    /// 数据通过 GetMappedData() + offset 写入。
    vk::DeviceSize Allocate(vk::DeviceSize size, vk::DeviceSize alignment = 0);

    [[nodiscard]] vk::Buffer GetBuffer() const { return m_Buffer.GetBuffer(); }
    [[nodiscard]] void *GetMappedData() const { return m_MappedData; }
    [[nodiscard]] vk::DeviceSize GetTotalSize() const { return m_TotalSize; }
    [[nodiscard]] vk::DeviceSize GetCurrentOffset() const { return m_CurrentOffset; }

private:
    VulkanBuffer m_Buffer;
    void *m_MappedData = nullptr;
    vk::DeviceSize m_TotalSize = 0;
    vk::DeviceSize m_Alignment = 64;
    vk::DeviceSize m_CurrentOffset = 0;
};

} // namespace GE
