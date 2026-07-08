#pragma once

#include "Render/TextureLib.h"
#include "Render/VulkanBase/VulkanHppImage.h"

namespace GE {

/// 资源管理器，集中管理纹理、着色器、网格等 GPU 资源的缓存。
///
/// 由 Application 持有和初始化，各 Layer 通过 Application::GetResourceManager() 访问。
///
/// 用法：
///   auto &res = Application::GetResourceManager();
///   VulkanHppImage *tex = res.GetTextures().Load("textures/brick.png");
class ResourceManager {
public:
    ResourceManager() = default;
    ~ResourceManager();

    ResourceManager(const ResourceManager &) = delete;
    ResourceManager &operator=(const ResourceManager &) = delete;

    /// 初始化所有资源子系统。
    void Init(VulkanDevice &device, vk::Queue queue, uint32_t queueFamilyIndex);

    /// 释放所有 GPU 资源。
    void Shutdown();

    /// 纹理库（按路径去重管理 VulkanHppImage）。
    [[nodiscard]] TextureLib &GetTextures() { return m_Textures; }

private:
    TextureLib m_Textures;
};

} // namespace GE
