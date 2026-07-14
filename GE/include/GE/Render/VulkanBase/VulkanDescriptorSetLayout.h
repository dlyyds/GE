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
 * @file VulkanDescriptorSetLayout.h
 * @brief DescriptorSetLayout 封装，从 Vulkan-Samples 适配而来。
 *
 * 对着色器 set 索引的 DescriptorSetLayout 进行缓存。
 * 从 ShaderResource 列表创建 VkDescriptorSetLayout，
 * 支持 update-after-bind 扩展，并提供 binding 名称查找。
 */

#pragma once

#include <vulkan/vulkan.hpp>

#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace GE
{

class ShaderModule;
class VulkanDevice;
struct ShaderResource;

/**
 * @brief 缓存着色器 set 索引的 DescriptorSetLayout。
 *        从 ShaderResource 创建 VkDescriptorSetLayout，
 *        并提供 binding 编号和名称的查找。
 */
class VulkanDescriptorSetLayout
{
  public:
    /**
     * @brief 从一组着色器资源创建 DescriptorSetLayout。
     * @param device        Vulkan 设备
     * @param set_index     此 layout 对应的 descriptor set 索引
     * @param shader_modules 此 set layout 涉及的着色器模块
     * @param resource_set  同一 set 的着色器资源分组
     */
    VulkanDescriptorSetLayout(VulkanDevice                   &device,
                              uint32_t                        set_index,
                              const std::vector<ShaderModule *> &shader_modules,
                              const std::vector<ShaderResource> &resource_set);

    VulkanDescriptorSetLayout(const VulkanDescriptorSetLayout &) = delete;

    VulkanDescriptorSetLayout(VulkanDescriptorSetLayout &&other);

    ~VulkanDescriptorSetLayout();

    VulkanDescriptorSetLayout &operator=(const VulkanDescriptorSetLayout &) = delete;

    VulkanDescriptorSetLayout &operator=(VulkanDescriptorSetLayout &&) = delete;

    vk::DescriptorSetLayout GetHandle() const { return m_Handle; }

    uint32_t GetIndex() const;

    const std::vector<vk::DescriptorSetLayoutBinding> &GetBindings() const;

    std::unique_ptr<vk::DescriptorSetLayoutBinding> GetLayoutBinding(uint32_t binding_index) const;

    std::unique_ptr<vk::DescriptorSetLayoutBinding> GetLayoutBinding(const std::string &name) const;

    const std::vector<vk::DescriptorBindingFlagsEXT> &GetBindingFlags() const;

    vk::DescriptorBindingFlagsEXT GetLayoutBindingFlag(uint32_t binding_index) const;

    const std::vector<ShaderModule *> &GetShaderModules() const;

  private:
    VulkanDevice &m_Device;

    vk::DescriptorSetLayout m_Handle{VK_NULL_HANDLE};

    uint32_t m_SetIndex;

    std::vector<vk::DescriptorSetLayoutBinding> m_Bindings;

    std::vector<vk::DescriptorBindingFlagsEXT> m_BindingFlags;

    std::unordered_map<uint32_t, vk::DescriptorSetLayoutBinding> m_BindingsLookup;

    std::unordered_map<uint32_t, vk::DescriptorBindingFlagsEXT> m_BindingFlagsLookup;

    std::unordered_map<std::string, uint32_t> m_ResourcesLookup;

    std::vector<ShaderModule *> m_ShaderModules;
};

} // namespace GE
