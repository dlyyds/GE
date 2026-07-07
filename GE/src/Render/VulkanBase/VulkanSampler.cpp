//
// Created by Lenovo on 2026/6/5.
//

#include "../../../include/GE/Render/VulkanBase/VulkanSampler.h"

namespace GE {

void VulkanSampler::Init(vk::Device device,
                          vk::Filter mag_filter, vk::Filter min_filter,
                          vk::SamplerAddressMode address_mode, float max_lod) {
    m_Device = device;

    vk::SamplerCreateInfo info{
        .magFilter = mag_filter,
        .minFilter = min_filter,
        .mipmapMode = vk::SamplerMipmapMode::eLinear,
        .addressModeU = address_mode,
        .addressModeV = address_mode,
        .addressModeW = address_mode,
        .mipLodBias = 0.0f,
        .anisotropyEnable = VK_FALSE,
        .maxAnisotropy = 1.0f,
        .compareEnable = VK_FALSE,
        .compareOp = vk::CompareOp::eAlways,
        .minLod = 0.0f,
        .maxLod = max_lod,
        .borderColor = vk::BorderColor::eIntOpaqueWhite,
    };
    m_Sampler = device.createSampler(info);
}

void VulkanSampler::Cleanup() {
    if (m_Device && m_Sampler) m_Device.destroySampler(m_Sampler);
    m_Sampler = nullptr;
    m_Device = nullptr;
}

} // namespace GE
