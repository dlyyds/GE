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

#include "Render/VulkanBase/VulkanPipeline.h"
#include "Render/VulkanBase/VulkanDevice.h"

#include <cstdint>
#include <type_traits>

namespace GE {

// ============================================================================
// VulkanPipeline（基类）
// ============================================================================

VulkanPipeline::VulkanPipeline(VulkanDevice &device) : m_Device(device) {
}

VulkanPipeline::VulkanPipeline(VulkanPipeline &&other) noexcept : m_Device(other.m_Device),
                                                                  m_Handle(other.m_Handle),
                                                                  m_State(std::move(other.m_State)) {
    other.m_Handle = VK_NULL_HANDLE;
}

VulkanPipeline::~VulkanPipeline() {
    if (m_Handle) {
        m_Device.GetHandle().destroyPipeline(m_Handle);
    }
}

void VulkanPipeline::SetDebugName(const std::string &name) {
    if (m_Handle && !name.empty()) {
        // vk::Pipeline 是非调度句柄，大小可能为 uint32_t 或 uint64_t（取决于平台）。
        // 与 VulkanResourceBase::GetHandleU64 保持一致，通过编译时大小检测转换。
        using UintHandle = typename std::conditional<sizeof(vk::Pipeline) == sizeof(uint32_t),
                                                     uint32_t, uint64_t>::type;
        uint64_t handle_u64 = static_cast<uint64_t>(
            *reinterpret_cast<const UintHandle *>(&m_Handle));

        m_Device.GetDebugUtils().SetDebugName(
            m_Device.GetHandle(),
            vk::ObjectType::ePipeline,
            handle_u64,
            name.c_str());
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

    // 构建管线创建信息
    auto bundle = m_State.BuildCreateInfo();

    // 创建 VkPipeline
    auto result = m_Device.GetHandle().createGraphicsPipeline(
        vk::PipelineCache{pipeline_cache},
        bundle.pipelineInfo);

    if (result.result != vk::Result::eSuccess) {
        throw std::runtime_error("Failed to create graphics pipeline");
    }

    m_Handle = result.value;

    // 清除脏标记，标记当前状态为已创建管线
    m_State.ClearAllDirty();
}

VulkanGraphicsPipeline::~VulkanGraphicsPipeline() {
    // 基类析构会销毁 m_Handle
}


// ============================================================================
// VulkanComputePipeline
// ============================================================================
// TODO: 计算管线使用 vk::ComputePipelineCreateInfo，需要从 VulkanPipelineState
//       中提取 compute shader 阶段和 pipeline layout。当前 VulkanPipelineState
//       主要针对图形管线设计，计算管线功能待后续补充。
// ============================================================================

VulkanComputePipeline::VulkanComputePipeline(VulkanDevice &device,
                                             VulkanPipelineState & /*pipeline_state*/,
                                             VkPipelineCache /*pipeline_cache*/) : VulkanPipeline(device) {
    throw std::runtime_error("Compute pipeline not yet implemented");
}

VulkanComputePipeline::~VulkanComputePipeline() = default;

} // namespace GE
