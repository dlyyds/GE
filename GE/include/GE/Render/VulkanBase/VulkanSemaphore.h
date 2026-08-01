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
 * @file VulkanSemaphore.h
 * @brief RAII 风格的 vk::Semaphore 封装。
 *
 * 构造时创建 Semaphore，析构时自动销毁。
 * 支持 timeline semaphore（通过 VkSemaphoreTypeCreateInfo 指定类型）。
 * 对于简单的 binary semaphore 场景（如每帧提交同步），可直接使用默认构造。
 *
 * 与 VulkanSemaphorePool 的区别：
 *   - VulkanSemaphore 是单个 semaphore 的 RAII 封装，生命周期由对象自身管理。
 *   - VulkanSemaphorePool 是对象池，适合每帧大量分配/释放的短期 semaphore。
 */

#pragma once

#include "Render/VulkanBase/VulkanResourceBase.h"
#include <Render/VulkanBase/VulkanDevice.h>

#include <vulkan/vulkan.hpp>

namespace GE {

/**
 * @brief RAII 风格的 vk::Semaphore 封装。
 *
 * 继承 VulkanResourceBase<vk::Semaphore>，构造时创建 Semaphore，
 * 析构时自动销毁。支持移动语义。
 *
 * 默认创建 binary semaphore；若需要 timeline semaphore，
 * 传入 vk::SemaphoreType::eTimeline 及初始值。
 */
class VulkanSemaphore : public VulkanResourceBase<vk::Semaphore> {
public:
    /**
     * @brief 构造 VulkanSemaphore。
     *
     * @param device          Vulkan 设备引用
     * @param semaphore_type  信号量类型（binary 或 timeline）
     * @param initial_value   初始值（仅 timeline semaphore 有效）
     * @param debug_name      可选的调试名称，传入后在 RenderDoc 中可见
     */
    explicit VulkanSemaphore(VulkanDevice &          device,
                             vk::SemaphoreType       semaphore_type = vk::SemaphoreType::eBinary,
                             uint64_t                initial_value  = 0,
                             const char *            debug_name     = nullptr);

    VulkanSemaphore(const VulkanSemaphore &) = delete;

    VulkanSemaphore(VulkanSemaphore &&other) noexcept;

    ~VulkanSemaphore() override;

    VulkanSemaphore &operator=(const VulkanSemaphore &) = delete;

    VulkanSemaphore &operator=(VulkanSemaphore &&other) noexcept;

    /**
     * @brief 获取信号量类型。
     */
    vk::SemaphoreType GetType() const { return m_Type; }

    /**
     * @brief 获取 timeline semaphore 的当前值。
     * @return 当前计数器值；binary semaphore 始终返回 0。
     */
    uint64_t GetCounterValue() const;

    /**
     * @brief 等待 timeline semaphore 达到指定值。
     * @param value        要等待的目标值
     * @param timeout_ns   超时时间（纳秒），默认 UINT64_MAX 表示无限等待
     * @return vk::Result  等待结果
     */
    vk::Result Wait(uint64_t value, uint64_t timeout_ns = UINT64_MAX) const;

    /**
     * @brief （仅 timeline semaphore）信号量值加 1。
     * @param increment  增加的量，默认 1
     */
    void Signal(uint64_t increment = 1) const;

private:
    vk::SemaphoreType m_Type{vk::SemaphoreType::eBinary};
    uint64_t          m_InitialValue{0};
};

} // namespace GE
