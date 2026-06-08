//
// Created by Lenovo on 2026/6/5.
//

#define VULKAN_HPP_DISPATCH_LOADER_DYNAMIC 1
#include "../../../include/GE/Render/VulkanBase/VulkanImage.h"
#include "../../../include/GE/Render/VulkanBase/VulkanBuffer.h"
#include "Render/VulkanBase/VulkanDevice.h"

#include "Core/Log.h"
#include "stb_image.h"

#include <cassert>
#include <stdexcept>

namespace GE {

void VulkanImage::LoadFromFile(vk::Device device, vk::PhysicalDevice gpu,
                               vk::Queue queue, uint32_t queue_family_index,
                               const std::string &filepath) {
    m_Device = device;
    int tex_width, tex_height, tex_channels;
    stbi_uc *pixels = stbi_load(filepath.c_str(), &tex_width, &tex_height, &tex_channels, STBI_rgb_alpha);
    if (!pixels) {
        throw std::runtime_error("Failed to load texture: " + filepath);
    }

    LoadFromMemory(device, gpu, queue, queue_family_index,
                   pixels, static_cast<uint32_t>(tex_width), static_cast<uint32_t>(tex_height));
    stbi_image_free(pixels);
}

void VulkanImage::LoadFromMemory(vk::Device device, vk::PhysicalDevice gpu,
                                 vk::Queue queue, uint32_t queue_family_index,
                                 const void *data, uint32_t width, uint32_t height,
                                 vk::Format format) {
    m_Device = device;
    m_Width = width;
    m_Height = height;

    vk::DeviceSize image_size = static_cast<vk::DeviceSize>(width * height * 4);

    // --- Staging buffer (using VulkanBuffer) ---
    VulkanBuffer staging_buffer;
    staging_buffer.Init(device, gpu, image_size,
                        vk::BufferUsageFlagBits::eTransferSrc,
                        vk::MemoryPropertyFlagBits::eHostVisible | vk::MemoryPropertyFlagBits::eHostCoherent);
    staging_buffer.Upload(data, image_size);

    // --- Final image ---
    vk::ImageCreateInfo image_info{
        .imageType = vk::ImageType::e2D,
        .format = format,
        .extent = {width, height, 1},
        .mipLevels = 1,
        .arrayLayers = 1,
        .samples = vk::SampleCountFlagBits::e1,
        .tiling = vk::ImageTiling::eOptimal,
        .usage = vk::ImageUsageFlagBits::eTransferDst | vk::ImageUsageFlagBits::eSampled,
        .sharingMode = vk::SharingMode::eExclusive,
        .initialLayout = vk::ImageLayout::eUndefined,
    };
    m_Image = device.createImage(image_info);

    vk::MemoryRequirements mem_req = device.getImageMemoryRequirements(m_Image);
    uint32_t mem_type = VulkanDevice::FindMemoryType(gpu, mem_req.memoryTypeBits,
                                                     vk::MemoryPropertyFlagBits::eDeviceLocal);

    vk::MemoryAllocateInfo alloc_info{.allocationSize = mem_req.size, .memoryTypeIndex = mem_type};
    m_Memory = device.allocateMemory(alloc_info);
    device.bindImageMemory(m_Image, m_Memory, 0);

    // --- One-time command buffer ---
    vk::CommandPoolCreateInfo pool_info{
        .flags = vk::CommandPoolCreateFlagBits::eTransient,
        .queueFamilyIndex = queue_family_index,
    };
    vk::CommandPool cmd_pool = device.createCommandPool(pool_info);

    vk::CommandBufferAllocateInfo alloc_info_cmd{
        .commandPool = cmd_pool,
        .level = vk::CommandBufferLevel::ePrimary,
        .commandBufferCount = 1,
    };
    vk::CommandBuffer cmd_buf = device.allocateCommandBuffers(alloc_info_cmd)[0];

    cmd_buf.begin(vk::CommandBufferBeginInfo{.flags = vk::CommandBufferUsageFlagBits::eOneTimeSubmit});

    TransitionLayout(cmd_buf, m_Image,
                     vk::ImageLayout::eUndefined, vk::ImageLayout::eTransferDstOptimal);

    vk::BufferImageCopy copy_region{
        .bufferOffset = 0,
        .bufferRowLength = 0,
        .bufferImageHeight = 0,
        .imageSubresource = {.aspectMask = vk::ImageAspectFlagBits::eColor, .mipLevel = 0, .baseArrayLayer = 0, .layerCount = 1},
        .imageOffset = {0, 0, 0},
        .imageExtent = {width, height, 1},
    };
    cmd_buf.copyBufferToImage(staging_buffer.GetBuffer(), m_Image, vk::ImageLayout::eTransferDstOptimal, copy_region);

    TransitionLayout(cmd_buf, m_Image,
                     vk::ImageLayout::eTransferDstOptimal, vk::ImageLayout::eShaderReadOnlyOptimal);

    cmd_buf.end();

    vk::SubmitInfo submit{.commandBufferCount = 1, .pCommandBuffers = &cmd_buf};
    queue.submit(submit, nullptr);
    queue.waitIdle();

    // Clean up staging and temp pool
    staging_buffer.Destroy();
    device.freeCommandBuffers(cmd_pool, cmd_buf);
    device.destroyCommandPool(cmd_pool);

    // Create view
    m_View = CreateView(m_Device, m_Image, vk::ImageViewType::e2D, format);
}

void VulkanImage::Init(vk::Device device, vk::PhysicalDevice gpu,
                       uint32_t width, uint32_t height, vk::Format format,
                       vk::ImageTiling tiling, vk::ImageUsageFlags usage,
                       vk::MemoryPropertyFlags memory_properties) {
    m_Device = device;
    m_Width = width;
    m_Height = height;

    vk::ImageCreateInfo image_info{
        .imageType = vk::ImageType::e2D,
        .format = format,
        .extent = {width, height, 1},
        .mipLevels = 1,
        .arrayLayers = 1,
        .samples = vk::SampleCountFlagBits::e1,
        .tiling = tiling,
        .usage = usage,
        .sharingMode = vk::SharingMode::eExclusive,
        .initialLayout = vk::ImageLayout::eUndefined,
    };
    m_Image = device.createImage(image_info);

    vk::MemoryRequirements mem_req = device.getImageMemoryRequirements(m_Image);
    uint32_t mem_type = VulkanDevice::FindMemoryType(gpu, mem_req.memoryTypeBits, memory_properties);

    vk::MemoryAllocateInfo alloc_info{.allocationSize = mem_req.size, .memoryTypeIndex = mem_type};
    m_Memory = device.allocateMemory(alloc_info);

    device.bindImageMemory(m_Image, m_Memory, 0);
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

void VulkanImage::TransitionLayout(vk::CommandBuffer cmd, vk::Image image,
                                   vk::ImageLayout old_layout, vk::ImageLayout new_layout) {
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
    };

    auto it = std::ranges::find_if(kTransitions, [&](auto const &t) {
        return t.old_layout == old_layout && t.new_layout == new_layout;
    });
    if (it == std::end(kTransitions))
        throw std::runtime_error("Unsupported layout transition");

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
        .subresourceRange = {.aspectMask = vk::ImageAspectFlagBits::eColor,
                             .baseMipLevel = 0, .levelCount = 1,
                             .baseArrayLayer = 0, .layerCount = 1},
    };

    vk::DependencyInfo dep_info{
        .imageMemoryBarrierCount = 1,
        .pImageMemoryBarriers = &barrier,
    };
    cmd.pipelineBarrier2(dep_info);
}

void VulkanImage::Cleanup() {
    if (m_Device) {
        if (m_View)
            m_Device.destroyImageView(m_View);
        if (m_Memory)
            m_Device.freeMemory(m_Memory);
        if (m_Image)
            m_Device.destroyImage(m_Image);
    }
    m_View = nullptr;
    m_Memory = nullptr;
    m_Image = nullptr;
    m_Width = 0;
    m_Height = 0;
    m_Device = nullptr;
}

} // namespace GE
