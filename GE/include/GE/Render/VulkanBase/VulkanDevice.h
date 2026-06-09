#pragma once

#include <vulkan/vulkan.hpp>
#include "vk_mem_alloc.h"

#include <string>
#include <vector>

namespace GE {

class VulkanInstance;
class Window;

/// Vulkan logical device. Two-phase init: default construct, then Init().
/// Owns the VMA allocator. Does NOT own the surface (managed by VulkanContext).
class VulkanDevice {
public:
    VulkanDevice() = default;

    /// Creates device from the given instance and surface.
    /// @param instance  the Vulkan instance (must outlive this device)
    /// @param surface   the window surface (used for queue family selection)
    void Init(VulkanInstance &instance, vk::SurfaceKHR surface);

    /// Destroy device, VMA allocator. Safe to call even if not initialized.
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

    /// Return the VMA allocator. Valid after Init().
    [[nodiscard]] VmaAllocator GetVmaAllocator() const { return m_VmaAllocator; }

    /// Utility: find a memory type matching type_filter with the given properties.
    static uint32_t FindMemoryType(vk::PhysicalDevice gpu, uint32_t type_filter,
                                   vk::MemoryPropertyFlags properties);

private:
    void SelectPhysicalDevice(vk::SurfaceKHR surface);
    void InitDevice();

    bool ValidateExtensions(const std::vector<const char *> &required,
                            const std::vector<vk::ExtensionProperties> &available);

    vk::Device m_Device = nullptr;
    vk::Queue m_Queue = nullptr;
    vk::PhysicalDevice m_Gpu = nullptr;
    int32_t m_GraphicsQueueIndex = -1;
    VmaAllocator m_VmaAllocator = nullptr;

    // Non-owning pointer — VulkanInstance must outlive this device.
    VulkanInstance *m_Instance = nullptr;
};

} // namespace GE
