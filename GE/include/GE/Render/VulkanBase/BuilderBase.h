/* Copyright (c) 2021-2026, NVIDIA CORPORATION. All rights reserved.
 * Copyright (c) 2025-2026, Bradley Austin Davis. All rights reserved.
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
 * @file BuilderBase.h
 * @brief 从 Vulkan-Samples 适配的 Builder 基类（去掉 BindingType）。
 *
 * 为 VMA 管理资源（Image / Buffer）提供 Builder 模式支持。
 * GE 统一使用 vulkan.hpp C++ 类型，因此不再需要 BindingType 模板参数。
 */

#pragma once

#include <vulkan/vulkan.hpp>
#include <vk_mem_alloc.h>

#include <cstdint>
#include <string>
#include <vector>

namespace GE
{
namespace allocated
{
/**
 * @brief Builder 模式基类，管理 VMA 分配参数和 Vulkan 创建信息。
 *
 * 派生类（如 HPPImageBuilder）通过链式调用设置参数，最后调用 build() 创建资源。
 *
 * @tparam BuilderType    CRTP：派生类自身类型。
 * @tparam CreateInfoType Vulkan 创建信息类型（如 vk::ImageCreateInfo）。
 */
template <typename BuilderType, typename CreateInfoType>
class BuilderBase
{
  public:
	VmaAllocationCreateInfo const &get_allocation_create_info() const;
	CreateInfoType const          &get_create_info() const;
	std::string const             &get_debug_name() const;

	BuilderType &with_debug_name(const std::string &name);
	BuilderType &with_implicit_sharing_mode();
	BuilderType &with_memory_type_bits(uint32_t type_bits);
	BuilderType &with_queue_families(uint32_t count, const uint32_t *family_indices);
	BuilderType &with_queue_families(std::vector<uint32_t> const &queue_families);
	BuilderType &with_sharing_mode(vk::SharingMode sharing_mode);
	BuilderType &with_vma_flags(VmaAllocationCreateFlags flags);
	BuilderType &with_vma_pool(VmaPool pool);
	BuilderType &with_vma_preferred_flags(VkMemoryPropertyFlags flags);
	BuilderType &with_vma_required_flags(VkMemoryPropertyFlags flags);
	BuilderType &with_vma_usage(VmaMemoryUsage usage);

  protected:
	BuilderBase(const BuilderBase &other) = delete;
	BuilderBase(const CreateInfoType &create_info);

	CreateInfoType &get_create_info();

  protected:
	VmaAllocationCreateInfo alloc_create_info = {};
	CreateInfoType          create_info       = {};
	std::string             debug_name        = {};
};

// ============================================================================
// 模板方法实现
// ============================================================================

template <typename BuilderType, typename CreateInfoType>
inline BuilderBase<BuilderType, CreateInfoType>::BuilderBase(const CreateInfoType &create_info_) :
    create_info(create_info_)
{
	alloc_create_info.usage = VMA_MEMORY_USAGE_AUTO;
}

template <typename BuilderType, typename CreateInfoType>
inline VmaAllocationCreateInfo const &BuilderBase<BuilderType, CreateInfoType>::get_allocation_create_info() const
{
	return alloc_create_info;
}

template <typename BuilderType, typename CreateInfoType>
inline CreateInfoType const &BuilderBase<BuilderType, CreateInfoType>::get_create_info() const
{
	return create_info;
}

template <typename BuilderType, typename CreateInfoType>
inline CreateInfoType &BuilderBase<BuilderType, CreateInfoType>::get_create_info()
{
	return create_info;
}

template <typename BuilderType, typename CreateInfoType>
inline std::string const &BuilderBase<BuilderType, CreateInfoType>::get_debug_name() const
{
	return debug_name;
}

template <typename BuilderType, typename CreateInfoType>
inline BuilderType &BuilderBase<BuilderType, CreateInfoType>::with_debug_name(const std::string &name)
{
	debug_name = name;
	return *static_cast<BuilderType *>(this);
}

template <typename BuilderType, typename CreateInfoType>
inline BuilderType &BuilderBase<BuilderType, CreateInfoType>::with_implicit_sharing_mode()
{
	create_info.sharingMode = (1 < create_info.queueFamilyIndexCount) ? vk::SharingMode::eConcurrent : vk::SharingMode::eExclusive;
	return *static_cast<BuilderType *>(this);
}

template <typename BuilderType, typename CreateInfoType>
inline BuilderType &BuilderBase<BuilderType, CreateInfoType>::with_memory_type_bits(uint32_t type_bits)
{
	alloc_create_info.memoryTypeBits = type_bits;
	return *static_cast<BuilderType *>(this);
}

template <typename BuilderType, typename CreateInfoType>
inline BuilderType &BuilderBase<BuilderType, CreateInfoType>::with_queue_families(uint32_t count, const uint32_t *family_indices)
{
	create_info.queueFamilyIndexCount = count;
	create_info.pQueueFamilyIndices   = family_indices;
	return *static_cast<BuilderType *>(this);
}

template <typename BuilderType, typename CreateInfoType>
inline BuilderType &BuilderBase<BuilderType, CreateInfoType>::with_queue_families(std::vector<uint32_t> const &queue_families)
{
	return with_queue_families(static_cast<uint32_t>(queue_families.size()), queue_families.data());
}

template <typename BuilderType, typename CreateInfoType>
inline BuilderType &BuilderBase<BuilderType, CreateInfoType>::with_sharing_mode(vk::SharingMode sharing_mode)
{
	create_info.sharingMode = sharing_mode;
	return *static_cast<BuilderType *>(this);
}

template <typename BuilderType, typename CreateInfoType>
inline BuilderType &BuilderBase<BuilderType, CreateInfoType>::with_vma_flags(VmaAllocationCreateFlags flags)
{
	alloc_create_info.flags = flags;
	return *static_cast<BuilderType *>(this);
}

template <typename BuilderType, typename CreateInfoType>
inline BuilderType &BuilderBase<BuilderType, CreateInfoType>::with_vma_pool(VmaPool pool)
{
	alloc_create_info.pool = pool;
	return *static_cast<BuilderType *>(this);
}

template <typename BuilderType, typename CreateInfoType>
inline BuilderType &BuilderBase<BuilderType, CreateInfoType>::with_vma_preferred_flags(VkMemoryPropertyFlags flags)
{
	alloc_create_info.preferredFlags = flags;
	return *static_cast<BuilderType *>(this);
}

template <typename BuilderType, typename CreateInfoType>
inline BuilderType &BuilderBase<BuilderType, CreateInfoType>::with_vma_required_flags(VkMemoryPropertyFlags flags)
{
	alloc_create_info.requiredFlags = flags;
	return *static_cast<BuilderType *>(this);
}

template <typename BuilderType, typename CreateInfoType>
inline BuilderType &BuilderBase<BuilderType, CreateInfoType>::with_vma_usage(VmaMemoryUsage usage)
{
	alloc_create_info.usage = usage;
	return *static_cast<BuilderType *>(this);
}

}        // namespace allocated
}        // namespace GE
