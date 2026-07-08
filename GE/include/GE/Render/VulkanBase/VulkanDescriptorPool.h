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
 * @file VulkanDescriptorPool.h
 * @brief 多池管理的 DescriptorPool 封装，从 Vulkan-Samples 适配而来。
 *
 * 管理一组固定大小的 VkDescriptorPool，能够分配和释放 descriptor set。
 * 从 VulkanDescriptorSetLayout 的 binding 信息自动计算 pool size。
 * 与 VulkanDescriptorSetLayout 配对使用。
 */

#pragma once

#include <vulkan/vulkan.hpp>

#include <cstdint>
#include <unordered_map>
#include <vector>

namespace GE
{

class VulkanDevice;
class VulkanDescriptorSetLayout;

/// vk::DescriptorSet 的哈希器（vulkan.hpp 未提供 std::hash 特化）
struct DescriptorSetHash
{
    size_t operator()(vk::DescriptorSet s) const
    {
        return std::hash<VkDescriptorSet>{}(static_cast<VkDescriptorSet>(s));
    }
};

/**
 * @brief 管理一组 VkDescriptorPool，支持跨池分配 descriptor set。
 *
 * 每个 pool 最多分配固定数量的 sets（默认为 16）。
 * 当前 pool 用满后自动创建新的 pool。
 * 支持释放 set 回对应 pool 并复用空间。
 */
class VulkanDescriptorPool
{
  public:
    static const uint32_t MAX_SETS_PER_POOL = 16;

    /**
     * @brief 从 DescriptorSetLayout 创建 descriptor pool。
     * @param device                 Vulkan 设备
     * @param descriptor_set_layout 用于计算 pool size 的 layout
     * @param pool_size              每个 pool 可分配的 sets 数量
     */
    VulkanDescriptorPool(VulkanDevice                    &device,
                         const VulkanDescriptorSetLayout &descriptor_set_layout,
                         uint32_t                         pool_size = MAX_SETS_PER_POOL);

    VulkanDescriptorPool(const VulkanDescriptorPool &) = delete;

    VulkanDescriptorPool(VulkanDescriptorPool &&) = default;

    ~VulkanDescriptorPool();

    VulkanDescriptorPool &operator=(const VulkanDescriptorPool &) = delete;

    VulkanDescriptorPool &operator=(VulkanDescriptorPool &&) = delete;

    /// 重置所有 descriptor pool（清空分配状态）。
    void Reset();

    const VulkanDescriptorSetLayout &GetDescriptorSetLayout() const;

    void SetDescriptorSetLayout(const VulkanDescriptorSetLayout &set_layout);

    /// 从当前可用 pool 分配一个 descriptor set。失败返回 nullptr。
    vk::DescriptorSet Allocate();

    /// 释放 descriptor set 回其所属 pool。
    vk::Result Free(vk::DescriptorSet descriptor_set);

  private:
    VulkanDevice &m_Device;

    const VulkanDescriptorSetLayout *m_DescriptorSetLayout{nullptr};

    /// 累计的 descriptor pool size（按类型）
    std::vector<vk::DescriptorPoolSize> m_PoolSizes;

    /// 每个 pool 最大 sets 数
    uint32_t m_PoolMaxSets{0};

    /// 已创建的 Vulkan descriptor pools
    std::vector<vk::DescriptorPool> m_Pools;

    /// 每个 pool 已分配的 set 计数
    std::vector<uint32_t> m_PoolSetsCount;

    /// 当前分配使用的 pool 索引
    uint32_t m_PoolIndex{0};

    /// descriptor set → pool 索引映射
    std::unordered_map<vk::DescriptorSet, uint32_t, DescriptorSetHash> m_SetPoolMapping;

    /// 查找可用 pool，必要时创建新的
    uint32_t FindAvailablePool(uint32_t search_index);
};

} // namespace GE
