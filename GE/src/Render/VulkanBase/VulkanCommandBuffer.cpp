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

#include "Render/VulkanBase/VulkanCommandBuffer.h"

#include "Render/VulkanBase/VulkanBuffer.h"
#include "Render/VulkanBase/VulkanCommandPool.h"
#include "Render/VulkanBase/VulkanDevice.h"
#include "Render/VulkanBase/VulkanImage.h"
#include "Render/VulkanBase/VulkanImageView.h"
#include "Render/VulkanBase/VulkanPipelineLayout.h"
#include "Render/VulkanBase/VulkanRenderFrame.h"
#include "Render/VulkanBase/VulkanResourceCache.h"
#include "Render/VulkanBase/VulkanSampler.h"

#include <tracy/Tracy.hpp>

#include <cassert>
#include <cstring>
#include <utility>

namespace GE {

// ============================================================================
// 构造 / 析构
// ============================================================================

VulkanCommandBuffer::VulkanCommandBuffer(VulkanCommandPool &pool, vk::CommandBufferLevel level)
    : VulkanResourceBase(nullptr, &pool.GetDevice())
    , m_Pool(pool)
    , m_Level(level) {
    vk::CommandBufferAllocateInfo alloc_info{
        .commandPool = pool.GetHandle(),
        .level = level,
        .commandBufferCount = 1,
    };

    SetHandle(GetDevice().GetHandle().allocateCommandBuffers(alloc_info).front());

    // 查询设备的最大 push constants 大小
    auto const &props = GetDevice().GetGpu().GetProperties();
    m_MaxPushConstantsSize = props.limits.maxPushConstantsSize;
}

VulkanCommandBuffer::VulkanCommandBuffer(VulkanCommandPool &pool,
                                         vk::CommandBufferLevel level,
                                         vk::CommandBuffer handle)
    : VulkanResourceBase(handle, &pool.GetDevice())
    , m_Pool(pool)
    , m_Level(level) {
    auto const &props = GetDevice().GetGpu().GetProperties();
    m_MaxPushConstantsSize = props.limits.maxPushConstantsSize;
}

VulkanCommandBuffer::VulkanCommandBuffer(VulkanCommandBuffer &&other) noexcept
    : VulkanResourceBase(std::move(other))
    , m_Pool(other.m_Pool)
    , m_Level(std::exchange(other.m_Level, vk::CommandBufferLevel::ePrimary))
    , m_PipelineState(std::move(other.m_PipelineState))
    , m_ResourceSets(std::move(other.m_ResourceSets))
    , m_ResourceBindingDirty(std::exchange(other.m_ResourceBindingDirty, false))
    , m_BoundDescriptorSetLayouts(std::move(other.m_BoundDescriptorSetLayouts))
    , m_StoredPushConstants(std::move(other.m_StoredPushConstants))
    , m_MaxPushConstantsSize(std::exchange(other.m_MaxPushConstantsSize, 128)) {
}

VulkanCommandBuffer::~VulkanCommandBuffer() {
    if (HasHandle()) {
        GetDevice().GetHandle().freeCommandBuffers(m_Pool.GetHandle(), GetHandle());
    }
}

// ============================================================================
// 生命周期
// ============================================================================

void VulkanCommandBuffer::Begin(vk::CommandBufferUsageFlags flags,
                                VulkanCommandBuffer *primary_cmd_buf) {
    ZoneScoped;

    // 重置状态
    m_PipelineState.Reset();
    m_ResourceSets.clear();
    m_ResourceBindingDirty = false;
    m_BoundDescriptorSetLayouts.clear();
    m_StoredPushConstants.clear();

    vk::CommandBufferBeginInfo begin_info{.flags = flags};

    if (m_Level == vk::CommandBufferLevel::eSecondary) {
        assert(primary_cmd_buf && "Secondary command buffer requires a primary command buffer");

        vk::CommandBufferInheritanceInfo inheritance{};
        // 继承 primary 的 render pass 和 framebuffer（若有）
        begin_info.pInheritanceInfo = &inheritance;
    }

    GetHandle().begin(begin_info);
}

void VulkanCommandBuffer::End() {
    ZoneScoped;
    GetHandle().end();
}

void VulkanCommandBuffer::Reset() {
    ZoneScoped;
    m_PipelineState.Reset();
    m_ResourceSets.clear();
    m_ResourceBindingDirty = false;
    m_BoundDescriptorSetLayouts.clear();
    m_StoredPushConstants.clear();
    GetHandle().reset(vk::CommandBufferResetFlagBits::eReleaseResources);
}

// ============================================================================
// 管线绑定
// ============================================================================

void VulkanCommandBuffer::BindPipelineLayout(VulkanPipelineLayout &pipeline_layout) {
    m_PipelineState.pipelineLayout = &pipeline_layout;
}

void VulkanCommandBuffer::BindVertexBuffers(
    uint32_t first_binding,
    std::vector<std::reference_wrapper<const VulkanBuffer>> const &buffers,
    std::vector<vk::DeviceSize> const &offsets) {
    std::vector<vk::Buffer> buffer_handles(buffers.size(), nullptr);
    for (size_t i = 0; i < buffers.size(); ++i) {
        buffer_handles[i] = buffers[i].get().GetHandle();
    }
    GetHandle().bindVertexBuffers(first_binding, buffer_handles, offsets);
}

void VulkanCommandBuffer::BindIndexBuffer(VulkanBuffer const &buffer,
                                          vk::DeviceSize offset,
                                          vk::IndexType index_type) {
    GetHandle().bindIndexBuffer(buffer.GetHandle(), offset, index_type);
}

// ============================================================================
// 资源绑定
// ============================================================================

void VulkanCommandBuffer::BindBuffer(VulkanBuffer const &buffer,
                                     vk::DeviceSize offset,
                                     vk::DeviceSize range,
                                     uint32_t set,
                                     uint32_t binding,
                                     uint32_t array_element) {
    m_ResourceSets[set][binding][array_element] = ResourceInfo{
        .buffer = &buffer,
        .offset = offset,
        .range = range,
        .image_view = nullptr,
        .sampler = nullptr,
    };
    m_ResourceBindingDirty = true;
}

void VulkanCommandBuffer::BindImage(VulkanImageView const &image_view,
                                    VulkanSampler const &sampler,
                                    uint32_t set,
                                    uint32_t binding,
                                    uint32_t array_element) {
    m_ResourceSets[set][binding][array_element] = ResourceInfo{
        .buffer = nullptr,
        .offset = 0,
        .range = 0,
        .image_view = &image_view,
        .sampler = &sampler,
    };
    m_ResourceBindingDirty = true;
}

void VulkanCommandBuffer::BindImage(VulkanImageView const &image_view,
                                    uint32_t set,
                                    uint32_t binding,
                                    uint32_t array_element) {
    m_ResourceSets[set][binding][array_element] = ResourceInfo{
        .buffer = nullptr,
        .offset = 0,
        .range = 0,
        .image_view = &image_view,
        .sampler = nullptr,
    };
    m_ResourceBindingDirty = true;
}

void VulkanCommandBuffer::BindInput(VulkanImageView const &image_view,
                                    uint32_t set,
                                    uint32_t binding,
                                    uint32_t array_element) {
    // input attachment 和 image 绑定使用相同的数据结构
    BindImage(image_view, set, binding, array_element);
}

// ============================================================================
// 管线状态设置（直接通过 GetPipelineState() 访问，无需单独 setter 方法）
// ============================================================================

void VulkanCommandBuffer::SetViewport(uint32_t first_viewport,
                                      std::vector<vk::Viewport> const &viewports) {
    GetHandle().setViewport(first_viewport, viewports);
}

void VulkanCommandBuffer::SetScissor(uint32_t first_scissor,
                                     std::vector<vk::Rect2D> const &scissors) {
    GetHandle().setScissor(first_scissor, scissors);
}

void VulkanCommandBuffer::SetLineWidth(float line_width) {
    GetHandle().setLineWidth(line_width);
}

void VulkanCommandBuffer::SetDepthBias(float depth_bias_constant_factor,
                                       float depth_bias_clamp,
                                       float depth_bias_slope_factor) {
    GetHandle().setDepthBias(depth_bias_constant_factor, depth_bias_clamp, depth_bias_slope_factor);
}

void VulkanCommandBuffer::SetBlendConstants(std::array<float, 4> const &blend_constants) {
    GetHandle().setBlendConstants(blend_constants.data());
}

void VulkanCommandBuffer::SetDepthBounds(float min_depth_bounds, float max_depth_bounds) {
    GetHandle().setDepthBounds(min_depth_bounds, max_depth_bounds);
}

// ============================================================================
// Push Constants
// ============================================================================

void VulkanCommandBuffer::PushConstants(const std::vector<uint8_t> &values) {
    if (m_StoredPushConstants.size() + values.size() > m_MaxPushConstantsSize) {
        assert(false && "Push constants exceed max size");
        return;
    }
    m_StoredPushConstants.insert(m_StoredPushConstants.end(), values.begin(), values.end());
}

// ============================================================================
// 渲染命令
// ============================================================================

void VulkanCommandBuffer::Draw(uint32_t vertex_count, uint32_t instance_count,
                               uint32_t first_vertex, uint32_t first_instance) {
    ZoneScoped;
    Flush(vk::PipelineBindPoint::eGraphics);
    GetHandle().draw(vertex_count, instance_count, first_vertex, first_instance);
}

void VulkanCommandBuffer::DrawIndexed(uint32_t index_count, uint32_t instance_count,
                                      uint32_t first_index, int32_t vertex_offset,
                                      uint32_t first_instance) {
    ZoneScoped;
    Flush(vk::PipelineBindPoint::eGraphics);
    GetHandle().drawIndexed(index_count, instance_count, first_index, vertex_offset, first_instance);
}

void VulkanCommandBuffer::Dispatch(uint32_t group_count_x, uint32_t group_count_y,
                                   uint32_t group_count_z) {
    ZoneScoped;
    Flush(vk::PipelineBindPoint::eCompute);
    GetHandle().dispatch(group_count_x, group_count_y, group_count_z);
}

// ============================================================================
// 屏障 / 拷贝
// ============================================================================

void VulkanCommandBuffer::BufferMemoryBarrier(VulkanBuffer const &buffer,
                                              vk::DeviceSize offset,
                                              vk::DeviceSize size,
                                              GE::BufferMemoryBarrier const &memory_barrier) {
    GetHandle().pipelineBarrier(
        memory_barrier.src_stage_mask,
        memory_barrier.dst_stage_mask,
        {},
        {},
        vk::BufferMemoryBarrier{
            .srcAccessMask = memory_barrier.src_access_mask,
            .dstAccessMask = memory_barrier.dst_access_mask,
            .buffer = buffer.GetHandle(),
            .offset = offset,
            .size = size,
        },
        {});
}

void VulkanCommandBuffer::ImageMemoryBarrier(VulkanImageView const &image_view,
                                             GE::ImageMemoryBarrier const &memory_barrier) const {
    GetHandle().pipelineBarrier(
        memory_barrier.src_stage_mask,
        memory_barrier.dst_stage_mask,
        {},
        {},
        {},
        vk::ImageMemoryBarrier{
            .srcAccessMask = memory_barrier.src_access_mask,
            .dstAccessMask = memory_barrier.dst_access_mask,
            .oldLayout = memory_barrier.old_layout,
            .newLayout = memory_barrier.new_layout,
            .srcQueueFamilyIndex = memory_barrier.src_queue_family,
            .dstQueueFamilyIndex = memory_barrier.dst_queue_family,
            .image = image_view.get_image().GetHandle(),
            .subresourceRange = image_view.get_subresource_range(),
        });
}

void VulkanCommandBuffer::CopyBuffer(VulkanBuffer const &src, VulkanBuffer const &dst,
                                     vk::DeviceSize size) {
    vk::BufferCopy copy_region{.size = size};
    GetHandle().copyBuffer(src.GetHandle(), dst.GetHandle(), copy_region);
}

void VulkanCommandBuffer::CopyBufferToImage(VulkanBuffer const &buffer,
                                            VulkanImage const &image,
                                            std::vector<vk::BufferImageCopy> const &regions) {
    GetHandle().copyBufferToImage(buffer.GetHandle(), image.GetHandle(),
                                  vk::ImageLayout::eTransferDstOptimal, regions);
}

// ============================================================================
// Flush 机制
// ============================================================================

void VulkanCommandBuffer::Flush(vk::PipelineBindPoint pipeline_bind_point) {
    FlushPipelineState(GetDevice(), pipeline_bind_point);
    FlushPushConstants();
    FlushDescriptorState(pipeline_bind_point);
}

void VulkanCommandBuffer::FlushPipelineState(VulkanDevice &device,
                                             vk::PipelineBindPoint pipeline_bind_point) {
    // 检查管线状态是否需要更新
    if (!m_PipelineState.HasPipelineDirty()) {
        // 即使 pipeline 不需要重建，也要刷入动态状态
        if (m_PipelineState.HasDynamicDirty()) {
            m_PipelineState.FlushDynamicStates(GetHandle());
            m_PipelineState.ClearAllDirty();
        }
        return;
    }

    // 管线需要重建或更新
    // 通过 ResourceCache 请求管线
    auto &cache = device.GetResourceCache();

    if (pipeline_bind_point == vk::PipelineBindPoint::eGraphics) {
        auto &pipeline = cache.RequestGraphicsPipeline(m_PipelineState);
        GetHandle().bindPipeline(vk::PipelineBindPoint::eGraphics, pipeline.GetHandle());
    } else if (pipeline_bind_point == vk::PipelineBindPoint::eCompute) {
        auto &pipeline = cache.RequestComputePipeline(m_PipelineState);
        GetHandle().bindPipeline(vk::PipelineBindPoint::eCompute, pipeline.GetHandle());
    }

    // 刷入动态状态
    m_PipelineState.FlushDynamicStates(GetHandle());
    m_PipelineState.ClearAllDirty();
}

void VulkanCommandBuffer::FlushPushConstants() {
    if (m_StoredPushConstants.empty()) {
        return;
    }

    // 获取 pipeline layout
    auto *layout = m_PipelineState.pipelineLayout.Get();
    if (!layout) {
        m_StoredPushConstants.clear();
        return;
    }

    // 查询 push constant 对应的 shader stage
    vk::ShaderStageFlags shader_stage = layout->GetPushConstantRangeStage(
        static_cast<uint32_t>(m_StoredPushConstants.size()));

    if (shader_stage) {
        GetHandle().pushConstants(layout->GetHandle(), shader_stage, 0,
                                  static_cast<uint32_t>(m_StoredPushConstants.size()),
                                  m_StoredPushConstants.data());
    }

    m_StoredPushConstants.clear();
}

void VulkanCommandBuffer::FlushDescriptorState(vk::PipelineBindPoint pipeline_bind_point) {
    // 获取 pipeline layout
    auto *layout = m_PipelineState.pipelineLayout.Get();
    if (!layout) {
        return;
    }

    // 获取 RenderFrame
    auto *render_frame = m_Pool.GetRenderFrame();
    if (!render_frame) {
        return;
    }

    // 获取 shader sets（set index → resources）
    auto const &shader_sets = layout->GetShaderSets();

    // 检测需要更新的 descriptor set
    std::unordered_set<uint32_t> update_descriptor_sets;

    for (auto const &set_it : shader_sets) {
        uint32_t set_index = set_it.first;

        auto bound_it = m_BoundDescriptorSetLayouts.find(set_index);
        if (bound_it != m_BoundDescriptorSetLayouts.end()) {
            // 检查 layout 是否变化
            if (layout->HasDescriptorSetLayout(set_index)) {
                auto &current_layout = layout->GetDescriptorSetLayout(set_index);
                if (bound_it->second != current_layout.GetHandle()) {
                    update_descriptor_sets.insert(set_index);
                }
            }
        }
    }

    // 清理已不存在于当前 pipeline layout 中的绑定记录
    for (auto it = m_BoundDescriptorSetLayouts.begin();
         it != m_BoundDescriptorSetLayouts.end();) {
        if (!layout->HasDescriptorSetLayout(it->first)) {
            it = m_BoundDescriptorSetLayouts.erase(it);
        } else {
            ++it;
        }
    }

    // 如果资源绑定有变化，或有需要更新的 set
    if (!m_ResourceBindingDirty && update_descriptor_sets.empty()) {
        return;
    }

    m_ResourceBindingDirty = false;

    // 遍历所有有绑定的 set
    for (auto &resource_set_it : m_ResourceSets) {
        uint32_t set_index = resource_set_it.first;
        auto &resource_set = resource_set_it.second;

        // 检查此 set 是否需要更新
        if (!layout->HasDescriptorSetLayout(set_index)) {
            continue;
        }

        auto &descriptor_set_layout = layout->GetDescriptorSetLayout(set_index);

        // 更新绑定记录
        m_BoundDescriptorSetLayouts[set_index] = descriptor_set_layout.GetHandle();

        // 构建 BindingMap
        BindingMap<vk::DescriptorBufferInfo> buffer_infos;
        BindingMap<vk::DescriptorImageInfo> image_infos;

        for (auto &binding_it : resource_set) {
            uint32_t binding_index = binding_it.first;

            for (auto &element_it : binding_it.second) {
                uint32_t array_element = element_it.first;
                auto const &resource_info = element_it.second;

                if (resource_info.buffer != nullptr) {
                    // 缓冲区绑定
                    vk::DescriptorBufferInfo buf_info{
                        .buffer = resource_info.buffer->GetHandle(),
                        .offset = resource_info.offset,
                        .range = resource_info.range,
                    };
                    buffer_infos[binding_index][array_element] = buf_info;
                } else if (resource_info.image_view != nullptr) {
                    // 图像绑定
                    vk::DescriptorImageInfo img_info{
                        .sampler = resource_info.sampler ? resource_info.sampler->GetHandle() : nullptr,
                        .imageView = resource_info.image_view->GetHandle(),
                        .imageLayout = vk::ImageLayout::eShaderReadOnlyOptimal,
                    };
                    image_infos[binding_index][array_element] = img_info;
                }
            }
        }

        // 通过 RenderFrame 请求 descriptor set
        auto &descriptor_set = render_frame->RequestDescriptorSet(
            descriptor_set_layout, buffer_infos, image_infos);

        // 绑定 descriptor set
        GetHandle().bindDescriptorSets(
            pipeline_bind_point,
            layout->GetHandle(),
            set_index,
            descriptor_set.GetHandle(),
            {});
    }
}

} // namespace GE