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
 * @file VulkanPipeline.cpp
 * @brief VulkanPipeline 各子类实现。
 */

#include "Render/VulkanBase/VulkanDevice.h"
#include "Render/VulkanBase/VulkanPipeline.h"
#include "Render/VulkanBase/VulkanPipelineLayout.h"
#include "Render/VulkanBase/VulkanShaderModule.h"

namespace GE {

// ============================================================================
// VulkanPipeline（基类）
// ============================================================================

VulkanPipeline::VulkanPipeline(VulkanDevice &device) : Parent(vk::Pipeline{}, &device) {
}

VulkanPipeline::VulkanPipeline(VulkanPipeline &&other) noexcept
    : Parent(std::move(other)),
      m_State(std::move(other.m_State)) {
}

VulkanPipeline::~VulkanPipeline() {
    if (HasHandle()) {
        GetDevice().GetHandle().destroyPipeline(GetHandle());
    }
}


// ============================================================================
// VulkanGraphicsPipeline
// ============================================================================

VulkanGraphicsPipeline::VulkanGraphicsPipeline(VulkanDevice &device,
                                               VulkanPipelineState &pipeline_state,
                                               VkPipelineCache pipeline_cache) : VulkanPipeline(device) {
    // 复制外部状态
    m_State = pipeline_state;

    // 构建动态渲染模式的管线创建信息
    const auto &pipelineInfo = m_State.buildDynamicRenderingPipeline();

    // 创建 VkPipeline
    auto result = GetDevice().GetHandle().createGraphicsPipeline(
        vk::PipelineCache{pipeline_cache},
        pipelineInfo);

    if (result.result != vk::Result::eSuccess) {
        throw std::runtime_error("Failed to create graphics pipeline");
    }

    SetHandle(result.value);
}

VulkanGraphicsPipeline::~VulkanGraphicsPipeline() {
    // 基类析构会销毁底层 VkPipeline 句柄
}


// ============================================================================
// VulkanComputePipeline
// ============================================================================
// 计算管线只依赖 VulkanPipelineLayout：布局须恰好挂载一个 compute shader
// 模块，阶段与 pipeline layout 均从布局提取，不需要 VulkanPipelineState。
// ============================================================================

VulkanComputePipeline::VulkanComputePipeline(VulkanDevice &device,
                                             VulkanPipelineLayout &pipeline_layout,
                                             VkPipelineCache pipeline_cache) : VulkanPipeline(device) {
    const auto &modules = pipeline_layout.GetShaderModules();
    if (modules.size() != 1 || modules[0]->get_stage() != vk::ShaderStageFlagBits::eCompute) {
        throw std::runtime_error(
            "Compute pipeline requires a layout containing exactly one compute shader module");
    }

    // compute 阶段（入口点字符串来自模块，存活期覆盖同步创建调用）
    vk::PipelineShaderStageCreateInfo stage{
        .stage  = vk::ShaderStageFlagBits::eCompute,
        .module = modules[0]->GetHandle(),
        .pName  = modules[0]->get_entry_point().c_str(),
    };

    vk::ComputePipelineCreateInfo compute_info{
        .stage  = stage,
        .layout = pipeline_layout.GetHandle(),
    };

    auto result = GetDevice().GetHandle().createComputePipeline(
        vk::PipelineCache{pipeline_cache}, compute_info);

    if (result.result != vk::Result::eSuccess) {
        throw std::runtime_error("Failed to create compute pipeline");
    }

    SetHandle(result.value);
}

VulkanComputePipeline::~VulkanComputePipeline() = default;

} // namespace GE
