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
 * @file VulkanPipelineState.cpp
 * @brief VulkanPipelineState 实现。
 *
 * 比较运算符 + setter 脏标记检测。
 */

#include "Render/VulkanBase/VulkanPipelineState.h"

#include "Render/VulkanBase/VulkanPipelineLayout.h"

#include <tuple>

namespace GE
{

// ====================================================================
// 比较运算符
// ====================================================================

// --- VertexInputState ---
// vk::VertexInputBindingDescription / AttributeDescription 无内置 ==，
// 手动逐成员比较。

bool operator==(const VertexInputState &lhs, const VertexInputState &rhs)
{
    if (lhs.bindings.size() != rhs.bindings.size() ||
        lhs.attributes.size() != rhs.attributes.size())
        return false;

    for (size_t i = 0; i < lhs.bindings.size(); ++i)
    {
        auto &a = lhs.bindings[i];
        auto &b = rhs.bindings[i];
        if (a.binding != b.binding || a.stride != b.stride || a.inputRate != b.inputRate)
            return false;
    }
    for (size_t i = 0; i < lhs.attributes.size(); ++i)
    {
        auto &a = lhs.attributes[i];
        auto &b = rhs.attributes[i];
        if (a.location != b.location || a.binding != b.binding ||
            a.format != b.format || a.offset != b.offset)
            return false;
    }
    return true;
}

bool operator!=(const VertexInputState &lhs, const VertexInputState &rhs)
{
    return !(lhs == rhs);
}

// --- InputAssemblyState ---

bool operator==(const InputAssemblyState &lhs, const InputAssemblyState &rhs)
{
    return std::tie(lhs.topology, lhs.primitive_restart_enable) ==
           std::tie(rhs.topology, rhs.primitive_restart_enable);
}

bool operator!=(const InputAssemblyState &lhs, const InputAssemblyState &rhs)
{
    return !(lhs == rhs);
}

// --- RasterizationState ---

bool operator==(const RasterizationState &lhs, const RasterizationState &rhs)
{
    return std::tie(lhs.depth_clamp_enable, lhs.rasterizer_discard_enable,
                    lhs.polygon_mode, lhs.cull_mode, lhs.front_face,
                    lhs.depth_bias_enable) ==
           std::tie(rhs.depth_clamp_enable, rhs.rasterizer_discard_enable,
                    rhs.polygon_mode, rhs.cull_mode, rhs.front_face,
                    rhs.depth_bias_enable);
}

bool operator!=(const RasterizationState &lhs, const RasterizationState &rhs)
{
    return !(lhs == rhs);
}

// --- ViewportState ---

bool operator==(const ViewportState &lhs, const ViewportState &rhs)
{
    return lhs.viewport_count == rhs.viewport_count &&
           lhs.scissor_count == rhs.scissor_count;
}

bool operator!=(const ViewportState &lhs, const ViewportState &rhs)
{
    return !(lhs == rhs);
}

// --- MultisampleState ---

bool operator==(const MultisampleState &lhs, const MultisampleState &rhs)
{
    return std::tie(lhs.rasterization_samples, lhs.sample_shading_enable,
                    lhs.min_sample_shading, lhs.sample_mask,
                    lhs.alpha_to_coverage_enable, lhs.alpha_to_one_enable) ==
           std::tie(rhs.rasterization_samples, rhs.sample_shading_enable,
                    rhs.min_sample_shading, rhs.sample_mask,
                    rhs.alpha_to_coverage_enable, rhs.alpha_to_one_enable);
}

bool operator!=(const MultisampleState &lhs, const MultisampleState &rhs)
{
    return !(lhs == rhs);
}

// --- StencilOpState ---

bool operator==(const StencilOpState &lhs, const StencilOpState &rhs)
{
    return std::tie(lhs.fail_op, lhs.pass_op, lhs.depth_fail_op, lhs.compare_op) ==
           std::tie(rhs.fail_op, rhs.pass_op, rhs.depth_fail_op, rhs.compare_op);
}

bool operator!=(const StencilOpState &lhs, const StencilOpState &rhs)
{
    return !(lhs == rhs);
}

// --- DepthStencilState ---

bool operator==(const DepthStencilState &lhs, const DepthStencilState &rhs)
{
    return std::tie(lhs.depth_test_enable, lhs.depth_write_enable,
                    lhs.depth_compare_op, lhs.depth_bounds_test_enable,
                    lhs.stencil_test_enable) ==
               std::tie(rhs.depth_test_enable, rhs.depth_write_enable,
                        rhs.depth_compare_op, rhs.depth_bounds_test_enable,
                        rhs.stencil_test_enable) &&
           lhs.front == rhs.front &&
           lhs.back == rhs.back;
}

bool operator!=(const DepthStencilState &lhs, const DepthStencilState &rhs)
{
    return !(lhs == rhs);
}

// --- ColorBlendAttachmentState ---

bool operator==(const ColorBlendAttachmentState &lhs, const ColorBlendAttachmentState &rhs)
{
    return std::tie(lhs.blend_enable, lhs.src_color_blend_factor,
                    lhs.dst_color_blend_factor, lhs.color_blend_op,
                    lhs.src_alpha_blend_factor, lhs.dst_alpha_blend_factor,
                    lhs.alpha_blend_op, lhs.color_write_mask) ==
           std::tie(rhs.blend_enable, rhs.src_color_blend_factor,
                    rhs.dst_color_blend_factor, rhs.color_blend_op,
                    rhs.src_alpha_blend_factor, rhs.dst_alpha_blend_factor,
                    rhs.alpha_blend_op, rhs.color_write_mask);
}

bool operator!=(const ColorBlendAttachmentState &lhs, const ColorBlendAttachmentState &rhs)
{
    return !(lhs == rhs);
}

// --- ColorBlendState ---

bool operator==(const ColorBlendState &lhs, const ColorBlendState &rhs)
{
    return std::tie(lhs.logic_op_enable, lhs.logic_op) ==
               std::tie(rhs.logic_op_enable, rhs.logic_op) &&
           lhs.attachments.size() == rhs.attachments.size() &&
           std::equal(lhs.attachments.begin(), lhs.attachments.end(),
                      rhs.attachments.begin());
}

bool operator!=(const ColorBlendState &lhs, const ColorBlendState &rhs)
{
    return !(lhs == rhs);
}

// ====================================================================
// VulkanPipelineState
// ====================================================================

void VulkanPipelineState::Reset()
{
    ClearDirty();

    m_PipelineLayout = nullptr;

    m_VertexInputState      = {};
    m_InputAssemblyState    = {};
    m_RasterizationState    = {};
    m_ViewportState         = {};
    m_MultisampleState      = {};
    m_DepthStencilState     = {};
    m_ColorBlendState       = {};
}

void VulkanPipelineState::SetPipelineLayout(VulkanPipelineLayout &pipeline_layout)
{
    const auto new_handle = pipeline_layout.GetHandle();
    const auto old_handle = m_PipelineLayout ? m_PipelineLayout->GetHandle() : vk::PipelineLayout{};

    if (new_handle != old_handle)
    {
        m_PipelineLayout = &pipeline_layout;
        m_PipelineDirty = true;
    }
}

void VulkanPipelineState::SetVertexInputState(const VertexInputState &state)
{
    if (m_VertexInputState != state)
    {
        m_VertexInputState = state;
        m_PipelineDirty = true;
    }
}

void VulkanPipelineState::SetInputAssemblyState(const InputAssemblyState &state)
{
    if (m_InputAssemblyState != state)
    {
        m_InputAssemblyState = state;
        m_DynamicDirty = true;
    }
}

void VulkanPipelineState::SetRasterizationState(const RasterizationState &state)
{
    if (m_RasterizationState != state)
    {
        m_RasterizationState = state;
        m_DynamicDirty = true;
    }
}

void VulkanPipelineState::SetViewportState(const ViewportState &state)
{
    if (m_ViewportState != state)
    {
        m_ViewportState = state;
        m_PipelineDirty = true;
    }
}

void VulkanPipelineState::SetMultisampleState(const MultisampleState &state)
{
    if (m_MultisampleState != state)
    {
        m_MultisampleState = state;
        m_PipelineDirty = true;
    }
}

void VulkanPipelineState::SetDepthStencilState(const DepthStencilState &state)
{
    if (m_DepthStencilState != state)
    {
        m_DepthStencilState = state;
        m_DynamicDirty = true;
    }
}

void VulkanPipelineState::SetColorBlendState(const ColorBlendState &state)
{
    if (m_ColorBlendState != state)
    {
        m_ColorBlendState = state;
        m_PipelineDirty = true;
    }
}

const VulkanPipelineLayout *VulkanPipelineState::GetPipelineLayout() const
{
    return m_PipelineLayout;
}

const VertexInputState &VulkanPipelineState::GetVertexInputState() const
{
    return m_VertexInputState;
}

const InputAssemblyState &VulkanPipelineState::GetInputAssemblyState() const
{
    return m_InputAssemblyState;
}

const RasterizationState &VulkanPipelineState::GetRasterizationState() const
{
    return m_RasterizationState;
}

const ViewportState &VulkanPipelineState::GetViewportState() const
{
    return m_ViewportState;
}

const MultisampleState &VulkanPipelineState::GetMultisampleState() const
{
    return m_MultisampleState;
}

const DepthStencilState &VulkanPipelineState::GetDepthStencilState() const
{
    return m_DepthStencilState;
}

const ColorBlendState &VulkanPipelineState::GetColorBlendState() const
{
    return m_ColorBlendState;
}

std::vector<vk::DynamicState> VulkanPipelineState::GetDynamicStates()
{
    return {
        vk::DynamicState::eViewport,
        vk::DynamicState::eScissor,
        vk::DynamicState::eCullMode,
        vk::DynamicState::eFrontFace,
        vk::DynamicState::ePrimitiveTopology,
        vk::DynamicState::eDepthTestEnable,
        vk::DynamicState::eDepthWriteEnable,
        vk::DynamicState::eDepthCompareOp,
        vk::DynamicState::eDepthBoundsTestEnable,
        vk::DynamicState::eStencilTestEnable,
        vk::DynamicState::eStencilOp,
        vk::DynamicState::eRasterizerDiscardEnable,
        vk::DynamicState::eDepthBiasEnable,
    };
}

bool VulkanPipelineState::IsDynamicDirty() const
{
    return m_DynamicDirty;
}

bool VulkanPipelineState::IsPipelineDirty() const
{
    return m_PipelineDirty;
}

void VulkanPipelineState::ClearDirty()
{
    m_DynamicDirty  = false;
    m_PipelineDirty = false;
}

void VulkanPipelineState::FlushDynamicState(vk::CommandBuffer cmd) const
{
    // 光栅化状态
    cmd.setCullMode(m_RasterizationState.cull_mode);
    cmd.setFrontFace(m_RasterizationState.front_face);
    cmd.setRasterizerDiscardEnable(m_RasterizationState.rasterizer_discard_enable);
    cmd.setDepthBiasEnable(m_RasterizationState.depth_bias_enable);

    // 输入装配状态
    cmd.setPrimitiveTopology(m_InputAssemblyState.topology);

    // 深度/模板状态
    cmd.setDepthTestEnable(m_DepthStencilState.depth_test_enable);
    cmd.setDepthWriteEnable(m_DepthStencilState.depth_write_enable);
    cmd.setDepthCompareOp(m_DepthStencilState.depth_compare_op);
    cmd.setDepthBoundsTestEnable(m_DepthStencilState.depth_bounds_test_enable);
    cmd.setStencilTestEnable(m_DepthStencilState.stencil_test_enable);
    cmd.setStencilOp(vk::StencilFaceFlagBits::eFront,
                     m_DepthStencilState.front.fail_op,
                     m_DepthStencilState.front.pass_op,
                     m_DepthStencilState.front.depth_fail_op,
                     m_DepthStencilState.front.compare_op);
    cmd.setStencilOp(vk::StencilFaceFlagBits::eBack,
                     m_DepthStencilState.back.fail_op,
                     m_DepthStencilState.back.pass_op,
                     m_DepthStencilState.back.depth_fail_op,
                     m_DepthStencilState.back.compare_op);
}

} // namespace GE
