#pragma once


#include <vulkan/vulkan.hpp>

#include <string>

namespace GE {

class VulkanImage {
public:
    VulkanImage() = default;

    ~VulkanImage() = default;

    VulkanImage(const VulkanImage &) = delete;

    VulkanImage &operator=(const VulkanImage &) = delete;

    void LoadFromFile(vk::Device device, vk::PhysicalDevice gpu,
                      vk::Queue queue, uint32_t queue_family_index,
                      const std::string &filepath);

    void LoadFromMemory(vk::Device device, vk::PhysicalDevice gpu,
                        vk::Queue queue, uint32_t queue_family_index,
                        const void *data, uint32_t width, uint32_t height,
                        vk::Format format = vk::Format::eR8G8B8A8Srgb);

    void Init(vk::Device device, vk::PhysicalDevice gpu,
              uint32_t width, uint32_t height, vk::Format format,
              vk::ImageTiling tiling, vk::ImageUsageFlags usage,
              vk::MemoryPropertyFlags memory_properties);

    /// Create an ImageView for the given image. Returns the created view handle.
    [[nodiscard]] static vk::ImageView CreateView(vk::Device device, vk::Image image,
                                                  vk::ImageViewType type, vk::Format format,
                                                  vk::ImageAspectFlags aspect = vk::ImageAspectFlagBits::eColor);

    /// Transition an image's layout using a predefined lookup table
    static void TransitionLayout(vk::CommandBuffer cmd, vk::Image image,
                                 vk::ImageLayout old_layout, vk::ImageLayout new_layout);

    void Cleanup();

    [[nodiscard]] vk::Image GetImage() const { return m_Image; }
    [[nodiscard]] vk::ImageView GetView() const { return m_View; }
    [[nodiscard]] uint32_t GetWidth() const { return m_Width; }
    [[nodiscard]] uint32_t GetHeight() const { return m_Height; }

private:
    vk::Device m_Device = nullptr;
    vk::Image m_Image = nullptr;
    vk::DeviceMemory m_Memory = nullptr;
    vk::ImageView m_View = nullptr;
    uint32_t m_Width = 0;
    uint32_t m_Height = 0;
};

} // namespace GE
