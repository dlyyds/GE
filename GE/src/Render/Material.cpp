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
                    VulkanPipeline &&pipeline) {
    this->pipeline = std::move(pipeline);
    auto &bindings = this->pipeline.GetDescriptorBindings();

    // 从反射的 bindings 构建 pool sizes
    std::vector<vk::DescriptorPoolSize> poolSizes;
    for (auto &b : bindings) {
        poolSizes.push_back(vk::DescriptorPoolSize{
            .type = b.descriptorType,
            .descriptorCount = b.descriptorCount,
        });
    }
    descriptorPool.Init(device, 1, poolSizes);

    // 分配单个 descriptor set
    descriptorSet.Init(device, descriptorPool, this->pipeline.GetDescriptorSetLayout());
}

void Material::SetUniformBuffer(uint32_t binding, const vk::DescriptorBufferInfo &bufferInfo) {
    auto &bindings = pipeline.GetDescriptorBindings();
    for (auto &b : bindings) {
        if (b.binding == binding) {
            if (b.descriptorType == vk::DescriptorType::eUniformBuffer ||
                b.descriptorType == vk::DescriptorType::eUniformBufferDynamic ||
                b.descriptorType == vk::DescriptorType::eStorageBuffer ||
                b.descriptorType == vk::DescriptorType::eStorageBufferDynamic) {
                descriptorSet.WriteBuffer(binding, b.descriptorType, bufferInfo);
                return;
            }
            GE_CORE_WARN("Material::SetUniformBuffer: binding {} 不是 buffer 类型", binding);
            return;
        }
    }
    GE_CORE_WARN("Material::SetUniformBuffer: 未找到 binding {}", binding);
}

void Material::SetUniformBuffer(const std::string &name, const vk::DescriptorBufferInfo &bufferInfo) {
    uint32_t binding = pipeline.GetBindingByName(name);
    if (binding == UINT32_MAX) {
        GE_CORE_WARN("Material::SetUniformBuffer: 未找到名为 \"{}\" 的 binding", name);
        return;
    }
    SetUniformBuffer(binding, bufferInfo);
}

void Material::SetTexture(uint32_t binding, vk::ImageView textureView, vk::Sampler sampler) {
    vk::DescriptorImageInfo imageInfo{
        .sampler = sampler,
        .imageView = textureView,
        .imageLayout = vk::ImageLayout::eShaderReadOnlyOptimal,
    };

    descriptorSet.WriteImage(binding, imageInfo);
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
    descriptorSet.Destroy();
    descriptorPool.Cleanup();
    pipeline.Cleanup();
}

} // namespace GE
