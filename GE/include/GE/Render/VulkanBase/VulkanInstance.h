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

/**
 * @file VulkanInstance.h
 * @brief Vulkan Instance 包装类，参照 Vulkan-Samples 的 Instance 模式实现。
 *
 * 支持：
 * - API 版本检查（要求 ≥ 1.1）
 * - 可选/必需 Layer 和 Extension 的启用
 * - 通过 StructureChainBuilder 自定义 pNext 链
 * - Debug 回调自动注册
 * - 已启用扩展的查询
 */

#pragma once

#include <vulkan/vulkan.hpp>

#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

#include "Render/VulkanBase/StructureChainBuilder.h"
#include "Render/VulkanBase/VulkanCommon.h"

namespace GE {

/**
 * @brief Vulkan Instance 包装类，构造即创建实例，析构即销毁。
 *
 * 用法示例：
 * @code
 *   VulkanInstance inst("MyApp", VK_API_VERSION_1_3);
 *   vk::Instance handle = inst.GetHandle();
 * @endcode
 */
class VulkanInstance {
public:
    /**
     * @brief 创建 Vulkan Instance。
     * @param application_name               应用名称。
     * @param api_version                    请求的 Vulkan API 版本（默认 1.3）。
     * @param requested_layers               请求启用的 Layer，按名称 → RequestMode 映射。
     * @param requested_extensions           请求启用的 Extension，按名称 → RequestMode 映射。
     * @param get_create_flags               返回 InstanceCreateFlags 的回调函数。
     * @param extend_instance_create_info    扩展 pNext 链的回调函数，接收 StructureChainBuilder。
     * @throws std::runtime_error 必需 Layer 或 Extension 不可用时抛出。
     */
    explicit VulkanInstance(
        std::string const                                                              &application_name           = "GE App",
        uint32_t                                                                        api_version                 = VK_API_VERSION_1_3,
        std::unordered_map<std::string, RequestMode> const                             &requested_layers            = {},
        std::unordered_map<std::string, RequestMode> const                             &requested_extensions        = {},
        std::function<vk::InstanceCreateFlags(std::vector<std::string> const &)> const &get_create_flags            = DefaultGetCreateFlags,
        std::function<void(StructureChainBuilder<vk::InstanceCreateInfo> &)> const     &extend_instance_create_info = [](auto &) {});

    ~VulkanInstance();

    VulkanInstance(VulkanInstance const &) = delete;
    VulkanInstance(VulkanInstance &&)      = delete;
    VulkanInstance &operator=(VulkanInstance const &) = delete;
    VulkanInstance &operator=(VulkanInstance &&)      = delete;

    /// 返回 Vulkan 实例句柄。
    [[nodiscard]] vk::Instance GetHandle() const { return m_Instance; }

    /// 返回 vkGetInstanceProcAddr 函数指针（用于 ImGui 等外部库）。
    [[nodiscard]] PFN_vkGetInstanceProcAddr GetVkGetInstanceProcAddr() const {
        return m_Loader.getProcAddress<PFN_vkGetInstanceProcAddr>("vkGetInstanceProcAddr");
    }

    /// 检查指定扩展是否已启用。
    [[nodiscard]] bool IsExtensionEnabled(char const *extension) const;

    /// 返回已启用的扩展列表。
    [[nodiscard]] std::vector<std::string> const &GetEnabledExtensions() const { return m_EnabledExtensions; }

    /// 默认的 InstanceCreateFlags 回调（返回空 flags）。
    static vk::InstanceCreateFlags DefaultGetCreateFlags(std::vector<std::string> const &);

private:

    /// 正在使用的 Vulkan 实例句柄
    vk::Instance m_Instance = nullptr;

    /// 已启用的扩展名称列表（用于 IsExtensionEnabled 查询）
    std::vector<std::string> m_EnabledExtensions;

    /// 动态加载器，用于获取 vkGetInstanceProcAddr
    vk::detail::DynamicLoader m_Loader;
};

} // namespace GE
