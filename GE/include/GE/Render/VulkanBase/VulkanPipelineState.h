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
 * @file VulkanPipelineState.h
 * @brief 管线状态封装，从 Vulkan-Samples 适配，专用于动态管线。
 *
 * 提供状态结构体（VertexInput、InputAssembly、Rasterization 等）和
 * VulkanPipelineState 类，支持脏标记追踪和动态状态列表生成。
 * —— 不依赖 RenderPass / SpecializationConstant ——
 */

#pragma once

#include <vulkan/vulkan.hpp>

#include <cstdint>
#include <vector>

namespace GE
{

class VulkanPipelineLayout;

// ====================================================================
// 管线状态结构体
// ====================================================================

struct VertexInputState
{
    std::vector<vk::VertexInputBindingDescription>    bindings;
    std::vector<vk::VertexInputAttributeDescription>  attributes;
};

struct InputAssemblyState
{
    vk::PrimitiveTopology topology{vk::PrimitiveTopology::eTriangleList};
    vk::Bool32            primitive_restart_enable{VK_FALSE};
};

struct RasterizationState
{
    vk::Bool32       depth_clamp_enable{VK_FALSE};
    vk::Bool32       rasterizer_discard_enable{VK_FALSE};
    vk::PolygonMode  polygon_mode{vk::PolygonMode::eFill};
    vk::CullModeFlags cull_mode{vk::CullModeFlagBits::eBack};
    vk::FrontFace    front_face{vk::FrontFace::eCounterClockwise};
    vk::Bool32       depth_bias_enable{VK_FALSE};
};

struct ViewportState
{
    uint32_t viewport_count{1};
    uint32_t scissor_count{1};
};

struct MultisampleState
{
    vk::SampleCountFlagBits rasterization_samples{vk::SampleCountFlagBits::e1};
    vk::Bool32              sample_shading_enable{VK_FALSE};
    float                   min_sample_shading{0.0f};
    vk::SampleMask          sample_mask{0};
    vk::Bool32              alpha_to_coverage_enable{VK_FALSE};
    vk::Bool32              alpha_to_one_enable{VK_FALSE};
};

struct StencilOpState
{
    vk::StencilOp  fail_op{vk::StencilOp::eReplace};
    vk::StencilOp  pass_op{vk::StencilOp::eReplace};
    vk::StencilOp  depth_fail_op{vk::StencilOp::eReplace};
    vk::CompareOp  compare_op{vk::CompareOp::eNever};
};

struct DepthStencilState
{
    vk::Bool32     depth_test_enable{VK_TRUE};
    vk::Bool32     depth_write_enable{VK_TRUE};
    vk::CompareOp  depth_compare_op{vk::CompareOp::eGreater};
    vk::Bool32     depth_bounds_test_enable{VK_FALSE};
    vk::Bool32     stencil_test_enable{VK_FALSE};
    StencilOpState front{};
    StencilOpState back{};
};

struct ColorBlendAttachmentState
{
    vk::Bool32       blend_enable{VK_FALSE};
    vk::BlendFactor  src_color_blend_factor{vk::BlendFactor::eOne};
    vk::BlendFactor  dst_color_blend_factor{vk::BlendFactor::eZero};
    vk::BlendOp      color_blend_op{vk::BlendOp::eAdd};
    vk::BlendFactor  src_alpha_blend_factor{vk::BlendFactor::eOne};
    vk::BlendFactor  dst_alpha_blend_factor{vk::BlendFactor::eZero};
    vk::BlendOp      alpha_blend_op{vk::BlendOp::eAdd};
    vk::ColorComponentFlags color_write_mask{
        vk::ColorComponentFlagBits::eR | vk::ColorComponentFlagBits::eG |
        vk::ColorComponentFlagBits::eB | vk::ColorComponentFlagBits::eA};
};

struct ColorBlendState
{
    vk::Bool32                          logic_op_enable{VK_FALSE};
    vk::LogicOp                         logic_op{vk::LogicOp::eClear};
    std::vector<ColorBlendAttachmentState> attachments;
};

// ====================================================================
// 比较运算符（GE 命名空间，用于脏检测）
// ====================================================================

bool operator==(const VertexInputState &lhs, const VertexInputState &rhs);
bool operator!=(const VertexInputState &lhs, const VertexInputState &rhs);

bool operator==(const InputAssemblyState &lhs, const InputAssemblyState &rhs);
bool operator!=(const InputAssemblyState &lhs, const InputAssemblyState &rhs);

bool operator==(const RasterizationState &lhs, const RasterizationState &rhs);
bool operator!=(const RasterizationState &lhs, const RasterizationState &rhs);

bool operator==(const ViewportState &lhs, const ViewportState &rhs);
bool operator!=(const ViewportState &lhs, const ViewportState &rhs);

bool operator==(const MultisampleState &lhs, const MultisampleState &rhs);
bool operator!=(const MultisampleState &lhs, const MultisampleState &rhs);

bool operator==(const StencilOpState &lhs, const StencilOpState &rhs);
bool operator!=(const StencilOpState &lhs, const StencilOpState &rhs);

bool operator==(const DepthStencilState &lhs, const DepthStencilState &rhs);
bool operator!=(const DepthStencilState &lhs, const DepthStencilState &rhs);

bool operator==(const ColorBlendAttachmentState &lhs, const ColorBlendAttachmentState &rhs);
bool operator!=(const ColorBlendAttachmentState &lhs, const ColorBlendAttachmentState &rhs);

bool operator==(const ColorBlendState &lhs, const ColorBlendState &rhs);
bool operator!=(const ColorBlendState &lhs, const ColorBlendState &rhs);

// ====================================================================
// VulkanPipelineState — 动态管线状态追踪
// ====================================================================

/**
 * @brief 追踪管线各状态并支持脏标记。
 *
 * 专为动态管线设计，不依赖 RenderPass。
 * 每个 setter 在值真正变更时才置脏，
 * GetDynamicStates() 返回应启用的 vk::DynamicState 列表。
 */
class VulkanPipelineState
{
  public:
    void Reset();

    void SetPipelineLayout(VulkanPipelineLayout &pipeline_layout);
    void SetVertexInputState(const VertexInputState &state);
    void SetInputAssemblyState(const InputAssemblyState &state);
    void SetRasterizationState(const RasterizationState &state);
    void SetViewportState(const ViewportState &state);
    void SetMultisampleState(const MultisampleState &state);
    void SetDepthStencilState(const DepthStencilState &state);
    void SetColorBlendState(const ColorBlendState &state);

    [[nodiscard]] const VulkanPipelineLayout *GetPipelineLayout() const;

    [[nodiscard]] const VertexInputState      &GetVertexInputState() const;
    [[nodiscard]] const InputAssemblyState     &GetInputAssemblyState() const;
    [[nodiscard]] const RasterizationState     &GetRasterizationState() const;
    [[nodiscard]] const ViewportState          &GetViewportState() const;
    [[nodiscard]] const MultisampleState       &GetMultisampleState() const;
    [[nodiscard]] const DepthStencilState      &GetDepthStencilState() const;
    [[nodiscard]] const ColorBlendState        &GetColorBlendState() const;

    /// 返回应在 VkPipelineDynamicStateCreateInfo 中启用的所有动态状态
    [[nodiscard]] static std::vector<vk::DynamicState> GetDynamicStates();

    [[nodiscard]] bool IsDirty() const;
    void               ClearDirty();

  private:
    bool m_Dirty{false};

    VulkanPipelineLayout *m_PipelineLayout{nullptr};

    VertexInputState      m_VertexInputState{};
    InputAssemblyState    m_InputAssemblyState{};
    RasterizationState    m_RasterizationState{};
    ViewportState         m_ViewportState{};
    MultisampleState      m_MultisampleState{};
    DepthStencilState     m_DepthStencilState{};
    ColorBlendState       m_ColorBlendState{};
};

} // namespace GE
