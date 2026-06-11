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

    /// 接管管线所有权，创建一个 descriptor set，
    /// 从 pipeline 的反射绑定信息驱动 pool 创建和 descriptor 写入。
    void Init(vk::Device device,
              VulkanPipeline &&pipeline,
              const vk::DescriptorBufferInfo &uniformBufferInfo,
              vk::ImageView textureView, vk::Sampler sampler);

    /// 按 binding 编号更新纹理。
    void SetTexture(uint32_t binding, vk::ImageView textureView, vk::Sampler sampler);

    /// 按着色器变量名更新纹理（如 "texSampler", "albedo"）。
    void SetTexture(const std::string &name, vk::ImageView textureView, vk::Sampler sampler);

    void Cleanup();
};

} // namespace GE
