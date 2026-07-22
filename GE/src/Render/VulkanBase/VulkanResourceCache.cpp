/* Copyright (c) 2025, NVIDIA CORPORATION. All rights reserved.
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
 * @file VulkanResourceCache.cpp
 * @brief 全局资源去重缓存实现。
 */

#include "Render/VulkanBase/VulkanResourceCache.h"
#include "Render/VulkanBase/VulkanDevice.h"

#include <cstddef>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace GE {

// ============================================================================
// 构造 / 析构
// ============================================================================

VulkanResourceCache::VulkanResourceCache(VulkanDevice &device) :
    m_Device(device) {
}

VulkanResourceCache::~VulkanResourceCache() {
    Clear();
}

// ============================================================================
// 资源请求接口
// ============================================================================

ShaderModule &VulkanResourceCache::RequestShaderModule(vk::ShaderStageFlagBits stage,
                                                        const ShaderSource &shader_source,
                                                        const std::string &entry_point,
                                                        const ShaderVariant &shader_variant) {
    // ShaderModule 的构造函数签名：
    // ShaderModule(VulkanDevice &device, vk::ShaderStageFlagBits stage,
    //              const ShaderSource &shader_source, const std::string &entry_point,
    //              const ShaderVariant &shader_variant)
    //
    // 注意：hash_param 需要匹配构造函数参数列表（排除 device），
    // 但 entry_point 是 const std::string & 而非 const std::string &...，
    // 需要显式传递引用以避免拷贝。
    std::lock_guard<std::mutex> guard(m_ShaderModuleMutex);

    size_t hash{0U};
    detail::hash_param(hash, stage, shader_source, entry_point, shader_variant);

    auto res_it = m_ShaderModules.find(hash);
    if (res_it != m_ShaderModules.end()) {
        return res_it->second;
    }

    // 未命中缓存，创建新资源
    ShaderModule resource(m_Device, stage, shader_source, entry_point, shader_variant);
    auto res_ins_it = m_ShaderModules.emplace(hash, std::move(resource));
    if (!res_ins_it.second) {
        throw std::runtime_error{"插入 ShaderModule 缓存失败"};
    }

    return res_ins_it.first->second;
}

VulkanPipelineLayout &VulkanResourceCache::RequestPipelineLayout(const std::vector<ShaderModule *> &shader_modules) {
    std::lock_guard<std::mutex> guard(m_PipelineLayoutMutex);

    size_t hash{0U};
    detail::hash_param(hash, shader_modules);

    auto res_it = m_PipelineLayouts.find(hash);
    if (res_it != m_PipelineLayouts.end()) {
        return res_it->second;
    }

    VulkanPipelineLayout resource(m_Device, shader_modules);
    auto res_ins_it = m_PipelineLayouts.emplace(hash, std::move(resource));
    if (!res_ins_it.second) {
        throw std::runtime_error{"插入 PipelineLayout 缓存失败"};
    }

    return res_ins_it.first->second;
}

VulkanDescriptorSetLayout &VulkanResourceCache::RequestDescriptorSetLayout(uint32_t set_index,
                                                                             const std::vector<ShaderModule *> &shader_modules,
                                                                             const std::vector<ShaderResource> &set_resources) {
    std::lock_guard<std::mutex> guard(m_DescriptorSetLayoutMutex);

    size_t hash{0U};
    detail::hash_param(hash, set_index, shader_modules, set_resources);

    auto res_it = m_DescriptorSetLayouts.find(hash);
    if (res_it != m_DescriptorSetLayouts.end()) {
        return res_it->second;
    }

    VulkanDescriptorSetLayout resource(m_Device, set_index, shader_modules, set_resources);
    auto res_ins_it = m_DescriptorSetLayouts.emplace(hash, std::move(resource));
    if (!res_ins_it.second) {
        throw std::runtime_error{"插入 DescriptorSetLayout 缓存失败"};
    }

    return res_ins_it.first->second;
}

VulkanGraphicsPipeline &VulkanResourceCache::RequestGraphicsPipeline(VkPipelineCache pipeline_cache,
                                                                       VulkanPipelineState &pipeline_state) {
    std::lock_guard<std::mutex> guard(m_GraphicsPipelineMutex);

    size_t hash{0U};
    detail::hash_param(hash, pipeline_cache, pipeline_state);

    auto res_it = m_GraphicsPipelines.find(hash);
    if (res_it != m_GraphicsPipelines.end()) {
        return res_it->second;
    }

    // 优先使用成员变量中的 pipeline cache，若未设置则使用传入的
    VkPipelineCache effective_cache = m_PipelineCache ? m_PipelineCache : pipeline_cache;

    VulkanGraphicsPipeline resource(m_Device, effective_cache, pipeline_state);
    auto res_ins_it = m_GraphicsPipelines.emplace(hash, std::move(resource));
    if (!res_ins_it.second) {
        throw std::runtime_error{"插入 GraphicsPipeline 缓存失败"};
    }

    return res_ins_it.first->second;
}

VulkanComputePipeline &VulkanResourceCache::RequestComputePipeline(VkPipelineCache pipeline_cache,
                                                                     VulkanPipelineState &pipeline_state) {
    std::lock_guard<std::mutex> guard(m_ComputePipelineMutex);

    size_t hash{0U};
    detail::hash_param(hash, pipeline_cache, pipeline_state);

    auto res_it = m_ComputePipelines.find(hash);
    if (res_it != m_ComputePipelines.end()) {
        return res_it->second;
    }

    VkPipelineCache effective_cache = m_PipelineCache ? m_PipelineCache : pipeline_cache;

    VulkanComputePipeline resource(m_Device, effective_cache, pipeline_state);
    auto res_ins_it = m_ComputePipelines.emplace(hash, std::move(resource));
    if (!res_ins_it.second) {
        throw std::runtime_error{"插入 ComputePipeline 缓存失败"};
    }

    return res_ins_it.first->second;
}

// ============================================================================
// 生命周期管理
// ============================================================================

void VulkanResourceCache::Clear() {
    // 先清空依赖底层的资源（管线），再清空被依赖的资源
    {
        std::lock_guard<std::mutex> guard(m_GraphicsPipelineMutex);
        m_GraphicsPipelines.clear();
    }
    {
        std::lock_guard<std::mutex> guard(m_ComputePipelineMutex);
        m_ComputePipelines.clear();
    }
    {
        std::lock_guard<std::mutex> guard(m_DescriptorSetLayoutMutex);
        m_DescriptorSetLayouts.clear();
    }
    {
        std::lock_guard<std::mutex> guard(m_PipelineLayoutMutex);
        m_PipelineLayouts.clear();
    }
    {
        std::lock_guard<std::mutex> guard(m_ShaderModuleMutex);
        m_ShaderModules.clear();
    }
}

void VulkanResourceCache::ClearPipelines() {
    {
        std::lock_guard<std::mutex> guard(m_GraphicsPipelineMutex);
        m_GraphicsPipelines.clear();
    }
    {
        std::lock_guard<std::mutex> guard(m_ComputePipelineMutex);
        m_ComputePipelines.clear();
    }
}

} // namespace GE