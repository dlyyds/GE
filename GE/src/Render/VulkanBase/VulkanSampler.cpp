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
 * @file VulkanSampler.cpp
 * @brief RAII 风格的 vk::Sampler 封装实现。
 */

#include "Render/VulkanBase/VulkanSampler.h"
#include "Render/VulkanBase/VulkanDevice.h"

namespace GE {

VulkanSampler::VulkanSampler(VulkanDevice &device,
                             vk::Filter             mag_filter,
                             vk::Filter             min_filter,
                             vk::SamplerMipmapMode  mipmap_mode,
                             vk::SamplerAddressMode address_mode_u,
                             vk::SamplerAddressMode address_mode_v,
                             vk::SamplerAddressMode address_mode_w,
                             float                  mip_lod_bias,
                             vk::Bool32            anisotropy_enable,
                             float                  max_anisotropy,
                             vk::Bool32            compare_enable,
                             vk::CompareOp          compare_op,
                             float                  min_lod,
                             float                  max_lod,
                             vk::BorderColor        border_color,
                             vk::Bool32            unnormalized_coordinates)
    : VulkanResourceBase<vk::Sampler>{nullptr, &device} {

    m_create_info = vk::SamplerCreateInfo{
        .magFilter               = mag_filter,
        .minFilter               = min_filter,
        .mipmapMode              = mipmap_mode,
        .addressModeU            = address_mode_u,
        .addressModeV            = address_mode_v,
        .addressModeW            = address_mode_w,
        .mipLodBias              = mip_lod_bias,
        .anisotropyEnable        = anisotropy_enable,
        .maxAnisotropy           = max_anisotropy,
        .compareEnable           = compare_enable,
        .compareOp               = compare_op,
        .minLod                  = min_lod,
        .maxLod                  = max_lod,
        .borderColor             = border_color,
        .unnormalizedCoordinates = unnormalized_coordinates,
    };

    SetHandle(GetDevice().GetHandle().createSampler(m_create_info));
}

VulkanSampler::VulkanSampler(VulkanSampler &&other) noexcept
    : VulkanResourceBase<vk::Sampler>{std::move(other)},
      m_create_info{other.m_create_info} {
    other.SetHandle(nullptr);
}

VulkanSampler::~VulkanSampler() {
    if (GetHandle()) {
        GetDevice().GetHandle().destroySampler(GetHandle());
    }
}

const vk::SamplerCreateInfo &VulkanSampler::GetCreateInfo() const {
    return m_create_info;
}

} // namespace GE