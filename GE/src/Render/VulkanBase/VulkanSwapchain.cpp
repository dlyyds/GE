//
// Created by Lenovo on 2026/6/3.
//

#include "Render/VulkanBase/VulkanSwapchain.h"

#include "Core/Log.h"

#include <cassert>
#include <stdexcept>

namespace GE {
namespace {

// ============================================================================
// Helper — 限幅
// ============================================================================

template <class T>
constexpr const T &clamp(const T &v, const T &lo, const T &hi) {
    return (v < lo) ? lo : ((hi < v) ? hi : v);
}

// ============================================================================
// choose_image_count — 根据 surface 能力选择 image 数量
// ============================================================================

inline uint32_t choose_image_count(uint32_t request_image_count,
                                   uint32_t min_image_count,
                                   uint32_t max_image_count) {
    return clamp(request_image_count, min_image_count,
                 (max_image_count != 0) ? max_image_count : request_image_count);
}

// ============================================================================
// choose_extent — 根据 surface 能力选择 extent
// ============================================================================

vk::Extent2D choose_extent(vk::Extent2D request_extent,
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

    request_extent.width = clamp(request_extent.width, min_image_extent.width, max_image_extent.width);
    request_extent.height = clamp(request_extent.height, min_image_extent.height, max_image_extent.height);

    return request_extent;
}

// ============================================================================
// choose_present_mode — 根据 surface 支持的呈现模式选择
// ============================================================================

vk::PresentModeKHR choose_present_mode(vk::PresentModeKHR request_present_mode,
                                       const std::vector<vk::PresentModeKHR> &available_present_modes,
                                       const std::vector<vk::PresentModeKHR> &present_mode_priority_list) {
    // 尝试查找请求的呈现模式
    auto const present_mode_it = std::ranges::find(available_present_modes, request_present_mode);
    if (present_mode_it == available_present_modes.end()) {
        // 请求的模式不支持，从优先级列表中查找
        auto const chosen_it = std::ranges::find_if(present_mode_priority_list,
                                                    [&available_present_modes](vk::PresentModeKHR pm) {
                                                        return std::ranges::find(available_present_modes, pm) != available_present_modes.end();
                                                    });

        // 如果都没找到，始终默认 FIFO
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
// choose_surface_format — 根据 surface 支持的格式选择
// ============================================================================

vk::SurfaceFormatKHR choose_surface_format(const vk::SurfaceFormatKHR requested_surface_format,
                                           const std::vector<vk::SurfaceFormatKHR> &available_surface_formats,
                                           const std::vector<vk::SurfaceFormatKHR> &surface_format_priority_list) {
    // 尝试查找请求的格式
    auto const format_it = std::ranges::find(available_surface_formats, requested_surface_format);

    if (format_it == available_surface_formats.end()) {
        // 请求的格式不支持，从优先级列表中查找
        auto const chosen_it = std::ranges::find_if(surface_format_priority_list,
                                                    [&available_surface_formats](vk::SurfaceFormatKHR sf) {
                                                        return std::ranges::find(available_surface_formats, sf) != available_surface_formats.end();
                                                    });

        // 如果都没找到，默认使用第一个可用格式
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

inline uint32_t choose_image_array_layers(uint32_t request_image_array_layers, uint32_t max_image_array_layers) {
    return clamp(request_image_array_layers, 1u, max_image_array_layers);
}

// ============================================================================
// choose_transform
// ============================================================================

vk::SurfaceTransformFlagBitsKHR choose_transform(vk::SurfaceTransformFlagBitsKHR request_transform,
                                                 vk::SurfaceTransformFlagsKHR supported_transform,
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
                                                     vk::CompositeAlphaFlagsKHR supported_composite_alpha) {
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
                                                    return static_cast<bool>(alpha & supported_composite_alpha);
                                                });

    if (chosen_it == alpha_priority_list.end()) {
        throw std::runtime_error("No compatible composite alpha found.");
    }

    GE_CORE_WARN("(VulkanSwapchain) Composite alpha '{}' not supported. Selecting '{}'.",
                 vk::to_string(request_composite_alpha), vk::to_string(*chosen_it));
    return *chosen_it;
}

// ============================================================================
// validate_format_feature
// ============================================================================

bool validate_format_feature(vk::ImageUsageFlagBits image_usage, vk::FormatFeatureFlags supported_features) {
    return (image_usage != vk::ImageUsageFlagBits::eStorage) ||
           (supported_features & vk::FormatFeatureFlagBits::eStorageImage);
}

// ============================================================================
// choose_image_usage
// ============================================================================

std::set<vk::ImageUsageFlagBits> choose_image_usage(const std::set<vk::ImageUsageFlagBits> &requested_image_usage_flags,
                                                    vk::ImageUsageFlags supported_image_usage,
                                                    vk::FormatFeatureFlags supported_features) {
    std::set<vk::ImageUsageFlagBits> validated;
    for (auto flag : requested_image_usage_flags) {
        if ((flag & supported_image_usage) && validate_format_feature(flag, supported_features)) {
            validated.insert(flag);
        } else {
            GE_CORE_WARN("(VulkanSwapchain) Image usage ({}) requested but not supported.", vk::to_string(flag));
        }
    }

    if (validated.empty()) {
        // 从默认优先级列表中选取第一个支持的用法
        static const std::vector<vk::ImageUsageFlagBits> usage_priority_list = {
            vk::ImageUsageFlagBits::eColorAttachment,
            vk::ImageUsageFlagBits::eStorage,
            vk::ImageUsageFlagBits::eSampled,
            vk::ImageUsageFlagBits::eTransferDst,
        };

        auto const priority_it = std::ranges::find_if(usage_priority_list,
                                                      [&supported_image_usage, &supported_features](auto usage) {
                                                          return static_cast<bool>(usage & supported_image_usage) && validate_format_feature(
                                                                     usage, supported_features);
                                                      });

        if (priority_it != usage_priority_list.end()) {
            validated.insert(*priority_it);
        }
    }

    if (validated.empty()) {
        throw std::runtime_error("No compatible image usage found.");
    }

    // 日志：输出使用的 image usage flags
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

// ============================================================================
// query_applied_compression — 查询 swapchain image 实际应用的压缩参数
// ============================================================================


} // anonymous namespace

// ============================================================================
// 重建构造函数：仅修改 extent
// ============================================================================

VulkanSwapchain::VulkanSwapchain(VulkanSwapchain &old_swapchain, const vk::Extent2D &extent) : VulkanSwapchain{old_swapchain,
    old_swapchain.m_Device,
    old_swapchain.m_Surface,
    old_swapchain.m_Properties.present_mode,
    old_swapchain.m_PresentModePriorityList,
    old_swapchain.m_SurfaceFormatPriorityList,
    extent,
    old_swapchain.m_Properties.image_count,
    old_swapchain.m_Properties.pre_transform,
    old_swapchain.m_ImageUsageFlags,
    old_swapchain.m_RequestedCompression,
    old_swapchain.m_RequestedCompressionFixedRate} {
}

// ============================================================================
// 重建构造函数：仅修改 image count
// ============================================================================

VulkanSwapchain::VulkanSwapchain(VulkanSwapchain &old_swapchain, uint32_t image_count) : VulkanSwapchain{old_swapchain,
    old_swapchain.m_Device,
    old_swapchain.m_Surface,
    old_swapchain.m_Properties.present_mode,
    old_swapchain.m_PresentModePriorityList,
    old_swapchain.m_SurfaceFormatPriorityList,
    old_swapchain.m_Properties.extent,
    image_count,
    old_swapchain.m_Properties.pre_transform,
    old_swapchain.m_ImageUsageFlags,
    old_swapchain.m_RequestedCompression,
    old_swapchain.m_RequestedCompressionFixedRate} {
}

// ============================================================================
// 重建构造函数：仅修改 image usage
// ============================================================================

VulkanSwapchain::VulkanSwapchain(VulkanSwapchain &old_swapchain, const std::set<vk::ImageUsageFlagBits> &image_usage_flags) : VulkanSwapchain{
    old_swapchain,
    old_swapchain.m_Device,
    old_swapchain.m_Surface,
    old_swapchain.m_Properties.present_mode,
    old_swapchain.m_PresentModePriorityList,
    old_swapchain.m_SurfaceFormatPriorityList,
    old_swapchain.m_Properties.extent,
    old_swapchain.m_Properties.image_count,
    old_swapchain.m_Properties.pre_transform,
    image_usage_flags,
    old_swapchain.m_RequestedCompression,
    old_swapchain.m_RequestedCompressionFixedRate} {
}

// ============================================================================
// 重建构造函数：修改 extent + transform
// ============================================================================

VulkanSwapchain::VulkanSwapchain(VulkanSwapchain &swapchain,
                                 const vk::Extent2D &extent,
                                 const vk::SurfaceTransformFlagBitsKHR transform) : VulkanSwapchain{swapchain,
    swapchain.m_Device,
    swapchain.m_Surface,
    swapchain.m_Properties.present_mode,
    swapchain.m_PresentModePriorityList,
    swapchain.m_SurfaceFormatPriorityList,
    extent,
    swapchain.m_Properties.image_count,
    transform,
    swapchain.m_ImageUsageFlags,
    swapchain.m_RequestedCompression,
    swapchain.m_RequestedCompressionFixedRate} {
}

// ============================================================================
// 重建构造函数：修改压缩设置
// ============================================================================

VulkanSwapchain::VulkanSwapchain(VulkanSwapchain &swapchain,
                                 vk::ImageCompressionFlagsEXT requested_compression,
                                 vk::ImageCompressionFixedRateFlagsEXT requested_compression_fixed_rate) : VulkanSwapchain{swapchain,
    swapchain.m_Device,
    swapchain.m_Surface,
    swapchain.m_Properties.present_mode,
    swapchain.m_PresentModePriorityList,
    swapchain.m_SurfaceFormatPriorityList,
    swapchain.m_Properties.extent,
    swapchain.m_Properties.image_count,
    swapchain.m_Properties.pre_transform,
    swapchain.m_ImageUsageFlags,
    requested_compression,
    requested_compression_fixed_rate} {
}

// ============================================================================
// 主构造函数（公开入口）→ 委托到完整构造函数
// ============================================================================

VulkanSwapchain::VulkanSwapchain(VulkanDevice &device,
                                 vk::SurfaceKHR surface,
                                 vk::PresentModeKHR present_mode,
                                 const std::vector<vk::PresentModeKHR> &present_mode_priority_list,
                                 const std::vector<vk::SurfaceFormatKHR> &surface_format_priority_list,
                                 const vk::Extent2D &extent,
                                 uint32_t image_count,
                                 vk::SurfaceTransformFlagBitsKHR transform,
                                 const std::set<vk::ImageUsageFlagBits> &image_usage_flags,
                                 vk::ImageCompressionFlagsEXT requested_compression,
                                 vk::ImageCompressionFixedRateFlagsEXT requested_compression_fixed_rate) : VulkanSwapchain{*this,
    device,
    surface,
    present_mode,
    present_mode_priority_list,
    surface_format_priority_list,
    extent,
    image_count,
    transform,
    image_usage_flags,
    requested_compression,
    requested_compression_fixed_rate} {
}

// ============================================================================
// 完整构造函数（所有构造函数最终委托至此）
// ============================================================================

VulkanSwapchain::VulkanSwapchain(VulkanSwapchain &old_swapchain,
                                 VulkanDevice &device,
                                 vk::SurfaceKHR surface,
                                 vk::PresentModeKHR present_mode,
                                 std::vector<vk::PresentModeKHR> const &present_mode_priority_list,
                                 const std::vector<vk::SurfaceFormatKHR> &surface_format_priority_list,
                                 const vk::Extent2D &extent,
                                 uint32_t image_count,
                                 vk::SurfaceTransformFlagBitsKHR transform,
                                 const std::set<vk::ImageUsageFlagBits> &image_usage_flags,
                                 vk::ImageCompressionFlagsEXT requested_compression,
                                 vk::ImageCompressionFixedRateFlagsEXT requested_compression_fixed_rate) : m_Device{device},
    m_Surface{surface},
    m_RequestedCompression{requested_compression},
    m_RequestedCompressionFixedRate{requested_compression_fixed_rate} {
    // 存储优先级列表
    this->m_PresentModePriorityList = present_mode_priority_list;
    this->m_SurfaceFormatPriorityList = surface_format_priority_list;

    auto &vkDevice = m_Device.GetHandle();
    auto gpu = m_Device.GetGpu().GetHandle();

    // 日志：surface 支持的格式
    std::vector<vk::SurfaceFormatKHR> surface_formats = gpu.getSurfaceFormatsKHR(m_Surface);
    GE_CORE_INFO("Surface supports the following surface formats:");
    for (auto &sf : surface_formats) {
        GE_CORE_INFO("  \t{}", vk::to_string(sf.format) + ", " + vk::to_string(sf.colorSpace));
    }

    // 日志：surface 支持的呈现模式
    std::vector<vk::PresentModeKHR> present_modes = gpu.getSurfacePresentModesKHR(m_Surface);
    GE_CORE_INFO("Surface supports the following present modes:");
    for (auto &pm : present_modes) {
        GE_CORE_INFO("  \t{}", vk::to_string(pm));
    }

    // 基于 surface capabilities 选择最佳属性
    vk::SurfaceCapabilitiesKHR const caps = gpu.getSurfaceCapabilitiesKHR(m_Surface);

    m_Properties.old_swapchain = old_swapchain.m_Handle;
    m_Properties.image_count = choose_image_count(image_count, caps.minImageCount, caps.maxImageCount);
    m_Properties.extent = choose_extent(extent, caps.minImageExtent, caps.maxImageExtent, caps.currentExtent);
    m_Properties.surface_format = choose_surface_format(m_Properties.surface_format, surface_formats, surface_format_priority_list);
    m_Properties.array_layers = choose_image_array_layers(1U, caps.maxImageArrayLayers);

    vk::FormatProperties const format_props = gpu.getFormatProperties(m_Properties.surface_format.format);
    this->m_ImageUsageFlags = choose_image_usage(image_usage_flags, caps.supportedUsageFlags, format_props.optimalTilingFeatures);

    m_Properties.image_usage = composite_image_flags(this->m_ImageUsageFlags);
    m_Properties.pre_transform = choose_transform(transform, caps.supportedTransforms, caps.currentTransform);
    m_Properties.composite_alpha = choose_composite_alpha(vk::CompositeAlphaFlagBitsKHR::eInherit, caps.supportedCompositeAlpha);
    m_Properties.present_mode = choose_present_mode(present_mode, present_modes, present_mode_priority_list);

    // 创建 Vulkan swapchain
    vk::SwapchainCreateInfoKHR create_info{
        .surface = m_Surface,
        .minImageCount = m_Properties.image_count,
        .imageFormat = m_Properties.surface_format.format,
        .imageColorSpace = m_Properties.surface_format.colorSpace,
        .imageExtent = m_Properties.extent,
        .imageArrayLayers = m_Properties.array_layers,
        .imageUsage = m_Properties.image_usage,
        .preTransform = m_Properties.pre_transform,
        .compositeAlpha = m_Properties.composite_alpha,
        .presentMode = m_Properties.present_mode,
        .oldSwapchain = m_Properties.old_swapchain,
    };

    // 压缩控制
    auto fixed_rate_flags = requested_compression_fixed_rate;
    vk::ImageCompressionControlEXT compression_control;
    compression_control.flags = requested_compression;
    if (m_Device.IsExtensionEnabled(VK_EXT_IMAGE_COMPRESSION_CONTROL_SWAPCHAIN_EXTENSION_NAME)) {
        create_info.pNext = &compression_control;

        if (vk::ImageCompressionFlagBitsEXT::eFixedRateExplicit == requested_compression) {
            // 不支持多平面格式的压缩
            compression_control.compressionControlPlaneCount = 1;
            compression_control.pFixedRateFlags = &fixed_rate_flags;
        } else if (vk::ImageCompressionFlagBitsEXT::eDisabled == requested_compression) {
            GE_CORE_WARN("(VulkanSwapchain) 禁用默认（无损）压缩，可能对性能产生负面影响");
        }
    } else {
        if (vk::ImageCompressionFlagBitsEXT::eDefault != requested_compression) {
            GE_CORE_WARN("(VulkanSwapchain) 无法控制压缩，因为 VK_EXT_image_compression_control_swapchain 未启用");

            this->m_RequestedCompression = vk::ImageCompressionFlagBitsEXT::eDefault;
            this->m_RequestedCompressionFixedRate = vk::ImageCompressionFixedRateFlagBitsEXT::eNone;
        }
    }

    m_Handle = vkDevice.createSwapchainKHR(create_info);

    // 获取 swapchain images 并包装为 VulkanImage
    auto rawImages = vkDevice.getSwapchainImagesKHR(m_Handle);
    m_Images.reserve(rawImages.size());
    for (auto &img : rawImages) {
        m_Images.emplace_back(m_Device, img, vk::Extent3D{m_Properties.extent.width, m_Properties.extent.height, 1},
                              m_Properties.surface_format.format, m_Properties.image_usage);
    }

    // 验证固定速率压缩是否被应用
    if (m_Device.IsExtensionEnabled(VK_EXT_IMAGE_COMPRESSION_CONTROL_SWAPCHAIN_EXTENSION_NAME) &&
        vk::ImageCompressionFlagBitsEXT::eFixedRateDefault == requested_compression) {
        const auto applied_compression_fixed_rate = query_applied_compression(vkDevice, m_Images[0].GetHandle()).imageCompressionFixedRateFlags;

        if (applied_compression_fixed_rate != requested_compression_fixed_rate) {
            GE_CORE_WARN("(VulkanSwapchain) 请求的固定速率压缩 ({}) 未被应用，image 实际使用 {}",
                         vk::to_string(requested_compression_fixed_rate),
                         vk::to_string(applied_compression_fixed_rate));

            this->m_RequestedCompressionFixedRate = applied_compression_fixed_rate;

            if (vk::ImageCompressionFixedRateFlagBitsEXT::eNone == applied_compression_fixed_rate) {
                this->m_RequestedCompression = vk::ImageCompressionFlagBitsEXT::eDefault;
            }
        } else {
            GE_CORE_INFO("(VulkanSwapchain) 已应用固定速率压缩: {}", vk::to_string(applied_compression_fixed_rate));
        }
    }
}

// ============================================================================
// 析构函数
// ============================================================================

VulkanSwapchain::~VulkanSwapchain() {
    if (m_Handle) {
        m_Device.GetHandle().destroySwapchainKHR(m_Handle);
    }
}

// ============================================================================
// 移动构造函数
// ============================================================================

VulkanSwapchain::VulkanSwapchain(VulkanSwapchain &&other) noexcept : m_Device{other.m_Device},
                                                                     m_Surface{std::exchange(other.m_Surface, nullptr)},
                                                                     m_Handle{std::exchange(other.m_Handle, nullptr)},
                                                                     m_Images{std::exchange(other.m_Images, {})},
                                                                     m_Properties{std::exchange(other.m_Properties, {})},
                                                                     m_PresentModePriorityList{std::exchange(other.m_PresentModePriorityList, {})},
                                                                     m_SurfaceFormatPriorityList
                                                                     {std::exchange(other.m_SurfaceFormatPriorityList, {})},
                                                                     m_ImageUsageFlags{std::move(other.m_ImageUsageFlags)},
                                                                     m_RequestedCompression{
                                                                         std::exchange(other.m_RequestedCompression,
                                                                                       vk::ImageCompressionFlagBitsEXT::eDefault)},
                                                                     m_RequestedCompressionFixedRate{
                                                                         std::exchange(other.m_RequestedCompressionFixedRate,
                                                                                       vk::ImageCompressionFixedRateFlagBitsEXT::eNone)} {
}

// ============================================================================
// IsValid
// ============================================================================

bool VulkanSwapchain::IsValid() const {
    return !!m_Handle;
}

// ============================================================================
// GetDevice
// ============================================================================

VulkanDevice const &VulkanSwapchain::GetDevice() const {
    return m_Device;
}

// ============================================================================
// GetHandle
// ============================================================================

vk::SwapchainKHR VulkanSwapchain::GetHandle() const {
    return m_Handle;
}

// ============================================================================
// AcquireNextImage
// ============================================================================

std::pair<vk::Result, uint32_t> VulkanSwapchain::AcquireNextImage(vk::Semaphore image_acquired_semaphore, vk::Fence fence) const {
    vk::ResultValue<uint32_t> rv = m_Device.GetHandle().acquireNextImageKHR(
        m_Handle, std::numeric_limits<uint64_t>::max(), image_acquired_semaphore, fence);
    return {rv.result, rv.value};
}

// ============================================================================
// 属性访问器
// ============================================================================

const vk::Extent2D &VulkanSwapchain::GetExtent() const {
    return m_Properties.extent;
}

vk::Format VulkanSwapchain::GetFormat() const {
    return m_Properties.surface_format.format;
}

const std::vector<VulkanImage> &VulkanSwapchain::GetImages() const {
    return m_Images;
}

vk::SurfaceTransformFlagBitsKHR VulkanSwapchain::GetTransform() const {
    return m_Properties.pre_transform;
}

vk::SurfaceKHR VulkanSwapchain::GetSurface() const {
    return m_Surface;
}

vk::ImageUsageFlags VulkanSwapchain::GetUsage() const {
    return m_Properties.image_usage;
}

vk::PresentModeKHR VulkanSwapchain::GetPresentMode() const {
    return m_Properties.present_mode;
}

} // namespace GE