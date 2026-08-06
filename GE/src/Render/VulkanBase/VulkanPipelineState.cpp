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
 * @brief VulkanPipelineState 实现——链式状态构建器 + 动态状态刷入。
 */

#include "Render/VulkanBase/VulkanPipelineState.h"
#include "Render/VulkanBase/VulkanPipelineLayout.h"
#include "Render/ShaderModule.h"

#include <algorithm>
#include <functional>

namespace GE {

// ============================================================================
// 动态状态管理
// ============================================================================

VulkanPipelineState &VulkanPipelineState::enableDynamicState(vk::DynamicState state) {
    m_DynamicStateSet.insert(state);
    m_DynamicStateCache.clear();
    return *this;
}

VulkanPipelineState &VulkanPipelineState::disableDynamicState(vk::DynamicState state) {
    m_DynamicStateSet.erase(state);
    m_DynamicStateCache.clear();
    return *this;
}

void VulkanPipelineState::clearDynamicStates() {
    m_DynamicStateSet.clear();
    m_DynamicStateCache.clear();
}

const std::vector<vk::DynamicState> &VulkanPipelineState::getDynamicStates() const {
    if (m_DynamicStateCache.empty() && !m_DynamicStateSet.empty()) {
        m_DynamicStateCache.assign(m_DynamicStateSet.begin(), m_DynamicStateSet.end());
    }
    return m_DynamicStateCache;
}

// ============================================================================
// 渲染附件格式
// ============================================================================

VulkanPipelineState &VulkanPipelineState::setRenderingFormats(std::vector<vk::Format> colorFmts,
                                                              vk::Format              depthFmt,
                                                              vk::Format              stencilFmt,
                                                              uint32_t                viewMask) {
    m_ColorFormats   = std::move(colorFmts);
    m_DepthFormat    = depthFmt;
    m_StencilFormat  = stencilFmt;
    m_ViewMask       = viewMask;
    return *this;
}

// ============================================================================
// 管线布局
// ============================================================================

VulkanPipelineState &VulkanPipelineState::setPipelineLayout(VulkanPipelineLayout *layout) {
    m_PipelineLayout = layout;
    return *this;
}

VulkanPipelineLayout *VulkanPipelineState::getPipelineLayout() const {
    return m_PipelineLayout;
}

// ============================================================================
// 顶点输入
// ============================================================================

VulkanPipelineState &VulkanPipelineState::setVertexInput(const vk::PipelineVertexInputStateCreateInfo &info) {
    m_VertexBindings.assign(info.pVertexBindingDescriptions,
                            info.pVertexBindingDescriptions + info.vertexBindingDescriptionCount);
    m_VertexAttributes.assign(info.pVertexAttributeDescriptions,
                              info.pVertexAttributeDescriptions + info.vertexAttributeDescriptionCount);
    return *this;
}

VulkanPipelineState &VulkanPipelineState::setVertexBindings(const std::vector<vk::VertexInputBindingDescription> &bindings) {
    m_VertexBindings = bindings;
    return *this;
}

VulkanPipelineState &VulkanPipelineState::setVertexAttributes(const std::vector<vk::VertexInputAttributeDescription> &attrs) {
    m_VertexAttributes = attrs;
    return *this;
}

uint32_t VulkanPipelineState::setVertexInputFromShader(const ShaderModule &vertShader,
                                                       uint32_t           binding,
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
        attrs.push_back(vk::VertexInputAttributeDescription{
            .location = res->location,
            .binding  = binding,
            .format   = res->format,
            .offset   = offset,
        });

        offset += GetVertexFormatSize(res->format) * res->array_size;
    }

    // 4. 设置到 pipeline state
    m_VertexBindings = std::vector<vk::VertexInputBindingDescription>{
        vk::VertexInputBindingDescription{
            .binding   = binding,
            .stride    = offset,
            .inputRate = rate,
        }
    };
    m_VertexAttributes = std::move(attrs);

    return offset; // stride
}

// ============================================================================
// 输入装配
// ============================================================================

VulkanPipelineState &VulkanPipelineState::setInputAssembly(vk::PrimitiveTopology topology, vk::Bool32 restart) {
    m_InputAssembly.topology = topology;
    m_InputAssembly.primitiveRestartEnable = restart;
    return *this;
}

// ============================================================================
// 细分曲面
// ============================================================================

VulkanPipelineState &VulkanPipelineState::setTessellationPatchControlPoints(uint32_t patchControlPoints) {
    m_Tessellation.patchControlPoints = patchControlPoints;
    return *this;
}

// ============================================================================
// 视口
// ============================================================================

VulkanPipelineState &VulkanPipelineState::setViewportState(const vk::PipelineViewportStateCreateInfo &info) {
    m_ViewportState = info;
    // pViewports / pScissors 指针不持有，重新设置为空（动态模式）
    m_ViewportState.pViewports = nullptr;
    m_ViewportState.pScissors  = nullptr;
    return *this;
}

VulkanPipelineState &VulkanPipelineState::setViewportCount(uint32_t viewportCount, uint32_t scissorCount) {
    m_ViewportState.viewportCount = viewportCount;
    m_ViewportState.scissorCount  = scissorCount;
    return *this;
}

// ============================================================================
// 光栅化
// ============================================================================

VulkanPipelineState &VulkanPipelineState::setRasterization(const vk::PipelineRasterizationStateCreateInfo &info) {
    m_Rasterization = info;
    return *this;
}

VulkanPipelineState &VulkanPipelineState::setDepthClampEnable(vk::Bool32 enable) {
    m_Rasterization.depthClampEnable = enable;
    return *this;
}

VulkanPipelineState &VulkanPipelineState::setRasterizerDiscardEnable(vk::Bool32 enable) {
    m_Rasterization.rasterizerDiscardEnable = enable;
    return *this;
}

VulkanPipelineState &VulkanPipelineState::setPolygonMode(vk::PolygonMode mode) {
    m_Rasterization.polygonMode = mode;
    return *this;
}

VulkanPipelineState &VulkanPipelineState::setCullMode(vk::CullModeFlags mode) {
    m_Rasterization.cullMode = mode;
    return *this;
}

VulkanPipelineState &VulkanPipelineState::setFrontFace(vk::FrontFace face) {
    m_Rasterization.frontFace = face;
    return *this;
}

VulkanPipelineState &VulkanPipelineState::setDepthBiasEnable(vk::Bool32 enable) {
    m_Rasterization.depthBiasEnable = enable;
    return *this;
}

VulkanPipelineState &VulkanPipelineState::setLineWidth(float lineWidth) {
    m_Rasterization.lineWidth = lineWidth;
    return *this;
}

// ============================================================================
// 多重采样
// ============================================================================

VulkanPipelineState &VulkanPipelineState::setMultisample(const vk::PipelineMultisampleStateCreateInfo &info) {
    m_Multisample = info;
    // pSampleMask 指针不持有，用内部 m_SampleMask 存储
    if (info.pSampleMask) {
        m_SampleMask = *info.pSampleMask;
    }
    m_Multisample.pSampleMask = nullptr; // 构建时再设置
    return *this;
}

VulkanPipelineState &VulkanPipelineState::setRasterizationSamples(vk::SampleCountFlagBits samples) {
    m_Multisample.rasterizationSamples = samples;
    return *this;
}

VulkanPipelineState &VulkanPipelineState::setSampleShadingEnable(vk::Bool32 enable) {
    m_Multisample.sampleShadingEnable = enable;
    return *this;
}

VulkanPipelineState &VulkanPipelineState::setMinSampleShading(float rate) {
    m_Multisample.minSampleShading = rate;
    return *this;
}

VulkanPipelineState &VulkanPipelineState::setSampleMask(vk::SampleMask mask) {
    m_SampleMask = mask;
    return *this;
}

VulkanPipelineState &VulkanPipelineState::setAlphaToCoverageEnable(vk::Bool32 enable) {
    m_Multisample.alphaToCoverageEnable = enable;
    return *this;
}

VulkanPipelineState &VulkanPipelineState::setAlphaToOneEnable(vk::Bool32 enable) {
    m_Multisample.alphaToOneEnable = enable;
    return *this;
}

// ============================================================================
// 深度/模板
// ============================================================================

VulkanPipelineState &VulkanPipelineState::setDepthStencil(const vk::PipelineDepthStencilStateCreateInfo &info) {
    m_DepthStencil = info;
    return *this;
}

VulkanPipelineState &VulkanPipelineState::setDepthTestEnable(vk::Bool32 enable) {
    m_DepthStencil.depthTestEnable = enable;
    return *this;
}

VulkanPipelineState &VulkanPipelineState::setDepthWriteEnable(vk::Bool32 enable) {
    m_DepthStencil.depthWriteEnable = enable;
    return *this;
}

VulkanPipelineState &VulkanPipelineState::setDepthCompareOp(vk::CompareOp op) {
    m_DepthStencil.depthCompareOp = op;
    return *this;
}

VulkanPipelineState &VulkanPipelineState::setDepthBoundsTestEnable(vk::Bool32 enable) {
    m_DepthStencil.depthBoundsTestEnable = enable;
    return *this;
}

VulkanPipelineState &VulkanPipelineState::setStencilTestEnable(vk::Bool32 enable) {
    m_DepthStencil.stencilTestEnable = enable;
    return *this;
}

VulkanPipelineState &VulkanPipelineState::setStencilFront(const vk::StencilOpState &state) {
    m_DepthStencil.front = state;
    return *this;
}

VulkanPipelineState &VulkanPipelineState::setStencilBack(const vk::StencilOpState &state) {
    m_DepthStencil.back = state;
    return *this;
}

// ============================================================================
// 颜色混合
// ============================================================================

VulkanPipelineState &VulkanPipelineState::addColorBlendAttachment(const vk::PipelineColorBlendAttachmentState &att) {
    m_BlendAttachments.push_back(att);
    return *this;
}

VulkanPipelineState &VulkanPipelineState::clearColorBlendAttachments() {
    m_BlendAttachments.clear();
    return *this;
}

VulkanPipelineState &VulkanPipelineState::setColorBlendAttachments(const std::vector<vk::PipelineColorBlendAttachmentState> &attachments) {
    m_BlendAttachments = attachments;
    return *this;
}

const std::vector<vk::PipelineColorBlendAttachmentState> &VulkanPipelineState::getColorBlendAttachments() const {
    return m_BlendAttachments;
}

VulkanPipelineState &VulkanPipelineState::setColorBlendConstants(float r, float g, float b, float a) {
    m_BlendConstants = {r, g, b, a};
    return *this;
}

VulkanPipelineState &VulkanPipelineState::setLogicOpEnable(vk::Bool32 enable) {
    m_ColorBlend.logicOpEnable = enable;
    return *this;
}

VulkanPipelineState &VulkanPipelineState::setLogicOp(vk::LogicOp op) {
    m_ColorBlend.logicOp = op;
    return *this;
}

// ============================================================================
// 内部辅助：填充 CreateBundle 的公共部分
// ============================================================================

void VulkanPipelineState::fillBundleCommon(CreateBundle &bundle, vk::PipelineCreateFlags flags) const {
    // ---- 1. 着色器阶段（从 pipelineLayout 提取） ----
    if (m_PipelineLayout) {
        const auto &modules = m_PipelineLayout->GetShaderModules();
        bundle.shaderStageCreateInfos.reserve(modules.size());
        for (const auto *mod : modules) {
            bundle.shaderStageCreateInfos.push_back(
                vk::PipelineShaderStageCreateInfo{
                    .stage  = mod->get_stage(),
                    .module = mod->GetHandle(),
                    .pName  = mod->get_entry_point().c_str(),
                });
        }
    }

    // ---- 2. 动态状态 ----
    bundle.dynamicStates = getDynamicStates();

    // ---- 3. 顶点输入 ----
    bundle.vertexBindings   = m_VertexBindings;
    bundle.vertexAttributes = m_VertexAttributes;

    // ---- 4. 混合附件 ----
    bundle.blendAttachmentStates = m_BlendAttachments;

    // ---- 5. 颜色附件格式 ----
    bundle.colorAttachmentFormats = m_ColorFormats;

    // ---- 6. 构建各 CreateInfo ----

    // VertexInput
    bundle.vertexInputInfo = vk::PipelineVertexInputStateCreateInfo{
        .vertexBindingDescriptionCount   = static_cast<uint32_t>(bundle.vertexBindings.size()),
        .pVertexBindingDescriptions      = bundle.vertexBindings.data(),
        .vertexAttributeDescriptionCount = static_cast<uint32_t>(bundle.vertexAttributes.size()),
        .pVertexAttributeDescriptions    = bundle.vertexAttributes.data(),
    };

    // InputAssembly
    bundle.inputAssemblyInfo = m_InputAssembly;

    // Tessellation
    bundle.tessellationInfo = m_Tessellation;

    // Viewport（动态模式下 pViewports/pScissors 为 nullptr，Vulkan 忽略）
    bundle.viewportInfo = m_ViewportState;

    // Rasterization
    bundle.rasterizationInfo = m_Rasterization;

    // Multisample
    bundle.sampleMaskData = m_SampleMask;
    bundle.multisampleInfo = m_Multisample;
    // pSampleMask：0 表示"禁用所有 sample"，nullptr 表示"启用所有 sample"
    bundle.multisampleInfo.pSampleMask = m_SampleMask ? &bundle.sampleMaskData : nullptr;

    // DepthStencil
    bundle.depthStencilInfo = m_DepthStencil;

    // ColorBlend
    bundle.colorBlendInfo = m_ColorBlend;
    bundle.colorBlendInfo.attachmentCount = static_cast<uint32_t>(bundle.blendAttachmentStates.size());
    bundle.colorBlendInfo.pAttachments    = bundle.blendAttachmentStates.data();
    bundle.colorBlendInfo.blendConstants  = m_BlendConstants;

    // DynamicState
    bundle.dynamicStateInfo = vk::PipelineDynamicStateCreateInfo{
        .dynamicStateCount = static_cast<uint32_t>(bundle.dynamicStates.size()),
        .pDynamicStates    = bundle.dynamicStates.data(),
    };

    // ---- 7. 主 CreateInfo ----
    bundle.pipelineInfo = vk::GraphicsPipelineCreateInfo{
        .flags              = flags,
        .stageCount         = static_cast<uint32_t>(bundle.shaderStageCreateInfos.size()),
        .pStages            = bundle.shaderStageCreateInfos.data(),
        .pVertexInputState  = &bundle.vertexInputInfo,
        .pInputAssemblyState = &bundle.inputAssemblyInfo,
        .pTessellationState = &bundle.tessellationInfo,
        .pViewportState     = &bundle.viewportInfo,
        .pRasterizationState = &bundle.rasterizationInfo,
        .pMultisampleState  = &bundle.multisampleInfo,
        .pDepthStencilState = &bundle.depthStencilInfo,
        .pColorBlendState   = &bundle.colorBlendInfo,
        .pDynamicState      = &bundle.dynamicStateInfo,
        .layout             = m_PipelineLayout ? m_PipelineLayout->GetHandle() : VK_NULL_HANDLE,
        .renderPass         = VK_NULL_HANDLE,
        .subpass            = 0,
        .basePipelineHandle = VK_NULL_HANDLE,
        .basePipelineIndex  = -1,
    };
}

// ============================================================================
// 管线构建：动态渲染模式
// ============================================================================

VulkanPipelineState::CreateBundle VulkanPipelineState::buildDynamicRenderingBundle(vk::PipelineCreateFlags flags) const {
    CreateBundle bundle;
    fillBundleCommon(bundle, flags);

    // DynamicRendering pNext 链
    bundle.renderingInfo = vk::PipelineRenderingCreateInfo{
        .viewMask                  = m_ViewMask,
        .colorAttachmentCount      = static_cast<uint32_t>(bundle.colorAttachmentFormats.size()),
        .pColorAttachmentFormats   = bundle.colorAttachmentFormats.data(),
        .depthAttachmentFormat     = m_DepthFormat,
        .stencilAttachmentFormat   = m_StencilFormat,
    };

    // pNext 挂载
    bundle.pipelineInfo.setPNext(&bundle.renderingInfo);
    bundle.pipelineInfo.flags |= vk::PipelineCreateFlagBits::eDynamicRenderingKHR;

    return bundle;
}

// ============================================================================
// 管线构建：传统 RenderPass 模式
// ============================================================================

VulkanPipelineState::CreateBundle VulkanPipelineState::buildRenderPassBundle(vk::RenderPass          renderPass,
                                                                             uint32_t                subpass,
                                                                             vk::PipelineCreateFlags flags) const {
    CreateBundle bundle;
    fillBundleCommon(bundle, flags);

    bundle.pipelineInfo.renderPass = renderPass;
    bundle.pipelineInfo.subpass    = subpass;
    bundle.pipelineInfo.pNext      = nullptr;

    return bundle;
}

// ============================================================================
// 动态状态刷入
// ============================================================================

void VulkanPipelineState::flushDynamicStates(vk::CommandBuffer cmd) const {
    // ---- 输入装配 ----
    if (m_DynamicStateSet.count(vk::DynamicState::ePrimitiveTopology))
        cmd.setPrimitiveTopology(m_InputAssembly.topology);

    // ---- 光栅化 ----
    if (m_DynamicStateSet.count(vk::DynamicState::eRasterizerDiscardEnable))
        cmd.setRasterizerDiscardEnable(m_Rasterization.rasterizerDiscardEnable);
    if (m_DynamicStateSet.count(vk::DynamicState::eCullMode))
        cmd.setCullMode(m_Rasterization.cullMode);
    if (m_DynamicStateSet.count(vk::DynamicState::eFrontFace))
        cmd.setFrontFace(m_Rasterization.frontFace);
    if (m_DynamicStateSet.count(vk::DynamicState::eDepthBiasEnable))
        cmd.setDepthBiasEnable(m_Rasterization.depthBiasEnable);

    // ---- 深度/模板 ----
    // 优先使用 VK_EXT_extended_dynamic_state 扩展（Vulkan 1.3 core）
    if (m_DynamicStateSet.count(vk::DynamicState::eDepthTestEnable))
        cmd.setDepthTestEnableEXT(m_DepthStencil.depthTestEnable);
    if (m_DynamicStateSet.count(vk::DynamicState::eDepthWriteEnable))
        cmd.setDepthWriteEnableEXT(m_DepthStencil.depthWriteEnable);
    if (m_DynamicStateSet.count(vk::DynamicState::eDepthCompareOp))
        cmd.setDepthCompareOpEXT(m_DepthStencil.depthCompareOp);
    if (m_DynamicStateSet.count(vk::DynamicState::eDepthBoundsTestEnable))
        cmd.setDepthBoundsTestEnable(m_DepthStencil.depthBoundsTestEnable);
    if (m_DynamicStateSet.count(vk::DynamicState::eStencilTestEnable))
        cmd.setStencilTestEnableEXT(m_DepthStencil.stencilTestEnable);
    if (m_DynamicStateSet.count(vk::DynamicState::eStencilOp)) {
        cmd.setStencilOp(vk::StencilFaceFlagBits::eFront,
                         m_DepthStencil.front.failOp,
                         m_DepthStencil.front.passOp,
                         m_DepthStencil.front.depthFailOp,
                         m_DepthStencil.front.compareOp);
        cmd.setStencilOp(vk::StencilFaceFlagBits::eBack,
                         m_DepthStencil.back.failOp,
                         m_DepthStencil.back.passOp,
                         m_DepthStencil.back.depthFailOp,
                         m_DepthStencil.back.compareOp);
    }
}

// ============================================================================
// 哈希
// ============================================================================

size_t VulkanPipelineState::hash() const {
    size_t seed = 0;
    auto hashCombine = [&seed](size_t v) {
        seed ^= v + 0x9e3779b9 + (seed << 6) + (seed >> 2);
    };

    // —— 颜色附件格式 ——
    for (auto fmt : m_ColorFormats)
        hashCombine(std::hash<vk::Format>{}(fmt));

    // —— 深度/模板格式 ——
    hashCombine(std::hash<vk::Format>{}(m_DepthFormat));
    hashCombine(std::hash<vk::Format>{}(m_StencilFormat));
    hashCombine(m_ViewMask);

    // —— 顶点输入 ——
    hashCombine(m_VertexBindings.size());
    for (const auto &b : m_VertexBindings) {
        hashCombine(b.binding);
        hashCombine(b.stride);
        hashCombine(static_cast<uint32_t>(b.inputRate));
    }
    hashCombine(m_VertexAttributes.size());
    for (const auto &a : m_VertexAttributes) {
        hashCombine(a.location);
        hashCombine(a.binding);
        hashCombine(std::hash<vk::Format>{}(a.format));
        hashCombine(a.offset);
    }

    // —— 输入装配 ——
    hashCombine(std::hash<vk::PrimitiveTopology>{}(m_InputAssembly.topology));
    hashCombine(m_InputAssembly.primitiveRestartEnable);

    // —— 细分曲面 ——
    hashCombine(m_Tessellation.patchControlPoints);

    // —— 视口计数 ——
    hashCombine(m_ViewportState.viewportCount);
    hashCombine(m_ViewportState.scissorCount);

    // —— 光栅化 ——
    hashCombine(m_Rasterization.depthClampEnable);
    hashCombine(m_Rasterization.rasterizerDiscardEnable);
    hashCombine(static_cast<uint32_t>(m_Rasterization.polygonMode));
    hashCombine(static_cast<uint32_t>(m_Rasterization.cullMode));
    hashCombine(static_cast<uint32_t>(m_Rasterization.frontFace));
    hashCombine(m_Rasterization.depthBiasEnable);
    hashCombine(std::hash<float>{}(m_Rasterization.lineWidth));

    // —— 多重采样 ——
    hashCombine(static_cast<uint32_t>(m_Multisample.rasterizationSamples));
    hashCombine(m_Multisample.sampleShadingEnable);
    hashCombine(std::hash<float>{}(m_Multisample.minSampleShading));
    hashCombine(m_SampleMask);
    hashCombine(m_Multisample.alphaToCoverageEnable);
    hashCombine(m_Multisample.alphaToOneEnable);

    // —— 深度/模板 ——
    hashCombine(m_DepthStencil.depthTestEnable);
    hashCombine(m_DepthStencil.depthWriteEnable);
    hashCombine(static_cast<uint32_t>(m_DepthStencil.depthCompareOp));
    hashCombine(m_DepthStencil.depthBoundsTestEnable);
    hashCombine(m_DepthStencil.stencilTestEnable);
    // 正面模板
    hashCombine(static_cast<uint32_t>(m_DepthStencil.front.failOp));
    hashCombine(static_cast<uint32_t>(m_DepthStencil.front.passOp));
    hashCombine(static_cast<uint32_t>(m_DepthStencil.front.depthFailOp));
    hashCombine(static_cast<uint32_t>(m_DepthStencil.front.compareOp));
    // 背面模板
    hashCombine(static_cast<uint32_t>(m_DepthStencil.back.failOp));
    hashCombine(static_cast<uint32_t>(m_DepthStencil.back.passOp));
    hashCombine(static_cast<uint32_t>(m_DepthStencil.back.depthFailOp));
    hashCombine(static_cast<uint32_t>(m_DepthStencil.back.compareOp));

    // —— 颜色混合 ——
    hashCombine(m_ColorBlend.logicOpEnable);
    hashCombine(static_cast<uint32_t>(m_ColorBlend.logicOp));
    for (const auto &att : m_BlendAttachments) {
        hashCombine(att.blendEnable);
        hashCombine(static_cast<uint32_t>(att.srcColorBlendFactor));
        hashCombine(static_cast<uint32_t>(att.dstColorBlendFactor));
        hashCombine(static_cast<uint32_t>(att.colorBlendOp));
        hashCombine(static_cast<uint32_t>(att.srcAlphaBlendFactor));
        hashCombine(static_cast<uint32_t>(att.dstAlphaBlendFactor));
        hashCombine(static_cast<uint32_t>(att.alphaBlendOp));
        hashCombine(static_cast<uint32_t>(att.colorWriteMask));
    }
    for (float c : m_BlendConstants)
        hashCombine(std::hash<float>{}(c));

    // —— 动态状态集合 ——
    for (auto s : m_DynamicStateSet)
        hashCombine(static_cast<uint32_t>(s));

    // —— PipelineLayout ——
    if (m_PipelineLayout) {
        hashCombine(std::hash<vk::PipelineLayout>{}(m_PipelineLayout->GetHandle()));
    }

    return seed;
}

} // namespace GE
