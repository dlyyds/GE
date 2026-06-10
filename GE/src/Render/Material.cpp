//
// Created by Lenovo on 2026/6/7.
//

#define VULKAN_HPP_DISPATCH_LOADER_DYNAMIC 1
#include "Render/Material.h"

#include "Core/Log.h"

#include <array>
#include <unordered_map>

namespace GE {

void Material::Init(vk::Device device,
                    VulkanPipeline &&pipeline,
                    uint32_t imageCount,
                    const vk::DescriptorBufferInfo *uniformBufferInfos,
                    vk::ImageView textureView, vk::Sampler sampler) {
    this->pipeline = std::move(pipeline);
    auto &bindings = this->pipeline.GetDescriptorBindings();

    // 从反射的 bindings 构建 pool sizes
    std::vector<vk::DescriptorPoolSize> poolSizes;
    for (auto &b : bindings) {
        poolSizes.push_back(vk::DescriptorPoolSize{
            .type = b.descriptorType,
            .descriptorCount = b.descriptorCount * imageCount,
        });
    }
    descriptorPool.Init(device, imageCount, poolSizes);

    // 为每个 swapchain image 分配 descriptor set
    descriptorSets.resize(imageCount);
    for (uint32_t i = 0; i < imageCount; i++) {
        descriptorSets[i].Init(device, descriptorPool, this->pipeline.GetDescriptorSetLayout());

        // 遍历反射的 bindings，按类型写入
        for (auto &b : bindings) {
            switch (b.descriptorType) {
            case vk::DescriptorType::eUniformBuffer:
            case vk::DescriptorType::eUniformBufferDynamic: {
                descriptorSets[i].WriteBuffer(b.binding, b.descriptorType,
                                              uniformBufferInfos[i]);
                break;
            }
            case vk::DescriptorType::eCombinedImageSampler: {
                vk::DescriptorImageInfo imageInfo{
                    .sampler = sampler,
                    .imageView = textureView,
                    .imageLayout = vk::ImageLayout::eShaderReadOnlyOptimal,
                };
                descriptorSets[i].WriteImage(b.binding, imageInfo, b.descriptorType);
                break;
            }
            default:
                GE_CORE_WARN("Material::Init: unhandled descriptor type {} at binding {}",
                             static_cast<int>(b.descriptorType), b.binding);
                break;
            }
        }
    }
}

void Material::SetTexture(uint32_t binding, vk::ImageView textureView, vk::Sampler sampler) {
    vk::DescriptorImageInfo imageInfo{
        .sampler = sampler,
        .imageView = textureView,
        .imageLayout = vk::ImageLayout::eShaderReadOnlyOptimal,
    };

    for (auto &ds : descriptorSets) {
        ds.WriteImage(binding, imageInfo);
    }
}

void Material::SetTexture(const std::string &name, vk::ImageView textureView, vk::Sampler sampler) {
    uint32_t binding = pipeline.GetBindingByName(name);
    if (binding == UINT32_MAX) {
        GE_CORE_WARN("Material::SetTexture: 未找到名为 \"{}\" 的纹理 binding", name);
        return;
    }
    SetTexture(binding, textureView, sampler);
}

void Material::Cleanup() {
    for (auto &ds : descriptorSets)
        ds.Destroy();
    descriptorSets.clear();
    descriptorPool.Cleanup();
    pipeline.Cleanup();
}

} // namespace GE
