/* Copyright (c) 2021-2026, NVIDIA CORPORATION. All rights reserved.
 * Copyright (c) 2024-2026, Bradley Austin Davis. All rights reserved.
 * Copyright (c) 2025-2026, Arm Limited and Contributors
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
 * @file VulkanAllocated.h
 * @brief 从 Vulkan-Samples 适配的 VMA 内存分配基类。
 *
 * 提供 Allocated RAII 基类，为 Vulkan Image / Buffer 资源提供自动内存管理。
 * VMA 分配器由 VulkanDevice 创建和管理，通过 this->GetDevice().GetVmaAllocator() 获取。
 *
 * GE 统一使用 vulkan.hpp C++ 风格句柄，因此移除了原版的 BindingType 模板参数。
 */

#pragma once

#include "Render/VulkanBase/VulkanCommon.h"
#include "Render/VulkanBase/VulkanDevice.h"
#include "Render/VulkanBase/VulkanResourceBase.h"

#include <vulkan/vulkan.hpp>
#include <vk_mem_alloc.h>

#include <array>
#include <cstdint>
#include <cstring>
#include <vector>

namespace GE
{
namespace allocated
{

/**
 * @brief VMA 内存分配 RAII 基类，为 VkImage 和 VkBuffer 等需要内存分配的
 *        Vulkan 资源提供自动内存管理。
 *
 * 继承自 VulkanResourceBase<HandleType>，支持：
 * - 移动语义（禁止拷贝）
 * - 内存映射（map / unmap）
 * - 数据上传（update / updateTyped）
 * - flush 同步（非 HOST_COHERENT 内存）
 *
 * VMA 分配器通过 this->GetDevice().GetVmaAllocator() 获取。
 *
 * @tparam HandleType Vulkan 句柄类型，如 vk::Buffer 或 vk::Image。
 */
template <typename HandleType>
class Allocated : public VulkanResourceBase<HandleType>
{
  public:
	using ParentType = VulkanResourceBase<HandleType>;

  public:
	Allocated()                              = delete;
	Allocated(const Allocated &)             = delete;
	Allocated(Allocated &&other) noexcept;
	Allocated &operator=(Allocated const &other) = delete;
	Allocated &operator=(Allocated &&other)      = default;

  protected:
	/**
	 * @brief VMA 构造函数，用于创建新资源。
	 * @param allocation_create_info VMA 内存分配配置。
	 * @param device                 Vulkan 设备指针。
	 */
	Allocated(const VmaAllocationCreateInfo &allocation_create_info, VulkanDevice *device);

	/**
	 * @brief 句柄包装构造函数，用于包装已存在的资源（如 swapchain images）。
	 * @param handle 已有的 Vulkan 句柄。
	 * @param device Vulkan 设备指针（可选）。
	 */
	Allocated(HandleType handle, VulkanDevice *device = nullptr);

  public:
	/**
	 * @brief 获取 Vulkan 句柄指针。
	 */
	const HandleType *get() const;

	/**
	 * @brief 如果内存非 HOST_COHERENT，则 flush 内存。
	 *        HOST_COHERENT 内存下为 no-op。
	 * @param offset flush 起始偏移，默认 0。
	 * @param size   flush 大小，默认 VK_WHOLE_SIZE。
	 */
	void flush(VkDeviceSize offset = 0, VkDeviceSize size = VK_WHOLE_SIZE);

	/**
	 * @brief 获取 mapped 数据的只读指针。
	 * @note 不执行映射操作，仅返回当前 mapped_data。
	 */
	const uint8_t *get_data() const;

	/**
	 * @brief 获取底层 VkDeviceMemory。
	 */
	vk::DeviceMemory get_memory() const;

	/**
	 * @brief 获取在 VkDeviceMemory 中的偏移量。
	 */
	VkDeviceSize get_memory_offset() const;

	/**
	 * @brief 映射内存（如果尚未映射）。
	 * @return 指向 host-visible 内存的指针。
	 */
	uint8_t *map();

	/**
	 * @brief 检查内存是否已映射。
	 */
	bool mapped() const;

	/**
	 * @brief 取消映射内存。
	 *         对于 persistent 映射不做任何操作。
	 */
	void unmap();

	/**
	 * @brief 将字节数据复制到 mapped 内存区域。
	 * @note 非 persistent 映射会临时 map / unmap。
	 * @return 复制的字节数。
	 */
	size_t update(const uint8_t *data, size_t size, size_t offset = 0);

	/**
	 * @brief 将任意数据复制到 mapped 内存区域（void 指针重载）。
	 */
	size_t update(void const *data, size_t size, size_t offset = 0);

	/**
	 * @brief 上传 vector 数据到 mapped 内存。
	 */
	template <typename T>
	size_t update(std::vector<T> const &data, size_t offset = 0)
	{
		return update(data.data(), data.size() * sizeof(T), offset);
	}

	/**
	 * @brief 上传 std::array 数据到 mapped 内存。
	 */
	template <typename T, size_t N>
	size_t update(std::array<T, N> const &data, size_t offset = 0)
	{
		return update(data.data(), data.size() * sizeof(T), offset);
	}

	/**
	 * @brief 将任意对象按字节复制到 mapped 内存。
	 */
	template <class T>
	size_t convert_and_update(const T &object, size_t offset = 0)
	{
		return update(reinterpret_cast<const uint8_t *>(&object), sizeof(T), offset);
	}

	/**
	 * @brief 通过 vk::ArrayProxy 上传数据。
	 * @note 支持 T、std::vector<T>、std::array<T, N>、vk::ArrayProxy<T> 等类型。
	 */
	template <class T>
	size_t updateTyped(const vk::ArrayProxy<T> &object, size_t offset = 0)
	{
		return update(reinterpret_cast<const uint8_t *>(object.data()), object.size() * sizeof(T), offset);
	}

  protected:
	/**
	 * @brief 创建 VkBuffer 并分配内存。
	 * @param create_info Buffer 创建信息。
	 * @param alignment  对齐要求（0 表示无特殊对齐）。
	 * @return 创建的 vk::Buffer 句柄。
	 */
	[[nodiscard]] vk::Buffer create_buffer(const vk::BufferCreateInfo &create_info, VkDeviceSize alignment = 0);

	/**
	 * @brief 创建 VkImage 并分配内存。
	 * @param create_info Image 创建信息。
	 * @return 创建的 vk::Image 句柄。
	 */
	[[nodiscard]] vk::Image create_image(const vk::ImageCreateInfo &create_info);

	/**
	 * @brief 获取 VMA allocation 句柄。
	 */
	VmaAllocation get_allocation() const;

	/**
	 * @brief 设置 VMA allocation 句柄。
	 */
	void set_allocation(VmaAllocation alloc);

	/**
	 * @brief 创建后的回调，存储 allocation info。
	 *        派生类可重写以执行额外的后处理操作，
	 *        但应始终调用基类实现以确保状态正确。
	 */
	virtual void post_create(VmaAllocationInfo const &allocation_info);

	/**
	 * @brief 销毁 VkBuffer 并释放 VMA 内存。
	 */
	void destroy_buffer(vk::Buffer buffer);

	/**
	 * @brief 销毁 VkImage 并释放 VMA 内存。
	 */
	void destroy_image(vk::Image image);

	/**
	 * @brief 清除内部状态。在 destroy_* 中调用。
	 */
	void clear();

  private:
	VmaAllocationCreateInfo allocation_create_info = {};
	VmaAllocation           allocation             = VK_NULL_HANDLE;
	uint8_t                *mapped_data            = nullptr;
	bool                    coherent               = false;
	bool                    persistent             = false;
};

// ============================================================================
// Allocated 模板方法实现（必须内联在头文件中）
// ============================================================================

template <typename HandleType>
inline Allocated<HandleType>::Allocated(Allocated &&other) noexcept :
    ParentType{static_cast<ParentType &&>(other)},
    allocation_create_info(std::exchange(other.allocation_create_info, {})),
    allocation(std::exchange(other.allocation, {})),
    mapped_data(std::exchange(other.mapped_data, {})),
    coherent(std::exchange(other.coherent, {})),
    persistent(std::exchange(other.persistent, {}))
{
}

template <typename HandleType>
inline Allocated<HandleType>::Allocated(const VmaAllocationCreateInfo &allocation_create_info, VulkanDevice *device) :
    ParentType{HandleType{nullptr}, device},
    allocation_create_info(allocation_create_info)
{
}

template <typename HandleType>
inline Allocated<HandleType>::Allocated(HandleType handle, VulkanDevice *device) :
    ParentType(handle, device)
{
}

template <typename HandleType>
inline const HandleType *Allocated<HandleType>::get() const
{
	return &ParentType::GetHandle();
}

template <typename HandleType>
inline void Allocated<HandleType>::clear()
{
	mapped_data            = nullptr;
	persistent             = false;
	allocation_create_info = {};
}

template <typename HandleType>
inline vk::Buffer Allocated<HandleType>::create_buffer(const vk::BufferCreateInfo &create_info, VkDeviceSize alignment)
{
	vk::Buffer        buffer = VK_NULL_HANDLE;
	VmaAllocationInfo allocation_info{};

	VkResult result = VK_SUCCESS;
	if (alignment == 0)
	{
		result = vmaCreateBuffer(
		    this->GetDevice().GetVmaAllocator(),
		    reinterpret_cast<const VkBufferCreateInfo *>(&create_info),
		    &allocation_create_info,
		    reinterpret_cast<VkBuffer *>(&buffer),
		    &allocation,
		    &allocation_info);
	}
	else
	{
		result = vmaCreateBufferWithAlignment(
		    this->GetDevice().GetVmaAllocator(),
		    reinterpret_cast<const VkBufferCreateInfo *>(&create_info),
		    &allocation_create_info,
		    alignment,
		    reinterpret_cast<VkBuffer *>(&buffer),
		    &allocation,
		    &allocation_info);
	}

	if (result != VK_SUCCESS)
	{
		throw std::runtime_error("Cannot create Buffer");
	}
	post_create(allocation_info);
	return buffer;
}

template <typename HandleType>
inline vk::Image Allocated<HandleType>::create_image(const vk::ImageCreateInfo &create_info)
{
	vk::Image         image = VK_NULL_HANDLE;
	VmaAllocationInfo allocation_info{};

	VkResult result = vmaCreateImage(this->GetDevice().GetVmaAllocator(),
	                                 reinterpret_cast<const VkImageCreateInfo *>(&create_info),
	                                 &allocation_create_info,
	                                 reinterpret_cast<VkImage *>(&image),
	                                 &allocation,
	                                 &allocation_info);

	if (result != VK_SUCCESS)
	{
		throw std::runtime_error("Cannot create Image");
	}

	post_create(allocation_info);
	return image;
}

template <typename HandleType>
inline void Allocated<HandleType>::destroy_buffer(vk::Buffer handle)
{
	if (handle != VK_NULL_HANDLE && allocation != VK_NULL_HANDLE)
	{
		unmap();
		vmaDestroyBuffer(this->GetDevice().GetVmaAllocator(), static_cast<VkBuffer>(handle), allocation);
		clear();
	}
}

template <typename HandleType>
inline void Allocated<HandleType>::destroy_image(vk::Image image)
{
	if (image != VK_NULL_HANDLE && allocation != VK_NULL_HANDLE)
	{
		unmap();
		vmaDestroyImage(this->GetDevice().GetVmaAllocator(), static_cast<VkImage>(image), allocation);
		clear();
	}
}

template <typename HandleType>
inline void Allocated<HandleType>::flush(VkDeviceSize offset, VkDeviceSize size)
{
	if (!coherent)
	{
		vmaFlushAllocation(this->GetDevice().GetVmaAllocator(), allocation, offset, size);
	}
}

template <typename HandleType>
inline const uint8_t *Allocated<HandleType>::get_data() const
{
	return mapped_data;
}

template <typename HandleType>
inline vk::DeviceMemory Allocated<HandleType>::get_memory() const
{
	VmaAllocationInfo alloc_info;
	vmaGetAllocationInfo(this->GetDevice().GetVmaAllocator(), allocation, &alloc_info);
	return static_cast<vk::DeviceMemory>(alloc_info.deviceMemory);
}

template <typename HandleType>
inline VkDeviceSize Allocated<HandleType>::get_memory_offset() const
{
	VmaAllocationInfo alloc_info;
	vmaGetAllocationInfo(this->GetDevice().GetVmaAllocator(), allocation, &alloc_info);
	return alloc_info.offset;
}

template <typename HandleType>
inline uint8_t *Allocated<HandleType>::map()
{
	if (!persistent && !mapped())
	{
		VkResult result = vmaMapMemory(this->GetDevice().GetVmaAllocator(), allocation, reinterpret_cast<void **>(&mapped_data));
		if (result != VK_SUCCESS)
		{
			throw std::runtime_error("Cannot map memory");
		}
	}
	return mapped_data;
}

template <typename HandleType>
inline bool Allocated<HandleType>::mapped() const
{
	return mapped_data != nullptr;
}

template <typename HandleType>
inline VmaAllocation Allocated<HandleType>::get_allocation() const
{
	return allocation;
}

template <typename HandleType>
inline void Allocated<HandleType>::set_allocation(VmaAllocation alloc)
{
	allocation = alloc;
}

template <typename HandleType>
inline void Allocated<HandleType>::post_create(VmaAllocationInfo const &allocation_info)
{
	VkMemoryPropertyFlags memory_properties;
	vmaGetAllocationMemoryProperties(this->GetDevice().GetVmaAllocator(), allocation, &memory_properties);
	coherent    = (memory_properties & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) == VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
	mapped_data = static_cast<uint8_t *>(allocation_info.pMappedData);
	persistent  = mapped();
}

template <typename HandleType>
inline void Allocated<HandleType>::unmap()
{
	if (!persistent && mapped())
	{
		vmaUnmapMemory(this->GetDevice().GetVmaAllocator(), allocation);
		mapped_data = nullptr;
	}
}

template <typename HandleType>
inline size_t Allocated<HandleType>::update(const uint8_t *data, size_t size, size_t offset)
{
	if (persistent)
	{
		std::copy(data, data + size, mapped_data + offset);
		flush();
	}
	else
	{
		map();
		std::copy(data, data + size, mapped_data + offset);
		flush();
		unmap();
	}
	return size;
}

template <typename HandleType>
inline size_t Allocated<HandleType>::update(void const *data, size_t size, size_t offset)
{
	return update(reinterpret_cast<const uint8_t *>(data), size, offset);
}

// ============================================================================
// 便捷类型别名
// ============================================================================

using AllocatedBuffer = Allocated<vk::Buffer>;
using AllocatedImage  = Allocated<vk::Image>;

}        // namespace allocated
}        // namespace GE
