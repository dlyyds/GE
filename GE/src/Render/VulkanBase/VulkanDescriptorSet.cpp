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
 * @file VulkanDescriptorSet.cpp
 * @brief VulkanDescriptorSet 实现，从 Vulkan-Samples 适配。
 */

#include "Render/VulkanBase/VulkanDescriptorSet.h"

#include "Render/VulkanBase/VulkanDescriptorSetLayout.h"
#include "Render/VulkanBase/VulkanDescriptorPool.h"
#include "Render/VulkanBase/VulkanDevice.h"
#include "Render/VulkanBase/PhysicalDevice.h"
#include "Core/Log.h"

#include <cstdint>
#include <utility>

namespace GE
{
namespace
{

// ==================================================================
// 哈希辅助（boost hash_combine 风格）
// ==================================================================

template <class T>
inline void hash_combine(size_t &seed, const T &v)
{
    std::hash<T> hasher;
    seed ^= hasher(v) + 0x9e3779b9 + (seed << 6) + (seed >> 2);
}

/// 对 vk::WriteDescriptorSet 的内容做哈希（用于变更追踪）。
size_t hash_write_descriptor_set(const vk::WriteDescriptorSet &write)
{
    size_t seed = 0;

    hash_combine(seed, static_cast<VkDescriptorSet>(write.dstSet));
    hash_combine(seed, write.dstBinding);
    hash_combine(seed, write.dstArrayElement);
    hash_combine(seed, write.descriptorCount);
    hash_combine(seed, static_cast<uint32_t>(write.descriptorType));

    auto desc_type = static_cast<uint32_t>(write.descriptorType);

    if (desc_type == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER ||
        desc_type == VK_DESCRIPTOR_TYPE_STORAGE_BUFFER ||
        desc_type == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC ||
        desc_type == VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC)
    {
        for (uint32_t i = 0; i < write.descriptorCount; i++)
        {
            hash_combine(seed, static_cast<VkBuffer>(write.pBufferInfo[i].buffer));
            hash_combine(seed, write.pBufferInfo[i].offset);
            hash_combine(seed, write.pBufferInfo[i].range);
        }
    }
    else if (desc_type == VK_DESCRIPTOR_TYPE_SAMPLER ||
             desc_type == VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER ||
             desc_type == VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE ||
             desc_type == VK_DESCRIPTOR_TYPE_STORAGE_IMAGE ||
             desc_type == VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT)
    {
        for (uint32_t i = 0; i < write.descriptorCount; i++)
        {
            hash_combine(seed, static_cast<VkSampler>(write.pImageInfo[i].sampler));
            hash_combine(seed, static_cast<VkImageView>(write.pImageInfo[i].imageView));
            hash_combine(seed, static_cast<uint32_t>(write.pImageInfo[i].imageLayout));
        }
    }

    return seed;
}

} // anonymous namespace

// ==================================================================
// 构造函数
// ==================================================================

VulkanDescriptorSet::VulkanDescriptorSet(
    VulkanDevice                          &device,
    const VulkanDescriptorSetLayout       &layout,
    VulkanDescriptorPool                   &pool,
    const BindingMap<vk::DescriptorBufferInfo> &buffer_infos,
    const BindingMap<vk::DescriptorImageInfo>  &image_infos) :
    m_Device(&device),
    m_Layout(&layout),
    m_Pool(&pool),
    m_BufferInfos(buffer_infos),
    m_ImageInfos(image_infos),
    m_Handle(pool.Allocate())
{
    Prepare();
}

VulkanDescriptorSet::VulkanDescriptorSet(VulkanDescriptorSet &&other) noexcept :
    m_Device(std::exchange(other.m_Device, nullptr)),
    m_Layout(std::exchange(other.m_Layout, nullptr)),
    m_Pool(std::exchange(other.m_Pool, nullptr)),
    m_BufferInfos(std::move(other.m_BufferInfos)),
    m_ImageInfos(std::move(other.m_ImageInfos)),
    m_Handle(std::exchange(other.m_Handle, vk::DescriptorSet{nullptr})),
    m_WriteDescriptorSets(std::move(other.m_WriteDescriptorSets)),
    m_UpdatedBindings(std::move(other.m_UpdatedBindings))
{}

// ==================================================================
// Prepare — 从 buffer_infos / image_infos 构建 write 操作
// ==================================================================

void VulkanDescriptorSet::Prepare()
{
    if (!m_WriteDescriptorSets.empty())
    {
        GE_CORE_WARN("Trying to prepare a descriptor set that has already been prepared, skipping.");
        return;
    }

    size_t uniform_buffer_range_limit = std::numeric_limits<size_t>::max();
    size_t storage_buffer_range_limit = std::numeric_limits<size_t>::max();

    if (m_Device)
    {
        auto props = m_Device->GetGpu().GetHandle().getProperties();
        uniform_buffer_range_limit = props.limits.maxUniformBufferRange;
        storage_buffer_range_limit = props.limits.maxStorageBufferRange;
    }

    for (auto &binding_it : m_BufferInfos)
    {
        auto  binding_index   = binding_it.first;
        auto &buffer_bindings = binding_it.second;

        if (auto binding_info = m_Layout->GetLayoutBinding(binding_index))
        {
            for (auto &element_it : buffer_bindings)
            {
                auto &buffer_info = element_it.second;

                size_t buffer_range_limit = static_cast<size_t>(buffer_info.range);

                if ((binding_info->descriptorType == vk::DescriptorType::eUniformBuffer ||
                     binding_info->descriptorType == vk::DescriptorType::eUniformBufferDynamic) &&
                    buffer_range_limit > uniform_buffer_range_limit)
                {
                    GE_CORE_ERROR("Set {} binding {}: buffer size {} exceeds uniform buffer range limit {}",
                                  m_Layout->GetIndex(), binding_index, buffer_info.range, uniform_buffer_range_limit);
                    buffer_range_limit = uniform_buffer_range_limit;
                }
                else if ((binding_info->descriptorType == vk::DescriptorType::eStorageBuffer ||
                          binding_info->descriptorType == vk::DescriptorType::eStorageBufferDynamic) &&
                         buffer_range_limit > storage_buffer_range_limit)
                {
                    GE_CORE_ERROR("Set {} binding {}: buffer size {} exceeds storage buffer range limit {}",
                                  m_Layout->GetIndex(), binding_index, buffer_info.range, storage_buffer_range_limit);
                    buffer_range_limit = storage_buffer_range_limit;
                }

                buffer_info.range = buffer_range_limit;

                vk::WriteDescriptorSet write_op{};
                write_op.dstSet          = m_Handle;
                write_op.dstBinding      = binding_index;
                write_op.descriptorType  = binding_info->descriptorType;
                write_op.pBufferInfo     = &buffer_info;
                write_op.dstArrayElement = element_it.first;
                write_op.descriptorCount = 1;

                m_WriteDescriptorSets.push_back(write_op);
            }
        }
        else
        {
            GE_CORE_ERROR("Shader layout set does not use buffer binding at #{}", binding_index);
        }
    }

    for (auto &binding_it : m_ImageInfos)
    {
        auto  binding_index     = binding_it.first;
        auto &binding_resources = binding_it.second;

        if (auto binding_info = m_Layout->GetLayoutBinding(binding_index))
        {
            for (auto &element_it : binding_resources)
            {
                auto &image_info = element_it.second;

                vk::WriteDescriptorSet write_op{};
                write_op.dstSet          = m_Handle;
                write_op.dstBinding      = binding_index;
                write_op.descriptorType  = binding_info->descriptorType;
                write_op.pImageInfo      = &image_info;
                write_op.dstArrayElement = element_it.first;
                write_op.descriptorCount = 1;

                m_WriteDescriptorSets.push_back(write_op);
            }
        }
        else
        {
            GE_CORE_ERROR("Shader layout set does not use image binding at #{}", binding_index);
        }
    }
}

// ==================================================================
// Update — 增量写入（仅写入已变更的 bindings）
// ==================================================================

void VulkanDescriptorSet::Update(const std::vector<uint32_t> &bindings_to_update)
{
    std::vector<vk::WriteDescriptorSet> write_ops;
    std::vector<size_t>                 write_op_hashes;

    auto process_write_op = [&](const auto &write_op)
    {
        size_t op_hash = hash_write_descriptor_set(write_op);

        auto update_pair_it = m_UpdatedBindings.find(write_op.dstBinding);
        if (update_pair_it == m_UpdatedBindings.end() || update_pair_it->second != op_hash)
        {
            write_ops.push_back(write_op);
            write_op_hashes.push_back(op_hash);
        }
    };

    if (bindings_to_update.empty())
    {
        for (auto &write_op : m_WriteDescriptorSets)
        {
            process_write_op(write_op);
        }
    }
    else
    {
        for (auto &write_op : m_WriteDescriptorSets)
        {
            if (std::ranges::find(bindings_to_update, write_op.dstBinding) != bindings_to_update.end())
            {
                process_write_op(write_op);
            }
        }
    }

    if (!write_ops.empty())
    {
        m_Device->GetHandle().updateDescriptorSets(
            static_cast<uint32_t>(write_ops.size()), write_ops.data(), 0, nullptr);
    }

    for (size_t i = 0; i < write_ops.size(); i++)
    {
        m_UpdatedBindings[write_ops[i].dstBinding] = write_op_hashes[i];
    }
}

// ==================================================================
// ApplyWrites — 强制应用所有 write 操作
// ==================================================================

void VulkanDescriptorSet::ApplyWrites() const
{
    m_Device->GetHandle().updateDescriptorSets(
        static_cast<uint32_t>(m_WriteDescriptorSets.size()),
        m_WriteDescriptorSets.data(), 0, nullptr);
}

// ==================================================================
// Reset — 重新准备
// ==================================================================

void VulkanDescriptorSet::Reset(
    const BindingMap<vk::DescriptorBufferInfo> &new_buffer_infos,
    const BindingMap<vk::DescriptorImageInfo>  &new_image_infos)
{
    if (!new_buffer_infos.empty() || !new_image_infos.empty())
    {
        m_BufferInfos = new_buffer_infos;
        m_ImageInfos  = new_image_infos;
    }
    else
    {
        GE_CORE_WARN("Calling reset on DescriptorSet with no new buffer infos and no new image infos.");
    }

    m_WriteDescriptorSets.clear();
    m_UpdatedBindings.clear();

    Prepare();
}

// ==================================================================
// 访问器
// ==================================================================

const VulkanDescriptorSetLayout &VulkanDescriptorSet::GetLayout() const
{
    return *m_Layout;
}

} // namespace GE
