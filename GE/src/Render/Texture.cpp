//
// Created by Lenovo on 2026/6/12.
//

#include "Render/Texture.h"
#include "Render/VulkanBase/VulkanBuffer.h"
#include "Render/VulkanBase/VulkanDevice.h"

#include "Core/Log.h"
#include "stb_image.h"

#include <algorithm>
#include <cmath>

namespace GE {

Texture::~Texture() {
    Cleanup();
}

void Texture::LoadFromFile(VulkanDevice &device,
                            vk::Queue queue, uint32_t queueFamilyIndex,
                            const std::string &filepath,
                            vk::Filter magFilter, vk::Filter minFilter,
                            vk::SamplerAddressMode addressMode) {
    // 1. 用 stb_image 加载像素数据
    int tex_width, tex_height, tex_channels;
    stbi_uc *pixels = stbi_load(filepath.c_str(), &tex_width, &tex_height, &tex_channels, STBI_rgb_alpha);
    if (!pixels) {
        throw std::runtime_error("Failed to load texture: " + filepath);
    }

    LoadFromMemory(device, queue, queueFamilyIndex,
                   pixels, static_cast<uint32_t>(tex_width), static_cast<uint32_t>(tex_height),
                   vk::Format::eR8G8B8A8Srgb, magFilter, minFilter, addressMode);
    stbi_image_free(pixels);
}

void Texture::LoadFromColor(VulkanDevice &device,
                            vk::Queue queue, uint32_t queueFamilyIndex,
                            const glm::vec3 &color) {
    uint8_t pixel[4] = {
        static_cast<uint8_t>(std::clamp(color.r, 0.0f, 1.0f) * 255.0f),
        static_cast<uint8_t>(std::clamp(color.g, 0.0f, 1.0f) * 255.0f),
        static_cast<uint8_t>(std::clamp(color.b, 0.0f, 1.0f) * 255.0f),
        255,
    };

    LoadFromMemory(device, queue, queueFamilyIndex,
                   pixel, 1, 1,
                   vk::Format::eR8G8B8A8Srgb,
                   vk::Filter::eLinear, vk::Filter::eLinear,
                   vk::SamplerAddressMode::eRepeat);
}

void Texture::LoadFromMemory(VulkanDevice &device,
                              vk::Queue queue, uint32_t queueFamilyIndex,
                              const void *data, uint32_t width, uint32_t height,
                              vk::Format format,
                              vk::Filter magFilter, vk::Filter minFilter,
                              vk::SamplerAddressMode addressMode) {
    auto  allocator = device.GetVmaAllocator();
    auto  vkDevice  = device.GetHandle();

    // 计算完整 mip 链级数
    uint32_t mipLevels = static_cast<uint32_t>(std::floor(std::log2(std::max(width, height)))) + 1;

    vk::DeviceSize image_size = static_cast<vk::DeviceSize>(width * height * 4);

    // --- Staging buffer（使用 VMA-backed VulkanBuffer）---
    VulkanBuffer staging_buffer;
    staging_buffer.Init(allocator, image_size,
                        vk::BufferUsageFlagBits::eTransferSrc,
                        VMA_MEMORY_USAGE_AUTO,
                        VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT |
                        VMA_ALLOCATION_CREATE_MAPPED_BIT);
    staging_buffer.Upload(data, image_size);

    // --- 创建 HPPImage（带完整 mip 链）---
    m_Image = std::make_unique<VulkanHppImage>(device,
        vk::Extent3D{width, height, 1},
        format,
        vk::ImageUsageFlagBits::eTransferDst | vk::ImageUsageFlagBits::eTransferSrc | vk::ImageUsageFlagBits::eSampled,
        VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE,
        vk::SampleCountFlagBits::e1,
        mipLevels);

    // --- 创建 HPPImageView（覆盖所有 mip level）---
    m_ImageView = std::make_unique<VulkanHppImageView>(
        *m_Image, vk::ImageViewType::e2D, format,
        0, 0, mipLevels, 1);

    // --- One-time command buffer ---
    vk::CommandPoolCreateInfo pool_info{
        .flags = vk::CommandPoolCreateFlagBits::eTransient,
        .queueFamilyIndex = queueFamilyIndex,
    };
    vk::CommandPool cmd_pool = vkDevice.createCommandPool(pool_info);

    vk::CommandBufferAllocateInfo alloc_info_cmd{
        .commandPool = cmd_pool,
        .level = vk::CommandBufferLevel::ePrimary,
        .commandBufferCount = 1,
    };
    vk::CommandBuffer cmd_buf = vkDevice.allocateCommandBuffers(alloc_info_cmd)[0];

    cmd_buf.begin(vk::CommandBufferBeginInfo{.flags = vk::CommandBufferUsageFlagBits::eOneTimeSubmit});

    // 所有 mip level 从 Undefined → TransferDst
    image_utils::TransitionLayout(cmd_buf, m_Image->GetHandle(),
                                  vk::ImageLayout::eUndefined, vk::ImageLayout::eTransferDstOptimal,
                                  0, mipLevels);

    // 拷贝 base level (mip 0)
    vk::BufferImageCopy copy_region{
        .bufferOffset = 0,
        .bufferRowLength = 0,
        .bufferImageHeight = 0,
        .imageSubresource = {.aspectMask = vk::ImageAspectFlagBits::eColor, .mipLevel = 0, .baseArrayLayer = 0, .layerCount = 1},
        .imageOffset = {0, 0, 0},
        .imageExtent = {width, height, 1},
    };
    cmd_buf.copyBufferToImage(staging_buffer.GetBuffer(), m_Image->GetHandle(), vk::ImageLayout::eTransferDstOptimal, copy_region);

    // 逐级 blit 生成 mip chain
    for (uint32_t i = 1; i < mipLevels; i++) {
        // 源 level i-1: TransferDst → TransferSrc
        image_utils::TransitionLayout(cmd_buf, m_Image->GetHandle(),
                                      vk::ImageLayout::eTransferDstOptimal, vk::ImageLayout::eTransferSrcOptimal,
                                      i - 1, 1);

        vk::ImageBlit blit{};
        blit.srcSubresource = {vk::ImageAspectFlagBits::eColor, i - 1, 0, 1};
        blit.srcOffsets[0] = {0, 0, 0};
        blit.srcOffsets[1] = {static_cast<int32_t>(std::max(width >> (i - 1), 1u)),
                              static_cast<int32_t>(std::max(height >> (i - 1), 1u)), 1};
        blit.dstSubresource = {vk::ImageAspectFlagBits::eColor, i, 0, 1};
        blit.dstOffsets[0] = {0, 0, 0};
        blit.dstOffsets[1] = {static_cast<int32_t>(std::max(width >> i, 1u)),
                              static_cast<int32_t>(std::max(height >> i, 1u)), 1};

        cmd_buf.blitImage(m_Image->GetHandle(), vk::ImageLayout::eTransferSrcOptimal,
                          m_Image->GetHandle(), vk::ImageLayout::eTransferDstOptimal,
                          blit, vk::Filter::eLinear);

        // 源 level i-1: TransferSrc → ShaderReadOnly
        image_utils::TransitionLayout(cmd_buf, m_Image->GetHandle(),
                                      vk::ImageLayout::eTransferSrcOptimal, vk::ImageLayout::eShaderReadOnlyOptimal,
                                      i - 1, 1);
    }

    // 最后一个 mip level: TransferDst → ShaderReadOnly
    image_utils::TransitionLayout(cmd_buf, m_Image->GetHandle(),
                                  vk::ImageLayout::eTransferDstOptimal, vk::ImageLayout::eShaderReadOnlyOptimal,
                                  mipLevels - 1, 1);

    cmd_buf.end();

    vk::SubmitInfo submit{.commandBufferCount = 1, .pCommandBuffers = &cmd_buf};
    queue.submit(submit, nullptr);
    queue.waitIdle();

    // Clean up staging and temp pool
    staging_buffer.Destroy();
    vkDevice.freeCommandBuffers(cmd_pool, cmd_buf);
    vkDevice.destroyCommandPool(cmd_pool);

    // 自动创建匹配的 sampler（maxLod 与 mip 级数一致）
    m_Sampler.Init(vkDevice, magFilter, minFilter,
                   addressMode, static_cast<float>(mipLevels));
}

void Texture::Cleanup() {
    m_ImageView.reset();
    m_Image.reset();
    m_Sampler.Cleanup();
}

} // namespace GE
