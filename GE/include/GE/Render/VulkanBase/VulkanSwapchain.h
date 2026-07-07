#pragma once

#include <set>
#include <vector>
#include <vulkan/vulkan.hpp>

namespace GE {

/// Swapchain 属性配置，与 HPPSwapchainProperties 完全一致。
struct VulkanSwapchainProperties {
    vk::SwapchainKHR                old_swapchain       = nullptr;
    uint32_t                        image_count         = 3;
    vk::Extent2D                    extent              = {};
    vk::SurfaceFormatKHR            surface_format      = {};
    uint32_t                        array_layers        = 1;
    vk::ImageUsageFlags             image_usage         = vk::ImageUsageFlagBits::eColorAttachment;
    vk::SurfaceTransformFlagBitsKHR pre_transform       = vk::SurfaceTransformFlagBitsKHR::eIdentity;
    vk::CompositeAlphaFlagBitsKHR   composite_alpha     = vk::CompositeAlphaFlagBitsKHR::eOpaque;
    vk::PresentModeKHR              present_mode        = vk::PresentModeKHR::eFifo;
};

class VulkanSwapchain {
public:
    // -- 重建构造函数：仅修改 extent --
    VulkanSwapchain(VulkanSwapchain &old_swapchain, const vk::Extent2D &extent);

    // -- 重建构造函数：仅修改 image count --
    VulkanSwapchain(VulkanSwapchain &old_swapchain, uint32_t image_count);

    // -- 重建构造函数：仅修改 image usage --
    VulkanSwapchain(VulkanSwapchain &old_swapchain, const std::set<vk::ImageUsageFlagBits> &image_usage_flags);

    // -- 重建构造函数：修改 extent + transform --
    VulkanSwapchain(VulkanSwapchain &swapchain, const vk::Extent2D &extent, const vk::SurfaceTransformFlagBitsKHR transform);

    // -- 主构造函数 --
    VulkanSwapchain(vk::Device                                       device,
                    vk::PhysicalDevice                               gpu,
                    vk::SurfaceKHR                                   surface,
                    const vk::PresentModeKHR                         present_mode,
                    const std::vector<vk::PresentModeKHR>           &present_mode_priority_list       = {vk::PresentModeKHR::eFifo, vk::PresentModeKHR::eMailbox},
                    const std::vector<vk::SurfaceFormatKHR>         &surface_format_priority_list     = {{vk::Format::eR8G8B8A8Srgb, vk::ColorSpaceKHR::eSrgbNonlinear},
                                                                                                           {vk::Format::eB8G8R8A8Srgb, vk::ColorSpaceKHR::eSrgbNonlinear}},
                    const vk::Extent2D                              &extent                           = {},
                    uint32_t                                         image_count                      = 3,
                    const vk::SurfaceTransformFlagBitsKHR            transform                        = vk::SurfaceTransformFlagBitsKHR::eIdentity,
                    const std::set<vk::ImageUsageFlagBits>          &image_usage_flags                = {vk::ImageUsageFlagBits::eColorAttachment, vk::ImageUsageFlagBits::eTransferSrc});

    VulkanSwapchain(const VulkanSwapchain &) = delete;
    VulkanSwapchain(VulkanSwapchain &&other);
    ~VulkanSwapchain();

    VulkanSwapchain &operator=(const VulkanSwapchain &) = delete;
    VulkanSwapchain &operator=(VulkanSwapchain &&) = delete;

    bool IsValid() const { return m_Handle != nullptr; }

    vk::Device                   GetDevice() const { return m_Device; }
    vk::SwapchainKHR             GetHandle() const { return m_Handle; }
    const vk::Extent2D          &GetExtent() const { return m_Properties.extent; }
    vk::Format                   GetFormat() const { return m_Properties.surface_format.format; }
    const std::vector<vk::Image> &GetImages() const { return m_Images; }
    vk::SurfaceTransformFlagBitsKHR GetTransform() const { return m_Properties.pre_transform; }
    vk::SurfaceKHR               GetSurface() const { return m_Surface; }
    vk::ImageUsageFlags          GetUsage() const { return m_Properties.image_usage; }
    vk::PresentModeKHR           GetPresentMode() const { return m_Properties.present_mode; }

    std::pair<vk::Result, uint32_t> AcquireNextImage(vk::Semaphore image_acquired_semaphore, vk::Fence fence = nullptr) const;

private:
    /// 完整构造函数（所有其他构造函数委托至此）。
    VulkanSwapchain(VulkanSwapchain                               &old_swapchain,
                    vk::Device                                     device,
                    vk::PhysicalDevice                             gpu,
                    vk::SurfaceKHR                                 surface,
                    const vk::PresentModeKHR                       present_mode,
                    const std::vector<vk::PresentModeKHR>         &present_mode_priority_list,
                    const std::vector<vk::SurfaceFormatKHR>       &surface_format_priority_list,
                    const vk::Extent2D                            &extent,
                    uint32_t                                       image_count,
                    const vk::SurfaceTransformFlagBitsKHR          transform,
                    const std::set<vk::ImageUsageFlagBits>        &image_usage_flags);

    vk::Device                          m_Device = nullptr;
    vk::PhysicalDevice                  m_Gpu = nullptr;
    vk::SurfaceKHR                      m_Surface = nullptr;
    vk::SwapchainKHR                    m_Handle = nullptr;
    VulkanSwapchainProperties           m_Properties;
    std::vector<vk::Image>              m_Images;
    std::vector<vk::PresentModeKHR>     m_PresentModePriorityList;
    std::vector<vk::SurfaceFormatKHR>   m_SurfaceFormatPriorityList;
    std::set<vk::ImageUsageFlagBits>    m_ImageUsageFlags;
};

} // namespace GE
