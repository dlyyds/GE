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
 * @file VulkanImageView.h
 * @brief 从 Vulkan-Samples 适配的 VulkanImageView（RAII 风格的 vk::ImageView 封装）。
 */

#pragma once

#include "Render/VulkanBase/VulkanResourceBase.h"
#include <Render/VulkanBase/VulkanDevice.h>

#include <vulkan/vulkan.hpp>

namespace GE {

class VulkanImage;

/**
 * @brief RAII 风格的 vk::ImageView 封装。
 *
 * 构造时创建 ImageView，析构时自动销毁。
 * 与 VulkanImage 关联，在 Image 被移动时自动更新引用。
 */
class VulkanImageView : public VulkanResourceBase<vk::ImageView> {
public:
    VulkanImageView(VulkanImage &image,
                    vk::ImageViewType view_type,
                    vk::Format format = vk::Format::eUndefined,
                    uint32_t base_mip_level = 0,
                    uint32_t base_array_layer = 0,
                    uint32_t n_mip_levels = 0,
                    uint32_t n_array_layers = 0);

    VulkanImageView(VulkanImageView &) = delete;

    VulkanImageView(VulkanImageView &&other);

    ~VulkanImageView() override;

    VulkanImageView &operator=(const VulkanImageView &) = delete;

    VulkanImageView &operator=(VulkanImageView &&) = delete;

    vk::Format get_format() const;

    VulkanImage const &get_image() const;

    VulkanImage &get_image();

    void set_image(VulkanImage &image);

    vk::ImageSubresourceLayers get_subresource_layers() const;

    vk::ImageSubresourceRange get_subresource_range() const;

private:
    VulkanImage *image = nullptr;
    vk::Format format;
    vk::ImageSubresourceRange subresource_range;
};

} // namespace GE
