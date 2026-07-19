/* Copyright (c) 2019-2025, Arm Limited and Contributors
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
 * @file VulkanRenderFrame.h
 * @brief 每帧数据容器，适配自 Vulkan-Samples 的 RenderFrame。
 *
 * VulkanRenderFrame 整合了单帧渲染所需的所有资源：
 * - BufferPool（按 usage 分类，多线程）
 * - CommandPool（按 queue family 索引，多线程）
 * - FencePool / SemaphorePool
 * - DescriptorSet 缓存管理
 * - 持有 RenderTarget（swapchain 图像）
 *
 * 使用方式：
 * @code
 *   // 创建帧资源（每个 swapchain image 一个）
 *   VulkanRenderFrame frame(device, std::move(render_target), thread_count);
 *
 *   // 每帧渲染循环：
 *   frame.Reset();  // 重置所有池
 *
 *   // 分配 uniform buffer
 *   auto alloc = frame.AllocateBuffer(vk::BufferUsageFlagBits::eUniformBuffer, 256);
 *
 *   // 获取 command buffer
 *   auto &cmd_pool = frame.GetCommandPool(queue, CommandBufferResetMode::ResetPool, 0);
 *   auto cmd = cmd_pool.RequestCommandBuffer();
 *
 *   // 请求 descriptor set
 *   auto desc_set = frame.RequestDescriptorSet(layout, buffer_infos, image_infos, false, 0);
 * @endcode
 */

#pragma once

#include "Render/VulkanBase/BufferPool.h"
#include "Render/VulkanBase/RenderTarget.h"
#include "Render/VulkanBase/ResourceCaching.h"
#include "Render/VulkanBase/VulkanCommandPool.h"
#include "Render/VulkanBase/VulkanDescriptorPool.h"
#include "Render/VulkanBase/VulkanDescriptorSet.h"
#include "Render/VulkanBase/VulkanDescriptorSetLayout.h"
#include "Render/VulkanBase/VulkanDevice.h"
#include "Render/VulkanBase/VulkanFencePool.h"
#include "Render/VulkanBase/VulkanQueue.h"
#include "Render/VulkanBase/VulkanSemaphorePool.h"

#include <vulkan/vulkan.hpp>

#include <cstddef>
#include <map>
#include <memory>
#include <unordered_map>
#include <vector>

namespace GE {

// ============================================================================
// 策略枚举
// ============================================================================

/**
 * @brief Buffer 分配策略。
 */
enum class BufferAllocationStrategy {
    OneAllocationPerBuffer,       ///< 每次分配都创建独立的 VkBuffer
    MultipleAllocationsPerBuffer  ///< 多个子分配共享同一个 VkBuffer（默认）
};

/**
 * @brief DescriptorSet 管理策略。
 */
enum class DescriptorManagementStrategy {
    StoreInCache,     ///< 缓存已创建的 descriptor set，避免重复创建（默认）
    CreateDirectly    ///< 每次请求都创建新的 descriptor set
};

// ============================================================================
// VulkanRenderFrame
// ============================================================================

/**
 * @brief 每帧数据容器。
 *
 * 管理单帧渲染所需的所有资源，包括 BufferPool、CommandPool、FencePool、
 * SemaphorePool 和 DescriptorSet 缓存。
 *
 * 一个 VulkanRenderFrame 对应一个 swapchain image，在帧轮换中循环使用。
 * 每帧开始时应调用 Reset() 重置所有池，然后通过 AllocateBuffer、
 * GetCommandPool、RequestDescriptorSet 等接口获取资源。
 */
class VulkanRenderFrame {
public:
    /**
     * @brief 构造 RenderFrame。
     * @param device        Vulkan 设备
     * @param render_target 此帧的渲染目标（通常为 swapchain render target）
     * @param thread_count  多线程录制时的线程数（默认 1）
     */
    VulkanRenderFrame(VulkanDevice &device,
                      std::unique_ptr<RenderTarget> &&render_target,
                      size_t thread_count = 1);

    VulkanRenderFrame(const VulkanRenderFrame &) = delete;

    VulkanRenderFrame(VulkanRenderFrame &&) = default;

    ~VulkanRenderFrame() = default;

    VulkanRenderFrame &operator=(const VulkanRenderFrame &) = delete;

    VulkanRenderFrame &operator=(VulkanRenderFrame &&) = default;

    // ========================================================================
    // 核心接口
    // ========================================================================

    /**
     * @brief 从 buffer 池中分配一块内存。
     * @param usage        Buffer 用途（如 eUniformBuffer、eStorageBuffer）
     * @param size         分配大小（字节）
     * @param thread_index 线程索引（多线程录制时使用）
     * @return BufferAllocation 子分配。若分配失败，返回 empty() == true 的分配。
     */
    BufferAllocation AllocateBuffer(vk::BufferUsageFlags usage, vk::DeviceSize size, size_t thread_index = 0);

    /**
     * @brief 获取此帧的 command pool。
     * @param queue       用于提交 command buffer 的队列
     * @param reset_mode  CommandBuffer 重置模式
     * @param thread_index 线程索引（多线程录制时使用）
     * @return 对应线程的 VulkanCommandPool 引用。
     */
    VulkanCommandPool &GetCommandPool(const VulkanQueue &queue,
                                      CommandBufferResetMode reset_mode = CommandBufferResetMode::ResetPool,
                                      size_t thread_index = 0);

    /**
     * @brief 请求一个 descriptor set。
     *
     * 根据策略不同：
     * - StoreInCache：通过 hash 缓存 descriptor set，避免重复创建
     * - CreateDirectly：每次都创建新的 descriptor set
     *
     * @param descriptor_set_layout Descriptor set layout
     * @param buffer_infos         Buffer binding 信息
     * @param image_infos          Image binding 信息
     * @param update_after_bind    是否启用 update-after-bind（仅 StoreInCache 策略有效）
     * @param thread_index         线程索引
     * @return vk::DescriptorSet 句柄。
     */
    vk::DescriptorSet RequestDescriptorSet(const VulkanDescriptorSetLayout &descriptor_set_layout,
                                           const BindingMap<vk::DescriptorBufferInfo> &buffer_infos,
                                           const BindingMap<vk::DescriptorImageInfo> &image_infos,
                                           bool update_after_bind = false,
                                           size_t thread_index = 0);

    // ========================================================================
    // 生命周期
    // ========================================================================

    /**
     * @brief 重置此帧的所有资源，准备下一帧使用。
     *
     * 操作包括：
     * - 等待所有 fence 完成并重置
     * - 重置所有 command pool
     * - 重置所有 buffer pool（offset 归零）
     * - 重置 semaphore pool
     * - 若策略为 CreateDirectly，清空 descriptor sets
     */
    void Reset();

    /**
     * @brief 更新所有 descriptor set 的写入（仅 StoreInCache 策略需要）。
     * @param thread_index 线程索引
     */
    void UpdateDescriptorSets(size_t thread_index = 0);

    /**
     * @brief 清空所有 descriptor sets 和 descriptor pools 缓存。
     */
    void ClearDescriptors();

    /**
     * @brief 更新此帧的 render target（swapchain 重建时调用）。
     * @param render_target 新的 render target
     */
    void UpdateRenderTarget(std::unique_ptr<RenderTarget> &&render_target);

    // ========================================================================
    // 策略设置
    // ========================================================================

    void SetBufferAllocationStrategy(BufferAllocationStrategy strategy) { m_BufferAllocationStrategy = strategy; }
    void SetDescriptorManagementStrategy(DescriptorManagementStrategy strategy) { m_DescriptorManagementStrategy = strategy; }

    // ========================================================================
    // 访问器
    // ========================================================================

    VulkanDevice &GetDevice() { return m_Device; }
    const VulkanDevice &GetDevice() const { return m_Device; }

    VulkanFencePool &GetFencePool() { return m_FencePool; }
    const VulkanFencePool &GetFencePool() const { return m_FencePool; }

    VulkanSemaphorePool &GetSemaphorePool() { return m_SemaphorePool; }
    const VulkanSemaphorePool &GetSemaphorePool() const { return m_SemaphorePool; }

    RenderTarget &GetRenderTarget() { return *m_RenderTarget; }
    const RenderTarget &GetRenderTarget() const { return *m_RenderTarget; }

private:
    // ========================================================================
    // 内部实现
    // ========================================================================

    /** @brief 获取 command pools（按 queue family 索引），必要时创建。 */
    std::vector<VulkanCommandPool> &GetCommandPools(const VulkanQueue &queue, CommandBufferResetMode reset_mode);

private:
    // ========================================================================
    // 成员变量
    // ========================================================================

    VulkanDevice &m_Device;

    // Buffer pools: usage -> [thread_index -> (BufferPool, current BufferBlock*)]
    std::map<vk::BufferUsageFlags, std::vector<std::pair<BufferPool, BufferBlock *>>> m_BufferPools;

    // Command pools: queue_family_index -> [command_pool per thread]
    std::map<uint32_t, std::vector<VulkanCommandPool>> m_CommandPools;

    // 跟踪每个 queue family 的 reset mode（用于检测变化时重建）
    std::map<uint32_t, CommandBufferResetMode> m_CommandPoolResetModes;

    // Descriptor pools per thread: hash(layout) -> VulkanDescriptorPool
    std::vector<std::unordered_map<size_t, VulkanDescriptorPool>> m_DescriptorPools;

    // Descriptor sets per thread: hash(layout + buffer_infos + image_infos) -> VulkanDescriptorSet
    std::vector<std::unordered_map<size_t, VulkanDescriptorSet>> m_DescriptorSets;

    VulkanFencePool m_FencePool;
    VulkanSemaphorePool m_SemaphorePool;
    std::unique_ptr<RenderTarget> m_RenderTarget;

    size_t m_ThreadCount{1};

    BufferAllocationStrategy     m_BufferAllocationStrategy     = BufferAllocationStrategy::MultipleAllocationsPerBuffer;
    DescriptorManagementStrategy m_DescriptorManagementStrategy = DescriptorManagementStrategy::StoreInCache;
};

} // namespace GE