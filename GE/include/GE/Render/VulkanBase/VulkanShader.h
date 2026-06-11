#pragma once

#include <vulkan/vulkan.hpp>

#include <string>
#include <vector>

namespace GE {

struct VertexInputState {
    uint32_t stride;
    std::vector<vk::VertexInputAttributeDescription> attributes;
};

/// Descriptor binding 信息（扩展 vk::DescriptorSetLayoutBinding，增加反射名称）。
struct DescriptorBindingInfo {
    uint32_t binding = 0;
    uint32_t set = 0;
    vk::DescriptorType descriptorType = vk::DescriptorType::eCombinedImageSampler;
    uint32_t descriptorCount = 0;
    vk::ShaderStageFlags stageFlags;
    std::string name;  // 着色器变量名, 如 "albedo", "normal"
};

/// Vulkan 着色器封装。
/// 从 .spv 文件加载 SPIR-V 二进制数据，创建并拥有 vk::ShaderModule。
/// 支持 SPIRV-Cross 反射获取 vertex input 属性。
class VulkanShader {
public:
    VulkanShader() = default;

    ~VulkanShader();

    VulkanShader(const VulkanShader &) = delete;
    VulkanShader &operator=(const VulkanShader &) = delete;

    VulkanShader(VulkanShader &&) = default;
    VulkanShader &operator=(VulkanShader &&) = default;

    /// 从 .spv 文件加载着色器。
    void Init(vk::Device device, const std::string &filepath, vk::ShaderStageFlagBits stage);

    /// 销毁 ShaderModule。
    void Cleanup();

    /// 获取用于管线创建的 ShaderStageCreateInfo。
    [[nodiscard]] vk::PipelineShaderStageCreateInfo GetStageCreateInfo(
        const char *entryPoint = "main") const;

    /// --- 反射接口 ---

    /// 反射 vertex shader 的输入属性列表。
    /// 仅对 vertex stage 有意义，其他 stage 返回空列表。
    [[nodiscard]] std::vector<vk::VertexInputAttributeDescription> ReflectVertexAttributes() const;

    /// 计算 vertex shader 输入属性的总 stride。
    /// 将所有 attribute 的 size 累加计算。
    [[nodiscard]] uint32_t ReflectVertexStride() const;

    /// 便捷方法：反射 VertexInputState（attributes + stride）。
    [[nodiscard]] VertexInputState ReflectVertexInput() const;

    /// 反射当前阶段的所有 descriptor set layout bindings。
    /// 包括 uniform buffer、sampled image、storage buffer、storage image。
    [[nodiscard]] std::vector<DescriptorBindingInfo> ReflectDescriptorBindings() const;

    /// 合并来自多个 shader stage 的 descriptor bindings。
    /// 相同 binding 编号的 stageFlags 取并集，name 取非空的那个。
    [[nodiscard]] static std::vector<DescriptorBindingInfo> MergeDescriptorBindings(
        std::initializer_list<std::vector<DescriptorBindingInfo>> stages);

    // -- 访问器 --
    [[nodiscard]] vk::ShaderModule GetModule() const { return m_Module; }
    [[nodiscard]] vk::ShaderStageFlagBits GetStage() const { return m_Stage; }
    [[nodiscard]] const std::string &GetFilepath() const { return m_Filepath; }

private:
    vk::Device m_Device = nullptr;
    vk::ShaderModule m_Module = nullptr;
    vk::ShaderStageFlagBits m_Stage = vk::ShaderStageFlagBits::eVertex;
    std::string m_Filepath;
    std::vector<uint32_t> m_SPIRV;  // 保留 SPIR-V 数据，供反射使用
};

} // namespace GE
