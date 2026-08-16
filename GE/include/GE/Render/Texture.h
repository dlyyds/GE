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

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>

namespace GE {

class VulkanCommandBuffer;
class AsyncUploadManager;

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
     * @param generate_mipmaps  是否自动生成完整 mip 链（默认 true）
     * @return std::unique_ptr<Texture>  失败时返回 nullptr
     */
    static std::unique_ptr<Texture> LoadFromFile(
        VulkanDevice &device,
        VulkanResourceCache &cache,
        const std::string &filepath,
        vk::Format format = vk::Format::eR8G8B8A8Unorm,
        vk::Filter mag_filter = vk::Filter::eLinear,
        vk::Filter min_filter = vk::Filter::eLinear,
        bool generate_mipmaps = true);

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
     * @param generate_mipmaps  是否自动生成完整 mip 链（默认 true）
     * @return std::unique_ptr<Texture>  失败时返回 nullptr
     */
    static std::unique_ptr<Texture> LoadFromMemory(
        VulkanDevice &device,
        VulkanResourceCache &cache,
        const void *pixels,
        uint32_t width, uint32_t height,
        vk::Format format = vk::Format::eR8G8B8A8Unorm,
        vk::Filter mag_filter = vk::Filter::eLinear,
        vk::Filter min_filter = vk::Filter::eLinear,
        bool generate_mipmaps = true);

    /**
     * @brief 从 KTX2 文件加载 cubemap 纹理（6 面，eCube 视图）。
     *
     * 用 libktx 读取 `.ktx` 数据，创建 6 层 cubemap 图像（eCubeCompatible）并
     * 逐 (level, face) 上传，最终建立 eCube ImageView。供天空盒 / IBL 环境图
     * 等 cubemap 资源使用。
     *
     * @param device    Vulkan 设备
     * @param cache     全局资源缓存（用于 Sampler 去重）
     * @param filepath  KTX2 文件路径（须为 6 面 cubemap）
     * @return std::unique_ptr<Texture>  失败时返回 nullptr
     */
    static std::unique_ptr<Texture> LoadCubeMapFromFile(
        VulkanDevice &device,
        VulkanResourceCache &cache,
        const std::string &filepath);

    /// ========================================================================
    /// 异步工厂方法（解码 + 上传移入后台线程，主线程每帧 Poll 回收后注入）
    /// ========================================================================
    /// 调用后立即返回一个"空壳"纹理（m_Ready=false，m_Image 为空）；后台线程
    /// 完成解码与 GPU 上传后，主线程 Poll() 时经 InstallAsyncImage 注入并置就绪。
    /// 就绪前调用方应经 IsReady() 检查，未就绪时降级为默认纹理。
    ///
    /// 生命周期：返回的空壳被销毁（如 unload）时自动作废内部注入槽位，已提交的
    /// 异步任务 finalize 会安全跳过注入，不会 use-after-free。

    /**
     * @brief 异步从文件加载纹理（stb_image 解码 + GPU 上传全后台）。
     *
     * @param device       Vulkan 设备
     * @param cache        全局资源缓存（Sampler 去重）
     * @param upload       异步上传管理器（提交 UploadTask）
     * @param filepath     纹理文件路径
     * @param format       纹理格式（默认 eR8G8B8A8Unorm）
     * @param mag_filter   放大过滤器（默认 eLinear）
     * @param min_filter   缩小过滤器（默认 eLinear）
     * @param generate_mipmaps  是否自动生成完整 mip 链（默认 true）
     * @return std::unique_ptr<Texture>  空壳纹理（未就绪），失败时返回 nullptr
     */
    static std::unique_ptr<Texture> LoadFromFileAsync(
        VulkanDevice &device,
        VulkanResourceCache &cache,
        AsyncUploadManager &upload,
        const std::string &filepath,
        vk::Format format = vk::Format::eR8G8B8A8Unorm,
        vk::Filter mag_filter = vk::Filter::eLinear,
        vk::Filter min_filter = vk::Filter::eLinear,
        bool generate_mipmaps = true);

    /**
     * @brief 异步从内存像素数据创建纹理（GPU 上传后台，pixels 会被拷贝）。
     *
     * @param device       Vulkan 设备
     * @param cache        全局资源缓存（Sampler 去重）
     * @param upload       异步上传管理器（提交 UploadTask）
     * @param pixels       RGBA 像素数据（每个像素 4 字节，调用方须在返回前保持有效，
     *                     本函数会拷贝到内部容器）
     * @param width        纹理宽度
     * @param height       纹理高度
     * @param format       纹理格式（默认 eR8G8B8A8Unorm）
     * @param mag_filter   放大过滤器（默认 eLinear）
     * @param min_filter   缩小过滤器（默认 eLinear）
     * @param generate_mipmaps  是否自动生成完整 mip 链（默认 true）
     * @return std::unique_ptr<Texture>  空壳纹理（未就绪），失败时返回 nullptr
     */
    static std::unique_ptr<Texture> LoadFromMemoryAsync(
        VulkanDevice &device,
        VulkanResourceCache &cache,
        AsyncUploadManager &upload,
        const void *pixels,
        uint32_t width, uint32_t height,
        vk::Format format = vk::Format::eR8G8B8A8Unorm,
        vk::Filter mag_filter = vk::Filter::eLinear,
        vk::Filter min_filter = vk::Filter::eLinear,
        bool generate_mipmaps = true);

    /**
     * @brief 异步从 KTX 文件加载 cubemap 纹理（6 面，eCube 视图）。
     *
     * @param device    Vulkan 设备
     * @param cache     全局资源缓存（Sampler 去重）
     * @param upload    异步上传管理器（提交 UploadTask）
     * @param filepath  KTX 文件路径（须为 6 面 cubemap）
     * @return std::unique_ptr<Texture>  空壳纹理（未就绪），失败时返回 nullptr
     */
    static std::unique_ptr<Texture> LoadCubeMapFromFileAsync(
        VulkanDevice &device,
        VulkanResourceCache &cache,
        AsyncUploadManager &upload,
        const std::string &filepath);

    // ========================================================================
    // 直接构造（空白纹理，不传数据）
    // ========================================================================

    /**
     * @brief 创建空白纹理（仅分配 GPU 图像，不包含数据）。
     *
     * 适用于需要预分配纹理的场景（如 RenderTarget）。
     * 图像默认使用 eTransferDst | eSampled 用法，
     * 可通过 extra_usage 添加额外标志（如 eColorAttachment）。
     * 当 mip_levels > 1 时会自动添加 eTransferSrc 用法（用于 blit 源）。
     *
     * @note 此构造函数不设置 Sampler，需要手动调用 SetupSampler() 或
     *       在创建后通过 GetSampler() 设置。
     *
     * @param device       Vulkan 设备
     * @param extent       纹理尺寸
     * @param format       纹理格式
     * @param extra_usage  额外的图像用法标志
     * @param mip_levels   mip 级别数（默认 1，即无 mipmap）
     */
    Texture(VulkanDevice &device,
            vk::Extent3D extent,
            vk::Format format,
            vk::ImageUsageFlags extra_usage = {},
            uint32_t mip_levels = 1);

private:
    /**
     * @brief 内部构造：创建支持 cubemap 的空白纹理图像。
     *
     * @param array_layers  数组层数（cubemap 为 6，普通 2D 为 1）
     * @param cube_map      是否为 cubemap（true 时设置 eCubeCompatible 标志）
     *
     * 公开的无参构造委托给本构造（array_layers=1, cube_map=false）。
     */
    Texture(VulkanDevice &device,
            vk::Extent3D extent,
            vk::Format format,
            vk::ImageUsageFlags extra_usage,
            uint32_t mip_levels,
            uint32_t array_layers,
            bool cube_map);

    /**
     * @brief 空壳构造（异步加载用）：不创建任何 GPU 图像，m_Image 为空。
     *
     * 由异步工厂（LoadFromFileAsync 等）创建，返回给调用方后立即提交后台任务；
     * 后台加载完成时经 InstallAsyncImage 注入图像并置就绪。
     */
    Texture();

    /**
     * @brief 异步加载注入槽位。
     *
     * 由空壳纹理持有（shared_ptr），异步任务 finalize 亦持有同份引用。空壳被
     * 销毁时（如 unload）析构函数将其标记作废，finalize 据此安全跳过注入，避免
     * 对已销毁目标的 use-after-free。主线程 Poll() 与 unload 均跑在主线程，故
     * target/abandoned 无需原子。
     */
    struct AsyncPendingSlot {
        Texture *target = nullptr;   ///< 待注入的目标空壳（析构时置空）
        bool abandoned = false;      ///< 目标已销毁，finalize 应跳过
    };

public:

    /**
     * @brief 生成 mipmap 链（使用 blit + 线性过滤）。
     *
     * 要求：
     * - Image 创建时已指定 mip_levels > 1
     * - Image 具有 eTransferSrc 和 eTransferDst 用法
     * - level 0 已包含有效数据，布局为 TransferDstOptimal 或 ShaderReadOnlyOptimal
     *
     * 生成后所有 mip level 的布局均为 ShaderReadOnlyOptimal。
     */
    void GenerateMipmaps(VulkanDevice &device);

    ~Texture();

    Texture(Texture &&other) noexcept;
    Texture(const Texture &) = delete;
    Texture &operator=(Texture &&) = delete;
    Texture &operator=(const Texture &) = delete;

    // ========================================================================
    // 就绪状态 / 注入
    // ========================================================================

    /**
     * @brief 纹理是否已完全就绪（图像 + 视图 + 采样器可用）。
     *
     * 异步工厂返回的空壳纹理在后台加载完成、主线程注入前为 false。
     * 渲染端绑定前必须检查：未就绪时降级为默认纹理，避免访问空句柄。
     * 同步工厂 / 直接构造创建的纹理恒为 true。
     */
    bool IsReady() const { return m_Ready.load(std::memory_order_acquire); }

    /**
     * @brief 主线程安装后台异步加载完成的本地图像，并创建视图 + 采样器。
     *
     * 仅由异步任务 finalize（主线程 Poll()）调用。注入后置 m_Ready=true，
     * 下帧渲染自动可见。若本纹理已被销毁（经 AsyncPendingSlot 门控），
     * 调用方会跳过本方法，image 由调用方释放。
     *
     * @param image       后台线程创建的本地 VulkanImage（接管所有权）
     * @param cache       全局资源缓存（Sampler 去重）
     * @param mag_filter  放大过滤器
     * @param min_filter  缩小过滤器
     */
    void InstallAsyncImage(std::unique_ptr<VulkanImage> image,
                           VulkanResourceCache &cache,
                           vk::Filter mag_filter,
                           vk::Filter min_filter);

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
     * @brief 获取纹理源文件路径。
     *
     * 仅当纹理通过 LoadFromFile() 加载时有有效路径；
     * 通过 LoadFromMemory() 或直接构造（空白纹理）创建的纹理路径为空字符串。
     */
    const std::string &GetFilePath() const { return m_FilePath; }

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

    // ========================================================================
    // 采样器便捷修改（运行时生效，无需重建纹理）
    // ========================================================================
    // 以下方法通过 LoadFromFile / LoadFromMemory 创建时记录的采样器缓存
    // （VulkanResourceCache）按同样参数重新请求一个采样器并替换，仅影响采样
    // 行为，不改动图像存储。对空白纹理（未设置缓存）调用为无操作。

    /**
     * @brief 设置放大/缩小过滤方式。
     */
    void SetFilter(vk::Filter mag_filter, vk::Filter min_filter);

    /**
     * @brief 设置寻址模式（U/V/W 三轴一致）。
     */
    void SetAddressMode(vk::SamplerAddressMode mode);

    /**
     * @brief 开关各向异性过滤。
     */
    void SetAnisotropy(bool enable);

    /// 当前放大过滤方式。
    vk::Filter GetMagFilter() const { return m_MagFilter; }

    /// 当前缩小过滤方式。
    vk::Filter GetMinFilter() const { return m_MinFilter; }

    /// 当前寻址模式（三轴一致时才有意义）。
    vk::SamplerAddressMode GetAddressMode() const { return m_AddressU; }

    /// 当前是否启用各向异性过滤。
    bool GetAnisotropyEnabled() const { return m_AnisotropyEnabled; }

    /**
     * @brief 设置调试名称（同时作用于 Image、ImageView、Sampler）。
     *
     * Image     → name
     * ImageView → name + "_View"
     * Sampler   → name + "_Sampler"
     *
     * 便于在 RenderDoc / Nsight 等调试工具中识别纹理资源。
     *
     * @note Sampler 可能由 VulkanResourceCache 共享，此时会影响所有引用该
     *       Sampler 的纹理。若不希望此行为，可单独对 Image / ImageView 调用
     *       GetImage().SetDebugName() / GetImageView().SetDebugName()。
     */
    void SetDebugName(const std::string &name);

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
     * @brief 在给定 command buffer 上生成 mipmap 链（blit 方式）。
     *
     * 前置条件：level 0 布局为 TransferDstOptimal 且包含有效数据。
     * 后置条件：所有 mip level 布局为 ShaderReadOnlyOptimal。
     */
    void GenerateMipmapsInternal(class VulkanCommandBuffer &cmd);

    /**
     * @brief 创建 ImageView 并通过缓存请求 Sampler。
     */
    void CreateViewAndSampler(VulkanDevice &device,
                              VulkanResourceCache &cache,
                              vk::Filter mag_filter,
                              vk::Filter min_filter);

    /**
     * @brief 按当前记录的采样器参数从缓存重新请求一个 Sampler 并替换。
     *
     * 前置条件：m_SamplerCache 已设置（由 CreateViewAndSampler 记录）。
     */
    void RecreateSampler();

    // ========================================================================
    // 成员
    // ========================================================================

    std::unique_ptr<VulkanImage>     m_Image;
    std::unique_ptr<VulkanImageView> m_ImageView;
    VulkanSampler                   *m_Sampler = nullptr; // 由 VulkanResourceCache 管理
    vk::Format                       m_Format  = vk::Format::eR8G8B8A8Unorm;
    vk::Extent3D                     m_Extent{};
    bool                             m_IsCubeMap = false; ///< 是否为 cubemap（true 时建 eCube 视图）
    std::string                      m_FilePath; ///< 源文件路径（LoadFromFile 时有值）

    // 异步加载状态（空壳纹理专用）
    std::atomic<bool>                m_Ready{false}; ///< 异步加载是否已就绪（同步纹理恒 true）
    std::shared_ptr<AsyncPendingSlot> m_AsyncSlot;   ///< 异步注入槽位（空壳纹理持有）
    VulkanDevice                    *m_AsyncDevice = nullptr; ///< 异步加载用设备（创建视图/采样器）

    // 采样器配置（便捷方法用）：记录当前参数，便于按同样参数重建采样器
    VulkanResourceCache      *m_SamplerCache   = nullptr; // 非拥有，LoadFromX 时记录
    vk::Filter                m_MagFilter      = vk::Filter::eLinear;
    vk::Filter                m_MinFilter      = vk::Filter::eLinear;
    vk::SamplerMipmapMode     m_MipmapMode     = vk::SamplerMipmapMode::eLinear;
    vk::SamplerAddressMode    m_AddressU       = vk::SamplerAddressMode::eRepeat;
    vk::SamplerAddressMode    m_AddressV       = vk::SamplerAddressMode::eRepeat;
    vk::SamplerAddressMode    m_AddressW       = vk::SamplerAddressMode::eRepeat;
    bool                      m_AnisotropyEnabled = false;
    float                     m_MaxAnisotropy  = 0.0f;
    bool                      m_SamplerConfigExplicit = false; ///< 安装前 Set* 显式设置过采样器（异步纹理安装时保留）
};

} // namespace GE