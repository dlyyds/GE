/* Copyright (c) 2018-2026, Arm Limited and Contributors
 * Copyright (c) 2022-2026, NVIDIA CORPORATION. All rights reserved.
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Licensed under the Apache License, Version 2.0 the "License";
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#define VULKAN_HPP_DISPATCH_LOADER_DYNAMIC 1
#include "Render/VulkanBase/VulkanInstance.h"

#include "Core/Log.h"

#include <cstring>
#include <stdexcept>

VULKAN_HPP_DEFAULT_DISPATCH_LOADER_DYNAMIC_STORAGE

namespace GE {

// ============================================================================
// 内部辅助函数
// ============================================================================

namespace {

/// 尝试启用一个扩展，如果可用则加入 enabled_extensions。
/// @return true 如果扩展可用。
bool enable_extension(std::string const                           &requested_extension,
                      std::vector<vk::ExtensionProperties> const  &available_extensions,
                      std::vector<std::string>                    &enabled_extensions)
{
    bool is_available = std::ranges::any_of(
        available_extensions, [&requested_extension](auto const &avail) {
            return requested_extension == avail.extensionName;
        });

    if (is_available)
    {
        bool already = std::ranges::any_of(
            enabled_extensions, [&requested_extension](auto const &enabled) {
                return requested_extension == enabled;
            });
        if (!already)
        {
            GE_CORE_INFO("Extension '{}' available, enabling it", requested_extension);
            enabled_extensions.emplace_back(requested_extension);
        }
    }
    else
    {
        GE_CORE_TRACE("Extension '{}' not available", requested_extension);
    }
    return is_available;
}

/// 尝试启用一个 Layer，如果可用则加入 enabled_layers。
/// @return true 如果 Layer 可用。
bool enable_layer(std::string const                        &requested_layer,
                  std::vector<vk::LayerProperties> const   &available_layers,
                  std::vector<std::string>                 &enabled_layers)
{
    bool is_available = std::ranges::any_of(
        available_layers, [&requested_layer](auto const &avail) {
            return requested_layer == avail.layerName;
        });

    if (is_available)
    {
        bool already = std::ranges::any_of(
            enabled_layers, [&requested_layer](auto const &enabled) {
                return requested_layer == enabled;
            });
        if (!already)
        {
            GE_CORE_INFO("Layer '{}' available, enabling it", requested_layer);
            enabled_layers.emplace_back(requested_layer);
        }
    }
    else
    {
        GE_CORE_TRACE("Layer '{}' not available", requested_layer);
    }
    return is_available;
}

/// Debug 回调，将验证层消息路由到 GE 日志系统。
VKAPI_ATTR vk::Bool32 VKAPI_CALL DebugCallback(vk::DebugUtilsMessageSeverityFlagBitsEXT  message_severity,
                                                vk::DebugUtilsMessageTypeFlagsEXT         message_types,
                                                vk::DebugUtilsMessengerCallbackDataEXT const *callback_data,
                                                void                                       * /*user_data*/)
{
    auto severity   = message_severity;
    auto types      = message_types;
    auto cb_data    = callback_data;

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

/// 获取平台相关的 surface 扩展名称。
const char *GetPlatformSurfaceExtension()
{
#if defined(VK_USE_PLATFORM_ANDROID_KHR)
    return VK_KHR_ANDROID_SURFACE_EXTENSION_NAME;
#elif defined(VK_USE_PLATFORM_WIN32_KHR)
    return VK_KHR_WIN32_SURFACE_EXTENSION_NAME;
#elif defined(VK_USE_PLATFORM_METAL_EXT)
    return VK_EXT_METAL_SURFACE_EXTENSION_NAME;
#elif defined(VK_USE_PLATFORM_XCB_KHR)
    return VK_KHR_XCB_SURFACE_EXTENSION_NAME;
#elif defined(VK_USE_PLATFORM_XLIB_KHR)
    return VK_KHR_XLIB_SURFACE_EXTENSION_NAME;
#elif defined(VK_USE_PLATFORM_WAYLAND_KHR)
    return VK_KHR_WAYLAND_SURFACE_EXTENSION_NAME;
#elif defined(VK_USE_PLATFORM_DISPLAY_KHR)
    return VK_KHR_DISPLAY_EXTENSION_NAME;
#else
    return nullptr;
#endif
}

} // anonymous namespace

// ============================================================================
// 构造函数
// ============================================================================

VulkanInstance::VulkanInstance(
    std::string const                                                              &application_name,
    uint32_t                                                                        api_version,
    std::unordered_map<std::string, RequestMode> const                             &requested_layers,
    std::unordered_map<std::string, RequestMode> const                             &requested_extensions,
    std::function<vk::InstanceCreateFlags(std::vector<std::string> const &)> const &get_create_flags,
    std::function<void(StructureChainBuilder<vk::InstanceCreateInfo> &)> const     &extend_instance_create_info)
{
    // ---- 0. 初始化动态加载器 ----
    auto vkGetInstanceProcAddr =
        m_Loader.getProcAddress<PFN_vkGetInstanceProcAddr>("vkGetInstanceProcAddr");
    VULKAN_HPP_DEFAULT_DISPATCHER.init(vkGetInstanceProcAddr);

    // ---- 1. 检查 API 版本 ----
    GE_CORE_INFO("Requesting Vulkan API version {}.{}",
                 VK_VERSION_MAJOR(api_version), VK_VERSION_MINOR(api_version));

    if (api_version < VK_API_VERSION_1_1)
    {
        GE_CORE_ERROR("Vulkan API version {}.{} is requested but version 1.1 or higher is required.",
                      VK_VERSION_MAJOR(api_version), VK_VERSION_MINOR(api_version));
        throw std::runtime_error("Requested Vulkan API version is too low.");
    }

    uint32_t instance_api_version = vk::enumerateInstanceVersion();
    GE_CORE_INFO("Vulkan instance supports API version {}.{}",
                 VK_VERSION_MAJOR(instance_api_version), VK_VERSION_MINOR(instance_api_version));

    if (instance_api_version < api_version)
    {
        GE_CORE_ERROR("Vulkan API version {}.{} is requested but only version {}.{} is supported.",
                      VK_VERSION_MAJOR(api_version), VK_VERSION_MINOR(api_version),
                      VK_VERSION_MAJOR(instance_api_version), VK_VERSION_MINOR(instance_api_version));
        throw std::runtime_error("Requested Vulkan API version is too high.");
    }

    // ---- 2. 启用 Layers ----
    std::vector<vk::LayerProperties> available_layers = vk::enumerateInstanceLayerProperties();
    std::vector<std::string>         enabled_layers;

    for (auto const &[layer_name, mode] : requested_layers)
    {
        if (!enable_layer(layer_name, available_layers, enabled_layers))
        {
            if (mode == RequestMode::Optional)
            {
                GE_CORE_WARN("Optional layer '{}' not available, some features may be disabled", layer_name);
            }
            else
            {
                GE_CORE_ERROR("Required layer '{}' not available, cannot run", layer_name);
                throw std::runtime_error("Required layers are missing.");
            }
        }
    }

    // ---- 3. 启用 Extensions ----
    std::vector<vk::ExtensionProperties> available_extensions = vk::enumerateInstanceExtensionProperties();

    // 如果启用了 VK_LAYER_KHRONOS_validation，合并该 Layer 提供的实例扩展
    if (std::ranges::any_of(enabled_layers, [](auto const &l) { return l == "VK_LAYER_KHRONOS_validation"; }))
    {
        std::string const                    validation_layer_name = "VK_LAYER_KHRONOS_validation";
        std::vector<vk::ExtensionProperties> layer_extensions      = vk::enumerateInstanceExtensionProperties(validation_layer_name);
        available_extensions.insert(available_extensions.end(), layer_extensions.begin(), layer_extensions.end());
    }

    // 始终添加平台必需的扩展
    m_EnabledExtensions.emplace_back(VK_KHR_SURFACE_EXTENSION_NAME);
    const char *platform_surface_ext = GetPlatformSurfaceExtension();
    if (platform_surface_ext)
    {
        m_EnabledExtensions.emplace_back(platform_surface_ext);
    }

    // 如果 Debug/Validation 宏定义，自动添加 VK_EXT_DEBUG_UTILS 作为可选扩展
#if defined(VKB_DEBUG) || defined(VKB_VALIDATION_LAYERS)
    auto it = std::ranges::find_if(available_extensions, [](auto const &ext) {
        return strcmp(ext.extensionName, VK_EXT_DEBUG_UTILS_EXTENSION_NAME) == 0;
    });
    bool has_debug_utils = (it != available_extensions.end());
    if (has_debug_utils)
    {
        m_EnabledExtensions.emplace_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
    }
    else
    {
        GE_CORE_WARN("{} is not available; debug messenger disabled", VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
    }
#endif

    // 启用用户请求的扩展
    for (auto const &[ext_name, mode] : requested_extensions)
    {
        if (!enable_extension(ext_name, available_extensions, m_EnabledExtensions))
        {
            if (mode == RequestMode::Optional)
            {
                GE_CORE_WARN("Optional instance extension '{}' not available, some features may be disabled", ext_name);
            }
            else
            {
                GE_CORE_ERROR("Required instance extension '{}' not available, cannot run", ext_name);
                throw std::runtime_error("Required instance extensions are missing.");
            }
        }
    }

    // ---- 4. 构建 InstanceCreateInfo ----
    vk::ApplicationInfo app_info{
        .pApplicationName = application_name.c_str(),
        .pEngineName      = "Game Engine",
        .apiVersion       = api_version,
    };

    // 转换为 C 字符串数组
    std::vector<char const *> enabled_layers_cstr;
    enabled_layers_cstr.reserve(enabled_layers.size());
    for (auto const &l : enabled_layers)
        enabled_layers_cstr.push_back(l.c_str());

    std::vector<char const *> enabled_extensions_cstr;
    enabled_extensions_cstr.reserve(m_EnabledExtensions.size());
    for (auto const &e : m_EnabledExtensions)
        enabled_extensions_cstr.push_back(e.c_str());

    vk::InstanceCreateInfo create_info{
        .flags                 = static_cast<vk::InstanceCreateFlags>(get_create_flags(m_EnabledExtensions)),
        .pApplicationInfo      = &app_info,
        .enabledLayerCount     = static_cast<uint32_t>(enabled_layers_cstr.size()),
        .ppEnabledLayerNames   = enabled_layers_cstr.data(),
        .enabledExtensionCount = static_cast<uint32_t>(enabled_extensions_cstr.size()),
        .ppEnabledExtensionNames = enabled_extensions_cstr.data(),
    };

    // 使用 StructureChainBuilder 支持 pNext 扩展
    StructureChainBuilder<vk::InstanceCreateInfo> scb;
    scb.set_anchor_struct(create_info);
    extend_instance_create_info(scb);

    // ---- 5. 创建 Vulkan Instance ----
    m_Instance = vk::createInstance(*scb.get_struct<vk::InstanceCreateInfo>());
    VULKAN_HPP_DEFAULT_DISPATCHER.init(m_Instance);

    // ---- 6. 注册 Debug 回调 ----
#if defined(VKB_DEBUG) || defined(VKB_VALIDATION_LAYERS)
    if (has_debug_utils)
    {
        vk::DebugUtilsMessengerCreateInfoEXT debug_info{
            .messageSeverity = vk::DebugUtilsMessageSeverityFlagBitsEXT::eError |
                               vk::DebugUtilsMessageSeverityFlagBitsEXT::eWarning,
            .messageType = vk::DebugUtilsMessageTypeFlagBitsEXT::eGeneral |
                           vk::DebugUtilsMessageTypeFlagBitsEXT::eValidation,
            .pfnUserCallback = DebugCallback,
        };

        m_DebugCallback = m_Instance.createDebugUtilsMessengerEXT(debug_info);
        GE_CORE_TRACE("DebugCallback has been registered");
    }
#endif
}

// ============================================================================
// 析构函数
// ============================================================================

VulkanInstance::~VulkanInstance()
{
    if (m_DebugCallback && m_Instance)
    {
        m_Instance.destroyDebugUtilsMessengerEXT(m_DebugCallback);
        m_DebugCallback = nullptr;
    }

    if (m_Instance)
    {
        m_Instance.destroy();
        m_Instance = nullptr;
    }
}

// ============================================================================
// 成员函数
// ============================================================================

bool VulkanInstance::IsExtensionEnabled(char const *extension) const
{
    return std::ranges::any_of(m_EnabledExtensions,
                               [extension](std::string const &enabled) { return enabled == extension; });
}

// static
vk::InstanceCreateFlags VulkanInstance::DefaultGetCreateFlags(std::vector<std::string> const &)
{
    return vk::InstanceCreateFlags{};
}

} // namespace GE
