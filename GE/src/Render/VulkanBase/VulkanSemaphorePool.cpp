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

namespace GE {

VulkanSemaphorePool::VulkanSemaphorePool(VulkanDevice &device) :
    m_Device(device) {
}

VulkanSemaphorePool::~VulkanSemaphorePool() {
    auto vkDevice = m_Device.GetHandle();
    for (auto sem : m_Semaphores) {
        vkDevice.destroySemaphore(sem);
    }
    m_Semaphores.clear();
    m_ReleasedSemaphores.clear();
}

vk::Semaphore VulkanSemaphorePool::RequestSemaphore() {
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
    m_Semaphores.push_back(sem);
    m_ActiveSemaphoreCount++;
    return sem;
}

vk::Semaphore VulkanSemaphorePool::RequestSemaphoreWithOwnership() {
    auto vkDevice = m_Device.GetHandle();
    return vkDevice.createSemaphore(vk::SemaphoreCreateInfo{});
}

void VulkanSemaphorePool::ReleaseOwnedSemaphore(vk::Semaphore semaphore) {
    m_ReleasedSemaphores.push_back(semaphore);
}

void VulkanSemaphorePool::Reset() {
    m_ActiveSemaphoreCount = 0;
    m_ReleasedSemaphores.clear();
}

} // namespace GE