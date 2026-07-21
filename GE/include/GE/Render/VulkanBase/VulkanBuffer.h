/* Copyright (c) 2021-2026, NVIDIA CORPORATION. All rights reserved.
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Licensed under the Apache License, Version 2.0 the "License";
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

/**
 * @file VulkanBuffer.h
 * @brief 从 Vulkan-Samples 适配的 VulkanBuffer（VMA 管理 Buffer）+ Builder + staging buffer。
 *
 * 替换早期基于 C API 的 VulkanBuffer 类，提供：
 * - VulkanBufferBuilder：Builder 模式创建 Buffer
 * - VulkanBuffer：RAII 风格的 VMA 托管 vk::Buffer
 * - create_staging_buffer：便捷创建 staging buffer 并上传数据
 */

#pragma once

#include "Render/VulkanBase/BuilderBase.h"
#include "Render/VulkanBase/VulkanAllocated.h"
#include <Render/VulkanBase/VulkanDevice.h>

#include <vulkan/vulkan.hpp>
#include <vk_mem_alloc.h>

#include <cstdint>
#include <memory>
#include <vector>

namespace GE {

using VulkanBufferPtr = std::unique_ptr<class VulkanBuffer>;

// ============================================================================
// VulkanBufferBuilder — Builder 模式创建 VulkanBuffer
// ============================================================================

class VulkanBufferBuilder : public allocated::BuilderBase<VulkanBufferBuilder, vk::BufferCreateInfo> {
private:
    using Parent = allocated::BuilderBase<VulkanBufferBuilder, vk::BufferCreateInfo>;

public:
    /** @brief 使用指定大小构造 Builder */
    explicit VulkanBufferBuilder(vk::DeviceSize size);

    VulkanBufferBuilder &with_flags(vk::BufferCreateFlags flags);

    VulkanBufferBuilder &with_usage(vk::BufferUsageFlags usage);

    VulkanBufferBuilder &with_alignment(vk::DeviceSize alignment);

    /** @brief 获取对齐要求 */
    vk::DeviceSize get_alignment() const;

    VulkanBuffer build(GE::VulkanDevice &device) const;

    VulkanBufferPtr build_unique(GE::VulkanDevice &device) const;

private:
    vk::DeviceSize m_Alignment{0};
};

// ============================================================================
// VulkanBuffer — RAII VMA 托管的 VkBuffer
// ============================================================================

class VulkanBuffer : public allocated::Allocated<vk::Buffer> {
public:
    using BufferUsageFlagsType = vk::BufferUsageFlags;
    using DeviceSizeType = vk::DeviceSize;

public:
    // ======================================================================
    // 静态工厂方法
    // ======================================================================

    /** @brief 创建 staging buffer 并上传数据 */
    static VulkanBuffer create_staging_buffer(VulkanDevice &device, vk::DeviceSize size, const void *data);

    /** @brief 创建 staging buffer 并上传 vector 数据 */
    template <typename T>
    static VulkanBuffer create_staging_buffer(VulkanDevice &device, std::vector<T> const &data);

    /** @brief 创建 staging buffer 并上传单个对象数据 */
    template <typename T>
    static VulkanBuffer create_staging_buffer(VulkanDevice &device, const T &data);

    // ======================================================================
    // 构造 / 析构
    // ======================================================================

    VulkanBuffer() = delete;

    VulkanBuffer(const VulkanBuffer &) = delete;

    VulkanBuffer(VulkanBuffer &&other) = default;

    VulkanBuffer &operator=(const VulkanBuffer &) = delete;

    VulkanBuffer &operator=(VulkanBuffer &&) = default;

    /**
     * @brief 便捷构造函数：使用直接参数创建 Buffer
     * @param device                 Vulkan 设备
     * @param size                   缓冲区大小（字节）
     * @param buffer_usage           Buffer 使用标志
     * @param memory_usage           VMA 内存使用提示（默认 AUTO）
     * @param flags                  VMA allocation 创建标志
     * @param queue_family_indices   可选的队列族索引
     */
    VulkanBuffer(VulkanDevice &device,
                 vk::DeviceSize size,
                 vk::BufferUsageFlags buffer_usage,
                 VmaMemoryUsage memory_usage = VMA_MEMORY_USAGE_AUTO,
                 VmaAllocationCreateFlags flags = VMA_ALLOCATION_CREATE_MAPPED_BIT |
                                                  VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT,
                 const std::vector<uint32_t> &queue_family_indices = {});

    /** @brief Builder 构造函数 */
    VulkanBuffer(VulkanDevice &device, VulkanBufferBuilder const &builder);

    ~VulkanBuffer();

    // ======================================================================
    // 方法
    // ======================================================================

    /**
     * @brief 获取 buffer 的设备地址
     * @note 需要 buffer 创建时带有 VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT 标志
     */
    uint64_t get_device_address() const;

    /** @brief 获取 buffer 大小 */
    vk::DeviceSize get_size() const;

private:
    vk::DeviceSize m_Size{0};
};

// ============================================================================
// 模板方法实现（必须内联在头文件中）
// ============================================================================

template <typename T>
inline VulkanBuffer VulkanBuffer::create_staging_buffer(VulkanDevice &device, const T &data) {
    return create_staging_buffer(device, sizeof(T), &data);
}

template <typename T>
inline VulkanBuffer VulkanBuffer::create_staging_buffer(VulkanDevice &device, std::vector<T> const &data) {
    return create_staging_buffer(device, data.size() * sizeof(T), data.data());
}

} // namespace GE