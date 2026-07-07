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

// ============================================================================
// 扩展管理
// ============================================================================

void VulkanContext::AddInstanceExtension(std::string name, RequestMode mode) {
    m_InstanceExtensions[std::move(name)] = mode;
}

void VulkanContext::AddDeviceExtension(std::string name, RequestMode mode) {
    m_DeviceExtensions[std::move(name)] = mode;
}

// ============================================================================
// 填充引擎默认扩展（用户已自定义的不覆盖）
// ============================================================================

void VulkanContext::ApplyDefaultExtensions() {
    // -- Instance 默认扩展 --
    m_InstanceExtensions.try_emplace(VK_KHR_SURFACE_EXTENSION_NAME, RequestMode::Required);

    // PhysicalDevice 的扩展特性查询依赖此扩展（1.1+ 核心化，显式启用无副作用）
    m_InstanceExtensions.try_emplace(VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME, RequestMode::Required);

#if defined(VK_USE_PLATFORM_ANDROID_KHR)
    m_InstanceExtensions.try_emplace(VK_KHR_ANDROID_SURFACE_EXTENSION_NAME, RequestMode::Required);
#elif defined(VK_USE_PLATFORM_WIN32_KHR)
    m_InstanceExtensions.try_emplace(VK_KHR_WIN32_SURFACE_EXTENSION_NAME, RequestMode::Required);
#elif defined(VK_USE_PLATFORM_METAL_EXT)
    m_InstanceExtensions.try_emplace(VK_EXT_METAL_SURFACE_EXTENSION_NAME, RequestMode::Required);
#elif defined(VK_USE_PLATFORM_XCB_KHR)
    m_InstanceExtensions.try_emplace(VK_KHR_XCB_SURFACE_EXTENSION_NAME, RequestMode::Required);
#elif defined(VK_USE_PLATFORM_XLIB_KHR)
    m_InstanceExtensions.try_emplace(VK_KHR_XLIB_SURFACE_EXTENSION_NAME, RequestMode::Required);
#elif defined(VK_USE_PLATFORM_WAYLAND_KHR)
    m_InstanceExtensions.try_emplace(VK_KHR_WAYLAND_SURFACE_EXTENSION_NAME, RequestMode::Required);
#elif defined(VK_USE_PLATFORM_DISPLAY_KHR)
    m_InstanceExtensions.try_emplace(VK_KHR_DISPLAY_EXTENSION_NAME, RequestMode::Required);
#endif

#if defined(VK_DEBUG) || defined(VK_VALIDATION_LAYERS)
    m_InstanceExtensions.try_emplace(VK_EXT_DEBUG_UTILS_EXTENSION_NAME, RequestMode::Optional);
#endif

    // -- Device 默认扩展 --
    m_DeviceExtensions.try_emplace(VK_KHR_SWAPCHAIN_EXTENSION_NAME, RequestMode::Required);
    m_DeviceExtensions.try_emplace(VK_EXT_EXTENDED_DYNAMIC_STATE_EXTENSION_NAME, RequestMode::Required);
}

VulkanContext::VulkanContext(Window &window) {
    // 0. 填充引擎默认扩展（用户已添加的不覆盖）
    ApplyDefaultExtensions();

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
            m_PhysicalDevice = std::make_unique<PhysicalDevice>(*m_Instance, gpu);
            break;
        }
    }
    if (!m_PhysicalDevice) {
        throw std::runtime_error("Failed to find a suitable GPU with Vulkan 1.3 support.");
    }

    // 4. 创建 Device
    m_Device = CreateDevice();
}

// ============================================================================
// CreateInstance
// ============================================================================

std::unique_ptr<VulkanInstance> VulkanContext::CreateInstance() {
    // ---- 组装 Layers ----
    std::unordered_map<std::string, RequestMode> layers;

#if defined(VK_DEBUG) || defined(VK_VALIDATION_LAYERS)
    layers["VK_LAYER_KHRONOS_validation"] = RequestMode::Optional;
#endif

    // ---- pNext 扩展回调（注册 Debug 回调结构体到 Instance 创建链）----
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
        m_InstanceExtensions,               // ← 扩展列表由 ApplyDefaultExtensions + 用户填充
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

// ============================================================================
// CreateDevice
// ============================================================================

std::unique_ptr<VulkanDevice> VulkanContext::CreateDevice() {
    // 组装 DebugUtils（启用 debug utils 扩展时用真实实现，否则用空实现）
    std::unique_ptr<DebugUtils> debug_utils;
    if (m_Instance->IsExtensionEnabled(VK_EXT_DEBUG_UTILS_EXTENSION_NAME)) {
        debug_utils = std::make_unique<DebugUtilsExt>();
    } else {
        debug_utils = std::make_unique<DummyDebugUtils>();
    }

    return std::make_unique<VulkanDevice>(*m_PhysicalDevice, m_Surface, m_DeviceExtensions,
        [](PhysicalDevice &gpu) {
            // 启用 Dynamic Rendering & Synchronization2（Vulkan 1.3 核心特性）
            auto &vulkan13 = gpu.AddExtensionFeatures<vk::PhysicalDeviceVulkan13Features>();
            vulkan13.synchronization2 = true;
            vulkan13.dynamicRendering = true;

            // 启用 Extended Dynamic State
            auto &ext_dyn_state = gpu.AddExtensionFeatures<vk::PhysicalDeviceExtendedDynamicStateFeaturesEXT>();
            ext_dyn_state.extendedDynamicState = true;
        },
        std::move(debug_utils));
}

// ============================================================================
// Destroy
// ============================================================================

void VulkanContext::Destroy() {
    // 1. 销毁 Device（unique_ptr 析构触发 VulkanDevice 析构 → 销毁 VMA + Device）
    m_Device.reset();

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
