//
// Created by Lenovo on 2026/6/3.
//

#define VULKAN_HPP_DISPATCH_LOADER_DYNAMIC 1
#include "Render/VulkanBase/VulkanInstance.h"

#include "Core/Log.h"

#include <cstring>

VULKAN_HPP_DEFAULT_DISPATCH_LOADER_DYNAMIC_STORAGE

namespace GE {

static VKAPI_ATTR vk::Bool32 VKAPI_CALL DebugCallback(vk::DebugUtilsMessageSeverityFlagBitsEXT message_severity,
                                                      vk::DebugUtilsMessageTypeFlagsEXT message_types,
                                                      vk::DebugUtilsMessengerCallbackDataEXT const *callback_data,
                                                      void *user_data) {
    if (message_severity & vk::DebugUtilsMessageSeverityFlagBitsEXT::eError) {
        GE_CORE_ERROR("{} Validation Layer: Error: {}: {}", callback_data->messageIdNumber, callback_data->pMessageIdName,
                      callback_data->pMessage);
    } else if (message_severity & vk::DebugUtilsMessageSeverityFlagBitsEXT::eWarning) {
        GE_CORE_WARN("{} Validation Layer: Warning: {}: {}", callback_data->messageIdNumber, callback_data->pMessageIdName,
                     callback_data->pMessage);
    } else if (message_severity & vk::DebugUtilsMessageSeverityFlagBitsEXT::eInfo) {
        GE_CORE_INFO("{} Validation Layer: Information: {}: {}", callback_data->messageIdNumber, callback_data->pMessageIdName,
                     callback_data->pMessage);
    } else if (message_types & vk::DebugUtilsMessageTypeFlagBitsEXT::ePerformance) {
        GE_CORE_TRACE("{} Validation Layer: Performance warning: {}: {}", callback_data->messageIdNumber, callback_data->pMessageIdName,
                      callback_data->pMessage);
    } else if (message_severity & vk::DebugUtilsMessageSeverityFlagBitsEXT::eVerbose) {
        GE_CORE_TRACE("{} Validation Layer: Verbose: {}: {}", callback_data->messageIdNumber, callback_data->pMessageIdName,
                      callback_data->pMessage);
    }
    return false;
}

VulkanInstance::~VulkanInstance() {
    if (m_DebugCallback && m_Instance) {
        m_Instance.destroyDebugUtilsMessengerEXT(m_DebugCallback);
    }
    m_DebugCallback = nullptr;
    if (m_Instance) {
        m_Instance.destroy();
    }
    m_Instance = nullptr;
}

VulkanInstance::VulkanInstance(const std::string &app_name, uint32_t api_version) {
    GE_CORE_INFO("Initializing Vulkan instance.");

    // Load vkGetInstanceProcAddr from the Vulkan loader
    auto vkGetInstanceProcAddr =
        m_Loader.getProcAddress<PFN_vkGetInstanceProcAddr>("vkGetInstanceProcAddr");
    VULKAN_HPP_DEFAULT_DISPATCHER.init(vkGetInstanceProcAddr);

    std::vector<vk::ExtensionProperties> available_instance_extensions = vk::enumerateInstanceExtensionProperties();

    std::vector<const char *> required_instance_extensions{VK_KHR_SURFACE_EXTENSION_NAME};

#if defined(VKB_DEBUG) || defined(VKB_VALIDATION_LAYERS)
    bool has_debug_utils = std::ranges::any_of(
        available_instance_extensions,
        [](auto const &ep) { return strncmp(ep.extensionName, VK_EXT_DEBUG_UTILS_EXTENSION_NAME, strlen(VK_EXT_DEBUG_UTILS_EXTENSION_NAME)) == 0; });
    if (has_debug_utils) {
        required_instance_extensions.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
    } else {
        GE_CORE_WARN("{} is not available; disabling debug utils messenger", VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
    }
#endif

#if (defined(VKB_ENABLE_PORTABILITY))
    required_instance_extensions.push_back(VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME);
    bool portability_enumeration_available = std::ranges::any_of(
        available_instance_extensions,
        [](VkExtensionProperties const &extension) { return strcmp(extension.extensionName, VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME) == 0; });
    if (portability_enumeration_available) {
        required_instance_extensions.push_back(VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME);
    }
#endif

#if defined(VK_USE_PLATFORM_ANDROID_KHR)
    required_instance_extensions.push_back(VK_KHR_ANDROID_SURFACE_EXTENSION_NAME);
#elif defined(VK_USE_PLATFORM_WIN32_KHR)
    required_instance_extensions.push_back(VK_KHR_WIN32_SURFACE_EXTENSION_NAME);
#elif defined(VK_USE_PLATFORM_METAL_EXT)
    required_instance_extensions.push_back(VK_EXT_METAL_SURFACE_EXTENSION_NAME);
#elif defined(VK_USE_PLATFORM_XCB_KHR)
    required_instance_extensions.push_back(VK_KHR_XCB_SURFACE_EXTENSION_NAME);
#elif defined(VK_USE_PLATFORM_XLIB_KHR)
    required_instance_extensions.push_back(VK_KHR_XLIB_SURFACE_EXTENSION_NAME);
#elif defined(VK_USE_PLATFORM_WAYLAND_KHR)
    required_instance_extensions.push_back(VK_KHR_WAYLAND_SURFACE_EXTENSION_NAME);
#elif defined(VK_USE_PLATFORM_DISPLAY_KHR)
    required_instance_extensions.push_back(VK_KHR_DISPLAY_EXTENSION_NAME);
#else
#	pragma error Platform not supported
#endif

    if (!ValidateExtensions(required_instance_extensions, available_instance_extensions)) {
        throw std::runtime_error("Required instance extensions are missing.");
    }

    std::vector<const char *> requested_instance_layers{};

#if defined(VKB_DEBUG) || defined(VKB_VALIDATION_LAYERS)
    char const *validationLayer = "VK_LAYER_KHRONOS_validation";

    std::vector<vk::LayerProperties> supported_instance_layers = vk::enumerateInstanceLayerProperties();

    if (std::ranges::any_of(supported_instance_layers,
                            [&validationLayer](auto const &lp) { return strcmp(lp.layerName, validationLayer) == 0; })) {
        requested_instance_layers.push_back(validationLayer);
        GE_CORE_TRACE("Enabled Validation Layer {}", validationLayer);
    } else {
        GE_CORE_WARN("Validation Layer {} is not available", validationLayer);
    }
#endif

    vk::ApplicationInfo app{.pApplicationName = app_name.c_str(), .pEngineName = "Game Engine", .apiVersion = api_version};

    vk::InstanceCreateInfo instance_info{.pApplicationInfo = &app,
                                         .enabledLayerCount = static_cast<uint32_t>(requested_instance_layers.size()),
                                         .ppEnabledLayerNames = requested_instance_layers.data(),
                                         .enabledExtensionCount = static_cast<uint32_t>(required_instance_extensions.size()),
                                         .ppEnabledExtensionNames = required_instance_extensions.data()};

#if defined(VKB_DEBUG) || defined(VKB_VALIDATION_LAYERS)
    vk::DebugUtilsMessengerCreateInfoEXT debug_messenger_create_info;
    if (has_debug_utils) {
        debug_messenger_create_info.messageSeverity = vk::DebugUtilsMessageSeverityFlagBitsEXT::eError |
                                                      vk::DebugUtilsMessageSeverityFlagBitsEXT::eWarning;
        debug_messenger_create_info.messageType = vk::DebugUtilsMessageTypeFlagBitsEXT::eGeneral |
                                                  vk::DebugUtilsMessageTypeFlagBitsEXT::eValidation;
        debug_messenger_create_info.pfnUserCallback = DebugCallback;

        instance_info.pNext = &debug_messenger_create_info;
    }
#endif

#if defined(VKB_ENABLE_PORTABILITY)
    if (portability_enumeration_available) {
        instance_info.flags |= vk::InstanceCreateFlagBits::eEnumeratePortabilityKHR;
    }
#endif

    m_Instance = vk::createInstance(instance_info);

    VULKAN_HPP_DEFAULT_DISPATCHER.init(m_Instance);

#if defined(VKB_DEBUG) || defined(VKB_VALIDATION_LAYERS)
    if (has_debug_utils) {
        m_DebugCallback = m_Instance.createDebugUtilsMessengerEXT(debug_messenger_create_info);
        GE_CORE_TRACE("DebugCallback has been registered");
    }
#endif

}

bool VulkanInstance::ValidateExtensions(const std::vector<const char *> &required,
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
