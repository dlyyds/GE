#pragma once

#include <vulkan/vulkan.hpp>
#include <string>
#include <vector>

#include "Render/VulkanBase/VulkanPipeline.h"
#include "Render/VulkanBase/VulkanDescriptorPool.h"
#include "Render/VulkanBase/VulkanDescriptorSet.h"

namespace GE {

/// Material = pipeline + 一个 descriptor set。
/// UBO binding 通过 dynamic offset 指向 ring buffer 中的不同区域。
struct Material {
    VulkanPipeline pipeline;
    VulkanDescriptorPool descriptorPool;
    VulkanDescriptorSet descriptorSet;

    /// 接管管线所有权，从反射的 binding 信息创建 descriptor pool 并分配 descriptor set。
    /// 不写入任何 descriptor——由 SetUniformBuffer / SetTexture 完成。
    void Init(vk::Device device,
              VulkanPipeline &&pipeline);

    /// 按 binding 编号更新 uniform buffer 信息。
    void SetUniformBuffer(uint32_t binding, const vk::DescriptorBufferInfo &bufferInfo);

    /// 按着色器变量名更新 uniform buffer 信息。
    void SetUniformBuffer(const std::string &name, const vk::DescriptorBufferInfo &bufferInfo);

    /// 按 binding 编号更新纹理。
    void SetTexture(uint32_t binding, vk::ImageView textureView, vk::Sampler sampler);

    /// 按着色器变量名更新纹理（如 "texSampler", "albedo"）。
    void SetTexture(const std::string &name, vk::ImageView textureView, vk::Sampler sampler);

    void Cleanup();
};

} // namespace GE
