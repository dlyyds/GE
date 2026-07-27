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

#include "Render/VulkanBase/VulkanRenderFrame.h"

#include "Core/Log.h"

#include <algorithm>
#include <cassert>
#include <set>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <utility>

namespace GE {

// ============================================================================
// 辅助：将 CommandBufferResetMode 转换为 CommandPoolCreateFlags
// ============================================================================

namespace {

vk::CommandPoolCreateFlags ToCreateFlags(CommandBufferResetMode reset_mode) {
    switch (reset_mode) {
    case CommandBufferResetMode::ResetPool: return vk::CommandPoolCreateFlagBits::eTransient;
    case CommandBufferResetMode::ResetIndividually: return vk::CommandPoolCreateFlagBits::eTransient |
                                                           vk::CommandPoolCreateFlagBits::eResetCommandBuffer;
    case CommandBufferResetMode::AlwaysAllocate: return vk::CommandPoolCreateFlags{};
    default: return vk::CommandPoolCreateFlagBits::eTransient;
    }
}

} // namespace

// ============================================================================
// 构造函数
// ============================================================================

VulkanRenderFrame::VulkanRenderFrame(VulkanDevice &device,
                                     std::unique_ptr<RenderTarget> &&render_target,
                                     size_t thread_count) : m_Device(device),
                                                            m_FencePool(device.GetHandle()),
                                                            m_SemaphorePool(device),
                                                            m_ThreadCount(thread_count),
                                                            m_DescriptorPools(thread_count),
                                                            m_DescriptorSets(thread_count) {
    // Buffer pool 默认块大小（256KB）
    static constexpr vk::DeviceSize BUFFER_POOL_BLOCK_SIZE = 256 * 1024; // 256 KB

    // 支持的 usage 及其块大小乘数
    static const std::unordered_map<vk::BufferUsageFlags, uint32_t> supported_usage_map = {
        {vk::BufferUsageFlagBits::eUniformBuffer, 1},
        {vk::BufferUsageFlagBits::eStorageBuffer, 2}, // SSBO 通常更大，x2
        {vk::BufferUsageFlagBits::eVertexBuffer, 1},
        {vk::BufferUsageFlagBits::eIndexBuffer, 1},
    };

    // 设置 render target
    UpdateRenderTarget(std::move(render_target));

    // 为每个 usage 创建 buffer pools（每个线程一个）
    for (auto &usage_it : supported_usage_map) {
        auto [buffer_pools_it, inserted] = m_BufferPools.emplace(
            usage_it.first, std::vector<std::pair<BufferPool, BufferBlock *> >{});
        if (!inserted) {
            throw std::runtime_error("VulkanRenderFrame: 创建 buffer pool 失败");
        }

        for (size_t i = 0; i < thread_count; ++i) {
            buffer_pools_it->second.push_back(std::make_pair(
                BufferPool{m_Device, BUFFER_POOL_BLOCK_SIZE * usage_it.second, usage_it.first},
                nullptr));
        }
    }
}

// ============================================================================
// AllocateBuffer
// ============================================================================

BufferAllocation VulkanRenderFrame::AllocateBuffer(vk::BufferUsageFlags usage, vk::DeviceSize size, size_t thread_index) {
    assert(thread_index < m_ThreadCount && "线程索引越界");

    // 查找对应的 buffer pool
    auto buffer_pool_it = m_BufferPools.find(usage);
    if (buffer_pool_it == m_BufferPools.end()) {
        GE_CORE_ERROR("VulkanRenderFrame::AllocateBuffer: 没有为 usage {} 创建 buffer pool", vk::to_string(usage));
        return BufferAllocation{};
    }

    assert(thread_index < buffer_pool_it->second.size());
    auto &buffer_pool = buffer_pool_it->second[thread_index].first;
    auto &buffer_block = buffer_pool_it->second[thread_index].second;

    bool want_minimal_block = (m_BufferAllocationStrategy == BufferAllocationStrategy::OneAllocationPerBuffer);

    if (want_minimal_block || !buffer_block || !buffer_block->can_allocate(size)) {
        // 策略为 OneAllocationPerBuffer，或当前 block 空间不足 -> 请求新 block
        buffer_block = &buffer_pool.request_buffer_block(size, want_minimal_block);
    }

    return buffer_block->allocate(size);
}

// ============================================================================
// GetCommandPools（内部）
// ============================================================================

std::vector<VulkanCommandPool> &VulkanRenderFrame::GetCommandPools(const VulkanQueue &queue, CommandBufferResetMode reset_mode) {
    auto command_pool_it = m_CommandPools.find(queue.GetFamilyIndex());

    if (command_pool_it != m_CommandPools.end()) {
        assert(!command_pool_it->second.empty());
        // 检查 reset_mode 是否变化（通过辅助 map 跟踪）
        auto mode_it = m_CommandPoolResetModes.find(queue.GetFamilyIndex());
        if (mode_it != m_CommandPoolResetModes.end() && mode_it->second != reset_mode) {
            m_Device.GetHandle().waitIdle();
            // 删除旧 pools
            m_CommandPools.erase(command_pool_it);
            m_CommandPoolResetModes.erase(mode_it);
        } else {
            return command_pool_it->second;
        }
    }

    // 创建新的 command pools
    bool inserted = false;
    std::tie(command_pool_it, inserted) = m_CommandPools.emplace(
        queue.GetFamilyIndex(), std::vector<VulkanCommandPool>{});
    if (!inserted) {
        throw std::runtime_error("VulkanRenderFrame: 创建 command pool 失败");
    }

    m_CommandPoolResetModes[queue.GetFamilyIndex()] = reset_mode;

    vk::CommandPoolCreateFlags create_flags = ToCreateFlags(reset_mode);

    for (size_t i = 0; i < m_ThreadCount; ++i) {
        auto &pool = command_pool_it->second.emplace_back(
            m_Device, queue.GetFamilyIndex(), create_flags);
        pool.SetRenderFrame(this);
    }

    return command_pool_it->second;
}

// ============================================================================
// GetCommandPool
// ============================================================================

VulkanCommandPool &VulkanRenderFrame::GetCommandPool(const VulkanQueue &queue,
                                                     CommandBufferResetMode reset_mode,
                                                     size_t thread_index) {
    assert(thread_index < m_ThreadCount && "线程索引越界");

    auto &command_pools = GetCommandPools(queue, reset_mode);

    // command pools 按线程索引顺序创建，直接索引访问
    assert(thread_index < command_pools.size() && "未找到对应线程的 command pool");
    return command_pools[thread_index];
}

// ============================================================================
// RequestDescriptorSet
// ============================================================================

VulkanDescriptorSet &VulkanRenderFrame::RequestDescriptorSet(
    const VulkanDescriptorSetLayout &descriptor_set_layout,
    const BindingMap<vk::DescriptorBufferInfo> &buffer_infos,
    const BindingMap<vk::DescriptorImageInfo> &image_infos,
    bool update_after_bind,
    size_t thread_index) {
    assert(thread_index < m_ThreadCount && "线程索引越界");
    assert(thread_index < m_DescriptorPools.size() && "线程索引越界（descriptor pools）");

    // 获取或创建 descriptor pool（通过 hash 缓存）
    auto &descriptor_pool = request_resource(
        m_Device, m_DescriptorPools[thread_index], descriptor_set_layout);

    if (m_DescriptorManagementStrategy == DescriptorManagementStrategy::StoreInCache) {
        // 需要更新 before bind 的 binding 集合
        std::set<uint32_t> bindings_to_update;

        // 如果 update_after_bind 启用，只更新未标记 eUpdateAfterBind 的 binding
        if (update_after_bind) {
            auto aggregate_binding_to_update = [&bindings_to_update, &descriptor_set_layout](const auto &infos_map) {
                for (const auto &[binding_index, ignored] : infos_map) {
                    if (!(descriptor_set_layout.GetLayoutBindingFlag(binding_index) &
                          vk::DescriptorBindingFlagBits::eUpdateAfterBind)) {
                        bindings_to_update.insert(binding_index);
                    }
                }
            };
            aggregate_binding_to_update(buffer_infos);
            aggregate_binding_to_update(image_infos);
        }

        // 通过缓存请求 descriptor set
        assert(thread_index < m_DescriptorSets.size() && "线程索引越界（descriptor sets）");
        auto &ds_map = m_DescriptorSets[thread_index];
        size_t before_count = ds_map.size();
        auto &descriptor_set = request_resource(
            m_Device, ds_map,
            descriptor_set_layout, descriptor_pool, buffer_infos, image_infos);
        bool cache_hit = (ds_map.size() == before_count);

        // GE_CORE_TRACE("RequestDescriptorSet: set={}, cache_{}, total_sets={}",
        //               descriptor_set_layout.GetIndex(),
        //               cache_hit ? "hit" : "miss",
        //               ds_map.size());

        // 更新指定的 bindings（空 = 全部更新）
        descriptor_set.Update({bindings_to_update.begin(), bindings_to_update.end()});

        return descriptor_set;
    } else {
        // CreateDirectly 策略：每次创建新 descriptor set，不缓存
        // 存入成员向量确保引用有效
        m_DirectDescriptorSets.emplace_back(
            m_Device, descriptor_set_layout, descriptor_pool, buffer_infos, image_infos);
        m_DirectDescriptorSets.back().ApplyWrites();
        return m_DirectDescriptorSets.back();
    }
}

// ============================================================================
// Reset
// ============================================================================

void VulkanRenderFrame::Reset() {
    // 1. 等待所有 fence 完成
    vk::Result result = m_FencePool.Wait();
    if (result != vk::Result::eSuccess) {
        GE_CORE_ERROR("VulkanRenderFrame::Reset: 等待 fence 失败");
    }
    result = m_FencePool.Reset();
    if (result != vk::Result::eSuccess) {
        GE_CORE_ERROR("VulkanRenderFrame::Reset: 重置 fence 失败");
    }

    // 2. 重置所有 command pool
    for (auto &command_pools_per_queue : m_CommandPools) {
        for (auto &command_pool : command_pools_per_queue.second) {
            command_pool.ResetPool();
        }
    }

    // 3. 重置所有 buffer pool（offset 归零）
    for (auto &buffer_pools_per_usage : m_BufferPools) {
        for (auto &buffer_pool_pair : buffer_pools_per_usage.second) {
            buffer_pool_pair.first.reset();
            buffer_pool_pair.second = nullptr;
        }
    }

    // 4. 重置 semaphore pool
    m_SemaphorePool.Reset();

    // 5. CreateDirectly 策略下清空 descriptor sets
    if (m_DescriptorManagementStrategy == DescriptorManagementStrategy::CreateDirectly) {
        ClearDescriptors();
    }
}

// ============================================================================
// UpdateDescriptorSets
// ============================================================================

void VulkanRenderFrame::UpdateDescriptorSets(size_t thread_index) {
    assert(thread_index < m_DescriptorSets.size() && "线程索引越界");

    auto &thread_descriptor_sets = m_DescriptorSets[thread_index];
    for (auto &descriptor_set_it : thread_descriptor_sets) {
        descriptor_set_it.second.Update();
    }
}

// ============================================================================
// ClearDescriptors
// ============================================================================

void VulkanRenderFrame::ClearDescriptors() {
    for (auto &desc_sets_per_thread : m_DescriptorSets) {
        desc_sets_per_thread.clear();
    }
    m_DirectDescriptorSets.clear();
    for (auto &desc_pools_per_thread : m_DescriptorPools) {
        for (auto &desc_pool : desc_pools_per_thread) {
            desc_pool.second.Reset();
        }
    }
}

// ============================================================================
// UpdateRenderTarget
// ============================================================================

void VulkanRenderFrame::UpdateRenderTarget(std::unique_ptr<RenderTarget> &&render_target) {
    m_RenderTarget = std::move(render_target);
}

// ============================================================================
// 析构
// ============================================================================

VulkanRenderFrame::~VulkanRenderFrame() {
    // 显式按依赖顺序清理，防止 unique_ptr 默认反序析构导致的问题
    // 清空 descriptor sets/pools（可能引用 Device）
    m_DescriptorSets.clear();
    m_DescriptorPools.clear();

    // 清空 command pools（持有 VkCommandPool，需在 Device 销毁前释放）
    m_CommandPools.clear();

    // 清空 buffer pools（持有 VkBuffer，需在 Device 销毁前释放）
    m_BufferPools.clear();

    // 销毁 RenderTarget（内部持有 VulkanImageView）
    m_RenderTarget.reset();
}

} // namespace GE