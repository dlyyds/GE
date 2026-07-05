/* Copyright (c) 2023-2025, NVIDIA CORPORATION. All rights reserved.
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
 * @file VulkanDebug.cpp
 * @brief VulkanDebug 类的实现。
 */

#include "Render/VulkanBase/VulkanDebug.h"

#include <cassert>

namespace GE {

// ============================================================================
// DebugUtilsExt — 基于 VK_EXT_debug_utils
// ============================================================================

void DebugUtilsExt::SetDebugName(vk::Device device, vk::ObjectType object_type, uint64_t object_handle, const char *name) const
{
    vk::DebugUtilsObjectNameInfoEXT name_info{
        .objectType   = object_type,
        .objectHandle = object_handle,
        .pObjectName  = name,
    };
    device.setDebugUtilsObjectNameEXT(name_info);
}

void DebugUtilsExt::SetDebugTag(
    vk::Device device, vk::ObjectType object_type, uint64_t object_handle, uint64_t tag_name, const void *tag_data, size_t tag_data_size) const
{
    vk::DebugUtilsObjectTagInfoEXT tag_info{
        .objectType   = object_type,
        .objectHandle = object_handle,
        .tagName      = tag_name,
        .tagSize      = tag_data_size,
        .pTag         = tag_data,
    };
    device.setDebugUtilsObjectTagEXT(tag_info);
}

void DebugUtilsExt::CmdBeginLabel(vk::CommandBuffer command_buffer, const char *name, glm::vec4 const color) const
{
    vk::DebugUtilsLabelEXT label_info{
        .pLabelName = name,
        .color      = reinterpret_cast<std::array<float, 4> const &>(*&color[0]),
    };
    command_buffer.beginDebugUtilsLabelEXT(label_info);
}

void DebugUtilsExt::CmdEndLabel(vk::CommandBuffer command_buffer) const
{
    command_buffer.endDebugUtilsLabelEXT();
}

void DebugUtilsExt::CmdInsertLabel(vk::CommandBuffer command_buffer, const char *name, glm::vec4 const color) const
{
    vk::DebugUtilsLabelEXT label_info{
        .pLabelName = name,
        .color      = reinterpret_cast<std::array<float, 4> const &>(*&color[0]),
    };
    command_buffer.insertDebugUtilsLabelEXT(label_info);
}

// ============================================================================
// DebugMarkerExt — 基于 VK_EXT_debug_marker
// ============================================================================

void DebugMarkerExt::SetDebugName(vk::Device device, vk::ObjectType object_type, uint64_t object_handle, const char *name) const
{
    vk::DebugMarkerObjectNameInfoEXT name_info{
        .objectType   = vk::debugReportObjectType(object_type),
        .object       = object_handle,
        .pObjectName  = name,
    };
    device.debugMarkerSetObjectNameEXT(name_info);
}

void DebugMarkerExt::SetDebugTag(
    vk::Device device, vk::ObjectType object_type, uint64_t object_handle, uint64_t tag_name, const void *tag_data, size_t tag_data_size) const
{
    vk::DebugMarkerObjectTagInfoEXT tag_info{
        .objectType = vk::debugReportObjectType(object_type),
        .object     = object_handle,
        .tagName    = tag_name,
        .tagSize    = tag_data_size,
        .pTag       = tag_data,
    };
    device.debugMarkerSetObjectTagEXT(tag_info);
}

void DebugMarkerExt::CmdBeginLabel(vk::CommandBuffer command_buffer, const char *name, glm::vec4 const color) const
{
    vk::DebugMarkerMarkerInfoEXT marker_info{
        .pMarkerName = name,
        .color       = reinterpret_cast<std::array<float, 4> const &>(*&color[0]),
    };
    command_buffer.debugMarkerBeginEXT(marker_info);
}

void DebugMarkerExt::CmdEndLabel(vk::CommandBuffer command_buffer) const
{
    command_buffer.debugMarkerEndEXT();
}

void DebugMarkerExt::CmdInsertLabel(vk::CommandBuffer command_buffer, const char *name, glm::vec4 const color) const
{
    vk::DebugMarkerMarkerInfoEXT marker_info{
        .pMarkerName = name,
        .color       = reinterpret_cast<std::array<float, 4> const &>(*&color[0]),
    };
    command_buffer.debugMarkerInsertEXT(marker_info);
}

// ============================================================================
// ScopedDebugLabel — RAII 调试标签
// ============================================================================

ScopedDebugLabel::ScopedDebugLabel(const DebugUtils &debug_utils, vk::CommandBuffer command_buffer, std::string const &name, glm::vec4 const color) :
    m_DebugUtils{&debug_utils},
    m_CommandBuffer{nullptr}
{
    if (!name.empty())
    {
        assert(command_buffer);
        m_CommandBuffer = command_buffer;

        m_DebugUtils->CmdBeginLabel(command_buffer, name.c_str(), color);
    }
}

ScopedDebugLabel::~ScopedDebugLabel()
{
    if (m_CommandBuffer)
    {
        m_DebugUtils->CmdEndLabel(m_CommandBuffer);
    }
}

} // namespace GE
