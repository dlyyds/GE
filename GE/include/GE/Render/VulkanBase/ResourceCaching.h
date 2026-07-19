/* Copyright (c) 2023-2026, NVIDIA CORPORATION. All rights reserved.
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
 * @file ResourceCaching.h
 * @brief 从 Vulkan-Samples 的 hpp_resource_caching.h / resource_caching.h 适配。
 *
 * 提供通用资源缓存机制，通过 hash 参数自动创建并缓存各类 Vulkan 资源：
 * - hash_combine / hash_param：用于生成参数的复合 hash
 * - request_resource<T>()：查找缓存，未命中则创建并插入
 * - std::hash 特化：支持 GE 自定义类型作为 unordered_map 的 key
 */

#pragma once

#include "Render/VulkanBase/VulkanCommon.h"
#include "Render/VulkanBase/VulkanDescriptorSet.h"
#include "Render/VulkanBase/VulkanDescriptorSetLayout.h"
#include "Render/VulkanBase/VulkanImage.h"
#include "Render/VulkanBase/VulkanImageView.h"
#include "Render/VulkanBase/VulkanPipeline.h"
#include "Render/VulkanBase/VulkanPipelineLayout.h"
#include "Render/VulkanBase/VulkanPipelineState.h"
#include "Render/VulkanBase/ShaderModule.h"

#include <vulkan/vulkan.hpp>

#include <glm/gtx/hash.hpp>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <unordered_map>
#include <vector>

// ============================================================================
// hash_combine — 通用 hash 组合函数
// 使用 GLM 内置的 hash_combine 实现（与 Vulkan-Samples 原版一致）
// ============================================================================

namespace GE {
namespace detail {

/**
 * @brief 将值 v 的 hash 与 seed 组合（boost::hash_combine 风格）。
 * @param seed 输入/输出的 hash 种子
 * @param v    要组合的值
 */
template <class T>
inline void hash_combine(size_t &seed, const T &v)
{
    std::hash<T> hasher;
    glm::detail::hash_combine(seed, hasher(v));
}

} // namespace detail
} // namespace GE

// ============================================================================
// std::hash 特化 — 使 GE 类型可用作 unordered_map 的 key
// ============================================================================

namespace std {

// ---- vulkan.hpp 类型特化（vulkan.hpp 未提供 std::hash 特化） ----

// vk::Flags<T> 转换为底层整数类型
template <typename T>
struct hash<vk::Flags<T>>
{
    size_t operator()(vk::Flags<T> const &flags) const
    {
        return std::hash<typename vk::Flags<T>::MaskType>()(static_cast<typename vk::Flags<T>::MaskType>(flags));
    }
};

template <>
struct hash<vk::Extent3D>
{
    size_t operator()(vk::Extent3D const &extent) const
    {
        size_t result = 0;
        GE::detail::hash_combine(result, extent.width);
        GE::detail::hash_combine(result, extent.height);
        GE::detail::hash_combine(result, extent.depth);
        return result;
    }
};

template <>
struct hash<vk::Extent2D>
{
    size_t operator()(vk::Extent2D const &extent) const
    {
        size_t result = 0;
        GE::detail::hash_combine(result, extent.width);
        GE::detail::hash_combine(result, extent.height);
        return result;
    }
};

template <>
struct hash<vk::Offset2D>
{
    size_t operator()(vk::Offset2D const &offset) const
    {
        size_t result = 0;
        GE::detail::hash_combine(result, offset.x);
        GE::detail::hash_combine(result, offset.y);
        return result;
    }
};

template <>
struct hash<vk::Offset3D>
{
    size_t operator()(vk::Offset3D const &offset) const
    {
        size_t result = 0;
        GE::detail::hash_combine(result, offset.x);
        GE::detail::hash_combine(result, offset.y);
        GE::detail::hash_combine(result, offset.z);
        return result;
    }
};

template <>
struct hash<vk::Rect2D>
{
    size_t operator()(vk::Rect2D const &rect) const
    {
        size_t result = 0;
        GE::detail::hash_combine(result, rect.offset);
        GE::detail::hash_combine(result, rect.extent);
        return result;
    }
};

template <>
struct hash<vk::Viewport>
{
    size_t operator()(vk::Viewport const &viewport) const
    {
        size_t result = 0;
        GE::detail::hash_combine(result, viewport.x);
        GE::detail::hash_combine(result, viewport.y);
        GE::detail::hash_combine(result, viewport.width);
        GE::detail::hash_combine(result, viewport.height);
        GE::detail::hash_combine(result, viewport.minDepth);
        GE::detail::hash_combine(result, viewport.maxDepth);
        return result;
    }
};

template <>
struct hash<vk::ImageSubresource>
{
    size_t operator()(vk::ImageSubresource const &sub) const
    {
        size_t result = 0;
        GE::detail::hash_combine(result, sub.aspectMask);
        GE::detail::hash_combine(result, sub.mipLevel);
        GE::detail::hash_combine(result, sub.arrayLayer);
        return result;
    }
};

template <>
struct hash<vk::ImageSubresourceLayers>
{
    size_t operator()(vk::ImageSubresourceLayers const &layers) const
    {
        size_t result = 0;
        GE::detail::hash_combine(result, layers.aspectMask);
        GE::detail::hash_combine(result, layers.mipLevel);
        GE::detail::hash_combine(result, layers.baseArrayLayer);
        GE::detail::hash_combine(result, layers.layerCount);
        return result;
    }
};

template <>
struct hash<vk::ImageSubresourceRange>
{
    size_t operator()(vk::ImageSubresourceRange const &range) const
    {
        size_t result = 0;
        GE::detail::hash_combine(result, range.aspectMask);
        GE::detail::hash_combine(result, range.baseMipLevel);
        GE::detail::hash_combine(result, range.levelCount);
        GE::detail::hash_combine(result, range.baseArrayLayer);
        GE::detail::hash_combine(result, range.layerCount);
        return result;
    }
};

// ---- std::map / std::vector 泛型 hash ----

template <typename Key, typename Value>
struct hash<std::map<Key, Value>>
{
    size_t operator()(std::map<Key, Value> const &bindings) const
    {
        size_t result = 0;
        GE::detail::hash_combine(result, bindings.size());
        for (auto const &binding : bindings)
        {
            GE::detail::hash_combine(result, binding.first);
            GE::detail::hash_combine(result, binding.second);
        }
        return result;
    }
};

template <typename T>
struct hash<std::vector<T>>
{
    size_t operator()(std::vector<T> const &values) const
    {
        size_t result = 0;
        GE::detail::hash_combine(result, values.size());
        for (auto const &value : values)
        {
            GE::detail::hash_combine(result, value);
        }
        return result;
    }
};

// ---- GE::LoadStoreInfo ----

template <>
struct hash<GE::LoadStoreInfo>
{
    size_t operator()(GE::LoadStoreInfo const &lsi) const
    {
        size_t result = 0;
        GE::detail::hash_combine(result, lsi.load_op);
        GE::detail::hash_combine(result, lsi.store_op);
        return result;
    }
};

// ---- GE::ShaderModule ----

template <>
struct hash<GE::ShaderModule>
{
    size_t operator()(GE::ShaderModule const &shader_module) const
    {
        return std::hash<size_t>()(shader_module.get_id());
    }
};

// ---- GE::ShaderResource ----

template <>
struct hash<GE::ShaderResource>
{
    size_t operator()(GE::ShaderResource const &sr) const
    {
        size_t result = 0;
        GE::detail::hash_combine(result, sr.stages);
        GE::detail::hash_combine(result, sr.type);
        GE::detail::hash_combine(result, sr.mode);
        GE::detail::hash_combine(result, sr.set);
        GE::detail::hash_combine(result, sr.binding);
        GE::detail::hash_combine(result, sr.location);
        GE::detail::hash_combine(result, sr.input_attachment_index);
        GE::detail::hash_combine(result, sr.vec_size);
        GE::detail::hash_combine(result, sr.columns);
        GE::detail::hash_combine(result, sr.array_size);
        GE::detail::hash_combine(result, sr.offset);
        GE::detail::hash_combine(result, sr.size);
        GE::detail::hash_combine(result, sr.constant_id);
        GE::detail::hash_combine(result, sr.qualifiers);
        GE::detail::hash_combine(result, sr.name);
        return result;
    }
};

// ---- GE::ShaderVariant ----

template <>
struct hash<GE::ShaderVariant>
{
    size_t operator()(GE::ShaderVariant const &variant) const
    {
        return std::hash<size_t>()(variant.get_id());
    }
};

// ---- GE::ShaderSource ----

template <>
struct hash<GE::ShaderSource>
{
    size_t operator()(GE::ShaderSource const &source) const
    {
        return std::hash<size_t>()(source.get_id());
    }
};

// ---- GE::VulkanDescriptorSetLayout ----

template <>
struct hash<GE::VulkanDescriptorSetLayout>
{
    size_t operator()(GE::VulkanDescriptorSetLayout const &layout) const
    {
        return std::hash<vk::DescriptorSetLayout>()(layout.GetHandle());
    }
};

// ---- GE::VulkanPipelineLayout ----

template <>
struct hash<GE::VulkanPipelineLayout>
{
    size_t operator()(GE::VulkanPipelineLayout const &layout) const
    {
        return std::hash<vk::PipelineLayout>()(layout.GetHandle());
    }
};

// ---- GE::VulkanImage ----

template <>
struct hash<GE::VulkanImage>
{
    size_t operator()(GE::VulkanImage const &image) const
    {
        size_t result = 0;
        // 注意：VulkanImage 没有 get_memory() / get_sample_count() 等方法，
        // 这里使用可用的 getter 构造 hash
        GE::detail::hash_combine(result, image.get_type());
        GE::detail::hash_combine(result, image.get_extent());
        GE::detail::hash_combine(result, image.get_format());
        GE::detail::hash_combine(result, image.get_usage());
        GE::detail::hash_combine(result, image.get_tiling());
        GE::detail::hash_combine(result, image.get_subresource());
        GE::detail::hash_combine(result, image.get_array_layer_count());
        return result;
    }
};

// ---- GE::VulkanImageView ----

template <>
struct hash<GE::VulkanImageView>
{
    size_t operator()(GE::VulkanImageView const &image_view) const
    {
        size_t result = 0;
        GE::detail::hash_combine(result, image_view.GetHandle());
        GE::detail::hash_combine(result, image_view.get_format());
        GE::detail::hash_combine(result, image_view.get_subresource_range());
        return result;
    }
};

// ---- GE::VulkanDescriptorSet ----

template <>
struct hash<GE::VulkanDescriptorSet>
{
    size_t operator()(GE::VulkanDescriptorSet const &descriptor_set) const
    {
        size_t result = 0;
        GE::detail::hash_combine(result, descriptor_set.GetHandle());
        // 当 VulkanDescriptorSet 有 get_layout() / get_buffer_infos() 等方法时，可在此扩展
        return result;
    }
};

// ---- GE::VulkanPipelineState ----

template <>
struct hash<GE::VulkanPipelineState>
{
    size_t operator()(GE::VulkanPipelineState const &pipeline_state) const
    {
        // 使用 vk::Pipeline 句柄作为 hash 基础
        // 当 VulkanPipelineState 有更多 getter 时，可在此扩展
        return std::hash<vk::Pipeline>()(VK_NULL_HANDLE);
    }
};

// ---- GE::VulkanGraphicsPipeline ----

template <>
struct hash<GE::VulkanGraphicsPipeline>
{
    size_t operator()(GE::VulkanGraphicsPipeline const &pipeline) const
    {
        return std::hash<vk::Pipeline>()(pipeline.GetHandle());
    }
};

// ---- GE::VulkanComputePipeline ----

template <>
struct hash<GE::VulkanComputePipeline>
{
    size_t operator()(GE::VulkanComputePipeline const &pipeline) const
    {
        return std::hash<vk::Pipeline>()(pipeline.GetHandle());
    }
};

// ---- vk::DescriptorBufferInfo ----

template <>
struct hash<vk::DescriptorBufferInfo>
{
    size_t operator()(vk::DescriptorBufferInfo const &info) const
    {
        size_t result = 0;
        GE::detail::hash_combine(result, info.buffer);
        GE::detail::hash_combine(result, info.range);
        GE::detail::hash_combine(result, info.offset);
        return result;
    }
};

// ---- vk::DescriptorImageInfo ----

template <>
struct hash<vk::DescriptorImageInfo>
{
    size_t operator()(vk::DescriptorImageInfo const &info) const
    {
        size_t result = 0;
        GE::detail::hash_combine(result, info.imageView);
        GE::detail::hash_combine(result, info.imageLayout);
        GE::detail::hash_combine(result, info.sampler);
        return result;
    }
};

// ---- vk::WriteDescriptorSet ----

template <>
struct hash<vk::WriteDescriptorSet>
{
    size_t operator()(vk::WriteDescriptorSet const &wds) const
    {
        size_t result = 0;
        GE::detail::hash_combine(result, wds.dstSet);
        GE::detail::hash_combine(result, wds.dstBinding);
        GE::detail::hash_combine(result, wds.dstArrayElement);
        GE::detail::hash_combine(result, wds.descriptorCount);
        GE::detail::hash_combine(result, wds.descriptorType);

        switch (wds.descriptorType)
        {
            case vk::DescriptorType::eSampler:
            case vk::DescriptorType::eCombinedImageSampler:
            case vk::DescriptorType::eSampledImage:
            case vk::DescriptorType::eStorageImage:
            case vk::DescriptorType::eInputAttachment:
                for (uint32_t i = 0; i < wds.descriptorCount; i++)
                {
                    GE::detail::hash_combine(result, wds.pImageInfo[i]);
                }
                break;

            case vk::DescriptorType::eUniformTexelBuffer:
            case vk::DescriptorType::eStorageTexelBuffer:
                for (uint32_t i = 0; i < wds.descriptorCount; i++)
                {
                    GE::detail::hash_combine(result, wds.pTexelBufferView[i]);
                }
                break;

            case vk::DescriptorType::eUniformBuffer:
            case vk::DescriptorType::eStorageBuffer:
            case vk::DescriptorType::eUniformBufferDynamic:
            case vk::DescriptorType::eStorageBufferDynamic:
                for (uint32_t i = 0; i < wds.descriptorCount; i++)
                {
                    GE::detail::hash_combine(result, wds.pBufferInfo[i]);
                }
                break;

            default:
                // 暂不支持的类型
                break;
        }

        return result;
    }
};

} // namespace std

// ============================================================================
// hash_param — 变参 hash 计算
// ============================================================================

namespace GE {
namespace detail {

/**
 * @brief 对单个值计算 hash 并组合到 seed 中。
 */
template <typename T>
inline void hash_param(size_t &seed, const T &value)
{
    hash_combine(seed, value);
}

/**
 * @brief 对 VkPipelineCache 忽略 hash（不参与缓存 key）。
 */
template <>
inline void hash_param(size_t & /*seed*/, const VkPipelineCache & /*value*/)
{
}

/**
 * @brief 对 vector<uint8_t> 按字符串形式计算 hash。
 */
template <>
inline void hash_param(size_t &seed, const std::vector<uint8_t> &value)
{
    hash_combine(seed, std::string{value.begin(), value.end()});
}

/**
 * @brief 递归变参 hash：组合第一个参数，再递归处理剩余参数。
 */
template <typename T, typename... Args>
inline void hash_param(size_t &seed, const T &first_arg, const Args &...args)
{
    hash_param(seed, first_arg);
    hash_param(seed, args...);
}

} // namespace detail
} // namespace GE

// ============================================================================
// request_resource — 通用缓存-创建模板函数
// ============================================================================

namespace GE {
namespace detail {

/**
 * @brief 辅助类，用于记录资源创建信息（默认无操作）。
 *
 * 当需要支持资源回放录制时，可特化此模板。
 */
template <class T, class... A>
struct ResourceRecordHelper
{
    size_t record(/* HPPResourceRecord &recorder, */ A &.../*args*/)
    {
        return 0;
    }

    void index(/* HPPResourceRecord &recorder, */ size_t /*index*/, T & /*resource*/)
    {
    }
};

} // namespace detail

/**
 * @brief 请求一个已缓存的资源，若不存在则创建并缓存。
 *
 * 模板函数，通过 hash 参数查找缓存：
 * 1. 对所有参数计算复合 hash
 * 2. 在 unordered_map 中查找
 * 3. 若命中，返回已有资源引用
 * 4. 若未命中，创建新资源、插入缓存、返回引用
 *
 * @tparam T       资源类型（如 VulkanGraphicsPipeline）
 * @tparam A       构造参数类型
 * @param device   Vulkan 设备引用
 * @param resources 缓存 unordered_map
 * @param args     传递给 T 构造函数的参数
 * @return T&      缓存中的资源引用
 */
template <class T, class... A>
T &request_resource(
    VulkanDevice &device,
    std::unordered_map<size_t, T> &resources,
    A &...args)
{
    size_t hash{0U};
    detail::hash_param(hash, args...);

    auto res_it = resources.find(hash);

    if (res_it != resources.end())
    {
        return res_it->second;
    }

    // 未命中缓存，创建新资源
    const char *res_type = typeid(T).name();
    size_t      res_id   = resources.size();

    // 仅在非 Debug 模式下捕获异常
#ifndef DEBUG
    try
    {
#endif
        T resource(device, args...);

        auto res_ins_it = resources.emplace(hash, std::move(resource));

        if (!res_ins_it.second)
        {
            throw std::runtime_error{std::string{"插入失败: #"} + std::to_string(res_id) + " 缓存对象 (" + res_type + ")"};
        }

        res_it = res_ins_it.first;
#ifndef DEBUG
    }
    catch (const std::exception &e)
    {
        // 资源创建失败，记录错误并重新抛出
        throw std::runtime_error{std::string{"创建失败: #"} + std::to_string(res_id) + " 缓存对象 (" + res_type + "): " + e.what()};
    }
#endif

    return res_it->second;
}

} // namespace GE