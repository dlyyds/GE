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
 * @file SPIRVReflection.h
 * @brief SPIR-V 反射器，从 Vulkan-Samples 适配而来。
 *
 * 基于 SPIRV-Cross 对着色器资源进行反射，
 * 生成 ShaderResource 列表，支持 ShaderVariant 的运行时数组大小。
 */

#pragma once

#include <string>
#include <unordered_map>
#include <vector>

#pragma warning(push)
#pragma warning(disable : 4065)
#include <spirv_glsl.hpp>
#pragma warning(pop)

#include "Render/VulkanBase/VulkanCommon.h"
#include "Render/VulkanBase/VulkanShaderModule.h"

namespace GE
{

/// 基于 SPIRV 反射代码和 ShaderVariant 生成着色器资源列表
class SPIRVReflection
{
  public:
    /// @brief 从 SPIR-V 代码中反射着色器资源
    /// @param stage Vulkan 着色器阶段标志
    /// @param spirv 着色器的 SPIR-V 代码
    /// @param[out] resources 反射出的着色器资源列表
    /// @param variant 用于指定运行时数组大小的 ShaderVariant
    bool reflect_shader_resources(vk::ShaderStageFlagBits        stage,
                                  const std::vector<uint32_t> &spirv,
                                  std::vector<ShaderResource> &resources,
                                  const ShaderVariant         &variant);

  private:
    void parse_shader_resources(const spirv_cross::Compiler &compiler,
                                vk::ShaderStageFlagBits      stage,
                                std::vector<ShaderResource> &resources,
                                const ShaderVariant         &variant);

    void parse_push_constants(const spirv_cross::Compiler &compiler,
                              vk::ShaderStageFlagBits      stage,
                              std::vector<ShaderResource> &resources,
                              const ShaderVariant         &variant);

    void parse_specialization_constants(const spirv_cross::Compiler &compiler,
                                        vk::ShaderStageFlagBits      stage,
                                        std::vector<ShaderResource> &resources,
                                        const ShaderVariant         &variant);
};

} // namespace GE
