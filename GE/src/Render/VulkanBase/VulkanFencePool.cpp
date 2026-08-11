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

#include "Render/VulkanBase/VulkanFencePool.h"

#include "Debug/Profiler.h"

#include <stdexcept>

namespace GE {

VulkanFencePool::VulkanFencePool(vk::Device device) :
    m_Device{device} {
}

VulkanFencePool::~VulkanFencePool() {
    Wait();
    Reset();

    // 销毁所有 fence
    for (auto fence : m_Fences) {
        m_Device.destroyFence(fence);
    }
    m_Fences.clear();
}

vk::Fence VulkanFencePool::RequestFence() {
    GE_PROFILE_FUNCTION();
    // 优先返回池中已分配但当前未使用的 fence
    if (m_ActiveFenceCount < m_Fences.size()) {
        return m_Fences[m_ActiveFenceCount++];
    }

    vk::FenceCreateInfo create_info{};

    auto fence = m_Device.createFence(create_info);

    m_Fences.push_back(fence);
    m_ActiveFenceCount++;

    return m_Fences.back();
}

vk::Result VulkanFencePool::Wait(uint64_t timeout) const {
    GE_PROFILE_FUNCTION();
    if (m_ActiveFenceCount < 1 || m_Fences.empty()) {
        return vk::Result::eSuccess;
    }

    return m_Device.waitForFences(m_ActiveFenceCount, m_Fences.data(), VK_TRUE, timeout);
}

vk::Result VulkanFencePool::Reset() {
    GE_PROFILE_FUNCTION();
    if (m_ActiveFenceCount < 1 || m_Fences.empty()) {
        return vk::Result::eSuccess;
    }

    vk::Result result = m_Device.resetFences(m_ActiveFenceCount, m_Fences.data());

    if (result != vk::Result::eSuccess) {
        return result;
    }

    m_ActiveFenceCount = 0;

    return vk::Result::eSuccess;
}

} // namespace GE
