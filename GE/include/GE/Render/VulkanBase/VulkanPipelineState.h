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
 * @brief 专为动态管线设计的管线状态封装。
 *
 * ==================== 设计说明 ====================
 *
 * 1. 每个可调参数单独封装为 DynamicParam<T> 或 StaticParam<T>：
 *    - DynamicParam<T>   —— 可自由切换动态/静态的参数（如 cullMode、depthTestEnable）
 *    - StaticParam<T>    —— 不可动态化的参数（如 vertex input、multisample），固定为静态
 *
 * 2. DynamicParam<T> 内部有 m_IsDynamic 标记：
 *    - true  = 运行时通过 vkCmdSet* 动态更新，变更不重建管线
 *    - false = 作为静态参数参与管线创建，变更触发重建
 *
 * 3. 每个参数有独立的脏标记追踪：
 *    - Set() 设置值并标记 m_ValueDirty
 *    - SetDynamic() 切换动态/静态并标记 m_ConfigDirty
 *    - 通过 HasDynamicDirty() / HasPipelineDirty() 聚合查询
 *
 * 4. 支持的 Vulkan 1.3 Core 动态状态：
 *    eViewport, eScissor, eCullMode, eFrontFace, ePrimitiveTopology,
 *    eDepthTestEnable, eDepthWriteEnable, eDepthCompareOp,
 *    eDepthBoundsTestEnable, eStencilTestEnable, eStencilOp,
 *    eRasterizerDiscardEnable, eDepthBiasEnable,
 *    eViewportWithCount, eScissorWithCount
 *
 * 5. 对于 Vulkan 1.3 Core 不支持动态化的参数（如 polygonMode、blend 等），
 *    使用 StaticParam<T> 包装，始终为静态。
 *
 * ==================================================
 */

#pragma once

#include <vulkan/vulkan.hpp>

#include <cstdint>
#include <string>
#include <vector>

namespace GE {

// ============================================================================
// 前向声明
// ============================================================================

class VulkanPipelineLayout;


// ============================================================================
// 参数包装器模板
// ============================================================================

/**
 * @brief 可自由切换动态/静态的管线参数包装器。
 *
 * 内部状态：
 *   - m_Value：参数值
 *   - m_IsDynamic：是否为动态（true → vkCmdSet* 更新，false → 需重建管线）
 *   - m_ValueDirty：值是否在上次清除后发生过变更
 *   - m_ConfigDirty：动态/静态标记是否在上次清除后发生过变更
 */
template <typename T>
class DynamicParam {
public:
    explicit DynamicParam(const T &val = {})
        : m_Value(val) {
    }

    /// 设置值并标记脏（总是标记 m_ValueDirty）
    DynamicParam &operator=(const T &val) {
        m_Value = val;
        m_ValueDirty = true;
        return *this;
    }

    /// 赋值但不标记脏（用于初始化）
    void Assign(const T &val) {
        m_Value = val;
    }

    const T &Get() const { return m_Value; }
    T &Get() { return m_Value; }

    /// 切换动态/静态。与上次不同时标记 m_ConfigDirty。
    void SetDynamic(bool dyn) {
        if (m_IsDynamic != dyn) {
            m_IsDynamic = dyn;
            m_ConfigDirty = true;
        }
    }

    bool IsDynamic() const { return m_IsDynamic; }

    bool IsValueDirty() const { return m_ValueDirty; }
    bool IsConfigDirty() const { return m_ConfigDirty; }

    void ClearDirty() {
        m_ValueDirty = false;
        m_ConfigDirty = false;
    }

private:
    T m_Value{};
    bool m_IsDynamic{false};
    bool m_ValueDirty{false};
    bool m_ConfigDirty{true}; // 初始为 true，管线尚未创建
};


/**
 * @brief 始终静态的管线参数包装器。
 *
 * 用于不可动态化的参数，仅追踪值变更脏标记。
 */
template <typename T>
class StaticParam {
public:
    explicit StaticParam(const T &val = {})
        : m_Value(val) {
    }

    /// 设置值并标记脏（总是标记 m_ValueDirty）
    StaticParam &operator=(const T &val) {
        m_Value = val;
        m_ValueDirty = true;
        return *this;
    }

    /// 赋值但不标记脏（用于初始化）
    void Assign(const T &val) {
        m_Value = val;
    }

    const T &Get() const { return m_Value; }
    T &Get() { return m_Value; }

    bool IsValueDirty() const { return m_ValueDirty; }

    void ClearDirty() {
        m_ValueDirty = false;
    }

private:
    T m_Value{};
    bool m_ValueDirty{false};
};


// ============================================================================
// 辅助结构体
// ============================================================================

/**
 * @brief 模板操作状态（可为动态）。
 *
 * 四个成员对应 VkStencilOpState 的四个字段，
 * 每个都可以独立控制是否为动态。
 */
struct StencilOpState {
    DynamicParam<vk::StencilOp> failOp{vk::StencilOp::eKeep};
    DynamicParam<vk::StencilOp> passOp{vk::StencilOp::eKeep};
    DynamicParam<vk::StencilOp> depthFailOp{vk::StencilOp::eKeep};
    DynamicParam<vk::CompareOp> compareOp{vk::CompareOp::eAlways};

    /// 是否有任一子字段标记为动态
    bool IsAnyDynamic() const {
        return failOp.IsDynamic() || passOp.IsDynamic()
               || depthFailOp.IsDynamic() || compareOp.IsDynamic();
    }

    /// 是否有任一子字段的值是脏的
    bool IsAnyValueDirty() const {
        return failOp.IsValueDirty() || passOp.IsValueDirty()
               || depthFailOp.IsValueDirty() || compareOp.IsValueDirty();
    }

    /// 是否有任一子字段的配置是脏的（动态/静态标记变更）
    bool IsAnyConfigDirty() const {
        return failOp.IsConfigDirty() || passOp.IsConfigDirty()
               || depthFailOp.IsConfigDirty() || compareOp.IsConfigDirty();
    }

    void ClearAllDirty() {
        failOp.ClearDirty();
        passOp.ClearDirty();
        depthFailOp.ClearDirty();
        compareOp.ClearDirty();
    }

    /// 一键设置所有子字段的动态/静态
    void SetAllDynamic(bool dyn) {
        failOp.SetDynamic(dyn);
        passOp.SetDynamic(dyn);
        depthFailOp.SetDynamic(dyn);
        compareOp.SetDynamic(dyn);
    }
};


// 混合附件直接使用 vk::PipelineColorBlendAttachmentState，无需额外封装。


// ============================================================================
// PipelineCreateBundle —— 一键构建 VkGraphicsPipelineCreateInfo
// ============================================================================

/**
 * @brief 持有所有 Vk*CreateInfo 及其依赖数据的 Bundle。
 *
 * 由 VulkanPipelineState::BuildCreateInfo() 生成，
 * 可直接传给 vk::Device::createGraphicsPipeline()。
 * 生命周期：调用者需保证 Bundle 在 pipeline 创建完成前存活。
 */
struct PipelineCreateBundle {
    // —— 次级数据（被 CreateInfo 指向） ——
    std::vector<vk::PipelineShaderStageCreateInfo> shaderStageCreateInfos;
    std::vector<vk::DynamicState> dynamicStates;
    std::vector<vk::VertexInputBindingDescription> vertexBindings;
    std::vector<vk::VertexInputAttributeDescription> vertexAttributes;
    std::vector<vk::PipelineColorBlendAttachmentState> blendAttachmentStates;
    std::vector<vk::Format> colorAttachmentFormats;
    std::vector<vk::Viewport> viewports; // 动态时可为空
    std::vector<vk::Rect2D> scissors; // 动态时可为空
    vk::SampleMask sampleMaskData{0};

    // —— CreateInfo 链 ——
    vk::PipelineShaderStageCreateInfo shaderStageInfo{}; // 实际未使用，stages 直接用 vector
    vk::PipelineVertexInputStateCreateInfo vertexInputInfo{};
    vk::PipelineInputAssemblyStateCreateInfo inputAssemblyInfo{};
    vk::PipelineTessellationStateCreateInfo tessellationInfo{};
    vk::PipelineViewportStateCreateInfo viewportInfo{};
    vk::PipelineRasterizationStateCreateInfo rasterizationInfo{};
    vk::PipelineMultisampleStateCreateInfo multisampleInfo{};
    vk::PipelineDepthStencilStateCreateInfo depthStencilInfo{};
    vk::PipelineColorBlendStateCreateInfo colorBlendInfo{};
    vk::PipelineDynamicStateCreateInfo dynamicStateInfo{};
    vk::PipelineRenderingCreateInfo renderingInfo{};

    /// 主 CreateInfo（pNext 链已串联好）
    vk::GraphicsPipelineCreateInfo pipelineInfo{};
};


// ============================================================================
// VulkanPipelineState —— 动态管线状态主类
// ============================================================================

/**
 * @brief 专为动态管线设计的状态管理器。
 *
 * 特性：
 *   - 所有管线参数以 public 成员暴露，可直接读写
 *   - 每个参数独立追踪脏标记
 *   - FlushDynamicStates() 自动将动态参数刷入 command buffer
 *   - BuildCreateInfo() 一键生成完整的 GraphicsPipelineCreateInfo
 *   - 支持 Vulkan 1.3 Dynamic Rendering
 *
 * 使用示例：
 * @code
 *   VulkanPipelineState state;
 *   state.topology        = vk::PrimitiveTopology::eTriangleList;
 *   state.cullMode        = vk::CullModeFlagBits::eBack;
 *   state.depthTestEnable = VK_TRUE;
 *
 *   // 设为动态
 *   state.cullMode.SetDynamic(true);
 *   state.depthTestEnable.SetDynamic(true);
 *
 *   // 创建管线
 *   auto bundle = state.BuildCreateInfo();
 *   auto pipeline = device.createGraphicsPipeline(nullptr, bundle.pipelineInfo);
 *   state.ClearAllDirty();
 *
 *   // 运行时更新动态状态
 *   state.cullMode = vk::CullModeFlagBits::eFront;
 *   state.FlushDynamicStates(cmdBuffer);
 * @endcode
 */
class VulkanPipelineState {
public:
    // ====================================================================
    // 管线参数（public 成员，直接读写）
    // ====================================================================

    // ----- 渲染附件格式（始终静态） -----
    StaticParam<std::vector<vk::Format> > colorAttachmentFormats{};
    StaticParam<vk::Format> depthFormat{};
    StaticParam<vk::Format> stencilFormat{};

    // ----- 着色器阶段（始终静态，从 pipelineLayout->GetShaderModules() 获取） -----
    // 无需单独存储，BuildCreateInfo 中从 VulkanPipelineLayout 提取。

    // ----- 管线布局（始终静态，指向外部所有的 VulkanPipelineLayout） -----
    StaticParam<VulkanPipelineLayout *> pipelineLayout{nullptr};

    // ----- 顶点输入（始终静态） -----
    StaticParam<std::vector<vk::VertexInputBindingDescription> > vertexBindingDescriptions{};
    StaticParam<std::vector<vk::VertexInputAttributeDescription> > vertexAttributeDescriptions{};

    // ----- 输入装配 -----
    DynamicParam<vk::PrimitiveTopology> topology{vk::PrimitiveTopology::eTriangleList};
    StaticParam<vk::Bool32> primitiveRestartEnable{VK_FALSE};

    // ----- 细分曲面（始终静态） -----
    StaticParam<uint32_t> patchControlPoints{3};

    // ----- 视口 -----
    // 视口/剪刀矩形本身不存储在这里（它们是每帧的动态数据），
    // 只存储创建管线时所需的计数。当视口设为动态时，Vulkan 忽略这些计数值。
    DynamicParam<uint32_t> viewportCount{1};
    DynamicParam<uint32_t> scissorCount{1};

    // ----- 光栅化 -----
    StaticParam<vk::Bool32> depthClampEnable{VK_FALSE};
    DynamicParam<vk::Bool32> rasterizerDiscardEnable{VK_FALSE};
    StaticParam<vk::PolygonMode> polygonMode{vk::PolygonMode::eFill};
    DynamicParam<vk::CullModeFlags> cullMode{vk::CullModeFlagBits::eBack};
    DynamicParam<vk::FrontFace> frontFace{vk::FrontFace::eCounterClockwise};
    DynamicParam<vk::Bool32> depthBiasEnable{VK_FALSE};

    // ----- 多重采样（始终静态） -----
    StaticParam<vk::SampleCountFlagBits> rasterizationSamples{vk::SampleCountFlagBits::e1};
    StaticParam<vk::Bool32> sampleShadingEnable{VK_FALSE};
    StaticParam<float> minSampleShading{0.0f};
    StaticParam<vk::SampleMask> sampleMask{0};
    StaticParam<vk::Bool32> alphaToCoverageEnable{VK_FALSE};
    StaticParam<vk::Bool32> alphaToOneEnable{VK_FALSE};

    // ----- 深度/模板 -----
    DynamicParam<vk::Bool32> depthTestEnable{VK_TRUE};
    DynamicParam<vk::Bool32> depthWriteEnable{VK_TRUE};
    DynamicParam<vk::CompareOp> depthCompareOp{vk::CompareOp::eLess};
    DynamicParam<vk::Bool32> depthBoundsTestEnable{VK_FALSE};
    DynamicParam<vk::Bool32> stencilTestEnable{VK_FALSE};
    StencilOpState stencilFront{};
    StencilOpState stencilBack{};


    StaticParam<vk::Bool32> logicOpEnable{VK_FALSE};
    StaticParam<vk::LogicOp> logicOp{vk::LogicOp::eClear};

private:
    std::vector<vk::PipelineColorBlendAttachmentState> m_BlendAttachments{};

    /// blendAttachments 的脏标记（vector 大小或内容变更）
    bool m_BlendAttachmentsDirty{false};

public:
    // ====================================================================
    // 混合附件操作方法
    // ====================================================================

    /// 设置完整的混合附件列表（标记脏）
    void SetBlendAttachments(const std::vector<vk::PipelineColorBlendAttachmentState> &attachments);

    /// 设置指定索引的混合附件（标记脏）
    void SetBlendAttachment(uint32_t index, const vk::PipelineColorBlendAttachmentState &attachment);

    /// 获取混合附件列表（只读）
    const std::vector<vk::PipelineColorBlendAttachmentState> &GetBlendAttachments() const { return m_BlendAttachments; }

    /// 获取可修改的混合附件列表引用（调用者需手动标记脏）
    std::vector<vk::PipelineColorBlendAttachmentState> &GetMutableBlendAttachments() {
        m_BlendAttachmentsDirty = true;
        return m_BlendAttachments;
    }

    // ====================================================================
    // 公共方法
    // ====================================================================

    // ---- 脏标记查询 ----

    /// 是否有动态参数的值发生了变更（需要 vkCmdSet* 刷入）
    [[nodiscard]] bool HasDynamicDirty() const;

    /// 是否需要重建 VkPipeline（静态参数变更 / 动态标记变更 / blend attachments 变更）
    [[nodiscard]] bool HasPipelineDirty() const;

    /// 清除所有脏标记
    void ClearAllDirty();

    // ---- 动态状态枚举 ----

    /// 收集当前所有标记为动态的 vk::DynamicState 值。
    /// 用于创建 VkPipelineDynamicStateCreateInfo。
    [[nodiscard]] std::vector<vk::DynamicState> GetEnabledDynamicStates() const;

    // ---- 状态刷入 ----

    /// 将所有动态参数的最新值通过 vkCmdSet* 写入 command buffer。
    /// 注意：不包含 Viewport/Scissor 矩形（需额外调用 vkCmdSetViewport/Scissor）。
    void FlushDynamicStates(vk::CommandBuffer cmd) const;

    // ---- 管线创建 ----

    /// 从当前状态构建完整的 VkGraphicsPipelineCreateInfo。
    /// @param flags 可选的 VkPipelineCreateFlags（如 eAllowDerivatives）
    [[nodiscard]] PipelineCreateBundle BuildCreateInfo(vk::PipelineCreateFlags flags = {}) const;

    // ---- 便利方法：批量切换动态/静态 ----

    void SetAllInputAssemblyDynamic(bool dyn);

    void SetAllRasterizationDynamic(bool dyn);

    void SetAllDepthStencilDynamic(bool dyn);

    void SetAllViewportDynamic(bool dyn);

    void SetAllDynamic(bool dyn); // 所有可动态参数设为 dyn
};

} // namespace GE
