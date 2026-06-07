#pragma once


#include <vulkan/vulkan.hpp>

#include "VulkanDescriptorPool.h"

#include <vector>

namespace GE {

class VulkanDescriptorSet {
public:
    VulkanDescriptorSet() = default;

    ~VulkanDescriptorSet() = default;

    VulkanDescriptorSet(const VulkanDescriptorSet &) = delete;

    VulkanDescriptorSet &operator=(const VulkanDescriptorSet &) = delete;

    /// Allocate descriptor set from pool.
    void Init(vk::Device device, VulkanDescriptorPool &pool,
              vk::DescriptorSetLayout layout);

    /// Write a uniform buffer descriptor at the given binding.
    void WriteBuffer(uint32_t binding, vk::DescriptorType type,
                     vk::DescriptorBufferInfo buffer_info);

    /// Write a combined image sampler descriptor at the given binding.
    void WriteImage(uint32_t binding, vk::DescriptorImageInfo image_info,
                    vk::DescriptorType type = vk::DescriptorType::eCombinedImageSampler);

    void Destroy();

    [[nodiscard]] vk::DescriptorSet Get() const { return m_DescriptorSet; }

private:
    vk::Device m_Device = nullptr;
    vk::DescriptorSet m_DescriptorSet = nullptr;
    vk::DescriptorPool m_DescriptorPool = nullptr;
};

} // namespace GE
