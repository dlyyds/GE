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
 * @file VulkanDescriptorPool.cpp
 * @brief VulkanDescriptorPool 实现，从 Vulkan-Samples 适配。
 */

#include "Render/VulkanBase/VulkanDescriptorPool.h"

#include "Render/VulkanBase/VulkanDescriptorSetLayout.h"
#include "Render/VulkanBase/VulkanDevice.h"

#include <map>

namespace GE
{

VulkanDescriptorPool::VulkanDescriptorPool(VulkanDevice                    &device,
                                           const VulkanDescriptorSetLayout &descriptor_set_layout,
                                           uint32_t                         pool_size) :
    m_Device(device),
    m_DescriptorSetLayout(&descriptor_set_layout)
{
    const auto &bindings = descriptor_set_layout.GetBindings();

    std::map<vk::DescriptorType, uint32_t> descriptor_type_counts;

    // 统计每种 descriptor type 的总数
    for (auto &binding : bindings)
    {
        descriptor_type_counts[binding.descriptorType] += binding.descriptorCount;
    }

    // 分配 pool sizes 数组
    m_PoolSizes.resize(descriptor_type_counts.size());

    auto pool_size_it = m_PoolSizes.begin();

    // 每种 descriptor type 的数量乘以 pool size（即每个 pool 能容纳的 descriptor 总量）
    for (auto &it : descriptor_type_counts)
    {
        pool_size_it->type            = it.first;
        pool_size_it->descriptorCount = it.second * pool_size;

        ++pool_size_it;
    }

    m_PoolMaxSets = pool_size;
}

VulkanDescriptorPool::~VulkanDescriptorPool()
{
    // 销毁所有 descriptor pools
    for (auto pool : m_Pools)
    {
        m_Device.GetHandle().destroyDescriptorPool(pool);
    }
}

void VulkanDescriptorPool::Reset()
{
    // 重置所有 descriptor pools
    for (auto pool : m_Pools)
    {
        m_Device.GetHandle().resetDescriptorPool(pool);
    }

    // 清除内部分配追踪
    std::fill(m_PoolSetsCount.begin(), m_PoolSetsCount.end(), 0);
    m_SetPoolMapping.clear();

    // 重置 pool 索引
    m_PoolIndex = 0;
}

const VulkanDescriptorSetLayout &VulkanDescriptorPool::GetDescriptorSetLayout() const
{
    return *m_DescriptorSetLayout;
}

void VulkanDescriptorPool::SetDescriptorSetLayout(const VulkanDescriptorSetLayout &set_layout)
{
    m_DescriptorSetLayout = &set_layout;
}

vk::DescriptorSet VulkanDescriptorPool::Allocate()
{
    m_PoolIndex = FindAvailablePool(m_PoolIndex);

    // 增加当前 pool 的已分配计数
    ++m_PoolSetsCount[m_PoolIndex];

    vk::DescriptorSetLayout set_layout = GetDescriptorSetLayout().GetHandle();

    vk::DescriptorSetAllocateInfo alloc_info{
        .descriptorPool     = m_Pools[m_PoolIndex],
        .descriptorSetCount = 1,
        .pSetLayouts        = &set_layout,
    };

    vk::DescriptorSet handle{nullptr};

    // 从当前 pool 分配新的 descriptor set
    try
    {
        handle = m_Device.GetHandle().allocateDescriptorSets(alloc_info).front();
    }
    catch (const std::exception &)
    {
        // 分配失败，回滚计数
        --m_PoolSetsCount[m_PoolIndex];
        return vk::DescriptorSet{nullptr};
    }

    // 存储 descriptor set → pool 的映射
    m_SetPoolMapping.emplace(handle, m_PoolIndex);

    return handle;
}

vk::Result VulkanDescriptorPool::Free(vk::DescriptorSet descriptor_set)
{
    // 查找 descriptor set 所属的 pool 索引
    auto it = m_SetPoolMapping.find(descriptor_set);

    if (it == m_SetPoolMapping.end())
    {
        return vk::Result::eIncomplete;
    }

    auto desc_pool_index = it->second;

    // 释放 descriptor set 回其 pool
    m_Device.GetHandle().freeDescriptorSets(m_Pools[desc_pool_index], descriptor_set);

    // 移除映射
    m_SetPoolMapping.erase(it);

    // 减少该 pool 的已分配计数
    --m_PoolSetsCount[desc_pool_index];

    // 将当前 pool 索引切换到这个有空位的 pool
    m_PoolIndex = desc_pool_index;

    return vk::Result::eSuccess;
}

uint32_t VulkanDescriptorPool::FindAvailablePool(uint32_t search_index)
{
    // 需要创建新 pool
    if (m_Pools.size() <= search_index)
    {
        vk::DescriptorPoolCreateInfo create_info{
            .maxSets       = m_PoolMaxSets,
            .poolSizeCount = static_cast<uint32_t>(m_PoolSizes.size()),
            .pPoolSizes    = m_PoolSizes.data(),
        };

        // 检查 descriptor set layout 中的 binding flags，设置所需标志
        auto &binding_flags = m_DescriptorSetLayout->GetBindingFlags();
        for (auto binding_flag : binding_flags)
        {
            if (binding_flag & vk::DescriptorBindingFlagBitsEXT::eUpdateAfterBind)
            {
                create_info.flags |= vk::DescriptorPoolCreateFlagBits::eUpdateAfterBind;
            }
        }

        // 创建 Vulkan descriptor pool
        auto handle = m_Device.GetHandle().createDescriptorPool(create_info);

        // 内部存储 Vulkan 句柄
        m_Pools.push_back(handle);

        // 为新 pool 添加 set 计数
        m_PoolSetsCount.push_back(0);

        return search_index;
    }
    else if (m_PoolSetsCount[search_index] < m_PoolMaxSets)
    {
        return search_index;
    }

    // 递归查找下一个 pool
    return FindAvailablePool(++search_index);
}

} // namespace GE
