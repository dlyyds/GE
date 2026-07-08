//
// Created by Lenovo on 2026/6/7.
//

#include "Render/Material.h"
#include "Render/Texture.h"

#include "Core/Log.h"

#include <algorithm>
#include <array>
#include <unordered_map>

namespace GE {

void Material::Init(vk::Device device,
                    VulkanPipeline &&pipeline) {
    this->pipeline = std::move(pipeline);
    this->Device = device;
    auto &bindings = this->pipeline.GetDescriptorBindings();

    // 只过滤 set=1 的 bindings（材质纹理）
    std::vector<vk::DescriptorPoolSize> poolSizes;
    for (auto &b : bindings) {
        if (b.set != 1) continue;
        poolSizes.push_back(vk::DescriptorPoolSize{
            .type = b.descriptorType,
            .descriptorCount = b.descriptorCount,
        });
    }
    descriptorPool.Init(device, 1, poolSizes);

    // 分配 descriptor set（使用 pipeline 中 set=1 的 layout）
    auto set_layout = this->pipeline.GetSetLayout(1);
    vk::DescriptorSetAllocateInfo alloc_info{
        .descriptorPool     = descriptorPool.Get(),
        .descriptorSetCount = 1,
        .pSetLayouts        = &set_layout,
    };
    DescriptorSet = device.allocateDescriptorSets(alloc_info).front();
}

void Material::SetTexture(uint32_t binding, vk::ImageView textureView, vk::Sampler sampler) {
    vk::DescriptorImageInfo imageInfo{
        .sampler    = sampler,
        .imageView  = textureView,
        .imageLayout = vk::ImageLayout::eShaderReadOnlyOptimal,
    };

    vk::WriteDescriptorSet write{
        .dstSet          = DescriptorSet,
        .dstBinding      = binding,
        .descriptorCount = 1,
        .descriptorType  = vk::DescriptorType::eCombinedImageSampler,
        .pImageInfo      = &imageInfo,
    };
    Device.updateDescriptorSets(write, nullptr);
}

void Material::SetTexture(const std::string &name, vk::ImageView textureView, vk::Sampler sampler) {
    uint32_t binding = pipeline.GetBindingByName(name);
    if (binding == UINT32_MAX) {
        GE_CORE_WARN("Material::SetTexture: 未找到名为 \"{}\" 的纹理 binding", name);
        return;
    }
    SetTexture(binding, textureView, sampler);
}

void Material::SetTexture(const std::string &name, const Texture &texture) {
    SetTexture(name, texture.GetImageView(), texture.GetSampler());
}

void Material::Cleanup() {
    if (Device && DescriptorSet) {
        Device.freeDescriptorSets(descriptorPool.Get(), DescriptorSet);
    }
    DescriptorSet = nullptr;
    Device        = nullptr;
    descriptorPool.Cleanup();
    pipeline.Cleanup();
}

} // namespace GE
