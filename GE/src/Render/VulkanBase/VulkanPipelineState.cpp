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
 * 包含 Dirty 检查、动态状态刷入、一键创建 Bundle 构建。
 */

#include "Render/VulkanBase/VulkanPipelineState.h"
#include "Render/VulkanBase/VulkanPipelineLayout.h"

#include <algorithm>
#include <cstring>

namespace GE
{

// ============================================================================
// 混合附件操作方法
// ============================================================================

void VulkanPipelineState::SetBlendAttachments(const std::vector<BlendAttachment> &attachments)
{
    m_BlendAttachments      = attachments;
    m_BlendAttachmentsDirty = true;
}

void VulkanPipelineState::SetBlendAttachment(uint32_t index, const BlendAttachment &attachment)
{
    if (index >= m_BlendAttachments.size())
    {
        m_BlendAttachments.resize(index + 1);
    }
    m_BlendAttachments[index] = attachment;
    m_BlendAttachmentsDirty   = true;
}


// ============================================================================
// Reset
// ============================================================================

void VulkanPipelineState::Reset()
{
    // StaticParam 重置：直接赋默认值，保持脏标记
    colorAttachmentFormats   = {};
    depthFormat              = {};
    stencilFormat            = {};
    shaderStages             = {};
    pipelineLayout           = {};
    vertexBindingDescriptions   = {};
    vertexAttributeDescriptions = {};
    primitiveRestartEnable   = {VK_FALSE};
    patchControlPoints       = {3};
    depthClampEnable         = {VK_FALSE};
    polygonMode              = {vk::PolygonMode::eFill};
    rasterizationSamples     = {vk::SampleCountFlagBits::e1};
    sampleShadingEnable      = {VK_FALSE};
    minSampleShading         = {0.0f};
    sampleMask               = {0};
    alphaToCoverageEnable    = {VK_FALSE};
    alphaToOneEnable         = {VK_FALSE};
    logicOpEnable            = {VK_FALSE};
    logicOp                  = {vk::LogicOp::eClear};

    // DynamicParam 重置
    topology                 = {vk::PrimitiveTopology::eTriangleList};
    viewportCount            = {1};
    scissorCount             = {1};
    rasterizerDiscardEnable  = {VK_FALSE};
    cullMode                 = {vk::CullModeFlagBits::eBack};
    frontFace                = {vk::FrontFace::eCounterClockwise};
    depthBiasEnable          = {VK_FALSE};
    depthTestEnable          = {VK_TRUE};
    depthWriteEnable         = {VK_TRUE};
    depthCompareOp           = {vk::CompareOp::eLess};
    depthBoundsTestEnable    = {VK_FALSE};
    stencilTestEnable        = {VK_FALSE};

    m_BlendAttachments.clear();
    m_BlendAttachmentsDirty = true;    // 需要重建管线

    // StencilOpState 重置
    stencilFront = StencilOpState{};
    stencilBack  = StencilOpState{};

    // 注意：DynamicParam 的 m_ConfigDirty 在 ClearAllDirty 中清除，
    // 但 Reset 后应保持为 true（需要重建管线），所以这里显式设置为 true。
    // ClearAllDirty 会清掉所有脏标记，因此先 Reset 值再手动标记。
    ClearAllDirty();
}


// ============================================================================
// 脏标记查询
// ============================================================================

bool VulkanPipelineState::HasDynamicDirty() const
{
    // 检查所有 DynamicParam：当它是动态的且值发生过变更时返回 true
    auto check = [](const auto &param) -> bool {
        return param.IsDynamic() && param.IsValueDirty();
    };

    return check(topology)
        || check(viewportCount)
        || check(scissorCount)
        || check(rasterizerDiscardEnable)
        || check(cullMode)
        || check(frontFace)
        || check(depthBiasEnable)
        || check(depthTestEnable)
        || check(depthWriteEnable)
        || check(depthCompareOp)
        || check(depthBoundsTestEnable)
        || check(stencilTestEnable)
        || (stencilFront.IsAnyDynamic() && stencilFront.IsAnyValueDirty())
        || (stencilBack.IsAnyDynamic()  && stencilBack.IsAnyValueDirty());
}

bool VulkanPipelineState::HasPipelineDirty() const
{
    // ---- StaticParam 值脏 ----
    if (colorAttachmentFormats.IsValueDirty()
        || depthFormat.IsValueDirty()
        || stencilFormat.IsValueDirty()
        || shaderStages.IsValueDirty()
        || pipelineLayout.IsValueDirty()
        || vertexBindingDescriptions.IsValueDirty()
        || vertexAttributeDescriptions.IsValueDirty()
        || primitiveRestartEnable.IsValueDirty()
        || patchControlPoints.IsValueDirty()
        || depthClampEnable.IsValueDirty()
        || polygonMode.IsValueDirty()
        || rasterizationSamples.IsValueDirty()
        || sampleShadingEnable.IsValueDirty()
        || minSampleShading.IsValueDirty()
        || sampleMask.IsValueDirty()
        || alphaToCoverageEnable.IsValueDirty()
        || alphaToOneEnable.IsValueDirty()
        || logicOpEnable.IsValueDirty()
        || logicOp.IsValueDirty()
        || m_BlendAttachmentsDirty)
    {
        return true;
    }

    // ---- DynamicParam 值脏且当前为静态（值变更需重建管线） ----
    auto checkStaticValue = [](const auto &param) -> bool {
        return !param.IsDynamic() && param.IsValueDirty();
    };

    if (checkStaticValue(topology)
        || checkStaticValue(viewportCount)
        || checkStaticValue(scissorCount)
        || checkStaticValue(rasterizerDiscardEnable)
        || checkStaticValue(cullMode)
        || checkStaticValue(frontFace)
        || checkStaticValue(depthBiasEnable)
        || checkStaticValue(depthTestEnable)
        || checkStaticValue(depthWriteEnable)
        || checkStaticValue(depthCompareOp)
        || checkStaticValue(depthBoundsTestEnable)
        || checkStaticValue(stencilTestEnable))
    {
        return true;
    }

    // StencilOpState：当子字段为静态且值变更时
    if ((!stencilFront.IsAnyDynamic() && stencilFront.IsAnyValueDirty())
        || (!stencilBack.IsAnyDynamic() && stencilBack.IsAnyValueDirty()))
    {
        return true;
    }

    // ---- DynamicParam 配置变更（动态↔静态切换，需重建管线） ----
    auto checkConfig = [](const auto &param) -> bool {
        return param.IsConfigDirty();
    };

    if (checkConfig(topology)
        || checkConfig(viewportCount)
        || checkConfig(scissorCount)
        || checkConfig(rasterizerDiscardEnable)
        || checkConfig(cullMode)
        || checkConfig(frontFace)
        || checkConfig(depthBiasEnable)
        || checkConfig(depthTestEnable)
        || checkConfig(depthWriteEnable)
        || checkConfig(depthCompareOp)
        || checkConfig(depthBoundsTestEnable)
        || checkConfig(stencilTestEnable))
    {
        return true;
    }

    if (stencilFront.IsAnyConfigDirty() || stencilBack.IsAnyConfigDirty())
    {
        return true;
    }

    return false;
}

void VulkanPipelineState::ClearAllDirty()
{
    // StaticParam
    colorAttachmentFormats.ClearDirty();
    depthFormat.ClearDirty();
    stencilFormat.ClearDirty();
    shaderStages.ClearDirty();
    pipelineLayout.ClearDirty();
    vertexBindingDescriptions.ClearDirty();
    vertexAttributeDescriptions.ClearDirty();
    primitiveRestartEnable.ClearDirty();
    patchControlPoints.ClearDirty();
    depthClampEnable.ClearDirty();
    polygonMode.ClearDirty();
    rasterizationSamples.ClearDirty();
    sampleShadingEnable.ClearDirty();
    minSampleShading.ClearDirty();
    sampleMask.ClearDirty();
    alphaToCoverageEnable.ClearDirty();
    alphaToOneEnable.ClearDirty();
    logicOpEnable.ClearDirty();
    logicOp.ClearDirty();

    // DynamicParam
    topology.ClearDirty();
    viewportCount.ClearDirty();
    scissorCount.ClearDirty();
    rasterizerDiscardEnable.ClearDirty();
    cullMode.ClearDirty();
    frontFace.ClearDirty();
    depthBiasEnable.ClearDirty();
    depthTestEnable.ClearDirty();
    depthWriteEnable.ClearDirty();
    depthCompareOp.ClearDirty();
    depthBoundsTestEnable.ClearDirty();
    stencilTestEnable.ClearDirty();

    // StencilOpState
    stencilFront.ClearAllDirty();
    stencilBack.ClearAllDirty();

    // Blend attachments
    m_BlendAttachmentsDirty = false;
}


// ============================================================================
// 动态状态枚举
// ============================================================================

std::vector<vk::DynamicState> VulkanPipelineState::GetEnabledDynamicStates() const
{
    std::vector<vk::DynamicState> states;

    // 输入装配
    if (topology.IsDynamic())
        states.push_back(vk::DynamicState::ePrimitiveTopology);

    // 视口
    if (viewportCount.IsDynamic())
        states.push_back(vk::DynamicState::eViewportWithCount);
    if (scissorCount.IsDynamic())
        states.push_back(vk::DynamicState::eScissorWithCount);

    // 光栅化
    if (rasterizerDiscardEnable.IsDynamic())
        states.push_back(vk::DynamicState::eRasterizerDiscardEnable);
    if (cullMode.IsDynamic())
        states.push_back(vk::DynamicState::eCullMode);
    if (frontFace.IsDynamic())
        states.push_back(vk::DynamicState::eFrontFace);
    if (depthBiasEnable.IsDynamic())
        states.push_back(vk::DynamicState::eDepthBiasEnable);

    // 深度/模板
    if (depthTestEnable.IsDynamic())
        states.push_back(vk::DynamicState::eDepthTestEnable);
    if (depthWriteEnable.IsDynamic())
        states.push_back(vk::DynamicState::eDepthWriteEnable);
    if (depthCompareOp.IsDynamic())
        states.push_back(vk::DynamicState::eDepthCompareOp);
    if (depthBoundsTestEnable.IsDynamic())
        states.push_back(vk::DynamicState::eDepthBoundsTestEnable);
    if (stencilTestEnable.IsDynamic())
        states.push_back(vk::DynamicState::eStencilTestEnable);
    if (stencilFront.IsAnyDynamic() || stencilBack.IsAnyDynamic())
        states.push_back(vk::DynamicState::eStencilOp);

    return states;
}


// ============================================================================
// 动态状态刷入
// ============================================================================

void VulkanPipelineState::FlushDynamicStates(vk::CommandBuffer cmd) const
{
    // ---- 输入装配 ----
    if (topology.IsDynamic())
        cmd.setPrimitiveTopology(topology.Get());

    // ---- 光栅化 ----
    if (rasterizerDiscardEnable.IsDynamic())
        cmd.setRasterizerDiscardEnable(rasterizerDiscardEnable.Get());
    if (cullMode.IsDynamic())
        cmd.setCullMode(cullMode.Get());
    if (frontFace.IsDynamic())
        cmd.setFrontFace(frontFace.Get());
    if (depthBiasEnable.IsDynamic())
        cmd.setDepthBiasEnable(depthBiasEnable.Get());

    // ---- 深度/模板 ----
    if (depthTestEnable.IsDynamic())
        cmd.setDepthTestEnable(depthTestEnable.Get());
    if (depthWriteEnable.IsDynamic())
        cmd.setDepthWriteEnable(depthWriteEnable.Get());
    if (depthCompareOp.IsDynamic())
        cmd.setDepthCompareOp(depthCompareOp.Get());
    if (depthBoundsTestEnable.IsDynamic())
        cmd.setDepthBoundsTestEnable(depthBoundsTestEnable.Get());
    if (stencilTestEnable.IsDynamic())
        cmd.setStencilTestEnable(stencilTestEnable.Get());

    if (stencilFront.IsAnyDynamic())
    {
        cmd.setStencilOp(vk::StencilFaceFlagBits::eFront,
                         stencilFront.failOp.Get(),
                         stencilFront.passOp.Get(),
                         stencilFront.depthFailOp.Get(),
                         stencilFront.compareOp.Get());
    }
    if (stencilBack.IsAnyDynamic())
    {
        cmd.setStencilOp(vk::StencilFaceFlagBits::eBack,
                         stencilBack.failOp.Get(),
                         stencilBack.passOp.Get(),
                         stencilBack.depthFailOp.Get(),
                         stencilBack.compareOp.Get());
    }
}


// ============================================================================
// 一键创建 Bundle
// ============================================================================

PipelineCreateBundle VulkanPipelineState::BuildCreateInfo(vk::PipelineCreateFlags flags) const
{
    PipelineCreateBundle bundle;

    // ---- 1. 着色器阶段 ----
    const auto &stages = shaderStages.Get();
    bundle.shaderStageCreateInfos.reserve(stages.size());
    for (const auto &s : stages)
    {
        bundle.shaderStageCreateInfos.push_back(
            vk::PipelineShaderStageCreateInfo{
                {},
                s.stage,
                s.module,
                s.entryPoint.c_str(),
                nullptr     // pSpecializationInfo
            });
    }

    // ---- 2. 动态状态 ----
    bundle.dynamicStates = GetEnabledDynamicStates();

    // ---- 3. 顶点输入 ----
    const auto &bindings   = vertexBindingDescriptions.Get();
    const auto &attributes = vertexAttributeDescriptions.Get();
    bundle.vertexBindings   = bindings;
    bundle.vertexAttributes = attributes;

    // ---- 4. 混合附件 ----
    bundle.blendAttachmentStates.reserve(m_BlendAttachments.size());
    for (const auto &a : m_BlendAttachments)
    {
        bundle.blendAttachmentStates.push_back(vk::PipelineColorBlendAttachmentState{
            a.blendEnable,
            a.srcColorBlendFactor,
            a.dstColorBlendFactor,
            a.colorBlendOp,
            a.srcAlphaBlendFactor,
            a.dstAlphaBlendFactor,
            a.alphaBlendOp,
            a.colorWriteMask
        });
    }

    // ---- 5. 颜色附件格式 ----
    bundle.colorAttachmentFormats = colorAttachmentFormats.Get();

    // ---- 6. 构建各 CreateInfo ----

    // VertexInput
    bundle.vertexInputInfo = vk::PipelineVertexInputStateCreateInfo{
        {},
        static_cast<uint32_t>(bundle.vertexBindings.size()),
        bundle.vertexBindings.data(),
        static_cast<uint32_t>(bundle.vertexAttributes.size()),
        bundle.vertexAttributes.data()
    };

    // InputAssembly
    bundle.inputAssemblyInfo = vk::PipelineInputAssemblyStateCreateInfo{
        {},
        topology.Get(),
        primitiveRestartEnable.Get()
    };

    // Tessellation
    bundle.tessellationInfo = vk::PipelineTessellationStateCreateInfo{
        {},
        patchControlPoints.Get()
    };

    // Viewport（如果动态，Vulkan 忽略 viewport/scissor 数据）
    bundle.viewportInfo = vk::PipelineViewportStateCreateInfo{
        {},
        viewportCount.Get(),
        nullptr,    // pViewports — 动态时忽略
        scissorCount.Get(),
        nullptr     // pScissors  — 动态时忽略
    };

    // Rasterization
    bundle.rasterizationInfo = vk::PipelineRasterizationStateCreateInfo{
        {},
        depthClampEnable.Get(),
        rasterizerDiscardEnable.Get(),
        polygonMode.Get(),
        cullMode.Get(),
        frontFace.Get(),
        depthBiasEnable.Get(),
        0.0f,   // depthBiasConstantFactor
        0.0f,   // depthBiasClamp
        0.0f,   // depthBiasSlopeFactor
        1.0f    // lineWidth
    };

    // Multisample
    bundle.sampleMaskData = sampleMask.Get();
    bundle.multisampleInfo = vk::PipelineMultisampleStateCreateInfo{
        {},
        rasterizationSamples.Get(),
        sampleShadingEnable.Get(),
        minSampleShading.Get(),
        &bundle.sampleMaskData,
        alphaToCoverageEnable.Get(),
        alphaToOneEnable.Get()
    };

    // DepthStencil
    bundle.depthStencilInfo = vk::PipelineDepthStencilStateCreateInfo{
        {},
        depthTestEnable.Get(),
        depthWriteEnable.Get(),
        depthCompareOp.Get(),
        depthBoundsTestEnable.Get(),
        stencilTestEnable.Get(),
        vk::StencilOpState{
            stencilFront.failOp.Get(),
            stencilFront.passOp.Get(),
            stencilFront.depthFailOp.Get(),
            stencilFront.compareOp.Get()
        },
        vk::StencilOpState{
            stencilBack.failOp.Get(),
            stencilBack.passOp.Get(),
            stencilBack.depthFailOp.Get(),
            stencilBack.compareOp.Get()
        },
        0.0f,   // minDepthBounds
        0.0f    // maxDepthBounds
    };

    // ColorBlend
    bundle.colorBlendInfo = vk::PipelineColorBlendStateCreateInfo{
        {},
        logicOpEnable.Get(),
        logicOp.Get(),
        static_cast<uint32_t>(bundle.blendAttachmentStates.size()),
        bundle.blendAttachmentStates.data(),
        {{0.0f, 0.0f, 0.0f, 0.0f}}   // blendConstants
    };

    // DynamicState
    bundle.dynamicStateInfo = vk::PipelineDynamicStateCreateInfo{
        {},
        static_cast<uint32_t>(bundle.dynamicStates.size()),
        bundle.dynamicStates.data()
    };

    // DynamicRendering
    bundle.renderingInfo = vk::PipelineRenderingCreateInfo{
        0,
        static_cast<uint32_t>(bundle.colorAttachmentFormats.size()),
        bundle.colorAttachmentFormats.data(),
        depthFormat.Get(),
        stencilFormat.Get()
    };

    // ---- 7. 主 CreateInfo ----
    bundle.pipelineInfo = vk::GraphicsPipelineCreateInfo{
        flags,
        static_cast<uint32_t>(bundle.shaderStageCreateInfos.size()),
        bundle.shaderStageCreateInfos.data(),
        &bundle.vertexInputInfo,
        &bundle.inputAssemblyInfo,
        &bundle.tessellationInfo,
        &bundle.viewportInfo,
        &bundle.rasterizationInfo,
        &bundle.multisampleInfo,
        &bundle.depthStencilInfo,
        &bundle.colorBlendInfo,
        &bundle.dynamicStateInfo,
        pipelineLayout.Get() ? pipelineLayout.Get()->GetHandle() : VK_NULL_HANDLE,
        VK_NULL_HANDLE,     // renderPass — Dynamic Rendering 不用
        0,                  // subpass
        VK_NULL_HANDLE,     // basePipelineHandle
        -1                  // basePipelineIndex
    };

    // pNext 链：renderingInfo → pipelineInfo
    bundle.pipelineInfo.setPNext(&bundle.renderingInfo);

    return bundle;
}


// ============================================================================
// 便利方法
// ============================================================================

void VulkanPipelineState::SetAllInputAssemblyDynamic(bool dyn)
{
    topology.SetDynamic(dyn);
    // primitiveRestartEnable 不可动态
}

void VulkanPipelineState::SetAllRasterizationDynamic(bool dyn)
{
    rasterizerDiscardEnable.SetDynamic(dyn);
    cullMode.SetDynamic(dyn);
    frontFace.SetDynamic(dyn);
    depthBiasEnable.SetDynamic(dyn);
    // depthClampEnable、polygonMode 不可动态
}

void VulkanPipelineState::SetAllDepthStencilDynamic(bool dyn)
{
    depthTestEnable.SetDynamic(dyn);
    depthWriteEnable.SetDynamic(dyn);
    depthCompareOp.SetDynamic(dyn);
    depthBoundsTestEnable.SetDynamic(dyn);
    stencilTestEnable.SetDynamic(dyn);
    stencilFront.SetAllDynamic(dyn);
    stencilBack.SetAllDynamic(dyn);
}

void VulkanPipelineState::SetAllViewportDynamic(bool dyn)
{
    viewportCount.SetDynamic(dyn);
    scissorCount.SetDynamic(dyn);
}

void VulkanPipelineState::SetAllDynamic(bool dyn)
{
    SetAllInputAssemblyDynamic(dyn);
    SetAllRasterizationDynamic(dyn);
    SetAllDepthStencilDynamic(dyn);
    SetAllViewportDynamic(dyn);
}

} // namespace GE
