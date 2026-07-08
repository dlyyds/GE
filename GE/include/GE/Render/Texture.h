#pragma once

#include <vulkan/vulkan.hpp>
#include "vk_mem_alloc.h"

#include <memory>
#include <string>

#include "glm/glm.hpp"
#include "Render/VulkanBase/VulkanHppImage.h"
#include "Render/VulkanBase/VulkanHppImageView.h"
#include "Render/VulkanBase/VulkanSampler.h"

namespace GE {

class VulkanDevice;

/// 纹理 = GPU image + view + sampler 的封装。
/// 加载图片时自动生成 mip chain，并创建与之匹配的 ImageView 和 sampler。
class Texture {
public:
    Texture() = default;
    ~Texture();

    Texture(const Texture &) = delete;
    Texture &operator=(const Texture &) = delete;

    Texture(Texture &&) = default;
    Texture &operator=(Texture &&) = default;

    /// 从文件加载纹理，自动创建 image view + sampler（mip 级数与 image 匹配）。
    /// @param device           Vulkan 设备
    /// @param queue            用于上传数据 + 生成 mip 的队列
    /// @param queueFamilyIndex 队列族索引
    /// @param filepath         图片文件路径
    /// @param magFilter        sampler 放大过滤
    /// @param minFilter        sampler 缩小过滤
    /// @param addressMode      sampler 寻址模式
    void LoadFromFile(VulkanDevice &device,
                      vk::Queue queue, uint32_t queueFamilyIndex,
                      const std::string &filepath,
                      vk::Filter magFilter = vk::Filter::eLinear,
                      vk::Filter minFilter = vk::Filter::eLinear,
                      vk::SamplerAddressMode addressMode = vk::SamplerAddressMode::eRepeat);

    /// 从纯色创建 1x1 纹理（用于 MTL 材质没有 map_Kd 时的 fallback）。
    void LoadFromColor(VulkanDevice &device,
                       vk::Queue queue, uint32_t queueFamilyIndex,
                       const glm::vec3 &color);

    /// 销毁 GPU 资源。
    void Cleanup();

private:
    /// 内部：从像素数据加载纹理（staging buffer → image → mip → view → sampler）。
    void LoadFromMemory(VulkanDevice &device,
                        vk::Queue queue, uint32_t queueFamilyIndex,
                        const void *data, uint32_t width, uint32_t height,
                        vk::Format format,
                        vk::Filter magFilter = vk::Filter::eLinear,
                        vk::Filter minFilter = vk::Filter::eLinear,
                        vk::SamplerAddressMode addressMode = vk::SamplerAddressMode::eRepeat);

public:
    [[nodiscard]] vk::ImageView GetImageView() const { return m_ImageView ? m_ImageView->GetHandle() : nullptr; }
    [[nodiscard]] vk::Sampler  GetSampler()   const { return m_Sampler.Get(); }
    [[nodiscard]] uint32_t     GetWidth()     const { return m_Image ? m_Image->get_extent().width : 0; }
    [[nodiscard]] uint32_t     GetHeight()    const { return m_Image ? m_Image->get_extent().height : 0; }
    [[nodiscard]] uint32_t     GetMipLevels() const { return m_Image ? m_Image->get_subresource().mipLevel : 0; }
    [[nodiscard]] bool         IsLoaded()     const { return m_Image && m_Image->GetHandle() != nullptr; }

private:
    std::unique_ptr<VulkanHppImage>      m_Image;
    std::unique_ptr<VulkanHppImageView>  m_ImageView;
    VulkanSampler                       m_Sampler;
};

} // namespace GE
