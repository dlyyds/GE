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

#include "Render/VulkanBase/VulkanDevice.h"
#include "Render/VulkanBase/VulkanCommandBuffer.h"
#include "Render/VulkanBase/VulkanCommandPool.h"
#include "Render/VulkanBase/VulkanDebug.h"
#include "Render/VulkanBase/VulkanFencePool.h"
#include "Render/VulkanBase/VulkanResourceCache.h"
#include "Core/Log.h"

#include <cassert>
#include <stdexcept>
#include <string>
#include <utility>

namespace GE {

// ============================================================================
// 主构造函数
// ============================================================================

VulkanDevice::VulkanDevice(PhysicalDevice &gpu,
                           vk::SurfaceKHR surface,
                           std::unique_ptr<DebugUtils> &&debug_utils,
                           std::unordered_map<std::string, RequestMode> const &requested_extensions,
                           std::function<void(PhysicalDevice &)> request_gpu_features) : VulkanResourceBase<vk::Device>{nullptr, this},
                                                                                         m_DebugUtils{
                                                                                             debug_utils
                                                                                                 ? std::move(debug_utils)
                                                                                                 : std::make_unique<DummyDebugUtils>()},
                                                                                         m_Gpu{gpu},
                                                                                         m_Surface{surface} {
    Init(requested_extensions, std::move(request_gpu_features));
}

// ============================================================================
// 包装构造函数：包装已有 vk::Device
// ============================================================================

VulkanDevice::VulkanDevice(PhysicalDevice &gpu, vk::Device &vulkan_device, vk::SurfaceKHR surface) : VulkanResourceBase<vk::Device>
    {vulkan_device, this},
    m_DebugUtils{std::make_unique<DummyDebugUtils>()},
    m_Gpu{gpu},
    m_Surface{surface} {
    // 包装模式下不执行 Init，直接使用传入的设备
}

// ============================================================================
// 析构函数
// ============================================================================

VulkanDevice::~VulkanDevice() {
    // 先销毁资源缓存（其内部资源可能引用 command pool / fence pool）
    m_ResourceCache.reset();

    m_CommandPool.reset();
    m_FencePool.reset();

    // 销毁 VMA 分配器（必须在 vkDestroyDevice 之前）
    if (m_VmaAllocator) {
        vmaDestroyAllocator(m_VmaAllocator);
        m_VmaAllocator = nullptr;
    }

    if (this->GetHandle()) {
        this->GetHandle().destroy();
    }
}

// ============================================================================
// Init — 核心初始化
// ============================================================================

void VulkanDevice::Init(std::unordered_map<std::string, RequestMode> const &requested_extensions,
                        std::function<void(PhysicalDevice &)> request_gpu_features) {
    GE_CORE_INFO("Selected GPU: {}", m_Gpu.GetProperties().deviceName.data());

    // ---- 1. 准备所有队列族的创建信息 ----
    auto const &queue_family_properties = m_Gpu.GetQueueFamilyProperties();
    std::vector<vk::DeviceQueueCreateInfo> queue_create_infos;
    std::vector<std::vector<float> > queue_priorities;

    queue_create_infos.reserve(queue_family_properties.size());
    queue_priorities.reserve(queue_family_properties.size());

    for (uint32_t family_index = 0; family_index < queue_family_properties.size(); ++family_index) {
        auto const &qfp = queue_family_properties[family_index];

        queue_priorities.emplace_back(qfp.queueCount, 0.5f);

        // 如果启用了高优先级图形队列，将图形队列族的第一个队列设为 1.0
        if (m_Gpu.HasHighPriorityGraphicsQueue() &&
            (get_queue_family_index(queue_family_properties, vk::QueueFlagBits::eGraphics) == family_index)) {
            queue_priorities.back()[0] = 1.0f;
        }

        queue_create_infos.push_back(vk::DeviceQueueCreateInfo{
            .queueFamilyIndex = family_index,
            .queueCount = qfp.queueCount,
            .pQueuePriorities = queue_priorities[family_index].data(),
        });
    }

    // ---- 2. 检查并启用扩展 ----
    // 2a. 基础必需扩展（Swapchain + ExtendedDynamicState）
    std::vector<const char *> required_core_extensions = {
        VK_KHR_SWAPCHAIN_EXTENSION_NAME,
        VK_EXT_EXTENDED_DYNAMIC_STATE_EXTENSION_NAME,
    };

    for (auto const &ext : required_core_extensions) {
        if (m_Gpu.IsExtensionSupported(ext)) {
            m_EnabledExtensions.emplace_back(ext);
        } else {
            throw std::runtime_error(std::string("Required device extension not available: ") + ext);
        }
    }

#if defined(VK_ENABLE_PORTABILITY)
    if (m_Gpu.IsExtensionSupported(VK_KHR_PORTABILITY_SUBSET_EXTENSION_NAME)) {
        m_EnabledExtensions.push_back(VK_KHR_PORTABILITY_SUBSET_EXTENSION_NAME);
    }
#endif

    // 2b. 可选扩展（VMA Dedicated Allocation）
    bool can_get_memory_requirements = m_Gpu.IsExtensionSupported("VK_KHR_get_memory_requirements2");
    bool has_dedicated_allocation = m_Gpu.IsExtensionSupported("VK_KHR_dedicated_allocation");

    if (can_get_memory_requirements && has_dedicated_allocation) {
        m_EnabledExtensions.emplace_back("VK_KHR_get_memory_requirements2");
        m_EnabledExtensions.emplace_back("VK_KHR_dedicated_allocation");
        GE_CORE_INFO("Dedicated Allocation enabled");
    }

    // 2c. 请求的外部扩展
    std::vector<const char *> unsupported_extensions;
    for (auto const &[ext_name, mode] : requested_extensions) {
        if (m_Gpu.IsExtensionSupported(ext_name)) {
            // 避免重复添加
            auto already = std::ranges::find_if(m_EnabledExtensions,
                                                [&ext_name](const char *enabled) { return ext_name == enabled; });
            if (already == m_EnabledExtensions.end()) {
                m_EnabledExtensions.push_back(ext_name.c_str());
            }
        } else {
            unsupported_extensions.push_back(ext_name.c_str());
        }
    }

    // 记录启用的扩展
    if (!m_EnabledExtensions.empty()) {
        GE_CORE_INFO("Device enabled extensions:");
        for (auto const &ext : m_EnabledExtensions) {
            GE_CORE_INFO("  \t{}", ext);
        }
    }

    // 处理不支持的扩展
    bool error = false;
    for (auto const &ext : unsupported_extensions) {
        auto it = requested_extensions.find(ext);
        if (it != requested_extensions.end() && it->second == RequestMode::Optional) {
            GE_CORE_WARN("Optional device extension '{}' not available, some features may be disabled", ext);
        } else {
            GE_CORE_ERROR("Required device extension '{}' not available, cannot run", ext);
            error = true;
        }
    }
    if (error) {
        throw std::runtime_error("Required device extensions not present");
    }

    // ---- 3. 调用 GPU 特性请求回调 ----
    if (request_gpu_features) {
        request_gpu_features(m_Gpu);
    }

    // ---- 4. 创建逻辑设备 ----
    vk::DeviceCreateInfo create_info{
        .pNext = m_Gpu.GetExtensionFeatureChain(),
        .queueCreateInfoCount = static_cast<uint32_t>(queue_create_infos.size()),
        .pQueueCreateInfos = queue_create_infos.data(),
        .enabledExtensionCount = static_cast<uint32_t>(m_EnabledExtensions.size()),
        .ppEnabledExtensionNames = m_EnabledExtensions.data(),
        .pEnabledFeatures = &m_Gpu.GetRequestedFeatures(),
    };

    vk::Device device = m_Gpu.GetHandle().createDevice(create_info);
    this->SetHandle(device);

    VULKAN_HPP_DEFAULT_DISPATCHER.init(device);

    // ---- 5. 创建所有队列 ----
    m_Queues.resize(queue_family_properties.size());

    for (uint32_t family_index = 0; family_index < queue_family_properties.size(); ++family_index) {
        auto const &qfp = queue_family_properties[family_index];
        vk::Bool32 present_supported = m_Gpu.IsPresentSupported(m_Surface, family_index);

        for (uint32_t queue_index = 0; queue_index < qfp.queueCount; ++queue_index) {
            m_Queues[family_index].emplace_back(*this, family_index, qfp, present_supported, queue_index);
        }

        // 诊断日志：列出该队列族的能力位，便于确认设备上是否有独立 transfer / compute
        // 队列族（若 graphics 族已含 transfer，则独立 transfer 族才有额外收益）。
        auto has_bit = [&qfp](vk::QueueFlagBits bit) {
            return (qfp.queueFlags & bit) != vk::QueueFlags{};
        };
        std::string caps;
        auto append = [&caps](bool on, const char *name) {
            if (on) {
                if (!caps.empty()) {
                    caps += "|";
                }
                caps += name;
            }
        };
        append(has_bit(vk::QueueFlagBits::eGraphics), "Graphics");
        append(has_bit(vk::QueueFlagBits::eCompute), "Compute");
        append(has_bit(vk::QueueFlagBits::eTransfer), "Transfer");
        append(has_bit(vk::QueueFlagBits::eSparseBinding), "SparseBinding");
        append(has_bit(vk::QueueFlagBits::eProtected), "Protected");

        GE_CORE_INFO("[QueueFamily {0}] count={1} caps=[{2}] present={3}",
                     family_index, qfp.queueCount, caps,
                     present_supported ? "yes" : "no");
    }

    // ---- 6. 初始化 VMA 分配器 ----
    VmaVulkanFunctions vk_funcs{};
    vk_funcs.vkGetInstanceProcAddr = VULKAN_HPP_DEFAULT_DISPATCHER.vkGetInstanceProcAddr;
    vk_funcs.vkGetDeviceProcAddr = VULKAN_HPP_DEFAULT_DISPATCHER.vkGetDeviceProcAddr;

    VmaAllocatorCreateInfo alloc_info{};
    alloc_info.vulkanApiVersion = VK_API_VERSION_1_3;
    alloc_info.instance = static_cast<VkInstance>(m_Gpu.GetInstance().GetHandle());
    alloc_info.physicalDevice = static_cast<VkPhysicalDevice>(m_Gpu.GetHandle());
    alloc_info.device = static_cast<VkDevice>(device);
    alloc_info.pVulkanFunctions = &vk_funcs;

    VmaAllocator vma_allocator{nullptr};
    VkResult result = vmaCreateAllocator(&alloc_info, &vma_allocator);
    if (result != VK_SUCCESS) {
        throw std::runtime_error("Failed to create VMA allocator");
    }
    m_VmaAllocator = vma_allocator;

    // ---- 7. 创建内建 command pool 和 fence pool ----
    uint32_t family_index = GetQueueByFlags(
        vk::QueueFlagBits::eGraphics | vk::QueueFlagBits::eCompute, 0).GetFamilyIndex();
    m_CommandPool = std::make_unique<VulkanCommandPool>(*this, family_index);
    m_FencePool = std::make_unique<VulkanFencePool>(this->GetHandle());

    // ---- 8. 创建全局资源缓存 ----
    m_ResourceCache = std::make_unique<VulkanResourceCache>(*this);
}

// ============================================================================
// AddQueue
// ============================================================================

void VulkanDevice::AddQueue(size_t global_index, uint32_t family_index,
                            vk::QueueFamilyProperties const &properties, vk::Bool32 can_present) {
    if (m_Queues.size() <= global_index) {
        m_Queues.resize(global_index + 1);
    }
    m_Queues[global_index].emplace_back(*this, family_index, properties, can_present, 0);
}

// ============================================================================
// RequestCommandBuffer
// ============================================================================

VulkanCommandBuffer &VulkanDevice::RequestCommandBuffer(
    vk::CommandBufferLevel level, bool begin) {
    assert(m_CommandPool && "No command pool exists in the device");

    auto &cmd = m_CommandPool->RequestCommandBuffer(level);
    if (begin) {
        cmd.Begin(vk::CommandBufferUsageFlagBits::eOneTimeSubmit);
    }
    return cmd;
}

// ============================================================================
// FlushCommandBuffer
// ============================================================================

void VulkanDevice::FlushCommandBuffer(const VulkanCommandBuffer &command_buffer,
                                      const VulkanQueue &queue,
                                      vk::Semaphore signal_semaphore) const {
    if (!command_buffer.HasHandle()) {
        return;
    }

    vk::CommandBuffer native = command_buffer.GetHandle();

    // 创建 fence 确保 command buffer 执行完成
    vk::Fence fence = GetHandle().createFence(vk::FenceCreateInfo{});

    // 统一经队列的提交入口（内部对本队列提交加互斥锁串行）
    queue.Submit({native}, fence, nullptr, {}, signal_semaphore);

    // 等待 fence
    vk::Result result = GetHandle().waitForFences(1, &fence, VK_TRUE, DEFAULT_FENCE_TIMEOUT);
    if (result != vk::Result::eSuccess) {
        GE_CORE_ERROR("Detected Vulkan error: {}", vk::to_string(result));
        abort();
    }

    GetHandle().destroyFence(fence);
}

// ============================================================================
// GetDebugUtils
// ============================================================================

DebugUtils const &VulkanDevice::GetDebugUtils() const {
    return *m_DebugUtils;
}

// ============================================================================
// GetGpu
// ============================================================================

PhysicalDevice const &VulkanDevice::GetGpu() const {
    return m_Gpu;
}

// ============================================================================
// GetQueue — 按队列族索引获取
// ============================================================================

VulkanQueue const &VulkanDevice::GetQueue(uint32_t family_index, uint32_t queue_index) const {
    assert(family_index < m_Queues.size() && "family_index 超出范围");
    assert(queue_index < m_Queues[family_index].size() && "queue_index 超出范围");
    assert(!m_Queues[family_index].empty() && "该队列族无可用队列");
    return m_Queues[family_index][queue_index];
}

// ============================================================================
// GetQueueByFlags
// ============================================================================

VulkanQueue const &VulkanDevice::GetQueueByFlags(vk::QueueFlags required_queue_flags, uint32_t queue_index) const {
    auto queue_it = std::ranges::find_if(m_Queues,
                                         [required_queue_flags, queue_index](std::vector<VulkanQueue> const &family) {
                                             assert(!family.empty());
                                             vk::QueueFamilyProperties const &props = family[0].GetProperties();
                                             return ((props.queueFlags & required_queue_flags) == required_queue_flags) &&
                                                    (queue_index < props.queueCount);
                                         });

    if (queue_it == m_Queues.end()) {
        throw std::runtime_error("Queue not found");
    }

    return (*queue_it)[queue_index];
}

// ============================================================================
// IsExtensionEnabled
// ============================================================================

bool VulkanDevice::IsExtensionEnabled(const char *extension) const {
    return std::ranges::find_if(m_EnabledExtensions,
                                [extension](const char *enabled) {
                                    return strcmp(extension, enabled) == 0;
                                }) != m_EnabledExtensions.end();
}

// ============================================================================
// ResetCommandPool
// ============================================================================

void VulkanDevice::ResetCommandPool() {
    if (m_CommandPool) {
        m_CommandPool->ResetPool();
    }
}

// ============================================================================
// WaitIdle
// ============================================================================

void VulkanDevice::WaitIdle() const {
    if (this->GetHandle()) {
        this->GetHandle().waitIdle();
    }
}

} // namespace GE
