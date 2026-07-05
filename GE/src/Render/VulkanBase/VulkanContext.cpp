//
// Created by Lenovo on 2026/6/9.
//

#include "Render/VulkanBase/VulkanContext.h"
#include "Core/GEWindow.h"
#include "Core/Log.h"

#include <cstring>
#include <stdexcept>

namespace GE {

namespace {

/// Debug 回调，将验证层消息路由到 GE 日志系统。
VKAPI_ATTR vk::Bool32 VKAPI_CALL DebugCallback(vk::DebugUtilsMessageSeverityFlagBitsEXT   message_severity,
                                                vk::DebugUtilsMessageTypeFlagsEXT          message_types,
                                                vk::DebugUtilsMessengerCallbackDataEXT const *callback_data,
                                                void                                        * /*user_data*/)
{
    auto severity = message_severity;
    auto types    = message_types;
    auto cb_data  = callback_data;

    if (severity & vk::DebugUtilsMessageSeverityFlagBitsEXT::eError)
    {
        GE_CORE_ERROR("{} Validation Layer: Error: {}: {}",
                      cb_data->messageIdNumber, cb_data->pMessageIdName, cb_data->pMessage);
    }
    else if (severity & vk::DebugUtilsMessageSeverityFlagBitsEXT::eWarning)
    {
        GE_CORE_WARN("{} Validation Layer: Warning: {}: {}",
                     cb_data->messageIdNumber, cb_data->pMessageIdName, cb_data->pMessage);
    }
    else if (severity & vk::DebugUtilsMessageSeverityFlagBitsEXT::eInfo)
    {
        GE_CORE_INFO("{} Validation Layer: Information: {}: {}",
                     cb_data->messageIdNumber, cb_data->pMessageIdName, cb_data->pMessage);
    }
    else if (types & vk::DebugUtilsMessageTypeFlagBitsEXT::ePerformance)
    {
        GE_CORE_TRACE("{} Validation Layer: Performance warning: {}: {}",
                      cb_data->messageIdNumber, cb_data->pMessageIdName, cb_data->pMessage);
    }
    else if (severity & vk::DebugUtilsMessageSeverityFlagBitsEXT::eVerbose)
    {
        GE_CORE_TRACE("{} Validation Layer: Verbose: {}: {}",
                      cb_data->messageIdNumber, cb_data->pMessageIdName, cb_data->pMessage);
    }
    return false;
}

} // anonymous namespace

VulkanContext::~VulkanContext() {
    if (IsInitialized())
        Destroy();
}

void VulkanContext::Init(Window &window) {
    // 1. 创建 Vulkan Instance
    m_Instance = CreateInstance();

    // 2. 从 Instance + Window 创建 Surface
    VkSurfaceKHR raw_surface = window.CreateVulkanSurface(m_Instance->GetHandle());
    if (!raw_surface) {
        throw std::runtime_error("Failed to create window surface.");
    }
    m_Surface = raw_surface;

    // 3. 选择 PhysicalDevice（找第一个支持 Vulkan 1.3 的 GPU）
    auto gpus = m_Instance->GetHandle().enumeratePhysicalDevices();
    for (auto &gpu : gpus) {
        if (gpu.getProperties().apiVersion >= VK_API_VERSION_1_3) {
            m_PhysicalDevice.emplace(*m_Instance, gpu);
            break;
        }
    }
    if (!m_PhysicalDevice) {
        throw std::runtime_error("Failed to find a suitable GPU with Vulkan 1.3 support.");
    }

    // 4. 初始化 Device（传入已选 GPU 和 surface）
    m_Device.Init(*m_Instance, m_PhysicalDevice->GetHandle(), m_Surface);
}

std::unique_ptr<VulkanInstance> VulkanContext::CreateInstance() {
    // ---- 组装平台必需的扩展 ----
    std::unordered_map<std::string, RequestMode> extensions;
    extensions[VK_KHR_SURFACE_EXTENSION_NAME] = RequestMode::Required;

#if defined(VK_USE_PLATFORM_ANDROID_KHR)
    extensions[VK_KHR_ANDROID_SURFACE_EXTENSION_NAME] = RequestMode::Required;
#elif defined(VK_USE_PLATFORM_WIN32_KHR)
    extensions[VK_KHR_WIN32_SURFACE_EXTENSION_NAME] = RequestMode::Required;
#elif defined(VK_USE_PLATFORM_METAL_EXT)
    extensions[VK_EXT_METAL_SURFACE_EXTENSION_NAME] = RequestMode::Required;
#elif defined(VK_USE_PLATFORM_XCB_KHR)
    extensions[VK_KHR_XCB_SURFACE_EXTENSION_NAME] = RequestMode::Required;
#elif defined(VK_USE_PLATFORM_XLIB_KHR)
    extensions[VK_KHR_XLIB_SURFACE_EXTENSION_NAME] = RequestMode::Required;
#elif defined(VK_USE_PLATFORM_WAYLAND_KHR)
    extensions[VK_KHR_WAYLAND_SURFACE_EXTENSION_NAME] = RequestMode::Required;
#elif defined(VK_USE_PLATFORM_DISPLAY_KHR)
    extensions[VK_KHR_DISPLAY_EXTENSION_NAME] = RequestMode::Required;
#endif

    // ---- 组装 Layers ----
    std::unordered_map<std::string, RequestMode> layers;

#if defined(VK_DEBUG) || defined(VK_VALIDATION_LAYERS)
    extensions[VK_EXT_DEBUG_UTILS_EXTENSION_NAME] = RequestMode::Optional;
    layers["VK_LAYER_KHRONOS_validation"] = RequestMode::Optional;
#endif

    // ---- pNext 扩展回调 ----
    auto extend_cb = [](StructureChainBuilder<vk::InstanceCreateInfo> &scb) {
#if defined(VK_DEBUG) || defined(VK_VALIDATION_LAYERS)
        vk::DebugUtilsMessengerCreateInfoEXT debug_pnext{
            .messageSeverity = vk::DebugUtilsMessageSeverityFlagBitsEXT::eError |
                               vk::DebugUtilsMessageSeverityFlagBitsEXT::eWarning,
            .messageType = vk::DebugUtilsMessageTypeFlagBitsEXT::eGeneral |
                           vk::DebugUtilsMessageTypeFlagBitsEXT::eValidation,
            .pfnUserCallback = DebugCallback,
        };
        scb.add_struct(debug_pnext);
#endif
    };

    auto instance = std::make_unique<VulkanInstance>(
        "GE App",
        VK_API_VERSION_1_3,
        layers,
        extensions,
        VulkanInstance::DefaultGetCreateFlags,
        extend_cb);

    // ---- 注册 Debug 回调（如果 VK_EXT_DEBUG_UTILS 已启用）----
#if defined(VK_DEBUG) || defined(VK_VALIDATION_LAYERS)
    if (instance->IsExtensionEnabled(VK_EXT_DEBUG_UTILS_EXTENSION_NAME))
    {
        vk::DebugUtilsMessengerCreateInfoEXT debug_info{
            .messageSeverity = vk::DebugUtilsMessageSeverityFlagBitsEXT::eError |
                               vk::DebugUtilsMessageSeverityFlagBitsEXT::eWarning,
            .messageType = vk::DebugUtilsMessageTypeFlagBitsEXT::eGeneral |
                           vk::DebugUtilsMessageTypeFlagBitsEXT::eValidation,
            .pfnUserCallback = DebugCallback,
        };
        m_DebugCallback = instance->GetHandle().createDebugUtilsMessengerEXT(debug_info);
        GE_CORE_TRACE("DebugCallback has been registered");
    }
#endif

    return instance;
}

void VulkanContext::Destroy() {
    // 1. 销毁 Device
    m_Device.Destroy();

    // 2. 释放 PhysicalDevice（持有 Instance 引用，须在 Instance 之前销毁）
    m_PhysicalDevice.reset();

    // 3. 销毁 Debug 回调（必须在 Instance 之前销毁）
    if (m_DebugCallback) {
        m_Instance->GetHandle().destroyDebugUtilsMessengerEXT(m_DebugCallback);
        m_DebugCallback = nullptr;
    }

    // 4. 销毁 Surface（必须在 Instance 之前销毁）
    if (m_Surface) {
        m_Instance->GetHandle().destroySurfaceKHR(m_Surface);
        m_Surface = nullptr;
    }

    // 5. 销毁 Instance（unique_ptr 析构触发 VulkanInstance 析构）
    m_Instance.reset();
}

} // namespace GE
