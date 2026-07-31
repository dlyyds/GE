/* Copyright (c) 2025, NVIDIA CORPORATION. All rights reserved.
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
 * @file VulkanDevice.h
 * @brief Vulkan 逻辑设备封装，从 Vulkan-Samples 的 Device 模板适配（仅 Cpp 绑定）。
 *
 * 包装 vk::Device，管理队列、CommandPool、FencePool、VMA 分配器和资源缓存。
 * 构造即创建设备，析构即销毁设备。
 */

#pragma once

#include <vulkan/vulkan.hpp>
#include "vk_mem_alloc.h"

#include <cstdint>
#include <cstring>
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "Render/VulkanBase/PhysicalDevice.h"
#include "Render/VulkanBase/VulkanCommon.h"
#include "Render/VulkanBase/VulkanQueue.h"
#include "Render/VulkanBase/VulkanResourceBase.h"

namespace GE {

class DebugUtils;
class VulkanBuffer;
class VulkanCommandBuffer;
class VulkanCommandPool;
class VulkanFencePool;
class VulkanResourceCache;

/**
 * @brief Vulkan 逻辑设备。
 *
 * 封装 vk::Device 的生命周期，提供队列管理、命令缓冲、buffer/image 辅助方法、
 * fence pool、debug utils 和 VMA 分配器初始化。
 *
 * 使用方式：
 * @code
 *   VulkanDevice device(gpu, surface, debug_utils, extensions, features_cb);
 *   vk::Queue graphics_queue = device.GetQueueByFlags(vk::QueueFlagBits::eGraphics, 0).GetHandle();
 * @endcode
 */
class VulkanDevice : public VulkanResourceBase<vk::Device> {
public:
    /// @brief 主构造函数：创建逻辑设备
    /// @param gpu                  已选定的物理设备
    /// @param surface              窗口 surface（用于 present 支持查询）
    /// @param debug_utils          调试工具实例（DebugUtilsExt / DummyDebugUtils）
    /// @param requested_extensions 请求的扩展列表（扩展名 → 是否可选）
    /// @param request_gpu_features 创建前回调，用于请求 GPU 扩展特性
    VulkanDevice(PhysicalDevice &gpu,
                 vk::SurfaceKHR surface,
                 std::unique_ptr<DebugUtils> &&debug_utils,
                 std::unordered_map<std::string, RequestMode> const &requested_extensions = {},
                 std::function<void(PhysicalDevice &)> request_gpu_features = {});

    /// @brief 包装构造函数：包装已有 vk::Device
    /// @param gpu            关联的物理设备
    /// @param vulkan_device  已有的 vk::Device 句柄
    /// @param surface        窗口 surface
    VulkanDevice(PhysicalDevice &gpu, vk::Device &vulkan_device, vk::SurfaceKHR surface);

    VulkanDevice(const VulkanDevice &) = delete;

    VulkanDevice(VulkanDevice &&) = delete;

    ~VulkanDevice();

    VulkanDevice &operator=(const VulkanDevice &) = delete;

    VulkanDevice &operator=(VulkanDevice &&) = delete;

    // =================================================================
    // 队列管理
    // =================================================================

    /// @brief 向设备添加一个队列。
    /// @param global_index  队列在 m_Queues 中的外层索引（通常 = family_index）
    /// @param family_index  队列族索引
    /// @param properties    队列族属性
    /// @param can_present   是否支持 present
    void AddQueue(size_t global_index, uint32_t family_index,
                  vk::QueueFamilyProperties const &properties, vk::Bool32 can_present);

    // =================================================================
    // Buffer / Image 工具
    // =================================================================

    /// @brief 从内建 command pool 请求一个 command buffer（pool 为唯一所有者）。
    /// @param level  Command buffer 级别
    /// @param begin  是否立即开始录制
    /// @return command buffer 引用，生命周期跟随内建 command pool。
    [[nodiscard]] VulkanCommandBuffer &RequestCommandBuffer(
        vk::CommandBufferLevel level = vk::CommandBufferLevel::ePrimary, bool begin = false);

    /// @brief 提交 command buffer、等待完成。
    /// @param command_buffer     要刷新的 command buffer
    /// @param queue              提交到的队列
    /// @param signal_semaphore   可选，提交时 signal 的信号量
    void FlushCommandBuffer(const VulkanCommandBuffer &command_buffer,
                            vk::Queue queue,
                            vk::Semaphore signal_semaphore = nullptr) const;

    // =================================================================
    // 访问器
    // =================================================================

    /// @brief 获取 debug utils。
    DebugUtils const &GetDebugUtils() const;

    /// @brief 获取关联的 PhysicalDevice。
    PhysicalDevice const &GetGpu() const;

    /// @brief 获取 VMA 分配器。
    [[nodiscard]] VmaAllocator GetVmaAllocator() const { return m_VmaAllocator; }

    /// @brief 获取全局资源缓存。
    VulkanResourceCache &GetResourceCache() { return *m_ResourceCache; }

    /// @brief 按队列能力标志获取队列。
    VulkanQueue const &GetQueueByFlags(vk::QueueFlags required_queue_flags, uint32_t queue_index) const;

    // =================================================================
    // 查询
    // =================================================================

    /// @brief 检查扩展是否已启用。
    bool IsExtensionEnabled(const char *extension) const;

    /// @brief 重置内建 command pool，释放所有已分配的 command buffer。
    /// 调用后之前从 RequestCommandBuffer 获取的引用全部失效。
    void ResetCommandPool();

    /// @brief 等待设备空闲（调试/析构时使用，性能敏感路径避免调用）
    void WaitIdle() const;

private:
    void Init(std::unordered_map<std::string, RequestMode> const &requested_extensions,
              std::function<void(PhysicalDevice &)> request_gpu_features);

    // ---- 成员 ----
    std::unique_ptr<VulkanCommandPool> m_CommandPool;
    std::unique_ptr<DebugUtils> m_DebugUtils;
    std::vector<const char *> m_EnabledExtensions;
    std::unique_ptr<VulkanFencePool> m_FencePool;
    PhysicalDevice &m_Gpu;
    std::unique_ptr<VulkanResourceCache> m_ResourceCache;
    std::vector<std::vector<VulkanQueue> > m_Queues; ///< [family_index][queue_index]
    vk::SurfaceKHR m_Surface = nullptr;
    VmaAllocator m_VmaAllocator = nullptr;
};

} // namespace GE
