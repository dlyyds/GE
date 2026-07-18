/* Copyright (c) 2023-2025, NVIDIA CORPORATION. All rights reserved.
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

#include "Render/VulkanBase/VulkanImageView.h"
#include "Render/VulkanBase/VulkanImage.h"

#include <vulkan/vulkan_format_traits.hpp>

#include <stdexcept>

namespace GE
{

VulkanImageView::VulkanImageView(VulkanImage &img,
                                       vk::ImageViewType    view_type,
                                       vk::Format           format,
                                       uint32_t             mip_level,
                                       uint32_t             array_layer,
                                       uint32_t             n_mip_levels,
                                       uint32_t             n_array_layers) :
    VulkanResourceBase<vk::ImageView>{nullptr, &img.GetDevice()},
    image{&img},
    format{format}
{
	if (format == vk::Format::eUndefined)
	{
		this->format = format = image->get_format();
	}

	// 自动推断 aspect mask：遍历所有组件（D=Depth, S=Stencil）
	vk::ImageAspectFlags aspect = {};
	for (uint32_t i = 0;; ++i)
	{
		std::string name = vk::componentName(format, i);
		if (name.empty())
			break;
		if (name == "D")
			aspect |= vk::ImageAspectFlagBits::eDepth;
		if (name == "S")
			aspect |= vk::ImageAspectFlagBits::eStencil;
	}
	if (aspect == vk::ImageAspectFlags{})
	{
		aspect = vk::ImageAspectFlagBits::eColor;
	}

	subresource_range = vk::ImageSubresourceRange{
	    .aspectMask     = aspect,
	    .baseMipLevel   = mip_level,
	    .levelCount     = n_mip_levels == 0 ? image->get_subresource().mipLevel : n_mip_levels,
	    .baseArrayLayer = array_layer,
	    .layerCount     = n_array_layers == 0 ? image->get_subresource().arrayLayer : n_array_layers,
	};

	vk::ImageViewCreateInfo image_view_create_info{
	    .image            = image->GetHandle(),
	    .viewType         = view_type,
	    .format           = format,
	    .subresourceRange = subresource_range,
	};

	SetHandle(GetDevice().GetHandle().createImageView(image_view_create_info));

	// 向 Image 注册此 View，以便 Image 移动时收到通知
	image->get_views().emplace(this);
}

VulkanImageView::VulkanImageView(VulkanImageView &&other) :
    VulkanResourceBase<vk::ImageView>{std::move(other)},
    image{other.image},
    format{other.format},
    subresource_range{other.subresource_range}
{
	// 从旧 Image 的 view 集合中移除旧的 this 指针，加入新的
	auto &views = image->get_views();
	views.erase(&other);
	views.emplace(this);

	other.SetHandle(nullptr);
}

VulkanImageView::~VulkanImageView()
{
	if (GetHandle())
	{
		GetDevice().GetHandle().destroyImageView(GetHandle());
	}
}

vk::Format VulkanImageView::get_format() const
{
	return format;
}

const VulkanImage &VulkanImageView::get_image() const
{
	if (!image)
	{
		throw std::runtime_error("VulkanImageView is referring an invalid image");
	}
	return *image;
}

void VulkanImageView::set_image(VulkanImage &img)
{
	image = &img;
}

vk::ImageSubresourceLayers VulkanImageView::get_subresource_layers() const
{
	return vk::ImageSubresourceLayers{
	    subresource_range.aspectMask,
	    subresource_range.baseMipLevel,
	    subresource_range.baseArrayLayer,
	    subresource_range.layerCount,
	};
}

vk::ImageSubresourceRange VulkanImageView::get_subresource_range() const
{
	return subresource_range;
}

}        // namespace GE
