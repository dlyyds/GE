#include "Render/VulkanBase/VulkanContext.h"
#include "Render/VulkanBase/VulkanDebug.h"
#include "Core/GEWindow.h"
#include "Core/Log.h"

#include "Debug/Profiler.h"

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
    // Extended Dynamic State 在 Vulkan 1.3 **已提升为核心**，因此不能在扩展名上硬要求：
    // 提升之后这个扩展名可以合法地不再出现在设备的扩展列表里（Android 模拟器与部分
    // 1.3 驱动就是这样），而引擎用到的全是已提升为核心的动态状态（eCullMode /
    // eFrontFace / ePrimitiveTopology / eDepth*）。把扩展名当硬门槛会让这些设备直接
    // 启动失败，报错还指向一个"看起来必备"的扩展，很误导。
    // 真正的判定放到 VulkanDevice —— 那里才拿得到物理设备的 apiVersion，
    // 规则是「扩展名与 1.3 核心二者取其一」。
    m_DeviceExtensions.try_emplace(VK_EXT_EXTENDED_DYNAMIC_STATE_EXTENSION_NAME, RequestMode::Optional);
    // 描述符索引（CSM 逐片元选片：Lighting 按片元 viewZ 非均匀索引 samplerShadowDepth 数组，
    // 需要 shaderSampledImageArrayNonUniformIndexing 特性，见 CreateDevice 特性回调）。
    // 与上面的 Extended Dynamic State 同理：它在 Vulkan 1.2 **已提升为核心**，扩展名
    // 合法地可能不出现，故不能按扩展名硬要求；判定在 VulkanDevice，特性在下方回调里
    // 按"扩展名 or 1.2 核心"二选一启用。
    m_DeviceExtensions.try_emplace(VK_EXT_DESCRIPTOR_INDEXING_EXTENSION_NAME, RequestMode::Optional);
}

VulkanContext::VulkanContext(Window &window) {
    GE_PROFILE_SCOPE("VulkanContextInit");
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

    // 3. 选择 PhysicalDevice
    m_PhysicalDevice = SelectPhysicalDevice();

    // 4. 创建 Device
    m_Device = CreateDevice();
}

// ============================================================================
// CreateInstance
// ============================================================================

std::unique_ptr<VulkanInstance> VulkanContext::CreateInstance() {
    GE_PROFILE_FUNCTION();
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
// SelectPhysicalDevice
// ============================================================================

std::unique_ptr<PhysicalDevice> VulkanContext::SelectPhysicalDevice() {
    GE_PROFILE_FUNCTION();
    auto gpus = m_Instance->GetHandle().enumeratePhysicalDevices();
    for (auto &gpu : gpus) {
        if (gpu.getProperties().apiVersion >= VK_API_VERSION_1_3) {
            return std::make_unique<PhysicalDevice>(*m_Instance, gpu);
        }
    }
    throw std::runtime_error("Failed to find a suitable GPU with Vulkan 1.3 support.");
}

// ============================================================================
// CreateDevice
// ============================================================================

std::unique_ptr<VulkanDevice> VulkanContext::CreateDevice() {
    GE_PROFILE_FUNCTION();
    // 组装 DebugUtils（真实实现 / 空实现）。
    std::unique_ptr<DebugUtils> debug_utils;
    // **除了扩展可用，还要求验证层真的启用了**。理由：debug utils 的用途是①消费
    // 验证层的消息、②给对象起个名字。没有验证层时①是空的，而②在部分驱动上会崩 ——
    // 实测 Android 模拟器（gfxstream / vulkan.ranchu.so）在
    // vkSetDebugUtilsObjectNameEXT 里 SIGSEGV（vk_common_* 空指针解引用，fault addr
    // 0x40），而 Android 上**根本没有验证层**（`VK_LAYER_KHRONOS_validation` 不可用）。
    // 对象命名纯属调试便利，不该让它把进程带走；桌面有验证层，行为不变。
    const bool hasValidationLayer = m_Instance->IsLayerEnabled("VK_LAYER_KHRONOS_validation");
    if (m_Instance->IsExtensionEnabled(VK_EXT_DEBUG_UTILS_EXTENSION_NAME) && hasValidationLayer) {
        debug_utils = std::make_unique<DebugUtilsExt>();
    } else {
        debug_utils = std::make_unique<DummyDebugUtils>();
    }

    return std::make_unique<VulkanDevice>(*m_PhysicalDevice, m_Surface, std::move(debug_utils), m_DeviceExtensions,
        [](PhysicalDevice &gpu) {
            // 启用 Dynamic Rendering & Synchronization2（Vulkan 1.3 核心特性）
            auto &vulkan13 = gpu.AddExtensionFeatures<vk::PhysicalDeviceVulkan13Features>();
            vulkan13.synchronization2 = true;
            vulkan13.dynamicRendering = true;

            // Extended Dynamic State **不需要启用任何特性结构**：它被提升进 1.3 时
            // 没有留下核心特性位（VkPhysicalDeviceVulkan13Features 里就没有
            // extendedDynamicState 这个成员），1.3 设备上那组动态状态默认可用 ——
            // 这正是 VulkanPipelineState::flushDynamicStates 能直接调核心入口点的前提。
            // 此前这里会在扩展名可用时填 VkPhysicalDeviceExtendedDynamicStateFeaturesEXT，
            // 那是"走扩展路径"的遗留：该路径已随那次崩溃（EXT 入口点在 1.3 设备上为 null）
            // 一并取消，见 flushDynamicStates 的注释。

            // 启用采样器数组非均匀索引（CSM C3）：Lighting 逐片元选片后按动态索引采样
            // samplerShadowDepth[cascade]，片元间索引不一致（非均匀），需此特性。现代
            // 桌面 GPU 均支持；不支持则无法运行 CSM（级联数=1 同样走动态索引路径）。
            //
            // VK_EXT_descriptor_indexing 在 1.2 已提升为核心，扩展名可能不出现 —— 那时
            // 必须走核心 1.2 特性结构。两者只能填一个：扩展未启用时把 EXT 结构挂进
            // pNext 是非法用法。核心路径下 descriptorIndexing 是总开关，子特性依赖它。
            if (gpu.IsExtensionSupported(VK_EXT_DESCRIPTOR_INDEXING_EXTENSION_NAME)) {
                REQUEST_REQUIRED_FEATURE(gpu, vk::PhysicalDeviceDescriptorIndexingFeatures,
                                         shaderSampledImageArrayNonUniformIndexing);
            } else {
                gpu.AddExtensionFeatures<vk::PhysicalDeviceVulkan12Features>().descriptorIndexing = true;
                REQUEST_REQUIRED_FEATURE(gpu, vk::PhysicalDeviceVulkan12Features,
                                         shaderSampledImageArrayNonUniformIndexing);
            }

            // 启用各向异性过滤（所有现代 GPU 均支持，用于提升曲面纹理质量）
            if (gpu.GetFeatures().samplerAnisotropy) {
                gpu.GetMutableRequestedFeatures().samplerAnisotropy = VK_TRUE;
            }
        });
}

// ============================================================================
// Destroy
// ============================================================================

void VulkanContext::Destroy() {
    GE_PROFILE_FUNCTION();
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
