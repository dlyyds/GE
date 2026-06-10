#pragma once


#include <vulkan/vulkan.hpp>

#include "VulkanPerFrame.h"

#include <vector>

namespace GE {

struct SwapchainDimensions {
    uint32_t width = 0;
    uint32_t height = 0;
    vk::Format format = vk::Format::eUndefined;
};

class VulkanSwapchain {
public:
    VulkanSwapchain() = default;

    ~VulkanSwapchain() = default;

    VulkanSwapchain(const VulkanSwapchain &) = delete;

    VulkanSwapchain &operator=(const VulkanSwapchain &) = delete;

    void Init(vk::Device device, vk::PhysicalDevice gpu, vk::SurfaceKHR surface,
              vk::Queue queue, int32_t graphics_queue_index, uint32_t width, uint32_t height);

    void Destroy();

    bool BeginFrame();

    void EndFrame();

    [[nodiscard]] const SwapchainDimensions &GetDimensions() const { return m_Dimensions; }
    [[nodiscard]] uint32_t GetImageCount() const { return static_cast<uint32_t>(m_Images.size()); }
    [[nodiscard]] vk::SwapchainKHR GetSwapchain() const { return m_Swapchain; }
    [[nodiscard]] vk::ImageView GetImageView(uint32_t index) const { return m_ImageViews[index]; }
    [[nodiscard]] vk::Image GetImage(uint32_t index) const { return m_Images[index]; }
    [[nodiscard]] vk::CommandBuffer GetCommandBuffer(uint32_t index) const { return m_PerFrame[index].GetCommandBuffer(); }

    [[nodiscard]] vk::CommandBuffer GetCurrentCmd() const { return m_CurrentCmd; }
    [[nodiscard]] uint32_t GetCurrentImageIndex() const { return m_CurrentImageIndex; }
    [[nodiscard]] vk::Image GetCurrentImage() const { return GetImage(m_CurrentImageIndex); }
    [[nodiscard]] vk::ImageView GetCurrentImageView() const { return GetImageView(m_CurrentImageIndex); }

private:
    void Resize();

    void CreateSwapchain(uint32_t width, uint32_t height, vk::SwapchainKHR old_swapchain);

    void CreateImageViews();

    // Low-level per-frame helpers — called by the public BeginFrame/EndFrame wrappers.
    vk::Result AcquireNextImage(uint32_t *image);
    vk::CommandBuffer BeginFrame(uint32_t imageIndex);
    void EndFrame(uint32_t imageIndex);
    vk::Result Present(uint32_t index);

    vk::SurfaceFormatKHR SelectSurfaceFormat();

    vk::Device m_Device = nullptr;
    vk::PhysicalDevice m_Gpu = nullptr;
    vk::SurfaceKHR m_Surface = nullptr;
    vk::Queue m_Queue = nullptr;
    int32_t m_GraphicsQueueIndex = -1;

    vk::SwapchainKHR m_Swapchain = nullptr;
    SwapchainDimensions m_Dimensions;
    std::vector<vk::ImageView> m_ImageViews;
    std::vector<vk::Image> m_Images;
    std::vector<VulkanPerFrame> m_PerFrame;
    std::vector<vk::Semaphore> m_RecycledSemaphores;

    uint32_t m_CurrentImageIndex = ~0u;
    vk::CommandBuffer m_CurrentCmd = nullptr;

    bool m_NeedsResize = false;
};

} // namespace GE
