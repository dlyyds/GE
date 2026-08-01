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

#include "Render/VulkanBase/VulkanSemaphorePool.h"
#include "Render/VulkanBase/VulkanDevice.h"
#include "Render/VulkanBase/VulkanDebug.h"

#include <format>

namespace GE {

VulkanSemaphorePool::VulkanSemaphorePool(VulkanDevice &device) : m_Device(device) {
}

VulkanSemaphorePool::~VulkanSemaphorePool() {
    Reset();
    auto vkDevice = m_Device.GetHandle();
    for (auto sem : m_Semaphores) {
        vkDevice.destroySemaphore(sem);
    }

    m_Semaphores.clear();
    m_ReleasedSemaphores.clear();
}

vk::Semaphore VulkanSemaphorePool::RequestSemaphore(const char *debug_name) {
    // 检查是否有已释放的 semaphore（所有权已归还）
    if (!m_ReleasedSemaphores.empty()) {
        auto sem = m_ReleasedSemaphores.back();
        m_ReleasedSemaphores.pop_back();
        return sem;
    }

    // 检查是否有未使用的已分配 semaphore
    if (m_ActiveSemaphoreCount < m_Semaphores.size()) {
        return m_Semaphores[m_ActiveSemaphoreCount++];
    }

    // 创建新 semaphore
    auto vkDevice = m_Device.GetHandle();
    auto sem = vkDevice.createSemaphore(vk::SemaphoreCreateInfo{});

    // 设置 Debug Name
    if (debug_name) {
        m_Device.GetDebugUtils().SetDebugName(
            vkDevice, vk::ObjectType::eSemaphore, (uint64_t)static_cast<VkSemaphore>(sem), debug_name);
    } else {
        auto name = std::format("SemaphorePool_{}", m_Semaphores.size());
        m_Device.GetDebugUtils().SetDebugName(
            vkDevice, vk::ObjectType::eSemaphore, (uint64_t)static_cast<VkSemaphore>(sem), name.c_str());
    }

    m_Semaphores.push_back(sem);
    m_ActiveSemaphoreCount++;
    return sem;
}

vk::Semaphore VulkanSemaphorePool::RequestSemaphoreWithOwnership(const char *debug_name) {
    auto vkDevice = m_Device.GetHandle();

    // 优先复用已归还所有权的 semaphore，避免每帧新建导致泄漏
    if (!m_ReleasedSemaphores.empty()) {
        auto sem = m_ReleasedSemaphores.back();
        m_ReleasedSemaphores.pop_back();
        return sem;
    }

    // 没有可复用的，新建一个
    auto sem = vkDevice.createSemaphore(vk::SemaphoreCreateInfo{});

    // 设置 Debug Name
    if (debug_name) {
        m_Device.GetDebugUtils().SetDebugName(
            vkDevice, vk::ObjectType::eSemaphore, (uint64_t)static_cast<VkSemaphore>(sem), debug_name);
    } else {
        auto name = std::format("SemaphoreOwned_{}", m_Semaphores.size());
        m_Device.GetDebugUtils().SetDebugName(
            vkDevice, vk::ObjectType::eSemaphore, (uint64_t)static_cast<VkSemaphore>(sem), name.c_str());
    }

    return sem;
}

void VulkanSemaphorePool::ReleaseOwnedSemaphore(vk::Semaphore semaphore) {
    m_ReleasedSemaphores.push_back(semaphore);
}

void VulkanSemaphorePool::Reset() {
    m_ActiveSemaphoreCount = 0;

    // 将已归还所有权的 semaphore 回收回可用池中
    for (auto &sem : m_ReleasedSemaphores) {
        m_Semaphores.push_back(sem);
    }
    m_ReleasedSemaphores.clear();
}

} // namespace GE