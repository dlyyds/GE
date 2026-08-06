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

#include <vulkan/vulkan_hash.hpp>

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
                                                              vk::Format depthFmt,
                                                              vk::Format stencilFmt,
                                                              uint32_t viewMask) {
    m_ColorFormats = std::move(colorFmts);
    m_DepthFormat = depthFmt;
    m_StencilFormat = stencilFmt;
    m_ViewMask = viewMask;
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
        attrs.push_back(vk::VertexInputAttributeDescription{
            .location = res->location,
            .binding = binding,
            .format = res->format,
            .offset = offset,
        });

        offset += GetVertexFormatSize(res->format) * res->array_size;
    }

    // 4. 设置到 pipeline state
    m_VertexBindings = std::vector<vk::VertexInputBindingDescription>{
        vk::VertexInputBindingDescription{
            .binding = binding,
            .stride = offset,
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
    m_ViewportState.pScissors = nullptr;
    return *this;
}

VulkanPipelineState &VulkanPipelineState::setViewportCount(uint32_t viewportCount, uint32_t scissorCount) {
    m_ViewportState.viewportCount = viewportCount;
    m_ViewportState.scissorCount = scissorCount;
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
    // 重新指向内部存储
    m_Multisample.pSampleMask = m_SampleMask ? &m_SampleMask : nullptr;
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
    // 同步更新 multisample CreateInfo 中的指针（0 表示禁用所有 sample，nullptr 表示启用所有 sample）
    m_Multisample.pSampleMask = m_SampleMask ? &m_SampleMask : nullptr;
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
// 内部辅助：组装内部各 CreateInfo 的公共部分
// ============================================================================

void VulkanPipelineState::buildCommon(vk::PipelineCreateFlags flags) const {
    // ---- 1. 着色器阶段（从 pipelineLayout 提取，缓存到 m_ShaderStageCache） ----
    m_ShaderStageCache.clear();
    if (m_PipelineLayout) {
        const auto &modules = m_PipelineLayout->GetShaderModules();
        m_ShaderStageCache.reserve(modules.size());
        for (const auto *mod : modules) {
            m_ShaderStageCache.push_back(
                vk::PipelineShaderStageCreateInfo{
                    .stage = mod->get_stage(),
                    .module = mod->GetHandle(),
                    .pName = mod->get_entry_point().c_str(),
                });
        }
    }

    // ---- 2. 动态状态（确保 m_DynamicStateCache 已刷新） ----
    const auto &dynStates = getDynamicStates();
    m_DynamicStateInfo = vk::PipelineDynamicStateCreateInfo{
        .dynamicStateCount = static_cast<uint32_t>(dynStates.size()),
        .pDynamicStates = dynStates.data(),
    };

    // ---- 3. 顶点输入（指针指向 m_VertexBindings / m_VertexAttributes） ----
    m_VertexInputInfo = vk::PipelineVertexInputStateCreateInfo{
        .vertexBindingDescriptionCount = static_cast<uint32_t>(m_VertexBindings.size()),
        .pVertexBindingDescriptions = m_VertexBindings.data(),
        .vertexAttributeDescriptionCount = static_cast<uint32_t>(m_VertexAttributes.size()),
        .pVertexAttributeDescriptions = m_VertexAttributes.data(),
    };

    // ---- 4. 颜色混合（指针指向 m_BlendAttachments / m_BlendConstants） ----
    m_ColorBlendInfo = m_ColorBlend;
    m_ColorBlendInfo.attachmentCount = static_cast<uint32_t>(m_BlendAttachments.size());
    m_ColorBlendInfo.pAttachments = m_BlendAttachments.data();
    m_ColorBlendInfo.blendConstants = m_BlendConstants;

    // ---- 5. 多重采样的 pSampleMask 已在 setSampleMask / setMultisample 时同步 ----

    // ---- 6. 主 CreateInfo ----
    m_PipelineInfo = vk::GraphicsPipelineCreateInfo{
        .flags = flags,
        .stageCount = static_cast<uint32_t>(m_ShaderStageCache.size()),
        .pStages = m_ShaderStageCache.data(),
        .pVertexInputState = &m_VertexInputInfo,
        .pInputAssemblyState = &m_InputAssembly,
        .pTessellationState = &m_Tessellation,
        .pViewportState = &m_ViewportState,
        .pRasterizationState = &m_Rasterization,
        .pMultisampleState = &m_Multisample,
        .pDepthStencilState = &m_DepthStencil,
        .pColorBlendState = &m_ColorBlendInfo,
        .pDynamicState = &m_DynamicStateInfo,
        .layout = m_PipelineLayout ? m_PipelineLayout->GetHandle() : VK_NULL_HANDLE,
        .renderPass = VK_NULL_HANDLE,
        .subpass = 0,
        .basePipelineHandle = VK_NULL_HANDLE,
        .basePipelineIndex = -1,
    };
}

// ============================================================================
// 管线构建：动态渲染模式
// ============================================================================

const vk::GraphicsPipelineCreateInfo &VulkanPipelineState::buildDynamicRenderingPipeline(
    vk::PipelineCreateFlags flags) const {
    buildCommon(flags);

    // DynamicRendering pNext 链（颜色格式指针指向 m_ColorFormats）
    m_RenderingInfo = vk::PipelineRenderingCreateInfo{
        .viewMask = m_ViewMask,
        .colorAttachmentCount = static_cast<uint32_t>(m_ColorFormats.size()),
        .pColorAttachmentFormats = m_ColorFormats.data(),
        .depthAttachmentFormat = m_DepthFormat,
        .stencilAttachmentFormat = m_StencilFormat,
    };

    // pNext 挂载（Vulkan 1.3 core，pNext 链中有 rendering info 即可）
    m_PipelineInfo.setPNext(&m_RenderingInfo);

    return m_PipelineInfo;
}

// ============================================================================
// 管线构建：传统 RenderPass 模式
// ============================================================================

const vk::GraphicsPipelineCreateInfo &VulkanPipelineState::buildRenderPassPipeline(
    vk::RenderPass renderPass,
    uint32_t subpass,
    vk::PipelineCreateFlags flags) const {
    buildCommon(flags);

    m_PipelineInfo.renderPass = renderPass;
    m_PipelineInfo.subpass = subpass;
    m_PipelineInfo.pNext = nullptr;

    return m_PipelineInfo;
}

// ============================================================================
// 动态状态刷入
// ============================================================================

void VulkanPipelineState::flushDynamicStates(vk::CommandBuffer cmd) const {
    // ---- 输入装配 ----
    if (m_DynamicStateSet.contains(vk::DynamicState::ePrimitiveTopology))
        cmd.setPrimitiveTopology(m_InputAssembly.topology);

    // ---- 光栅化 ----
    if (m_DynamicStateSet.contains(vk::DynamicState::eRasterizerDiscardEnable))
        cmd.setRasterizerDiscardEnable(m_Rasterization.rasterizerDiscardEnable);
    if (m_DynamicStateSet.contains(vk::DynamicState::eCullMode))
        cmd.setCullMode(m_Rasterization.cullMode);
    if (m_DynamicStateSet.contains(vk::DynamicState::eFrontFace))
        cmd.setFrontFace(m_Rasterization.frontFace);
    if (m_DynamicStateSet.contains(vk::DynamicState::eDepthBiasEnable))
        cmd.setDepthBiasEnable(m_Rasterization.depthBiasEnable);

    // ---- 深度/模板 ----
    // 优先使用 VK_EXT_extended_dynamic_state 扩展（Vulkan 1.3 core）
    if (m_DynamicStateSet.contains(vk::DynamicState::eDepthTestEnable))
        cmd.setDepthTestEnableEXT(m_DepthStencil.depthTestEnable);
    if (m_DynamicStateSet.contains(vk::DynamicState::eDepthWriteEnable))
        cmd.setDepthWriteEnableEXT(m_DepthStencil.depthWriteEnable);
    if (m_DynamicStateSet.contains(vk::DynamicState::eDepthCompareOp))
        cmd.setDepthCompareOpEXT(m_DepthStencil.depthCompareOp);
    if (m_DynamicStateSet.contains(vk::DynamicState::eDepthBoundsTestEnable))
        cmd.setDepthBoundsTestEnable(m_DepthStencil.depthBoundsTestEnable);
    if (m_DynamicStateSet.contains(vk::DynamicState::eStencilTestEnable))
        cmd.setStencilTestEnableEXT(m_DepthStencil.stencilTestEnable);
    if (m_DynamicStateSet.contains(vk::DynamicState::eStencilOp)) {
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

    // 辅助：仅当指定动态状态未启用时才 hash 对应值
    // —— 动态状态的值在管线创建时被忽略，不应参与管线 hash
    // —— 只处理 flushDynamicStates() 中实际支持的动态状态
    auto hashIfNotDynamic = [&](vk::DynamicState s, size_t v) {
        if (!m_DynamicStateSet.count(s))
            hashCombine(v);
    };

    // —— 颜色附件格式 ——
    for (auto fmt : m_ColorFormats)
        hashCombine(std::hash<vk::Format>{}(fmt));

    // —— 深度/模板格式 ——
    hashCombine(std::hash<vk::Format>{}(m_DepthFormat));
    hashCombine(std::hash<vk::Format>{}(m_StencilFormat));
    hashCombine(m_ViewMask);

    // —— 顶点输入（数组内容需要手动遍历 hash，CreateInfo 的指针只 hash 地址）——
    hashCombine(m_VertexBindings.size());
    for (const auto &b : m_VertexBindings) {
        hashCombine(b.binding);
        hashCombine(b.stride);
        hashCombine(std::hash<vk::VertexInputRate>{}(b.inputRate));
    }
    hashCombine(m_VertexAttributes.size());
    for (const auto &a : m_VertexAttributes) {
        hashCombine(a.location);
        hashCombine(a.binding);
        hashCombine(std::hash<vk::Format>{}(a.format));
        hashCombine(a.offset);
    }

    // —— 输入装配 ——
    hashCombine(m_InputAssembly.primitiveRestartEnable);
    hashIfNotDynamic(vk::DynamicState::ePrimitiveTopology,
                     std::hash<vk::PrimitiveTopology>{}(m_InputAssembly.topology));

    // —— 细分曲面（整体 hash）——
    hashCombine(std::hash<vk::PipelineTessellationStateCreateInfo>{}(m_Tessellation));

    // —— 视口（count 参与管线创建，视口矩形本身是动态的）——
    hashCombine(m_ViewportState.viewportCount);
    hashCombine(m_ViewportState.scissorCount);

    // —— 光栅化（逐字段 hash，动态状态对应字段跳过）——
    hashCombine(m_Rasterization.depthClampEnable);
    hashIfNotDynamic(vk::DynamicState::eRasterizerDiscardEnable,
                     m_Rasterization.rasterizerDiscardEnable);
    hashCombine(std::hash<vk::PolygonMode>{}(m_Rasterization.polygonMode));
    hashIfNotDynamic(vk::DynamicState::eCullMode,
                     std::hash<vk::CullModeFlags>{}(m_Rasterization.cullMode));
    hashIfNotDynamic(vk::DynamicState::eFrontFace,
                     std::hash<vk::FrontFace>{}(m_Rasterization.frontFace));
    hashIfNotDynamic(vk::DynamicState::eDepthBiasEnable,
                     m_Rasterization.depthBiasEnable);
    // 注：depthBias 三个因子 / lineWidth / depthBiasClamp 等当前 flush 未支持，
    //     暂视为始终参与 hash，后续扩展动态状态时再补充
    hashCombine(std::hash<float>{}(m_Rasterization.depthBiasConstantFactor));
    hashCombine(std::hash<float>{}(m_Rasterization.depthBiasClamp));
    hashCombine(std::hash<float>{}(m_Rasterization.depthBiasSlopeFactor));
    hashCombine(std::hash<float>{}(m_Rasterization.lineWidth));

    // —— 多重采样（当前无对应动态状态，全部参与 hash）——
    hashCombine(std::hash<vk::SampleCountFlagBits>{}(m_Multisample.rasterizationSamples));
    hashCombine(m_Multisample.sampleShadingEnable);
    hashCombine(std::hash<float>{}(m_Multisample.minSampleShading));
    hashCombine(m_SampleMask);
    hashCombine(m_Multisample.alphaToCoverageEnable);
    hashCombine(m_Multisample.alphaToOneEnable);

    // —— 深度/模板（逐字段 hash，动态状态对应字段跳过）——
    hashIfNotDynamic(vk::DynamicState::eDepthTestEnable,
                     m_DepthStencil.depthTestEnable);
    hashIfNotDynamic(vk::DynamicState::eDepthWriteEnable,
                     m_DepthStencil.depthWriteEnable);
    hashIfNotDynamic(vk::DynamicState::eDepthCompareOp,
                     std::hash<vk::CompareOp>{}(m_DepthStencil.depthCompareOp));
    hashIfNotDynamic(vk::DynamicState::eDepthBoundsTestEnable,
                     m_DepthStencil.depthBoundsTestEnable);
    hashIfNotDynamic(vk::DynamicState::eStencilTestEnable,
                     m_DepthStencil.stencilTestEnable);
    // 模板操作（eStencilOp 对应 front+back 的 failOp/passOp/depthFailOp/compareOp）
    if (!m_DynamicStateSet.count(vk::DynamicState::eStencilOp)) {
        // 正面
        hashCombine(std::hash<vk::StencilOp>{}(m_DepthStencil.front.failOp));
        hashCombine(std::hash<vk::StencilOp>{}(m_DepthStencil.front.passOp));
        hashCombine(std::hash<vk::StencilOp>{}(m_DepthStencil.front.depthFailOp));
        hashCombine(std::hash<vk::CompareOp>{}(m_DepthStencil.front.compareOp));
        hashCombine(m_DepthStencil.front.compareMask);
        hashCombine(m_DepthStencil.front.writeMask);
        hashCombine(m_DepthStencil.front.reference);
        // 背面
        hashCombine(std::hash<vk::StencilOp>{}(m_DepthStencil.back.failOp));
        hashCombine(std::hash<vk::StencilOp>{}(m_DepthStencil.back.passOp));
        hashCombine(std::hash<vk::StencilOp>{}(m_DepthStencil.back.depthFailOp));
        hashCombine(std::hash<vk::CompareOp>{}(m_DepthStencil.back.compareOp));
        hashCombine(m_DepthStencil.back.compareMask);
        hashCombine(m_DepthStencil.back.writeMask);
        hashCombine(m_DepthStencil.back.reference);
    }
    // min/maxDepthBounds 暂未实现动态状态，始终参与 hash
    hashCombine(std::hash<float>{}(m_DepthStencil.minDepthBounds));
    hashCombine(std::hash<float>{}(m_DepthStencil.maxDepthBounds));

    // —— 颜色混合（当前无对应动态状态，全部参与 hash）——
    hashCombine(m_ColorBlend.logicOpEnable);
    hashCombine(std::hash<vk::LogicOp>{}(m_ColorBlend.logicOp));
    for (const auto &att : m_BlendAttachments)
        hashCombine(std::hash<vk::PipelineColorBlendAttachmentState>{}(att));
    for (float c : m_BlendConstants)
        hashCombine(std::hash<float>{}(c));

    // —— 动态状态集合本身参与 hash（决定了哪些字段被跳过）——
    std::vector<vk::DynamicState> dynStates(m_DynamicStateSet.begin(), m_DynamicStateSet.end());
    std::sort(dynStates.begin(), dynStates.end());
    for (auto s : dynStates)
        hashCombine(std::hash<vk::DynamicState>{}(s));

    // —— PipelineLayout ——
    if (m_PipelineLayout) {
        hashCombine(std::hash<vk::PipelineLayout>{}(m_PipelineLayout->GetHandle()));
    }

    return seed;
}

} // namespace GE
