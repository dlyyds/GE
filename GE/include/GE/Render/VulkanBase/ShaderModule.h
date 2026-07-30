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
 * @file ShaderModule.h
 * @brief 着色器模块封装，从 Vulkan-Samples 适配而来。
 *
 * 包含 ShaderResource、ShaderVariant、ShaderSource、ShaderModule 等类型，
 * 提供 SPIR-V 加载、反射、变体管理等完整着色器功能。
 */

#pragma once

#include <vulkan/vulkan.hpp>

#include <string>
#include <unordered_map>
#include <vector>

#include "Render/VulkanBase/VulkanResourceBase.h"

namespace GE {

class VulkanDevice;

/// 着色器资源类型
enum class ShaderResourceType
{
    Input,
    InputAttachment,
    Output,
    Image,
    ImageSampler,
    ImageStorage,
    Sampler,
    BufferUniform,
    BufferStorage,
    PushConstant,
    SpecializationConstant,
    All
};

/// 决定 descriptor set 的创建和绑定方式
enum class ShaderResourceMode
{
    Static,
    Dynamic,
    UpdateAfterBind
};

/// 资源限定符位掩码
struct ShaderResourceQualifiers
{
    enum : uint32_t
    {
        None        = 0,
        NonReadable = 1,
        NonWritable = 2,
    };
};

/// 存储着色器资源反射数据。
struct ShaderResource
{
    vk::ShaderStageFlags stages;

    ShaderResourceType type;

    ShaderResourceMode mode;

    uint32_t set;

    uint32_t binding;

    uint32_t location;

    uint32_t input_attachment_index;

    uint32_t vec_size;

    uint32_t columns;

    uint32_t array_size;

    uint32_t offset;

    uint32_t size;

    uint32_t constant_id;

    uint32_t qualifiers;

    /// 顶点输入/输出属性推导出的 Vulkan 格式（仅对 Input/Output 类型有效）
    vk::Format format{vk::Format::eUndefined};

    std::string name;
};

/// 返回给定 Vulkan 顶点格式的字节大小（用于顶点属性 offset/stride 计算）。
/// 仅支持常用的 32/64-bit 标量/向量格式，其他返回 0。
inline uint32_t GetVertexFormatSize(vk::Format format)
{
    switch (format)
    {
        case vk::Format::eR32Sfloat:
        case vk::Format::eR32Sint:
        case vk::Format::eR32Uint:
            return 4;
        case vk::Format::eR32G32Sfloat:
        case vk::Format::eR32G32Sint:
        case vk::Format::eR32G32Uint:
            return 8;
        case vk::Format::eR32G32B32Sfloat:
        case vk::Format::eR32G32B32Sint:
        case vk::Format::eR32G32B32Uint:
            return 12;
        case vk::Format::eR32G32B32A32Sfloat:
        case vk::Format::eR32G32B32A32Sint:
        case vk::Format::eR32G32B32A32Uint:
            return 16;
        case vk::Format::eR64Sfloat:
            return 8;
        case vk::Format::eR64G64Sfloat:
            return 16;
        case vk::Format::eR64G64B64Sfloat:
            return 24;
        case vk::Format::eR64G64B64A64Sfloat:
            return 32;
        default:
            return 0;
    }
}

/**
 * @brief 为 GLSL 着色器添加类 C 预处理器宏支持，
 *        可以定义或取消定义特定符号。同时支持运行时数组大小设置。
 */
class ShaderVariant
{
  public:
    ShaderVariant() = default;

    size_t get_id() const;

    /**
     * @brief 指定命名的运行时数组大小，用于自动反射。
     *        若已指定则覆盖原大小。
     * @param runtime_array_name 着色器中的运行时数组名
     * @param size 运行时数组的元素个数（非字节数），用于自动分配缓冲区
     * @see get_declared_struct_size_runtime_array() in spirv_cross.h
     */
    void add_runtime_array_size(const std::string &runtime_array_name, size_t size);

    void set_runtime_array_sizes(const std::unordered_map<std::string, size_t> &sizes);

    const std::unordered_map<std::string, size_t> &get_runtime_array_sizes() const;

    void clear();

  private:
    size_t id;

    std::unordered_map<std::string, size_t> runtime_array_sizes;
};

/// 着色器源代码（文本或 SPIR-V 路径）包装。
class ShaderSource
{
  public:
    ShaderSource() = default;

    ShaderSource(const std::string &filename);

    size_t get_id() const;

    const std::string &get_filename() const;

    void set_source(const std::string &source);

    const std::string &get_source() const;

  private:
    size_t id;

    std::string filename;

    std::string source;
};

/**
 * @brief 包含特定着色器阶段的代码及入口点。
 * 为 PipelineLayout 创建 Pipeline 提供必需的着色器信息。
 * ShaderModule 可以自动将着色器代码与纹理做绑定配对，
 * 仅基于纹理名称修改底层绑定，并为每个纹理生成变体（如 HAS_BASE_COLOR_TEX）。
 * 属性位置也以类似方式处理。
 *
 * 当前限制：仅考虑 set 0；统一缓冲区目前是硬编码的。
 */
class ShaderModule : public VulkanResourceBase<vk::ShaderModule>
{
  public:
    using Parent = VulkanResourceBase<vk::ShaderModule>;

    ShaderModule(VulkanDevice              &device,
                 vk::ShaderStageFlagBits    stage,
                 const ShaderSource        &shader_source,
                 const std::string         &entry_point,
                 const ShaderVariant       &shader_variant);

    ShaderModule(const ShaderModule &) = delete;

    ShaderModule(ShaderModule &&other);

    ~ShaderModule();

    ShaderModule &operator=(const ShaderModule &) = delete;

    ShaderModule &operator=(ShaderModule &&) = delete;

    size_t get_id() const;

    vk::ShaderStageFlagBits get_stage() const;

    const std::string &get_entry_point() const;

    const std::vector<ShaderResource> &get_resources() const;

    const std::vector<uint32_t> &get_binary() const;

    /**
     * @brief 标记某个资源使用不同的绑定方式。
     * @param resource_name 着色器资源名
     * @param resource_mode 绑定模式
     */
    void set_resource_mode(const std::string &resource_name, const ShaderResourceMode &resource_mode);

  private:
    /// 着色器唯一 ID
    size_t id = 0;

    /// 着色器阶段（顶点、片元等）
    vk::ShaderStageFlagBits stage{};

    /// 入口函数名
    std::string entry_point;

    /// 编译后的 SPIR-V 二进制
    std::vector<uint32_t> spirv;

    /// 反射出的资源列表
    std::vector<ShaderResource> resources;
};

} // namespace GE
