#pragma once

#include <vulkan/vulkan.hpp>
#include "vk_mem_alloc.h"

#include <string>
#include <unordered_map>
#include <memory>

namespace GE {

class VulkanImage;

/// 纹理库，按文件路径去重管理 GPU 纹理。
///
/// 路径会自动归一化（转为绝对路径、统一小写），
/// 确保 "tex/a.png" 与 "./tex/a.png" 指向同一缓存。
///
/// 用法：
///   TextureLib texLib;
///   texLib.Init(allocator, queue, qfi);
///
///   VulkanImage *tex = texLib.Load("textures/brick.png");
///   // ... 使用 tex ...
///   texLib.Release("textures/brick.png");
///
///   texLib.GC();  // 清理引用计数归零的纹理
///
/// 纹理的生存期由引用计数管理：
///   - Load()   递增引用计数，首次加载创建 GPU Image
///   - Release()递减引用计数
///   - GC()     销毁引用计数归零的纹理
class TextureLib {
public:
    TextureLib() = default;
    ~TextureLib();

    TextureLib(const TextureLib &) = delete;
    TextureLib &operator=(const TextureLib &) = delete;

    /// 初始化纹理库，保存设备/队列引用供后续纹理加载使用。
    /// 必须在第一次 Load() 前调用。
    void Init(VmaAllocator allocator, vk::Queue queue, uint32_t queueFamilyIndex);

    /// 加载纹理，按文件路径去重。
    /// 首次加载会创建 VulkanImage，之后直接返回缓存指针。
    /// 返回的指针在 Release() 或 GC()/Clear() 前始终有效。
    /// @param filepath 纹理文件路径（传递给 stb_image）
    /// @return VulkanImage 指针，加载失败返回 nullptr
    VulkanImage *Load(const std::string &filepath);

    /// 释放纹理引用（引用计数减一）。
    /// 引用计数归零后，纹理不会被立即销毁，等待下次 GC() 清理。
    void Release(const std::string &filepath);

    /// 清理所有引用计数归零的纹理，释放 GPU 资源。
    void GC();

    /// 清空所有缓存的纹理（强制释放所有 GPU 资源）。
    void Clear();

    /// 纹理库中的纹理总数。
    [[nodiscard]] size_t GetCount() const { return m_Textures.size(); }

    /// 检查指定路径是否已加载。
    [[nodiscard]] bool Has(const std::string &filepath) const;

private:
    /// 路径归一化：转绝对路径 + 统一小写。
    static std::string NormalizePath(const std::string &path);

    struct CachedTexture {
        std::unique_ptr<VulkanImage> image;
        uint32_t refCount = 0;
    };

    VmaAllocator m_Allocator = nullptr;
    vk::Queue m_Queue = nullptr;
    uint32_t m_QueueFamilyIndex = 0;

    std::unordered_map<std::string, std::unique_ptr<CachedTexture>> m_Textures;
};

} // namespace GE
