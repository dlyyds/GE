/* Copyright (c) 2021-2025, Arm Limited and Contributors
 * Copyright (c) 2024-2025, NVIDIA CORPORATION. All rights reserved.
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
 * @file VulkanResourceBase.h
 * @brief Vulkan 资源基类，从 Vulkan-Samples 的 VulkanResource 适配而来。
 *
 * 作为任何 Vulkan 对象（Image、Buffer、Sampler 等）的基类，
 * 存储 Vulkan 句柄、关联的 VulkanDevice 引用，
 * 并支持调试命名（debug name）。
 *
 * GE 统一使用 vulkan.hpp C++ 风格句柄，因此不再需要 BindingType 模板参数。
 */

#pragma once

#include "Render/VulkanBase/VulkanDebug.h"

#include <string>
#include <utility>
#include <vulkan/vulkan.hpp>

namespace GE {

class VulkanDevice;

/**
 * @brief 作为任何带有句柄的 Vulkan 对象的基类。
 *
 * 派生类可以存储 Vulkan 句柄，同时持有父 VulkanDevice 的指针。
 * 支持为任何 Vulkan 对象设置调试名称。
 */
template <typename Handle>
class VulkanResourceBase {
public:
    VulkanResourceBase(Handle handle = nullptr, VulkanDevice *device = nullptr);

    VulkanResourceBase(const VulkanResourceBase &) = delete;

    VulkanResourceBase &operator=(const VulkanResourceBase &) = delete;

    VulkanResourceBase(VulkanResourceBase &&other);

    VulkanResourceBase &operator=(VulkanResourceBase &&other);

    virtual ~VulkanResourceBase() = default;

    const std::string &GetDebugName() const;

    VulkanDevice &GetDevice();

    [[nodiscard]] VulkanDevice const &GetDevice() const;

    Handle &GetHandle();

    Handle const &GetHandle() const;

    /// 将 Vulkan 句柄转为 uint64_t（用于 DebugUtils 等需要 uint64_t 句柄的 API）。
    uint64_t GetHandleU64() const;

    /// 获取 Vulkan 对象类型枚举（如 vk::ObjectType::eImage）。
    vk::ObjectType GetObjectType() const;

    bool HasDevice() const;

    bool HasHandle() const;

    /**
     * @brief 设置调试名称。
     *
     * 若 device 和 debug_utils 均有效，则立即调用 DebugUtils::SetDebugName。
     */
    void SetDebugName(const std::string &name);

    void SetHandle(Handle hdl);

private:
    std::string debug_name;
    VulkanDevice *device = nullptr;
    Handle handle{nullptr};
};

// ==================================================================
// 模板实现（全部内联在头文件中，因 VulkanDevice 仅前向声明，
// SetDebugName / GetDevice 的实现由包含 VulkanDevice.h 的 TU 实例化）
// ==================================================================

template <typename Handle>
inline VulkanResourceBase<Handle>::VulkanResourceBase(Handle handle_, VulkanDevice *device_) : handle(handle_),
                                                                                               device(device_) {
}

template <typename Handle>
inline VulkanResourceBase<Handle>::VulkanResourceBase(VulkanResourceBase &&other) : handle(std::exchange(other.handle, {})),
                                                                                    device(std::exchange(other.device, {})),
                                                                                    debug_name(std::exchange(other.debug_name, {})) {
}

template <typename Handle>
inline VulkanResourceBase<Handle> &VulkanResourceBase<Handle>::operator=(VulkanResourceBase &&other) {
    handle = std::exchange(other.handle, {});
    device = std::exchange(other.device, {});
    debug_name = std::exchange(other.debug_name, {});
    return *this;
}

template <typename Handle>
inline const std::string &VulkanResourceBase<Handle>::GetDebugName() const {
    return debug_name;
}

template <typename Handle>
inline VulkanDevice &VulkanResourceBase<Handle>::GetDevice() {
    return *device;
}

template <typename Handle>
inline VulkanDevice const &VulkanResourceBase<Handle>::GetDevice() const {
    return *device;
}

template <typename Handle>
inline Handle &VulkanResourceBase<Handle>::GetHandle() {
    return handle;
}

template <typename Handle>
inline Handle const &VulkanResourceBase<Handle>::GetHandle() const {
    return handle;
}

template <typename Handle>
inline uint64_t VulkanResourceBase<Handle>::GetHandleU64() const {
    // vulkan.hpp 句柄类型不保证与 uint64_t 二进制兼容（32 位平台上非调度句柄可能只有 32 位），
    // 因此在编译时检查大小后，通过 reinterpret_cast 读取句柄的原始内存表示。
    using UintHandle = typename std::conditional<sizeof(Handle) == sizeof(uint32_t), uint32_t, uint64_t>::type;
    return static_cast<uint64_t>(*reinterpret_cast<UintHandle const *>(&handle));
}

template <typename Handle>
inline vk::ObjectType VulkanResourceBase<Handle>::GetObjectType() const {
    // vulkan.hpp 中每个句柄类型（如 vk::Image）都有静态成员 objectType
    return Handle::objectType;
}

template <typename Handle>
inline bool VulkanResourceBase<Handle>::HasDevice() const {
    return device != nullptr;
}

template <typename Handle>
inline bool VulkanResourceBase<Handle>::HasHandle() const {
    return handle != Handle{};
}

template <typename Handle>
inline void VulkanResourceBase<Handle>::SetDebugName(const std::string &name) {
    debug_name = name;

    if (device && !debug_name.empty()) {
        device->GetDebugUtils().SetDebugName(
            device->GetHandle(),
            GetObjectType(),
            GetHandleU64(),
            debug_name.c_str());
    }
}

template <typename Handle>
inline void VulkanResourceBase<Handle>::SetHandle(Handle hdl) {
    handle = hdl;
}

} // namespace GE
