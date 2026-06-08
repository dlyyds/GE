#pragma once

#include <vulkan/vulkan.hpp>
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

    /// 接管管线所有权，为每个 swapchain image 创建一个 descriptor set。
    /// uniformBufferInfos 长度必须等于 imageCount。
    void Init(vk::Device device,
              VulkanPipeline &&pipeline,
              uint32_t imageCount,
              const vk::DescriptorBufferInfo *uniformBufferInfos,
              vk::ImageView textureView, vk::Sampler sampler);

    /// 更新所有 descriptor set 的纹理 binding。
    void SetTexture(vk::ImageView textureView, vk::Sampler sampler);

    void Cleanup();
};

} // namespace GE
