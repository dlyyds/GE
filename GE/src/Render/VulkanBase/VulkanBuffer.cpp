/* Copyright (c) 2022-2026, NVIDIA CORPORATION. All rights reserved.
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
 * @file VulkanBuffer.cpp
 * @brief 从 Vulkan-Samples 适配的 VulkanBuffer 实现。
 */

#include "Render/VulkanBase/VulkanBuffer.h"

namespace GE {

// ============================================================================
// VulkanBufferBuilder
// ============================================================================

VulkanBufferBuilder::VulkanBufferBuilder(vk::DeviceSize size) : Parent(vk::BufferCreateInfo{.size = size}) {
}

VulkanBufferBuilder &VulkanBufferBuilder::with_flags(vk::BufferCreateFlags flags) {
    create_info.flags = flags;
    return *this;
}

VulkanBufferBuilder &VulkanBufferBuilder::with_usage(vk::BufferUsageFlags usage) {
    create_info.usage = usage;
    return *this;
}

VulkanBufferBuilder &VulkanBufferBuilder::with_alignment(vk::DeviceSize alignment) {
    m_Alignment = alignment;
    return *this;
}

vk::DeviceSize VulkanBufferBuilder::get_alignment() const {
    return m_Alignment;
}

VulkanBuffer VulkanBufferBuilder::build(GE::VulkanDevice &device) const {
    return VulkanBuffer(device, *this);
}

VulkanBufferPtr VulkanBufferBuilder::build_unique(GE::VulkanDevice &device) const {
    return std::make_unique<VulkanBuffer>(device, *this);
}

// ============================================================================
// VulkanBuffer — 便捷构造函数（委托给 Builder）
// ============================================================================

VulkanBuffer::VulkanBuffer(VulkanDevice &device,
                           vk::DeviceSize size,
                           vk::BufferUsageFlags buffer_usage,
                           VmaMemoryUsage memory_usage,
                           VmaAllocationCreateFlags flags,
                           const std::vector<uint32_t> &queue_family_indices) : VulkanBuffer(device,
                                                                                             VulkanBufferBuilder(size)
                                                                                             .with_usage(buffer_usage)
                                                                                             .with_vma_usage(memory_usage)
                                                                                             .with_alignment(0)
                                                                                             .with_vma_flags(flags)
                                                                                             .with_queue_families(queue_family_indices)
                                                                                             .with_implicit_sharing_mode()) {
}

// ============================================================================
// VulkanBuffer — Builder 构造函数
// ============================================================================

VulkanBuffer::VulkanBuffer(VulkanDevice &device, VulkanBufferBuilder const &builder) : allocated::Allocated<vk::Buffer>{
                                                                                           builder.get_allocation_create_info(), &device},
                                                                                       m_Size(builder.get_create_info().size) {
    GetHandle() = create_buffer(builder.get_create_info(), builder.get_alignment());
    if (!builder.get_debug_name().empty()) {
        SetDebugName(builder.get_debug_name());
    }
}

// ============================================================================
// VulkanBuffer — 析构
// ============================================================================

VulkanBuffer::~VulkanBuffer() {
    destroy_buffer(GetHandle());
}

// ============================================================================
// VulkanBuffer — 静态工厂方法
// ============================================================================

VulkanBuffer VulkanBuffer::create_staging_buffer(VulkanDevice &device, vk::DeviceSize size, const void *data) {
    VulkanBufferBuilder builder(size);

    VulkanBuffer result = builder.with_vma_flags(VMA_ALLOCATION_CREATE_MAPPED_BIT | VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT)
        .with_usage(vk::BufferUsageFlagBits::eTransferSrc).build(device);

    if (data != nullptr) {
        result.update(data, size);
    }
    return result;
}

// ============================================================================
// VulkanBuffer — 方法
// ============================================================================

uint64_t VulkanBuffer::get_device_address() const {
    return GetDevice().GetHandle().getBufferAddressKHR({.buffer = GetHandle()});
}

vk::DeviceSize VulkanBuffer::get_size() const {
    return m_Size;
}

} // namespace GE