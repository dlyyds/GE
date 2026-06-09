//
// Created by Lenovo on 2026/6/9.
//

#include "Render/VulkanBase/VulkanShader.h"

#include "FileSystem/FileSystem.h"
#include "Core/Log.h"

namespace GE {

VulkanShader::~VulkanShader() {
    Cleanup();
}

void VulkanShader::Init(vk::Device device, const std::string &filepath, vk::ShaderStageFlagBits stage) {
    m_Device = device;
    m_Filepath = filepath;
    m_Stage = stage;

    // 从 .spv 文件加载 SPIR-V 二进制数据
    auto spirv = FileSystem::ReadBinaryU32(filepath);
    if (spirv.empty()) {
        GE_CORE_ERROR("VulkanShader: failed to load SPIR-V from '{}'", filepath);
        return;
    }

    // 创建 ShaderModule
    vk::ShaderModuleCreateInfo module_info{
        .codeSize = spirv.size() * sizeof(uint32_t),
        .pCode = spirv.data(),
    };
    m_Module = device.createShaderModule(module_info);
}

void VulkanShader::Cleanup() {
    if (m_Device && m_Module) {
        m_Device.destroyShaderModule(m_Module);
        m_Module = nullptr;
    }
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

} // namespace GE
