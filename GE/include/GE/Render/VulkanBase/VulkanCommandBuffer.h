/* Copyright (c) 2025, NVIDIA CORPORATION. All rights reserved.
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
 * @file VulkanCommandBuffer.h
 * @brief 适配 Vulkan-Samples 的 CommandBuffer 设计，带惰性刷入（flush-before-draw）机制。
 *
 * 核心设计：
 * 1. 在 CommandBuffer 上累积管线状态（VulkanPipelineState）、资源绑定、push constants
 * 2. draw() / draw_indexed() / dispatch() 调用时自动 flush()：
 *    flush pipeline → flush push constants → flush descriptor state
 * 3. 管线创建走 VulkanResourceCache，描述符走 VulkanRenderFrame::RequestDescriptorSet
 * 4. GetHandle() 保留，需要时仍可直接操作裸 vk::CommandBuffer
 */

#pragma once

#include <vulkan/vulkan.hpp>

#include <cstdint>
#include <map>
#include <unordered_map>
#include <vector>

#include "Render/VulkanBase/VulkanCommon.h"
#include "Render/VulkanBase/VulkanPipelineState.h"
#include "Render/VulkanBase/VulkanResourceBase.h"

namespace GE {

class VulkanBuffer;
class VulkanCommandPool;
class VulkanDevice;
class VulkanImage;
class VulkanImageView;
class VulkanPipelineLayout;
class VulkanRenderFrame;
class VulkanResourceCache;
class VulkanSampler;

/// 适配 Vulkan-Samples 的 CommandBuffer，带惰性刷入机制。
class VulkanCommandBuffer : public VulkanResourceBase<vk::CommandBuffer> {
public:
    // ========================================================================
    // 构造 / 析构
    // ========================================================================

    /// 从 pool 分配新的 command buffer。
    explicit VulkanCommandBuffer(VulkanCommandPool &pool,
                                 vk::CommandBufferLevel level = vk::CommandBufferLevel::ePrimary);

    /// 包装已有的 command buffer handle（由 pool 预分配）。
    VulkanCommandBuffer(VulkanCommandPool &pool,
                        vk::CommandBufferLevel level,
                        vk::CommandBuffer handle);

    VulkanCommandBuffer(const VulkanCommandBuffer &) = delete;
    VulkanCommandBuffer(VulkanCommandBuffer &&other) noexcept;
    VulkanCommandBuffer &operator=(const VulkanCommandBuffer &) = delete;
    VulkanCommandBuffer &operator=(VulkanCommandBuffer &&) = delete;

    ~VulkanCommandBuffer();

    // ========================================================================
    // 生命周期
    // ========================================================================

    /// 开始录制 command buffer。若为 secondary，需提供 primary command buffer。
    void Begin(vk::CommandBufferUsageFlags flags,
               VulkanCommandBuffer *primary_cmd_buf = nullptr);

    /// 结束录制。
    void End();

    /// 重置 command buffer。
    void Reset();

    // ========================================================================
    // 管线绑定
    // ========================================================================

    /// 绑定 pipeline layout（设置管线布局，标记 pipeline state 脏）。
    void BindPipelineLayout(VulkanPipelineLayout &pipeline_layout);

    /// 绑定顶点缓冲。
    void BindVertexBuffers(uint32_t first_binding,
                           std::vector<std::reference_wrapper<const VulkanBuffer>> const &buffers,
                           std::vector<vk::DeviceSize> const &offsets);

    /// 绑定索引缓冲。
    void BindIndexBuffer(VulkanBuffer const &buffer, vk::DeviceSize offset, vk::IndexType index_type);

    // ========================================================================
    // 资源绑定（惰性，在 draw/dispatch 时刷入）
    // ========================================================================

    /// 绑定 uniform/storage buffer 到指定 set + binding。
    void BindBuffer(VulkanBuffer const &buffer, vk::DeviceSize offset, vk::DeviceSize range,
                    uint32_t set, uint32_t binding, uint32_t array_element = 0);

    /// 绑定 image + sampler 到指定 set + binding。
    void BindImage(VulkanImageView const &image_view, VulkanSampler const &sampler,
                   uint32_t set, uint32_t binding, uint32_t array_element = 0);

    /// 仅绑定 image（无 sampler，如 storage image / input attachment）。
    void BindImage(VulkanImageView const &image_view,
                   uint32_t set, uint32_t binding, uint32_t array_element = 0);

    /// 绑定 input attachment。
    void BindInput(VulkanImageView const &image_view,
                   uint32_t set, uint32_t binding, uint32_t array_element = 0);

    // ========================================================================
    // 管线状态设置（直接通过 GetPipelineState() 访问 m_PipelineState）
    // ========================================================================

    void SetViewport(uint32_t first_viewport, std::vector<vk::Viewport> const &viewports);
    void SetScissor(uint32_t first_scissor, std::vector<vk::Rect2D> const &scissors);
    void SetLineWidth(float line_width);
    void SetDepthBias(float depth_bias_constant_factor, float depth_bias_clamp, float depth_bias_slope_factor);
    void SetBlendConstants(std::array<float, 4> const &blend_constants);
    void SetDepthBounds(float min_depth_bounds, float max_depth_bounds);

    // ========================================================================
    // Push Constants
    // ========================================================================

    /// 累积 push constant 数据（在 flush 时一起刷入）。
    void PushConstants(const std::vector<uint8_t> &values);

    /// 便捷模板：将任意 POD 类型作为 push constant 累积。
    template <typename T>
    void PushConstants(const T &value);

    // ========================================================================
    // 渲染命令（内部调用 flush）
    // ========================================================================

    void Draw(uint32_t vertex_count, uint32_t instance_count,
              uint32_t first_vertex, uint32_t first_instance);

    void DrawIndexed(uint32_t index_count, uint32_t instance_count,
                     uint32_t first_index, int32_t vertex_offset, uint32_t first_instance);

    void Dispatch(uint32_t group_count_x, uint32_t group_count_y, uint32_t group_count_z);

    // ========================================================================
    // 屏障 / 拷贝辅助
    // ========================================================================

    void BufferMemoryBarrier(VulkanBuffer const &buffer, vk::DeviceSize offset, vk::DeviceSize size,
                             GE::BufferMemoryBarrier const &memory_barrier);

    void ImageMemoryBarrier(VulkanImageView const &image_view,
                            GE::ImageMemoryBarrier const &memory_barrier) const;

    void CopyBuffer(VulkanBuffer const &src, VulkanBuffer const &dst, vk::DeviceSize size);

    void CopyBufferToImage(VulkanBuffer const &buffer, VulkanImage const &image,
                           std::vector<vk::BufferImageCopy> const &regions);

    // ========================================================================
    // 访问器
    // ========================================================================

    [[nodiscard]] vk::CommandBufferLevel GetLevel() const { return m_Level; }
    [[nodiscard]] VulkanCommandPool &GetPool() const { return m_Pool; }
    [[nodiscard]] uint32_t GetQueueFamilyIndex() const;
    [[nodiscard]] VulkanPipelineState &GetPipelineState() { return m_PipelineState; }
    [[nodiscard]] VulkanPipelineState const &GetPipelineState() const { return m_PipelineState; }

private:
    // ========================================================================
    // 内部资源绑定类型
    // ========================================================================

    /// 单个 binding 的资源信息。
    struct ResourceInfo {
        VulkanBuffer const *buffer = nullptr;
        vk::DeviceSize offset = 0;
        vk::DeviceSize range = 0;
        VulkanImageView const *image_view = nullptr;
        VulkanSampler const *sampler = nullptr;
    };

    // ========================================================================
    // Flush 机制
    // ========================================================================

    /// 按序刷入：pipeline → push constants → descriptor state。
    void Flush(vk::PipelineBindPoint pipeline_bind_point);

    /// 刷入管线状态（若脏则通过 ResourceCache 创建/获取管线并绑定）。
    void FlushPipelineState(VulkanDevice &device, vk::PipelineBindPoint pipeline_bind_point);

    /// 刷入描述符状态（构建 BindingMap → RenderFrame::RequestDescriptorSet → bind）。
    void FlushDescriptorState(vk::PipelineBindPoint pipeline_bind_point);

    /// 刷入 push constants（累积 → 一次 push → 清空）。
    void FlushPushConstants();

    // ========================================================================
    // 成员变量
    // ========================================================================

    VulkanCommandPool &m_Pool;
    vk::CommandBufferLevel m_Level = vk::CommandBufferLevel::ePrimary;

    /// 管线状态累积器，在 begin() 时重置，在 draw() 时刷入。
    VulkanPipelineState m_PipelineState;

    /// 资源绑定状态：set → binding → array_element → ResourceInfo。
    std::map<uint32_t, std::map<uint32_t, std::map<uint32_t, ResourceInfo>>> m_ResourceSets;
    bool m_ResourceBindingDirty = false;

    /// 已绑定的 descriptor set layout 跟踪（用于检测 layout 变化）。
    std::unordered_map<uint32_t, vk::DescriptorSetLayout> m_BoundDescriptorSetLayouts;

    /// Push constants 累积缓冲区。
    std::vector<uint8_t> m_StoredPushConstants;
    uint32_t m_MaxPushConstantsSize = 128;
};

// ============================================================================
// 模板方法实现
// ============================================================================

template <typename T>
inline void VulkanCommandBuffer::PushConstants(const T &value) {
    const auto *data = reinterpret_cast<const uint8_t *>(&value);
    m_StoredPushConstants.insert(m_StoredPushConstants.end(), data, data + sizeof(T));
}

} // namespace GE