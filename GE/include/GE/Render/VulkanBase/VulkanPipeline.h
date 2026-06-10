#pragma once


#include <vulkan/vulkan.hpp>

#include <array>
#include <string>
#include <vector>

#include "Render/VulkanBase/VulkanShader.h"

namespace GE {

/// Vulkan 图形管线封装。
/// 通过 VulkanShader 对象提供着色器阶段，着色器模块由调用方管理生命周期。
class VulkanPipeline {
public:
    VulkanPipeline() = default;

    ~VulkanPipeline() = default;

    VulkanPipeline(const VulkanPipeline &) = delete;
    VulkanPipeline &operator=(const VulkanPipeline &) = delete;

    VulkanPipeline(VulkanPipeline &&) = default;
    VulkanPipeline &operator=(VulkanPipeline &&) = default;

    /// 创建图形管线。vertex input layout 从 vertex shader 自动反射获取，
    /// descriptor set layout 从两个 shader 的反射结果合并创建。
    /// @param device           Vulkan 逻辑设备
    /// @param color_format     颜色附件格式
    /// @param vertShader       顶点着色器（内部自动反射 vertex input）
    /// @param fragShader       片元着色器
    /// @param dynamicBindings  需要改为 Dynamic 类型的 binding 编号列表
    void Init(vk::Device device, vk::Format color_format,
              const VulkanShader &vertShader, const VulkanShader &fragShader,
              const std::vector<uint32_t> &dynamicBindings = {});

    void Cleanup();

    void Bind(vk::CommandBuffer cmd) const { cmd.bindPipeline(vk::PipelineBindPoint::eGraphics, m_Pipeline); }

    [[nodiscard]] vk::Pipeline GetPipeline() const { return m_Pipeline; }
    [[nodiscard]] vk::PipelineLayout GetLayout() const { return m_PipelineLayout; }
    [[nodiscard]] vk::DescriptorSetLayout GetDescriptorSetLayout() const { return m_DescriptorSetLayout; }
    [[nodiscard]] const std::vector<DescriptorBindingInfo> &GetDescriptorBindings() const { return m_DescriptorBindings; }

    /// 通过着色器变量名查找 descriptor binding 编号，未找到返回 UINT32_MAX。
    [[nodiscard]] uint32_t GetBindingByName(const std::string &name) const;

    /// Takes ownership of the layout handle. Destroys any previously owned layout.
    void SetDescriptorSetLayout(vk::DescriptorSetLayout layout);

private:
    vk::Device m_Device = nullptr;
    vk::DescriptorSetLayout m_DescriptorSetLayout = nullptr;
    vk::PipelineLayout m_PipelineLayout = nullptr;
    vk::Pipeline m_Pipeline = nullptr;
    std::vector<DescriptorBindingInfo> m_DescriptorBindings;
};

} // namespace GE
