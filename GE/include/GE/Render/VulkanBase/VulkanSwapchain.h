#pragma once

#include <set>
#include <vector>
#include <vulkan/vulkan.hpp>

#include "Render/VulkanBase/VulkanDevice.h"
#include "Render/VulkanBase/VulkanImage.h"

namespace GE {

/// Swapchain 属性配置
struct VulkanSwapchainProperties {
    vk::SwapchainKHR              old_swapchain{};
    uint32_t                      image_count{3};
    vk::Extent2D                  extent{};
    vk::SurfaceFormatKHR          surface_format{};
    uint32_t                      array_layers{};
    vk::ImageUsageFlags           image_usage{};
    vk::SurfaceTransformFlagBitsKHR pre_transform{};
    vk::CompositeAlphaFlagBitsKHR composite_alpha{};
    vk::PresentModeKHR            present_mode{};
};

class VulkanSwapchain {
public:
    // -- 重建构造函数：仅修改 extent --
    VulkanSwapchain(VulkanSwapchain &old_swapchain, const vk::Extent2D &extent);

    // -- 重建构造函数：仅修改 image count --
    VulkanSwapchain(VulkanSwapchain &old_swapchain, uint32_t image_count);

    // -- 重建构造函数：仅修改 image usage --
    VulkanSwapchain(VulkanSwapchain &old_swapchain, const std::set<vk::ImageUsageFlagBits> &image_usage_flags);

    // -- 重建构造函数：仅修改 present mode --
    VulkanSwapchain(VulkanSwapchain &old_swapchain, vk::PresentModeKHR present_mode);

    // -- 重建构造函数：修改 extent + transform --
    VulkanSwapchain(VulkanSwapchain &swapchain, const vk::Extent2D &extent, const vk::SurfaceTransformFlagBitsKHR transform);

    // -- 重建构造函数：修改压缩设置 --
    VulkanSwapchain(VulkanSwapchain                  &swapchain,
                    vk::ImageCompressionFlagsEXT      requested_compression,
                    vk::ImageCompressionFixedRateFlagsEXT requested_compression_fixed_rate);

    // -- 主构造函数 --
    VulkanSwapchain(VulkanDevice                            &device,
                    vk::SurfaceKHR                           surface,
                    vk::PresentModeKHR                       present_mode,
                    const std::vector<vk::PresentModeKHR>   &present_mode_priority_list       = {vk::PresentModeKHR::eFifo, vk::PresentModeKHR::eMailbox},
                    const std::vector<vk::SurfaceFormatKHR> &surface_format_priority_list     = {{vk::Format::eR8G8B8A8Srgb, vk::ColorSpaceKHR::eSrgbNonlinear},
                                                                                                  {vk::Format::eB8G8R8A8Srgb, vk::ColorSpaceKHR::eSrgbNonlinear}},
                    const vk::Extent2D                      &extent                           = {},
                    uint32_t                                 image_count                      = 3,
                    vk::SurfaceTransformFlagBitsKHR          transform                        = vk::SurfaceTransformFlagBitsKHR::eIdentity,
                    const std::set<vk::ImageUsageFlagBits>  &image_usage_flags                = {vk::ImageUsageFlagBits::eColorAttachment, vk::ImageUsageFlagBits::eTransferSrc},
                    vk::ImageCompressionFlagsEXT             requested_compression            = vk::ImageCompressionFlagBitsEXT::eDefault,
                    vk::ImageCompressionFixedRateFlagsEXT    requested_compression_fixed_rate = vk::ImageCompressionFixedRateFlagBitsEXT::eNone);

    // -- 从旧 swapchain 重建的完整构造函数 --
    VulkanSwapchain(VulkanSwapchain                         &old_swapchain,
                    VulkanDevice                            &device,
                    vk::SurfaceKHR                           surface,
                    vk::PresentModeKHR                       present_mode,
                    const std::vector<vk::PresentModeKHR>   &present_mode_priority_list       = {vk::PresentModeKHR::eFifo, vk::PresentModeKHR::eMailbox},
                    const std::vector<vk::SurfaceFormatKHR> &surface_format_priority_list     = {{vk::Format::eR8G8B8A8Srgb, vk::ColorSpaceKHR::eSrgbNonlinear},
                                                                                                  {vk::Format::eB8G8R8A8Srgb, vk::ColorSpaceKHR::eSrgbNonlinear}},
                    const vk::Extent2D                      &extent                           = {},
                    uint32_t                                 image_count                      = 3,
                    vk::SurfaceTransformFlagBitsKHR          transform                        = vk::SurfaceTransformFlagBitsKHR::eIdentity,
                    const std::set<vk::ImageUsageFlagBits>  &image_usage_flags                = {vk::ImageUsageFlagBits::eColorAttachment, vk::ImageUsageFlagBits::eTransferSrc},
                    vk::ImageCompressionFlagsEXT             requested_compression            = vk::ImageCompressionFlagBitsEXT::eDefault,
                    vk::ImageCompressionFixedRateFlagsEXT    requested_compression_fixed_rate = vk::ImageCompressionFixedRateFlagBitsEXT::eNone);

    VulkanSwapchain(const VulkanSwapchain &) = delete;

    VulkanSwapchain(VulkanSwapchain &&other) noexcept;

    ~VulkanSwapchain();

    VulkanSwapchain &operator=(const VulkanSwapchain &) = delete;

    VulkanSwapchain &operator=(VulkanSwapchain &&) = delete;

    bool IsValid() const;

    VulkanDevice const &GetDevice() const;
    vk::SwapchainKHR GetHandle() const;

    std::pair<vk::Result, uint32_t> AcquireNextImage(vk::Semaphore image_acquired_semaphore, vk::Fence fence = nullptr) const;

    const vk::Extent2D &GetExtent() const;
    vk::Format GetFormat() const;
    const std::vector<VulkanImage> &GetImages() const;
    std::vector<VulkanImage> &GetImages();
    vk::SurfaceTransformFlagBitsKHR GetTransform() const;
    vk::SurfaceKHR GetSurface() const;
    vk::ImageUsageFlags GetUsage() const;
    vk::PresentModeKHR GetPresentMode() const;

private:
    VulkanDevice       &m_Device;                     ///< 逻辑设备引用
    vk::SurfaceKHR      m_Surface{};                  ///< 呈现 surface
    vk::SwapchainKHR    m_Handle{};                   ///< Vulkan swapchain 句柄
    std::vector<VulkanImage> m_Images;                ///< swapchain images

    VulkanSwapchainProperties m_Properties;           ///< swapchain 属性

    // 呈现模式优先级列表（vector[0] 优先级最高，vector[size-1] 最低）
    std::vector<vk::PresentModeKHR>   m_PresentModePriorityList;
    // surface 格式优先级列表（vector[0] 优先级最高，vector[size-1] 最低）
    std::vector<vk::SurfaceFormatKHR> m_SurfaceFormatPriorityList;

    std::set<vk::ImageUsageFlagBits> m_ImageUsageFlags;  ///< 经过验证的 image 用法标志

    vk::ImageCompressionFlagsEXT          m_RequestedCompression{vk::ImageCompressionFlagBitsEXT::eDefault};
    vk::ImageCompressionFixedRateFlagsEXT m_RequestedCompressionFixedRate{vk::ImageCompressionFixedRateFlagBitsEXT::eNone};
};

} // namespace GE