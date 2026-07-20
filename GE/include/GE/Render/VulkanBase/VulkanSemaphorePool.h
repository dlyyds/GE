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
 * @file VulkanSemaphorePool.h
 * @brief Vulkan Semaphore 对象池，适配自 Vulkan-Samples 的 HPPSemaphorePool。
 *
 * 管理一组 vk::Semaphore 的生命周期，支持按需分配和批量重置。
 * 适用于需要每帧提交多个 command buffer 并分别同步的场景。
 */

#pragma once

#include <vulkan/vulkan.hpp>

#include <cstdint>
#include <vector>

namespace GE {

class VulkanDevice;

/**
 * @brief Vulkan Semaphore 对象池。
 *
 * 维护一个可增长的 semaphore 池，active_semaphore_count 之前的 semaphore 视为"已分配使用"，
 * 之后的 semaphore 视为"可用"。RequestSemaphore 优先返回可用 semaphore，无可用时创建新 semaphore。
 *
 * 支持所有权转移：RequestSemaphoreWithOwnership 返回的 semaphore 由调用方负责销毁，
 * ReleaseOwnedSemaphore 可将所有权归还池中。
 */
class VulkanSemaphorePool {
public:
    explicit VulkanSemaphorePool(VulkanDevice &device);

    VulkanSemaphorePool(const VulkanSemaphorePool &) = delete;

    VulkanSemaphorePool(VulkanSemaphorePool &&) = delete;

    ~VulkanSemaphorePool();

    VulkanSemaphorePool &operator=(const VulkanSemaphorePool &) = delete;

    VulkanSemaphorePool &operator=(VulkanSemaphorePool &&) = delete;

    /**
     * @brief 请求一个 semaphore。
     *        优先返回池中已分配但当前未使用的 semaphore，否则创建新 semaphore。
     * @param debug_name 可选的调试名称，传入后在 RenderDoc 中可见。
     * @return vk::Semaphore 句柄（池内管理，调用方无需销毁）。
     */
    vk::Semaphore RequestSemaphore(const char *debug_name = nullptr);

    /**
     * @brief 请求一个 semaphore 并转移所有权给调用方。
     *        调用方负责在适当时机销毁返回的 semaphore。
     * @param debug_name 可选的调试名称，传入后在 RenderDoc 中可见。
     * @return vk::Semaphore 句柄（调用方拥有所有权）。
     */
    vk::Semaphore RequestSemaphoreWithOwnership(const char *debug_name = nullptr);

    /**
     * @brief 归还拥有所有权的 semaphore 到池中。
     * @param semaphore 之前通过 RequestSemaphoreWithOwnership 获取的 semaphore。
     */
    void ReleaseOwnedSemaphore(vk::Semaphore semaphore);

    /**
     * @brief 重置所有 semaphore 为未使用状态。
     *        标记所有 semaphore 为可用，不会销毁任何 semaphore。
     */
    void Reset();

    /** @brief 获取当前活跃的 semaphore 数量（已分配但未释放的）。 */
    uint32_t GetActiveSemaphoreCount() const { return m_ActiveSemaphoreCount; }

private:
    VulkanDevice &m_Device;

    std::vector<vk::Semaphore> m_Semaphores;       ///< 所有已创建的 semaphore
    std::vector<vk::Semaphore> m_ReleasedSemaphores; ///< 已归还所有权的 semaphore
    uint32_t m_ActiveSemaphoreCount{0};             ///< 已分配但未释放的 semaphore 计数
};

} // namespace GE