//
// Created by Lenovo on 2026/6/12.
//

#include "Render/Texture.h"

#include "Core/Log.h"

#include <algorithm>

namespace GE {

Texture::~Texture() {
    Cleanup();
}

void Texture::LoadFromFile(VmaAllocator allocator,
                            vk::Queue queue, uint32_t queueFamilyIndex,
                            const std::string &filepath,
                            vk::Filter magFilter, vk::Filter minFilter,
                            vk::SamplerAddressMode addressMode) {
    // 加载图片并生成 mip chain
    m_Image.LoadFromFile(allocator, queue, queueFamilyIndex, filepath);

    // 从 allocator 反推 device（VulkanImage 内部也是这么做的）
    VmaAllocatorInfo allocator_info;
    vmaGetAllocatorInfo(allocator, &allocator_info);
    vk::Device device = allocator_info.device;

    // 自动创建匹配的 sampler（maxLod 与 mip 级数一致）
    m_Sampler.Init(device, magFilter, minFilter,
                   addressMode, static_cast<float>(m_Image.GetMipLevels()));
}

void Texture::LoadFromColor(VmaAllocator allocator,
                            vk::Queue queue, uint32_t queueFamilyIndex,
                            const glm::vec3 &color) {
    uint8_t pixel[4] = {
        static_cast<uint8_t>(std::clamp(color.r, 0.0f, 1.0f) * 255.0f),
        static_cast<uint8_t>(std::clamp(color.g, 0.0f, 1.0f) * 255.0f),
        static_cast<uint8_t>(std::clamp(color.b, 0.0f, 1.0f) * 255.0f),
        255,
    };

    m_Image.LoadFromMemory(allocator, queue, queueFamilyIndex,
                           pixel, 1, 1,
                           vk::Format::eR8G8B8A8Srgb);

    VmaAllocatorInfo allocator_info;
    vmaGetAllocatorInfo(allocator, &allocator_info);
    vk::Device device = allocator_info.device;

    m_Sampler.Init(device, vk::Filter::eLinear, vk::Filter::eLinear,
                   vk::SamplerAddressMode::eRepeat, 1.0f);
}

void Texture::Cleanup() {
    m_Sampler.Cleanup();
    m_Image.Cleanup();
}

} // namespace GE
