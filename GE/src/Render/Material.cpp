//
// Created by Lenovo on 2026/6/7.
//

#define VULKAN_HPP_DISPATCH_LOADER_DYNAMIC 1
#include "Render/Material.h"

#include <array>

namespace GE {

void Material::Init(vk::Device device,
                    VulkanPipeline &&pipeline,
                    const vk::DescriptorBufferInfo &uniformBufferInfo,
                    vk::ImageView textureView, vk::Sampler sampler) {
    this->pipeline = std::move(pipeline);

    // Create descriptor pool (one set: UBO + texture)
    descriptorPool.Init(device, 1,
                        {vk::DescriptorPoolSize{.type = vk::DescriptorType::eUniformBuffer,
                                                .descriptorCount = 1},
                         vk::DescriptorPoolSize{.type = vk::DescriptorType::eCombinedImageSampler,
                                                .descriptorCount = 1}});

    // Allocate descriptor set
    descriptorSet.Init(device, descriptorPool, this->pipeline.GetDescriptorSetLayout());

    // Write binding 0 = UBO
    descriptorSet.WriteBuffer(0, vk::DescriptorType::eUniformBuffer, uniformBufferInfo);

    // Write binding 1 = texture
    SetTexture(textureView, sampler);
}

void Material::SetTexture(vk::ImageView textureView, vk::Sampler sampler) {
    vk::DescriptorImageInfo imageInfo;
    imageInfo.sampler = sampler;
    imageInfo.imageView = textureView;
    imageInfo.imageLayout = vk::ImageLayout::eShaderReadOnlyOptimal;
    descriptorSet.WriteImage(1, imageInfo);
}

void Material::Cleanup() {
    descriptorSet.Destroy();
    descriptorPool.Cleanup();
    pipeline.Cleanup();
}

} // namespace GE
