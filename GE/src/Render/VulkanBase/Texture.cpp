//
// Texture 实现 —— 纹理加载、上传、采样配置
//

#include "Render/VulkanBase/Texture.h"
#include "Render/VulkanBase/VulkanBuffer.h"
#include "Render/VulkanBase/VulkanCommandBuffer.h"
#include "Render/VulkanBase/VulkanImage.h"
#include "Core/Log.h"

#include "stb_image.h"

#include <vulkan/vulkan.hpp>

namespace GE {

// ============================================================================
// 工厂方法：LoadFromFile
// ============================================================================

std::unique_ptr<Texture> Texture::LoadFromFile(
    VulkanDevice &device,
    VulkanResourceCache &cache,
    const std::string &filepath,
    vk::Format format,
    vk::Filter mag_filter,
    vk::Filter min_filter)
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
                                  format, mag_filter, min_filter);

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
    vk::Filter min_filter)
{
    auto texture = std::unique_ptr<Texture>(new Texture(device, vk::Extent3D{width, height, 1}, format));

    // 上传像素数据到 GPU
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
                 vk::ImageUsageFlags extra_usage)
    : m_Format(format)
    , m_Extent(extent)
{
    m_Image = std::make_unique<VulkanImage>(
        device,
        extent,
        format,
        vk::ImageUsageFlagBits::eTransferDst | vk::ImageUsageFlagBits::eSampled | extra_usage);
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
    auto uploadCmd = device.RequestCommandBuffer(vk::CommandBufferLevel::ePrimary, true);

    // 布局转换：UNDEFINED -> TRANSFER_DST
    image_utils::TransitionLayout(uploadCmd->GetHandle(), m_Image->GetHandle(),
                                  vk::ImageLayout::eUndefined,
                                  vk::ImageLayout::eTransferDstOptimal);

    // 拷贝 staging buffer 到纹理图像
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

    uploadCmd->GetHandle().copyBufferToImage(stagingBuffer.GetHandle(), m_Image->GetHandle(),
                                             vk::ImageLayout::eTransferDstOptimal, copyRegion);

    // 布局转换：TRANSFER_DST -> SHADER_READ_ONLY
    image_utils::TransitionLayout(uploadCmd->GetHandle(), m_Image->GetHandle(),
                                  vk::ImageLayout::eTransferDstOptimal,
                                  vk::ImageLayout::eShaderReadOnlyOptimal);

    // 提交并等待完成
    uploadCmd->End();
    device.FlushCommandBuffer(uploadCmd, gfxQueue);

    // staging buffer 在此处自动析构
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

    // 通过缓存获取 Sampler（默认参数）
    m_Sampler = &cache.RequestSampler(
        mag_filter,  // mag
        min_filter,  // min
        vk::SamplerMipmapMode::eLinear,   // mipmap
        vk::SamplerAddressMode::eRepeat,  // address U
        vk::SamplerAddressMode::eRepeat,  // address V
        vk::SamplerAddressMode::eRepeat); // address W
}

} // namespace GE