#pragma once

#include <vulkan/vulkan.hpp>

#include <string>

namespace GE {

/// Vulkan 着色器封装。
/// 从 .spv 文件加载 SPIR-V 二进制数据，创建并拥有 vk::ShaderModule。
/// 可用于创建 vk::PipelineShaderStageCreateInfo 供管线创建使用。
class VulkanShader {
public:
    VulkanShader() = default;

    ~VulkanShader();

    VulkanShader(const VulkanShader &) = delete;
    VulkanShader &operator=(const VulkanShader &) = delete;

    VulkanShader(VulkanShader &&) = default;
    VulkanShader &operator=(VulkanShader &&) = default;

    /// 从 .spv 文件加载着色器。
    /// @param device    Vulkan 逻辑设备
    /// @param filepath  .spv 文件的完整路径
    /// @param stage     着色器阶段（顶点、片元等）
    void Init(vk::Device device, const std::string &filepath, vk::ShaderStageFlagBits stage);

    /// 销毁 ShaderModule。
    void Cleanup();

    /// 获取用于管线创建的 ShaderStageCreateInfo。
    /// @param entryPoint 入口函数名（默认 "main"）
    [[nodiscard]] vk::PipelineShaderStageCreateInfo GetStageCreateInfo(
        const char *entryPoint = "main") const;

    [[nodiscard]] vk::ShaderModule GetModule() const { return m_Module; }
    [[nodiscard]] vk::ShaderStageFlagBits GetStage() const { return m_Stage; }
    [[nodiscard]] const std::string &GetFilepath() const { return m_Filepath; }

private:
    vk::Device m_Device = nullptr;
    vk::ShaderModule m_Module = nullptr;
    vk::ShaderStageFlagBits m_Stage = vk::ShaderStageFlagBits::eVertex;
    std::string m_Filepath;
};

} // namespace GE
