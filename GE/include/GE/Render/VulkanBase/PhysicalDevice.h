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
 * @file PhysicalDevice.h
 * @brief 物理设备包装类，参照 Vulkan-Samples 的 PhysicalDevice 模板类实现（仅 Cpp 绑定）。
 *
 * 封装 vk::PhysicalDevice 的功能、属性、队列族等信息，提供扩展特性链管理、
 * 可选/必需特性请求等能力，用于逻辑设备创建前的 GPU 信息收集和特性协商。
 */

#pragma once

#include <vulkan/vulkan.hpp>

#include <map>
#include <memory>
#include <string>
#include <vector>

#include "Core/Log.h"
#include "Render/VulkanBase/VulkanCommon.h"
#include "Render/VulkanBase/VulkanInstance.h"

namespace GE {

/**
 * @brief 驱动版本号（厂商无关表示）。
 */
struct DriverVersion
{
    uint16_t major;
    uint16_t minor;
    uint16_t patch;
};

/**
 * @brief 物理设备包装类。
 *
 * 负责管理 GPU 功能、属性、队列族和扩展特性结构链。
 * 非拷贝可移动，生命周期由持有者（VulkanDevice 或类似）管理。
 */
class PhysicalDevice
{
  public:
    PhysicalDevice(VulkanInstance &instance, vk::PhysicalDevice physical_device);

    PhysicalDevice(PhysicalDevice const &) = delete;
    PhysicalDevice(PhysicalDevice &&)      = delete;
    PhysicalDevice &operator=(PhysicalDevice const &) = delete;
    PhysicalDevice &operator=(PhysicalDevice &&)      = delete;

    // ========================================================================
    // 模板方法 — 必须在头文件中实现
    // ========================================================================

    /**
     * @brief 向结构链中添加扩展特性结构体。
     *
     * 必须在逻辑设备创建前调用。返回的引用可修改特性标志位，
     * 修改会传播到逻辑设备创建时的 pNext 链。
     *
     * @tparam FeatureType 扩展特性结构体类型（如 vk::PhysicalDeviceVulkan12Features）。
     * @return 扩展特性结构体的引用。
     */
    template <typename FeatureType>
    FeatureType &AddExtensionFeatures();

    /**
     * @brief 获取 GPU 实际支持的扩展特性。
     *
     * @tparam T 扩展特性结构体类型。
     * @return GPU 支持的扩展特性结构体（只读，标志位反映硬件能力）。
     */
    template <typename T>
    T GetExtensionFeatures();

    /**
     * @brief 请求可选的特性标志。
     *
     * 若硬件支持则启用该标志；否则仅记录日志。
     *
     * @tparam Feature 扩展特性结构体类型。
     * @param flag      指向特性标志的成员指针（如 &vk::PhysicalDeviceFeatures::tessellationShader）。
     * @param featureName 特性名称（用于日志/异常）。
     * @param flagName    标志名称（用于日志/异常）。
     * @return VK_TRUE 如果特性受支持。
     */
    template <typename Feature>
    vk::Bool32 RequestOptionalFeature(vk::Bool32 Feature::*flag, std::string const &featureName, std::string const &flagName);

    /**
     * @brief 请求必需的特性标志。
     *
     * 若硬件支持则启用该标志；否则抛出 std::runtime_error。
     *
     * @tparam Feature 扩展特性结构体类型。
     * @param flag      指向特性标志的成员指针。
     * @param featureName 特性名称（用于异常消息）。
     * @param flagName    标志名称（用于异常消息）。
     */
    template <typename Feature>
    void RequestRequiredFeature(vk::Bool32 Feature::*flag, std::string const &featureName, std::string const &flagName);

    // ========================================================================
    // 普通方法
    // ========================================================================

    /// 枚举指定队列族的性能查询计数器。
    std::pair<std::vector<vk::PerformanceCounterKHR>, std::vector<vk::PerformanceCounterDescriptionKHR>>
        EnumerateQueueFamilyPerformanceQueryCounters(uint32_t queue_family_index) const;

    /// 获取驱动版本号（从 driverVersion 字段解析，适配各厂商编码方式）。
    DriverVersion GetDriverVersion() const;

    /// 获取扩展特性结构链起始指针，用于 vkCreateDevice 的 pNext。
    void *GetExtensionFeatureChain() const;

    /// 获取硬件支持的设备特性。
    vk::PhysicalDeviceFeatures const &GetFeatures() const;

    /// 获取指定格式的属性。
    vk::FormatProperties GetFormatProperties(vk::Format format) const;

    /// 获取原生 Vulkan 物理设备句柄。
    vk::PhysicalDevice GetHandle() const;

    /// 获取关联的 Vulkan Instance。
    VulkanInstance &GetInstance() const;

    /// 获取物理设备内存属性。
    vk::PhysicalDeviceMemoryProperties const &GetMemoryProperties() const;

    /// 查找满足类型位掩码和内存属性要求的内存类型索引。
    uint32_t GetMemoryType(uint32_t bits, vk::MemoryPropertyFlags properties, vk::Bool32 *memory_type_found = nullptr) const;

    /// 获取可修改的已请求特性引用（可在创建设备前修改）。
    vk::PhysicalDeviceFeatures &GetMutableRequestedFeatures();

    /// 获取物理设备属性。
    vk::PhysicalDeviceProperties const &GetProperties() const;

    /// 获取队列族性能查询所需的 passes 数量。
    uint32_t GetQueueFamilyPerformanceQueryPasses(vk::QueryPoolPerformanceCreateInfoKHR const *perf_query_create_info) const;

    /// 获取队列族属性列表。
    std::vector<vk::QueueFamilyProperties> const &GetQueueFamilyProperties() const;

    /// 获取已请求的设备特性（用于逻辑设备创建）。
    vk::PhysicalDeviceFeatures const &GetRequestedFeatures() const;

    /// 是否启用了高优先级图形队列（用于异步计算等场景）。
    bool HasHighPriorityGraphicsQueue() const;

    /// 检查指定扩展名称是否受此 GPU 支持。
    bool IsExtensionSupported(const std::string &extension) const;

    /// 检查指定队列族是否支持呈现到给定 surface。
    vk::Bool32 IsPresentSupported(vk::SurfaceKHR surface, uint32_t queue_family_index) const;

    /// 设置是否启用高优先级图形队列。
    void SetHighPriorityGraphicsQueueEnable(bool enable);

  private:
    // -- 非模板内部实现 --
    uint32_t GetMemoryTypeImpl(uint32_t bits, vk::MemoryPropertyFlags properties, vk::Bool32 *memory_type_found = nullptr) const;

    // -- 成员变量 --
    std::vector<vk::ExtensionProperties>                   device_extensions;        ///< GPU 支持的扩展列表
    std::map<vk::StructureType, std::shared_ptr<void>>     extension_features;       ///< 扩展特性结构链（按 StructureType 有序存储）
    vk::PhysicalDeviceFeatures                             features;                 ///< GPU 支持的设备特性
    vk::PhysicalDevice                                     handle;                   ///< Vulkan 物理设备句柄
    bool                                                   high_priority_graphics_queue = {}; ///< 高优先级图形队列开关
    VulkanInstance                                        &instance;                 ///< 关联的 Vulkan Instance
    void                                                 *last_requested_extension_feature = nullptr; ///< 扩展特性 pNext 链头（头插法最新节点）
    vk::PhysicalDeviceMemoryProperties                     memory_properties;        ///< GPU 内存属性
    vk::PhysicalDeviceProperties                           properties;               ///< GPU 属性
    std::vector<vk::QueueFamilyProperties>                 queue_family_properties;  ///< 队列族属性列表
    vk::PhysicalDeviceFeatures                             requested_features;       ///< 请求启用的设备特性
};

// ========================================================================
// 模板方法实现（必须在头文件中，因模板实例化需可见）
// ========================================================================

template <typename FeatureType>
inline FeatureType &PhysicalDevice::AddExtensionFeatures()
{
    if (!instance.IsExtensionEnabled(VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME))
    {
        throw std::runtime_error("无法请求扩展特性：" +
                                 std::string(VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME) + " 未启用");
    }

    auto ptr = std::make_shared<FeatureType>();
    ptr->sType = FeatureType::structureType;
    auto [it, added] = extension_features.insert({FeatureType::structureType, std::move(ptr)});
    if (added)
    {
        // 头插法：新节点插入链表头部，pNext 指向旧头
        static_cast<FeatureType *>(it->second.get())->pNext = last_requested_extension_feature;
        last_requested_extension_feature = it->second.get();
    }

    return *static_cast<FeatureType *>(it->second.get());
}

template <typename FeatureType>
inline FeatureType PhysicalDevice::GetExtensionFeatures()
{
    if (!instance.IsExtensionEnabled(VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME))
    {
        throw std::runtime_error("无法请求扩展特性：" +
                                 std::string(VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME) + " 未启用");
    }

    // 使用 vkGetPhysicalDeviceFeatures2KHR 获取扩展特性
    return handle.getFeatures2KHR<vk::PhysicalDeviceFeatures2KHR, FeatureType>().template get<FeatureType>();
}

template <typename Feature>
inline vk::Bool32 PhysicalDevice::RequestOptionalFeature(
    vk::Bool32 Feature::*flag,
    std::string const    &featureName,
    std::string const    &flagName)
{
    vk::Bool32 supported = GetExtensionFeatures<Feature>().*flag;
    if (supported)
    {
        AddExtensionFeatures<Feature>().*flag = true;
    }
    else
    {
        GE_CORE_INFO("请求的可选特性 <{}::{}> 不受支持", featureName, flagName);
    }
    return supported;
}

template <typename Feature>
inline void PhysicalDevice::RequestRequiredFeature(
    vk::Bool32 Feature::*flag,
    std::string const    &featureName,
    std::string const    &flagName)
{
    if (GetExtensionFeatures<Feature>().*flag)
    {
        AddExtensionFeatures<Feature>().*flag = true;
    }
    else
    {
        throw std::runtime_error(std::string("请求的必需特性 <") + featureName + "::" + flagName + "> 不受支持");
    }
}

// ========================================================================
// 便捷宏 — 简化可选/必需特性请求调用
// ========================================================================

/// 请求可选特性：若 GPU 支持则启用。
#define REQUEST_OPTIONAL_FEATURE(gpu, Feature, flag) gpu.RequestOptionalFeature<Feature>(&Feature::flag, #Feature, #flag)

/// 请求必需特性：若 GPU 不支持则抛出异常。
#define REQUEST_REQUIRED_FEATURE(gpu, Feature, flag) gpu.RequestRequiredFeature<Feature>(&Feature::flag, #Feature, #flag)

} // namespace GE
