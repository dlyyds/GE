/* Copyright (c) 2020-2026, Arm Limited and Contributors
 * Copyright (c) 2025-2026, NVIDIA CORPORATION. All rights reserved.
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

/**
 * @file PhysicalDevice.cpp
 * @brief PhysicalDevice 类的非模板方法实现。
 */

#include "Render/VulkanBase/PhysicalDevice.h"

#include <cstring>
#include <stdexcept>

namespace GE {

// ============================================================================
// 构造函数
// ============================================================================

PhysicalDevice::PhysicalDevice(VulkanInstance &instance, vk::PhysicalDevice physical_device) :
    instance{instance},
    handle{physical_device}
{
    features                = physical_device.getFeatures();
    properties              = physical_device.getProperties();
    memory_properties       = physical_device.getMemoryProperties();
    queue_family_properties = physical_device.getQueueFamilyProperties();
    device_extensions       = physical_device.enumerateDeviceExtensionProperties();

    GE_CORE_INFO("找到 GPU: {}", properties.deviceName.data());

    if (!device_extensions.empty())
    {
        GE_CORE_TRACE("该 GPU 支持以下扩展：");
        for (auto const &ext : device_extensions)
        {
            GE_CORE_TRACE("  \t{}", ext.extensionName.data());
        }
    }
}

// ============================================================================
// 队列族性能查询计数器
// ============================================================================

std::pair<std::vector<vk::PerformanceCounterKHR>, std::vector<vk::PerformanceCounterDescriptionKHR>>
    PhysicalDevice::EnumerateQueueFamilyPerformanceQueryCounters(uint32_t queue_family_index) const
{
    return handle.enumerateQueueFamilyPerformanceQueryCountersKHR(queue_family_index);
}

// ============================================================================
// 驱动版本
// ============================================================================

DriverVersion PhysicalDevice::GetDriverVersion() const
{
    DriverVersion version{};

    switch (properties.vendorID)
    {
        case 0x10DE:
            // NVIDIA 编码: major[22:31], minor[14:21], patch[6:13]
            version.major = (properties.driverVersion >> 22) & 0x3ff;
            version.minor = (properties.driverVersion >> 14) & 0x0ff;
            version.patch = (properties.driverVersion >> 6) & 0x0ff;
            break;

        case 0x8086:
            // Intel 编码: major[14:31], minor[0:13]
            version.major = (properties.driverVersion >> 14) & 0x3ffff;
            version.minor = properties.driverVersion & 0x3ffff;
            version.patch = 0;
            break;

        default:
            // 其他厂商使用标准 Vulkan 版本宏编码
            version.major = VK_VERSION_MAJOR(properties.driverVersion);
            version.minor = VK_VERSION_MINOR(properties.driverVersion);
            version.patch = VK_VERSION_PATCH(properties.driverVersion);
            break;
    }

    return version;
}

// ============================================================================
// 扩展特性结构链
// ============================================================================

void *PhysicalDevice::GetExtensionFeatureChain() const
{
    return last_requested_extension_feature;
}

// ============================================================================
// 设备特性
// ============================================================================

vk::PhysicalDeviceFeatures const &PhysicalDevice::GetFeatures() const
{
    return features;
}

// ============================================================================
// 格式属性
// ============================================================================

vk::FormatProperties PhysicalDevice::GetFormatProperties(vk::Format format) const
{
    return handle.getFormatProperties(format);
}

// ============================================================================
// 原生句柄
// ============================================================================

vk::PhysicalDevice PhysicalDevice::GetHandle() const
{
    return handle;
}

// ============================================================================
// VulkanInstance 引用
// ============================================================================

VulkanInstance &PhysicalDevice::GetInstance() const
{
    return instance;
}

// ============================================================================
// 内存属性
// ============================================================================

vk::PhysicalDeviceMemoryProperties const &PhysicalDevice::GetMemoryProperties() const
{
    return memory_properties;
}

// ============================================================================
// 查找内存类型
// ============================================================================

uint32_t PhysicalDevice::GetMemoryType(uint32_t bits, vk::MemoryPropertyFlags properties, vk::Bool32 *memory_type_found) const
{
    return GetMemoryTypeImpl(bits, properties, memory_type_found);
}

uint32_t PhysicalDevice::GetMemoryTypeImpl(uint32_t bits, vk::MemoryPropertyFlags properties, vk::Bool32 *memory_type_found) const
{
    for (uint32_t i = 0; i < memory_properties.memoryTypeCount; i++)
    {
        if ((bits & 1) == 1)
        {
            if ((memory_properties.memoryTypes[i].propertyFlags & properties) == properties)
            {
                if (memory_type_found)
                {
                    *memory_type_found = true;
                }
                return i;
            }
        }
        bits >>= 1;
    }

    if (memory_type_found)
    {
        *memory_type_found = false;
        return ~0U;
    }
    else
    {
        throw std::runtime_error("无法找到匹配的内存类型");
    }
}

// ============================================================================
// 可修改的请求特性
// ============================================================================

vk::PhysicalDeviceFeatures &PhysicalDevice::GetMutableRequestedFeatures()
{
    return requested_features;
}

// ============================================================================
// 物理设备属性
// ============================================================================

vk::PhysicalDeviceProperties const &PhysicalDevice::GetProperties() const
{
    return properties;
}

// ============================================================================
// 队列族性能查询 passes
// ============================================================================

uint32_t PhysicalDevice::GetQueueFamilyPerformanceQueryPasses(
    vk::QueryPoolPerformanceCreateInfoKHR const *perf_query_create_info) const
{
    return handle.getQueueFamilyPerformanceQueryPassesKHR(*perf_query_create_info);
}

// ============================================================================
// 队列族属性
// ============================================================================

std::vector<vk::QueueFamilyProperties> const &PhysicalDevice::GetQueueFamilyProperties() const
{
    return queue_family_properties;
}

// ============================================================================
// 已请求特性
// ============================================================================

vk::PhysicalDeviceFeatures const &PhysicalDevice::GetRequestedFeatures() const
{
    return requested_features;
}

// ============================================================================
// 高优先级图形队列
// ============================================================================

bool PhysicalDevice::HasHighPriorityGraphicsQueue() const
{
    return high_priority_graphics_queue;
}

void PhysicalDevice::SetHighPriorityGraphicsQueueEnable(bool enable)
{
    high_priority_graphics_queue = enable;
}

// ============================================================================
// 扩展支持检查
// ============================================================================

bool PhysicalDevice::IsExtensionSupported(const std::string &requested_extension) const
{
    return std::ranges::find_if(
               device_extensions,
               [&requested_extension](auto const &device_extension) {
                   return std::strcmp(device_extension.extensionName, requested_extension.c_str()) == 0;
               }) != device_extensions.end();
}

// ============================================================================
// Present 支持检查
// ============================================================================

vk::Bool32 PhysicalDevice::IsPresentSupported(vk::SurfaceKHR surface, uint32_t queue_family_index) const
{
    if (!surface)
    {
        return false;
    }
    return handle.getSurfaceSupportKHR(queue_family_index, surface);
}

} // namespace GE
