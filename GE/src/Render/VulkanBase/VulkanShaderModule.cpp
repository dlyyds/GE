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
 * @file VulkanShaderModule.cpp
 * @brief VulkanShaderModule 实现，从 Vulkan-Samples 适配。
 */

#include "Render/VulkanBase/VulkanShaderModule.h"

#include "Render/SPIRVReflection.h"
#include "Render/VulkanBase/VulkanDevice.h"
#include "FileSystem/FileSystem.h"
#include "Core/Log.h"

#include <fstream>
#include <sstream>

namespace GE
{
// ============================================================
// 辅助：读取文本文件内容
// ============================================================
namespace
{

std::string read_text_file(const std::string &filename)
{
    std::ifstream file(filename, std::ios::in);
    if (!file.is_open())
    {
        GE_CORE_ERROR("Failed to open text file: {}", filename);
        return {};
    }
    std::stringstream buffer;
    buffer << file.rdbuf();
    return buffer.str();
}

} // anonymous namespace

// ============================================================
// VulkanShaderModule
// ============================================================

VulkanShaderModule::VulkanShaderModule(VulkanDevice            &device,
                                       vk::ShaderStageFlagBits  stage,
                                       const ShaderSource      &shader_source,
                                       const std::string       &entry_point,
                                       const ShaderVariant     &shader_variant) :
    Parent(vk::ShaderModule{}, &device), stage{stage}, entry_point{entry_point}
{
    std::string debug_name = fmt::format("{} [variant {:X}] [entrypoint {}]",
                                         shader_source.get_filename(),
                                         shader_variant.get_id(),
                                         entry_point);

    // 从 .spv 文件加载 SPIR-V 二进制
    spirv = FileSystem::ReadBinaryU32(shader_source.get_filename());

    // 使用 SPIRV-Cross 反射动态创建 descriptor binding 信息
    SPIRVReflection spirv_reflection;
    if (!spirv_reflection.reflect_shader_resources(stage, spirv, resources, shader_variant))
    {
        throw std::runtime_error("SPIR-V reflection failed for shader: " + shader_source.get_filename());
    }

    // 根据 SPIR-V 内容生成唯一 ID
    std::hash<std::string> hasher{};
    id = hasher(std::string{reinterpret_cast<const char *>(spirv.data()),
                            reinterpret_cast<const char *>(spirv.data() + spirv.size())});

    // 创建 Vulkan ShaderModule 句柄
    vk::ShaderModuleCreateInfo moduleCI{
        .codeSize = spirv.size() * sizeof(uint32_t),
        .pCode    = spirv.data(),
    };
    SetHandle(GetDevice().GetHandle().createShaderModule(moduleCI));

    // 设置调试名（创建句柄后立即设置，确保调试工具可见）
    SetDebugName(debug_name);
}

VulkanShaderModule::~VulkanShaderModule()
{
    if (HasHandle())
    {
        GetDevice().GetHandle().destroyShaderModule(GetHandle());
    }
}

VulkanShaderModule::VulkanShaderModule(VulkanShaderModule &&other) :
    Parent(std::move(other)),
    id{other.id},
    stage{other.stage},
    entry_point{std::move(other.entry_point)},
    spirv{std::move(other.spirv)},
    resources{std::move(other.resources)}
{
    other.id    = 0;
    other.stage = {};
}

size_t VulkanShaderModule::get_id() const
{
    return id;
}

vk::ShaderStageFlagBits VulkanShaderModule::get_stage() const
{
    return stage;
}

const std::string &VulkanShaderModule::get_entry_point() const
{
    return entry_point;
}

const std::vector<ShaderResource> &VulkanShaderModule::get_resources() const
{
    return resources;
}

const std::vector<uint32_t> &VulkanShaderModule::get_binary() const
{
    return spirv;
}

void VulkanShaderModule::set_resource_mode(const std::string &resource_name, const ShaderResourceMode &resource_mode)
{
    auto it = std::ranges::find_if(resources, [&resource_name](const ShaderResource &resource) { return resource.name == resource_name; });

    if (it != resources.end())
    {
        if (resource_mode == ShaderResourceMode::Dynamic)
        {
            if (it->type == ShaderResourceType::BufferUniform || it->type == ShaderResourceType::BufferStorage)
            {
                it->mode = resource_mode;
            }
            else
            {
                GE_CORE_WARN("Resource `{}` does not support dynamic.", resource_name);
            }
        }
        else
        {
            it->mode = resource_mode;
        }
    }
    else
    {
        GE_CORE_WARN("Resource `{}` not found for shader.", resource_name);
    }
}

// ============================================================
// ShaderVariant
// ============================================================

size_t ShaderVariant::get_id() const
{
    return id;
}

void ShaderVariant::add_runtime_array_size(const std::string &runtime_array_name, size_t size)
{
    if (runtime_array_sizes.find(runtime_array_name) == runtime_array_sizes.end())
    {
        runtime_array_sizes.insert({runtime_array_name, size});
    }
    else
    {
        runtime_array_sizes[runtime_array_name] = size;
    }
}

void ShaderVariant::set_runtime_array_sizes(const std::unordered_map<std::string, size_t> &sizes)
{
    this->runtime_array_sizes = sizes;
}

const std::unordered_map<std::string, size_t> &ShaderVariant::get_runtime_array_sizes() const
{
    return runtime_array_sizes;
}

void ShaderVariant::clear()
{
    runtime_array_sizes.clear();
    id = 0;
}

// ============================================================
// ShaderSource
// ============================================================

ShaderSource::ShaderSource(const std::string &filename) :
    filename{filename},
    source{read_text_file(filename)}
{
    std::hash<std::string> hasher{};
    id = hasher(std::string{this->source.cbegin(), this->source.cend()});
}

size_t ShaderSource::get_id() const
{
    return id;
}

const std::string &ShaderSource::get_filename() const
{
    return filename;
}

void ShaderSource::set_source(const std::string &source_)
{
    source = source_;
    std::hash<std::string> hasher{};
    id = hasher(std::string{this->source.cbegin(), this->source.cend()});
}

const std::string &ShaderSource::get_source() const
{
    return source;
}

} // namespace GE
