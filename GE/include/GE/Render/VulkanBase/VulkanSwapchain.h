#pragma once

#include <vulkan/vulkan.hpp>

#include "VulkanPerFrame.h"

#include <vector>

namespace GE {

/// Swapchain 属性配置，与 HPPSwapchainProperties 结构一致。
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
    /// 主构造函数：根据属性创建 swapchain。
    VulkanSwapchain(vk::Device device, vk::PhysicalDevice gpu, vk::SurfaceKHR surface,
                    vk::Queue queue, int32_t graphics_queue_index,
                    const std::vector<vk::SurfaceFormatKHR> &surface_format_priority_list,
                    const std::vector<vk::PresentModeKHR>   &present_mode_priority_list,
                    const VulkanSwapchainProperties         &properties = {});

    /// 重建构造函数：基于旧 swapchain 修改 extent。
    VulkanSwapchain(VulkanSwapchain &old_swapchain, const vk::Extent2D &extent);

    /// 重建构造函数：基于旧 swapchain 修改 image count。
    VulkanSwapchain(VulkanSwapchain &old_swapchain, uint32_t image_count);

    ~VulkanSwapchain();

    VulkanSwapchain(const VulkanSwapchain &) = delete;
    VulkanSwapchain &operator=(const VulkanSwapchain &) = delete;
    VulkanSwapchain(VulkanSwapchain &&other) noexcept;
    VulkanSwapchain &operator=(VulkanSwapchain &&) = delete;

    bool BeginFrame();
    void EndFrame();

    [[nodiscard]] const vk::Extent2D             &GetExtent() const { return m_Properties.extent; }
    [[nodiscard]] vk::Format                      GetFormat() const { return m_Properties.surface_format.format; }
    [[nodiscard]] uint32_t                        GetImageCount() const { return static_cast<uint32_t>(m_Images.size()); }
    [[nodiscard]] vk::SwapchainKHR                GetHandle() const { return m_Handle; }
    [[nodiscard]] vk::SurfaceKHR                  GetSurface() const { return m_Surface; }
    [[nodiscard]] vk::Image                       GetImage(uint32_t index) const { return m_Images[index]; }
    [[nodiscard]] vk::ImageView                   GetImageView(uint32_t index) const { return m_ImageViews[index]; }
    [[nodiscard]] vk::CommandBuffer               GetCommandBuffer(uint32_t index) const { return m_PerFrame[index].GetCommandBuffer(); }
    [[nodiscard]] vk::ImageUsageFlags             GetUsage() const { return m_Properties.image_usage; }
    [[nodiscard]] vk::PresentModeKHR              GetPresentMode() const { return m_Properties.present_mode; }
    [[nodiscard]] vk::SurfaceTransformFlagBitsKHR GetTransform() const { return m_Properties.pre_transform; }

    // -- 帧状态访问 --
    [[nodiscard]] vk::CommandBuffer GetCurrentCmd() const { return m_CurrentCmd; }
    [[nodiscard]] uint32_t          GetCurrentImageIndex() const { return m_CurrentImageIndex; }
    [[nodiscard]] vk::Image         GetCurrentImage() const { return GetImage(m_CurrentImageIndex); }
    [[nodiscard]] vk::ImageView     GetCurrentImageView() const { return GetImageView(m_CurrentImageIndex); }

private:
    /// 内部创建 swapchain（被构造函数和 Resize 调用）。
    void Create(const VulkanSwapchainProperties &props);

    /// 创建 image views 并初始化 per-frame 数据。
    void CreateImageViews();

    /// 检查 surface 尺寸变化并重建 swapchain。
    void Resize();

    /// 从优先级列表中选第一个支持的表面格式。
    vk::SurfaceFormatKHR SelectSurfaceFormat(const std::vector<vk::SurfaceFormatKHR> &priority_list);

    /// 从优先级列表中选第一个支持的呈现模式。
    vk::PresentModeKHR SelectPresentMode(const std::vector<vk::PresentModeKHR> &priority_list);

    // -- 低级帧管理（被 BeginFrame/EndFrame 调用）--
    vk::Result AcquireNextImage(uint32_t *image);
    vk::CommandBuffer BeginFrame(uint32_t imageIndex);
    void EndFrame(uint32_t imageIndex);
    vk::Result Present(uint32_t index);

    // -- 核心设备句柄 --
    vk::Device         m_Device = nullptr;
    vk::PhysicalDevice m_Gpu    = nullptr;
    vk::SurfaceKHR     m_Surface = nullptr;
    vk::Queue          m_Queue  = nullptr;
    int32_t            m_GraphicsQueueIndex = -1;

    // -- Swapchain 资源 --
    vk::SwapchainKHR              m_Handle = nullptr;
    VulkanSwapchainProperties     m_Properties;
    std::vector<vk::Image>        m_Images;
    std::vector<vk::ImageView>    m_ImageViews;
    std::vector<VulkanPerFrame>   m_PerFrame;
    std::vector<vk::Semaphore>    m_RecycledSemaphores;

    // -- 帧状态 --
    uint32_t          m_CurrentImageIndex = ~0u;
    vk::CommandBuffer m_CurrentCmd       = nullptr;
    bool              m_NeedsResize      = false;
};

} // namespace GE
