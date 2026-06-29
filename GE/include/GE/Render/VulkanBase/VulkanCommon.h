/* Copyright (c) 2018-2026, Arm Limited and Contributors
 * Copyright (c) 2019-2026, Sascha Willems
 * Copyright (c) 2024-2026, Mobica Limited
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
 * @file VulkanCommon.h
 * @brief Vulkan 通用工具函数集合，从 Vulkan-Samples 适配而来。
 *
 * 提供格式查询、shader 加载、图像布局转换、队列族选择等常用工具。
 */

#pragma once

#include <vulkan/vulkan.hpp>

#include <cstdio>
#include <map>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

#include <vk_mem_alloc.h>

// 自定义宏，用于提高代码可读性
#define VK_FLAGS_NONE 0

// 默认 fence 超时时间（纳秒）
#define DEFAULT_FENCE_TIMEOUT 100000000000

template <class T>
using ShaderStageMap = std::map<vk::ShaderStageFlagBits, T>;

template <class T>
using BindingMap = std::map<uint32_t, std::map<uint32_t, T>>;

namespace GE {

/// 着色器语言类型
enum class ShadingLanguage
{
    GLSL,
    HLSL,
    SLANG,
};

/// 绑定类型
enum class BindingType
{
    C,
    Cpp
};

/// CommandBuffer 重置模式
enum class CommandBufferResetMode
{
    ResetPool,
    ResetIndividually,
    AlwaysAllocate,
};

/// 请求模式
enum class RequestMode
{
    Optional,
    Required
};

/**
 * @brief 判断 VkFormat 是否为纯深度格式。
 * @param format 要检查的 VkFormat。
 * @return 如果是纯深度格式返回 true，否则 false。
 */
bool is_depth_only_format(VkFormat format);

/**
 * @brief 判断 VkFormat 是否为深度+模板格式。
 * @param format 要检查的 VkFormat。
 * @return 如果是深度+模板格式返回 true，否则 false。
 */
bool is_depth_stencil_format(VkFormat format);

/**
 * @brief 判断 VkFormat 是否为深度格式（含深度+模板）。
 * @param format 要检查的 VkFormat。
 * @return 如果是深度格式返回 true，否则 false。
 */
bool is_depth_format(VkFormat format);

/**
 * @brief 按优先级列表选择合适的支持深度格式。
 * @param physical_device 要检查的物理设备。
 * @param depth_only （可选）是否仅选择纯深度格式（不含模板）。
 * @param depth_format_priority_list （可选）深度格式优先级列表，默认从最高精度压缩格式开始。
 * @return 合适的深度格式。
 */
vk::Format get_suitable_depth_format(vk::PhysicalDevice             physical_device,
                                     bool                           depth_only                 = false,
                                     const std::vector<vk::Format> &depth_format_priority_list = {
                                         vk::Format::eD32Sfloat,
                                         vk::Format::eD24UnormS8Uint,
                                         vk::Format::eD16Unorm});

/**
 * @brief 从优先级列表中选取支持 blending 的格式。
 * @param physical_device 要检查的物理设备。
 * @param format_priority_list 按优先级排列的格式列表。
 * @return 选中的格式。
 */
vk::Format choose_blendable_format(vk::PhysicalDevice physical_device, const std::vector<vk::Format> &format_priority_list);

/**
 * @brief 检查线性过滤支持，必要时调整过滤参数。
 * @param physical_device 要检查的物理设备。
 * @param format 要检查的格式。
 * @param filter 要调整的首选过滤器。
 * @param mipmapMode （可选）要调整的首选 mipmap 模式。
 */
void make_filters_valid(vk::PhysicalDevice physical_device, VkFormat format, vk::Filter *filter, vk::SamplerMipmapMode *mipmapMode = nullptr);

/**
 * @brief 判断 Vulkan descriptor 类型是否为动态存储缓冲区或动态统一缓冲区。
 * @param descriptor_type 要检查的 VkDescriptorType。
 * @return 如果是动态缓冲区类型返回 true，否则 false。
 */
bool is_dynamic_buffer_descriptor_type(vk::DescriptorType descriptor_type);

/**
 * @brief 判断 Vulkan descriptor 类型是否为缓冲区类型（统一或存储，动态与否皆可）。
 * @param descriptor_type 要检查的 VkDescriptorType。
 * @return 如果是缓冲区类型返回 true，否则 false。
 */
bool is_buffer_descriptor_type(vk::DescriptorType descriptor_type);

/**
 * @brief 获取 VkFormat 的每像素比特数。
 * @param format 要检查的 VkFormat。
 * @return 每像素比特数，无效格式返回 -1。
 */
int32_t get_bits_per_pixel(VkFormat format);

/**
 * @brief 从 SPIR-V 着色器文件创建 VkShaderModule。
 * @param filename 着色器文件路径。
 * @param device 逻辑设备。
 * @param stage 着色器阶段（当前未使用，仅用于 API 兼容）。
 * @return 包含已加载着色器的 ShaderModule。
 */
vk::ShaderModule load_shader(const std::string &filename, vk::Device device, vk::ShaderStageFlagBits stage);

/**
 * @brief 从 SPIR-V 向量创建 VkShaderModule。
 * @param spirv SPIR-V 代码的向量表示。
 * @param device 逻辑设备。
 * @return 包含已加载着色器的 ShaderModule。
 */
vk::ShaderModule load_shader_from_vector(const std::vector<uint32_t> &spirv, vk::Device device);

/**
 * @brief 选择 VkSurfaceFormatKHR。
 * @param gpu 要选择格式的 VkPhysicalDevice。
 * @param surface 要选择格式的 VkSurfaceKHR。
 * @param preferred_formats 首选 VkFormat 列表。
 * @return 最优的 VkSurfaceFormatKHR。
 */
vk::SurfaceFormatKHR select_surface_format(vk::PhysicalDevice             gpu,
                                           vk::SurfaceKHR                 surface,
                                           std::vector<vk::Format> const &preferred_formats = {
                                               vk::Format::eR8G8B8A8Srgb, vk::Format::eB8G8R8A8Srgb, vk::Format::eA8B8G8R8SrgbPack32});

/**
 * @brief 图像内存屏障结构，用于在命令录制期间定义图像的访问方式。
 */
struct ImageMemoryBarrier
{
    vk::PipelineStageFlags src_stage_mask{vk::PipelineStageFlagBits::eBottomOfPipe};
    vk::PipelineStageFlags dst_stage_mask{vk::PipelineStageFlagBits::eTopOfPipe};
    vk::AccessFlags        src_access_mask{0};
    vk::AccessFlags        dst_access_mask{0};
    vk::ImageLayout        old_layout{vk::ImageLayout::eUndefined};
    vk::ImageLayout        new_layout{vk::ImageLayout::eUndefined};
    uint32_t               src_queue_family{VK_QUEUE_FAMILY_IGNORED};
    uint32_t               dst_queue_family{VK_QUEUE_FAMILY_IGNORED};
};

/**
 * @brief 缓冲区内存屏障结构，用于在命令录制期间定义缓冲区的访问方式。
 */
struct BufferMemoryBarrier
{
    vk::PipelineStageFlags src_stage_mask{vk::PipelineStageFlagBits::eBottomOfPipe};
    vk::PipelineStageFlags dst_stage_mask{vk::PipelineStageFlagBits::eTopOfPipe};
    vk::AccessFlags        src_access_mask{0};
    vk::AccessFlags        dst_access_mask{0};
};

/**
 * @brief 使用显式过渡参数对图像执行布局转换的屏障。
 * @param command_buffer 录制屏障的命令缓冲区。
 * @param image 要转换的 VkImage。
 * @param src_stage_mask 源阶段掩码。
 * @param dst_stage_mask 目标阶段掩码。
 * @param src_access_mask 源访问掩码。
 * @param dst_access_mask 目标访问掩码。
 * @param old_layout 源布局。
 * @param new_layout 目标布局。
 * @param subresource_range 子资源范围。
 */
void image_layout_transition(vk::CommandBuffer            command_buffer,
                             vk::Image                    image,
                             vk::PipelineStageFlags       src_stage_mask,
                             vk::PipelineStageFlags       dst_stage_mask,
                             vk::AccessFlags              src_access_mask,
                             vk::AccessFlags              dst_access_mask,
                             vk::ImageLayout              old_layout,
                             vk::ImageLayout              new_layout,
                             vk::ImageSubresourceRange    subresource_range);

/**
 * @brief 在给定的子资源范围上对图像执行布局转换的屏障。
 *
 * src_stage_mask、dst_stage_mask、src_access_mask 和 dst_access_mask
 * 根据 old_layout 和 new_layout 自动确定。
 *
 * @param command_buffer 录制屏障的命令缓冲区。
 * @param image 要转换的 VkImage。
 * @param old_layout 源布局。
 * @param new_layout 目标布局。
 * @param subresource_range 子资源范围。
 */
void image_layout_transition(vk::CommandBuffer         command_buffer,
                             vk::Image                 image,
                             vk::ImageLayout           old_layout,
                             vk::ImageLayout           new_layout,
                             vk::ImageSubresourceRange subresource_range);

/**
 * @brief 对图像的固定子资源（第一个 mip 级别和 layer）执行布局转换的屏障。
 *
 * src_stage_mask、dst_stage_mask、src_access_mask 和 dst_access_mask
 * 根据 old_layout 和 new_layout 自动确定。
 *
 * @param command_buffer 录制屏障的命令缓冲区。
 * @param image 要转换的 VkImage。
 * @param old_layout 源布局。
 * @param new_layout 目标布局。
 */
void image_layout_transition(vk::CommandBuffer command_buffer,
                             vk::Image         image,
                             vk::ImageLayout   old_layout,
                             vk::ImageLayout   new_layout);

/**
 * @brief 对一组图像执行布局转换的屏障，每张图像附带各自的子资源范围。
 *
 * src_stage_mask、dst_stage_mask、src_access_mask 和 dst_access_mask
 * 根据 old_layout 和 new_layout 自动确定。
 *
 * @param command_buffer 录制屏障的命令缓冲区。
 * @param imagesAndRanges 要转换的图像及其子资源范围对。
 * @param old_layout 源布局。
 * @param new_layout 目标布局。
 */
void image_layout_transition(vk::CommandBuffer                                              command_buffer,
                             std::vector<std::pair<vk::Image, vk::ImageSubresourceRange>> const &imagesAndRanges,
                             vk::ImageLayout                                                old_layout,
                             vk::ImageLayout                                                new_layout);

/**
 * @brief 压缩控制辅助函数
 */
std::vector<vk::ImageCompressionFixedRateFlagBitsEXT> fixed_rate_compression_flags_to_vector(vk::ImageCompressionFixedRateFlagsEXT flags);

vk::ImageCompressionPropertiesEXT query_supported_fixed_rate_compression(vk::PhysicalDevice gpu, const vk::ImageCreateInfo &create_info);

vk::ImageCompressionPropertiesEXT query_applied_compression(vk::Device device, vk::Image image);

/**
 * @brief RenderPass 附件的加载和存储信息。
 */
struct LoadStoreInfo
{
    vk::AttachmentLoadOp  load_op  = vk::AttachmentLoadOp::eClear;
    vk::AttachmentStoreOp store_op = vk::AttachmentStoreOp::eStore;
};

namespace gbuffer
{
/**
 * @return LoadAll、仅 Store swapchain 的 LoadStoreInfo
 */
std::vector<LoadStoreInfo> get_load_all_store_swapchain();

/**
 * @return ClearAll、仅 Store swapchain 的 LoadStoreInfo
 */
std::vector<LoadStoreInfo> get_clear_all_store_swapchain();

/**
 * @return Clear 并 Store 所有图像的 LoadStoreInfo
 */
std::vector<LoadStoreInfo> get_clear_store_all();

/**
 * @return G-buffer 的默认清除值
 */
std::vector<vk::ClearValue> get_clear_value();
}        // namespace gbuffer

/**
 * @brief 获取匹配指定队列标志的队列族索引。
 * @param queue_family_properties 队列族属性列表。
 * @param queue_flag 要匹配的队列标志。
 * @return 匹配的队列族索引。
 */
uint32_t get_queue_family_index(std::vector<vk::QueueFamilyProperties> const &queue_family_properties, vk::QueueFlagBits queue_flag);

}        // namespace GE
