/* Copyright (c) 2022-2025, NVIDIA CORPORATION. All rights reserved.
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

#include "Render/VulkanBase/VulkanQueue.h"
#include "Render/VulkanBase/VulkanDevice.h"
#include "Render/VulkanBase/VulkanCommandBuffer.h"

#include <utility>

namespace GE {

VulkanQueue::VulkanQueue(VulkanDevice &device,
                         uint32_t family_index,
                         vk::QueueFamilyProperties const &properties,
                         vk::Bool32 can_present,
                         uint32_t index) :
    m_Device{device},
    m_FamilyIndex{family_index},
    m_Index{index},
    m_CanPresent{can_present},
    m_Properties{properties} {
    // 构造函数中从设备获取队列句柄
    m_Handle = m_Device.GetHandle().getQueue(family_index, index);
}

VulkanQueue::VulkanQueue(VulkanQueue &&other) :
    m_Device(other.m_Device),
    m_Handle(std::exchange(other.m_Handle, {})),
    m_FamilyIndex(std::exchange(other.m_FamilyIndex, {})),
    m_Index(std::exchange(other.m_Index, 0)),
    m_CanPresent(std::exchange(other.m_CanPresent, false)),
    m_Properties(std::exchange(other.m_Properties, {})) {
}

VulkanDevice const &VulkanQueue::GetDevice() const {
    return m_Device;
}

vk::Queue VulkanQueue::GetHandle() const {
    return m_Handle;
}

uint32_t VulkanQueue::GetFamilyIndex() const {
    return m_FamilyIndex;
}

uint32_t VulkanQueue::GetIndex() const {
    return m_Index;
}

vk::QueueFamilyProperties const &VulkanQueue::GetProperties() const {
    return m_Properties;
}

vk::Bool32 VulkanQueue::SupportPresent() const {
    return m_CanPresent;
}

void VulkanQueue::Submit(const VulkanCommandBuffer &command_buffer, vk::Fence fence) const {
    vk::CommandBuffer cmd_handle = command_buffer.GetHandle();
    vk::SubmitInfo submit_info{
        .commandBufferCount = 1,
        .pCommandBuffers = &cmd_handle,
    };
    m_Handle.submit(submit_info, fence);
}

vk::Result VulkanQueue::Present(const vk::PresentInfoKHR &present_info) const {
    if (!m_CanPresent) {
        return vk::Result::eErrorIncompatibleDisplayKHR;
    }
    return m_Handle.presentKHR(present_info);
}

} // namespace GE
