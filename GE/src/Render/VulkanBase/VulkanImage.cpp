//
// Created by Lenovo on 2026/6/5.
//

#include "../../../include/GE/Render/VulkanBase/VulkanImage.h"
#include "../../../include/GE/Render/VulkanBase/VulkanBuffer.h"

#include "Core/Log.h"
#include "stb_image.h"

#include <cassert>
#include <cmath>
#include <algorithm>
#include <stdexcept>

namespace GE {

void VulkanImage::LoadFromFile(VmaAllocator allocator,
                               vk::Queue queue, uint32_t queue_family_index,
                               const std::string &filepath) {
    int tex_width, tex_height, tex_channels;
    stbi_uc *pixels = stbi_load(filepath.c_str(), &tex_width, &tex_height, &tex_channels, STBI_rgb_alpha);
    if (!pixels) {
        throw std::runtime_error("Failed to load texture: " + filepath);
    }

    LoadFromMemory(allocator, queue, queue_family_index,
                   pixels, static_cast<uint32_t>(tex_width), static_cast<uint32_t>(tex_height));
    stbi_image_free(pixels);
}

void VulkanImage::LoadFromMemory(VmaAllocator allocator,
                                 vk::Queue queue, uint32_t queue_family_index,
                                 const void *data, uint32_t width, uint32_t height,
                                 vk::Format format) {
    m_Allocator = allocator;

    VmaAllocatorInfo allocator_info;
    vmaGetAllocatorInfo(allocator, &allocator_info);
    m_Device = allocator_info.device;

    m_Width = width;
    m_Height = height;
    m_Format = format;

    // 计算完整 mip 链级数
    m_MipLevels = static_cast<uint32_t>(std::floor(std::log2(std::max(width, height)))) + 1;

    vk::DeviceSize image_size = static_cast<vk::DeviceSize>(width * height * 4);

    // --- Staging buffer (using VMA-backed VulkanBuffer) ---
    VulkanBuffer staging_buffer;
    staging_buffer.Init(allocator, image_size,
                        vk::BufferUsageFlagBits::eTransferSrc,
                        VMA_MEMORY_USAGE_AUTO,
                        VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT |
                        VMA_ALLOCATION_CREATE_MAPPED_BIT);
    staging_buffer.Upload(data, image_size);

    // --- Final image（带完整 mip 链）---
    VkImageCreateInfo image_info{};
    image_info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    image_info.imageType = VK_IMAGE_TYPE_2D;
    image_info.format = static_cast<VkFormat>(format);
    image_info.extent = {width, height, 1};
    image_info.mipLevels = m_MipLevels;
    image_info.arrayLayers = 1;
    image_info.samples = VK_SAMPLE_COUNT_1_BIT;
    image_info.tiling = VK_IMAGE_TILING_OPTIMAL;
    image_info.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    image_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    image_info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

    VmaAllocationCreateInfo img_alloc_info{};
    img_alloc_info.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;

    VmaAllocationInfo vma_alloc_info;
    VkImage image;
    vmaCreateImage(allocator, &image_info, &img_alloc_info, &image, &m_Allocation, &vma_alloc_info);
    m_Image = image;

    // --- One-time command buffer ---
    vk::CommandPoolCreateInfo pool_info{
        .flags = vk::CommandPoolCreateFlagBits::eTransient,
        .queueFamilyIndex = queue_family_index,
    };
    vk::CommandPool cmd_pool = m_Device.createCommandPool(pool_info);

    vk::CommandBufferAllocateInfo alloc_info_cmd{
        .commandPool = cmd_pool,
        .level = vk::CommandBufferLevel::ePrimary,
        .commandBufferCount = 1,
    };
    vk::CommandBuffer cmd_buf = m_Device.allocateCommandBuffers(alloc_info_cmd)[0];

    cmd_buf.begin(vk::CommandBufferBeginInfo{.flags = vk::CommandBufferUsageFlagBits::eOneTimeSubmit});

    // 所有 mip level 从 Undefined → TransferDst
    TransitionLayout(cmd_buf, m_Image,
                     vk::ImageLayout::eUndefined, vk::ImageLayout::eTransferDstOptimal,
                     0, m_MipLevels);

    // 拷贝 base level (mip 0)
    vk::BufferImageCopy copy_region{
        .bufferOffset = 0,
        .bufferRowLength = 0,
        .bufferImageHeight = 0,
        .imageSubresource = {.aspectMask = vk::ImageAspectFlagBits::eColor, .mipLevel = 0, .baseArrayLayer = 0, .layerCount = 1},
        .imageOffset = {0, 0, 0},
        .imageExtent = {width, height, 1},
    };
    cmd_buf.copyBufferToImage(staging_buffer.GetBuffer(), m_Image, vk::ImageLayout::eTransferDstOptimal, copy_region);

    // 逐级 blit 生成 mip chain
    for (uint32_t i = 1; i < m_MipLevels; i++) {
        // 源 level i-1: TransferDst → TransferSrc
        TransitionLayout(cmd_buf, m_Image,
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

        cmd_buf.blitImage(m_Image, vk::ImageLayout::eTransferSrcOptimal,
                          m_Image, vk::ImageLayout::eTransferDstOptimal,
                          blit, vk::Filter::eLinear);

        // 源 level i-1: TransferSrc → ShaderReadOnly（不再需要作为 blit 源）
        TransitionLayout(cmd_buf, m_Image,
                         vk::ImageLayout::eTransferSrcOptimal, vk::ImageLayout::eShaderReadOnlyOptimal,
                         i - 1, 1);
    }

    // 最后一个 mip level: TransferDst → ShaderReadOnly
    TransitionLayout(cmd_buf, m_Image,
                     vk::ImageLayout::eTransferDstOptimal, vk::ImageLayout::eShaderReadOnlyOptimal,
                     m_MipLevels - 1, 1);

    cmd_buf.end();

    vk::SubmitInfo submit{.commandBufferCount = 1, .pCommandBuffers = &cmd_buf};
    queue.submit(submit, nullptr);
    queue.waitIdle();

    // Clean up staging and temp pool
    staging_buffer.Destroy();
    m_Device.freeCommandBuffers(cmd_pool, cmd_buf);
    m_Device.destroyCommandPool(cmd_pool);

    // 创建 view（覆盖所有 mip level）
    vk::ImageViewCreateInfo view_info{
        .image = m_Image,
        .viewType = vk::ImageViewType::e2D,
        .format = format,
        .subresourceRange = {.aspectMask = vk::ImageAspectFlagBits::eColor,
                             .baseMipLevel = 0, .levelCount = m_MipLevels,
                             .baseArrayLayer = 0, .layerCount = 1},
    };
    m_View = m_Device.createImageView(view_info);
}

void VulkanImage::Init(VmaAllocator allocator,
                       uint32_t width, uint32_t height, vk::Format format,
                       vk::ImageTiling tiling, vk::ImageUsageFlags usage,
                       VmaMemoryUsage memory_usage, VmaAllocationCreateFlags flags) {
    m_Allocator = allocator;

    VmaAllocatorInfo allocator_info;
    vmaGetAllocatorInfo(allocator, &allocator_info);
    m_Device = allocator_info.device;

    m_Width = width;
    m_Height = height;

    // 存储参数供 Resize 复用
    m_Format = format;
    m_Tiling = tiling;
    m_Usage = usage;
    m_MemoryUsage = memory_usage;
    m_AllocFlags = flags;

    VkImageCreateInfo image_info{};
    image_info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    image_info.imageType = VK_IMAGE_TYPE_2D;
    image_info.format = static_cast<VkFormat>(format);
    image_info.extent = {width, height, 1};
    image_info.mipLevels = 1;
    image_info.arrayLayers = 1;
    image_info.samples = VK_SAMPLE_COUNT_1_BIT;
    image_info.tiling = static_cast<VkImageTiling>(tiling);
    image_info.usage = static_cast<VkImageUsageFlags>(usage);
    image_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    image_info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

    VmaAllocationCreateInfo img_alloc_info{};
    img_alloc_info.usage = memory_usage;
    img_alloc_info.flags = flags;

    VmaAllocationInfo vma_alloc_info;
    VkImage image;
    vmaCreateImage(allocator, &image_info, &img_alloc_info, &image, &m_Allocation, &vma_alloc_info);
    m_Image = image;
}

void VulkanImage::Resize(uint32_t newWidth, uint32_t newHeight) {
    // 销毁旧的 image 和 view
    if (m_View)
        m_Device.destroyImageView(m_View);
    if (m_Image)
        vmaDestroyImage(m_Allocator, static_cast<VkImage>(m_Image), m_Allocation);

    m_View = nullptr;
    m_Image = nullptr;
    m_Allocation = nullptr;

    m_Width = newWidth;
    m_Height = newHeight;

    // 用存储的参数重建
    VkImageCreateInfo image_info{};
    image_info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    image_info.imageType = VK_IMAGE_TYPE_2D;
    image_info.format = static_cast<VkFormat>(m_Format);
    image_info.extent = {newWidth, newHeight, 1};
    image_info.mipLevels = m_MipLevels;
    image_info.arrayLayers = 1;
    image_info.samples = VK_SAMPLE_COUNT_1_BIT;
    image_info.tiling = static_cast<VkImageTiling>(m_Tiling);
    image_info.usage = static_cast<VkImageUsageFlags>(m_Usage);
    image_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    image_info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

    VmaAllocationCreateInfo img_alloc_info{};
    img_alloc_info.usage = m_MemoryUsage;
    img_alloc_info.flags = m_AllocFlags;

    VmaAllocationInfo vma_alloc_info;
    VkImage image;
    vmaCreateImage(m_Allocator, &image_info, &img_alloc_info, &image, &m_Allocation, &vma_alloc_info);
    m_Image = image;

    // 重建 view
    vk::ImageViewCreateInfo view_info{
        .image = m_Image,
        .viewType = m_ViewType,
        .format = m_Format,
        .subresourceRange = {.aspectMask = m_Aspect,
                             .baseMipLevel = 0, .levelCount = m_MipLevels,
                             .baseArrayLayer = 0, .layerCount = 1},
    };
    m_View = m_Device.createImageView(view_info);
}

vk::ImageView VulkanImage::CreateView(vk::Device device, vk::Image image,
                                      vk::ImageViewType type, vk::Format format,
                                      vk::ImageAspectFlags aspect) {
    vk::ImageViewCreateInfo view_info{
        .image = image,
        .viewType = type,
        .format = format,
        .subresourceRange = {.aspectMask = aspect, .baseMipLevel = 0, .levelCount = 1, .baseArrayLayer = 0, .layerCount = 1},
    };
    return device.createImageView(view_info);
}

void VulkanImage::CreateView(vk::Format format,
                             vk::ImageViewType type,
                             vk::ImageAspectFlags aspect) {
    m_ViewType = type;
    m_Aspect = aspect;
    m_View = CreateView(m_Device, m_Image, type, format, aspect);
}

void VulkanImage::TransitionLayout(vk::CommandBuffer cmd, vk::Image image,
                                   vk::ImageLayout old_layout, vk::ImageLayout new_layout,
                                   uint32_t baseMipLevel, uint32_t levelCount) {
    struct Transition {
        vk::ImageLayout old_layout;
        vk::ImageLayout new_layout;
        vk::PipelineStageFlags2 src_stage;
        vk::AccessFlags2 src_access;
        vk::PipelineStageFlags2 dst_stage;
        vk::AccessFlags2 dst_access;
    };

    static const Transition kTransitions[] = {
        // Undefined → TransferDst:  新 image，准备收 staging 拷贝
        {vk::ImageLayout::eUndefined, vk::ImageLayout::eTransferDstOptimal,
         vk::PipelineStageFlagBits2::eTopOfPipe, vk::AccessFlagBits2::eNone,
         vk::PipelineStageFlagBits2::eTransfer, vk::AccessFlagBits2::eTransferWrite},

        // TransferDst → TransferSrc:  mip 生成前，准备作为 blit 源
        {vk::ImageLayout::eTransferDstOptimal, vk::ImageLayout::eTransferSrcOptimal,
         vk::PipelineStageFlagBits2::eTransfer, vk::AccessFlagBits2::eTransferWrite,
         vk::PipelineStageFlagBits2::eTransfer, vk::AccessFlagBits2::eTransferRead},

        // TransferSrc → ShaderReadOnly:  mip 生成完成，转给着色器采样
        {vk::ImageLayout::eTransferSrcOptimal, vk::ImageLayout::eShaderReadOnlyOptimal,
         vk::PipelineStageFlagBits2::eTransfer, vk::AccessFlagBits2::eTransferRead,
         vk::PipelineStageFlagBits2::eFragmentShader, vk::AccessFlagBits2::eShaderRead},

        // TransferDst → ShaderReadOnly:  staging 完成，准备给着色器采样
        {vk::ImageLayout::eTransferDstOptimal, vk::ImageLayout::eShaderReadOnlyOptimal,
         vk::PipelineStageFlagBits2::eTransfer, vk::AccessFlagBits2::eTransferWrite,
         vk::PipelineStageFlagBits2::eFragmentShader, vk::AccessFlagBits2::eShaderRead},

        // Undefined → ColorAttachment:  新 swapchain image，准备渲染
        {vk::ImageLayout::eUndefined, vk::ImageLayout::eColorAttachmentOptimal,
         vk::PipelineStageFlagBits2::eTopOfPipe, vk::AccessFlagBits2::eNone,
         vk::PipelineStageFlagBits2::eColorAttachmentOutput, vk::AccessFlagBits2::eColorAttachmentWrite},

        // ColorAttachment → PresentSrc:  渲染完成，准备呈现
        {vk::ImageLayout::eColorAttachmentOptimal, vk::ImageLayout::ePresentSrcKHR,
         vk::PipelineStageFlagBits2::eColorAttachmentOutput, vk::AccessFlagBits2::eColorAttachmentWrite,
         vk::PipelineStageFlagBits2::eBottomOfPipe, vk::AccessFlagBits2::eNone},

        // Undefined → DepthStencilAttachment:  新 depth image，准备渲染深度
        {vk::ImageLayout::eUndefined, vk::ImageLayout::eDepthStencilAttachmentOptimal,
         vk::PipelineStageFlagBits2::eTopOfPipe, vk::AccessFlagBits2::eNone,
         vk::PipelineStageFlagBits2::eEarlyFragmentTests, vk::AccessFlagBits2::eDepthStencilAttachmentWrite},
    };

    auto it = std::ranges::find_if(kTransitions, [&](auto const &t) {
        return t.old_layout == old_layout && t.new_layout == new_layout;
    });
    if (it == std::end(kTransitions))
        throw std::runtime_error("Unsupported layout transition");

    // 根据目标 layout 选择 aspect mask
    vk::ImageAspectFlags aspectMask = vk::ImageAspectFlagBits::eColor;
    if (new_layout == vk::ImageLayout::eDepthStencilAttachmentOptimal)
        aspectMask = vk::ImageAspectFlagBits::eDepth;

    vk::ImageMemoryBarrier2 barrier{
        .srcStageMask = it->src_stage,
        .srcAccessMask = it->src_access,
        .dstStageMask = it->dst_stage,
        .dstAccessMask = it->dst_access,
        .oldLayout = old_layout,
        .newLayout = new_layout,
        .srcQueueFamilyIndex = vk::QueueFamilyIgnored,
        .dstQueueFamilyIndex = vk::QueueFamilyIgnored,
        .image = image,
        .subresourceRange = {.aspectMask = aspectMask,
                             .baseMipLevel = baseMipLevel, .levelCount = levelCount,
                             .baseArrayLayer = 0, .layerCount = 1},
    };

    vk::DependencyInfo dep_info{
        .imageMemoryBarrierCount = 1,
        .pImageMemoryBarriers = &barrier,
    };
    cmd.pipelineBarrier2(dep_info);
}

void VulkanImage::Cleanup() {
    if (m_Allocator) {
        if (m_View)
            m_Device.destroyImageView(m_View);
        if (m_Image)
            vmaDestroyImage(m_Allocator, static_cast<VkImage>(m_Image), m_Allocation);
    }
    m_Allocator = nullptr;
    m_Device = nullptr;
    m_Allocation = nullptr;
    m_Image = nullptr;
    m_View = nullptr;
    m_Width = 0;
    m_Height = 0;
    m_MipLevels = 1;
    m_Format = vk::Format::eUndefined;
    m_Usage = {};
    m_MemoryUsage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
    m_AllocFlags = 0;
    m_ViewType = vk::ImageViewType::e2D;
    m_Aspect = vk::ImageAspectFlagBits::eColor;
}

} // namespace GE
