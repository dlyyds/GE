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
 * @file VulkanDescriptorSetLayout.cpp
 * @brief VulkanDescriptorSetLayout 实现，从 Vulkan-Samples 适配。
 */

#include "Render/VulkanBase/VulkanDescriptorSetLayout.h"

#include "Render/VulkanBase/VulkanShaderModule.h"
#include "Render/VulkanBase/VulkanDevice.h"
#include "Core/Log.h"

namespace GE {
namespace {

/// 将 ShaderResourceType 转换为 vk::DescriptorType
inline vk::DescriptorType find_descriptor_type(ShaderResourceType resource_type, bool dynamic) {
    switch (resource_type) {
    case ShaderResourceType::InputAttachment: return vk::DescriptorType::eInputAttachment;
    case ShaderResourceType::Image: return vk::DescriptorType::eSampledImage;
    case ShaderResourceType::ImageSampler: return vk::DescriptorType::eCombinedImageSampler;
    case ShaderResourceType::ImageStorage: return vk::DescriptorType::eStorageImage;
    case ShaderResourceType::Sampler: return vk::DescriptorType::eSampler;
    case ShaderResourceType::BufferUniform: return dynamic
                                                       ? vk::DescriptorType::eUniformBufferDynamic
                                                       : vk::DescriptorType::eUniformBuffer;
    case ShaderResourceType::BufferStorage: return dynamic
                                                       ? vk::DescriptorType::eStorageBufferDynamic
                                                       : vk::DescriptorType::eStorageBuffer;
    default: throw std::runtime_error("No conversion possible for the shader resource type.");
    }
}

/// 检查 binding 是否不在黑名单中
inline bool validate_binding(const vk::DescriptorSetLayoutBinding &binding,
                             const std::vector<vk::DescriptorType> &blacklist) {
    return std::ranges::find_if(blacklist, [&binding](vk::DescriptorType type) { return type == binding.descriptorType; }) == blacklist.end();
}

/// 检查 binding flags 是否有效
inline bool validate_flags(const std::vector<vk::DescriptorSetLayoutBinding> &bindings,
                           const std::vector<vk::DescriptorBindingFlagsEXT> &flags) {
    // 无 flags 则默认有效
    if (flags.empty()) {
        return true;
    }

    // binding 数量必须与 flag 数量一致（1:1 映射）
    if (bindings.size() != flags.size()) {
        GE_CORE_ERROR("Binding count has to be equal to flag count.");
        return false;
    }

    return true;
}

} // anonymous namespace

// ============================================================
// VulkanDescriptorSetLayout
// ============================================================

VulkanDescriptorSetLayout::VulkanDescriptorSetLayout(
    VulkanDevice &device,
    uint32_t set_index,
    const std::vector<VulkanShaderModule *> &shader_modules,
    const std::vector<ShaderResource> &resource_set) : m_Device(device),
                                                       m_SetIndex(set_index),
                                                       m_ShaderModules(shader_modules) {
    // shader_modules 主要用于在 request_resource 中对它们的句柄做哈希。
    // 这样不同管线（不同着色器/着色器变体）会获得不同的 descriptor set layout
    // （包含相应的 name → binding 查找表）。

    for (auto &resource : resource_set) {
        // 跳过没有 binding 点的着色器资源
        if (resource.type == ShaderResourceType::Input ||
            resource.type == ShaderResourceType::Output ||
            resource.type == ShaderResourceType::PushConstant ||
            resource.type == ShaderResourceType::SpecializationConstant) {
            continue;
        }

        // 从 ShaderResourceType 转换为 vk::DescriptorType
        auto descriptor_type = find_descriptor_type(resource.type,
                                                    resource.mode == ShaderResourceMode::Dynamic);

        if (resource.mode == ShaderResourceMode::UpdateAfterBind) {
            m_BindingFlags.push_back(vk::DescriptorBindingFlagBitsEXT::eUpdateAfterBind);
        } else {
            // 创建 descriptor set layout 时，若给 create_info.pNext 传结构体，
            // 则每个 binding 都需要有一个 binding flag（pBindings[i] 使用 pBindingFlags[i]）。
            // 添加 0 确保不使用 flags 的 binding 正确映射。
            m_BindingFlags.push_back(static_cast<vk::DescriptorBindingFlagBitsEXT>(0));
        }

        // 将 ShaderResource 转换为 vk::DescriptorSetLayoutBinding
        vk::DescriptorSetLayoutBinding layout_binding{
            .binding = resource.binding,
            .descriptorType = descriptor_type,
            .descriptorCount = resource.array_size,
            .stageFlags = resource.stages,
        };

        m_Bindings.push_back(layout_binding);

        // 存储 binding → layout_binding 映射
        m_BindingsLookup.emplace(resource.binding, layout_binding);
        m_BindingFlagsLookup.emplace(resource.binding, m_BindingFlags.back());
        m_ResourcesLookup.emplace(resource.name, resource.binding);
    }

    vk::DescriptorSetLayoutCreateInfo create_info{
        .bindingCount = static_cast<uint32_t>(m_Bindings.size()),
        .pBindings = m_Bindings.data(),
    };

    // 处理 UpdateAfterBind 扩展
    vk::DescriptorSetLayoutBindingFlagsCreateInfoEXT binding_flags_create_info{};
    bool has_update_after_bind = std::ranges::find_if(resource_set,
                                                      [](const ShaderResource &res) {
                                                          return res.mode == ShaderResourceMode::UpdateAfterBind;
                                                      }) != resource_set.end();

    if (has_update_after_bind) {
        // 规范规定：若任一 binding 设置为 update-after-bind，则不能有动态资源
        if (std::ranges::find_if(resource_set,
                                 [](const ShaderResource &res) { return res.mode == ShaderResourceMode::Dynamic; }) != resource_set.end()) {
            throw std::runtime_error(
                "Cannot create descriptor set layout, dynamic resources are not allowed if at least one resource is update-after-bind.");
        }

        if (!validate_flags(m_Bindings, m_BindingFlags)) {
            throw std::runtime_error("Invalid binding, couldn't create descriptor set layout.");
        }

        binding_flags_create_info = vk::DescriptorSetLayoutBindingFlagsCreateInfoEXT{
            .bindingCount = static_cast<uint32_t>(m_BindingFlags.size()),
            .pBindingFlags = m_BindingFlags.data(),
        };

        create_info.pNext = &binding_flags_create_info;

        if (std::ranges::find(m_BindingFlags, vk::DescriptorBindingFlagBitsEXT::eUpdateAfterBind) != m_BindingFlags.end()) {
            create_info.flags |= vk::DescriptorSetLayoutCreateFlagBits::eUpdateAfterBindPool;
        }
    }

    // 创建 Vulkan DescriptorSetLayout
    m_Handle = device.GetHandle().createDescriptorSetLayout(create_info);
}

VulkanDescriptorSetLayout::VulkanDescriptorSetLayout(VulkanDescriptorSetLayout &&other) noexcept : m_Device(other.m_Device),
                                                                                                   m_Handle(other.m_Handle),
                                                                                                   m_SetIndex(other.m_SetIndex),
                                                                                                   m_Bindings(std::move(other.m_Bindings)),
                                                                                                   m_BindingFlags(std::move(other.m_BindingFlags)),
                                                                                                   m_BindingsLookup(
                                                                                                       std::move(other.m_BindingsLookup)),
                                                                                                   m_BindingFlagsLookup(
                                                                                                       std::move(other.m_BindingFlagsLookup)),
                                                                                                   m_ResourcesLookup(
                                                                                                       std::move(other.m_ResourcesLookup)),
                                                                                                   m_ShaderModules(std::move(other.m_ShaderModules)) {
    other.m_Handle = VK_NULL_HANDLE;
}

VulkanDescriptorSetLayout::~VulkanDescriptorSetLayout() {
    if (m_Handle) {
        m_Device.GetHandle().destroyDescriptorSetLayout(m_Handle);
    }
}

uint32_t VulkanDescriptorSetLayout::GetIndex() const {
    return m_SetIndex;
}

const std::vector<vk::DescriptorSetLayoutBinding> &VulkanDescriptorSetLayout::GetBindings() const {
    return m_Bindings;
}

const std::vector<vk::DescriptorBindingFlagsEXT> &VulkanDescriptorSetLayout::GetBindingFlags() const {
    return m_BindingFlags;
}

const vk::DescriptorSetLayoutBinding *VulkanDescriptorSetLayout::GetLayoutBinding(uint32_t binding_index) const {
    auto it = m_BindingsLookup.find(binding_index);
    if (it == m_BindingsLookup.end()) {
        return nullptr;
    }
    return &it->second;
}

const vk::DescriptorSetLayoutBinding *VulkanDescriptorSetLayout::GetLayoutBinding(const std::string &name) const {
    auto it = m_ResourcesLookup.find(name);
    if (it == m_ResourcesLookup.end()) {
        return nullptr;
    }
    return GetLayoutBinding(it->second);
}

vk::DescriptorBindingFlagsEXT VulkanDescriptorSetLayout::GetLayoutBindingFlag(uint32_t binding_index) const {
    auto it = m_BindingFlagsLookup.find(binding_index);
    if (it == m_BindingFlagsLookup.end()) {
        return vk::DescriptorBindingFlagBitsEXT(0);
    }
    return it->second;
}

const std::vector<VulkanShaderModule *> &VulkanDescriptorSetLayout::GetShaderModules() const {
    return m_ShaderModules;
}

} // namespace GE
