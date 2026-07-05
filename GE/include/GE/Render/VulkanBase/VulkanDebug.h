/* Copyright (c) 2022-2026, NVIDIA CORPORATION. All rights reserved.
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
 * @file VulkanDebug.h
 * @brief Vulkan 调试工具包装，参照 Vulkan-Samples HPPDebugUtils 实现。
 *
 * 提供抽象接口 DebugUtils 及两个实现：
 * - DebugUtilsExt     — 基于 VK_EXT_debug_utils（首选）
 * - DebugMarkerExt    — 基于 VK_EXT_debug_marker（fallback）
 * - DummyDebugUtils   — 空实现（调试扩展不可用时）
 * - ScopedDebugLabel  — RAII 调试标签
 */

#pragma once

#include <vulkan/vulkan.hpp>

#include <glm/glm.hpp>

#include <string>

namespace GE {

/**
 * @brief 平台相关调试扩展的抽象接口。
 */
class DebugUtils
{
  public:
    virtual ~DebugUtils() = default;

    /// 为 Vulkan 对象设置调试名称。
    virtual void SetDebugName(vk::Device device, vk::ObjectType object_type, uint64_t object_handle, const char *name) const = 0;

    /// 为 Vulkan 对象附加调试标签数据。
    virtual void SetDebugTag(
        vk::Device device, vk::ObjectType object_type, uint64_t object_handle, uint64_t tag_name, const void *tag_data, size_t tag_data_size) const = 0;

    /// 在命令缓冲区中插入调试标签区域的开始标记。
    virtual void CmdBeginLabel(vk::CommandBuffer command_buffer, const char *name, glm::vec4 const color = {}) const = 0;

    /// 在命令缓冲区中结束当前调试标签区域。
    virtual void CmdEndLabel(vk::CommandBuffer command_buffer) const = 0;

    /// 在命令缓冲区中插入非作用域调试标签。
    virtual void CmdInsertLabel(vk::CommandBuffer command_buffer, const char *name, glm::vec4 const color = {}) const = 0;
};

/**
 * @brief 基于 VK_EXT_debug_utils 实现的 DebugUtils。
 */
class DebugUtilsExt final : public DebugUtils
{
  public:
    ~DebugUtilsExt() override = default;

    void SetDebugName(vk::Device device, vk::ObjectType object_type, uint64_t object_handle, const char *name) const override;

    void SetDebugTag(
        vk::Device device, vk::ObjectType object_type, uint64_t object_handle, uint64_t tag_name, const void *tag_data, size_t tag_data_size) const override;

    void CmdBeginLabel(vk::CommandBuffer command_buffer, const char *name, glm::vec4 const color) const override;

    void CmdEndLabel(vk::CommandBuffer command_buffer) const override;

    void CmdInsertLabel(vk::CommandBuffer command_buffer, const char *name, glm::vec4 const color) const override;
};

/**
 * @brief 基于 VK_EXT_debug_marker 实现的 DebugUtils。
 */
class DebugMarkerExt final : public DebugUtils
{
  public:
    ~DebugMarkerExt() override = default;

    void SetDebugName(vk::Device device, vk::ObjectType object_type, uint64_t object_handle, const char *name) const override;

    void SetDebugTag(
        vk::Device device, vk::ObjectType object_type, uint64_t object_handle, uint64_t tag_name, const void *tag_data, size_t tag_data_size) const override;

    void CmdBeginLabel(vk::CommandBuffer command_buffer, const char *name, glm::vec4 const color) const override;

    void CmdEndLabel(vk::CommandBuffer command_buffer) const override;

    void CmdInsertLabel(vk::CommandBuffer command_buffer, const char *name, glm::vec4 const color) const override;
};

/**
 * @brief 空实现的 DebugUtils（调试扩展不可用时使用）。
 */
class DummyDebugUtils final : public DebugUtils
{
  public:
    ~DummyDebugUtils() override = default;

    inline void SetDebugName(vk::Device, vk::ObjectType, uint64_t, const char *) const override
    {}

    inline void SetDebugTag(vk::Device, vk::ObjectType, uint64_t, uint64_t, const void *, size_t) const override
    {}

    inline void CmdBeginLabel(vk::CommandBuffer, const char *, glm::vec4 const) const override
    {}

    inline void CmdEndLabel(vk::CommandBuffer) const override
    {}

    inline void CmdInsertLabel(vk::CommandBuffer, const char *, glm::vec4 const) const override
    {}
};

/**
 * @brief RAII 调试标签。
 *
 * 构造时开始调试标签/标记，析构时自动结束。
 * 需同时传入 DebugUtils 实现和命令缓冲区。
 */
class ScopedDebugLabel final
{
  public:
    ScopedDebugLabel(const DebugUtils &debug_utils, vk::CommandBuffer command_buffer, std::string const &name, glm::vec4 const color = {});

    ~ScopedDebugLabel();

    ScopedDebugLabel(const ScopedDebugLabel &) = delete;
    ScopedDebugLabel &operator=(const ScopedDebugLabel &) = delete;

  private:
    const DebugUtils *m_DebugUtils  = nullptr;
    vk::CommandBuffer  m_CommandBuffer = nullptr;
};

/// Debug 回调函数（VK_EXT_debug_utils 验证层消息回调）。
#if defined(VK_DEBUG) || defined(VK_VALIDATION_LAYERS)
VKAPI_ATTR vk::Bool32 VKAPI_CALL DebugUtilsMessengerCallback(
    vk::DebugUtilsMessageSeverityFlagBitsEXT      message_severity,
    vk::DebugUtilsMessageTypeFlagsEXT             message_type,
    vk::DebugUtilsMessengerCallbackDataEXT const *callback_data,
    void                                         *user_data);
#endif

} // namespace GE
