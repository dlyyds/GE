/* Copyright (c) 2019-2025, Arm Limited and Contributors
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
 * @file VulkanFencePool.h
 * @brief Vulkan Fence 对象池，适配自 Vulkan-Samples 的 FencePool。
 *
 * 管理一组 vk::Fence 的生命周期，支持按需分配、批量等待和重置。
 * 适用于需要动态提交多个 command buffer 并分别同步的场景。
 */

#pragma once

#include <vulkan/vulkan.hpp>

#include <cstdint>
#include <limits>
#include <vector>

namespace GE {

/**
 * @brief Vulkan Fence 对象池。
 *
 * 维护一个可增长的 fence 池，active_fence_count 之前的 fence 视为"已分配使用"，
 * 之后的 fence 视为"可用"。RequestFence 优先返回可用 fence，无可用时创建新 fence。
 */
class VulkanFencePool {
public:
    explicit VulkanFencePool(vk::Device device);

    VulkanFencePool(const VulkanFencePool &) = delete;

    VulkanFencePool(VulkanFencePool &&) = delete;

    ~VulkanFencePool();

    VulkanFencePool &operator=(const VulkanFencePool &) = delete;

    VulkanFencePool &operator=(VulkanFencePool &&) = delete;

    /**
     * @brief 请求一个 fence。
     *        优先返回池中已分配但当前未使用的 fence，否则创建新 fence。
     * @return vk::Fence 句柄。
     */
    vk::Fence RequestFence();

    /**
     * @brief 等待所有已分配的 fence 发出信号。
     * @param timeout 超时时间（纳秒），默认无限等待。
     * @return vk::Result，同 vkWaitForFences。
     */
    vk::Result Wait(uint64_t timeout = std::numeric_limits<uint64_t>::max()) const;

    /**
     * @brief 重置所有已分配的 fence 为未信号状态。
     * @return vk::Result，同 vkResetFences。
     */
    vk::Result Reset();

private:
    vk::Device               m_Device;
    std::vector<vk::Fence>   m_Fences;
    uint32_t                 m_ActiveFenceCount{0};
};

} // namespace GE
