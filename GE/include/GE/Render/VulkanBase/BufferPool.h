/* Copyright (c) 2019-2025, Arm Limited and Contributors
 * Copyright (c) 2024-2025, NVIDIA CORPORATION. All rights reserved.
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
 * @file BufferPool.h
 * @brief 从 Vulkan-Samples 适配的 BufferPool / BufferBlock / BufferAllocation
 *
 * 提供线性分配器风格的 buffer 子分配机制：
 * - BufferAllocation：对 VulkanBuffer 中一段区域的视图（offset + size）
 * - BufferBlock：一个底层 VulkanBuffer，内部通过 offset 递增分配子区域
 * - BufferPool：管理多个 BufferBlock，按需扩容
 *
 * GE 统一使用 vulkan.hpp C++ 绑定，因此移除了 BindingType 模板参数。
 */

#pragma once

#include "Render/VulkanBase/VulkanBuffer.h"
#include "Render/VulkanBase/VulkanCommon.h"
#include "Render/VulkanBase/VulkanDevice.h"

#include <algorithm>
#include <cassert>
#include <cstdint>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

#include <vulkan/vulkan.hpp>
#include <vk_mem_alloc.h>

namespace GE {

/**
 * @brief 一个 Vulkan buffer 子分配块。
 *        不同的 BufferAllocation（offset 和 size 不同）可能来自同一个底层 VulkanBuffer。
 */
class BufferAllocation {
public:
    BufferAllocation() = default;

    BufferAllocation(const BufferAllocation &) = default;

    BufferAllocation(BufferAllocation &&) = default;

    BufferAllocation &operator=(const BufferAllocation &) = default;

    BufferAllocation &operator=(BufferAllocation &&) = default;

    BufferAllocation(VulkanBuffer &buffer, vk::DeviceSize size, vk::DeviceSize offset);

    /** @brief 是否为空分配（size == 0 或 buffer 为空） */
    bool empty() const;

    /** @brief 获取底层 VulkanBuffer */
    VulkanBuffer &get_buffer();

    /** @brief 获取子分配偏移 */
    vk::DeviceSize get_offset() const;

    /** @brief 获取子分配大小 */
    vk::DeviceSize get_size() const;

    /** @brief 更新 buffer 数据 */
    void update(const std::vector<uint8_t> &data, uint32_t offset = 0);

    /** @brief 更新单个对象数据到 buffer */
    template <typename T>
    void update(const T &value, uint32_t offset = 0) {
        assert(m_Buffer && "Invalid buffer pointer");
        if (static_cast<vk::DeviceSize>(offset) + sizeof(T) <= m_Size) {
            m_Buffer->update(&value, sizeof(T), static_cast<size_t>(m_Offset) + offset);
        } else {
            GE_CORE_ERROR("BufferAllocation::update: 数据超出分配范围，忽略更新");
        }
    }

    template <typename T>
    void update(const std::vector<T> &data, uint32_t offset = 0) {
        vk::DeviceSize dataSize = static_cast<vk::DeviceSize>(data.size()) * sizeof(T);
        if (static_cast<vk::DeviceSize>(offset) + dataSize <= m_Size) {
            m_Buffer->update(data.data(), static_cast<size_t>(dataSize),
                             static_cast<size_t>(m_Offset) + offset);
        } else {
            GE_CORE_ERROR("BufferAllocation::update: 数据超出分配范围，忽略更新");
        }
    }

private:
    VulkanBuffer *m_Buffer = nullptr;
    vk::DeviceSize m_Offset = 0;
    vk::DeviceSize m_Size = 0;
};

// ============================================================================
// BufferBlock — 管理一个底层 VulkanBuffer 的线性子分配
// ============================================================================

/**
 * @brief 辅助类，管理同一个底层 VulkanBuffer 的多次子分配。
 *
 * 通过递增 offset 的方式线性分配，每次 allocate 返回一个 BufferAllocation 视图。
 * 可通过 reset() 重置 offset 以复用底层 buffer。
 */
class BufferBlock {
public:
    BufferBlock() = delete;

    BufferBlock(const BufferBlock &) = delete;

    BufferBlock(BufferBlock &&) = default;

    BufferBlock &operator=(const BufferBlock &) = delete;

    BufferBlock &operator=(BufferBlock &&) = default;

    BufferBlock(VulkanDevice &device, vk::DeviceSize size, vk::BufferUsageFlags usage, VmaMemoryUsage memory_usage);

    /**
     * @brief 从当前 buffer block 中分配一段空间。
     * @return 若空间不足，返回空的 BufferAllocation。
     */
    BufferAllocation allocate(vk::DeviceSize size);

    /**
     * @brief 检查能否分配指定大小的空间。
     * @param size 需要分配的字节数。
     * @return true 表示可以分配。
     */
    bool can_allocate(vk::DeviceSize size) const;

    /** @brief 获取底层 buffer 大小 */
    vk::DeviceSize get_size() const;

    /** @brief 重置 offset，复用底层 buffer（不会清空数据） */
    void reset();

    /**
     * @brief 设置调试名称（作用于底层 VulkanBuffer）。
     *
     * 便于在 RenderDoc / Nsight 等调试工具中识别此 buffer block。
     */
    void SetDebugName(const std::string &name) { m_Buffer.SetDebugName(name); }

private:
    /** @brief 计算当前对齐后的 offset */
    vk::DeviceSize aligned_offset() const;

    /** @brief 根据 usage 确定对齐要求 */
    vk::DeviceSize determine_alignment(vk::BufferUsageFlags usage, vk::PhysicalDeviceLimits const &limits) const;

    VulkanBuffer m_Buffer;
    vk::DeviceSize m_Alignment = 0; ///< 内存对齐（根据 usage 不同而变化）
    vk::DeviceSize m_Offset = 0; ///< 当前 offset，每次分配后递增
};

// ============================================================================
// BufferPool — 管理多个 BufferBlock 的缓冲池
// ============================================================================

/**
 * @brief 针对特定 usage 的 BufferBlock 池。
 *
 * BufferPool 是一个线性分配器，按需分配 BufferBlock。
 * 每个 BufferBlock 对应一个 VkBuffer，内部可以分配多个子区域。
 *
 * 新帧开始时可以调用 reset() 重置所有 block 的 offset。
 * 最小分配块大小为 256KB，若请求超过该大小则创建专用 block。
 */
class BufferPool {
public:
    BufferPool(VulkanDevice &device,
               vk::DeviceSize block_size,
               vk::BufferUsageFlags usage,
               VmaMemoryUsage memory_usage = VMA_MEMORY_USAGE_CPU_TO_GPU);

    /**
     * @brief 请求一个能容纳 minimum_size 的 BufferBlock。
     * @param minimal  若为 true，则只查找大小恰好等于 minimum_size 的 block。
     * @return 引用到可用的 BufferBlock。
     */
    BufferBlock &request_buffer_block(vk::DeviceSize minimum_size, bool minimal = false);

    /** @brief 重置所有 block 的 offset（复用底层 buffer，不清除数据） */
    void reset();

    /**
     * @brief 设置调试名称前缀。
     *
     * 为所有已存在的 BufferBlock 设置调试名（格式：`name_BlockN`），
     * 同时存储前缀，后续新建的 block 也会自动加上该前缀。
     *
     * 便于在 RenderDoc / Nsight 等调试工具中识别此缓冲池的 buffer。
     */
    void SetDebugName(const std::string &name);

private:
    VulkanDevice &m_Device;
    std::vector<std::unique_ptr<BufferBlock> > m_BufferBlocks; ///< 所有已分配的 block（使用 unique_ptr 保证 vector 扩容时地址不变）
    vk::DeviceSize m_BlockSize = 0; ///< 默认最小 block 大小
    vk::BufferUsageFlags m_Usage;
    VmaMemoryUsage m_MemoryUsage{};
    std::string m_DebugName; ///< 调试名称前缀，新建 block 时自动应用
};

// ============================================================================
// BufferAllocation 实现
// ============================================================================

inline BufferAllocation::BufferAllocation(VulkanBuffer &buffer, vk::DeviceSize size, vk::DeviceSize offset) : m_Buffer(&buffer),
    m_Offset(offset),
    m_Size(size) {
}

inline bool BufferAllocation::empty() const {
    return m_Size == 0 || m_Buffer == nullptr;
}

inline VulkanBuffer &BufferAllocation::get_buffer() {
    assert(m_Buffer && "Invalid buffer pointer");
    return *m_Buffer;
}

inline vk::DeviceSize BufferAllocation::get_offset() const {
    return m_Offset;
}

inline vk::DeviceSize BufferAllocation::get_size() const {
    return m_Size;
}

inline void BufferAllocation::update(const std::vector<uint8_t> &data, uint32_t offset) {
    assert(m_Buffer && "Invalid buffer pointer");

    if (offset + data.size() <= m_Size) {
        m_Buffer->update(data, static_cast<size_t>(m_Offset) + offset);
    } else {
        GE_CORE_ERROR("BufferAllocation::update: 数据超出分配范围，忽略更新");
    }
}

// ============================================================================
// BufferBlock 实现
// ============================================================================

inline BufferBlock::BufferBlock(VulkanDevice &device, vk::DeviceSize size, vk::BufferUsageFlags usage, VmaMemoryUsage memory_usage) : m_Buffer(
    device, size, usage, memory_usage) {
    m_Alignment = determine_alignment(usage, device.GetGpu().GetProperties().limits);
}

inline BufferAllocation BufferBlock::allocate(vk::DeviceSize size) {
    if (can_allocate(size)) {
        auto aligned = aligned_offset();
        m_Offset = aligned + size;
        return BufferAllocation{m_Buffer, size, aligned};
    }

    // 空间不足，返回空分配
    return BufferAllocation{};
}

inline bool BufferBlock::can_allocate(vk::DeviceSize size) const {
    assert(size > 0 && "Allocation size must be greater than zero");
    return (aligned_offset() + size <= m_Buffer.get_size());
}

inline vk::DeviceSize BufferBlock::get_size() const {
    return m_Buffer.get_size();
}

inline void BufferBlock::reset() {
    m_Offset = 0;
}

inline vk::DeviceSize BufferBlock::aligned_offset() const {
    return (m_Offset + m_Alignment - 1) & ~(m_Alignment - 1);
}

inline vk::DeviceSize BufferBlock::determine_alignment(vk::BufferUsageFlags usage, vk::PhysicalDeviceLimits const &limits) const {
    if (usage == vk::BufferUsageFlagBits::eUniformBuffer) {
        return limits.minUniformBufferOffsetAlignment;
    } else if (usage == vk::BufferUsageFlagBits::eStorageBuffer) {
        return limits.minStorageBufferOffsetAlignment;
    } else if (usage == vk::BufferUsageFlagBits::eUniformTexelBuffer) {
        return limits.minTexelBufferOffsetAlignment;
    } else if (usage == vk::BufferUsageFlagBits::eIndexBuffer ||
               usage == vk::BufferUsageFlagBits::eVertexBuffer ||
               usage == vk::BufferUsageFlagBits::eIndirectBuffer) {
        // 用于计算 offset，要求值为 2 的幂
        return 16;
    } else {
        throw std::runtime_error("BufferBlock::determine_alignment: 不支持的 BufferUsage");
    }
}

// ============================================================================
// BufferPool 实现
// ============================================================================

inline BufferPool::BufferPool(VulkanDevice &device,
                              vk::DeviceSize block_size,
                              vk::BufferUsageFlags usage,
                              VmaMemoryUsage memory_usage) : m_Device(device),
                                                             m_BlockSize(block_size),
                                                             m_Usage(usage),
                                                             m_MemoryUsage(memory_usage) {
}

inline void BufferPool::SetDebugName(const std::string &name) {
    m_DebugName = name;
    for (size_t i = 0; i < m_BufferBlocks.size(); ++i) {
        m_BufferBlocks[i]->SetDebugName(name + "_Block" + std::to_string(i));
    }
}

inline BufferBlock &BufferPool::request_buffer_block(vk::DeviceSize minimum_size, bool minimal) {
    // 查找能容纳 minimum_size 的 block
    auto it = std::find_if(m_BufferBlocks.begin(), m_BufferBlocks.end(),
                           [&minimum_size, minimal](auto const &buffer_block) {
                               if (minimal) {
                                   return (buffer_block->get_size() == minimum_size) && buffer_block->can_allocate(minimum_size);
                               }
                               return buffer_block->can_allocate(minimum_size);
                           });

    if (it == m_BufferBlocks.end()) {
        GE_CORE_INFO("BufferPool: 创建第 {} 个 BufferBlock（usage = {}）",
                     m_BufferBlocks.size(), vk::to_string(m_Usage));

        vk::DeviceSize new_block_size = minimal ? minimum_size : std::max(m_BlockSize, minimum_size);

        // 创建新 block
        it = m_BufferBlocks.emplace(m_BufferBlocks.end(),
                                    std::make_unique<BufferBlock>(m_Device, new_block_size, m_Usage, m_MemoryUsage));

        // 若设置了调试名前缀，为新 block 自动命名
        if (!m_DebugName.empty()) {
            (*it)->SetDebugName(m_DebugName + "_Block" + std::to_string(m_BufferBlocks.size() - 1));
        }
    }

    return *it->get();
}

inline void BufferPool::reset() {
    for (auto &buffer_block : m_BufferBlocks) {
        buffer_block->reset();
    }
}

} // namespace GE