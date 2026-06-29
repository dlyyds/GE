/* Copyright (c) 2018-2025, Arm Limited and Contributors
 * Copyright (c) 2019-2025, Sascha Willems
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

#define VULKAN_HPP_DISPATCH_LOADER_DYNAMIC 1
#include "Render/VulkanBase/VulkanCommon.h"

#include "FileSystem/FileSystem.h"
#include "Core/Log.h"

#include <cassert>
#include <stdexcept>

std::ostream &operator<<(std::ostream &os, const VkResult result)
{
#define WRITE_VK_ENUM(r) \
	case VK_##r:         \
		os << #r;        \
		break;

	switch (result)
	{
		WRITE_VK_ENUM(NOT_READY);
		WRITE_VK_ENUM(TIMEOUT);
		WRITE_VK_ENUM(EVENT_SET);
		WRITE_VK_ENUM(EVENT_RESET);
		WRITE_VK_ENUM(INCOMPLETE);
		WRITE_VK_ENUM(ERROR_OUT_OF_HOST_MEMORY);
		WRITE_VK_ENUM(ERROR_OUT_OF_DEVICE_MEMORY);
		WRITE_VK_ENUM(ERROR_INITIALIZATION_FAILED);
		WRITE_VK_ENUM(ERROR_DEVICE_LOST);
		WRITE_VK_ENUM(ERROR_MEMORY_MAP_FAILED);
		WRITE_VK_ENUM(ERROR_LAYER_NOT_PRESENT);
		WRITE_VK_ENUM(ERROR_EXTENSION_NOT_PRESENT);
		WRITE_VK_ENUM(ERROR_FEATURE_NOT_PRESENT);
		WRITE_VK_ENUM(ERROR_INCOMPATIBLE_DRIVER);
		WRITE_VK_ENUM(ERROR_TOO_MANY_OBJECTS);
		WRITE_VK_ENUM(ERROR_FORMAT_NOT_SUPPORTED);
		WRITE_VK_ENUM(ERROR_SURFACE_LOST_KHR);
		WRITE_VK_ENUM(ERROR_NATIVE_WINDOW_IN_USE_KHR);
		WRITE_VK_ENUM(SUBOPTIMAL_KHR);
		WRITE_VK_ENUM(ERROR_OUT_OF_DATE_KHR);
		WRITE_VK_ENUM(ERROR_INCOMPATIBLE_DISPLAY_KHR);
		WRITE_VK_ENUM(ERROR_VALIDATION_FAILED_EXT);
		WRITE_VK_ENUM(ERROR_INVALID_SHADER_NV);
		default:
			os << "UNKNOWN_ERROR";
	}

#undef WRITE_VK_ENUM

	return os;
}

namespace GE
{
bool is_depth_only_format(VkFormat format)
{
	return format == VK_FORMAT_D16_UNORM ||
	       format == VK_FORMAT_D32_SFLOAT;
}

bool is_depth_stencil_format(VkFormat format)
{
	return format == VK_FORMAT_D16_UNORM_S8_UINT ||
	       format == VK_FORMAT_D24_UNORM_S8_UINT ||
	       format == VK_FORMAT_D32_SFLOAT_S8_UINT;
}

bool is_depth_format(VkFormat format)
{
	return is_depth_only_format(format) || is_depth_stencil_format(format);
}

vk::Format get_suitable_depth_format(vk::PhysicalDevice physical_device, bool depth_only, const std::vector<vk::Format> &depth_format_priority_list)
{
	vk::Format depth_format{vk::Format::eUndefined};

	for (auto &format : depth_format_priority_list)
	{
		if (depth_only && !is_depth_only_format(static_cast<VkFormat>(format)))
		{
			continue;
		}

		// 检查物理设备是否支持该深度格式
		vk::FormatProperties properties = physical_device.getFormatProperties(format);

		// 格式必须在 optimal tiling 下支持深度模板附件
		if (properties.optimalTilingFeatures & vk::FormatFeatureFlagBits::eDepthStencilAttachment)
		{
			depth_format = format;
			break;
		}
	}

	if (depth_format != vk::Format::eUndefined)
	{
		GE_CORE_INFO("Depth format selected: {}", vk::to_string(depth_format));
		return depth_format;
	}

	throw std::runtime_error("No suitable depth format could be determined");
}

vk::Format choose_blendable_format(vk::PhysicalDevice physical_device, const std::vector<vk::Format> &format_priority_list)
{
	for (const auto &format : format_priority_list)
	{
		vk::FormatProperties properties = physical_device.getFormatProperties(format);
		if (properties.optimalTilingFeatures & vk::FormatFeatureFlagBits::eColorAttachmentBlend)
		{
			return format;
		}
	}

	throw std::runtime_error("No suitable blendable format could be determined");
}

void make_filters_valid(vk::PhysicalDevice physical_device, VkFormat format, vk::Filter *filter, vk::SamplerMipmapMode *mipmapMode)
{
	// 并非所有格式都支持线性过滤，如果不支持则需要调整参数
	if (*filter == vk::Filter::eNearest && (mipmapMode == nullptr || *mipmapMode == vk::SamplerMipmapMode::eNearest))
	{
		return;        // 这两个参数必然有效
	}

	vk::FormatProperties properties = physical_device.getFormatProperties(static_cast<vk::Format>(format));

	if (!(properties.optimalTilingFeatures & vk::FormatFeatureFlagBits::eSampledImageFilterLinear))
	{
		*filter = vk::Filter::eNearest;
		if (mipmapMode)
		{
			*mipmapMode = vk::SamplerMipmapMode::eNearest;
		}
	}
}

bool is_dynamic_buffer_descriptor_type(vk::DescriptorType descriptor_type)
{
	return descriptor_type == vk::DescriptorType::eStorageBufferDynamic ||
	       descriptor_type == vk::DescriptorType::eUniformBufferDynamic;
}

bool is_buffer_descriptor_type(vk::DescriptorType descriptor_type)
{
	return descriptor_type == vk::DescriptorType::eStorageBuffer ||
	       descriptor_type == vk::DescriptorType::eUniformBuffer ||
	       is_dynamic_buffer_descriptor_type(descriptor_type);
}

int32_t get_bits_per_pixel(VkFormat format)
{
	switch (format)
	{
		case VK_FORMAT_R4G4_UNORM_PACK8:
			return 8;
		case VK_FORMAT_R4G4B4A4_UNORM_PACK16:
		case VK_FORMAT_B4G4R4A4_UNORM_PACK16:
		case VK_FORMAT_R5G6B5_UNORM_PACK16:
		case VK_FORMAT_B5G6R5_UNORM_PACK16:
		case VK_FORMAT_R5G5B5A1_UNORM_PACK16:
		case VK_FORMAT_B5G5R5A1_UNORM_PACK16:
		case VK_FORMAT_A1R5G5B5_UNORM_PACK16:
			return 16;
		case VK_FORMAT_R8_UNORM:
		case VK_FORMAT_R8_SNORM:
		case VK_FORMAT_R8_USCALED:
		case VK_FORMAT_R8_SSCALED:
		case VK_FORMAT_R8_UINT:
		case VK_FORMAT_R8_SINT:
		case VK_FORMAT_R8_SRGB:
			return 8;
		case VK_FORMAT_R8G8_UNORM:
		case VK_FORMAT_R8G8_SNORM:
		case VK_FORMAT_R8G8_USCALED:
		case VK_FORMAT_R8G8_SSCALED:
		case VK_FORMAT_R8G8_UINT:
		case VK_FORMAT_R8G8_SINT:
		case VK_FORMAT_R8G8_SRGB:
			return 16;
		case VK_FORMAT_R8G8B8_UNORM:
		case VK_FORMAT_R8G8B8_SNORM:
		case VK_FORMAT_R8G8B8_USCALED:
		case VK_FORMAT_R8G8B8_SSCALED:
		case VK_FORMAT_R8G8B8_UINT:
		case VK_FORMAT_R8G8B8_SINT:
		case VK_FORMAT_R8G8B8_SRGB:
		case VK_FORMAT_B8G8R8_UNORM:
		case VK_FORMAT_B8G8R8_SNORM:
		case VK_FORMAT_B8G8R8_USCALED:
		case VK_FORMAT_B8G8R8_SSCALED:
		case VK_FORMAT_B8G8R8_UINT:
		case VK_FORMAT_B8G8R8_SINT:
		case VK_FORMAT_B8G8R8_SRGB:
			return 24;
		case VK_FORMAT_R8G8B8A8_UNORM:
		case VK_FORMAT_R8G8B8A8_SNORM:
		case VK_FORMAT_R8G8B8A8_USCALED:
		case VK_FORMAT_R8G8B8A8_SSCALED:
		case VK_FORMAT_R8G8B8A8_UINT:
		case VK_FORMAT_R8G8B8A8_SINT:
		case VK_FORMAT_R8G8B8A8_SRGB:
		case VK_FORMAT_B8G8R8A8_UNORM:
		case VK_FORMAT_B8G8R8A8_SNORM:
		case VK_FORMAT_B8G8R8A8_USCALED:
		case VK_FORMAT_B8G8R8A8_SSCALED:
		case VK_FORMAT_B8G8R8A8_UINT:
		case VK_FORMAT_B8G8R8A8_SINT:
		case VK_FORMAT_B8G8R8A8_SRGB:
		case VK_FORMAT_A8B8G8R8_UNORM_PACK32:
		case VK_FORMAT_A8B8G8R8_SNORM_PACK32:
		case VK_FORMAT_A8B8G8R8_USCALED_PACK32:
		case VK_FORMAT_A8B8G8R8_SSCALED_PACK32:
		case VK_FORMAT_A8B8G8R8_UINT_PACK32:
		case VK_FORMAT_A8B8G8R8_SINT_PACK32:
		case VK_FORMAT_A8B8G8R8_SRGB_PACK32:
			return 32;
		case VK_FORMAT_A2R10G10B10_UNORM_PACK32:
		case VK_FORMAT_A2R10G10B10_SNORM_PACK32:
		case VK_FORMAT_A2R10G10B10_USCALED_PACK32:
		case VK_FORMAT_A2R10G10B10_SSCALED_PACK32:
		case VK_FORMAT_A2R10G10B10_UINT_PACK32:
		case VK_FORMAT_A2R10G10B10_SINT_PACK32:
		case VK_FORMAT_A2B10G10R10_UNORM_PACK32:
		case VK_FORMAT_A2B10G10R10_SNORM_PACK32:
		case VK_FORMAT_A2B10G10R10_USCALED_PACK32:
		case VK_FORMAT_A2B10G10R10_SSCALED_PACK32:
		case VK_FORMAT_A2B10G10R10_UINT_PACK32:
		case VK_FORMAT_A2B10G10R10_SINT_PACK32:
			return 32;
		case VK_FORMAT_R16_UNORM:
		case VK_FORMAT_R16_SNORM:
		case VK_FORMAT_R16_USCALED:
		case VK_FORMAT_R16_SSCALED:
		case VK_FORMAT_R16_UINT:
		case VK_FORMAT_R16_SINT:
		case VK_FORMAT_R16_SFLOAT:
			return 16;
		case VK_FORMAT_R16G16_UNORM:
		case VK_FORMAT_R16G16_SNORM:
		case VK_FORMAT_R16G16_USCALED:
		case VK_FORMAT_R16G16_SSCALED:
		case VK_FORMAT_R16G16_UINT:
		case VK_FORMAT_R16G16_SINT:
		case VK_FORMAT_R16G16_SFLOAT:
			return 32;
		case VK_FORMAT_R16G16B16_UNORM:
		case VK_FORMAT_R16G16B16_SNORM:
		case VK_FORMAT_R16G16B16_USCALED:
		case VK_FORMAT_R16G16B16_SSCALED:
		case VK_FORMAT_R16G16B16_UINT:
		case VK_FORMAT_R16G16B16_SINT:
		case VK_FORMAT_R16G16B16_SFLOAT:
			return 48;
		case VK_FORMAT_R16G16B16A16_UNORM:
		case VK_FORMAT_R16G16B16A16_SNORM:
		case VK_FORMAT_R16G16B16A16_USCALED:
		case VK_FORMAT_R16G16B16A16_SSCALED:
		case VK_FORMAT_R16G16B16A16_UINT:
		case VK_FORMAT_R16G16B16A16_SINT:
		case VK_FORMAT_R16G16B16A16_SFLOAT:
			return 64;
		case VK_FORMAT_R32_UINT:
		case VK_FORMAT_R32_SINT:
		case VK_FORMAT_R32_SFLOAT:
			return 32;
		case VK_FORMAT_R32G32_UINT:
		case VK_FORMAT_R32G32_SINT:
		case VK_FORMAT_R32G32_SFLOAT:
			return 64;
		case VK_FORMAT_R32G32B32_UINT:
		case VK_FORMAT_R32G32B32_SINT:
		case VK_FORMAT_R32G32B32_SFLOAT:
			return 96;
		case VK_FORMAT_R32G32B32A32_UINT:
		case VK_FORMAT_R32G32B32A32_SINT:
		case VK_FORMAT_R32G32B32A32_SFLOAT:
			return 128;
		case VK_FORMAT_R64_UINT:
		case VK_FORMAT_R64_SINT:
		case VK_FORMAT_R64_SFLOAT:
			return 64;
		case VK_FORMAT_R64G64_UINT:
		case VK_FORMAT_R64G64_SINT:
		case VK_FORMAT_R64G64_SFLOAT:
			return 128;
		case VK_FORMAT_R64G64B64_UINT:
		case VK_FORMAT_R64G64B64_SINT:
		case VK_FORMAT_R64G64B64_SFLOAT:
			return 192;
		case VK_FORMAT_R64G64B64A64_UINT:
		case VK_FORMAT_R64G64B64A64_SINT:
		case VK_FORMAT_R64G64B64A64_SFLOAT:
			return 256;
		case VK_FORMAT_B10G11R11_UFLOAT_PACK32:
			return 32;
		case VK_FORMAT_E5B9G9R9_UFLOAT_PACK32:
			return 32;
		case VK_FORMAT_D16_UNORM:
			return 16;
		case VK_FORMAT_X8_D24_UNORM_PACK32:
			return 32;
		case VK_FORMAT_D32_SFLOAT:
			return 32;
		case VK_FORMAT_S8_UINT:
			return 8;
		case VK_FORMAT_D16_UNORM_S8_UINT:
			return 24;
		case VK_FORMAT_D24_UNORM_S8_UINT:
			return 32;
		case VK_FORMAT_D32_SFLOAT_S8_UINT:
			return 40;
		case VK_FORMAT_UNDEFINED:
		default:
			return -1;
	}
}

vk::ShaderModule load_shader(const std::string &filename, vk::Device device, vk::ShaderStageFlagBits stage)
{
	auto spirv = FileSystem::ReadBinaryU32(filename);
	return load_shader_from_vector(spirv, device);
}

vk::ShaderModule load_shader_from_vector(const std::vector<uint32_t> &spirv, vk::Device device)
{
	assert(spirv.size() != 0);

	vk::ShaderModuleCreateInfo module_create_info;
	module_create_info.codeSize = spirv.size() * sizeof(uint32_t);
	module_create_info.pCode    = spirv.data();

	return device.createShaderModule(module_create_info);
}

namespace
{

/**
 * @brief 根据布局获取对应的访问掩码。
 */
vk::AccessFlags getAccessFlags(vk::ImageLayout layout)
{
	switch (layout)
	{
		case vk::ImageLayout::eUndefined:
		case vk::ImageLayout::ePresentSrcKHR:
			return {};
		case vk::ImageLayout::ePreinitialized:
			return vk::AccessFlagBits::eHostWrite;
		case vk::ImageLayout::eColorAttachmentOptimal:
			return vk::AccessFlagBits::eColorAttachmentRead | vk::AccessFlagBits::eColorAttachmentWrite;
		case vk::ImageLayout::eDepthAttachmentOptimal:
			return vk::AccessFlagBits::eDepthStencilAttachmentRead | vk::AccessFlagBits::eDepthStencilAttachmentWrite;
		case vk::ImageLayout::eFragmentShadingRateAttachmentOptimalKHR:
			return vk::AccessFlagBits::eFragmentShadingRateAttachmentReadKHR;
		case vk::ImageLayout::eShaderReadOnlyOptimal:
			return vk::AccessFlagBits::eShaderRead | vk::AccessFlagBits::eInputAttachmentRead;
		case vk::ImageLayout::eTransferSrcOptimal:
			return vk::AccessFlagBits::eTransferRead;
		case vk::ImageLayout::eTransferDstOptimal:
			return vk::AccessFlagBits::eTransferWrite;
		case vk::ImageLayout::eGeneral:
			assert(false && "Don't know how to get a meaningful VkAccessFlags for VK_IMAGE_LAYOUT_GENERAL! Don't use it!");
			return {};
		default:
			assert(false);
			return {};
	}
}

/**
 * @brief 根据布局获取对应的管线阶段掩码。
 */
vk::PipelineStageFlags getPipelineStageFlags(vk::ImageLayout layout)
{
	switch (layout)
	{
		case vk::ImageLayout::eUndefined:
			return vk::PipelineStageFlagBits::eTopOfPipe;
		case vk::ImageLayout::ePreinitialized:
			return vk::PipelineStageFlagBits::eHost;
		case vk::ImageLayout::eTransferDstOptimal:
		case vk::ImageLayout::eTransferSrcOptimal:
			return vk::PipelineStageFlagBits::eTransfer;
		case vk::ImageLayout::eColorAttachmentOptimal:
			return vk::PipelineStageFlagBits::eColorAttachmentOutput;
		case vk::ImageLayout::eDepthAttachmentOptimal:
			return vk::PipelineStageFlagBits::eEarlyFragmentTests | vk::PipelineStageFlagBits::eLateFragmentTests;
		case vk::ImageLayout::eFragmentShadingRateAttachmentOptimalKHR:
			return vk::PipelineStageFlagBits::eFragmentShadingRateAttachmentKHR;
		case vk::ImageLayout::eShaderReadOnlyOptimal:
			return vk::PipelineStageFlagBits::eVertexShader | vk::PipelineStageFlagBits::eFragmentShader;
		case vk::ImageLayout::ePresentSrcKHR:
			return vk::PipelineStageFlagBits::eBottomOfPipe;
		case vk::ImageLayout::eGeneral:
			assert(false && "Don't know how to get a meaningful VkPipelineStageFlags for VK_IMAGE_LAYOUT_GENERAL! Don't use it!");
			return {};
		default:
			assert(false);
			return {};
	}
}

}        // anonymous namespace

// 创建一个图像内存屏障用于转换布局，并将它插入到活跃的命令缓冲区中
void image_layout_transition(vk::CommandBuffer            command_buffer,
                             vk::Image                    image,
                             vk::PipelineStageFlags       src_stage_mask,
                             vk::PipelineStageFlags       dst_stage_mask,
                             vk::AccessFlags              src_access_mask,
                             vk::AccessFlags              dst_access_mask,
                             vk::ImageLayout              old_layout,
                             vk::ImageLayout              new_layout,
                             vk::ImageSubresourceRange    subresource_range)
{
	// 创建图像屏障对象
	vk::ImageMemoryBarrier image_memory_barrier;
	image_memory_barrier.srcAccessMask       = src_access_mask;
	image_memory_barrier.dstAccessMask       = dst_access_mask;
	image_memory_barrier.oldLayout           = old_layout;
	image_memory_barrier.newLayout           = new_layout;
	image_memory_barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	image_memory_barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	image_memory_barrier.image               = image;
	image_memory_barrier.subresourceRange    = subresource_range;

	// 将屏障插入 setup 命令缓冲区
	command_buffer.pipelineBarrier(
	    src_stage_mask, dst_stage_mask, {}, {}, {}, image_memory_barrier);
}

void image_layout_transition(vk::CommandBuffer         command_buffer,
                             vk::Image                 image,
                             vk::ImageLayout           old_layout,
                             vk::ImageLayout           new_layout,
                             vk::ImageSubresourceRange subresource_range)
{
	vk::PipelineStageFlags src_stage_mask  = getPipelineStageFlags(old_layout);
	vk::PipelineStageFlags dst_stage_mask  = getPipelineStageFlags(new_layout);
	vk::AccessFlags        src_access_mask = getAccessFlags(old_layout);
	vk::AccessFlags        dst_access_mask = getAccessFlags(new_layout);

	image_layout_transition(command_buffer, image, src_stage_mask, dst_stage_mask, src_access_mask, dst_access_mask, old_layout, new_layout, subresource_range);
}

// 固定子资源：第一个 mip 级别和 layer
void image_layout_transition(vk::CommandBuffer command_buffer,
                             vk::Image         image,
                             vk::ImageLayout   old_layout,
                             vk::ImageLayout   new_layout)
{
	vk::ImageSubresourceRange subresource_range;
	subresource_range.aspectMask     = vk::ImageAspectFlagBits::eColor;
	subresource_range.baseMipLevel   = 0;
	subresource_range.levelCount     = 1;
	subresource_range.baseArrayLayer = 0;
	subresource_range.layerCount     = 1;
	image_layout_transition(command_buffer, image, old_layout, new_layout, subresource_range);
}

void image_layout_transition(vk::CommandBuffer                                              command_buffer,
                             std::vector<std::pair<vk::Image, vk::ImageSubresourceRange>> const &imagesAndRanges,
                             vk::ImageLayout                                                old_layout,
                             vk::ImageLayout                                                new_layout)
{
	vk::PipelineStageFlags src_stage_mask  = getPipelineStageFlags(old_layout);
	vk::PipelineStageFlags dst_stage_mask  = getPipelineStageFlags(new_layout);
	vk::AccessFlags        src_access_mask = getAccessFlags(old_layout);
	vk::AccessFlags        dst_access_mask = getAccessFlags(new_layout);

	// 创建图像屏障对象
	std::vector<vk::ImageMemoryBarrier> image_memory_barriers;
	image_memory_barriers.reserve(imagesAndRanges.size());
	for (size_t i = 0; i < imagesAndRanges.size(); i++)
	{
		image_memory_barriers.emplace_back(
		    vk::ImageMemoryBarrier{
		        .srcAccessMask       = src_access_mask,
		        .dstAccessMask       = dst_access_mask,
		        .oldLayout           = old_layout,
		        .newLayout           = new_layout,
		        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
		        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
		        .image               = imagesAndRanges[i].first,
		        .subresourceRange    = imagesAndRanges[i].second,
		    });
	}

	// 将屏障插入 setup 命令缓冲区
	command_buffer.pipelineBarrier(
	    src_stage_mask, dst_stage_mask, {}, {}, {}, image_memory_barriers);
}

std::vector<vk::ImageCompressionFixedRateFlagBitsEXT> fixed_rate_compression_flags_to_vector(vk::ImageCompressionFixedRateFlagsEXT flags)
{
	const std::vector<vk::ImageCompressionFixedRateFlagBitsEXT> all_flags = {
	    vk::ImageCompressionFixedRateFlagBitsEXT::e1Bpc,
	    vk::ImageCompressionFixedRateFlagBitsEXT::e2Bpc,
	    vk::ImageCompressionFixedRateFlagBitsEXT::e3Bpc,
	    vk::ImageCompressionFixedRateFlagBitsEXT::e4Bpc,
	    vk::ImageCompressionFixedRateFlagBitsEXT::e5Bpc,
	    vk::ImageCompressionFixedRateFlagBitsEXT::e6Bpc,
	    vk::ImageCompressionFixedRateFlagBitsEXT::e7Bpc,
	    vk::ImageCompressionFixedRateFlagBitsEXT::e8Bpc,
	    vk::ImageCompressionFixedRateFlagBitsEXT::e9Bpc,
	    vk::ImageCompressionFixedRateFlagBitsEXT::e10Bpc,
	    vk::ImageCompressionFixedRateFlagBitsEXT::e11Bpc,
	    vk::ImageCompressionFixedRateFlagBitsEXT::e12Bpc,
	    vk::ImageCompressionFixedRateFlagBitsEXT::e13Bpc,
	    vk::ImageCompressionFixedRateFlagBitsEXT::e14Bpc,
	    vk::ImageCompressionFixedRateFlagBitsEXT::e15Bpc,
	    vk::ImageCompressionFixedRateFlagBitsEXT::e16Bpc,
	    vk::ImageCompressionFixedRateFlagBitsEXT::e17Bpc,
	    vk::ImageCompressionFixedRateFlagBitsEXT::e18Bpc,
	    vk::ImageCompressionFixedRateFlagBitsEXT::e19Bpc,
	    vk::ImageCompressionFixedRateFlagBitsEXT::e20Bpc,
	    vk::ImageCompressionFixedRateFlagBitsEXT::e21Bpc,
	    vk::ImageCompressionFixedRateFlagBitsEXT::e22Bpc,
	    vk::ImageCompressionFixedRateFlagBitsEXT::e23Bpc,
	    vk::ImageCompressionFixedRateFlagBitsEXT::e24Bpc,
	};

	std::vector<vk::ImageCompressionFixedRateFlagBitsEXT> flags_vector;

	for (size_t i = 0; i < all_flags.size(); i++)
	{
		if (static_cast<VkImageCompressionFixedRateFlagsEXT>(all_flags[i]) & static_cast<VkImageCompressionFixedRateFlagsEXT>(flags))
		{
			flags_vector.push_back(all_flags[i]);
		}
	}

	return flags_vector;
}

vk::ImageCompressionPropertiesEXT query_supported_fixed_rate_compression(vk::PhysicalDevice gpu, const vk::ImageCreateInfo &create_info)
{
	vk::ImageCompressionPropertiesEXT supported_compression_properties;

	vk::ImageCompressionControlEXT compression_control;
	compression_control.flags = vk::ImageCompressionFlagBitsEXT::eFixedRateDefault;

	vk::PhysicalDeviceImageFormatInfo2 image_format_info;
	image_format_info.format = create_info.format;
	image_format_info.type   = create_info.imageType;
	image_format_info.tiling = create_info.tiling;
	image_format_info.usage  = create_info.usage;
	image_format_info.pNext  = &compression_control;

	vk::ImageFormatProperties2 image_format_properties;
	image_format_properties.pNext = &supported_compression_properties;

	(void)gpu.getImageFormatProperties2(&image_format_info, &image_format_properties);

	return supported_compression_properties;
}

vk::ImageCompressionPropertiesEXT query_applied_compression(vk::Device device, vk::Image image)
{
	vk::ImageSubresource2EXT image_subresource;
	image_subresource.imageSubresource.aspectMask = vk::ImageAspectFlagBits::eColor;
	image_subresource.imageSubresource.mipLevel   = 0;
	image_subresource.imageSubresource.arrayLayer = 0;

	vk::ImageCompressionPropertiesEXT compression_properties;
	vk::SubresourceLayout2EXT         subresource_layout;
	subresource_layout.pNext = &compression_properties;

	(void)device.getImageSubresourceLayout2EXT(image, &image_subresource, &subresource_layout);

	return compression_properties;
}

vk::SurfaceFormatKHR select_surface_format(vk::PhysicalDevice gpu, vk::SurfaceKHR surface, std::vector<vk::Format> const &preferred_formats)
{
	std::vector<vk::SurfaceFormatKHR> supported_surface_formats = gpu.getSurfaceFormatsKHR(surface);

	auto it = std::ranges::find_if(supported_surface_formats,
	                               [&preferred_formats](vk::SurfaceFormatKHR const &surface_format) {
		                               return std::ranges::any_of(preferred_formats,
		                                                          [&surface_format](vk::Format format) { return format == surface_format.format; });
	                               });

	// 如果首选格式均不可用，则回退到第一个支持的格式
	return it != supported_surface_formats.end() ? *it : supported_surface_formats[0];
}

namespace gbuffer
{
std::vector<LoadStoreInfo> get_load_all_store_swapchain()
{
	// Load 所有附件，仅 Store swapchain
	std::vector<LoadStoreInfo> load_store{4};

	// Swapchain
	load_store[0].load_op  = vk::AttachmentLoadOp::eDontCare;
	load_store[0].store_op = vk::AttachmentStoreOp::eStore;

	// Depth
	load_store[1].load_op  = vk::AttachmentLoadOp::eLoad;
	load_store[1].store_op = vk::AttachmentStoreOp::eDontCare;

	// Albedo
	load_store[2].load_op  = vk::AttachmentLoadOp::eLoad;
	load_store[2].store_op = vk::AttachmentStoreOp::eDontCare;

	// Normal
	load_store[3].load_op  = vk::AttachmentLoadOp::eLoad;
	load_store[3].store_op = vk::AttachmentStoreOp::eDontCare;

	return load_store;
}

std::vector<LoadStoreInfo> get_clear_all_store_swapchain()
{
	// Clear 所有附件，仅 Store swapchain
	std::vector<LoadStoreInfo> load_store{4};

	// Swapchain
	load_store[0].load_op  = vk::AttachmentLoadOp::eClear;
	load_store[0].store_op = vk::AttachmentStoreOp::eStore;

	// Depth
	load_store[1].load_op  = vk::AttachmentLoadOp::eClear;
	load_store[1].store_op = vk::AttachmentStoreOp::eDontCare;

	// Albedo
	load_store[2].load_op  = vk::AttachmentLoadOp::eClear;
	load_store[2].store_op = vk::AttachmentStoreOp::eDontCare;

	// Normal
	load_store[3].load_op  = vk::AttachmentLoadOp::eClear;
	load_store[3].store_op = vk::AttachmentStoreOp::eDontCare;

	return load_store;
}

std::vector<LoadStoreInfo> get_clear_store_all()
{
	// Clear 并 Store 所有附件
	std::vector<LoadStoreInfo> load_store{4};

	// Swapchain
	load_store[0].load_op  = vk::AttachmentLoadOp::eClear;
	load_store[0].store_op = vk::AttachmentStoreOp::eStore;

	// Depth
	load_store[1].load_op  = vk::AttachmentLoadOp::eClear;
	load_store[1].store_op = vk::AttachmentStoreOp::eStore;

	// Albedo
	load_store[2].load_op  = vk::AttachmentLoadOp::eClear;
	load_store[2].store_op = vk::AttachmentStoreOp::eStore;

	// Normal
	load_store[3].load_op  = vk::AttachmentLoadOp::eClear;
	load_store[3].store_op = vk::AttachmentStoreOp::eStore;

	return load_store;
}

std::vector<vk::ClearValue> get_clear_value()
{
	// G-buffer 清除值
	std::vector<vk::ClearValue> clear_value{4};
	clear_value[0].color        = vk::ClearColorValue{std::array<float, 4>{0.0f, 0.0f, 0.0f, 1.0f}};
	clear_value[1].depthStencil = vk::ClearDepthStencilValue{0.0f, ~0U};
	clear_value[2].color        = vk::ClearColorValue{std::array<float, 4>{0.0f, 0.0f, 0.0f, 1.0f}};
	clear_value[3].color        = vk::ClearColorValue{std::array<float, 4>{0.0f, 0.0f, 0.0f, 1.0f}};

	return clear_value;
}
}        // namespace gbuffer

uint32_t get_queue_family_index(std::vector<vk::QueueFamilyProperties> const &queue_family_properties, vk::QueueFlagBits queue_flag)
{
	// 专用于计算的队列
	// 尝试查找支持计算但不支持图形的队列族
	if (static_cast<VkQueueFlags>(queue_flag) & VK_QUEUE_COMPUTE_BIT)
	{
		auto propertyIt = std::ranges::find_if(queue_family_properties,
		                                       [queue_flag](const vk::QueueFamilyProperties &property) {
			                                       return (property.queueFlags & queue_flag) &&
			                                              !(property.queueFlags & vk::QueueFlagBits::eGraphics);
		                                       });
		if (propertyIt != queue_family_properties.end())
		{
			return static_cast<uint32_t>(std::distance(queue_family_properties.begin(), propertyIt));
		}
	}

	// 专用于传输的队列
	// 尝试查找支持传输但不支持图形和计算的队列族
	if (static_cast<VkQueueFlags>(queue_flag) & VK_QUEUE_TRANSFER_BIT)
	{
		auto propertyIt = std::ranges::find_if(queue_family_properties,
		                                       [queue_flag](const vk::QueueFamilyProperties &property) {
			                                       return (property.queueFlags & queue_flag) &&
			                                              !(property.queueFlags & vk::QueueFlagBits::eGraphics) &&
			                                              !(property.queueFlags & vk::QueueFlagBits::eCompute);
		                                       });
		if (propertyIt != queue_family_properties.end())
		{
			return static_cast<uint32_t>(std::distance(queue_family_properties.begin(), propertyIt));
		}
	}

	// 对于其他队列类型，或未找到专用计算/传输队列时，返回第一个支持请求标志的队列族
	auto propertyIt = std::ranges::find_if(
	    queue_family_properties, [queue_flag](const vk::QueueFamilyProperties &property) { return (property.queueFlags & queue_flag) == queue_flag; });
	if (propertyIt != queue_family_properties.end())
	{
		return static_cast<uint32_t>(std::distance(queue_family_properties.begin(), propertyIt));
	}

	throw std::runtime_error("Could not find a matching queue family index");
}

}        // namespace GE
