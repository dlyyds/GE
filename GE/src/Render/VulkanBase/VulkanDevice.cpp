//
// Created by Lenovo on 2026/6/3.
//

#define VULKAN_HPP_DISPATCH_LOADER_DYNAMIC 1
#include "Render/VulkanBase/VulkanDevice.h"
#include "Render/VulkanBase/VulkanInstance.h"
#include "Core/Log.h"

#include <cstring>

namespace GE {

void VulkanDevice::Destroy() {
    if (m_VmaAllocator) {
        vmaDestroyAllocator(m_VmaAllocator);
        m_VmaAllocator = nullptr;
    }

    m_Queue = nullptr;
    if (m_Device)
        m_Device.destroy();
    m_Device = nullptr;
    m_Gpu = nullptr;
    m_GraphicsQueueIndex = -1;

    m_Instance = nullptr;
}

VulkanDevice::~VulkanDevice() {
    Destroy();
}

void VulkanDevice::Init(VulkanInstance &instance, vk::SurfaceKHR surface) {
    m_Instance = &instance;

    SelectPhysicalDevice(surface);
    InitDevice();
}

void VulkanDevice::SelectPhysicalDevice(vk::SurfaceKHR surface) {
    auto gpus = m_Instance->GetHandle().enumeratePhysicalDevices();

    for (auto &gpu : gpus) {
        vk::PhysicalDeviceProperties device_properties = gpu.getProperties();
        if (device_properties.apiVersion < vk::ApiVersion13) {
            GE_CORE_WARN("Physical device '{}' does not support Vulkan 1.3, skipping.", device_properties.deviceName.data());
            continue;
        }

        std::vector<vk::QueueFamilyProperties> queue_family_properties = gpu.getQueueFamilyProperties();

        uint32_t index = 0;
        auto qfpIt = std::ranges::find_if(queue_family_properties,
                                          [&gpu, surface, &index](vk::QueueFamilyProperties const &qfp) {
                                              return (qfp.queueFlags & vk::QueueFlagBits::eGraphics) &&
                                                     gpu.getSurfaceSupportKHR(index++, surface);
                                          });
        if (qfpIt != queue_family_properties.end()) {
            m_GraphicsQueueIndex = static_cast<int32_t>(std::distance(queue_family_properties.begin(), qfpIt));
            m_Gpu = gpu;
            GE_CORE_INFO("Selected GPU: {}", device_properties.deviceName.data());
            break;
        }
    }

    if (m_GraphicsQueueIndex < 0) {
        throw std::runtime_error("Failed to find a suitable GPU with Vulkan 1.3 support.");
    }
}

void VulkanDevice::InitDevice() {
    GE_CORE_INFO("Initializing Vulkan device.");

    std::vector<vk::ExtensionProperties> device_extensions = m_Gpu.enumerateDeviceExtensionProperties();

    std::vector<const char *> required_device_extensions{VK_KHR_SWAPCHAIN_EXTENSION_NAME,
                                                         VK_EXT_EXTENDED_DYNAMIC_STATE_EXTENSION_NAME};

    if (!ValidateExtensions(required_device_extensions, device_extensions)) {
        throw std::runtime_error("Required device extensions are missing");
    }

#if (defined(VK_ENABLE_PORTABILITY))
    if (std::ranges::any_of(device_extensions,
                            [](vk::ExtensionProperties const &extension) {
                                return strcmp(extension.extensionName, VK_KHR_PORTABILITY_SUBSET_EXTENSION_NAME) == 0;
                            })) {
        required_device_extensions.push_back(VK_KHR_PORTABILITY_SUBSET_EXTENSION_NAME);
    }
#endif

    auto supported_features_chain =
        m_Gpu.getFeatures2<vk::PhysicalDeviceFeatures2, vk::PhysicalDeviceVulkan13Features,
                           vk::PhysicalDeviceExtendedDynamicStateFeaturesEXT>();

    if (!supported_features_chain.get<vk::PhysicalDeviceVulkan13Features>().dynamicRendering) {
        throw std::runtime_error("Dynamic Rendering feature is missing");
    }
    if (!supported_features_chain.get<vk::PhysicalDeviceVulkan13Features>().synchronization2) {
        throw std::runtime_error("Synchronization2 feature is missing");
    }
    if (!supported_features_chain.get<vk::PhysicalDeviceExtendedDynamicStateFeaturesEXT>().extendedDynamicState) {
        throw std::runtime_error("Extended Dynamic State feature is missing");
    }

    vk::PhysicalDeviceVulkan13Features vulkan13_features;
    vulkan13_features.synchronization2 = true;
    vulkan13_features.dynamicRendering = true;
    vk::PhysicalDeviceExtendedDynamicStateFeaturesEXT ext_features;
    ext_features.extendedDynamicState = true;
    vk::StructureChain<vk::PhysicalDeviceFeatures2, vk::PhysicalDeviceVulkan13Features,
                       vk::PhysicalDeviceExtendedDynamicStateFeaturesEXT>
        enabled_features_chain = {vk::PhysicalDeviceFeatures2{}, vulkan13_features, ext_features};

    float queue_priority = 0.5f;

    vk::DeviceQueueCreateInfo queue_info;
    queue_info.queueFamilyIndex = static_cast<uint32_t>(m_GraphicsQueueIndex);
    queue_info.queueCount = 1;
    queue_info.pQueuePriorities = &queue_priority;

    vk::DeviceCreateInfo device_info;
    device_info.pNext = &enabled_features_chain.get<vk::PhysicalDeviceFeatures2>();
    device_info.queueCreateInfoCount = 1;
    device_info.pQueueCreateInfos = &queue_info;
    device_info.enabledExtensionCount = static_cast<uint32_t>(required_device_extensions.size());
    device_info.ppEnabledExtensionNames = required_device_extensions.data();

    m_Device = m_Gpu.createDevice(device_info);

    VULKAN_HPP_DEFAULT_DISPATCHER.init(m_Device);

    // Init VMA allocator
    {
        VmaVulkanFunctions vk_funcs{};
        vk_funcs.vkGetInstanceProcAddr = VULKAN_HPP_DEFAULT_DISPATCHER.vkGetInstanceProcAddr;
        vk_funcs.vkGetDeviceProcAddr = VULKAN_HPP_DEFAULT_DISPATCHER.vkGetDeviceProcAddr;

        VmaAllocatorCreateInfo alloc_info{};
        alloc_info.vulkanApiVersion = VK_API_VERSION_1_3;
        alloc_info.instance = m_Instance->GetHandle();
        alloc_info.physicalDevice = m_Gpu;
        alloc_info.device = m_Device;
        alloc_info.pVulkanFunctions = &vk_funcs;

        vmaCreateAllocator(&alloc_info, &m_VmaAllocator);
    }

    m_Queue = m_Device.getQueue(static_cast<uint32_t>(m_GraphicsQueueIndex), 0);
}

uint32_t VulkanDevice::FindMemoryType(vk::PhysicalDevice gpu, uint32_t type_filter,
                                      vk::MemoryPropertyFlags properties) {
    vk::PhysicalDeviceMemoryProperties mem_properties = gpu.getMemoryProperties();

    for (uint32_t i = 0; i < mem_properties.memoryTypeCount; i++) {
        if (type_filter & (1 << i)) {
            if ((mem_properties.memoryTypes[i].propertyFlags & properties) == properties) {
                return i;
            }
        }
    }

    throw std::runtime_error("Failed to find suitable memory type.");
}

bool VulkanDevice::ValidateExtensions(const std::vector<const char *> &required,
                                      const std::vector<vk::ExtensionProperties> &available) {
    return std::ranges::all_of(required,
                               [&available](auto const &extension_name) {
                                   bool found = std::ranges::any_of(
                                       available, [&extension_name](auto const &ep) {
                                           return strcmp(ep.extensionName, extension_name) == 0;
                                       });
                                   if (!found) {
                                       GE_CORE_ERROR("Required extension not found: {}", extension_name);
                                   }
                                   return found;
                               });
}

} // namespace GE
