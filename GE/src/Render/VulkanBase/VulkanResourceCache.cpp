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

VulkanShaderModule &VulkanResourceCache::RequestShaderModule(vk::ShaderStageFlagBits stage,
                                                              const ShaderSource &shader_source,
                                                              const std::string &entry_point,
                                                              const ShaderVariant &shader_variant) {
    return RequestResource<VulkanShaderModule>(m_ShaderModuleMutex, m_ShaderModules,
                                               stage, shader_source, entry_point, shader_variant);
}

VulkanPipelineLayout &VulkanResourceCache::RequestPipelineLayout(const std::vector<VulkanShaderModule *> &shader_modules) {
    return RequestResource<VulkanPipelineLayout>(m_PipelineLayoutMutex, m_PipelineLayouts,
                                                  shader_modules);
}

VulkanDescriptorSetLayout &VulkanResourceCache::RequestDescriptorSetLayout(uint32_t set_index,
                                                                             const std::vector<VulkanShaderModule *> &shader_modules,
                                                                             const std::vector<ShaderResource> &set_resources) {
    return RequestResource<VulkanDescriptorSetLayout>(m_DescriptorSetLayoutMutex, m_DescriptorSetLayouts,
                                                       set_index, shader_modules, set_resources);
}

VulkanGraphicsPipeline &VulkanResourceCache::RequestGraphicsPipeline(VulkanPipelineState &pipeline_state) {
    return RequestResource<VulkanGraphicsPipeline>(m_GraphicsPipelineMutex, m_GraphicsPipelines,
                                                    pipeline_state);
}

VulkanComputePipeline &VulkanResourceCache::RequestComputePipeline(VulkanPipelineState &pipeline_state) {
    return RequestResource<VulkanComputePipeline>(m_ComputePipelineMutex, m_ComputePipelines,
                                                   pipeline_state);
}

VulkanSampler &VulkanResourceCache::RequestSampler(vk::Filter              mag_filter,
                                                    vk::Filter              min_filter,
                                                    vk::SamplerMipmapMode   mipmap_mode,
                                                    vk::SamplerAddressMode  address_mode_u,
                                                    vk::SamplerAddressMode  address_mode_v,
                                                    vk::SamplerAddressMode  address_mode_w,
                                                    float                   mip_lod_bias,
                                                    vk::Bool32             anisotropy_enable,
                                                    float                   max_anisotropy,
                                                    vk::Bool32             compare_enable,
                                                    vk::CompareOp           compare_op,
                                                    float                   min_lod,
                                                    float                   max_lod,
                                                    vk::BorderColor         border_color,
                                                    vk::Bool32             unnormalized_coordinates) {
    return RequestResource<VulkanSampler>(m_SamplerMutex, m_Samplers,
                                          mag_filter, min_filter, mipmap_mode,
                                          address_mode_u, address_mode_v, address_mode_w,
                                          mip_lod_bias, anisotropy_enable, max_anisotropy,
                                          compare_enable, compare_op,
                                          min_lod, max_lod, border_color, unnormalized_coordinates);
}

// ============================================================================
// 生命周期管理
// ============================================================================

void VulkanResourceCache::Clear() {
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
        std::lock_guard<std::mutex> guard(m_SamplerMutex);
        m_Samplers.clear();
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