//
// Created by Lenovo on 2026/6/7.
//

#define VULKAN_HPP_DISPATCH_LOADER_DYNAMIC 1
#include "Render/Material.h"

#include <array>

namespace GE {

void Material::Init(vk::Device device,
                    VulkanPipeline &&pipeline,
                    uint32_t imageCount,
                    const vk::DescriptorBufferInfo *uniformBufferInfos,
                    vk::ImageView textureView, vk::Sampler sampler) {
    this->pipeline = std::move(pipeline);

    // 创建 descriptor pool（imageCount 组，每组：UBO + 纹理）
    descriptorPool.Init(device, imageCount,
                        {vk::DescriptorPoolSize{.type = vk::DescriptorType::eUniformBufferDynamic,
                                                .descriptorCount = imageCount},
                         vk::DescriptorPoolSize{.type = vk::DescriptorType::eCombinedImageSampler,
                                                .descriptorCount = imageCount}});

    // 为每个 swapchain image 分配 descriptor set
    descriptorSets.resize(imageCount);
    for (uint32_t i = 0; i < imageCount; i++) {
        descriptorSets[i].Init(device, descriptorPool, this->pipeline.GetDescriptorSetLayout());

        // 写入 binding 0 = 第 i 个 ring buffer 的动态 UBO
        descriptorSets[i].WriteBuffer(0, vk::DescriptorType::eUniformBufferDynamic,
                                      uniformBufferInfos[i]);

        // 写入 binding 1 = 纹理
        vk::DescriptorImageInfo imageInfo;
        imageInfo.sampler = sampler;
        imageInfo.imageView = textureView;
        imageInfo.imageLayout = vk::ImageLayout::eShaderReadOnlyOptimal;
        descriptorSets[i].WriteImage(1, imageInfo);
    }
}

void Material::SetTexture(vk::ImageView textureView, vk::Sampler sampler) {
    // 更新所有 descriptor set 的纹理 binding
    vk::DescriptorImageInfo imageInfo;
    imageInfo.sampler = sampler;
    imageInfo.imageView = textureView;
    imageInfo.imageLayout = vk::ImageLayout::eShaderReadOnlyOptimal;

    for (auto &ds : descriptorSets) {
        ds.WriteImage(1, imageInfo);
    }
}

void Material::Cleanup() {
    for (auto &ds : descriptorSets)
        ds.Destroy();
    descriptorSets.clear();
    descriptorPool.Cleanup();
    pipeline.Cleanup();
}

} // namespace GE
