/* Copyright (c) 2021-2025, NVIDIA CORPORATION. All rights reserved.
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
 * @file VulkanQueue.h
 * @brief Vulkan 队列封装，适配自 Vulkan-Samples 的 HPPQueue。
 *
 * 包装 vk::Queue 及其关联的队列族信息（索引、属性、present 支持），
 * 提供便捷的 Submit / Present 封装。
 */

#pragma once

#include <vulkan/vulkan.hpp>

#include <mutex>

namespace GE {

class VulkanDevice;
class VulkanCommandBuffer;

/**
 * @brief Vulkan 队列封装类。
 *
 * 持有 vk::Queue 句柄及其所属队列族的索引、属性和 present 支持信息。
 * 提供 Submit 和 Present 的便捷封装。
 */
class VulkanQueue {
public:
    VulkanQueue(VulkanDevice &device,
                uint32_t family_index,
                vk::QueueFamilyProperties const &properties,
                vk::Bool32 can_present,
                uint32_t index);

    VulkanQueue(const VulkanQueue &) = default;

    VulkanQueue(VulkanQueue &&other);

    VulkanQueue &operator=(const VulkanQueue &) = delete;

    VulkanQueue &operator=(VulkanQueue &&) = delete;

    /// 获取所属的 VulkanDevice。
    [[nodiscard]] VulkanDevice const &GetDevice() const;

    /// 获取原生 vk::Queue 句柄。
    [[nodiscard]] vk::Queue GetHandle() const;

    /// 获取队列所属的队列族索引。
    [[nodiscard]] uint32_t GetFamilyIndex() const;

    /// 获取队列在所属队列族中的索引。
    [[nodiscard]] uint32_t GetIndex() const;

    /// 获取队列族属性。
    [[nodiscard]] vk::QueueFamilyProperties const &GetProperties() const;

    /// 查询该队列是否支持 present 到 surface。
    [[nodiscard]] vk::Bool32 SupportPresent() const;

    /**
     * @brief 提交 command buffer 到队列。
     * @param command_buffer 要提交的 command buffer。
     * @param fence          submit 完成后 signal 的 fence（可为 nullptr）。
     */
    void Submit(const VulkanCommandBuffer &command_buffer, vk::Fence fence) const;

    /// @brief 对本队列的提交加互斥锁（per-queue submit 串行）。
    ///
    /// Vulkan 规范要求对同一个 VkQueue 的并发 vkQueueSubmit / vkQueueWaitIdle 等
    /// 调用必须由应用层串行（external synchronization）。每个 VulkanQueue 实例
    /// 自带一把锁，同一队列的并发提交（如后台异步上传与主线程帧提交）经此串行，
    /// 不同队列各用各的锁互不阻塞。返回的 unique_lock 在其作用域内持有锁。
    [[nodiscard]] std::unique_lock<std::mutex> LockSubmit() const {
        return std::unique_lock<std::mutex>(m_SubmitMutex);
    }

    /**
     * @brief 将图像呈现到 surface。
     * @param present_info 呈现信息。
     * @return 呈现结果。
     */
    vk::Result Present(const vk::PresentInfoKHR &present_info) const;

private:
    VulkanDevice               &m_Device;
    vk::Queue                   m_Handle;
    uint32_t                    m_FamilyIndex{0};
    uint32_t                    m_Index{0};
    vk::Bool32                  m_CanPresent = false;
    vk::QueueFamilyProperties   m_Properties{};

    /// 提交互斥锁：串行对本队列（同一 VkQueue）的 vkQueueSubmit（per-queue 外部同步）。
    mutable std::mutex          m_SubmitMutex;
};

} // namespace GE
