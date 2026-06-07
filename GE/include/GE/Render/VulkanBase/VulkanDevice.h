#pragma once

#include <vulkan/vulkan.hpp>

#include <string>
#include <vector>

namespace GE {

class VulkanInstance;
class Window;

/// Non-static Vulkan logical device. Two-phase init: default construct, then Init().
/// Owns a surface created from Window.
class VulkanDevice {
public:
    VulkanDevice() = default;

    /// Creates surface from window, selects GPU, creates device.
    void Init(VulkanInstance &instance, Window &window);

    /// Destroy device and surface. Safe to call even if not initialized.
    void Destroy();

    ~VulkanDevice();

    VulkanDevice(const VulkanDevice &) = delete;
    VulkanDevice &operator=(const VulkanDevice &) = delete;

    VulkanDevice(VulkanDevice &&) = default;
    VulkanDevice &operator=(VulkanDevice &&) = default;

    [[nodiscard]] bool IsInitialized() const { return m_Device != nullptr; }

    [[nodiscard]] vk::PhysicalDevice GetGpu() const { return m_Gpu; }
    [[nodiscard]] vk::Device GetDevice() const { return m_Device; }
    [[nodiscard]] vk::Queue GetQueue() const { return m_Queue; }
    [[nodiscard]] int32_t GetGraphicsQueueIndex() const { return m_GraphicsQueueIndex; }
    [[nodiscard]] VkSurfaceKHR GetSurface() const { return m_Surface; }

    /// Utility: find a memory type matching type_filter with the given properties.
    static uint32_t FindMemoryType(vk::PhysicalDevice gpu, uint32_t type_filter,
                                   vk::MemoryPropertyFlags properties);

private:
    void SelectPhysicalDevice(VkSurfaceKHR surface);
    void InitDevice();
    bool ValidateExtensions(const std::vector<const char *> &required,
                            const std::vector<vk::ExtensionProperties> &available);

    vk::Device m_Device = nullptr;
    vk::Queue m_Queue = nullptr;
    vk::PhysicalDevice m_Gpu = nullptr;
    int32_t m_GraphicsQueueIndex = -1;
    VkSurfaceKHR m_Surface = VK_NULL_HANDLE;

    // Non-owning pointer — VulkanInstance must outlive this device.
    VulkanInstance *m_Instance = nullptr;
};

} // namespace GE
