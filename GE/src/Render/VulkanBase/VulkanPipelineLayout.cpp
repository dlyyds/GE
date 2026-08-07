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
 * @file VulkanPipelineLayout.cpp
 * @brief VulkanPipelineLayout 实现，从 Vulkan-Samples 适配。
 *
 * 聚合 VulkanShaderModule 的反射资源，按 set 分组创建 DescriptorSetLayout，
 * 并创建最终的 VkPipelineLayout。
 */

#include "Render/VulkanBase/VulkanPipelineLayout.h"

#include "Render/VulkanBase/VulkanDescriptorSetLayout.h"
#include "Render/VulkanBase/VulkanDevice.h"
#include "Render/VulkanBase/VulkanResourceCache.h"

namespace GE
{

VulkanPipelineLayout::VulkanPipelineLayout(VulkanDevice &device, const std::vector<VulkanShaderModule *> &shader_modules) :
    Parent(vk::PipelineLayout{}, &device),
    m_ShaderModules{shader_modules}
{
    // 收集并合并所有着色器模块中的资源，按名称索引
    for (auto *shader_module : shader_modules)
    {
        for (const auto &shader_resource : shader_module->get_resources())
        {
            std::string key = shader_resource.name;

            // Input 和 Output 资源可能同名，加上 stage 标志前缀以区分
            if (shader_resource.type == ShaderResourceType::Input || shader_resource.type == ShaderResourceType::Output)
            {
                key = std::to_string(static_cast<uint32_t>(shader_resource.stages)) + "_" + key;
            }

            auto it = m_ShaderResources.find(key);

            if (it != m_ShaderResources.end())
            {
                // 资源已存在，合并 stage 标志
                it->second.stages |= shader_resource.stages;
            }
            else
            {
                // 创建新条目
                m_ShaderResources.emplace(key, shader_resource);
            }
        }
    }

    // 将按名称索引的描述符资源按 set 分组
    for (auto &it : m_ShaderResources)
    {
        auto &shader_resource = it.second;

        // 只对描述符类型的资源进行 set 分组，
        // Input/Output/PushConstant/SpecializationConstant 不参与 descriptor set layout 构建
        if (!IsDescriptorResourceType(shader_resource.type))
        {
            continue;
        }

        auto it2 = m_ShaderSets.find(shader_resource.set);

        if (it2 != m_ShaderSets.end())
        {
            it2->second.push_back(shader_resource);
        }
        else
        {
            m_ShaderSets.emplace(shader_resource.set, std::vector<ShaderResource>{shader_resource});
        }
    }

    // 为每个 set 通过全局缓存获取 DescriptorSetLayout（去重）
    auto &cache = GetDevice().GetResourceCache();
    for (auto &shader_set_it : m_ShaderSets)
    {
        m_DescriptorSetLayouts.push_back(
            &cache.RequestDescriptorSetLayout(
                shader_set_it.first, m_ShaderModules, shader_set_it.second));
    }

    // 收集所有 descriptor set layout 句柄，严格按 set 编号从小到大排列
    // 注意：m_ShaderSets 是 unordered_map，遍历顺序不确定，因此必须按 set index 排序
    // 对于缺失的 set 编号，用 VK_NULL_HANDLE 占位以保证数组索引 == set 编号
    uint32_t max_set = 0;
    for (auto &layout : m_DescriptorSetLayouts)
    {
        if (layout && layout->GetIndex() > max_set)
        {
            max_set = layout->GetIndex();
        }
    }

    std::vector<vk::DescriptorSetLayout> descriptor_set_layout_handles(max_set + 1, VK_NULL_HANDLE);
    for (auto &layout : m_DescriptorSetLayouts)
    {
        if (layout)
        {
            descriptor_set_layout_handles[layout->GetIndex()] = layout->GetHandle();
        }
    }

    // 收集所有 push constant 资源
    std::vector<vk::PushConstantRange> push_constant_ranges;
    for (auto &push_constant_resource : GetResources(ShaderResourceType::PushConstant))
    {
        push_constant_ranges.push_back({
            .stageFlags = static_cast<vk::ShaderStageFlags>(push_constant_resource.stages),
            .offset     = push_constant_resource.offset,
            .size       = push_constant_resource.size,
        });
    }

    vk::PipelineLayoutCreateInfo create_info{
        .setLayoutCount         = static_cast<uint32_t>(descriptor_set_layout_handles.size()),
        .pSetLayouts            = descriptor_set_layout_handles.data(),
        .pushConstantRangeCount = static_cast<uint32_t>(push_constant_ranges.size()),
        .pPushConstantRanges    = push_constant_ranges.data(),
    };

    // 创建 Vulkan pipeline layout
    auto result = GetDevice().GetHandle().createPipelineLayout(create_info);
    SetHandle(result);
}

VulkanPipelineLayout::VulkanPipelineLayout(VulkanPipelineLayout &&other) :
    Parent(std::move(other)),
    m_ShaderModules{std::move(other.m_ShaderModules)},
    m_ShaderResources{std::move(other.m_ShaderResources)},
    m_ShaderSets{std::move(other.m_ShaderSets)},
    m_DescriptorSetLayouts{std::move(other.m_DescriptorSetLayouts)}
{
    other.m_DescriptorSetLayouts.clear();
}

VulkanPipelineLayout::~VulkanPipelineLayout()
{
    if (HasHandle())
    {
        GetDevice().GetHandle().destroyPipelineLayout(GetHandle());
    }
    // m_DescriptorSetLayouts 由 VulkanResourceCache 持有生命周期，无需手动清理
}

const std::vector<VulkanShaderModule *> &VulkanPipelineLayout::GetShaderModules() const
{
    return m_ShaderModules;
}

const std::vector<ShaderResource> VulkanPipelineLayout::GetResources(const ShaderResourceType &type, vk::ShaderStageFlags stage) const
{
    std::vector<ShaderResource> found_resources;

    for (auto &it : m_ShaderResources)
    {
        auto &shader_resource = it.second;

        if (shader_resource.type == type || type == ShaderResourceType::All)
        {
            if (shader_resource.stages == static_cast<vk::ShaderStageFlags>(stage) || stage == vk::ShaderStageFlagBits::eAll)
            {
                found_resources.push_back(shader_resource);
            }
        }
    }

    return found_resources;
}

const std::unordered_map<uint32_t, std::vector<ShaderResource>> &VulkanPipelineLayout::GetShaderSets() const
{
    return m_ShaderSets;
}

bool VulkanPipelineLayout::HasDescriptorSetLayout(uint32_t set_index) const
{
    return set_index < m_DescriptorSetLayouts.size();
}

VulkanDescriptorSetLayout &VulkanPipelineLayout::GetDescriptorSetLayout(uint32_t set_index) const
{
    for (auto &descriptor_set_layout : m_DescriptorSetLayouts)
    {
        if (descriptor_set_layout && descriptor_set_layout->GetIndex() == set_index)
        {
            return *descriptor_set_layout;
        }
    }
    throw std::runtime_error("找不到 set index " + std::to_string(set_index) + " 对应的 DescriptorSetLayout");
}

vk::ShaderStageFlags VulkanPipelineLayout::GetPushConstantRangeStage(uint32_t size, uint32_t offset) const
{
    vk::ShaderStageFlags stages;

    for (auto &push_constant_resource : GetResources(ShaderResourceType::PushConstant))
    {
        if (offset >= push_constant_resource.offset && offset + size <= push_constant_resource.offset + push_constant_resource.size)
        {
            stages |= static_cast<vk::ShaderStageFlags>(push_constant_resource.stages);
        }
    }
    return stages;
}

} // namespace GE
