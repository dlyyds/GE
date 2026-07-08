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
 * @file VulkanDescriptorSet.h
 * @brief DescriptorSet 封装，从 Vulkan-Samples 适配而来。
 *
 * 从 VulkanDescriptorPool 分配的 descriptor set 句柄。
 * 支持批量更新 + 增量追踪，避免重复写入相同 binding。
 */

#pragma once

#include <vulkan/vulkan.hpp>

#include <cstdint>
#include <unordered_map>
#include <vector>

#include "Render/VulkanBase/VulkanCommon.h"

namespace GE
{

class VulkanDevice;
class VulkanDescriptorSetLayout;
class VulkanDescriptorPool;

/**
 * @brief 从 VulkanDescriptorPool 分配的 descriptor set 句柄。
 *        追踪已写入的 bindings，防止重复的 vkUpdateDescriptorSets 调用。
 */
class VulkanDescriptorSet
{
  public:
    VulkanDescriptorSet(VulkanDevice                          &device,
                        const VulkanDescriptorSetLayout       &layout,
                        VulkanDescriptorPool                   &pool,
                        const BindingMap<vk::DescriptorBufferInfo> &buffer_infos = {},
                        const BindingMap<vk::DescriptorImageInfo>  &image_infos  = {});

    VulkanDescriptorSet(const VulkanDescriptorSet &)            = delete;

    VulkanDescriptorSet(VulkanDescriptorSet &&other)            noexcept;

    ~VulkanDescriptorSet()                                      = default;

    VulkanDescriptorSet &operator=(const VulkanDescriptorSet &) = delete;

    VulkanDescriptorSet &operator=(VulkanDescriptorSet &&)      = default;

    /**
     * @brief 重置状态，可选择传入新的 buffer/image infos。
     *        清空 write_descriptor_sets 和 updated_bindings 后重新 prepare。
     */
    void Reset(const BindingMap<vk::DescriptorBufferInfo> &new_buffer_infos = {},
               const BindingMap<vk::DescriptorImageInfo>  &new_image_infos  = {});

    /**
     * @brief 执行所有待处理的写入操作。
     * @param bindings_to_update 空 = 更新所有未写入/已变更的 bindings；
     *                           非空 = 仅更新指定 binding 中已变更的。
     */
    void Update(const std::vector<uint32_t> &bindings_to_update = {});

    /// 强制应用所有 write 操作（不检查更新状态）。
    void ApplyWrites() const;

    [[nodiscard]] vk::DescriptorSet GetHandle() const { return m_Handle; }

    const VulkanDescriptorSetLayout &GetLayout() const;

    BindingMap<vk::DescriptorBufferInfo> &GetBufferInfos() { return m_BufferInfos; }

    BindingMap<vk::DescriptorImageInfo> &GetImageInfos() { return m_ImageInfos; }

  private:
    /// 从 buffer_infos / image_infos 构建 write_descriptor_sets。
    void Prepare();

    VulkanDevice                    *m_Device = nullptr;
    const VulkanDescriptorSetLayout *m_Layout = nullptr;
    VulkanDescriptorPool            *m_Pool   = nullptr;

    BindingMap<vk::DescriptorBufferInfo> m_BufferInfos;
    BindingMap<vk::DescriptorImageInfo>  m_ImageInfos;

    vk::DescriptorSet m_Handle{nullptr};

    std::vector<vk::WriteDescriptorSet>  m_WriteDescriptorSets;

    /// 已写入的 bindings → 写入内容的哈希值。
    std::unordered_map<uint32_t, size_t> m_UpdatedBindings;
};

} // namespace GE
