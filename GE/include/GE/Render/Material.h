#pragma once

#include <vulkan/vulkan.hpp>
#include <string>
#include <vector>

#include "Render/VulkanBase/VulkanPipeline.h"
#include "Render/VulkanBase/VulkanDescriptorPool.h"
#include "Render/VulkanBase/VulkanDescriptorSet.h"

namespace GE {

/// Material = pipeline + N 个 descriptor set（每个 swapchain image 一个）。
/// UBO binding 指向对应 image 的 ring buffer，避免更新冲突。
struct Material {
    VulkanPipeline pipeline;
    VulkanDescriptorPool descriptorPool;
    std::vector<VulkanDescriptorSet> descriptorSets;

    /// 接管管线所有权，为每个 swapchain image 创建一个 descriptor set，
    /// 从 pipeline 的反射绑定信息驱动 pool 创建和 descriptor 写入。
    /// uniformBufferInfos 长度必须等于 imageCount。
    void Init(vk::Device device,
              VulkanPipeline &&pipeline,
              uint32_t imageCount,
              const vk::DescriptorBufferInfo *uniformBufferInfos,
              vk::ImageView textureView, vk::Sampler sampler);

    /// 按 binding 编号更新纹理。
    void SetTexture(uint32_t binding, vk::ImageView textureView, vk::Sampler sampler);

    /// 按着色器变量名更新纹理（如 "texSampler", "albedo"）。
    void SetTexture(const std::string &name, vk::ImageView textureView, vk::Sampler sampler);

    void Cleanup();
};

} // namespace GE
