/* Copyright (c) 2026, NVIDIA CORPORATION. All rights reserved.
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
 * @file StructureChainBuilder.h
 * @brief Vulkan pNext 结构链构建器，仅保留 Vulkan-Hpp（C++）版本。
 *
 * 从 Vulkan-Samples 适配，用于方便地构建和管理 Vulkan 的 pNext 结构链。
 */

#pragma once

#include <any>
#include <vulkan/vulkan.hpp>

#include <memory>
#include <vector>

namespace GE {

/**
 * @brief Vulkan pNext 结构链构建器。
 *
 * 模板参数 AnchorStructType 是链的锚点结构类型（如 vk::GraphicsPipelineCreateInfo）。
 * 通过 add_struct() 向链中添加后续结构，链中的 pNext 指针自动维护。
 * 额外数据（如链中结构指针指向的数据）通过 add_chain_data() 添加，生命周期自动管理。
 */
template <typename AnchorStructType>
class StructureChainBuilder
{
  public:
	StructureChainBuilder();

	/**
	 * @brief 向构建器添加额外数据。
	 *
	 * 这些数据不是结构链的一部分，但被链中的结构引用（例如被结构成员指针指向）。
	 * 其生命周期与构建器绑定，确保在结构链使用期间始终有效。
	 *
	 * @param data_to_add 要添加的数据，默认值为 T{}。
	 * @return 添加的数据的引用。
	 */
	template <typename T>
	T &add_chain_data(T const &data_to_add = {});

	/**
	 * @brief 向 pNext 结构链末尾添加一个新结构。
	 *
	 * @param struct_to_add 要添加的结构，默认值为 StructType{}。
	 * @return 添加的结构的引用，可在后续继续修改。
	 */
	template <typename StructType>
	StructType &add_struct(StructType const &struct_to_add = {});

	/**
	 * @brief 从结构链中按类型查找结构。
	 *
	 * @param skip 跳过的匹配项数量，用于存在多个同类型结构时。
	 *             仅在 StructType::allowDuplicate == true 时允许非零值。
	 * @return 找到的结构指针，未找到返回 nullptr。
	 */
	template <typename StructType>
	StructType const *get_struct(size_t skip = 0) const;

	/**
	 * @brief 替换锚点结构的值，同时保留已有的 pNext 链。
	 * @param anchor_struct 新的锚点结构值。
	 */
	void set_anchor_struct(AnchorStructType const &anchor_struct);

  private:
	/// 结构链中的所有结构，按添加顺序存储
	std::vector<std::unique_ptr<std::any>> structure_chain;

	/// 链结构引用的额外数据，单独存储以确保正确的析构顺序
	///（结构中的指针可能指向此处的数据）
	std::vector<std::unique_ptr<std::any>> chain_data;
};

// ============================================================================
// 模板实现
// ============================================================================

template <typename AnchorStructType>
inline StructureChainBuilder<AnchorStructType>::StructureChainBuilder()
{
	// 验证 AnchorStructType 的内存布局符合 Vulkan 结构要求：
	// sType 必须在偏移 0，pNext 必须在偏移 sizeof(void*)。
	static_assert((offsetof(AnchorStructType, sType) == 0) &&
	              (offsetof(AnchorStructType, pNext) == sizeof(void *)));

	structure_chain.push_back(std::make_unique<std::any>(std::make_any<AnchorStructType>()));
}

template <typename AnchorStructType>
template <typename T>
inline T &StructureChainBuilder<AnchorStructType>::add_chain_data(T const &data_to_add)
{
	chain_data.push_back(std::make_unique<std::any>(std::make_any<T>(data_to_add)));
	return *std::any_cast<T>(chain_data.back().get());
}

template <typename AnchorStructType>
template <typename StructType>
inline StructType &StructureChainBuilder<AnchorStructType>::add_struct(StructType const &struct_to_add)
{
#if !defined(NDEBUG)
	// 调试模式下检查同类型结构是否已存在（除非该类型允许重复）
	auto it = std::ranges::find_if(structure_chain, [](auto const &chain_element) {
		return std::any_cast<StructType>(chain_element.get()) != nullptr;
	});
	assert(it == structure_chain.end() || StructType::allowDuplicate);
#endif

	structure_chain.push_back(std::make_unique<std::any>(std::make_any<StructType>(struct_to_add)));

	// 新结构的 pNext 继承当前锚点的 pNext
	std::any_cast<StructType>(structure_chain.back().get())->pNext =
	    std::any_cast<AnchorStructType>(structure_chain.front().get())->pNext;

	// 锚点的 pNext 指向新结构
	std::any_cast<AnchorStructType>(structure_chain.front().get())->pNext =
	    std::any_cast<StructType>(structure_chain.back().get());

	return *std::any_cast<StructType>(structure_chain.back().get());
}

template <typename AnchorStructType>
template <typename StructType>
inline StructType const *StructureChainBuilder<AnchorStructType>::get_struct(size_t skip) const
{
	assert(!skip || StructType::allowDuplicate);

	auto it = std::ranges::find_if(structure_chain, [](auto const &chain_element) {
		return std::any_cast<StructType>(chain_element.get()) != nullptr;
	});

	for (size_t i = 0; (i < skip) && (it != structure_chain.end()); ++i)
	{
		it = std::find_if(std::next(it), structure_chain.end(), [](auto const &chain_element) {
			return std::any_cast<StructType>(chain_element.get()) != nullptr;
		});
	}

	return (it != structure_chain.end()) ? std::any_cast<StructType>(it->get()) : nullptr;
}

template <typename AnchorStructType>
inline void StructureChainBuilder<AnchorStructType>::set_anchor_struct(AnchorStructType const &anchor_struct)
{
	// 保留现有的 pNext 链
	void const *pNext = std::any_cast<AnchorStructType>(structure_chain.front().get())->pNext;

	*std::any_cast<AnchorStructType>(structure_chain.front().get()) = anchor_struct;

	// 恢复 pNext 指针，保持链的完整性
	std::any_cast<AnchorStructType>(structure_chain.front().get())->pNext = pNext;
}

} // namespace GE
