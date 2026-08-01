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
 * @file VulkanSemaphore.cpp
 * @brief RAII 风格的 vk::Semaphore 封装实现。
 */

#include "Render/VulkanBase/VulkanSemaphore.h"
#include "Render/VulkanBase/VulkanDevice.h"

#include <format>

namespace GE {

VulkanSemaphore::VulkanSemaphore(VulkanDevice &    device,
                                 vk::SemaphoreType semaphore_type,
                                 uint64_t          initial_value,
                                 const char *      debug_name)
    : VulkanResourceBase<vk::Semaphore>{nullptr, &device},
      m_Type(semaphore_type),
      m_InitialValue(initial_value) {

    vk::SemaphoreTypeCreateInfo type_create_info{
        .semaphoreType = semaphore_type,
        .initialValue  = initial_value,
    };

    vk::SemaphoreCreateInfo create_info{
        .pNext = (semaphore_type == vk::SemaphoreType::eTimeline) ? &type_create_info : nullptr,
    };

    SetHandle(device.GetHandle().createSemaphore(create_info));

    // 设置 Debug Name
    if (debug_name) {
        SetDebugName(debug_name);
    } else {
        auto name = std::format("Semaphore_{}", static_cast<void *>(GetHandle()));
        SetDebugName(name);
    }
}

VulkanSemaphore::VulkanSemaphore(VulkanSemaphore &&other) noexcept
    : VulkanResourceBase<vk::Semaphore>{std::move(other)},
      m_Type{other.m_Type},
      m_InitialValue{other.m_InitialValue} {
    other.SetHandle(nullptr);
}

VulkanSemaphore::~VulkanSemaphore() {
    if (GetHandle()) {
        GetDevice().GetHandle().destroySemaphore(GetHandle());
    }
}

uint64_t VulkanSemaphore::GetCounterValue() const {
    if (m_Type != vk::SemaphoreType::eTimeline) {
        return 0;
    }
    return GetDevice().GetHandle().getSemaphoreCounterValue(GetHandle());
}

vk::Result VulkanSemaphore::Wait(uint64_t value, uint64_t timeout_ns) const {
    vk::SemaphoreWaitInfo wait_info{
        .semaphoreCount = 1,
        .pSemaphores    = &GetHandle(),
        .pValues        = &value,
    };
    return GetDevice().GetHandle().waitSemaphores(wait_info, timeout_ns);
}

void VulkanSemaphore::Signal(uint64_t increment) const {
    uint64_t signal_value = GetCounterValue() + increment;
    vk::SemaphoreSignalInfo signal_info{
        .semaphore = GetHandle(),
        .value     = signal_value,
    };
    GetDevice().GetHandle().signalSemaphore(signal_info);
}

} // namespace GE
