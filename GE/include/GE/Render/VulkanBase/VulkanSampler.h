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
 * @file VulkanSampler.h
 * @brief RAII 风格的 vk::Sampler 封装。
 *
 * 构造时创建 Sampler，析构时自动销毁。
 * 支持按 Vulkan Sampler 创建参数（过滤器、寻址模式、各向异性等）配置，
 * 并可配合 VulkanResourceCache 实现全局去重缓存。
 */

#pragma once

#include "Render/VulkanBase/VulkanResourceBase.h"
#include <Render/VulkanBase/VulkanDevice.h>

#include <vulkan/vulkan.hpp>

namespace GE {

/**
 * @brief RAII 风格的 vk::Sampler 封装。
 *
 * 继承 VulkanResourceBase<vk::Sampler>，构造时创建 Sampler，
 * 析构时自动销毁。支持移动语义。
 */
class VulkanSampler : public VulkanResourceBase<vk::Sampler> {
public:
    /**
     * @brief 构造 VulkanSampler。
     *
     * @param device            Vulkan 设备引用
     * @param mag_filter        放大过滤器
     * @param min_filter        缩小过滤器
     * @param mipmap_mode       Mipmap 模式
     * @param address_mode_u    U 轴寻址模式
     * @param address_mode_v    V 轴寻址模式
     * @param address_mode_w    W 轴寻址模式
     * @param mip_lod_bias      Mip LOD 偏移
     * @param anisotropy_enable 是否启用各向异性过滤
     * @param max_anisotropy    最大各向异性级别
     * @param compare_enable    是否启用比较操作（用于阴影）
     * @param compare_op        比较操作
     * @param min_lod           最小 LOD
     * @param max_lod           最大 LOD
     * @param border_color      边框颜色
     * @param unnormalized_coordinates 是否使用非归一化坐标
     */
    VulkanSampler(VulkanDevice &device,
                  vk::Filter              mag_filter            = vk::Filter::eLinear,
                  vk::Filter              min_filter            = vk::Filter::eLinear,
                  vk::SamplerMipmapMode   mipmap_mode           = vk::SamplerMipmapMode::eLinear,
                  vk::SamplerAddressMode  address_mode_u        = vk::SamplerAddressMode::eRepeat,
                  vk::SamplerAddressMode  address_mode_v        = vk::SamplerAddressMode::eRepeat,
                  vk::SamplerAddressMode  address_mode_w        = vk::SamplerAddressMode::eRepeat,
                  float                   mip_lod_bias          = 0.0f,
                  vk::Bool32             anisotropy_enable     = VK_FALSE,
                  float                   max_anisotropy        = 1.0f,
                  vk::Bool32             compare_enable        = VK_FALSE,
                  vk::CompareOp           compare_op            = vk::CompareOp::eAlways,
                  float                   min_lod               = 0.0f,
                  float                   max_lod               = VK_LOD_CLAMP_NONE,
                  vk::BorderColor         border_color          = vk::BorderColor::eFloatTransparentBlack,
                  vk::Bool32             unnormalized_coordinates = VK_FALSE);

    VulkanSampler(const VulkanSampler &) = delete;

    VulkanSampler(VulkanSampler &&other) noexcept;

    ~VulkanSampler() override;

    VulkanSampler &operator=(const VulkanSampler &) = delete;

    VulkanSampler &operator=(VulkanSampler &&) = delete;

    /**
     * @brief 获取创建参数结构体（用于 hash 和调试）。
     */
    const vk::SamplerCreateInfo &GetCreateInfo() const;

private:
    vk::SamplerCreateInfo m_create_info;
};

} // namespace GE