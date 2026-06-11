//
// Created by Lenovo on 2026/6/9.
//

#include "Render/VulkanBase/VulkanShader.h"

#include "FileSystem/FileSystem.h"
#include "Core/Log.h"

#include <spirv_cross.hpp>

#include <map>
#include <unordered_map>

namespace GE {

// ============================================================
// 类型映射表：SPIRV-Cross 类型 → vk::Format
// ============================================================
namespace {

vk::Format SpirvTypeToFormat(const spirv_cross::SPIRType &type) {
    using namespace spirv_cross;

    // 标量类型
    if (type.vecsize == 1 && type.columns == 1) {
        switch (type.basetype) {
        case SPIRType::Float: return vk::Format::eR32Sfloat;
        case SPIRType::Int: return vk::Format::eR32Sint;
        case SPIRType::UInt: return vk::Format::eR32Uint;
        case SPIRType::Double: return vk::Format::eR64Sfloat;
        default: return vk::Format::eR32Sfloat;
        }
    }

    // 向量类型
    if (type.columns == 1) {
        switch (type.basetype) {
        case SPIRType::Float: switch (type.vecsize) {
            case 2: return vk::Format::eR32G32Sfloat;
            case 3: return vk::Format::eR32G32B32Sfloat;
            case 4: return vk::Format::eR32G32B32A32Sfloat;
            default: ;
            }
            break;
        case SPIRType::Int: switch (type.vecsize) {
            case 2: return vk::Format::eR32G32Sint;
            case 3: return vk::Format::eR32G32B32Sint;
            case 4: return vk::Format::eR32G32B32A32Sint;
            default: ;
            }
            break;
        case SPIRType::UInt: switch (type.vecsize) {
            case 2: return vk::Format::eR32G32Uint;
            case 3: return vk::Format::eR32G32B32Uint;
            case 4: return vk::Format::eR32G32B32A32Uint;
            default: ;
            }
            break;
        case SPIRType::Double: switch (type.vecsize) {
            case 2: return vk::Format::eR64G64Sfloat;
            case 3: return vk::Format::eR64G64B64Sfloat;
            case 4: return vk::Format::eR64G64B64A64Sfloat;
            default: ;
            }
            break;
        default: ;
        }
    }

    // 矩阵类型：按列展开为多个 vec
    if (type.columns > 1 && type.basetype == SPIRType::Float) {
        // mat4 → 4 × vec4 → eR32G32B32A32Sfloat
        switch (type.vecsize) {
        case 2: return vk::Format::eR32G32Sfloat;
        case 3: return vk::Format::eR32G32B32Sfloat;
        case 4: return vk::Format::eR32G32B32A32Sfloat;
        default: ;
        }
    }

    GE_CORE_WARN("Unhandled SPIRV-Cross type: basetype={} vecsize={} columns={}",
                 static_cast<int>(type.basetype), type.vecsize, type.columns);
    return vk::Format::eR32G32B32A32Sfloat;
}

/// 计算 SPIRV-Cross 类型的大小（字节）。
/// 用于计算 vertex stride。
uint32_t SpirvTypeSize(const spirv_cross::SPIRType &type) {
    using namespace spirv_cross;

    uint32_t element_size = 0;
    switch (type.basetype) {
    case SPIRType::Float: element_size = 4;
        break;
    case SPIRType::Int: element_size = 4;
        break;
    case SPIRType::UInt: element_size = 4;
        break;
    case SPIRType::Double: element_size = 8;
        break;
    default: element_size = 4;
        break;
    }

    return element_size * type.vecsize * type.columns;
}

} // anonymous namespace

// ============================================================
// VulkanShader 实现
// ============================================================

VulkanShader::~VulkanShader() {
    Cleanup();
}

void VulkanShader::Init(vk::Device device, const std::string &filepath, vk::ShaderStageFlagBits stage) {
    m_Device = device;
    m_Filepath = filepath;
    m_Stage = stage;

    // 从 .spv 文件加载 SPIR-V 二进制数据
    m_SPIRV = FileSystem::ReadBinaryU32(filepath);
    if (m_SPIRV.empty()) {
        GE_CORE_ERROR("VulkanShader: failed to load SPIR-V from '{}'", filepath);
        return;
    }

    // 创建 ShaderModule
    vk::ShaderModuleCreateInfo module_info{
        .codeSize = m_SPIRV.size() * sizeof(uint32_t),
        .pCode = m_SPIRV.data(),
    };
    m_Module = device.createShaderModule(module_info);
}

void VulkanShader::Cleanup() {
    if (m_Device && m_Module) {
        m_Device.destroyShaderModule(m_Module);
        m_Module = nullptr;
    }
    m_SPIRV.clear();
    m_Device = nullptr;
    m_Filepath.clear();
}

vk::PipelineShaderStageCreateInfo VulkanShader::GetStageCreateInfo(const char *entryPoint) const {
    return vk::PipelineShaderStageCreateInfo{
        .stage = m_Stage,
        .module = m_Module,
        .pName = entryPoint,
    };
}

// ============================================================
// 反射实现
// ============================================================

std::vector<vk::VertexInputAttributeDescription> VulkanShader::ReflectVertexAttributes() const {
    std::vector<vk::VertexInputAttributeDescription> attributes;

    // 只有 vertex shader 才有 vertex input
    if (m_Stage != vk::ShaderStageFlagBits::eVertex || m_SPIRV.empty()) {
        return attributes;
    }

    try {
        spirv_cross::Compiler compiler(m_SPIRV);
        auto resources = compiler.get_shader_resources();

        // 按 location 排序
        std::map<uint32_t, vk::VertexInputAttributeDescription> sorted;

        for (auto &input : resources.stage_inputs) {
            auto &type = compiler.get_type(input.type_id);
            uint32_t location = compiler.get_decoration(input.id, spv::DecorationLocation);

            sorted[location] = vk::VertexInputAttributeDescription{
                .location = location,
                .binding = 0,
                .format = SpirvTypeToFormat(type),
                .offset = 0, // 由调用方计算 offset
            };
        }

        // 按 location 顺序输出
        for (auto &[loc, attr] : sorted) {
            attributes.push_back(attr);
        }
    } catch (const std::exception &e) {
        GE_CORE_ERROR("SPIRV-Cross reflection failed: {}", e.what());
    }

    return attributes;
}

uint32_t VulkanShader::ReflectVertexStride() const {
    if (m_Stage != vk::ShaderStageFlagBits::eVertex || m_SPIRV.empty()) {
        return 0;
    }

    try {
        spirv_cross::Compiler compiler(m_SPIRV);
        auto resources = compiler.get_shader_resources();

        uint32_t total = 0;
        for (auto &input : resources.stage_inputs) {
            auto &type = compiler.get_type(input.type_id);
            total += SpirvTypeSize(type);
        }
        return total;
    } catch (const std::exception &e) {
        GE_CORE_ERROR("SPIRV-Cross stride reflection failed: {}", e.what());
        return 0;
    }
}

VertexInputState VulkanShader::ReflectVertexInput() const {
    VertexInputState state;
    state.attributes = ReflectVertexAttributes();

    // 计算 offset：按声明顺序依次排列
    uint32_t offset = 0;
    for (auto &attr : state.attributes) {
        attr.offset = offset;
        // 根据 format 计算 size
        switch (attr.format) {
        case vk::Format::eR32Sfloat: offset += 4;
            break;
        case vk::Format::eR32G32Sfloat: offset += 8;
            break;
        case vk::Format::eR32G32B32Sfloat: offset += 12;
            break;
        case vk::Format::eR32G32B32A32Sfloat: offset += 16;
            break;
        case vk::Format::eR32Sint: offset += 4;
            break;
        case vk::Format::eR32G32Sint: offset += 8;
            break;
        case vk::Format::eR32G32B32Sint: offset += 12;
            break;
        case vk::Format::eR32G32B32A32Sint: offset += 16;
            break;
        case vk::Format::eR32Uint: offset += 4;
            break;
        case vk::Format::eR32G32Uint: offset += 8;
            break;
        case vk::Format::eR32G32B32Uint: offset += 12;
            break;
        case vk::Format::eR32G32B32A32Uint: offset += 16;
            break;
        case vk::Format::eR64Sfloat: offset += 8;
            break;
        case vk::Format::eR64G64Sfloat: offset += 16;
            break;
        case vk::Format::eR64G64B64Sfloat: offset += 24;
            break;
        case vk::Format::eR64G64B64A64Sfloat: offset += 32;
            break;
        default: offset += 16;
            break; // fallback
        }
    }
    state.stride = offset;

    return state;
}

std::vector<DescriptorBindingInfo> VulkanShader::ReflectDescriptorBindings() const {
    std::vector<DescriptorBindingInfo> bindings;
    if (m_SPIRV.empty())
        return bindings;

    try {
        spirv_cross::Compiler compiler(m_SPIRV);
        auto resources = compiler.get_shader_resources();

        // 映射 SPIRV-Cross 资源 → Vulkan descriptor type
        auto add_resources = [&](const spirv_cross::SmallVector<spirv_cross::Resource> &res_list,
                                 vk::DescriptorType type) {
            for (auto &res : res_list) {
                uint32_t binding = compiler.get_decoration(res.id, spv::DecorationBinding);
                auto &spir_type = compiler.get_type(res.type_id);

                // 数组长度：无数组 → 1, 固定数组 → array[0], runtime 数组 → 1
                uint32_t count = 1;
                if (!spir_type.array.empty() && spir_type.array[0] > 0)
                    count = spir_type.array[0];

                // res.name 对 interface block 返回的是 block 类型名（如 "UBO"），
                // 而用户期望的是实例名（如 "ubo"），用 get_name(res.id) 获取。
                std::string name = compiler.get_name(res.id);
                if (name.empty())
                    name = res.name;

                bindings.push_back(DescriptorBindingInfo{
                    .binding = binding,
                    .descriptorType = type,
                    .descriptorCount = count,
                    .stageFlags = m_Stage,
                    .name = name,
                });
            }
        };

        add_resources(resources.uniform_buffers, vk::DescriptorType::eUniformBuffer);
        add_resources(resources.sampled_images, vk::DescriptorType::eCombinedImageSampler);
        add_resources(resources.storage_buffers, vk::DescriptorType::eStorageBuffer);
        add_resources(resources.storage_images, vk::DescriptorType::eStorageImage);

    } catch (const std::exception &e) {
        GE_CORE_ERROR("SPIRV-Cross descriptor reflection failed: {}", e.what());
    }

    return bindings;
}

std::vector<DescriptorBindingInfo> VulkanShader::MergeDescriptorBindings(
    std::initializer_list<std::vector<DescriptorBindingInfo> > stages) {

    std::unordered_map<uint32_t, DescriptorBindingInfo> merged;
    for (auto &stage : stages) {
        for (auto &b : stage) {
            auto it = merged.find(b.binding);
            if (it != merged.end()) {
                it->second.stageFlags |= b.stageFlags;
                // 如果现有 name 为空则用新 name 补上（同一个 binding 可能只在一个 stage 有名字）
                if (it->second.name.empty())
                    it->second.name = b.name;
            } else {
                merged[b.binding] = b;
            }
        }
    }

    std::vector<DescriptorBindingInfo> result;
    for (auto &[_, b] : merged)
        result.push_back(b);

    // 日志输出合并结果
    GE_CORE_TRACE("MergeDescriptorBindings: {} bindings merged", result.size());
    for (auto &b : result) {
        GE_CORE_TRACE("  binding={} name=\"{}\" type={} count={} stageFlags={}",
                      b.binding, b.name, vk::to_string(b.descriptorType),
                      b.descriptorCount, vk::to_string(b.stageFlags));
    }

    return result;
}

} // namespace GE
