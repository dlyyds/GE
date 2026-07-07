//
// Created by Lenovo on 2026/6/3.
//

#include "../../../include/GE/Render/VulkanBase/VulkanSwapchain.h"

#include <cassert>
#include <stdexcept>

#include "Core/Log.h"

namespace GE {

// ============================================================================
// 主构造函数
// ============================================================================

VulkanSwapchain::VulkanSwapchain(vk::Device                                       device,
                                 vk::PhysicalDevice                               gpu,
                                 vk::SurfaceKHR                                   surface,
                                 const vk::PresentModeKHR                         present_mode,
                                 const std::vector<vk::PresentModeKHR>           &present_mode_priority_list,
                                 const std::vector<vk::SurfaceFormatKHR>         &surface_format_priority_list,
                                 const vk::Extent2D                              &extent,
                                 uint32_t                                         image_count,
                                 const vk::SurfaceTransformFlagBitsKHR            transform,
                                 const std::set<vk::ImageUsageFlagBits>          &image_usage_flags) :
    m_Device(device),
    m_Gpu(gpu),
    m_Surface(surface),
    m_PresentModePriorityList(present_mode_priority_list),
    m_SurfaceFormatPriorityList(surface_format_priority_list),
    m_ImageUsageFlags(image_usage_flags) {
    m_Properties.surface_format = SelectSurfaceFormat(surface_format_priority_list);
    m_Properties.present_mode   = present_mode;
    m_Properties.extent         = extent;
    m_Properties.image_count    = image_count;
    m_Properties.pre_transform  = transform;
    m_Properties.array_layers   = 1;
    m_Properties.old_swapchain  = nullptr;

    // 将 std::set 转换为 vk::ImageUsageFlags 位掩码
    vk::ImageUsageFlags usage;
    for (auto flag : image_usage_flags) {
        usage |= flag;
    }
    m_Properties.image_usage = usage;

    Create(m_Properties);
}

// ============================================================================
// 重建构造函数：仅修改 extent
// ============================================================================

VulkanSwapchain::VulkanSwapchain(VulkanSwapchain &old_swapchain, const vk::Extent2D &extent) :
    m_Device(old_swapchain.m_Device),
    m_Gpu(old_swapchain.m_Gpu),
    m_Surface(old_swapchain.m_Surface),
    m_PresentModePriorityList(old_swapchain.m_PresentModePriorityList),
    m_SurfaceFormatPriorityList(old_swapchain.m_SurfaceFormatPriorityList),
    m_ImageUsageFlags(old_swapchain.m_ImageUsageFlags) {
    m_Properties          = old_swapchain.m_Properties;
    m_Properties.extent   = extent;
    m_Properties.old_swapchain = old_swapchain.m_Handle;

    Create(m_Properties);
}

// ============================================================================
// 重建构造函数：仅修改 image count
// ============================================================================

VulkanSwapchain::VulkanSwapchain(VulkanSwapchain &old_swapchain, uint32_t image_count) :
    m_Device(old_swapchain.m_Device),
    m_Gpu(old_swapchain.m_Gpu),
    m_Surface(old_swapchain.m_Surface),
    m_PresentModePriorityList(old_swapchain.m_PresentModePriorityList),
    m_SurfaceFormatPriorityList(old_swapchain.m_SurfaceFormatPriorityList),
    m_ImageUsageFlags(old_swapchain.m_ImageUsageFlags) {
    m_Properties            = old_swapchain.m_Properties;
    m_Properties.image_count = image_count;
    m_Properties.old_swapchain = old_swapchain.m_Handle;

    Create(m_Properties);
}

// ============================================================================
// 重建构造函数：仅修改 image usage
// ============================================================================

VulkanSwapchain::VulkanSwapchain(VulkanSwapchain &old_swapchain, const std::set<vk::ImageUsageFlagBits> &image_usage_flags) :
    m_Device(old_swapchain.m_Device),
    m_Gpu(old_swapchain.m_Gpu),
    m_Surface(old_swapchain.m_Surface),
    m_PresentModePriorityList(old_swapchain.m_PresentModePriorityList),
    m_SurfaceFormatPriorityList(old_swapchain.m_SurfaceFormatPriorityList),
    m_ImageUsageFlags(image_usage_flags) {
    m_Properties = old_swapchain.m_Properties;
    m_Properties.old_swapchain = old_swapchain.m_Handle;

    vk::ImageUsageFlags usage;
    for (auto flag : image_usage_flags) {
        usage |= flag;
    }
    m_Properties.image_usage = usage;

    Create(m_Properties);
}

// ============================================================================
// 重建构造函数：修改 extent + transform
// ============================================================================

VulkanSwapchain::VulkanSwapchain(VulkanSwapchain &swapchain,
                                 const vk::Extent2D &extent,
                                 const vk::SurfaceTransformFlagBitsKHR transform) :
    m_Device(swapchain.m_Device),
    m_Gpu(swapchain.m_Gpu),
    m_Surface(swapchain.m_Surface),
    m_PresentModePriorityList(swapchain.m_PresentModePriorityList),
    m_SurfaceFormatPriorityList(swapchain.m_SurfaceFormatPriorityList),
    m_ImageUsageFlags(swapchain.m_ImageUsageFlags) {
    m_Properties              = swapchain.m_Properties;
    m_Properties.extent       = extent;
    m_Properties.pre_transform = transform;
    m_Properties.old_swapchain = swapchain.m_Handle;

    Create(m_Properties);
}

// ============================================================================
// 析构函数
// ============================================================================

VulkanSwapchain::~VulkanSwapchain() {
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
    m_Handle(std::move(other.m_Handle)),
    m_Properties(std::move(other.m_Properties)),
    m_Images(std::move(other.m_Images)),
    m_PresentModePriorityList(std::move(other.m_PresentModePriorityList)),
    m_SurfaceFormatPriorityList(std::move(other.m_SurfaceFormatPriorityList)),
    m_ImageUsageFlags(std::move(other.m_ImageUsageFlags)) {
    other.m_Device  = nullptr;
    other.m_Gpu     = nullptr;
    other.m_Surface = nullptr;
    other.m_Handle  = nullptr;
    other.m_Images.clear();
    other.m_PresentModePriorityList.clear();
    other.m_SurfaceFormatPriorityList.clear();
    other.m_ImageUsageFlags.clear();
}

// ============================================================================
// AcquireNextImage
// ============================================================================

std::pair<vk::Result, uint32_t> VulkanSwapchain::AcquireNextImage(vk::Semaphore image_acquired_semaphore, vk::Fence fence) const {
    uint32_t image_index;
    vk::Result result = m_Device.acquireNextImageKHR(m_Handle, UINT64_MAX, image_acquired_semaphore, fence, &image_index);
    return {result, image_index};
}

// ============================================================================
// Create — 内部 swapchain 创建
// ============================================================================

void VulkanSwapchain::Create(const VulkanSwapchainProperties &props) {
    vk::SurfaceCapabilitiesKHR surface_properties = m_Gpu.getSurfaceCapabilitiesKHR(m_Surface);

    // extent：如果 surface 返回 0xFFFFFFFF，用请求尺寸；否则用 surface 报告值
    vk::Extent2D swapchain_size;
    if (surface_properties.currentExtent.width == 0xFFFFFFFF) {
        swapchain_size.width  = props.extent.width;
        swapchain_size.height = props.extent.height;
    } else {
        swapchain_size = surface_properties.currentExtent;
    }

    // image count：min + 1，不超过 max
    uint32_t desired_count = surface_properties.minImageCount + 1;
    if ((surface_properties.maxImageCount > 0) &&
        (desired_count > surface_properties.maxImageCount)) {
        desired_count = surface_properties.maxImageCount;
    }

    // pre_transform：优先 identity
    vk::SurfaceTransformFlagBitsKHR pre_transform;
    if (surface_properties.supportedTransforms & vk::SurfaceTransformFlagBitsKHR::eIdentity) {
        pre_transform = vk::SurfaceTransformFlagBitsKHR::eIdentity;
    } else {
        pre_transform = surface_properties.currentTransform;
    }

    // composite_alpha：按优先级选择
    vk::CompositeAlphaFlagBitsKHR composite = vk::CompositeAlphaFlagBitsKHR::eOpaque;
    const std::vector<vk::CompositeAlphaFlagBitsKHR> alpha_preference = {
        vk::CompositeAlphaFlagBitsKHR::eOpaque,
        vk::CompositeAlphaFlagBitsKHR::eInherit,
        vk::CompositeAlphaFlagBitsKHR::ePreMultiplied,
        vk::CompositeAlphaFlagBitsKHR::ePostMultiplied,
    };
    for (auto alpha : alpha_preference) {
        if (surface_properties.supportedCompositeAlpha & alpha) {
            composite = alpha;
            break;
        }
    }

    vk::SwapchainCreateInfoKHR info{
        .surface          = m_Surface,
        .minImageCount    = desired_count,
        .imageFormat      = props.surface_format.format,
        .imageColorSpace  = props.surface_format.colorSpace,
        .imageExtent      = swapchain_size,
        .imageArrayLayers = props.array_layers,
        .imageUsage       = props.image_usage,
        .imageSharingMode = vk::SharingMode::eExclusive,
        .preTransform     = pre_transform,
        .compositeAlpha   = composite,
        .presentMode      = props.present_mode,
        .clipped          = true,
        .oldSwapchain     = props.old_swapchain,
    };

    m_Handle = m_Device.createSwapchainKHR(info);

    // 更新属性中的运行时值
    m_Properties.extent          = swapchain_size;
    m_Properties.pre_transform   = pre_transform;
    m_Properties.composite_alpha = composite;
    m_Properties.image_count     = desired_count;

    // 获取 swapchain images
    m_Images = m_Device.getSwapchainImagesKHR(m_Handle);

    GE_CORE_INFO("Swapchain created: {}x{}, {} images, format {}, present mode {}",
                 swapchain_size.width, swapchain_size.height, desired_count,
                 vk::to_string(props.surface_format.format),
                 vk::to_string(props.present_mode));
}

// ============================================================================
// SelectSurfaceFormat
// ============================================================================

vk::SurfaceFormatKHR VulkanSwapchain::SelectSurfaceFormat(const std::vector<vk::SurfaceFormatKHR> &priority_list) {
    std::vector<vk::SurfaceFormatKHR> supported = m_Gpu.getSurfaceFormatsKHR(m_Surface);
    assert(!supported.empty());

    // 按优先级顺序，找第一个 format + colorSpace 完全匹配的
    for (auto const &preferred : priority_list) {
        auto it = std::ranges::find_if(supported, [&preferred](vk::SurfaceFormatKHR sf) {
            return sf.format == preferred.format && sf.colorSpace == preferred.colorSpace;
        });
        if (it != supported.end()) {
            return *it;
        }
    }

    // 降级：只匹配 format
    for (auto const &preferred : priority_list) {
        auto it = std::ranges::find_if(supported, [&preferred](vk::SurfaceFormatKHR sf) {
            return sf.format == preferred.format;
        });
        if (it != supported.end()) {
            return *it;
        }
    }

    return supported[0];
}

} // namespace GE
