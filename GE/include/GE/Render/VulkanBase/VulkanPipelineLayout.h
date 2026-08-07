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
 * @file VulkanPipelineLayout.h
 * @brief PipelineLayout 封装，从 Vulkan-Samples 适配而来。
 *
 * 对着色器模块集合的资源进行反射，自动创建 VkPipelineLayout。
 * 管理 descriptor set layout 的生命周期，提供 set/resource 查询接口。
 */

#pragma once

#include <vulkan/vulkan.hpp>

#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "Render/VulkanBase/VulkanShaderModule.h"
#include "Render/VulkanBase/VulkanResourceBase.h"

namespace GE
{

class VulkanDevice;
class VulkanDescriptorSetLayout;

/**
 * @brief 对着色器资源进行反射并创建 VkPipelineLayout。
 *
 * 继承 VulkanResourceBase<vk::PipelineLayout>，自动获得：
 * - 句柄管理（GetHandle / SetHandle）
 * - 设备引用（GetDevice）
 * - 调试命名（SetDebugName / GetDebugName）
 *
 * 从一组 VulkanShaderModule 中收集所有着色器资源，
 * 按 set 分组并为每组创建 VulkanDescriptorSetLayout，
 * 最终汇总创建 VkPipelineLayout 句柄。
 */
class VulkanPipelineLayout : public VulkanResourceBase<vk::PipelineLayout>
{
  public:
    using Parent = VulkanResourceBase<vk::PipelineLayout>;

    VulkanPipelineLayout(VulkanDevice &device, const std::vector<VulkanShaderModule *> &shader_modules);

    VulkanPipelineLayout(const VulkanPipelineLayout &) = delete;

    VulkanPipelineLayout(VulkanPipelineLayout &&other);

    ~VulkanPipelineLayout();

    VulkanPipelineLayout &operator=(const VulkanPipelineLayout &) = delete;

    VulkanPipelineLayout &operator=(VulkanPipelineLayout &&) = delete;

    const std::vector<VulkanShaderModule *> &GetShaderModules() const;

    const std::vector<ShaderResource> GetResources(const ShaderResourceType &type = ShaderResourceType::All, vk::ShaderStageFlags stage = vk::ShaderStageFlagBits::eAll) const;

    const std::unordered_map<uint32_t, std::vector<ShaderResource>> &GetShaderSets() const;

    bool HasDescriptorSetLayout(uint32_t set_index) const;

    VulkanDescriptorSetLayout &GetDescriptorSetLayout(uint32_t set_index) const;

    vk::ShaderStageFlags GetPushConstantRangeStage(uint32_t size, uint32_t offset = 0) const;

  private:
    /// 此 pipeline layout 使用的着色器模块
    std::vector<VulkanShaderModule *> m_ShaderModules;

    /// 按名称索引的着色器资源
    std::unordered_map<std::string, ShaderResource> m_ShaderResources;

    /// 每个 set 拥有的资源
    std::unordered_map<uint32_t, std::vector<ShaderResource>> m_ShaderSets;

    /// 不同 set 的 descriptor set layout（由 VulkanResourceCache 持有生命周期）
    std::vector<VulkanDescriptorSetLayout *> m_DescriptorSetLayouts;
};

} // namespace GE
