//
// Created by Lenovo on 2026/6/3.
//

#define VULKAN_HPP_DISPATCH_LOADER_DYNAMIC 1
#include "../../../include/GE/Render/VulkanBase/VulkanSwapchain.h"
#include "../../../include/GE/Render/VulkanBase/VulkanImage.h"

#include "Core/Log.h"

#include <cassert>

namespace GE {

void VulkanSwapchain::Init(vk::Device device, vk::PhysicalDevice gpu, vk::SurfaceKHR surface,
                           vk::Queue queue, int32_t graphics_queue_index, uint32_t width, uint32_t height) {
    m_Device = device;
    m_Gpu = gpu;
    m_Surface = surface;
    m_Queue = queue;
    m_GraphicsQueueIndex = graphics_queue_index;

    m_Dimensions.width = width;
    m_Dimensions.height = height;

    CreateSwapchain(width, height, nullptr);
    CreateImageViews();
}

void VulkanSwapchain::Destroy() {
    if (!m_Device)
        return;

    m_Device.waitIdle();

    for (auto &pf : m_PerFrame)
        pf.Destroy(m_Device);
    m_PerFrame.clear();
    for (auto v : m_ImageViews)
        m_Device.destroyImageView(v);
    m_ImageViews.clear();
    m_Images.clear();
    for (auto sem : m_RecycledSemaphores)
        m_Device.destroySemaphore(sem);
    m_RecycledSemaphores.clear();
    if (m_Swapchain)
        m_Device.destroySwapchainKHR(m_Swapchain);
    m_Swapchain = nullptr;

    m_Device = nullptr;
    m_Gpu = nullptr;
    m_Surface = nullptr;
    m_Queue = nullptr;
    m_GraphicsQueueIndex = -1;
}

void VulkanSwapchain::Resize() {
    auto surface_properties = m_Gpu.getSurfaceCapabilitiesKHR(m_Surface);
    if (surface_properties.currentExtent.width == m_Dimensions.width &&
        surface_properties.currentExtent.height == m_Dimensions.height) {
        return;
    }

    m_Device.waitIdle();
    for (auto &pf : m_PerFrame)
        pf.Destroy(m_Device);
    m_PerFrame.clear();
    for (auto v : m_ImageViews)
        m_Device.destroyImageView(v);
    m_ImageViews.clear();
    m_Images.clear();

    vk::SwapchainKHR old_swapchain = m_Swapchain;
    m_Swapchain = nullptr;

    m_Dimensions.width = surface_properties.currentExtent.width;
    m_Dimensions.height = surface_properties.currentExtent.height;

    GE_CORE_TRACE("SwapChain resize {} {}", m_Dimensions.width, m_Dimensions.height);

    CreateSwapchain(surface_properties.currentExtent.width, surface_properties.currentExtent.height, old_swapchain);
    if (old_swapchain)
        m_Device.destroySwapchainKHR(old_swapchain);
    CreateImageViews();
}

vk::Result VulkanSwapchain::AcquireNextImage(uint32_t *image) {
    vk::Semaphore acquire_semaphore;
    if (m_RecycledSemaphores.empty()) {
        acquire_semaphore = m_Device.createSemaphore(vk::SemaphoreCreateInfo{});
    } else {
        acquire_semaphore = m_RecycledSemaphores.back();
        m_RecycledSemaphores.pop_back();
    }

    vk::Result result;
    try {
        std::tie(result, *image) = m_Device.acquireNextImageKHR(m_Swapchain, UINT64_MAX, acquire_semaphore);
    } catch (vk::OutOfDateKHRError &) {
        m_NeedsResize = true;
        result = vk::Result::eErrorOutOfDateKHR;
    }

    if (result != vk::Result::eSuccess) {
        m_RecycledSemaphores.push_back(acquire_semaphore);
    } else {
        assert(*image < m_PerFrame.size());

        auto &per_frame = m_PerFrame[*image];
        per_frame.WaitAndResetFence(m_Device);
        per_frame.ResetCommandPool(m_Device);

        vk::Semaphore old_semaphore = per_frame.TakeAcquireSemaphore();
        if (old_semaphore) {
            m_RecycledSemaphores.push_back(old_semaphore);
        }

        per_frame.GiveAcquireSemaphore(acquire_semaphore);
    }
    return result;
}

vk::CommandBuffer VulkanSwapchain::BeginFrame(uint32_t imageIndex) {
    auto cmd = GetCommandBuffer(imageIndex);

    vk::CommandBufferBeginInfo begin_info{.flags = vk::CommandBufferUsageFlagBits::eOneTimeSubmit};
    cmd.begin(begin_info);

    VulkanImage::TransitionLayout(cmd, m_Images[imageIndex],
                                  vk::ImageLayout::eUndefined, vk::ImageLayout::eColorAttachmentOptimal);

    return cmd;
}

void VulkanSwapchain::EndFrame(uint32_t imageIndex) {
    auto cmd = GetCommandBuffer(imageIndex);

    VulkanImage::TransitionLayout(cmd, m_Images[imageIndex],
                                  vk::ImageLayout::eColorAttachmentOptimal, vk::ImageLayout::ePresentSrcKHR);

    cmd.end();

    auto &per_frame = m_PerFrame[imageIndex];
    vk::PipelineStageFlags wait_stage = {vk::PipelineStageFlagBits::eColorAttachmentOutput};
    vk::Semaphore acquire = per_frame.GetAcquireSemaphore();
    vk::Semaphore release = per_frame.GetReleaseSemaphore();
    vk::Fence fence = per_frame.GetSubmitFence();

    vk::SubmitInfo info{
        .waitSemaphoreCount = 1,
        .pWaitSemaphores = &acquire,
        .pWaitDstStageMask = &wait_stage,
        .commandBufferCount = 1,
        .pCommandBuffers = &cmd,
        .signalSemaphoreCount = 1,
        .pSignalSemaphores = &release,
    };

    m_Queue.submit(info, fence);
}

bool VulkanSwapchain::BeginFrame() {
    if (m_NeedsResize) {
        Resize();
        m_NeedsResize = false;
    }

    uint32_t index;
    auto res = AcquireNextImage(&index);

    if (res != vk::Result::eSuccess) {
        return false;
    }

    m_CurrentImageIndex = index;
    m_CurrentCmd = BeginFrame(m_CurrentImageIndex);
    return true;
}

void VulkanSwapchain::EndFrame() {
    EndFrame(m_CurrentImageIndex);

    auto res = Present(m_CurrentImageIndex);
    if (res != vk::Result::eSuccess) {
        GE_CORE_ERROR("Failed to present swapchain image.");
    }

    m_CurrentImageIndex = ~0u;
    m_CurrentCmd = nullptr;
}

vk::Result VulkanSwapchain::Present(uint32_t index) {
    vk::Semaphore release = m_PerFrame[index].GetReleaseSemaphore();
    vk::PresentInfoKHR present{
        .waitSemaphoreCount = 1,
        .pWaitSemaphores = &release,
        .swapchainCount = 1,
        .pSwapchains = &m_Swapchain,
        .pImageIndices = &index,
    };

    vk::Result result;
    try {
        result = m_Queue.presentKHR(present);
    } catch (vk::OutOfDateKHRError &) {
        m_NeedsResize = true;
        result = vk::Result::eErrorOutOfDateKHR;
    }
    return result;
}

void VulkanSwapchain::CreateSwapchain(uint32_t width, uint32_t height, vk::SwapchainKHR old_swapchain) {
    vk::SurfaceCapabilitiesKHR surface_properties = m_Gpu.getSurfaceCapabilitiesKHR(m_Surface);

    vk::SurfaceFormatKHR format = SelectSurfaceFormat();

    vk::Extent2D swapchain_size;
    if (surface_properties.currentExtent.width == 0xFFFFFFFF) {
        swapchain_size.width = width;
        swapchain_size.height = height;
    } else {
        swapchain_size = surface_properties.currentExtent;
    }

    vk::PresentModeKHR swapchain_present_mode = vk::PresentModeKHR::eFifo;

    uint32_t desired_swapchain_images = surface_properties.minImageCount + 1;
    if ((surface_properties.maxImageCount > 0) && (desired_swapchain_images > surface_properties.maxImageCount)) {
        desired_swapchain_images = surface_properties.maxImageCount;
    }

    vk::SurfaceTransformFlagBitsKHR pre_transform;
    if (surface_properties.supportedTransforms & vk::SurfaceTransformFlagBitsKHR::eIdentity) {
        pre_transform = vk::SurfaceTransformFlagBitsKHR::eIdentity;
    } else {
        pre_transform = surface_properties.currentTransform;
    }

    vk::CompositeAlphaFlagBitsKHR composite = vk::CompositeAlphaFlagBitsKHR::eOpaque;
    if (surface_properties.supportedCompositeAlpha & vk::CompositeAlphaFlagBitsKHR::eOpaque) {
        composite = vk::CompositeAlphaFlagBitsKHR::eOpaque;
    } else if (surface_properties.supportedCompositeAlpha & vk::CompositeAlphaFlagBitsKHR::eInherit) {
        composite = vk::CompositeAlphaFlagBitsKHR::eInherit;
    } else if (surface_properties.supportedCompositeAlpha & vk::CompositeAlphaFlagBitsKHR::ePreMultiplied) {
        composite = vk::CompositeAlphaFlagBitsKHR::ePreMultiplied;
    } else if (surface_properties.supportedCompositeAlpha & vk::CompositeAlphaFlagBitsKHR::ePostMultiplied) {
        composite = vk::CompositeAlphaFlagBitsKHR::ePostMultiplied;
    }

    vk::SwapchainCreateInfoKHR info{
        .surface = m_Surface,
        .minImageCount = desired_swapchain_images,
        .imageFormat = format.format,
        .imageColorSpace = format.colorSpace,
        .imageExtent = swapchain_size,
        .imageArrayLayers = 1,
        .imageUsage = vk::ImageUsageFlagBits::eColorAttachment,
        .imageSharingMode = vk::SharingMode::eExclusive,
        .preTransform = pre_transform,
        .compositeAlpha = composite,
        .presentMode = swapchain_present_mode,
        .clipped = true,
        .oldSwapchain = old_swapchain,
    };

    m_Swapchain = m_Device.createSwapchainKHR(info);

    m_Dimensions = {swapchain_size.width, swapchain_size.height, format.format};
}

void VulkanSwapchain::CreateImageViews() {
    m_Images = m_Device.getSwapchainImagesKHR(m_Swapchain);

    m_PerFrame.clear();
    m_PerFrame.resize(m_Images.size());
    for (auto &per_frame : m_PerFrame) {
        per_frame.Init(m_Device, static_cast<uint32_t>(m_GraphicsQueueIndex));
    }

    m_ImageViews.reserve(m_Images.size());
    for (auto const &swapchain_image : m_Images) {
        m_ImageViews.push_back(VulkanImage::CreateView(
            m_Device, swapchain_image, vk::ImageViewType::e2D, m_Dimensions.format));
    }
}

vk::SurfaceFormatKHR VulkanSwapchain::SelectSurfaceFormat() {
    std::vector<vk::SurfaceFormatKHR> supported = m_Gpu.getSurfaceFormatsKHR(m_Surface);
    assert(!supported.empty());

    std::vector<vk::Format> preferred = {vk::Format::eR8G8B8A8Srgb, vk::Format::eB8G8R8A8Srgb,
                                         vk::Format::eA8B8G8R8SrgbPack32};

    auto it = std::ranges::find_if(supported, [&preferred](vk::SurfaceFormatKHR sf) {
        return std::ranges::any_of(preferred, [&sf](vk::Format f) { return f == sf.format; });
    });

    return it != supported.end() ? *it : supported[0];
}

} // namespace GE
