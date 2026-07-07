//
// Created by Lenovo on 2026/6/3.
//

#include "../../../include/GE/Render/VulkanBase/VulkanSwapchain.h"
#include "../../../include/GE/Render/VulkanBase/VulkanImage.h"

#include "Core/Log.h"

#include <cassert>
#include <stdexcept>

namespace GE {

// ============================================================================
// 主构造函数
// ============================================================================

VulkanSwapchain::VulkanSwapchain(vk::Device device, vk::PhysicalDevice gpu, vk::SurfaceKHR surface,
                                 vk::Queue queue, int32_t graphics_queue_index,
                                 const std::vector<vk::SurfaceFormatKHR> &surface_format_priority_list,
                                 const std::vector<vk::PresentModeKHR>   &present_mode_priority_list,
                                 const VulkanSwapchainProperties         &properties) :
    m_Device(device),
    m_Gpu(gpu),
    m_Surface(surface),
    m_Queue(queue),
    m_GraphicsQueueIndex(graphics_queue_index) {
    // 通过优先级列表选择表面格式（如果未指定则自动选择）
    if (properties.surface_format.format == vk::Format::eUndefined) {
        m_Properties.surface_format = SelectSurfaceFormat(surface_format_priority_list);
    } else {
        m_Properties.surface_format = properties.surface_format;
    }

    // 如果未指定呈现模式，从优先级列表中选择
    if (properties.present_mode == vk::PresentModeKHR::eFifo) {
        m_Properties.present_mode = SelectPresentMode(present_mode_priority_list);
    } else {
        m_Properties.present_mode = properties.present_mode;
    }

    m_Properties.image_count     = properties.image_count;
    m_Properties.array_layers    = properties.array_layers;
    m_Properties.image_usage     = properties.image_usage;
    m_Properties.pre_transform   = properties.pre_transform;
    m_Properties.composite_alpha = properties.composite_alpha;
    m_Properties.old_swapchain   = properties.old_swapchain;

    Create(properties);
    CreateImageViews();
}

// ============================================================================
// 重建构造函数（修改 extent）
// ============================================================================

VulkanSwapchain::VulkanSwapchain(VulkanSwapchain &old_swapchain, const vk::Extent2D &extent) :
    m_Device(old_swapchain.m_Device),
    m_Gpu(old_swapchain.m_Gpu),
    m_Surface(old_swapchain.m_Surface),
    m_Queue(old_swapchain.m_Queue),
    m_GraphicsQueueIndex(old_swapchain.m_GraphicsQueueIndex) {
    VulkanSwapchainProperties props = old_swapchain.m_Properties;
    props.extent        = extent;
    props.old_swapchain = old_swapchain.m_Handle;

    m_Properties = props;
    Create(props);
    CreateImageViews();
}

// ============================================================================
// 重建构造函数（修改 image count）
// ============================================================================

VulkanSwapchain::VulkanSwapchain(VulkanSwapchain &old_swapchain, uint32_t image_count) :
    m_Device(old_swapchain.m_Device),
    m_Gpu(old_swapchain.m_Gpu),
    m_Surface(old_swapchain.m_Surface),
    m_Queue(old_swapchain.m_Queue),
    m_GraphicsQueueIndex(old_swapchain.m_GraphicsQueueIndex) {
    VulkanSwapchainProperties props = old_swapchain.m_Properties;
    props.image_count   = image_count;
    props.old_swapchain = old_swapchain.m_Handle;

    m_Properties = props;
    Create(props);
    CreateImageViews();
}

// ============================================================================
// 析构函数
// ============================================================================

VulkanSwapchain::~VulkanSwapchain() {
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

    if (m_Handle)
        m_Device.destroySwapchainKHR(m_Handle);
    m_Handle = nullptr;
}

// ============================================================================
// 移动构造函数
// ============================================================================

VulkanSwapchain::VulkanSwapchain(VulkanSwapchain &&other) noexcept :
    m_Device(std::move(other.m_Device)),
    m_Gpu(std::move(other.m_Gpu)),
    m_Surface(std::move(other.m_Surface)),
    m_Queue(std::move(other.m_Queue)),
    m_GraphicsQueueIndex(std::move(other.m_GraphicsQueueIndex)),
    m_Handle(std::move(other.m_Handle)),
    m_Properties(std::move(other.m_Properties)),
    m_Images(std::move(other.m_Images)),
    m_ImageViews(std::move(other.m_ImageViews)),
    m_PerFrame(std::move(other.m_PerFrame)),
    m_RecycledSemaphores(std::move(other.m_RecycledSemaphores)),
    m_CurrentImageIndex(std::move(other.m_CurrentImageIndex)),
    m_CurrentCmd(std::move(other.m_CurrentCmd)),
    m_NeedsResize(std::move(other.m_NeedsResize)) {
    other.m_Device              = nullptr;
    other.m_Gpu                 = nullptr;
    other.m_Surface             = nullptr;
    other.m_Queue               = nullptr;
    other.m_GraphicsQueueIndex  = -1;
    other.m_Handle              = nullptr;
    other.m_Images.clear();
    other.m_ImageViews.clear();
    other.m_PerFrame.clear();
    other.m_RecycledSemaphores.clear();
    other.m_CurrentImageIndex   = ~0u;
    other.m_CurrentCmd          = nullptr;
    other.m_NeedsResize          = false;
}

// ============================================================================
// BeginFrame — 公开帧开始入口
// ============================================================================

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

// ============================================================================
// EndFrame — 公开帧结束入口
// ============================================================================

void VulkanSwapchain::EndFrame() {
    EndFrame(m_CurrentImageIndex);

    auto res = Present(m_CurrentImageIndex);
    if (res != vk::Result::eSuccess) {
        GE_CORE_ERROR("Failed to present swapchain image.");
    }

    m_CurrentImageIndex = ~0u;
    m_CurrentCmd = nullptr;
}

// ============================================================================
// Resize — 检查 surface 尺寸变化并重建
// ============================================================================

void VulkanSwapchain::Resize() {
    auto surface_properties = m_Gpu.getSurfaceCapabilitiesKHR(m_Surface);
    if (surface_properties.currentExtent.width  == m_Properties.extent.width &&
        surface_properties.currentExtent.height == m_Properties.extent.height) {
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

    VulkanSwapchainProperties props = m_Properties;
    props.extent        = surface_properties.currentExtent;
    props.old_swapchain = m_Handle;
    m_Handle = nullptr;

    m_Properties.extent = surface_properties.currentExtent;

    GE_CORE_TRACE("SwapChain resize {} {}", m_Properties.extent.width, m_Properties.extent.height);

    Create(props);
    CreateImageViews();

    if (props.old_swapchain)
        m_Device.destroySwapchainKHR(props.old_swapchain);
}

// ============================================================================
// AcquireNextImage
// ============================================================================

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
        std::tie(result, *image) = m_Device.acquireNextImageKHR(m_Handle, UINT64_MAX, acquire_semaphore);
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

// ============================================================================
// BeginFrame（私有，每个 image index）
// ============================================================================

vk::CommandBuffer VulkanSwapchain::BeginFrame(uint32_t imageIndex) {
    auto cmd = GetCommandBuffer(imageIndex);

    vk::CommandBufferBeginInfo begin_info{.flags = vk::CommandBufferUsageFlagBits::eOneTimeSubmit};
    cmd.begin(begin_info);

    VulkanImage::TransitionLayout(cmd, m_Images[imageIndex],
                                  vk::ImageLayout::eUndefined, vk::ImageLayout::eColorAttachmentOptimal);

    return cmd;
}

// ============================================================================
// EndFrame（私有，每个 image index）
// ============================================================================

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

// ============================================================================
// Present
// ============================================================================

vk::Result VulkanSwapchain::Present(uint32_t index) {
    vk::Semaphore release = m_PerFrame[index].GetReleaseSemaphore();
    vk::PresentInfoKHR present{
        .waitSemaphoreCount = 1,
        .pWaitSemaphores = &release,
        .swapchainCount = 1,
        .pSwapchains = &m_Handle,
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

// ============================================================================
// Create — 内部 swapchain 创建
// ============================================================================

void VulkanSwapchain::Create(const VulkanSwapchainProperties &props) {
    vk::SurfaceCapabilitiesKHR surface_properties = m_Gpu.getSurfaceCapabilitiesKHR(m_Surface);

    vk::Extent2D swapchain_size;
    if (surface_properties.currentExtent.width == 0xFFFFFFFF) {
        swapchain_size.width  = props.extent.width;
        swapchain_size.height = props.extent.height;
    } else {
        swapchain_size = surface_properties.currentExtent;
    }

    uint32_t desired_swapchain_images = surface_properties.minImageCount + 1;
    if ((surface_properties.maxImageCount > 0) &&
        (desired_swapchain_images > surface_properties.maxImageCount)) {
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
        .surface               = m_Surface,
        .minImageCount         = desired_swapchain_images,
        .imageFormat           = props.surface_format.format,
        .imageColorSpace       = props.surface_format.colorSpace,
        .imageExtent           = swapchain_size,
        .imageArrayLayers      = props.array_layers,
        .imageUsage            = props.image_usage,
        .imageSharingMode      = vk::SharingMode::eExclusive,
        .preTransform          = pre_transform,
        .compositeAlpha        = composite,
        .presentMode           = props.present_mode,
        .clipped               = true,
        .oldSwapchain          = props.old_swapchain,
    };

    m_Handle = m_Device.createSwapchainKHR(info);

    // 更新属性中的运行时值
    m_Properties.extent        = swapchain_size;
    m_Properties.pre_transform = pre_transform;
    m_Properties.composite_alpha = composite;
}

// ============================================================================
// CreateImageViews
// ============================================================================

void VulkanSwapchain::CreateImageViews() {
    m_Images = m_Device.getSwapchainImagesKHR(m_Handle);

    m_PerFrame.clear();
    m_PerFrame.resize(m_Images.size());
    for (auto &per_frame : m_PerFrame) {
        per_frame.Init(m_Device, static_cast<uint32_t>(m_GraphicsQueueIndex));
    }

    m_ImageViews.reserve(m_Images.size());
    for (auto const &swapchain_image : m_Images) {
        m_ImageViews.push_back(VulkanImage::CreateView(
            m_Device, swapchain_image, vk::ImageViewType::e2D, m_Properties.surface_format.format));
    }
}

// ============================================================================
// SelectSurfaceFormat — 从优先级列表中选择
// ============================================================================

vk::SurfaceFormatKHR VulkanSwapchain::SelectSurfaceFormat(const std::vector<vk::SurfaceFormatKHR> &priority_list) {
    std::vector<vk::SurfaceFormatKHR> supported = m_Gpu.getSurfaceFormatsKHR(m_Surface);
    assert(!supported.empty());

    for (auto const &preferred : priority_list) {
        auto it = std::ranges::find_if(supported, [&preferred](vk::SurfaceFormatKHR sf) {
            return sf.format == preferred.format && sf.colorSpace == preferred.colorSpace;
        });
        if (it != supported.end()) {
            return *it;
        }
    }

    return supported[0];
}

// ============================================================================
// SelectPresentMode — 从优先级列表中选择
// ============================================================================

vk::PresentModeKHR VulkanSwapchain::SelectPresentMode(const std::vector<vk::PresentModeKHR> &priority_list) {
    std::vector<vk::PresentModeKHR> supported = m_Gpu.getSurfacePresentModesKHR(m_Surface);

    for (auto const &preferred : priority_list) {
        auto it = std::ranges::find(supported, preferred);
        if (it != supported.end()) {
            return *it;
        }
    }

    return vk::PresentModeKHR::eFifo;
}

} // namespace GE
