#pragma once

#include <vulkan/vulkan.hpp>

#include <string>
#include <vector>

namespace GE {

/// Vulkan instance，构造即初始化，析构即销毁。
class VulkanInstance {
public:
    explicit VulkanInstance(const std::string &app_name = "GE App",
                            uint32_t api_version = VK_MAKE_VERSION(1, 3, 0));

    ~VulkanInstance();

    VulkanInstance(const VulkanInstance &) = delete;
    VulkanInstance &operator=(const VulkanInstance &) = delete;

    VulkanInstance(VulkanInstance &&) = delete;
    VulkanInstance &operator=(VulkanInstance &&) = delete;

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
