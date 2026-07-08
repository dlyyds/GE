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

/**
 * @file VulkanHppImageView.h
 * @brief 从 Vulkan-Samples 适配的 HPPImageView（RAII 风格的 vk::ImageView 封装）。
 */

#pragma once

#include "Render/VulkanBase/VulkanResourceBase.h"

#include <vulkan/vulkan.hpp>

namespace GE
{

class VulkanHppImage;

/**
 * @brief RAII 风格的 vk::ImageView 封装。
 *
 * 构造时创建 ImageView，析构时自动销毁。
 * 与 VulkanHppImage 关联，在 Image 被移动时自动更新引用。
 */
class VulkanHppImageView : public VulkanResourceBase<vk::ImageView>
{
  public:
	VulkanHppImageView(VulkanHppImage &image,
	                   vk::ImageViewType    view_type,
	                   vk::Format           format           = vk::Format::eUndefined,
	                   uint32_t             base_mip_level   = 0,
	                   uint32_t             base_array_layer = 0,
	                   uint32_t             n_mip_levels     = 0,
	                   uint32_t             n_array_layers   = 0);

	VulkanHppImageView(VulkanHppImageView &) = delete;
	VulkanHppImageView(VulkanHppImageView &&other);
	~VulkanHppImageView() override;

	VulkanHppImageView &operator=(const VulkanHppImageView &) = delete;
	VulkanHppImageView &operator=(VulkanHppImageView &&)      = delete;

	vk::Format                    get_format() const;
	VulkanHppImage const         &get_image() const;
	void                          set_image(VulkanHppImage &image);
	vk::ImageSubresourceLayers    get_subresource_layers() const;
	vk::ImageSubresourceRange     get_subresource_range() const;

  private:
	VulkanHppImage             *image              = nullptr;
	vk::Format                  format;
	vk::ImageSubresourceRange   subresource_range;
};

}        // namespace GE
