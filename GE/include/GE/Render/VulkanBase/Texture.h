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
 * @file Texture.h
 * @brief 纹理封装 —— 简化 Vulkan 纹理创建、上传、采样配置。
 *
 * 封装 VulkanImage + VulkanImageView + VulkanSampler，提供：
 * - LoadFromFile()：从文件加载纹理（自动使用 stb_image 解码）
 * - LoadFromMemory()：从内存像素数据创建纹理
 * - 直接构造：创建空白纹理（用于 RenderTarget 等）
 *
 * 内部自动处理：
 * - staging buffer 上传
 * - 布局转换（UNDEFINED -> TRANSFER_DST -> SHADER_READ_ONLY）
 * - ImageView 创建
 * - Sampler 请求（通过 VulkanResourceCache 去重）
 */

#pragma once

#include "Render/VulkanBase/VulkanImage.h"
#include "Render/VulkanBase/VulkanImageView.h"
#include "Render/VulkanBase/VulkanSampler.h"
#include "Render/VulkanBase/VulkanResourceCache.h"
#include "Render/VulkanBase/VulkanDevice.h"

#include <vulkan/vulkan.hpp>

#include <cstdint>
#include <memory>
#include <string>

namespace GE {

/**
 * @brief 纹理封装 —— 持有 VulkanImage + ImageView + Sampler。
 *
 * 使用方式：
 * @code
 *   auto &device = ctx.GetDevice();
 *   auto &cache = device.GetResourceCache();
 *
 *   // 从文件加载
 *   auto tex = Texture::LoadFromFile(device, cache, "textures/foo.png");
 *
 *   // 获取描述符信息用于绑定
 *   vk::DescriptorImageInfo info = tex->GetDescriptorInfo();
 * @endcode
 */
class Texture {
public:
    // ========================================================================
    // 工厂方法
    // ========================================================================

    /**
     * @brief 从文件加载纹理（自动使用 stb_image 解码为 RGBA）。
     *
     * @param device      Vulkan 设备
     * @param cache       全局资源缓存（用于 Sampler 去重）
     * @param filepath    纹理文件路径（支持 PNG / JPG 等 stb_image 格式）
     * @param format      纹理格式（默认 eR8G8B8A8Unorm）
     * @param mag_filter  放大过滤器（默认 eLinear）
     * @param min_filter  缩小过滤器（默认 eLinear）
     * @return std::unique_ptr<Texture>  失败时返回 nullptr
     */
    static std::unique_ptr<Texture> LoadFromFile(
        VulkanDevice &device,
        VulkanResourceCache &cache,
        const std::string &filepath,
        vk::Format format = vk::Format::eR8G8B8A8Unorm,
        vk::Filter mag_filter = vk::Filter::eLinear,
        vk::Filter min_filter = vk::Filter::eLinear);

    /**
     * @brief 从内存像素数据创建纹理。
     *
     * @param device      Vulkan 设备
     * @param cache       全局资源缓存（用于 Sampler 去重）
     * @param pixels      RGBA 像素数据（每个像素 4 字节）
     * @param width       纹理宽度（像素）
     * @param height      纹理高度（像素）
     * @param format      纹理格式（默认 eR8G8B8A8Unorm）
     * @param mag_filter  放大过滤器（默认 eLinear）
     * @param min_filter  缩小过滤器（默认 eLinear）
     * @return std::unique_ptr<Texture>  失败时返回 nullptr
     */
    static std::unique_ptr<Texture> LoadFromMemory(
        VulkanDevice &device,
        VulkanResourceCache &cache,
        const void *pixels,
        uint32_t width, uint32_t height,
        vk::Format format = vk::Format::eR8G8B8A8Unorm,
        vk::Filter mag_filter = vk::Filter::eLinear,
        vk::Filter min_filter = vk::Filter::eLinear);

    // ========================================================================
    // 直接构造（空白纹理，不传数据）
    // ========================================================================

    /**
     * @brief 创建空白纹理（仅分配 GPU 图像，不包含数据）。
     *
     * 适用于需要预分配纹理的场景（如 RenderTarget）。
     * 图像默认使用 eTransferDst | eSampled 用法，
     * 可通过 extra_usage 添加额外标志（如 eColorAttachment）。
     *
     * @note 此构造函数不设置 Sampler，需要手动调用 SetupSampler() 或
     *       在创建后通过 GetSampler() 设置。
     */
    Texture(VulkanDevice &device,
            vk::Extent3D extent,
            vk::Format format,
            vk::ImageUsageFlags extra_usage = {});

    ~Texture();

    Texture(Texture &&other) noexcept;
    Texture(const Texture &) = delete;
    Texture &operator=(Texture &&) = delete;
    Texture &operator=(const Texture &) = delete;

    // ========================================================================
    // 访问器
    // ========================================================================

    VulkanImage &GetImage() { return *m_Image; }
    const VulkanImage &GetImage() const { return *m_Image; }

    VulkanImageView &GetImageView() { return *m_ImageView; }
    const VulkanImageView &GetImageView() const { return *m_ImageView; }

    VulkanSampler &GetSampler() { return *m_Sampler; }
    const VulkanSampler &GetSampler() const { return *m_Sampler; }

    const vk::Extent3D &GetExtent() const { return m_Extent; }
    vk::Format GetFormat() const { return m_Format; }

    /**
     * @brief 获取描述符信息，用于绑定到 descriptor set。
     *
     * 返回的 DescriptorImageInfo 包含 sampler、imageView 和 imageLayout，
     * 可直接用于 vk::WriteDescriptorSet 的 pImageInfo。
     */
    vk::DescriptorImageInfo GetDescriptorInfo() const;

    /**
     * @brief 手动设置 Sampler（用于空白纹理构造后配置采样器）。
     */
    void SetSampler(VulkanSampler &sampler) { m_Sampler = &sampler; }

private:
    // ========================================================================
    // 内部辅助
    // ========================================================================

    /**
     * @brief 上传像素数据到 GPU 图像。
     *
     * 内部流程：
     * 1. 创建 staging buffer 并拷贝像素数据
     * 2. 获取临时 command buffer
     * 3. 布局转换：UNDEFINED -> TRANSFER_DST
     * 4. copyBufferToImage
     * 5. 布局转换：TRANSFER_DST -> SHADER_READ_ONLY
     * 6. 提交并等待完成
     */
    void UploadPixels(VulkanDevice &device, const void *pixels,
                      uint32_t width, uint32_t height);

    /**
     * @brief 创建 ImageView 并通过缓存请求 Sampler。
     */
    void CreateViewAndSampler(VulkanDevice &device,
                              VulkanResourceCache &cache,
                              vk::Filter mag_filter,
                              vk::Filter min_filter);

    // ========================================================================
    // 成员
    // ========================================================================

    std::unique_ptr<VulkanImage>     m_Image;
    std::unique_ptr<VulkanImageView> m_ImageView;
    VulkanSampler                   *m_Sampler = nullptr; // 由 VulkanResourceCache 管理
    vk::Format                       m_Format  = vk::Format::eR8G8B8A8Unorm;
    vk::Extent3D                     m_Extent{};
};

} // namespace GE