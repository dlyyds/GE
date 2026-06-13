#pragma once

#include <vulkan/vulkan.hpp>
#include <string>
#include <vector>

#include "Render/VulkanBase/VulkanPipeline.h"
#include "Render/VulkanBase/VulkanDescriptorPool.h"
#include "Render/VulkanBase/VulkanDescriptorSet.h"

namespace GE {

class Texture; // 前置声明

/// Material = pipeline + 一个 descriptor set（set=1，材质纹理）。
/// set=0（FrameUBO）和 set=2（ObjectUBO）由 Renderer 管理。
struct Material {
    VulkanPipeline pipeline;
    VulkanDescriptorPool descriptorPool;
    VulkanDescriptorSet descriptorSet;

    /// 接管管线所有权，从 pipeline 的 set=1 bindings 创建 descriptor pool 并分配 descriptor set。
    /// 不写入任何 descriptor——由 SetTexture 完成。
    void Init(vk::Device device,
              VulkanPipeline &&pipeline);

    /// 按 binding 编号更新纹理。
    void SetTexture(uint32_t binding, vk::ImageView textureView, vk::Sampler sampler);

    /// 按着色器变量名更新纹理（如 "texSampler", "albedo"）。
    void SetTexture(const std::string &name, vk::ImageView textureView, vk::Sampler sampler);

    /// 直接用 Texture 对象设置纹理（绑定由着色器变量名指定）。
    void SetTexture(const std::string &name, const Texture &texture);

    void Cleanup();
};

} // namespace GE
