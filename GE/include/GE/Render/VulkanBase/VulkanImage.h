#pragma once

#include <vulkan/vulkan.hpp>
#include "vk_mem_alloc.h"

#include <string>

namespace GE {

/// GPU image (texture) backed by VMA-managed memory.
class VulkanImage {
public:
    VulkanImage() = default;

    ~VulkanImage() = default;

    VulkanImage(const VulkanImage &) = delete;

    VulkanImage &operator=(const VulkanImage &) = delete;

    /// Load a texture from a file (PNG, JPG, etc.) via stb_image.
    void LoadFromFile(VmaAllocator allocator,
                      vk::Queue queue, uint32_t queue_family_index,
                      const std::string &filepath);

    /// Load a texture from raw pixel data in memory (RGBA8).
    void LoadFromMemory(VmaAllocator allocator,
                        vk::Queue queue, uint32_t queue_family_index,
                        const void *data, uint32_t width, uint32_t height,
                        vk::Format format = vk::Format::eR8G8B8A8Srgb);

    /// Create an uninitialized image (e.g. for use as a render target).
    void Init(VmaAllocator allocator,
              uint32_t width, uint32_t height, vk::Format format,
              vk::ImageTiling tiling, vk::ImageUsageFlags usage,
              VmaMemoryUsage memory_usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE,
              VmaAllocationCreateFlags flags = 0);

    /// 重新创建 image（保留 Init 时的 format/tiling/usage 参数，仅更新尺寸）。
    /// 用于 swapchain resize 时重建深度/颜色附件。
    void Resize(uint32_t newWidth, uint32_t newHeight);

    /// Create an ImageView for the given image. Returns the created view handle.
    [[nodiscard]] static vk::ImageView CreateView(vk::Device device, vk::Image image,
                                                   vk::ImageViewType type, vk::Format format,
                                                   vk::ImageAspectFlags aspect = vk::ImageAspectFlagBits::eColor);

    /// 为当前 image 创建 View（用于 Init 创建不含 view 的 image 后补建）。
    void CreateView(vk::Format format,
                    vk::ImageViewType type = vk::ImageViewType::e2D,
                    vk::ImageAspectFlags aspect = vk::ImageAspectFlagBits::eColor);

    /// Transition an image's layout using a predefined lookup table
    static void TransitionLayout(vk::CommandBuffer cmd, vk::Image image,
                                 vk::ImageLayout old_layout, vk::ImageLayout new_layout,
                                 uint32_t baseMipLevel = 0, uint32_t levelCount = 1);

    void Cleanup();

    [[nodiscard]] vk::Image GetImage() const { return m_Image; }
    [[nodiscard]] vk::ImageView GetView() const { return m_View; }
    [[nodiscard]] uint32_t GetWidth() const { return m_Width; }
    [[nodiscard]] uint32_t GetHeight() const { return m_Height; }
    [[nodiscard]] uint32_t GetMipLevels() const { return m_MipLevels; }

private:
    VmaAllocator m_Allocator = nullptr;
    vk::Device m_Device = nullptr;   // derived from m_Allocator
    VmaAllocation m_Allocation = nullptr;
    vk::Image m_Image = nullptr;
    vk::ImageView m_View = nullptr;
    uint32_t m_Width = 0;
    uint32_t m_Height = 0;
    uint32_t m_MipLevels = 1;

    // 存储 Init 参数，供 Resize 复用
    vk::Format m_Format = vk::Format::eUndefined;
    vk::ImageTiling m_Tiling = vk::ImageTiling::eOptimal;
    vk::ImageUsageFlags m_Usage;
    VmaMemoryUsage m_MemoryUsage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
    VmaAllocationCreateFlags m_AllocFlags = 0;
    vk::ImageViewType m_ViewType = vk::ImageViewType::e2D;
    vk::ImageAspectFlags m_Aspect = vk::ImageAspectFlagBits::eColor;
};

} // namespace GE
