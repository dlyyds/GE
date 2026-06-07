#pragma once

#include <vulkan/vulkan.hpp>

#include "Render/VulkanBase/VulkanPipeline.h"
#include "Render/VulkanBase/VulkanDescriptorPool.h"
#include "Render/VulkanBase/VulkanDescriptorSet.h"

namespace GE {

/// Material = pipeline + descriptor set with texture.
/// The UBO binding always points to the renderer's uniform buffer
/// (the buffer is written per-draw by Renderer2D, no per-material update needed).
struct Material {
    VulkanPipeline pipeline;
    VulkanDescriptorPool descriptorPool;
    VulkanDescriptorSet descriptorSet;

    /// Take ownership of a pipeline and create a descriptor set with texture.
    /// uniformBufferInfo points to the renderer's shared UBO (binding 0).
    void Init(vk::Device device,
              VulkanPipeline &&pipeline,
              const vk::DescriptorBufferInfo &uniformBufferInfo,
              vk::ImageView textureView, vk::Sampler sampler);

    /// Update just the texture binding without recreating the whole material.
    void SetTexture(vk::ImageView textureView, vk::Sampler sampler);

    void Cleanup();
};

} // namespace GE
