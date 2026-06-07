#pragma once


#include <vulkan/vulkan.hpp>

namespace GE {

class VulkanSampler {
public:
    VulkanSampler() = default;

    ~VulkanSampler() = default;

    VulkanSampler(const VulkanSampler &) = delete;

    VulkanSampler &operator=(const VulkanSampler &) = delete;

    void Init(vk::Device device,
              vk::Filter mag_filter = vk::Filter::eLinear,
              vk::Filter min_filter = vk::Filter::eLinear,
              vk::SamplerAddressMode address_mode = vk::SamplerAddressMode::eRepeat,
              float max_lod = 1.0f);

    void Cleanup();

    [[nodiscard]] vk::Sampler Get() const { return m_Sampler; }

private:
    vk::Device m_Device = nullptr;
    vk::Sampler m_Sampler = nullptr;
};

} // namespace GE
