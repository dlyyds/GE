//
// Created by Lenovo on 2026/6/3.
//

#include "../../../include/GE/Render/VulkanBase/VulkanSwapchain.h"

#include "Core/Log.h"

#include <cassert>
#include <stdexcept>

namespace GE {
namespace {

// ============================================================================
// Helper — clamped range
// ============================================================================

template <class T>
constexpr const T &clamp(const T &v, const T &lo, const T &hi) {
    return (v < lo) ? lo : ((hi < v) ? hi : v);
}

// ============================================================================
// choose_image_count
// ============================================================================

uint32_t choose_image_count(uint32_t request_image_count,
                            uint32_t min_image_count,
                            uint32_t max_image_count) {
    return clamp(request_image_count, min_image_count,
                 (max_image_count != 0) ? max_image_count : request_image_count);
}

// ============================================================================
// choose_extent
// ============================================================================

vk::Extent2D choose_extent(vk::Extent2D        request_extent,
                           const vk::Extent2D &min_image_extent,
                           const vk::Extent2D &max_image_extent,
                           const vk::Extent2D &current_extent) {
    if (current_extent.width == 0xFFFFFFFF) {
        return request_extent;
    }

    if (request_extent.width < 1 || request_extent.height < 1) {
        GE_CORE_WARN("(VulkanSwapchain) Image extent ({}, {}) not supported. Selecting ({}, {}).",
                     request_extent.width, request_extent.height,
                     current_extent.width, current_extent.height);
        return current_extent;
    }

    request_extent.width  = clamp(request_extent.width,  min_image_extent.width,  max_image_extent.width);
    request_extent.height = clamp(request_extent.height, min_image_extent.height, max_image_extent.height);

    return request_extent;
}

// ============================================================================
// choose_present_mode
// ============================================================================

vk::PresentModeKHR choose_present_mode(vk::PresentModeKHR                     request_present_mode,
                                       const std::vector<vk::PresentModeKHR> &available_present_modes,
                                       const std::vector<vk::PresentModeKHR> &present_mode_priority_list) {
    auto const present_mode_it = std::ranges::find(available_present_modes, request_present_mode);
    if (present_mode_it == available_present_modes.end()) {
        auto const chosen_it = std::ranges::find_if(present_mode_priority_list,
            [&available_present_modes](vk::PresentModeKHR pm) {
                return std::ranges::find(available_present_modes, pm) != available_present_modes.end();
            });

        vk::PresentModeKHR const chosen = (chosen_it != present_mode_priority_list.end())
                                              ? *chosen_it
                                              : vk::PresentModeKHR::eFifo;

        GE_CORE_WARN("(VulkanSwapchain) Present mode '{}' not supported. Selecting '{}'.",
                     vk::to_string(request_present_mode), vk::to_string(chosen));
        return chosen;
    }

    GE_CORE_INFO("(VulkanSwapchain) Present mode selected: {}", vk::to_string(request_present_mode));
    return request_present_mode;
}

// ============================================================================
// choose_surface_format
// ============================================================================

vk::SurfaceFormatKHR choose_surface_format(const vk::SurfaceFormatKHR               requested_surface_format,
                                           const std::vector<vk::SurfaceFormatKHR> &available_surface_formats,
                                           const std::vector<vk::SurfaceFormatKHR> &surface_format_priority_list) {
    auto const format_it = std::ranges::find(available_surface_formats, requested_surface_format);

    if (format_it == available_surface_formats.end()) {
        auto const chosen_it = std::ranges::find_if(surface_format_priority_list,
            [&available_surface_formats](vk::SurfaceFormatKHR sf) {
                return std::ranges::find(available_surface_formats, sf) != available_surface_formats.end();
            });

        vk::SurfaceFormatKHR const &chosen = (chosen_it != surface_format_priority_list.end())
                                                 ? *chosen_it
                                                 : available_surface_formats[0];

        GE_CORE_WARN("(VulkanSwapchain) Surface format ({}) not supported. Selecting ({}).",
                     vk::to_string(requested_surface_format.format) + ", " + vk::to_string(requested_surface_format.colorSpace),
                     vk::to_string(chosen.format) + ", " + vk::to_string(chosen.colorSpace));
        return chosen;
    }

    GE_CORE_INFO("(VulkanSwapchain) Surface format selected: {}",
                 vk::to_string(requested_surface_format.format) + ", " + vk::to_string(requested_surface_format.colorSpace));
    return requested_surface_format;
}

// ============================================================================
// choose_image_array_layers
// ============================================================================

uint32_t choose_image_array_layers(uint32_t request_image_array_layers, uint32_t max_image_array_layers) {
    return clamp(request_image_array_layers, 1u, max_image_array_layers);
}

// ============================================================================
// choose_transform
// ============================================================================

vk::SurfaceTransformFlagBitsKHR choose_transform(vk::SurfaceTransformFlagBitsKHR request_transform,
                                                 vk::SurfaceTransformFlagsKHR    supported_transform,
                                                 vk::SurfaceTransformFlagBitsKHR current_transform) {
    if (request_transform & supported_transform) {
        return request_transform;
    }

    GE_CORE_WARN("(VulkanSwapchain) Surface transform '{}' not supported. Selecting '{}'.",
                 vk::to_string(request_transform), vk::to_string(current_transform));
    return current_transform;
}

// ============================================================================
// choose_composite_alpha
// ============================================================================

vk::CompositeAlphaFlagBitsKHR choose_composite_alpha(vk::CompositeAlphaFlagBitsKHR request_composite_alpha,
                                                     vk::CompositeAlphaFlagsKHR    supported_composite_alpha) {
    if (request_composite_alpha & supported_composite_alpha) {
        return request_composite_alpha;
    }

    static const std::vector<vk::CompositeAlphaFlagBitsKHR> alpha_priority_list = {
        vk::CompositeAlphaFlagBitsKHR::eOpaque,
        vk::CompositeAlphaFlagBitsKHR::ePreMultiplied,
        vk::CompositeAlphaFlagBitsKHR::ePostMultiplied,
        vk::CompositeAlphaFlagBitsKHR::eInherit,
    };

    auto const chosen_it = std::ranges::find_if(alpha_priority_list,
        [&supported_composite_alpha](vk::CompositeAlphaFlagBitsKHR alpha) {
            return alpha & supported_composite_alpha;
        });

    if (chosen_it == alpha_priority_list.end()) {
        throw std::runtime_error("No compatible composite alpha found.");
    }

    GE_CORE_WARN("(VulkanSwapchain) Composite alpha '{}' not supported. Selecting '{}'.",
                 vk::to_string(request_composite_alpha), vk::to_string(*chosen_it));
    return *chosen_it;
}

// ============================================================================
// choose_image_usage
// ============================================================================

bool validate_format_feature(vk::ImageUsageFlagBits image_usage, vk::FormatFeatureFlags supported_features) {
    return (image_usage != vk::ImageUsageFlagBits::eStorage) ||
           (supported_features & vk::FormatFeatureFlagBits::eStorageImage);
}

std::set<vk::ImageUsageFlagBits> choose_image_usage(const std::set<vk::ImageUsageFlagBits> &requested_image_usage_flags,
                                                    vk::ImageUsageFlags                     supported_image_usage,
                                                    vk::FormatFeatureFlags                  supported_features) {
    std::set<vk::ImageUsageFlagBits> validated;
    for (auto flag : requested_image_usage_flags) {
        if ((flag & supported_image_usage) && validate_format_feature(flag, supported_features)) {
            validated.insert(flag);
        } else {
            GE_CORE_WARN("(VulkanSwapchain) Image usage ({}) requested but not supported.", vk::to_string(flag));
        }
    }

    if (validated.empty()) {
        static const std::vector<vk::ImageUsageFlagBits> usage_priority_list = {
            vk::ImageUsageFlagBits::eColorAttachment,
            vk::ImageUsageFlagBits::eStorage,
            vk::ImageUsageFlagBits::eSampled,
            vk::ImageUsageFlagBits::eTransferDst,
        };

        auto const priority_it = std::ranges::find_if(usage_priority_list,
            [&supported_image_usage, &supported_features](auto usage) {
                return (usage & supported_image_usage) && validate_format_feature(usage, supported_features);
            });

        if (priority_it != usage_priority_list.end()) {
            validated.insert(*priority_it);
        }
    }

    if (validated.empty()) {
        throw std::runtime_error("No compatible image usage found.");
    }

    std::string usage_list;
    for (auto u : validated)
        usage_list += vk::to_string(u) + " ";
    GE_CORE_INFO("(VulkanSwapchain) Image usage flags: {}", usage_list);

    return validated;
}

// ============================================================================
// composite_image_flags — 将 std::set 转为位掩码
// ============================================================================

vk::ImageUsageFlags composite_image_flags(const std::set<vk::ImageUsageFlagBits> &image_usage_flags) {
    vk::ImageUsageFlags usage;
    for (auto flag : image_usage_flags)
        usage |= flag;
    return usage;
}

} // anonymous namespace

// ============================================================================
// 重建构造函数：仅修改 extent
// ============================================================================

VulkanSwapchain::VulkanSwapchain(VulkanSwapchain &old_swapchain, const vk::Extent2D &extent) :
    VulkanSwapchain{old_swapchain,
                    old_swapchain.m_Device, old_swapchain.m_Gpu, old_swapchain.m_Surface,
                    old_swapchain.m_Properties.present_mode,
                    old_swapchain.m_PresentModePriorityList,
                    old_swapchain.m_SurfaceFormatPriorityList,
                    extent,
                    old_swapchain.m_Properties.image_count,
                    old_swapchain.m_Properties.pre_transform,
                    old_swapchain.m_ImageUsageFlags} {}

// ============================================================================
// 重建构造函数：仅修改 image count
// ============================================================================

VulkanSwapchain::VulkanSwapchain(VulkanSwapchain &old_swapchain, uint32_t image_count) :
    VulkanSwapchain{old_swapchain,
                    old_swapchain.m_Device, old_swapchain.m_Gpu, old_swapchain.m_Surface,
                    old_swapchain.m_Properties.present_mode,
                    old_swapchain.m_PresentModePriorityList,
                    old_swapchain.m_SurfaceFormatPriorityList,
                    old_swapchain.m_Properties.extent,
                    image_count,
                    old_swapchain.m_Properties.pre_transform,
                    old_swapchain.m_ImageUsageFlags} {}

// ============================================================================
// 重建构造函数：仅修改 image usage
// ============================================================================

VulkanSwapchain::VulkanSwapchain(VulkanSwapchain &old_swapchain, const std::set<vk::ImageUsageFlagBits> &image_usage_flags) :
    VulkanSwapchain{old_swapchain,
                    old_swapchain.m_Device, old_swapchain.m_Gpu, old_swapchain.m_Surface,
                    old_swapchain.m_Properties.present_mode,
                    old_swapchain.m_PresentModePriorityList,
                    old_swapchain.m_SurfaceFormatPriorityList,
                    old_swapchain.m_Properties.extent,
                    old_swapchain.m_Properties.image_count,
                    old_swapchain.m_Properties.pre_transform,
                    image_usage_flags} {}

// ============================================================================
// 重建构造函数：修改 extent + transform
// ============================================================================

VulkanSwapchain::VulkanSwapchain(VulkanSwapchain &swapchain,
                                 const vk::Extent2D &extent,
                                 const vk::SurfaceTransformFlagBitsKHR transform) :
    VulkanSwapchain{swapchain,
                    swapchain.m_Device, swapchain.m_Gpu, swapchain.m_Surface,
                    swapchain.m_Properties.present_mode,
                    swapchain.m_PresentModePriorityList,
                    swapchain.m_SurfaceFormatPriorityList,
                    extent,
                    swapchain.m_Properties.image_count,
                    transform,
                    swapchain.m_ImageUsageFlags} {}

// ============================================================================
// 主构造函数（公开入口）→ 委托到完整构造函数
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
    VulkanSwapchain{*this,
                    device, gpu, surface,
                    present_mode,
                    present_mode_priority_list,
                    surface_format_priority_list,
                    extent,
                    image_count,
                    transform,
                    image_usage_flags} {}

// ============================================================================
// 完整构造函数（内部入口，所有重建构造函数委托至此）
// ============================================================================

VulkanSwapchain::VulkanSwapchain(VulkanSwapchain                               &old_swapchain,
                                 vk::Device                                     device,
                                 vk::PhysicalDevice                             gpu,
                                 vk::SurfaceKHR                                 surface,
                                 const vk::PresentModeKHR                       present_mode,
                                 const std::vector<vk::PresentModeKHR>         &present_mode_priority_list,
                                 const std::vector<vk::SurfaceFormatKHR>       &surface_format_priority_list,
                                 const vk::Extent2D                            &extent,
                                 uint32_t                                       image_count,
                                 const vk::SurfaceTransformFlagBitsKHR          transform,
                                 const std::set<vk::ImageUsageFlagBits>        &image_usage_flags) :
    m_Device(device),
    m_Gpu(gpu),
    m_Surface(surface) {
    // 存储优先级列表
    this->m_PresentModePriorityList   = present_mode_priority_list;
    this->m_SurfaceFormatPriorityList = surface_format_priority_list;

    // 日志：surface 支持的格式
    std::vector<vk::SurfaceFormatKHR> surface_formats = m_Gpu.getSurfaceFormatsKHR(m_Surface);
    GE_CORE_INFO("Surface supports the following surface formats:");
    for (auto &sf : surface_formats) {
        GE_CORE_INFO("  \t{}", vk::to_string(sf.format) + ", " + vk::to_string(sf.colorSpace));
    }

    // 日志：surface 支持的呈现模式
    std::vector<vk::PresentModeKHR> present_modes = m_Gpu.getSurfacePresentModesKHR(m_Surface);
    GE_CORE_INFO("Surface supports the following present modes:");
    for (auto &pm : present_modes) {
        GE_CORE_INFO("  \t{}", vk::to_string(pm));
    }

    // 基于 surface capabilities 选择最佳属性
    vk::SurfaceCapabilitiesKHR const caps = m_Gpu.getSurfaceCapabilitiesKHR(m_Surface);

    m_Properties.old_swapchain  = old_swapchain.m_Handle;
    m_Properties.image_count    = choose_image_count(image_count, caps.minImageCount, caps.maxImageCount);
    m_Properties.extent         = choose_extent(extent, caps.minImageExtent, caps.maxImageExtent, caps.currentExtent);
    m_Properties.surface_format = choose_surface_format(m_Properties.surface_format, surface_formats, surface_format_priority_list);
    m_Properties.array_layers   = choose_image_array_layers(1U, caps.maxImageArrayLayers);

    vk::FormatProperties const format_props = m_Gpu.getFormatProperties(m_Properties.surface_format.format);
    this->m_ImageUsageFlags                 = choose_image_usage(image_usage_flags, caps.supportedUsageFlags, format_props.optimalTilingFeatures);

    m_Properties.image_usage     = composite_image_flags(this->m_ImageUsageFlags);
    m_Properties.pre_transform   = choose_transform(transform, caps.supportedTransforms, caps.currentTransform);
    m_Properties.composite_alpha = choose_composite_alpha(vk::CompositeAlphaFlagBitsKHR::eInherit, caps.supportedCompositeAlpha);
    m_Properties.present_mode    = choose_present_mode(present_mode, present_modes, present_mode_priority_list);

    // 创建 Vulkan swapchain
    vk::SwapchainCreateInfoKHR create_info{
        .surface          = m_Surface,
        .minImageCount    = m_Properties.image_count,
        .imageFormat      = m_Properties.surface_format.format,
        .imageColorSpace  = m_Properties.surface_format.colorSpace,
        .imageExtent      = m_Properties.extent,
        .imageArrayLayers = m_Properties.array_layers,
        .imageUsage       = m_Properties.image_usage,
        .preTransform     = m_Properties.pre_transform,
        .compositeAlpha   = m_Properties.composite_alpha,
        .presentMode      = m_Properties.present_mode,
        .clipped          = true,
        .oldSwapchain     = m_Properties.old_swapchain,
    };

    m_Handle = m_Device.createSwapchainKHR(create_info);

    // 获取 swapchain images
    m_Images = m_Device.getSwapchainImagesKHR(m_Handle);
}

// ============================================================================
// 析构函数
// ============================================================================

VulkanSwapchain::~VulkanSwapchain() {
    if (m_Handle) {
        m_Device.destroySwapchainKHR(m_Handle);
    }
}

// ============================================================================
// 移动构造函数
// ============================================================================

VulkanSwapchain::VulkanSwapchain(VulkanSwapchain &&other) noexcept :
    m_Device{std::exchange(other.m_Device, nullptr)},
    m_Gpu{std::exchange(other.m_Gpu, nullptr)},
    m_Surface{std::exchange(other.m_Surface, nullptr)},
    m_Handle{std::exchange(other.m_Handle, nullptr)},
    m_Properties{std::exchange(other.m_Properties, {})},
    m_Images{std::exchange(other.m_Images, {})},
    m_PresentModePriorityList{std::exchange(other.m_PresentModePriorityList, {})},
    m_SurfaceFormatPriorityList{std::exchange(other.m_SurfaceFormatPriorityList, {})},
    m_ImageUsageFlags{std::move(other.m_ImageUsageFlags)} {
    other.m_ImageUsageFlags.clear();
}

// ============================================================================
// AcquireNextImage
// ============================================================================

std::pair<vk::Result, uint32_t> VulkanSwapchain::AcquireNextImage(vk::Semaphore image_acquired_semaphore, vk::Fence fence) const {
    vk::ResultValue<uint32_t> rv = m_Device.acquireNextImageKHR(m_Handle, std::numeric_limits<uint64_t>::max(), image_acquired_semaphore, fence);
    return {rv.result, rv.value};
}

} // namespace GE
