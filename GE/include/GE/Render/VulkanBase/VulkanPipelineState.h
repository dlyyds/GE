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
 * @brief 图形管线状态构建器（链式调用 + 动态渲染支持）。
 *
 * ==================== 设计说明 ====================
 *
 * 1. 纯构建器模式：所有 set 方法返回 VulkanPipelineState&，支持链式调用。
 *
 * 2. 动态状态管理：
 *    - enableDynamicState() / disableDynamicState() 直接操作 vk::DynamicState 枚举
 *    - 内部使用 unordered_set 去重，缓存 vector 用于管线创建
 *
 * 3. 两种构建模式：
 *    - buildDynamicRenderingPipeline()：动态渲染模式（VK_NULL_HANDLE renderPass + pNext 链）
 *    - buildRenderPassPipeline()：传统 RenderPass 模式
 *
 * 4. flushDynamicStates()：成员方法，根据当前启用的动态状态集合
 *    将当前状态值刷入 command buffer（调用 vkCmdSet* 系列）。
 *
 * 5. hash()：用于管线缓存去重，对所有影响管线创建的状态计算哈希。
 *
 * ==================================================
 */

#pragma once

#include <vulkan/vulkan.hpp>

#include <cstdint>
#include <string>
#include <unordered_set>
#include <vector>

namespace GE {

// ============================================================================
// 前向声明
// ============================================================================

class VulkanPipelineLayout;
class VulkanShaderModule;


// ============================================================================
// VulkanPipelineState —— 图形管线状态构建器
// ============================================================================

/**
 * @brief 图形管线状态构建器，链式 API + 动态渲染 + 动态状态刷入。
 *
 * 使用示例：
 * @code
 *   VulkanPipelineState state;
 *   state.setInputAssembly(vk::PrimitiveTopology::eTriangleList)
 *        .setCullMode(vk::CullModeFlagBits::eBack)
 *        .setDepthTestEnable(VK_TRUE)
 *        .enableDynamicState(vk::DynamicState::eCullMode)
 *        .setRenderingFormats({vk::Format::eB8G8R8A8Unorm})
 *        .setPipelineLayout(pipelineLayout);
 *
 *   auto &pipelineInfo = state.buildDynamicRenderingPipeline();
 *   auto pipeline = device.createGraphicsPipeline(nullptr, pipelineInfo);
 *
 *   // 运行时更新动态状态
 *   state.setCullMode(vk::CullModeFlagBits::eFront);
 *   state.flushDynamicStates(cmd);
 * @endcode
 */
class VulkanPipelineState {
public:
    // ====================================================================
    // 动态状态管理
    // ====================================================================

    /// 启用一个动态状态（加入动态状态集合）
    VulkanPipelineState &enableDynamicState(vk::DynamicState state);

    /// 禁用一个动态状态（从动态状态集合移除）
    VulkanPipelineState &disableDynamicState(vk::DynamicState state);

    /// 清空所有动态状态
    void clearDynamicStates();

    /// 获取当前所有启用的动态状态
    const std::vector<vk::DynamicState> &getDynamicStates() const;

    // ====================================================================
    // 渲染附件格式（动态渲染 pNext 数据）
    // ====================================================================

    /// 设置动态渲染所需的附件格式信息
    VulkanPipelineState &setRenderingFormats(std::vector<vk::Format> colorFmts,
                                             vk::Format              depthFmt   = vk::Format::eUndefined,
                                             vk::Format              stencilFmt = vk::Format::eUndefined,
                                             uint32_t                viewMask   = 0);

    // ====================================================================
    // 管线布局
    // ====================================================================

    /// 设置管线布局（指针，外部所有）
    VulkanPipelineState &setPipelineLayout(VulkanPipelineLayout *layout);

    /// 获取管线布局指针
    VulkanPipelineLayout *getPipelineLayout() const;

    // ====================================================================
    // 顶点输入
    // ====================================================================

    /// 直接设置完整的顶点输入 CreateInfo
    VulkanPipelineState &setVertexInput(const vk::PipelineVertexInputStateCreateInfo &info);

    /// 从顶点着色器模块的反射数据自动生成单 binding 顶点输入描述（紧密打包）。
    /// 按 location 升序排列属性，offset 按属性大小紧密累加。
    /// @param vertShader 顶点阶段的 VulkanShaderModule（需已完成反射）
    /// @param binding    顶点缓冲绑定点（默认 0）
    /// @param rate       顶点/实例速率（默认 eVertex）
    /// @return 单个顶点的 stride（字节数）
    uint32_t setVertexInputFromShader(const VulkanShaderModule &vertShader,
                                      uint32_t           binding = 0,
                                      vk::VertexInputRate rate   = vk::VertexInputRate::eVertex);

    /// 设置顶点绑定描述
    VulkanPipelineState &setVertexBindings(const std::vector<vk::VertexInputBindingDescription> &bindings);

    /// 设置顶点属性描述
    VulkanPipelineState &setVertexAttributes(const std::vector<vk::VertexInputAttributeDescription> &attrs);

    // ====================================================================
    // 输入装配
    // ====================================================================

    /// 设置图元拓扑
    VulkanPipelineState &setInputAssembly(vk::PrimitiveTopology topology, vk::Bool32 restart = VK_FALSE);

    // ====================================================================
    // 细分曲面
    // ====================================================================

    /// 设置细分曲面 patch 控制点数量
    VulkanPipelineState &setTessellationPatchControlPoints(uint32_t patchControlPoints);

    // ====================================================================
    // 视口
    // ====================================================================

    /// 直接设置完整的视口状态 CreateInfo
    VulkanPipelineState &setViewportState(const vk::PipelineViewportStateCreateInfo &info);

    /// 设置视口数量和剪刀矩形数量（动态时使用计数版本）
    VulkanPipelineState &setViewportCount(uint32_t viewportCount, uint32_t scissorCount = 1);

    // ====================================================================
    // 光栅化
    // ====================================================================

    /// 直接设置完整的光栅化状态 CreateInfo
    VulkanPipelineState &setRasterization(const vk::PipelineRasterizationStateCreateInfo &info);

    /// 设置深度钳位
    VulkanPipelineState &setDepthClampEnable(vk::Bool32 enable);

    /// 设置光栅化丢弃
    VulkanPipelineState &setRasterizerDiscardEnable(vk::Bool32 enable);

    /// 设置多边形模式
    VulkanPipelineState &setPolygonMode(vk::PolygonMode mode);

    /// 设置剔除模式
    VulkanPipelineState &setCullMode(vk::CullModeFlags mode);

    /// 设置正面方向
    VulkanPipelineState &setFrontFace(vk::FrontFace face);

    /// 设置深度偏移启用
    VulkanPipelineState &setDepthBiasEnable(vk::Bool32 enable);

    /// 设置线宽
    VulkanPipelineState &setLineWidth(float lineWidth);

    // ====================================================================
    // 多重采样
    // ====================================================================

    /// 直接设置完整的多重采样状态 CreateInfo
    VulkanPipelineState &setMultisample(const vk::PipelineMultisampleStateCreateInfo &info);

    /// 设置采样数
    VulkanPipelineState &setRasterizationSamples(vk::SampleCountFlagBits samples);

    /// 设置采样着色启用
    VulkanPipelineState &setSampleShadingEnable(vk::Bool32 enable);

    /// 设置最小采样着色率
    VulkanPipelineState &setMinSampleShading(float rate);

    /// 设置采样遮罩
    VulkanPipelineState &setSampleMask(vk::SampleMask mask);

    /// 设置 Alpha 到覆盖
    VulkanPipelineState &setAlphaToCoverageEnable(vk::Bool32 enable);

    /// 设置 Alpha 到一
    VulkanPipelineState &setAlphaToOneEnable(vk::Bool32 enable);

    // ====================================================================
    // 深度/模板
    // ====================================================================

    /// 直接设置完整的深度模板状态 CreateInfo
    VulkanPipelineState &setDepthStencil(const vk::PipelineDepthStencilStateCreateInfo &info);

    /// 设置深度测试启用
    VulkanPipelineState &setDepthTestEnable(vk::Bool32 enable);

    /// 设置深度写入启用
    VulkanPipelineState &setDepthWriteEnable(vk::Bool32 enable);

    /// 设置深度比较函数
    VulkanPipelineState &setDepthCompareOp(vk::CompareOp op);

    /// 设置深度边界测试启用
    VulkanPipelineState &setDepthBoundsTestEnable(vk::Bool32 enable);

    /// 设置模板测试启用
    VulkanPipelineState &setStencilTestEnable(vk::Bool32 enable);

    /// 设置正面模板操作
    VulkanPipelineState &setStencilFront(const vk::StencilOpState &state);

    /// 设置背面模板操作
    VulkanPipelineState &setStencilBack(const vk::StencilOpState &state);

    // ====================================================================
    // 颜色混合
    // ====================================================================

    /// 添加一个颜色混合附件
    VulkanPipelineState &addColorBlendAttachment(const vk::PipelineColorBlendAttachmentState &att);

    /// 清空颜色混合附件
    VulkanPipelineState &clearColorBlendAttachments();

    /// 设置完整的颜色混合附件列表
    VulkanPipelineState &setColorBlendAttachments(const std::vector<vk::PipelineColorBlendAttachmentState> &attachments);

    /// 获取颜色混合附件列表
    const std::vector<vk::PipelineColorBlendAttachmentState> &getColorBlendAttachments() const;

    /// 设置混合常量
    VulkanPipelineState &setColorBlendConstants(float r, float g, float b, float a);

    /// 设置逻辑操作启用
    VulkanPipelineState &setLogicOpEnable(vk::Bool32 enable);

    /// 设置逻辑操作
    VulkanPipelineState &setLogicOp(vk::LogicOp op);

    // ====================================================================
    // 管线构建
    // ====================================================================

    /**
     * @brief 构建动态渲染模式的管线创建信息。
     *
     * 组装所有内部 CreateInfo 的指针后返回主 CreateInfo 的 const 引用。
     * 所有指针指向 VulkanPipelineState 自身的成员数据，调用者需保证
     * VulkanPipelineState 对象在管线创建完成前存活。
     *
     * @param flags  可选的管线创建标志（如 eAllowDerivatives）
     * @return 主 GraphicsPipelineCreateInfo 的 const 引用（pNext 链已串联）
     */
    const vk::GraphicsPipelineCreateInfo &buildDynamicRenderingPipeline(
        vk::PipelineCreateFlags flags = {}) const;

    /**
     * @brief 构建传统 RenderPass 模式的管线创建信息。
     *
     * @param renderPass  RenderPass 句柄
     * @param subpass     子通道索引（默认 0）
     * @param flags       可选的管线创建标志
     * @return 主 GraphicsPipelineCreateInfo 的 const 引用
     */
    const vk::GraphicsPipelineCreateInfo &buildRenderPassPipeline(
        vk::RenderPass          renderPass,
        uint32_t                subpass = 0,
        vk::PipelineCreateFlags flags   = {}) const;

    // ====================================================================
    // 动态状态刷入
    // ====================================================================

    /**
     * @brief 将当前动态状态值通过 vkCmdSet* 写入 command buffer。
     *
     * 仅对已通过 enableDynamicState() 启用的动态状态执行刷入。
     * 注意：不包含 Viewport/Scissor 矩形（需外部调用 vkCmdSetViewport/Scissor）。
     */
    void flushDynamicStates(vk::CommandBuffer cmd) const;

    // ====================================================================
    // 哈希（用于管线缓存去重）
    // ====================================================================

    /// 计算状态哈希（用于 unordered_map 缓存的 key）
    size_t hash() const;

    // ====================================================================
    // 状态访问器（只读，供 hash 特化等使用）
    // ====================================================================

    // 注意：以下 Get 方法主要供 std::hash 特化和其他需要读取状态的代码使用。
    // 对于设置状态，优先使用链式 set 方法。

    vk::PrimitiveTopology                            getTopology()              const { return m_InputAssembly.topology; }
    vk::Bool32                                       getPrimitiveRestartEnable() const { return m_InputAssembly.primitiveRestartEnable; }
    uint32_t                                         getPatchControlPoints()    const { return m_Tessellation.patchControlPoints; }
    uint32_t                                         getViewportCount()         const { return m_ViewportState.viewportCount; }
    uint32_t                                         getScissorCount()          const { return m_ViewportState.scissorCount; }
    vk::Bool32                                       getDepthClampEnable()      const { return m_Rasterization.depthClampEnable; }
    vk::Bool32                                       getRasterizerDiscardEnable() const { return m_Rasterization.rasterizerDiscardEnable; }
    vk::PolygonMode                                  getPolygonMode()           const { return m_Rasterization.polygonMode; }
    vk::CullModeFlags                                getCullMode()              const { return m_Rasterization.cullMode; }
    vk::FrontFace                                    getFrontFace()             const { return m_Rasterization.frontFace; }
    vk::Bool32                                       getDepthBiasEnable()       const { return m_Rasterization.depthBiasEnable; }
    float                                            getLineWidth()             const { return m_Rasterization.lineWidth; }
    vk::SampleCountFlagBits                          getRasterizationSamples()  const { return m_Multisample.rasterizationSamples; }
    vk::Bool32                                       getSampleShadingEnable()   const { return m_Multisample.sampleShadingEnable; }
    float                                            getMinSampleShading()      const { return m_Multisample.minSampleShading; }
    vk::SampleMask                                   getSampleMask()            const { return m_SampleMask; }
    vk::Bool32                                       getAlphaToCoverageEnable() const { return m_Multisample.alphaToCoverageEnable; }
    vk::Bool32                                       getAlphaToOneEnable()      const { return m_Multisample.alphaToOneEnable; }
    vk::Bool32                                       getDepthTestEnable()       const { return m_DepthStencil.depthTestEnable; }
    vk::Bool32                                       getDepthWriteEnable()      const { return m_DepthStencil.depthWriteEnable; }
    vk::CompareOp                                    getDepthCompareOp()        const { return m_DepthStencil.depthCompareOp; }
    vk::Bool32                                       getDepthBoundsTestEnable() const { return m_DepthStencil.depthBoundsTestEnable; }
    vk::Bool32                                       getStencilTestEnable()     const { return m_DepthStencil.stencilTestEnable; }
    const vk::StencilOpState &                       getStencilFront()          const { return m_DepthStencil.front; }
    const vk::StencilOpState &                       getStencilBack()           const { return m_DepthStencil.back; }
    vk::Bool32                                       getLogicOpEnable()         const { return m_ColorBlend.logicOpEnable; }
    vk::LogicOp                                      getLogicOp()               const { return m_ColorBlend.logicOp; }
    const std::array<float, 4> &                     getBlendConstants()        const { return m_BlendConstants; }
    const std::vector<vk::VertexInputBindingDescription> &   getVertexBindings()   const { return m_VertexBindings; }
    const std::vector<vk::VertexInputAttributeDescription> & getVertexAttributes() const { return m_VertexAttributes; }
    const std::vector<vk::Format> &                          getColorFormats()    const { return m_ColorFormats; }
    vk::Format                                               getDepthFormat()     const { return m_DepthFormat; }
    vk::Format                                               getStencilFormat()   const { return m_StencilFormat; }
    uint32_t                                                 getViewMask()        const { return m_ViewMask; }

private:
    // ====================================================================
    // 内部状态存储
    // ====================================================================

    // —— 顶点输入 ——
    std::vector<vk::VertexInputBindingDescription>   m_VertexBindings{};
    std::vector<vk::VertexInputAttributeDescription> m_VertexAttributes{};

    // —— 输入装配 ——
    vk::PipelineInputAssemblyStateCreateInfo m_InputAssembly{
        .topology = vk::PrimitiveTopology::eTriangleList,
        .primitiveRestartEnable = VK_FALSE,
    };

    // —— 细分曲面 ——
    vk::PipelineTessellationStateCreateInfo m_Tessellation{
        .patchControlPoints = 3,
    };

    // —— 视口 ——
    vk::PipelineViewportStateCreateInfo m_ViewportState{
        .viewportCount = 1,
        .pViewports    = nullptr,
        .scissorCount  = 1,
        .pScissors     = nullptr,
    };

    // —— 光栅化 ——
    vk::PipelineRasterizationStateCreateInfo m_Rasterization{
        .depthClampEnable        = VK_FALSE,
        .rasterizerDiscardEnable = VK_FALSE,
        .polygonMode             = vk::PolygonMode::eFill,
        .cullMode                = vk::CullModeFlagBits::eBack,
        .frontFace               = vk::FrontFace::eCounterClockwise,
        .depthBiasEnable         = VK_FALSE,
        .depthBiasConstantFactor = 0.0f,
        .depthBiasClamp          = 0.0f,
        .depthBiasSlopeFactor    = 0.0f,
        .lineWidth               = 1.0f,
    };

    // —— 多重采样 ——
    // sampleMask 单独存储，CreateInfo 中的 pSampleMask 指向它
    vk::SampleMask                               m_SampleMask{0};
    vk::PipelineMultisampleStateCreateInfo       m_Multisample{
        .rasterizationSamples  = vk::SampleCountFlagBits::e1,
        .sampleShadingEnable   = VK_FALSE,
        .minSampleShading      = 0.0f,
        .pSampleMask           = nullptr,
        .alphaToCoverageEnable = VK_FALSE,
        .alphaToOneEnable      = VK_FALSE,
    };

    // —— 深度/模板 ——
    vk::PipelineDepthStencilStateCreateInfo m_DepthStencil{
        .depthTestEnable       = VK_TRUE,
        .depthWriteEnable      = VK_TRUE,
        .depthCompareOp        = vk::CompareOp::eLess,
        .depthBoundsTestEnable = VK_FALSE,
        .stencilTestEnable     = VK_FALSE,
        .front                 = {vk::StencilOp::eKeep, vk::StencilOp::eKeep, vk::StencilOp::eKeep, vk::CompareOp::eAlways},
        .back                  = {vk::StencilOp::eKeep, vk::StencilOp::eKeep, vk::StencilOp::eKeep, vk::CompareOp::eAlways},
        .minDepthBounds        = 0.0f,
        .maxDepthBounds        = 0.0f,
    };

    // —— 颜色混合 ——
    std::vector<vk::PipelineColorBlendAttachmentState> m_BlendAttachments{};
    std::array<float, 4>                               m_BlendConstants{0.0f, 0.0f, 0.0f, 0.0f};
    vk::PipelineColorBlendStateCreateInfo              m_ColorBlend{
        .logicOpEnable = VK_FALSE,
        .logicOp       = vk::LogicOp::eClear,
        .attachmentCount = 0,
        .pAttachments    = nullptr,
        .blendConstants  = {{0.0f, 0.0f, 0.0f, 0.0f}},
    };

    // —— 动态状态 ——
    std::unordered_set<vk::DynamicState>   m_DynamicStateSet;
    mutable std::vector<vk::DynamicState>  m_DynamicStateCache;

    // —— 动态渲染格式信息 ——
    std::vector<vk::Format> m_ColorFormats{};
    vk::Format              m_DepthFormat    = vk::Format::eUndefined;
    vk::Format              m_StencilFormat  = vk::Format::eUndefined;
    uint32_t                m_ViewMask       = 0;

    // —— 管线布局（指针，外部所有） ——
    VulkanPipelineLayout *m_PipelineLayout = nullptr;

    // ====================================================================
    // 内部构建缓存（mutable，build 时填充，指针指向 state 自身成员）
    // ====================================================================

    // 着色器阶段缓存（从 pipeline layout 提取，pStages 指向这里）
    mutable std::vector<vk::PipelineShaderStageCreateInfo> m_ShaderStageCache;

    // 动态状态 CreateInfo（pDynamicStates 指向 m_DynamicStateCache）
    mutable vk::PipelineDynamicStateCreateInfo m_DynamicStateInfo{};

    // 顶点输入 CreateInfo（指向 m_VertexBindings / m_VertexAttributes）
    mutable vk::PipelineVertexInputStateCreateInfo m_VertexInputInfo{};

    // 颜色混合 CreateInfo（指向 m_BlendAttachments / m_BlendConstants）
    mutable vk::PipelineColorBlendStateCreateInfo m_ColorBlendInfo{};

    // 动态渲染 pNext 结构体（指向 m_ColorFormats）
    mutable vk::PipelineRenderingCreateInfo m_RenderingInfo{};

    // 主 CreateInfo
    mutable vk::GraphicsPipelineCreateInfo m_PipelineInfo{};

    // ====================================================================
    // 内部辅助
    // ====================================================================

    /// 组装内部各 CreateInfo 的公共部分（不含 renderPass / pNext）
    void buildCommon(vk::PipelineCreateFlags flags) const;
};

} // namespace GE
