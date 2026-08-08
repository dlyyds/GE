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

namespace GE {
namespace {

// ==================================================================
// 哈希辅助（boost hash_combine 风格）
// ==================================================================

template <class T>
inline void hash_combine(size_t &seed, const T &v) {
    // glm 也提供 hash_combine，但此处直接实现以保持独立
    std::hash<T> hasher;
    seed ^= hasher(v) + 0x9e3779b9 + (seed << 6) + (seed >> 2);
}

/// 对 vk::WriteDescriptorSet 的内容做哈希（用于变更追踪）。
size_t hash_write_descriptor_set(const vk::WriteDescriptorSet &write) {
    size_t seed = 0;

    // Cast 到 C 类型再哈希（vk::* 包装类型无 std::hash 特化）
    hash_combine(seed, static_cast<VkDescriptorSet>(write.dstSet));
    hash_combine(seed, write.dstBinding);
    hash_combine(seed, write.dstArrayElement);
    hash_combine(seed, write.descriptorCount);
    hash_combine(seed, static_cast<uint32_t>(write.descriptorType));

    // 按 descriptor 类型哈希其指向的 pBufferInfo / pImageInfo
    auto desc_type = write.descriptorType;

    if (desc_type == vk::DescriptorType::eUniformBuffer ||
        desc_type == vk::DescriptorType::eStorageBuffer ||
        desc_type == vk::DescriptorType::eUniformBufferDynamic ||
        desc_type == vk::DescriptorType::eStorageBufferDynamic) {
        for (uint32_t i = 0; i < write.descriptorCount; i++) {
            hash_combine(seed, static_cast<VkBuffer>(write.pBufferInfo[i].buffer));
            hash_combine(seed, write.pBufferInfo[i].offset);
            hash_combine(seed, write.pBufferInfo[i].range);
        }
    } else if (desc_type == vk::DescriptorType::eSampler ||
               desc_type == vk::DescriptorType::eCombinedImageSampler ||
               desc_type == vk::DescriptorType::eSampledImage ||
               desc_type == vk::DescriptorType::eStorageImage ||
               desc_type == vk::DescriptorType::eInputAttachment) {
        for (uint32_t i = 0; i < write.descriptorCount; i++) {
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
    VulkanDevice &device,
    const VulkanDescriptorSetLayout &layout,
    VulkanDescriptorPool &pool,
    const BindingMap<vk::DescriptorBufferInfo> &buffer_infos,
    const BindingMap<vk::DescriptorImageInfo> &image_infos) : m_Device(&device),
                                                              m_Layout(&layout),
                                                              m_Pool(&pool),
                                                              m_BufferInfos(buffer_infos),
                                                              m_ImageInfos(image_infos),
                                                              m_Handle(pool.Allocate()) {
    Prepare();
}


VulkanDescriptorSet::VulkanDescriptorSet(VulkanDescriptorSet &&other) noexcept : m_Device(other.m_Device),
                                                                                 m_Layout(other.m_Layout),
                                                                                 m_Pool(other.m_Pool),
                                                                                 m_BufferInfos(std::move(other.m_BufferInfos)),
                                                                                 m_ImageInfos(std::move(other.m_ImageInfos)),
                                                                                 m_Handle(std::exchange(other.m_Handle, vk::DescriptorSet{nullptr})),
                                                                                 m_WriteDescriptorSets(std::move(other.m_WriteDescriptorSets)),
                                                                                 m_UpdatedBindings(std::move(other.m_UpdatedBindings)) {
}

// ==================================================================
// 核心：Prepare — 从 buffer_infos / image_infos 构建 write 操作
// ==================================================================

void VulkanDescriptorSet::Prepare() {
    if (!m_WriteDescriptorSets.empty()) {
        GE_CORE_WARN("Trying to prepare a descriptor set that has already been prepared, skipping.");
        return;
    }

    // 获取设备限制（用于裁剪 buffer range）
    size_t uniform_buffer_range_limit = std::numeric_limits<size_t>::max();
    size_t storage_buffer_range_limit = std::numeric_limits<size_t>::max();

    if (m_Device) {
        auto props = m_Device->GetGpu().GetHandle().getProperties();
        uniform_buffer_range_limit = props.limits.maxUniformBufferRange;
        storage_buffer_range_limit = props.limits.maxStorageBufferRange;
    }

    // 遍历所有 buffer bindings
    for (auto &binding_it : m_BufferInfos) {
        auto binding_index = binding_it.first;
        auto &buffer_bindings = binding_it.second;

        if (auto binding_info = m_Layout->GetLayoutBinding(binding_index)) {
            for (auto &element_it : buffer_bindings) {
                auto &buffer_info = element_it.second;

                size_t buffer_range_limit = static_cast<size_t>(buffer_info.range);

                // 裁剪 buffer range 到设备限制，否则 Vulkan 验证层报错
                if ((binding_info->descriptorType == vk::DescriptorType::eUniformBuffer ||
                     binding_info->descriptorType == vk::DescriptorType::eUniformBufferDynamic) &&
                    buffer_range_limit > uniform_buffer_range_limit) {
                    GE_CORE_ERROR("Set {} binding {}: buffer size {} exceeds uniform buffer range limit {}",
                                  m_Layout->GetIndex(), binding_index, buffer_info.range, uniform_buffer_range_limit);
                    buffer_range_limit = uniform_buffer_range_limit;
                } else if ((binding_info->descriptorType == vk::DescriptorType::eStorageBuffer ||
                            binding_info->descriptorType == vk::DescriptorType::eStorageBufferDynamic) &&
                           buffer_range_limit > storage_buffer_range_limit) {
                    GE_CORE_ERROR("Set {} binding {}: buffer size {} exceeds storage buffer range limit {}",
                                  m_Layout->GetIndex(), binding_index, buffer_info.range, storage_buffer_range_limit);
                    buffer_range_limit = storage_buffer_range_limit;
                }

                buffer_info.range = buffer_range_limit;

                vk::WriteDescriptorSet write_op{};
                write_op.dstSet = m_Handle;
                write_op.dstBinding = binding_index;
                write_op.descriptorType = binding_info->descriptorType;
                write_op.pBufferInfo = &buffer_info;
                write_op.dstArrayElement = element_it.first;
                write_op.descriptorCount = 1;

                m_WriteDescriptorSets.push_back(write_op);
            }
        } else {
            GE_CORE_ERROR("Shader layout set does not use buffer binding at #{}", binding_index);
        }
    }

    // 遍历所有 image bindings
    for (auto &binding_it : m_ImageInfos) {
        auto binding_index = binding_it.first;
        auto &binding_resources = binding_it.second;

        if (auto binding_info = m_Layout->GetLayoutBinding(binding_index)) {
            for (auto &element_it : binding_resources) {
                auto &image_info = element_it.second;

                vk::WriteDescriptorSet write_op{};
                write_op.dstSet = m_Handle;
                write_op.dstBinding = binding_index;
                write_op.descriptorType = binding_info->descriptorType;
                write_op.pImageInfo = &image_info;
                write_op.dstArrayElement = element_it.first;
                write_op.descriptorCount = 1;

                m_WriteDescriptorSets.push_back(write_op);
            }
        } else {
            GE_CORE_ERROR("Shader layout set does not use image binding at #{}", binding_index);
        }
    }
}

// ==================================================================
// Update — 增量写入（仅写入已变更的 bindings）
// ==================================================================

void VulkanDescriptorSet::Update(const std::vector<uint32_t> &bindings_to_update) {
    std::vector<vk::WriteDescriptorSet> write_ops;
    std::vector<size_t> write_op_hashes;

    auto process_write_op = [&](const auto &write_op) {
        size_t op_hash = hash_write_descriptor_set(write_op);

        // 以 (binding, array element) 作为变更追踪键，精确到每个 element，
        // 避免同一 binding 的多 element write 互相覆盖哈希值导致误判。
        uint64_t update_key = (uint64_t(write_op.dstBinding) << 32) | write_op.dstArrayElement;
        auto update_pair_it = m_UpdatedBindings.find(update_key);
        if (update_pair_it == m_UpdatedBindings.end() || update_pair_it->second != op_hash) {
            write_ops.push_back(write_op);
            write_op_hashes.push_back(op_hash);
        }
    };

    if (bindings_to_update.empty()) {
        // 空列表 → 更新所有尚未写入或已变更的 binding
        for (auto &write_op : m_WriteDescriptorSets) {
            process_write_op(write_op);
        }
    } else {
        // 仅更新指定 binding
        for (auto &write_op : m_WriteDescriptorSets) {
            if (std::ranges::find(bindings_to_update, write_op.dstBinding) != bindings_to_update.end()) {
                process_write_op(write_op);
            }
        }
    }

    // 执行 Vulkan 调用
    if (!write_ops.empty()) {
        m_Device->GetHandle().updateDescriptorSets(static_cast<uint32_t>(write_ops.size()), write_ops.data(), 0, nullptr);
    }

    // 记录已写入的 bindings 及其哈希值
    for (size_t i = 0; i < write_ops.size(); i++) {
        uint64_t update_key = (uint64_t(write_ops[i].dstBinding) << 32) | write_ops[i].dstArrayElement;
        m_UpdatedBindings[update_key] = write_op_hashes[i];
    }
}

// ==================================================================
// ApplyWrites — 强制应用所有 write 操作
// ==================================================================

void VulkanDescriptorSet::ApplyWrites() const {
    m_Device->GetHandle().updateDescriptorSets(
        static_cast<uint32_t>(m_WriteDescriptorSets.size()),
        m_WriteDescriptorSets.data(), 0, nullptr);
}

// ==================================================================
// Reset — 重新准备
// ==================================================================

void VulkanDescriptorSet::Reset(
    const BindingMap<vk::DescriptorBufferInfo> &new_buffer_infos,
    const BindingMap<vk::DescriptorImageInfo> &new_image_infos) {
    if (!new_buffer_infos.empty() || !new_image_infos.empty()) {
        m_BufferInfos = new_buffer_infos;
        m_ImageInfos = new_image_infos;
    } else {
        GE_CORE_WARN("Calling reset on DescriptorSet with no new buffer infos and no new image infos.");
    }

    m_WriteDescriptorSets.clear();
    m_UpdatedBindings.clear();

    Prepare();
}

// ==================================================================
// 访问器
// ==================================================================

const VulkanDescriptorSetLayout &VulkanDescriptorSet::GetLayout() const {
    return *m_Layout;
}

} // namespace GE
