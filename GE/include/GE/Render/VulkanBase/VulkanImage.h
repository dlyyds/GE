/* Copyright (c) 2021-2026, NVIDIA CORPORATION. All rights reserved.
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
 * @file VulkanImage.h
 * @brief 从 Vulkan-Samples 适配的 VulkanImage（VMA 管理 Image）+ Builder + 工具函数。
 *
 * 替换早期基于 C API 的 VulkanImage 类，提供：
 * - VulkanImageBuilder：Builder 模式创建 Image
 * - VulkanImage：RAII 风格的 VMA 托管 vk::Image
 * - image_utils 命名空间下的工具函数（TransitionLayout、CreateView 等）
 */

#pragma once

#include "Render/VulkanBase/BuilderBase.h"
#include "Render/VulkanBase/VulkanAllocated.h"

#include <vulkan/vulkan.hpp>
#include <vk_mem_alloc.h>

#include <unordered_set>
#include <cstdint>

namespace GE
{
// 前向声明
class VulkanImageView;

using VulkanImagePtr = std::unique_ptr<class VulkanImage>;

// ============================================================================
// VulkanImageBuilder — Builder 模式创建 VulkanImage
// ============================================================================

class VulkanImageBuilder : public allocated::BuilderBase<VulkanImageBuilder, vk::ImageCreateInfo>
{
  private:
	using Parent = allocated::BuilderBase<VulkanImageBuilder, vk::ImageCreateInfo>;

  public:
	VulkanImageBuilder(vk::Extent3D const &extent) :
	    Parent(vk::ImageCreateInfo{.imageType = vk::ImageType::e2D, .format = vk::Format::eR8G8B8A8Unorm, .extent = extent, .mipLevels = 1, .arrayLayers = 1})
	{
	}

	VulkanImageBuilder(vk::Extent2D const &extent) :
	    VulkanImageBuilder(vk::Extent3D{extent.width, extent.height, 1})
	{
	}

	VulkanImageBuilder(uint32_t width, uint32_t height = 1, uint32_t depth = 1) :
	    VulkanImageBuilder(vk::Extent3D{width, height, depth})
	{
	}

	VulkanImageBuilder &with_format(vk::Format format)
	{
		create_info.format = format;
		return *this;
	}

	VulkanImageBuilder &with_image_type(vk::ImageType type)
	{
		create_info.imageType = type;
		return *this;
	}

	VulkanImageBuilder &with_array_layers(uint32_t layers)
	{
		create_info.arrayLayers = layers;
		return *this;
	}

	VulkanImageBuilder &with_mip_levels(uint32_t levels)
	{
		create_info.mipLevels = levels;
		return *this;
	}

	VulkanImageBuilder &with_sample_count(vk::SampleCountFlagBits sample_count)
	{
		create_info.samples = sample_count;
		return *this;
	}

	VulkanImageBuilder &with_tiling(vk::ImageTiling tiling)
	{
		create_info.tiling = tiling;
		return *this;
	}

	VulkanImageBuilder &with_usage(vk::ImageUsageFlags usage)
	{
		create_info.usage = usage;
		return *this;
	}

	VulkanImageBuilder &with_flags(vk::ImageCreateFlags flags)
	{
		create_info.flags = flags;
		return *this;
	}

	VulkanImage   build(GE::VulkanDevice &device) const;
	VulkanImagePtr build_unique(GE::VulkanDevice &device) const;
};

// ============================================================================
// VulkanImage — RAII VMA 托管的 VkImage
// ============================================================================

class VulkanImage : public allocated::Allocated<vk::Image>
{
  public:
	/// 包装已有句柄（如 swapchain image）
	VulkanImage(VulkanDevice          &device,
	               vk::Image              handle,
	               const vk::Extent3D    &extent,
	               vk::Format             format,
	               vk::ImageUsageFlags    image_usage,
	               vk::SampleCountFlagBits sample_count = vk::SampleCountFlagBits::e1);

	/// 通过 Builder 创建新 Image
	VulkanImage(VulkanDevice &device, VulkanImageBuilder const &builder);

	/// 便捷构造函数：直接参数创建新 Image（推荐使用 Builder）
	VulkanImage(VulkanDevice   &device,
	               const vk::Extent3D    &extent,
	               vk::Format             format,
	               vk::ImageUsageFlags    image_usage,
	               VmaMemoryUsage         memory_usage       = VMA_MEMORY_USAGE_AUTO,
	               vk::SampleCountFlagBits sample_count       = vk::SampleCountFlagBits::e1,
	               uint32_t               mip_levels         = 1,
	               uint32_t               array_layers       = 1,
	               vk::ImageTiling        tiling             = vk::ImageTiling::eOptimal,
	               vk::ImageCreateFlags   flags              = {},
	               uint32_t               num_queue_families = 0,
	               const uint32_t        *queue_families     = nullptr);

	VulkanImage(const VulkanImage &) = delete;

	VulkanImage(VulkanImage &&other) noexcept;

	~VulkanImage();

	VulkanImage &operator=(const VulkanImage &) = delete;

	VulkanImage &operator=(VulkanImage &&) = delete;

	/** @brief 映射内存到 host-visible 地址 */
	uint8_t *map();

	vk::ImageType               get_type() const;
	const vk::Extent3D         &get_extent() const;
	vk::Format                  get_format() const;
	vk::SampleCountFlagBits     get_sample_count() const;
	vk::ImageUsageFlags         get_usage() const;
	vk::ImageTiling             get_tiling() const;
	vk::ImageSubresource        get_subresource() const;
	uint32_t                    get_array_layer_count() const;
	std::unordered_set<VulkanImageView *> &get_views();

  private:
	vk::ImageCreateInfo                           create_info;
	vk::ImageSubresource                          subresource;
	std::unordered_set<VulkanImageView *>      views;        ///< 引用此 Image 的 View
};

// ============================================================================
// 工具函数（替换旧 VulkanImage 的静态方法）
// ============================================================================

namespace image_utils
{
/**
 * @brief 创建 ImageView（简易版，用于 swapchain image 等无需 VulkanImageView 包装的场景）。
 */
vk::ImageView CreateView(vk::Device device, vk::Image image,
                         vk::ImageViewType type, vk::Format format,
                         vk::ImageAspectFlags aspect = vk::ImageAspectFlagBits::eColor);

/**
 * @brief 图像布局转换（使用 VK_KHR_synchronization2 PipelineBarrier2）。
 * @param cmd           命令缓冲区
 * @param image         要转换的图像
 * @param old_layout    源布局
 * @param new_layout    目标布局
 * @param baseMipLevel  起始 mip level（默认 0）
 * @param levelCount    mip level 数量（默认 1）
 */
void TransitionLayout(vk::CommandBuffer cmd, vk::Image image,
                      vk::ImageLayout old_layout, vk::ImageLayout new_layout,
                      uint32_t baseMipLevel = 0, uint32_t levelCount = 1);

}        // namespace image_utils
}        // namespace GE
