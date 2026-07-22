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
 * @file VulkanResourceCache.h
 * @brief 全局资源去重缓存。
 *
 * 提供重量级 Vulkan 资源的全局去重缓存，避免重复创建 ShaderModule、
 * PipelineLayout、DescriptorSetLayout、GraphicsPipeline 和 ComputePipeline。
 *
 * 与 VulkanRenderFrame 互补：
 * - VulkanResourceCache：应用级全局缓存，生命周期与 Application 相同
 * - VulkanRenderFrame：每帧资源池，帧结束后重置
 *
 * 使用方式：
 * @code
 *   auto &shader = device.GetResourceCache().RequestShaderModule(
 *       stage, shader_source, "main", shader_variant);
 *   auto &layout = device.GetResourceCache().RequestPipelineLayout(
 *       shader_modules);
 * @endcode
 */

#pragma once

#include "Render/VulkanBase/ResourceCaching.h"
#include "Render/VulkanBase/ShaderModule.h"
#include "Render/VulkanBase/VulkanDescriptorSetLayout.h"
#include "Render/VulkanBase/VulkanPipeline.h"
#include "Render/VulkanBase/VulkanPipelineLayout.h"
#include "Render/VulkanBase/VulkanPipelineState.h"

#include <vulkan/vulkan.hpp>

#include <cstddef>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace GE {

class VulkanDevice;

/**
 * @brief 全局资源去重缓存。
 *
 * 管理重量级 Vulkan 资源的全局去重，所有资源通过 hash 参数自动去重。
 * 线程安全：每个资源类别有独立的互斥锁。
 */
class VulkanResourceCache {
public:
    explicit VulkanResourceCache(VulkanDevice &device);

    VulkanResourceCache(const VulkanResourceCache &) = delete;
    VulkanResourceCache(VulkanResourceCache &&) = delete;
    VulkanResourceCache &operator=(const VulkanResourceCache &) = delete;
    VulkanResourceCache &operator=(VulkanResourceCache &&) = delete;

    ~VulkanResourceCache();

    // ========================================================================
    // 资源请求接口
    // ========================================================================

    /**
     * @brief 请求着色器模块（按 stage + source + entry_point + variant 去重）。
     */
    ShaderModule &RequestShaderModule(vk::ShaderStageFlagBits stage,
                                       const ShaderSource &shader_source,
                                       const std::string &entry_point = "main",
                                       const ShaderVariant &shader_variant = {});

    /**
     * @brief 请求 PipelineLayout（按 shader_modules 指针集合去重）。
     */
    VulkanPipelineLayout &RequestPipelineLayout(const std::vector<ShaderModule *> &shader_modules);

    /**
     * @brief 请求 DescriptorSetLayout（按 set_index + shader_modules + resources 去重）。
     */
    VulkanDescriptorSetLayout &RequestDescriptorSetLayout(uint32_t set_index,
                                                           const std::vector<ShaderModule *> &shader_modules,
                                                           const std::vector<ShaderResource> &set_resources);

    /**
     * @brief 请求图形管线（按 pipeline_cache + pipeline_state 去重）。
     */
    VulkanGraphicsPipeline &RequestGraphicsPipeline(VkPipelineCache pipeline_cache,
                                                     VulkanPipelineState &pipeline_state);

    /**
     * @brief 请求计算管线（按 pipeline_cache + pipeline_state 去重）。
     */
    VulkanComputePipeline &RequestComputePipeline(VkPipelineCache pipeline_cache,
                                                   VulkanPipelineState &pipeline_state);

    // ========================================================================
    // 生命周期管理
    // ========================================================================

    /// 清空所有缓存。
    void Clear();

    /// 清空管线缓存（保留 shader module / pipeline layout / descriptor set layout）。
    void ClearPipelines();

    // ========================================================================
    // Pipeline Cache
    // ========================================================================

    /// 设置外部 VkPipelineCache（用于加速管线创建）。
    void SetPipelineCache(VkPipelineCache pipeline_cache) { m_PipelineCache = pipeline_cache; }

private:
    /**
     * @brief 带锁的 request_resource 封装。
     *
     * 在加锁后调用 GE::request_resource() 模板，确保线程安全。
     */
    template <class T, class... A>
    T &RequestResource(std::mutex &mutex, std::unordered_map<size_t, T> &resources, A &...args) {
        std::lock_guard<std::mutex> guard(mutex);
        return GE::request_resource(m_Device, resources, args...);
    }

private:
    VulkanDevice &m_Device;
    VkPipelineCache m_PipelineCache{VK_NULL_HANDLE};

    // 缓存容器
    std::unordered_map<size_t, ShaderModule>              m_ShaderModules;
    std::unordered_map<size_t, VulkanPipelineLayout>      m_PipelineLayouts;
    std::unordered_map<size_t, VulkanDescriptorSetLayout> m_DescriptorSetLayouts;
    std::unordered_map<size_t, VulkanGraphicsPipeline>    m_GraphicsPipelines;
    std::unordered_map<size_t, VulkanComputePipeline>     m_ComputePipelines;

    // 互斥锁
    std::mutex m_ShaderModuleMutex;
    std::mutex m_PipelineLayoutMutex;
    std::mutex m_DescriptorSetLayoutMutex;
    std::mutex m_GraphicsPipelineMutex;
    std::mutex m_ComputePipelineMutex;
};

} // namespace GE