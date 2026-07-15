/* Copyright (c) 2019-2025, Arm Limited and Contributors
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
 * @file VulkanPipeline.h
 * @brief Vulkan 管线封装，参考 Vulkan-Samples 的 Pipeline 设计。
 *
 * 体系结构：
 *   VulkanPipeline（基类）
 *     ├─ VulkanGraphicsPipeline（图形管线）
 *     └─ VulkanComputePipeline（计算管线）
 *
 * 基类持有由外部传入的 VulkanPipelineState 副本，
 * 构造函数创建管线后自动清除脏标记。
 */

#pragma once

#include <vulkan/vulkan.hpp>

#include <cstdint>

#include "Render/VulkanBase/VulkanPipelineState.h"

namespace GE
{

class VulkanDevice;

class VulkanDevice;

/**
 * @brief Vulkan 管线基类。
 *
 * 持有 vk::Pipeline 句柄和 VulkanPipelineState 副本。
 * 析构时自动销毁句柄。
 */
class VulkanPipeline
{
  public:
    VulkanPipeline(VulkanDevice &device);

    VulkanPipeline(const VulkanPipeline &) = delete;

    VulkanPipeline(VulkanPipeline &&other) noexcept;

    virtual ~VulkanPipeline();

    VulkanPipeline &operator=(const VulkanPipeline &) = delete;

    VulkanPipeline &operator=(VulkanPipeline &&) = delete;

    /// 获取底层 VkPipeline 句柄
    vk::Pipeline GetHandle() const { return m_Handle; }

    /// 获取可修改的状态引用（用于运行时动态更新）
    VulkanPipelineState &GetState() { return m_State; }

    /// 获取只读状态引用
    const VulkanPipelineState &GetState() const { return m_State; }

  protected:
    VulkanDevice       &m_Device;
    vk::Pipeline        m_Handle{VK_NULL_HANDLE};
    VulkanPipelineState m_State;
};


/**
 * @brief 图形管线。
 *
 * 从给定的 VulkanPipelineState 创建 VkPipeline，
 * 默认启用 Dynamic Rendering。
 */
class VulkanGraphicsPipeline : public VulkanPipeline
{
  public:
    VulkanGraphicsPipeline(VulkanDevice       &device,
                           VkPipelineCache     pipeline_cache,
                           VulkanPipelineState &pipeline_state);

    ~VulkanGraphicsPipeline() override;

    VulkanGraphicsPipeline(const VulkanGraphicsPipeline &) = delete;

    VulkanGraphicsPipeline(VulkanGraphicsPipeline &&) = default;

    VulkanGraphicsPipeline &operator=(const VulkanGraphicsPipeline &) = delete;

    VulkanGraphicsPipeline &operator=(VulkanGraphicsPipeline &&) = delete;

    /// 绑定到命令缓冲区
    void Bind(vk::CommandBuffer cmd) const;
};


/**
 * @brief 计算管线。
 *
 * 从给定的 VulkanPipelineState 创建 VkPipeline。
 */
class VulkanComputePipeline : public VulkanPipeline
{
  public:
    VulkanComputePipeline(VulkanDevice       &device,
                          VkPipelineCache     pipeline_cache,
                          VulkanPipelineState &pipeline_state);

    ~VulkanComputePipeline() override;

    VulkanComputePipeline(const VulkanComputePipeline &) = delete;

    VulkanComputePipeline(VulkanComputePipeline &&) = default;

    VulkanComputePipeline &operator=(const VulkanComputePipeline &) = delete;

    VulkanComputePipeline &operator=(VulkanComputePipeline &&) = delete;
};

} // namespace GE
