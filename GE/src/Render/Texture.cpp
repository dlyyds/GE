//
// Texture 实现 —— 纹理加载、上传、采样配置
//

#include "Render/Texture.h"
#include "Render/VulkanBase/VulkanBuffer.h"
#include "Render/VulkanBase/VulkanCommandBuffer.h"
#include "Render/VulkanBase/VulkanImage.h"
#include "Core/Log.h"

#include "stb_image.h"

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
    bool generate_mipmaps)
{
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
    bool generate_mipmaps)
{
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
    : m_Format(format)
    , m_Extent(extent)
{
    // 基础用法：传输目标 + 可采样
    vk::ImageUsageFlags usage = vk::ImageUsageFlagBits::eTransferDst
                              | vk::ImageUsageFlagBits::eSampled
                              | extra_usage;

    // 需要生成 mipmap 时，添加 TransferSrc（作为 blit 源）
    if (mip_levels > 1) {
        usage |= vk::ImageUsageFlagBits::eTransferSrc;
    }

    VulkanImageBuilder builder(extent);
    builder.with_format(format)
           .with_usage(usage)
           .with_mip_levels(mip_levels);

    m_Image = std::make_unique<VulkanImage>(device, builder);
}

// ============================================================================
// 析构函数
// ============================================================================

Texture::~Texture() = default;

// ============================================================================
// 移动构造
// ============================================================================

Texture::Texture(Texture &&other) noexcept
    : m_Image(std::move(other.m_Image))
    , m_ImageView(std::move(other.m_ImageView))
    , m_Sampler(other.m_Sampler)
    , m_Format(other.m_Format)
    , m_Extent(other.m_Extent)
{
    other.m_Sampler = nullptr;
    other.m_Format  = vk::Format::eR8G8B8A8Unorm;
    other.m_Extent  = vk::Extent3D{};
}

// ============================================================================
// 获取描述符信息
// ============================================================================

vk::DescriptorImageInfo Texture::GetDescriptorInfo() const
{
    vk::DescriptorImageInfo info{};
    info.sampler     = m_Sampler ? m_Sampler->GetHandle() : VK_NULL_HANDLE;
    info.imageView   = m_ImageView ? m_ImageView->GetHandle() : VK_NULL_HANDLE;
    info.imageLayout = vk::ImageLayout::eShaderReadOnlyOptimal;
    return info;
}

// ============================================================================
// 内部：上传像素数据到 GPU 图像
// ============================================================================

void Texture::UploadPixels(VulkanDevice &device, const void *pixels,
                           uint32_t width, uint32_t height)
{
    vk::DeviceSize imageSize = static_cast<vk::DeviceSize>(width * height * 4);

    // 创建 staging buffer 并拷贝像素数据
    auto stagingBuffer = VulkanBuffer::create_staging_buffer(device, imageSize, pixels);

    // 获取 graphics queue（用于上传后的 flush）
    auto &graphicsQueue = device.GetQueueByFlags(vk::QueueFlagBits::eGraphics, 0);
    vk::Queue gfxQueue = graphicsQueue.GetHandle();

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
    copyRegion.bufferOffset              = 0;
    copyRegion.bufferRowLength           = 0;
    copyRegion.bufferImageHeight         = 0;
    copyRegion.imageSubresource.aspectMask     = vk::ImageAspectFlagBits::eColor;
    copyRegion.imageSubresource.mipLevel       = 0;
    copyRegion.imageSubresource.baseArrayLayer = 0;
    copyRegion.imageSubresource.layerCount     = 1;
    copyRegion.imageOffset               = vk::Offset3D{0, 0, 0};
    copyRegion.imageExtent               = vk::Extent3D{width, height, 1};

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

void Texture::GenerateMipmaps(VulkanDevice &device)
{
    uint32_t mipLevels = m_Image->get_mip_level_count();
    if (mipLevels <= 1) {
        return; // 没有 mip 可生成
    }

    // 获取 graphics queue
    auto &graphicsQueue = device.GetQueueByFlags(vk::QueueFlagBits::eGraphics, 0);
    vk::Queue gfxQueue = graphicsQueue.GetHandle();

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

void Texture::GenerateMipmapsInternal(VulkanCommandBuffer &cmd)
{
    uint32_t mipLevels = m_Image->get_mip_level_count();
    if (mipLevels <= 1) {
        return;
    }

    vk::Extent3D extent = m_Extent;
    auto imageHandle = m_Image->GetHandle();

    // 逐级生成：从 level i-1 blit 到 level i
    for (uint32_t i = 1; i < mipLevels; i++) {
        // 上一级（源）：TransferDst → TransferSrc
        image_utils::TransitionLayout(cmd.GetHandle(), imageHandle,
                                      vk::ImageLayout::eTransferDstOptimal,
                                      vk::ImageLayout::eTransferSrcOptimal,
                                      i - 1, 1);

        // 计算下一级尺寸（每级减半，至少 1x1）
        vk::Extent3D nextExtent{
            std::max(1u, extent.width  >> 1),
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
        blit.srcSubresource.aspectMask     = vk::ImageAspectFlagBits::eColor;
        blit.srcSubresource.mipLevel       = i - 1;
        blit.srcSubresource.baseArrayLayer = 0;
        blit.srcSubresource.layerCount     = 1;
        blit.srcOffsets[0] = vk::Offset3D{0, 0, 0};
        blit.srcOffsets[1] = vk::Offset3D{(int32_t)extent.width, (int32_t)extent.height, 1};

        blit.dstSubresource.aspectMask     = vk::ImageAspectFlagBits::eColor;
        blit.dstSubresource.mipLevel       = i;
        blit.dstSubresource.baseArrayLayer = 0;
        blit.dstSubresource.layerCount     = 1;
        blit.dstOffsets[0] = vk::Offset3D{0, 0, 0};
        blit.dstOffsets[1] = vk::Offset3D{(int32_t)nextExtent.width, (int32_t)nextExtent.height, 1};

        cmd.BlitImage(*m_Image, vk::ImageLayout::eTransferSrcOptimal,
                      *m_Image, vk::ImageLayout::eTransferDstOptimal,
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

// ============================================================================
// 设置调试名称
// ============================================================================

void Texture::SetDebugName(const std::string &name)
{
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
// 内部：创建 ImageView 和请求 Sampler
// ============================================================================

void Texture::CreateViewAndSampler(VulkanDevice &device,
                                   VulkanResourceCache &cache,
                                   vk::Filter mag_filter,
                                   vk::Filter min_filter)
{
    // 创建 ImageView
    m_ImageView = std::make_unique<VulkanImageView>(
        *m_Image,
        vk::ImageViewType::e2D,
        m_Format);

    // 查询设备支持的最大各向异性级别
    float maxAnisotropy = device.GetGpu().GetProperties().limits.maxSamplerAnisotropy;
    // 设备支持各向异性时才启用（基本所有现代 GPU 都支持，这里做个保险判断）
    vk::Bool32 enableAnisotropy = (maxAnisotropy > 1.0f) ? VK_TRUE : VK_FALSE;

    // 通过缓存获取 Sampler（启用各向异性过滤，提升曲面纹理质量）
    m_Sampler = &cache.RequestSampler(
        mag_filter,                        // mag
        min_filter,                        // min
        vk::SamplerMipmapMode::eLinear,    // mipmap
        vk::SamplerAddressMode::eRepeat,   // address U
        vk::SamplerAddressMode::eRepeat,   // address V
        vk::SamplerAddressMode::eRepeat,   // address W
        0.0f,                              // mip_lod_bias
        enableAnisotropy,                  // anisotropy_enable
        maxAnisotropy);                    // max_anisotropy
}

} // namespace GE