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
#include "Render/VulkanBase/ShaderModule.h"

#include <algorithm>
#include <cstring>

namespace GE {

// ============================================================================
// 混合附件操作方法
// ============================================================================

void VulkanPipelineState::SetBlendAttachments(const std::vector<vk::PipelineColorBlendAttachmentState> &attachments) {
    m_BlendAttachments = attachments;
    m_BlendAttachmentsDirty = true;
}

void VulkanPipelineState::SetBlendAttachment(uint32_t index, const vk::PipelineColorBlendAttachmentState &attachment) {
    if (index >= m_BlendAttachments.size()) {
        m_BlendAttachments.resize(index + 1);
    }
    m_BlendAttachments[index] = attachment;
    m_BlendAttachmentsDirty = true;
}


// ============================================================================
// 顶点输入便利方法
// ============================================================================

uint32_t VulkanPipelineState::SetVertexInputFromShader(const ShaderModule &vertShader,
                                                       uint32_t binding,
                                                       vk::VertexInputRate rate) {
    // 1. 收集所有 Input 资源
    std::vector<const ShaderResource *> inputs;
    for (const auto &res : vertShader.get_resources()) {
        if (res.type == ShaderResourceType::Input) {
            inputs.push_back(&res);
        }
    }

    // 2. 按 location 升序排列
    std::sort(inputs.begin(), inputs.end(),
              [](const ShaderResource *a, const ShaderResource *b) {
                  return a->location < b->location;
              });

    // 3. 生成 attribute 描述，紧密打包 offset
    std::vector<vk::VertexInputAttributeDescription> attrs;
    uint32_t offset = 0;
    for (const auto *res : inputs) {
        if (res->format == vk::Format::eUndefined) {
            // 无法推导格式的属性跳过（矩阵等）
            continue;
        }
        attrs.push_back({
            res->location,
            binding,
            res->format,
            offset
        });

        offset += GetVertexFormatSize(res->format) * res->array_size;
    }

    // 4. 设置到 pipeline state
    vertexBindingDescriptions = std::vector<vk::VertexInputBindingDescription>{
        {binding, offset, rate}
    };
    vertexAttributeDescriptions = std::move(attrs);

    return offset; // stride
}

// ============================================================================
// 脏标记查询
// ============================================================================

bool VulkanPipelineState::HasDynamicDirty() const {
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
           || (stencilBack.IsAnyDynamic() && stencilBack.IsAnyValueDirty());
}

bool VulkanPipelineState::HasPipelineDirty() const {
    // ---- StaticParam 值脏 ----
    if (colorAttachmentFormats.IsValueDirty()
        || depthFormat.IsValueDirty()
        || stencilFormat.IsValueDirty()
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
        || m_BlendAttachmentsDirty) {
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
        || checkStaticValue(stencilTestEnable)) {
        return true;
    }

    // StencilOpState：当子字段为静态且值变更时
    if ((!stencilFront.IsAnyDynamic() && stencilFront.IsAnyValueDirty())
        || (!stencilBack.IsAnyDynamic() && stencilBack.IsAnyValueDirty())) {
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
        || checkConfig(stencilTestEnable)) {
        return true;
    }

    if (stencilFront.IsAnyConfigDirty() || stencilBack.IsAnyConfigDirty()) {
        return true;
    }

    return false;
}

void VulkanPipelineState::ClearAllDirty() {
    // StaticParam
    colorAttachmentFormats.ClearDirty();
    depthFormat.ClearDirty();
    stencilFormat.ClearDirty();
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

std::vector<vk::DynamicState> VulkanPipelineState::GetEnabledDynamicStates() const {
    std::vector<vk::DynamicState> states;

    // 视口/剪刀矩形总是动态（现代 Vulkan 应用无理由在创建时固定）
    states.push_back(vk::DynamicState::eViewport);
    states.push_back(vk::DynamicState::eScissor);

    // 输入装配
    if (topology.IsDynamic())
        states.push_back(vk::DynamicState::ePrimitiveTopology);

    // 视口计数（仅当用户显式设为动态时启用 WithCount 变体）
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

void VulkanPipelineState::FlushDynamicStates(vk::CommandBuffer cmd) const {
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

    if (stencilFront.IsAnyDynamic()) {
        cmd.setStencilOp(vk::StencilFaceFlagBits::eFront,
                         stencilFront.failOp.Get(),
                         stencilFront.passOp.Get(),
                         stencilFront.depthFailOp.Get(),
                         stencilFront.compareOp.Get());
    }
    if (stencilBack.IsAnyDynamic()) {
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

PipelineCreateBundle VulkanPipelineState::BuildCreateInfo(vk::PipelineCreateFlags flags) const {
    PipelineCreateBundle bundle;

    // ---- 1. 着色器阶段（从 pipelineLayout 提取） ----
    const auto *layout = pipelineLayout.Get();
    if (layout) {
        const auto &modules = layout->GetShaderModules();
        bundle.shaderStageCreateInfos.reserve(modules.size());
        for (const auto *mod : modules) {
            bundle.shaderStageCreateInfos.push_back(
                vk::PipelineShaderStageCreateInfo{
                    .stage = mod->get_stage(),
                    .module = mod->GetHandle(),
                    .pName = mod->get_entry_point().c_str(),
                });
        }
    }

    // ---- 2. 动态状态 ----
    bundle.dynamicStates = GetEnabledDynamicStates();

    // ---- 3. 顶点输入 ----
    const auto &bindings = vertexBindingDescriptions.Get();
    const auto &attributes = vertexAttributeDescriptions.Get();
    bundle.vertexBindings = bindings;
    bundle.vertexAttributes = attributes;

    // ---- 4. 混合附件 ----
    bundle.blendAttachmentStates = m_BlendAttachments;

    // ---- 5. 颜色附件格式 ----
    bundle.colorAttachmentFormats = colorAttachmentFormats.Get();

    // ---- 6. 构建各 CreateInfo ----
    // 注意：项目禁用 vulkan.hpp 构造函数，使用 aggregate init，
    // 必须用 designated initializer 显式指定字段。

    // VertexInput
    bundle.vertexInputInfo = vk::PipelineVertexInputStateCreateInfo{
        .vertexBindingDescriptionCount = static_cast<uint32_t>(bundle.vertexBindings.size()),
        .pVertexBindingDescriptions = bundle.vertexBindings.data(),
        .vertexAttributeDescriptionCount = static_cast<uint32_t>(bundle.vertexAttributes.size()),
        .pVertexAttributeDescriptions = bundle.vertexAttributes.data(),
    };

    // InputAssembly
    bundle.inputAssemblyInfo = vk::PipelineInputAssemblyStateCreateInfo{
        .topology = topology.Get(),
        .primitiveRestartEnable = primitiveRestartEnable.Get(),
    };

    // Tessellation
    bundle.tessellationInfo = vk::PipelineTessellationStateCreateInfo{
        .patchControlPoints = patchControlPoints.Get(),
    };

    // Viewport（如果动态，Vulkan 忽略 viewport/scissor 数据）
    bundle.viewportInfo = vk::PipelineViewportStateCreateInfo{
        .viewportCount = viewportCount.Get(),
        .pViewports = nullptr, // 动态时忽略
        .scissorCount = scissorCount.Get(),
        .pScissors = nullptr, // 动态时忽略
    };

    // Rasterization
    bundle.rasterizationInfo = vk::PipelineRasterizationStateCreateInfo{
        .depthClampEnable = depthClampEnable.Get(),
        .rasterizerDiscardEnable = rasterizerDiscardEnable.Get(),
        .polygonMode = polygonMode.Get(),
        .cullMode = cullMode.Get(),
        .frontFace = frontFace.Get(),
        .depthBiasEnable = depthBiasEnable.Get(),
        .depthBiasConstantFactor = 0.0f,
        .depthBiasClamp = 0.0f,
        .depthBiasSlopeFactor = 0.0f,
        .lineWidth = 1.0f,
    };

    // Multisample
    // pSampleMask=nullptr 表示"启用所有 sample"，
    // 指向 0 则"禁用所有 sample"（导致什么都画不出来）。
    auto sampleMaskVal = sampleMask.Get();
    bundle.sampleMaskData = sampleMaskVal;
    bundle.multisampleInfo = vk::PipelineMultisampleStateCreateInfo{
        .rasterizationSamples = rasterizationSamples.Get(),
        .sampleShadingEnable = sampleShadingEnable.Get(),
        .minSampleShading = minSampleShading.Get(),
        .pSampleMask = sampleMaskVal ? &bundle.sampleMaskData : nullptr,
        .alphaToCoverageEnable = alphaToCoverageEnable.Get(),
        .alphaToOneEnable = alphaToOneEnable.Get(),
    };

    // DepthStencil
    bundle.depthStencilInfo = vk::PipelineDepthStencilStateCreateInfo{
        .depthTestEnable = depthTestEnable.Get(),
        .depthWriteEnable = depthWriteEnable.Get(),
        .depthCompareOp = depthCompareOp.Get(),
        .depthBoundsTestEnable = depthBoundsTestEnable.Get(),
        .stencilTestEnable = stencilTestEnable.Get(),
        .front = vk::StencilOpState{
            stencilFront.failOp.Get(),
            stencilFront.passOp.Get(),
            stencilFront.depthFailOp.Get(),
            stencilFront.compareOp.Get(),
        },
        .back = vk::StencilOpState{
            stencilBack.failOp.Get(),
            stencilBack.passOp.Get(),
            stencilBack.depthFailOp.Get(),
            stencilBack.compareOp.Get(),
        },
        .minDepthBounds = 0.0f,
        .maxDepthBounds = 0.0f,
    };

    // ColorBlend
    bundle.colorBlendInfo = vk::PipelineColorBlendStateCreateInfo{
        .logicOpEnable = logicOpEnable.Get(),
        .logicOp = logicOp.Get(),
        .attachmentCount = static_cast<uint32_t>(bundle.blendAttachmentStates.size()),
        .pAttachments = bundle.blendAttachmentStates.data(),
        .blendConstants = {{0.0f, 0.0f, 0.0f, 0.0f}},
    };

    // DynamicState
    bundle.dynamicStateInfo = vk::PipelineDynamicStateCreateInfo{
        .dynamicStateCount = static_cast<uint32_t>(bundle.dynamicStates.size()),
        .pDynamicStates = bundle.dynamicStates.data(),
    };

    // DynamicRendering
    bundle.renderingInfo = vk::PipelineRenderingCreateInfo{
        .viewMask = 0,
        .colorAttachmentCount = static_cast<uint32_t>(bundle.colorAttachmentFormats.size()),
        .pColorAttachmentFormats = bundle.colorAttachmentFormats.data(),
        .depthAttachmentFormat = depthFormat.Get(),
        .stencilAttachmentFormat = stencilFormat.Get(),
    };

    // ---- 7. 主 CreateInfo ----
    bundle.pipelineInfo = vk::GraphicsPipelineCreateInfo{
        .flags = flags,
        .stageCount = static_cast<uint32_t>(bundle.shaderStageCreateInfos.size()),
        .pStages = bundle.shaderStageCreateInfos.data(),
        .pVertexInputState = &bundle.vertexInputInfo,
        .pInputAssemblyState = &bundle.inputAssemblyInfo,
        .pTessellationState = &bundle.tessellationInfo,
        .pViewportState = &bundle.viewportInfo,
        .pRasterizationState = &bundle.rasterizationInfo,
        .pMultisampleState = &bundle.multisampleInfo,
        .pDepthStencilState = &bundle.depthStencilInfo,
        .pColorBlendState = &bundle.colorBlendInfo,
        .pDynamicState = &bundle.dynamicStateInfo,
        .layout = pipelineLayout.Get() ? pipelineLayout.Get()->GetHandle() : VK_NULL_HANDLE,
        .renderPass = VK_NULL_HANDLE, // Dynamic Rendering 不用
        .subpass = 0,
        .basePipelineHandle = VK_NULL_HANDLE,
        .basePipelineIndex = -1,
    };

    // pNext 链：renderingInfo → pipelineInfo
    bundle.pipelineInfo.setPNext(&bundle.renderingInfo);

    return bundle;
}


// ============================================================================
// 便利方法
// ============================================================================

void VulkanPipelineState::SetAllInputAssemblyDynamic(bool dyn) {
    topology.SetDynamic(dyn);
    // primitiveRestartEnable 不可动态
}

void VulkanPipelineState::SetAllRasterizationDynamic(bool dyn) {
    rasterizerDiscardEnable.SetDynamic(dyn);
    cullMode.SetDynamic(dyn);
    frontFace.SetDynamic(dyn);
    depthBiasEnable.SetDynamic(dyn);
    // depthClampEnable、polygonMode 不可动态
}

void VulkanPipelineState::SetAllDepthStencilDynamic(bool dyn) {
    depthTestEnable.SetDynamic(dyn);
    depthWriteEnable.SetDynamic(dyn);
    depthCompareOp.SetDynamic(dyn);
    depthBoundsTestEnable.SetDynamic(dyn);
    stencilTestEnable.SetDynamic(dyn);
    stencilFront.SetAllDynamic(dyn);
    stencilBack.SetAllDynamic(dyn);
}

void VulkanPipelineState::SetAllViewportDynamic(bool dyn) {
    viewportCount.SetDynamic(dyn);
    scissorCount.SetDynamic(dyn);
}

void VulkanPipelineState::SetAllDynamic(bool dyn) {
    SetAllInputAssemblyDynamic(dyn);
    SetAllRasterizationDynamic(dyn);
    SetAllDepthStencilDynamic(dyn);
    SetAllViewportDynamic(dyn);
}

} // namespace GE
