/* Copyright (c) 2022-2025, NVIDIA CORPORATION. All rights reserved.
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

#include "Render/VulkanBase/VulkanImage.h"
#include "Render/VulkanBase/VulkanImageView.h"
#include "Core/Log.h"

#include <algorithm>
#include <stdexcept>

namespace GE
{
namespace
{
inline vk::ImageType find_image_type(vk::Extent3D const &extent)
{
	uint32_t dim_num = !!extent.width + !!extent.height + (1 < extent.depth);
	switch (dim_num)
	{
		case 1:
			return vk::ImageType::e1D;
		case 2:
			return vk::ImageType::e2D;
		case 3:
			return vk::ImageType::e3D;
		default:
			throw std::runtime_error("No image type found.");
			return vk::ImageType();
	}
}
}        // namespace

// ============================================================================
// VulkanImageBuilder
// ============================================================================

VulkanImage VulkanImageBuilder::build(GE::VulkanDevice &device) const
{
	return VulkanImage(device, *this);
}

VulkanImagePtr VulkanImageBuilder::build_unique(GE::VulkanDevice &device) const
{
	return std::make_unique<VulkanImage>(device, *this);
}

// ============================================================================
// VulkanImage — 便捷构造函数（委托给 Builder）
// ============================================================================

VulkanImage::VulkanImage(VulkanDevice   &device,
                               const vk::Extent3D    &extent,
                               vk::Format             format,
                               vk::ImageUsageFlags    image_usage,
                               VmaMemoryUsage         memory_usage,
                               vk::SampleCountFlagBits sample_count,
                               const uint32_t          mip_levels,
                               const uint32_t          array_layers,
                               vk::ImageTiling         tiling,
                               vk::ImageCreateFlags    flags,
                               uint32_t                num_queue_families,
                               const uint32_t         *queue_families) :
    VulkanImage{device,
                   VulkanImageBuilder{extent}
                       .with_format(format)
                       .with_mip_levels(mip_levels)
                       .with_array_layers(array_layers)
                       .with_sample_count(sample_count)
                       .with_tiling(tiling)
                       .with_flags(flags)
                       .with_usage(image_usage)
                       .with_queue_families(num_queue_families, queue_families)}
{}

// ============================================================================
// VulkanImage — Builder 构造函数
// ============================================================================

VulkanImage::VulkanImage(VulkanDevice &device, VulkanImageBuilder const &builder) :
    allocated::Allocated<vk::Image>{builder.get_allocation_create_info(), &device},
    create_info{builder.get_create_info()}
{
	GetHandle() = create_image(create_info);
	subresource.arrayLayer = create_info.arrayLayers;
	subresource.mipLevel   = create_info.mipLevels;
	if (!builder.get_debug_name().empty())
	{
		SetDebugName(builder.get_debug_name());
	}
}

// ============================================================================
// VulkanImage — 包装已有句柄
// ============================================================================

VulkanImage::VulkanImage(VulkanDevice   &device,
                               vk::Image               handle,
                               const vk::Extent3D     &extent,
                               vk::Format              format,
                               vk::ImageUsageFlags     image_usage,
                               vk::SampleCountFlagBits sample_count) :
    allocated::Allocated<vk::Image>{handle, &device}
{
	create_info.usage      = image_usage;
	create_info.samples     = sample_count;
	create_info.format      = format;
	create_info.extent      = extent;
	create_info.imageType   = find_image_type(extent);
	create_info.arrayLayers = 1;
	create_info.mipLevels   = 1;
	subresource.mipLevel    = 1;
	subresource.arrayLayer  = 1;
}

// ============================================================================
// VulkanImage — 移动构造
// ============================================================================

VulkanImage::VulkanImage(VulkanImage &&other) noexcept :
    allocated::Allocated<vk::Image>{std::move(other)},
    create_info(std::exchange(other.create_info, {})),
    subresource(std::exchange(other.subresource, {})),
    views(std::exchange(other.views, {}))
{
	// 更新所有引用了此 Image 的 View，避免悬空指针
	for (auto &view : views)
	{
		view->set_image(*this);
	}
}

// ============================================================================
// VulkanImage — 析构
// ============================================================================

VulkanImage::~VulkanImage()
{
	destroy_image(GetHandle());
}

// ============================================================================
// VulkanImage — 方法
// ============================================================================

uint8_t *VulkanImage::map()
{
	if (create_info.tiling != vk::ImageTiling::eLinear)
	{
		GE_CORE_WARN("Mapping image memory that is not linear");
	}
	return allocated::Allocated<vk::Image>::map();
}

vk::ImageType VulkanImage::get_type() const
{
	return create_info.imageType;
}

const vk::Extent3D &VulkanImage::get_extent() const
{
	return create_info.extent;
}

vk::Format VulkanImage::get_format() const
{
	return create_info.format;
}

vk::SampleCountFlagBits VulkanImage::get_sample_count() const
{
	return create_info.samples;
}

vk::ImageUsageFlags VulkanImage::get_usage() const
{
	return create_info.usage;
}

vk::ImageTiling VulkanImage::get_tiling() const
{
	return create_info.tiling;
}

vk::ImageSubresource VulkanImage::get_subresource() const
{
	return subresource;
}

uint32_t VulkanImage::get_array_layer_count() const
{
	return create_info.arrayLayers;
}

std::unordered_set<VulkanImageView *> &VulkanImage::get_views()
{
	return views;
}

// ============================================================================
// 工具函数实现
// ============================================================================

namespace image_utils
{
vk::ImageView CreateView(vk::Device device, vk::Image image,
                         vk::ImageViewType type, vk::Format format,
                         vk::ImageAspectFlags aspect)
{
	vk::ImageViewCreateInfo view_info{
	    .image            = image,
	    .viewType         = type,
	    .format           = format,
	    .subresourceRange = {.aspectMask = aspect, .baseMipLevel = 0, .levelCount = 1, .baseArrayLayer = 0, .layerCount = 1},
	};
	return device.createImageView(view_info);
}

void TransitionLayout(vk::CommandBuffer cmd, vk::Image image,
                      vk::ImageLayout old_layout, vk::ImageLayout new_layout,
                      uint32_t baseMipLevel, uint32_t levelCount,
                      uint32_t baseArrayLayer, uint32_t layerCount)
{
	struct Transition
	{
		vk::ImageLayout       old_layout;
		vk::ImageLayout       new_layout;
		vk::PipelineStageFlags2 src_stage;
		vk::AccessFlags2        src_access;
		vk::PipelineStageFlags2 dst_stage;
		vk::AccessFlags2        dst_access;
	};

	static const Transition kTransitions[] = {
	    // Undefined → TransferDst:  新 image，准备收 staging 拷贝
	    {vk::ImageLayout::eUndefined, vk::ImageLayout::eTransferDstOptimal,
	     vk::PipelineStageFlagBits2::eTopOfPipe, vk::AccessFlagBits2::eNone,
	     vk::PipelineStageFlagBits2::eTransfer, vk::AccessFlagBits2::eTransferWrite},

	    // TransferDst → TransferSrc:  mip 生成前，准备作为 blit 源
	    {vk::ImageLayout::eTransferDstOptimal, vk::ImageLayout::eTransferSrcOptimal,
	     vk::PipelineStageFlagBits2::eTransfer, vk::AccessFlagBits2::eTransferWrite,
	     vk::PipelineStageFlagBits2::eTransfer, vk::AccessFlagBits2::eTransferRead},

	    // TransferSrc → ShaderReadOnly:  mip 生成完成，转给着色器采样
	    {vk::ImageLayout::eTransferSrcOptimal, vk::ImageLayout::eShaderReadOnlyOptimal,
	     vk::PipelineStageFlagBits2::eTransfer, vk::AccessFlagBits2::eTransferRead,
	     vk::PipelineStageFlagBits2::eFragmentShader, vk::AccessFlagBits2::eShaderRead},

	    // TransferDst → ShaderReadOnly:  staging 完成，准备给着色器采样
	    {vk::ImageLayout::eTransferDstOptimal, vk::ImageLayout::eShaderReadOnlyOptimal,
	     vk::PipelineStageFlagBits2::eTransfer, vk::AccessFlagBits2::eTransferWrite,
	     vk::PipelineStageFlagBits2::eFragmentShader, vk::AccessFlagBits2::eShaderRead},

	    // ShaderReadOnly → TransferDst:  已采样的纹理重新作为传输目标（如重新生成 mip）
	    {vk::ImageLayout::eShaderReadOnlyOptimal, vk::ImageLayout::eTransferDstOptimal,
	     vk::PipelineStageFlagBits2::eFragmentShader, vk::AccessFlagBits2::eShaderRead,
	     vk::PipelineStageFlagBits2::eTransfer, vk::AccessFlagBits2::eTransferWrite},

	    // Undefined → ColorAttachment:  新 swapchain image，准备渲染
	    {vk::ImageLayout::eUndefined, vk::ImageLayout::eColorAttachmentOptimal,
	     vk::PipelineStageFlagBits2::eTopOfPipe, vk::AccessFlagBits2::eNone,
	     vk::PipelineStageFlagBits2::eColorAttachmentOutput, vk::AccessFlagBits2::eColorAttachmentWrite},

	    // Undefined → General:  新离屏颜色图，通用布局（既当颜色附件渲染、也可被采样）
	    {vk::ImageLayout::eUndefined, vk::ImageLayout::eGeneral,
	     vk::PipelineStageFlagBits2::eTopOfPipe, vk::AccessFlagBits2::eNone,
	     vk::PipelineStageFlagBits2::eAllCommands, vk::AccessFlagBits2::eMemoryRead | vk::AccessFlagBits2::eMemoryWrite},

	    // ColorAttachment → PresentSrc:  渲染完成，准备呈现
	    {vk::ImageLayout::eColorAttachmentOptimal, vk::ImageLayout::ePresentSrcKHR,
	     vk::PipelineStageFlagBits2::eColorAttachmentOutput, vk::AccessFlagBits2::eColorAttachmentWrite,
	     vk::PipelineStageFlagBits2::eBottomOfPipe, vk::AccessFlagBits2::eNone},

	    // Undefined → DepthStencilAttachment:  新 depth image，准备渲染深度
	    {vk::ImageLayout::eUndefined, vk::ImageLayout::eDepthStencilAttachmentOptimal,
	     vk::PipelineStageFlagBits2::eTopOfPipe, vk::AccessFlagBits2::eNone,
	     vk::PipelineStageFlagBits2::eEarlyFragmentTests, vk::AccessFlagBits2::eDepthStencilAttachmentWrite},
	};

	auto it = std::ranges::find_if(kTransitions, [&](auto const &t) {
		return t.old_layout == old_layout && t.new_layout == new_layout;
	});
	if (it == std::end(kTransitions))
	{
		throw std::runtime_error("Unsupported layout transition");
	}

	// 根据目标 layout 选择 aspect mask
	vk::ImageAspectFlags aspectMask = vk::ImageAspectFlagBits::eColor;
	if (new_layout == vk::ImageLayout::eDepthStencilAttachmentOptimal)
	{
		aspectMask = vk::ImageAspectFlagBits::eDepth;
	}

	vk::ImageMemoryBarrier2 barrier{
	    .srcStageMask     = it->src_stage,
	    .srcAccessMask    = it->src_access,
	    .dstStageMask     = it->dst_stage,
	    .dstAccessMask    = it->dst_access,
	    .oldLayout        = old_layout,
	    .newLayout        = new_layout,
	    .image            = image,
	    .subresourceRange = {.aspectMask     = aspectMask,
	                         .baseMipLevel   = baseMipLevel,
	                         .levelCount     = levelCount,
	                         .baseArrayLayer = baseArrayLayer,
	                         .layerCount     = layerCount},
	};

	vk::DependencyInfo dep_info{
	    .imageMemoryBarrierCount = 1,
	    .pImageMemoryBarriers   = &barrier,
	};
	cmd.pipelineBarrier2(dep_info);
}
}        // namespace image_utils
}        // namespace GE
