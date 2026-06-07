#pragma once

#include <vulkan/vulkan.hpp>

#include <string>
#include <vector>

namespace GE {

/// Non-static Vulkan instance. Two-phase init: default construct, then Init().
class VulkanInstance {
public:
    VulkanInstance() = default;

    /// Create the Vulkan instance. Must be called once before use.
    void Init(const std::string &app_name = "GE App",
              uint32_t api_version = VK_MAKE_VERSION(1, 3, 0));

    /// Destroy the instance. Safe to call even if not initialized.
    void Destroy();

    ~VulkanInstance();

    VulkanInstance(const VulkanInstance &) = delete;
    VulkanInstance &operator=(const VulkanInstance &) = delete;

    VulkanInstance(VulkanInstance &&) = default;
    VulkanInstance &operator=(VulkanInstance &&) = default;

    [[nodiscard]] bool IsInitialized() const { return m_Instance != nullptr; }

    [[nodiscard]] vk::Instance Get() const { return m_Instance; }

    /// Return vkGetInstanceProcAddr from the dynamic loader.
    [[nodiscard]] PFN_vkGetInstanceProcAddr GetVkGetInstanceProcAddr() const {
        return m_Loader.getProcAddress<PFN_vkGetInstanceProcAddr>("vkGetInstanceProcAddr");
    }

private:
    bool ValidateExtensions(const std::vector<const char *> &required,
                            const std::vector<vk::ExtensionProperties> &available);

    vk::detail::DynamicLoader m_Loader;
    vk::Instance m_Instance = nullptr;
    vk::DebugUtilsMessengerEXT m_DebugCallback = nullptr;
};

} // namespace GE
