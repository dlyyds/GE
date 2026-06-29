#pragma once

#include <vulkan/vulkan.hpp>
#include "vk_mem_alloc.h"

#include <string>

#include "glm/glm.hpp"
#include "Render/VulkanBase/VulkanImage.h"
#include "Render/VulkanBase/VulkanSampler.h"

namespace GE {

/// 纹理 = GPU image + sampler 的封装。
/// 加载图片时自动生成 mip chain，并创建与之匹配的 sampler。
class Texture {
public:
    Texture() = default;
    ~Texture();

    Texture(const Texture &) = delete;
    Texture &operator=(const Texture &) = delete;

    Texture(Texture &&) = default;
    Texture &operator=(Texture &&) = default;

    /// 从文件加载纹理，自动创建 sampler（mip 级数与 image 匹配）。
    /// @param allocator         VMA allocator
    /// @param queue             用于上传数据 + 生成 mip 的队列
    /// @param queueFamilyIndex  队列族索引
    /// @param filepath          图片文件路径
    /// @param magFilter         sampler 放大过滤
    /// @param minFilter         sampler 缩小过滤
    /// @param addressMode       sampler 寻址模式
    void LoadFromFile(VmaAllocator allocator,
                      vk::Queue queue, uint32_t queueFamilyIndex,
                      const std::string &filepath,
                      vk::Filter magFilter = vk::Filter::eLinear,
                      vk::Filter minFilter = vk::Filter::eLinear,
                      vk::SamplerAddressMode addressMode = vk::SamplerAddressMode::eRepeat);

    /// 从纯色创建 1x1 纹理（用于 MTL 材质没有 map_Kd 时的 fallback）。
    void LoadFromColor(VmaAllocator allocator,
                       vk::Queue queue, uint32_t queueFamilyIndex,
                       const glm::vec3 &color);

    /// 销毁 GPU 资源。
    void Cleanup();

    [[nodiscard]] vk::ImageView GetImageView() const { return m_Image.GetView(); }
    [[nodiscard]] vk::Sampler  GetSampler()   const { return m_Sampler.Get(); }
    [[nodiscard]] uint32_t     GetWidth()     const { return m_Image.GetWidth(); }
    [[nodiscard]] uint32_t     GetHeight()    const { return m_Image.GetHeight(); }
    [[nodiscard]] uint32_t     GetMipLevels() const { return m_Image.GetMipLevels(); }
    [[nodiscard]] bool         IsLoaded()     const { return m_Image.GetImage() != nullptr; }

private:
    VulkanImage   m_Image;
    VulkanSampler m_Sampler;
};

} // namespace GE
