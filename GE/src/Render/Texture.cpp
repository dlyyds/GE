//
// Texture 实现 —— 纹理加载、上传、采样配置
//

#include "Render/Texture.h"
#include "Render/AsyncUploadManager.h"
#include "Render/VulkanBase/VulkanBuffer.h"
#include "Render/VulkanBase/VulkanCommandBuffer.h"
#include "Render/VulkanBase/VulkanImage.h"
#include "Core/Log.h"

#include "stb_image.h"

#include <ktx.h>
#include <ktxvulkan.h>  // ktxTexture_GetVkFormat：KTX1/KTX2 统一取 Vulkan 格式

#include <vulkan/vulkan.hpp>
#include <algorithm>  // std::max

namespace GE {

namespace {
/**
 * @brief 计算完整 mip 链的级数。
 *
 * 公式：floor(log2(max(width, height))) + 1
 * 例如 1024x1024 → 10 级，512x256 → 9 级
 */
uint32_t CalculateMipLevels(uint32_t width, uint32_t height) {
    uint32_t maxDim = std::max(width, height);
    uint32_t levels = 1;
    while (maxDim > 1) {
        maxDim >>= 1;
        levels++;
    }
    return levels;
}

/**
 * @brief ktx 纹理析构器（RAII 用，空指针安全）。
 */
void DestroyKtxTexture(ktxTexture *tex) {
    if (tex) {
        ktxTexture_Destroy(tex);
    }
}

/**
 * @brief 异步加载共享数据容器（decode / upload / finalize 三阶段间传递）。
 *
 * 由主线程组装参数，后台线程写入 decode / upload 结果，最终主线程 finalize
 * 读取。经 shared_ptr 在所有阶段回调间共享，保证后台只产出局部对象（不持
 * Texture 裸指针），所有写 Texture 成员的动作都收敛到主线程 finalize。
 */
struct AsyncLoadData {
    // 加载参数（主线程组装时写入，后台只读）
    std::string filepath;                       ///< 源文件路径（2D/cubemap 解码用）
    vk::Format  format           = vk::Format::eR8G8B8A8Unorm;
    bool        generate_mipmaps = true;

    // decode 输出（后台线程写入）
    std::vector<uint8_t> pixels;                    ///< 2D：RGBA8 像素数据
    int                  width  = 0;                ///< 2D 宽度
    int                  height = 0;                ///< 2D 高度
    std::unique_ptr<ktxTexture, decltype(&DestroyKtxTexture)>
        ktex{nullptr, &DestroyKtxTexture};          ///< cubemap：KTX 对象（保底像素数据）

    // upload 输出（后台线程写入，主线程 finalize 消费）
    std::unique_ptr<VulkanImage>  image;            ///< 后台创建的本地图像
    std::unique_ptr<VulkanBuffer> staging;          ///< staging buffer（GPU 用完后随 bucket 释放）
};

/**
 * @brief 在给定 command buffer 上生成 mipmap 链（blit 方式，同步/异步共用）。
 *
 * 前置条件：level 0 布局为 TransferDstOptimal 且包含有效数据。
 * 后置条件：所有 mip level 布局为 ShaderReadOnlyOptimal。
 */
void GenerateMipmapsInCmd(VulkanCommandBuffer &cmd, VulkanImage &image, vk::Extent3D extent) {
    uint32_t mipLevels = image.get_mip_level_count();
    if (mipLevels <= 1) {
        return;
    }

    auto imageHandle = image.GetHandle();

    // 逐级生成：从 level i-1 blit 到 level i
    for (uint32_t i = 1; i < mipLevels; i++) {
        // 上一级（源）：TransferDst → TransferSrc
        image_utils::TransitionLayout(cmd.GetHandle(), imageHandle,
                                      vk::ImageLayout::eTransferDstOptimal,
                                      vk::ImageLayout::eTransferSrcOptimal,
                                      i - 1, 1);

        // 计算下一级尺寸（每级减半，至少 1x1）
        vk::Extent3D nextExtent{
            std::max(1u, extent.width >> 1),
            std::max(1u, extent.height >> 1),
            1
        };

        // 当前级（目标）：Undefined → TransferDst
        image_utils::TransitionLayout(cmd.GetHandle(), imageHandle,
                                      vk::ImageLayout::eUndefined,
                                      vk::ImageLayout::eTransferDstOptimal,
                                      i, 1);

        // Blit：源 level i-1 → 目标 level i
        vk::ImageBlit blit{};
        blit.srcSubresource.aspectMask = vk::ImageAspectFlagBits::eColor;
        blit.srcSubresource.mipLevel = i - 1;
        blit.srcSubresource.baseArrayLayer = 0;
        blit.srcSubresource.layerCount = 1;
        blit.srcOffsets[0] = vk::Offset3D{0, 0, 0};
        blit.srcOffsets[1] = vk::Offset3D{(int32_t)extent.width, (int32_t)extent.height, 1};

        blit.dstSubresource.aspectMask = vk::ImageAspectFlagBits::eColor;
        blit.dstSubresource.mipLevel = i;
        blit.dstSubresource.baseArrayLayer = 0;
        blit.dstSubresource.layerCount = 1;
        blit.dstOffsets[0] = vk::Offset3D{0, 0, 0};
        blit.dstOffsets[1] = vk::Offset3D{(int32_t)nextExtent.width, (int32_t)nextExtent.height, 1};

        cmd.BlitImage(image, vk::ImageLayout::eTransferSrcOptimal,
                      image, vk::ImageLayout::eTransferDstOptimal,
                      {blit}, vk::Filter::eLinear);

        // 上一级：TransferSrc → ShaderReadOnly（这一级处理完毕）
        image_utils::TransitionLayout(cmd.GetHandle(), imageHandle,
                                      vk::ImageLayout::eTransferSrcOptimal,
                                      vk::ImageLayout::eShaderReadOnlyOptimal,
                                      i - 1, 1);

        extent = nextExtent;
    }

    // 最后一级：TransferDst → ShaderReadOnly
    image_utils::TransitionLayout(cmd.GetHandle(), imageHandle,
                                  vk::ImageLayout::eTransferDstOptimal,
                                  vk::ImageLayout::eShaderReadOnlyOptimal,
                                  mipLevels - 1, 1);
}
} // namespace

// ============================================================================
// 工厂方法：LoadFromFile
// ============================================================================

std::unique_ptr<Texture> Texture::LoadFromFile(
    VulkanDevice &device,
    VulkanResourceCache &cache,
    const std::string &filepath,
    vk::Format format,
    vk::Filter mag_filter,
    vk::Filter min_filter,
    bool generate_mipmaps) {
    // 使用 stb_image 加载文件（强制 RGBA 4 通道）
    int texWidth = 0, texHeight = 0, texChannels = 0;
    unsigned char *pixels = stbi_load(filepath.c_str(), &texWidth, &texHeight, &texChannels, STBI_rgb_alpha);

    if (!pixels) {
        GE_CORE_ERROR("无法加载纹理：{}", filepath);
        return nullptr;
    }

    // 委托给 LoadFromMemory
    auto texture = LoadFromMemory(device, cache, pixels,
                                  static_cast<uint32_t>(texWidth),
                                  static_cast<uint32_t>(texHeight),
                                  format, mag_filter, min_filter,
                                  generate_mipmaps);

    // 记录源文件路径
    if (texture) {
        texture->m_FilePath = filepath;
    }

    // 释放 stb_image 加载的内存
    stbi_image_free(pixels);

    return texture;
}

// ============================================================================
// 工厂方法：LoadCubeMapFromFile
// ============================================================================

std::unique_ptr<Texture> Texture::LoadCubeMapFromFile(
    VulkanDevice &device,
    VulkanResourceCache &cache,
    const std::string &filepath) {
    // 1. 用 libktx 读取 KTX1/KTX2 文件（含像素数据）。
    //    ktxTexture_CreateFromNamedFile 是通用入口，自动识别 KTX1 与 KTX2，
    //    通过 classId 区分；KTX2 用 vkFormat，KTX1 用 glInternalformat。
    ktxTexture *ktex = nullptr;
    KTX_error_code kErr = ktxTexture_CreateFromNamedFile(
        filepath.c_str(), KTX_TEXTURE_CREATE_LOAD_IMAGE_DATA_BIT, &ktex);
    if (kErr != KTX_SUCCESS) {
        GE_CORE_ERROR("无法加载 cubemap ktx: {0} ({1})", filepath, ktxErrorString(kErr));
        return nullptr;
    }
    // RAII 哨兵：函数结束自动释放 ktx 对象
    struct KtxGuard {
        ktxTexture *tex;

        ~KtxGuard() {
            if (tex)
                ktxTexture_Destroy(tex);
        }
    } guard{ktex};

    // 2. 校验：必须是 6 面 cubemap
    if (ktex->numFaces != 6) {
        GE_CORE_ERROR("不是 cubemap（faces={0}）: {1}", ktex->numFaces, filepath);
        return nullptr;
    }

    // 3. 取 Vulkan 格式：ktxTexture_GetVkFormat 按 classId 分发——
    //    KTX2 直接返回内嵌 vkFormat，KTX1 用 libktx 的
    //    vkGetFormatFromOpenGLInternalFormat 自动转换 GL 格式。
    vk::Format format = static_cast<vk::Format>(ktxTexture_GetVkFormat(ktex));
    if (format == vk::Format::eUndefined) {
        GE_CORE_ERROR("无法获取 ktx 的 Vulkan 格式（classId={0}）: {1}",
                      static_cast<uint32_t>(ktex->classId), filepath);
        return nullptr;
    }

    uint32_t width = ktex->baseWidth;
    uint32_t height = ktex->baseHeight;
    uint32_t levels = std::max(1u, static_cast<uint32_t>(ktex->numLevels));
    constexpr uint32_t kFaces = 6;

    // 4. 创建 6 层 cubemap 图像（eCubeCompatible）
    auto texture = std::unique_ptr<Texture>(new Texture(
        device, vk::Extent3D{width, height, 1}, format, {}, levels, kFaces, true));

    // 5. 整块拷贝到 staging buffer，再逐 (level, face) 上传
    auto &graphicsQueue = device.GetQueueByFlags(vk::QueueFlagBits::eGraphics, 0);
    const VulkanQueue &gfxQueue = graphicsQueue;
    auto &uploadCmd = device.RequestCommandBuffer(vk::CommandBufferLevel::ePrimary, true);

    auto stagingBuffer = VulkanBuffer::create_staging_buffer(
        device, static_cast<vk::DeviceSize>(ktex->dataSize), ktex->pData);

    // 布局转换：UNDEFINED -> TRANSFER_DST（全 mip × 6 layer）
    image_utils::TransitionLayout(uploadCmd.GetHandle(), texture->m_Image->GetHandle(),
                                  vk::ImageLayout::eUndefined,
                                  vk::ImageLayout::eTransferDstOptimal,
                                  0, levels, 0, kFaces);

    // KTX1 与 KTX2 的 cubemap 数据均为 level-major：每 level 内 6 个 face 连续排列。
    // ktxTexture_GetImageSize 基类宏按 classId 分发，KTX1 返回的 size 已含
    // rowPadding，故逐 face 拷贝、按 level 基址 + face 偏移计算即可。
    vk::DeviceSize levelByteOffset = 0;
    for (uint32_t l = 0; l < levels; l++) {
        vk::DeviceSize faceSize = ktxTexture_GetImageSize(ktex, l);
        uint32_t lvlW = std::max(1u, width >> l);
        uint32_t lvlH = std::max(1u, height >> l);

        for (uint32_t f = 0; f < kFaces; f++) {
            vk::BufferImageCopy copyRegion{};
            copyRegion.bufferOffset = levelByteOffset + f * faceSize;
            copyRegion.bufferRowLength = 0;
            copyRegion.bufferImageHeight = 0;
            copyRegion.imageSubresource.aspectMask = vk::ImageAspectFlagBits::eColor;
            copyRegion.imageSubresource.mipLevel = l;
            copyRegion.imageSubresource.baseArrayLayer = f;
            copyRegion.imageSubresource.layerCount = 1;
            copyRegion.imageOffset = vk::Offset3D{0, 0, 0};
            copyRegion.imageExtent = vk::Extent3D{lvlW, lvlH, 1};

            uploadCmd.GetHandle().copyBufferToImage(
                stagingBuffer.GetHandle(), texture->m_Image->GetHandle(),
                vk::ImageLayout::eTransferDstOptimal, copyRegion);
        }
        levelByteOffset += static_cast<vk::DeviceSize>(kFaces) * faceSize;
    }

    // 布局转换：TRANSFER_DST -> SHADER_READ_ONLY（全 mip × 6 layer）
    image_utils::TransitionLayout(uploadCmd.GetHandle(), texture->m_Image->GetHandle(),
                                  vk::ImageLayout::eTransferDstOptimal,
                                  vk::ImageLayout::eShaderReadOnlyOptimal,
                                  0, levels, 0, kFaces);

    uploadCmd.End();
    device.FlushCommandBuffer(uploadCmd, gfxQueue);

    // 6. 创建 eCube 视图 + 请求 sampler（cubemap 自动用 ClampToEdge）
    texture->CreateViewAndSampler(device, cache, vk::Filter::eLinear, vk::Filter::eLinear);
    texture->m_FilePath = filepath;

    return texture;
}

// ============================================================================
// 工厂方法：异步加载（LoadFromFileAsync / LoadFromMemoryAsync / LoadCubeMapFromFileAsync）
// ============================================================================
// 统一模型：
//   1. 主线程创建空壳纹理（m_Image 空，m_Ready=false），组装 AsyncLoadData 与 UploadTask。
//   2. 后台线程执行 decode（stb/KTX 解码 → CPU 数据）与 upload（创建 local image + staging，
//      录 copy / 布局转换 / mip blit）。
//   3. 主线程每帧 Poll()，fence 完成后执行 finalize：经 AsyncPendingSlot 门控后调用
//      InstallAsyncImage 注入，置 m_Ready=true。
// 后台线程只产出 local 对象，绝不写 Texture 成员；staging 由 AsyncLoadData 持有，
// 随任务 finalize（主线程、GPU 完成后）释放。

std::unique_ptr<Texture> Texture::LoadFromFileAsync(
    VulkanDevice &device, VulkanResourceCache &cache, AsyncUploadManager &upload,
    const std::string &filepath, vk::Format format,
    vk::Filter mag_filter, vk::Filter min_filter, bool generate_mipmaps) {

    // 空壳纹理：无 GPU 图像，注入槽位指向自身
    auto texture = std::unique_ptr<Texture>(new Texture());
    texture->m_FilePath   = filepath;
    texture->m_Format     = format;
    texture->m_AsyncDevice = &device;
    texture->m_AsyncSlot  = std::make_shared<AsyncPendingSlot>();
    texture->m_AsyncSlot->target = texture.get();

    auto bucket = std::make_shared<AsyncLoadData>();
    bucket->filepath         = filepath;
    bucket->format           = format;
    bucket->generate_mipmaps = generate_mipmaps;

    // finalize 捕获的注入目标（shared_ptr 槽位，空壳销毁后自动作废）
    auto slot = texture->m_AsyncSlot;

    AsyncUploadManager::UploadTask task;
    task.decode = [bucket] {
        // 后台：stb_image 解码为 RGBA8
        int channels = 0;
        unsigned char *pixels = stbi_load(bucket->filepath.c_str(),
                                          &bucket->width, &bucket->height, &channels, STBI_rgb_alpha);
        if (!pixels) {
            GE_CORE_ERROR("纹理异步加载失败（解码）：{0}", bucket->filepath);
            return;
        }
        bucket->pixels.assign(pixels, pixels + static_cast<size_t>(bucket->width) * bucket->height * 4);
        stbi_image_free(pixels);
        GE_CORE_TRACE("纹理异步解码完成: {0} ({1}x{2} RGBA8)", bucket->filepath,
                      bucket->width, bucket->height);
    };

    task.upload = [&device, bucket](VulkanCommandBuffer &cmd) {
        // 后台：创建 local image + staging，录 copy + 布局转换 + mip blit
        if (bucket->pixels.empty()) {
            return; // 解码失败，不创建图像；finalize 见到空 image 会跳过注入
        }
        const uint32_t width  = static_cast<uint32_t>(bucket->width);
        const uint32_t height = static_cast<uint32_t>(bucket->height);
        uint32_t mipLevels = bucket->generate_mipmaps ? CalculateMipLevels(width, height) : 1;

        vk::ImageUsageFlags usage = vk::ImageUsageFlagBits::eTransferDst
                                    | vk::ImageUsageFlagBits::eSampled;
        if (mipLevels > 1) {
            usage |= vk::ImageUsageFlagBits::eTransferSrc;
        }

        VulkanImageBuilder builder(vk::Extent3D{width, height, 1});
        builder.with_format(bucket->format)
            .with_usage(usage)
            .with_mip_levels(mipLevels);
        bucket->image = builder.build_unique(device);

        // staging buffer（拷贝解码出的 RGBA8 像素）
        const vk::DeviceSize imageSize = static_cast<vk::DeviceSize>(bucket->pixels.size());
        bucket->staging = std::make_unique<VulkanBuffer>(
            std::move(VulkanBuffer::create_staging_buffer(device, imageSize, bucket->pixels.data())));

        // 布局转换：UNDEFINED -> TRANSFER_DST（仅 level 0）
        image_utils::TransitionLayout(cmd.GetHandle(), bucket->image->GetHandle(),
                                      vk::ImageLayout::eUndefined,
                                      vk::ImageLayout::eTransferDstOptimal,
                                      0, 1);

        // 拷贝 staging → 图像（仅 level 0）
        vk::BufferImageCopy copyRegion{};
        copyRegion.bufferOffset = 0;
        copyRegion.bufferRowLength = 0;
        copyRegion.bufferImageHeight = 0;
        copyRegion.imageSubresource.aspectMask = vk::ImageAspectFlagBits::eColor;
        copyRegion.imageSubresource.mipLevel = 0;
        copyRegion.imageSubresource.baseArrayLayer = 0;
        copyRegion.imageSubresource.layerCount = 1;
        copyRegion.imageOffset = vk::Offset3D{0, 0, 0};
        copyRegion.imageExtent = vk::Extent3D{width, height, 1};

        cmd.GetHandle().copyBufferToImage(bucket->staging->GetHandle(), bucket->image->GetHandle(),
                                          vk::ImageLayout::eTransferDstOptimal, copyRegion);

        if (mipLevels > 1) {
            // 同一 command buffer 内生成 mip 链（level 0 当前为 TransferDst）
            GenerateMipmapsInCmd(cmd, *bucket->image, vk::Extent3D{width, height, 1});
        } else {
            // 单级 mip：直接转到 ShaderReadOnly
            image_utils::TransitionLayout(cmd.GetHandle(), bucket->image->GetHandle(),
                                          vk::ImageLayout::eTransferDstOptimal,
                                          vk::ImageLayout::eShaderReadOnlyOptimal,
                                          0, 1);
        }
    };

    task.finalize = [slot, bucket, &cache, mag_filter, min_filter] {
        // 主线程：空壳已销毁或加载失败（无图像）则跳过注入，避免 use-after-free
        if (!slot->abandoned && slot->target && bucket->image) {
            slot->target->InstallAsyncImage(std::move(bucket->image), cache, mag_filter, min_filter);
        }
    };

    upload.Submit(std::move(task));
    GE_CORE_INFO("纹理异步加载提交: {0}", filepath);
    return texture;
}

std::unique_ptr<Texture> Texture::LoadFromMemoryAsync(
    VulkanDevice &device, VulkanResourceCache &cache, AsyncUploadManager &upload,
    const void *pixels, uint32_t width, uint32_t height, vk::Format format,
    vk::Filter mag_filter, vk::Filter min_filter, bool generate_mipmaps) {

    auto texture = std::unique_ptr<Texture>(new Texture());
    texture->m_Format     = format;
    texture->m_AsyncDevice = &device;
    texture->m_AsyncSlot  = std::make_shared<AsyncPendingSlot>();
    texture->m_AsyncSlot->target = texture.get();

    auto bucket = std::make_shared<AsyncLoadData>();
    bucket->format           = format;
    bucket->generate_mipmaps = generate_mipmaps;
    bucket->width            = static_cast<int>(width);
    bucket->height           = static_cast<int>(height);
    // 像素须拷贝到内部容器（调用方可能在返回后释放）
    const uint8_t *src = static_cast<const uint8_t *>(pixels);
    bucket->pixels.assign(src, src + static_cast<size_t>(width) * height * 4);

    auto slot = texture->m_AsyncSlot;

    AsyncUploadManager::UploadTask task;
    // 像素已就绪，无需 decode 阶段
    task.upload = [&device, bucket](VulkanCommandBuffer &cmd) {
        const uint32_t w  = bucket->width;
        const uint32_t h  = static_cast<uint32_t>(bucket->height);
        uint32_t mipLevels = bucket->generate_mipmaps ? CalculateMipLevels(w, h) : 1;

        vk::ImageUsageFlags usage = vk::ImageUsageFlagBits::eTransferDst
                                    | vk::ImageUsageFlagBits::eSampled;
        if (mipLevels > 1) {
            usage |= vk::ImageUsageFlagBits::eTransferSrc;
        }

        VulkanImageBuilder builder(vk::Extent3D{w, h, 1});
        builder.with_format(bucket->format)
            .with_usage(usage)
            .with_mip_levels(mipLevels);
        bucket->image = builder.build_unique(device);

        const vk::DeviceSize imageSize = static_cast<vk::DeviceSize>(bucket->pixels.size());
        bucket->staging = std::make_unique<VulkanBuffer>(
            std::move(VulkanBuffer::create_staging_buffer(device, imageSize, bucket->pixels.data())));

        image_utils::TransitionLayout(cmd.GetHandle(), bucket->image->GetHandle(),
                                      vk::ImageLayout::eUndefined,
                                      vk::ImageLayout::eTransferDstOptimal,
                                      0, 1);

        vk::BufferImageCopy copyRegion{};
        copyRegion.bufferOffset = 0;
        copyRegion.bufferRowLength = 0;
        copyRegion.bufferImageHeight = 0;
        copyRegion.imageSubresource.aspectMask = vk::ImageAspectFlagBits::eColor;
        copyRegion.imageSubresource.mipLevel = 0;
        copyRegion.imageSubresource.baseArrayLayer = 0;
        copyRegion.imageSubresource.layerCount = 1;
        copyRegion.imageOffset = vk::Offset3D{0, 0, 0};
        copyRegion.imageExtent = vk::Extent3D{w, h, 1};

        cmd.GetHandle().copyBufferToImage(bucket->staging->GetHandle(), bucket->image->GetHandle(),
                                          vk::ImageLayout::eTransferDstOptimal, copyRegion);

        if (mipLevels > 1) {
            GenerateMipmapsInCmd(cmd, *bucket->image, vk::Extent3D{w, h, 1});
        } else {
            image_utils::TransitionLayout(cmd.GetHandle(), bucket->image->GetHandle(),
                                          vk::ImageLayout::eTransferDstOptimal,
                                          vk::ImageLayout::eShaderReadOnlyOptimal,
                                          0, 1);
        }
    };

    task.finalize = [slot, bucket, &cache, mag_filter, min_filter] {
        if (!slot->abandoned && slot->target && bucket->image) {
            slot->target->InstallAsyncImage(std::move(bucket->image), cache, mag_filter, min_filter);
        }
    };

    upload.Submit(std::move(task));
    GE_CORE_INFO("纹理异步加载提交: (内存 {0}x{1})", width, height);
    return texture;
}

std::unique_ptr<Texture> Texture::LoadCubeMapFromFileAsync(
    VulkanDevice &device, VulkanResourceCache &cache, AsyncUploadManager &upload,
    const std::string &filepath) {

    auto texture = std::unique_ptr<Texture>(new Texture());
    texture->m_FilePath   = filepath;
    texture->m_IsCubeMap  = true;
    texture->m_AsyncDevice = &device;
    texture->m_AsyncSlot  = std::make_shared<AsyncPendingSlot>();
    texture->m_AsyncSlot->target = texture.get();

    auto bucket = std::make_shared<AsyncLoadData>();
    bucket->filepath = filepath;

    auto slot = texture->m_AsyncSlot;

    AsyncUploadManager::UploadTask task;
    task.decode = [bucket] {
        // 后台：libktx 读取 KTX1/KTX2（含像素数据）
        ktxTexture *ktex = nullptr;
        KTX_error_code kErr = ktxTexture_CreateFromNamedFile(
            bucket->filepath.c_str(), KTX_TEXTURE_CREATE_LOAD_IMAGE_DATA_BIT, &ktex);
        if (kErr != KTX_SUCCESS) {
            GE_CORE_ERROR("cubemap 异步加载失败（解码）：{0} ({1})",
                          bucket->filepath, ktxErrorString(kErr));
            return;
        }
        bucket->ktex.reset(ktex);
        GE_CORE_TRACE("cubemap 异步解码完成: {0} ({1}x{2} mip={3})", bucket->filepath,
                      ktex->baseWidth, ktex->baseHeight, ktex->numLevels);
    };

    task.upload = [&device, bucket](VulkanCommandBuffer &cmd) {
        // 后台：校验为 6 面 cubemap，创建 local cube image + staging，逐 (level, face) 上传
        ktxTexture *ktex = bucket->ktex.get();
        if (!ktex || ktex->numFaces != 6) {
            GE_CORE_ERROR("cubemap 异步上传失败（非 6 面或已解码）: {0}", bucket->filepath);
            return;
        }

        vk::Format format = static_cast<vk::Format>(ktxTexture_GetVkFormat(ktex));
        if (format == vk::Format::eUndefined) {
            GE_CORE_ERROR("cubemap 异步上传失败（无法取 Vulkan 格式）: {0}", bucket->filepath);
            return;
        }

        const uint32_t width  = ktex->baseWidth;
        const uint32_t height = ktex->baseHeight;
        const uint32_t levels = std::max(1u, static_cast<uint32_t>(ktex->numLevels));
        constexpr uint32_t kFaces = 6;

        vk::ImageUsageFlags usage = vk::ImageUsageFlagBits::eTransferDst
                                    | vk::ImageUsageFlagBits::eSampled;
        VulkanImageBuilder builder(vk::Extent3D{width, height, 1});
        builder.with_format(format)
            .with_usage(usage)
            .with_mip_levels(levels)
            .with_array_layers(kFaces)
            .with_flags(vk::ImageCreateFlagBits::eCubeCompatible);
        bucket->image = builder.build_unique(device);

        // 整块拷贝到 staging，再逐 (level, face) 上传
        bucket->staging = std::make_unique<VulkanBuffer>(std::move(
            VulkanBuffer::create_staging_buffer(device,
                static_cast<vk::DeviceSize>(ktex->dataSize), ktex->pData)));

        // 布局转换：UNDEFINED -> TRANSFER_DST（全 mip × 6 layer）
        image_utils::TransitionLayout(cmd.GetHandle(), bucket->image->GetHandle(),
                                      vk::ImageLayout::eUndefined,
                                      vk::ImageLayout::eTransferDstOptimal,
                                      0, levels, 0, kFaces);

        vk::DeviceSize levelByteOffset = 0;
        for (uint32_t l = 0; l < levels; l++) {
            vk::DeviceSize faceSize = ktxTexture_GetImageSize(ktex, l);
            uint32_t lvlW = std::max(1u, width >> l);
            uint32_t lvlH = std::max(1u, height >> l);

            for (uint32_t f = 0; f < kFaces; f++) {
                vk::BufferImageCopy copyRegion{};
                copyRegion.bufferOffset = levelByteOffset + f * faceSize;
                copyRegion.bufferRowLength = 0;
                copyRegion.bufferImageHeight = 0;
                copyRegion.imageSubresource.aspectMask = vk::ImageAspectFlagBits::eColor;
                copyRegion.imageSubresource.mipLevel = l;
                copyRegion.imageSubresource.baseArrayLayer = f;
                copyRegion.imageSubresource.layerCount = 1;
                copyRegion.imageOffset = vk::Offset3D{0, 0, 0};
                copyRegion.imageExtent = vk::Extent3D{lvlW, lvlH, 1};

                cmd.GetHandle().copyBufferToImage(
                    bucket->staging->GetHandle(), bucket->image->GetHandle(),
                    vk::ImageLayout::eTransferDstOptimal, copyRegion);
            }
            levelByteOffset += static_cast<vk::DeviceSize>(kFaces) * faceSize;
        }

        // 布局转换：TRANSFER_DST -> SHADER_READ_ONLY（全 mip × 6 layer）
        image_utils::TransitionLayout(cmd.GetHandle(), bucket->image->GetHandle(),
                                      vk::ImageLayout::eTransferDstOptimal,
                                      vk::ImageLayout::eShaderReadOnlyOptimal,
                                      0, levels, 0, kFaces);
    };

    task.finalize = [slot, bucket, &cache] {
        if (!slot->abandoned && slot->target && bucket->image) {
            slot->target->InstallAsyncImage(std::move(bucket->image), cache,
                                            vk::Filter::eLinear, vk::Filter::eLinear);
        }
    };

    upload.Submit(std::move(task));
    GE_CORE_INFO("cubemap 异步加载提交: {0}", filepath);
    return texture;
}

// ============================================================================
// 工厂方法：LoadFromMemory
// ============================================================================

std::unique_ptr<Texture> Texture::LoadFromMemory(
    VulkanDevice &device,
    VulkanResourceCache &cache,
    const void *pixels,
    uint32_t width, uint32_t height,
    vk::Format format,
    vk::Filter mag_filter,
    vk::Filter min_filter,
    bool generate_mipmaps) {
    uint32_t mipLevels = generate_mipmaps ? CalculateMipLevels(width, height) : 1;

    auto texture = std::unique_ptr<Texture>(
        new Texture(device, vk::Extent3D{width, height, 1}, format, {}, mipLevels));

    // 上传像素数据到 GPU（包含 mipmap 生成）
    texture->UploadPixels(device, pixels, width, height);

    // 创建 ImageView 和请求 Sampler
    texture->CreateViewAndSampler(device, cache, mag_filter, min_filter);

    return texture;
}

// ============================================================================
// 构造函数（空白纹理）
// ============================================================================

Texture::Texture(VulkanDevice &device,
                 vk::Extent3D extent,
                 vk::Format format,
                 vk::ImageUsageFlags extra_usage,
                 uint32_t mip_levels)
    : Texture(device, extent, format, extra_usage, mip_levels, 1, false) {
}

// ============================================================================
// 空壳构造（异步加载用）：不创建图像，m_Image 为空，m_Ready=false
// ============================================================================

Texture::Texture() = default;

Texture::Texture(VulkanDevice &device,
                 vk::Extent3D extent,
                 vk::Format format,
                 vk::ImageUsageFlags extra_usage,
                 uint32_t mip_levels,
                 uint32_t array_layers,
                 bool cube_map)
    : m_Format(format)
      , m_Extent(extent)
      , m_IsCubeMap(cube_map)
      , m_Ready(true) {  // 同步构造（含 LoadFromFile/Memory、直接构造）立即就绪
    // 基础用法：传输目标 + 可采样
    vk::ImageUsageFlags usage = vk::ImageUsageFlagBits::eTransferDst
                                | vk::ImageUsageFlagBits::eSampled
                                | extra_usage;

    // 需要生成 mipmap 时，添加 TransferSrc（作为 blit 源）
    if (mip_levels > 1) {
        usage |= vk::ImageUsageFlagBits::eTransferSrc;
    }

    vk::ImageCreateFlags flags = cube_map
                                     ? vk::ImageCreateFlagBits::eCubeCompatible
                                     : vk::ImageCreateFlagBits{};

    VulkanImageBuilder builder(extent);
    builder.with_format(format)
        .with_usage(usage)
        .with_mip_levels(mip_levels)
        .with_array_layers(array_layers)
        .with_flags(flags);

    m_Image = std::make_unique<VulkanImage>(device, builder);
}

// ============================================================================
// 析构函数
// ============================================================================

Texture::~Texture() {
    // 空壳纹理销毁时作废异步注入槽位，使在途 finalize 安全跳过注入
    if (m_AsyncSlot) {
        m_AsyncSlot->abandoned = true;
        m_AsyncSlot->target    = nullptr;
    }
}

// ============================================================================
// 移动构造
// ============================================================================

Texture::Texture(Texture &&other) noexcept
    : m_Image(std::move(other.m_Image))
      , m_ImageView(std::move(other.m_ImageView))
      , m_Sampler(other.m_Sampler)
      , m_Format(other.m_Format)
      , m_Extent(other.m_Extent)
      , m_IsCubeMap(other.m_IsCubeMap)
      , m_FilePath(std::move(other.m_FilePath))
      , m_Ready(other.m_Ready.load())
      , m_AsyncSlot(other.m_AsyncSlot)
      , m_AsyncDevice(other.m_AsyncDevice)
      , m_SamplerCache(other.m_SamplerCache)
      , m_MagFilter(other.m_MagFilter)
      , m_MinFilter(other.m_MinFilter)
      , m_MipmapMode(other.m_MipmapMode)
      , m_AddressU(other.m_AddressU)
      , m_AddressV(other.m_AddressV)
      , m_AddressW(other.m_AddressW)
      , m_AnisotropyEnabled(other.m_AnisotropyEnabled)
      , m_MaxAnisotropy(other.m_MaxAnisotropy)
      , m_SamplerConfigExplicit(other.m_SamplerConfigExplicit) {
    other.m_Sampler = nullptr;
    other.m_Format = vk::Format::eR8G8B8A8Unorm;
    other.m_Extent = vk::Extent3D{};
    other.m_SamplerCache = nullptr;
    other.m_AsyncDevice  = nullptr;

    // 转移后把注入槽位目标重定向到新对象，避免在途 finalize 注入进已移动的空壳
    if (m_AsyncSlot) {
        m_AsyncSlot->target = this;
    }
}

// ============================================================================
// 获取描述符信息
// ============================================================================

vk::DescriptorImageInfo Texture::GetDescriptorInfo() const {
    vk::DescriptorImageInfo info{};
    info.sampler = m_Sampler ? m_Sampler->GetHandle() : VK_NULL_HANDLE;
    info.imageView = m_ImageView ? m_ImageView->GetHandle() : VK_NULL_HANDLE;
    info.imageLayout = vk::ImageLayout::eShaderReadOnlyOptimal;
    return info;
}

// ============================================================================
// 内部：上传像素数据到 GPU 图像
// ============================================================================

void Texture::UploadPixels(VulkanDevice &device, const void *pixels,
                           uint32_t width, uint32_t height) {
    vk::DeviceSize imageSize = static_cast<vk::DeviceSize>(width * height * 4);

    // 创建 staging buffer 并拷贝像素数据
    auto stagingBuffer = VulkanBuffer::create_staging_buffer(device, imageSize, pixels);

    // 获取 graphics queue（用于上传后的 flush）
    auto &graphicsQueue = device.GetQueueByFlags(vk::QueueFlagBits::eGraphics, 0);
    const VulkanQueue &gfxQueue = graphicsQueue;

    // 获取临时 command buffer
    auto &uploadCmd = device.RequestCommandBuffer(vk::CommandBufferLevel::ePrimary, true);

    uint32_t mipLevels = m_Image->get_mip_level_count();

    // 布局转换：UNDEFINED -> TRANSFER_DST（仅 level 0）
    image_utils::TransitionLayout(uploadCmd.GetHandle(), m_Image->GetHandle(),
                                  vk::ImageLayout::eUndefined,
                                  vk::ImageLayout::eTransferDstOptimal,
                                  0, 1);

    // 拷贝 staging buffer 到纹理图像（仅 level 0）
    vk::BufferImageCopy copyRegion{};
    copyRegion.bufferOffset = 0;
    copyRegion.bufferRowLength = 0;
    copyRegion.bufferImageHeight = 0;
    copyRegion.imageSubresource.aspectMask = vk::ImageAspectFlagBits::eColor;
    copyRegion.imageSubresource.mipLevel = 0;
    copyRegion.imageSubresource.baseArrayLayer = 0;
    copyRegion.imageSubresource.layerCount = 1;
    copyRegion.imageOffset = vk::Offset3D{0, 0, 0};
    copyRegion.imageExtent = vk::Extent3D{width, height, 1};

    uploadCmd.GetHandle().copyBufferToImage(stagingBuffer.GetHandle(), m_Image->GetHandle(),
                                            vk::ImageLayout::eTransferDstOptimal, copyRegion);

    // staging buffer 在 copy 完成后即可释放（GPU 已读完），
    // 但因为我们在同一个 command buffer 里继续做 mip blit，
    // 这里先不做 barrier，等 mip 生成完一起提交。

    if (mipLevels > 1) {
        // 在同一个 command buffer 中生成 mipmap 链
        // 注意：level 0 当前布局为 TransferDstOptimal
        GenerateMipmapsInternal(uploadCmd);
    } else {
        // 单级 mip：直接转到 ShaderReadOnly
        image_utils::TransitionLayout(uploadCmd.GetHandle(), m_Image->GetHandle(),
                                      vk::ImageLayout::eTransferDstOptimal,
                                      vk::ImageLayout::eShaderReadOnlyOptimal,
                                      0, 1);
    }

    // 提交并等待完成
    uploadCmd.End();
    device.FlushCommandBuffer(uploadCmd, gfxQueue);

    // staging buffer 在此处自动析构
}

// ============================================================================
// 生成 mipmap 链（公开方法，独立 command buffer）
// ============================================================================

void Texture::GenerateMipmaps(VulkanDevice &device) {
    uint32_t mipLevels = m_Image->get_mip_level_count();
    if (mipLevels <= 1) {
        return; // 没有 mip 可生成
    }

    // 获取 graphics queue
    auto &graphicsQueue = device.GetQueueByFlags(vk::QueueFlagBits::eGraphics, 0);
    const VulkanQueue &gfxQueue = graphicsQueue;

    // 获取临时 command buffer
    auto &cmd = device.RequestCommandBuffer(vk::CommandBufferLevel::ePrimary, true);

    // 前置假设：level 0 已经是 TransferDstOptimal 或 ShaderReadOnlyOptimal
    // 如果是 ShaderReadOnly，先转回 TransferDst
    // （这里统一转一次 TransferDst，安全起见）
    image_utils::TransitionLayout(cmd.GetHandle(), m_Image->GetHandle(),
                                  vk::ImageLayout::eShaderReadOnlyOptimal,
                                  vk::ImageLayout::eTransferDstOptimal,
                                  0, 1);

    // 在 command buffer 上执行 mip 生成
    GenerateMipmapsInternal(cmd);

    // 提交并等待完成
    cmd.End();
    device.FlushCommandBuffer(cmd, gfxQueue);
}

// ============================================================================
// 内部：在已有 command buffer 上生成 mipmap 链（blit 方式）
// ============================================================================
// 前置条件：level 0 布局为 TransferDstOptimal，包含有效数据
// 后置条件：所有 mip level 布局为 ShaderReadOnlyOptimal
// ============================================================================

void Texture::GenerateMipmapsInternal(VulkanCommandBuffer &cmd) {
    // 委托给共享的 blit 辅助函数（同步 / 异步上传共用同一套 mip 生成逻辑）
    GenerateMipmapsInCmd(cmd, *m_Image, m_Extent);
}

// ============================================================================
// 设置调试名称
// ============================================================================

void Texture::SetDebugName(const std::string &name) {
    if (m_Image) {
        m_Image->SetDebugName(name);
    }
    if (m_ImageView) {
        m_ImageView->SetDebugName(name + "_View");
    }
    if (m_Sampler) {
        m_Sampler->SetDebugName(name + "_Sampler");
    }
}

// ============================================================================
// 异步：安装后台加载完成的本地图像
// ============================================================================

void Texture::InstallAsyncImage(std::unique_ptr<VulkanImage> image,
                                VulkanResourceCache &cache,
                                vk::Filter mag_filter,
                                vk::Filter min_filter) {
    // 记录安装前显式设置的采样器配置（SetAddressMode 等在安装前即可调用，
    // 如 BRDF LUT 的 ClampToEdge），安装后恢复，避免被 CreateViewAndSampler 覆盖。
    // 注意不恢复 m_MaxAnisotropy：它安装前是未初始化的 0，须保持
    // CreateViewAndSampler 写入的设备上限（≥1），否则 anisotropyEnable=true 时
    // maxAnisotropy=0 触发 Vulkan 校验错误。
    const bool explicitCfg = m_SamplerConfigExplicit;
    const vk::SamplerAddressMode aU = m_AddressU, aV = m_AddressV, aW = m_AddressW;
    const vk::Filter mg = m_MagFilter, mn = m_MinFilter;
    const bool aniso = m_AnisotropyEnabled;

    // 注入后台创建的本地图像（此前 m_Image 为空）
    m_Image = std::move(image);
    m_Extent = m_Image->get_extent();
    m_Format = m_Image->get_format();

    // 创建 ImageView 并请求 Sampler（cubemap 自动建 eCube 视图）
    CreateViewAndSampler(*m_AsyncDevice, cache, mag_filter, min_filter);

    // 安装前若显式配置过采样器，则恢复并按配置重建采样器
    // （m_MaxAnisotropy 保持 CreateViewAndSampler 写入的设备上限，见上方注释）
    if (explicitCfg) {
        m_AddressU = aU;
        m_AddressV = aV;
        m_AddressW = aW;
        m_MagFilter = mg;
        m_MinFilter = mn;
        m_AnisotropyEnabled = aniso;
        RecreateSampler();
    }

    // 置就绪，下帧渲染自动可见
    m_Ready.store(true, std::memory_order_release);

    GE_CORE_INFO("纹理异步就绪: {0} ({1}x{2}x{3} mip={4} 格式={5}{6})",
                 m_FilePath.empty() ? "(内存)" : m_FilePath,
                 m_Extent.width, m_Extent.height, m_Extent.depth,
                 m_Image->get_mip_level_count(),
                 vk::to_string(m_Format),
                 m_IsCubeMap ? " cubemap" : "");
}

// ============================================================================
// 内部：创建 ImageView 和请求 Sampler
// ============================================================================

void Texture::CreateViewAndSampler(VulkanDevice &device,
                                   VulkanResourceCache &cache,
                                   vk::Filter mag_filter,
                                   vk::Filter min_filter) {
    // 创建 ImageView：cubemap 用 eCube 视图，普通纹理用 e2D
    vk::ImageViewType viewType = m_IsCubeMap
                                     ? vk::ImageViewType::eCube
                                     : vk::ImageViewType::e2D;
    m_ImageView = std::make_unique<VulkanImageView>(
        *m_Image,
        viewType,
        m_Format);

    // 查询设备支持的最大各向异性级别
    float maxAnisotropy = device.GetGpu().GetProperties().limits.maxSamplerAnisotropy;
    // 设备支持各向异性时才启用（基本所有现代 GPU 都支持，这里做个保险判断）
    vk::Bool32 enableAnisotropy = (maxAnisotropy > 1.0f) ? VK_TRUE : VK_FALSE;

    // 记录采样器配置与缓存，供后续 Set* 便捷方法按同样参数重建采样器
    m_SamplerCache = &cache;
    m_MagFilter = mag_filter;
    m_MinFilter = min_filter;
    m_MipmapMode = vk::SamplerMipmapMode::eLinear;
    // cubemap 不允许 Repeat 寻址，统一用 ClampToEdge（越界采样边缘）
    vk::SamplerAddressMode addressMode = m_IsCubeMap
                                             ? vk::SamplerAddressMode::eClampToEdge
                                             : vk::SamplerAddressMode::eRepeat;
    m_AddressU = addressMode;
    m_AddressV = addressMode;
    m_AddressW = addressMode;
    m_AnisotropyEnabled = (enableAnisotropy == VK_TRUE);
    m_MaxAnisotropy = maxAnisotropy;

    // 通过缓存获取 Sampler（启用各向异性过滤，提升曲面纹理质量）
    RecreateSampler();
}

// ============================================================================
// 采样器便捷修改
// ============================================================================

void Texture::RecreateSampler() {
    if (!m_SamplerCache) {
        return; // 空白纹理未记录缓存，无法按需重建采样器
    }
    m_Sampler = &m_SamplerCache->RequestSampler(
        m_MagFilter, // mag
        m_MinFilter, // min
        m_MipmapMode, // mipmap
        m_AddressU, // address U
        m_AddressV, // address V
        m_AddressW, // address W
        0.0f, // mip_lod_bias
        m_AnisotropyEnabled ? VK_TRUE : VK_FALSE, // anisotropy_enable
        m_MaxAnisotropy); // max_anisotropy
}

void Texture::SetFilter(vk::Filter mag_filter, vk::Filter min_filter) {
    m_MagFilter = mag_filter;
    m_MinFilter = min_filter;
    m_SamplerConfigExplicit = true;
    RecreateSampler();
}

void Texture::SetAddressMode(vk::SamplerAddressMode mode) {
    m_AddressU = mode;
    m_AddressV = mode;
    m_AddressW = mode;
    m_SamplerConfigExplicit = true;
    RecreateSampler();
}

void Texture::SetAnisotropy(bool enable) {
    m_AnisotropyEnabled = enable;
    m_SamplerConfigExplicit = true;
    RecreateSampler();
}

} // namespace GE