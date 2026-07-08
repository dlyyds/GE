/* Copyright (c) 2021-2024, NVIDIA CORPORATION. All rights reserved.
 * Copyright (c) 2024, Bradley Austin Davis. All rights reserved.
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
 * @file VulkanAllocated.cpp
 * @brief VMA 分配器单例管理实现。
 *
 * 提供 VMA 分配器的全局单例存取，供 Allocated 基类内部使用。
 * 分配器的生命周期由 VulkanDevice 管理。
 */

#include "Render/VulkanBase/VulkanAllocated.h"
#include "Core/Log.h"

namespace GE
{
namespace allocated
{
VmaAllocator &get_memory_allocator()
{
	static VmaAllocator memory_allocator = VK_NULL_HANDLE;
	return memory_allocator;
}

void init(VmaAllocator allocator)
{
	auto &global_allocator = get_memory_allocator();
	if (global_allocator == VK_NULL_HANDLE)
	{
		global_allocator = allocator;
	}
}

void shutdown()
{
	auto &global_allocator = get_memory_allocator();
	if (global_allocator != VK_NULL_HANDLE)
	{
		VmaTotalStatistics stats;
		vmaCalculateStatistics(global_allocator, &stats);
		GE_CORE_INFO("VMA 设备内存泄漏统计：{} bytes",
		             stats.total.statistics.allocationBytes);
		// 注意：不调用 vmaDestroyAllocator，所有权由 VulkanDevice 管理
		global_allocator = VK_NULL_HANDLE;
	}
}

}        // namespace allocated
}        // namespace GE
