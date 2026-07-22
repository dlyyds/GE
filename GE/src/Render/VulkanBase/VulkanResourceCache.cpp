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
    return RequestResource(m_ShaderModuleMutex, m_ShaderModules,
                           [&](VulkanDevice &dev) -> ShaderModule {
                               return ShaderModule(dev, stage, shader_source, entry_point, shader_variant);
                           },
                           stage, shader_source, entry_point, shader_variant);
}

VulkanPipelineLayout &VulkanResourceCache::RequestPipelineLayout(const std::vector<ShaderModule *> &shader_modules) {
    return RequestResource(m_PipelineLayoutMutex, m_PipelineLayouts,
                           [&](VulkanDevice &dev) -> VulkanPipelineLayout {
                               return VulkanPipelineLayout(dev, shader_modules);
                           },
                           shader_modules);
}

VulkanDescriptorSetLayout &VulkanResourceCache::RequestDescriptorSetLayout(uint32_t set_index,
                                                                             const std::vector<ShaderModule *> &shader_modules,
                                                                             const std::vector<ShaderResource> &set_resources) {
    return RequestResource(m_DescriptorSetLayoutMutex, m_DescriptorSetLayouts,
                           [&](VulkanDevice &dev) -> VulkanDescriptorSetLayout {
                               return VulkanDescriptorSetLayout(dev, set_index, shader_modules, set_resources);
                           },
                           set_index, shader_modules, set_resources);
}

VulkanGraphicsPipeline &VulkanResourceCache::RequestGraphicsPipeline(VulkanPipelineState &pipeline_state) {
    return RequestResource(m_GraphicsPipelineMutex, m_GraphicsPipelines,
                           [&](VulkanDevice &dev) -> VulkanGraphicsPipeline {
                               return VulkanGraphicsPipeline(dev, pipeline_state);
                           },
                           pipeline_state);
}

VulkanComputePipeline &VulkanResourceCache::RequestComputePipeline(VulkanPipelineState &pipeline_state) {
    return RequestResource(m_ComputePipelineMutex, m_ComputePipelines,
                           [&](VulkanDevice &dev) -> VulkanComputePipeline {
                               return VulkanComputePipeline(dev, VK_NULL_HANDLE, pipeline_state);
                           },
                           pipeline_state);
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