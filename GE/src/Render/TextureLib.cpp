//
// Created by Lenovo on 2026/6/10.
//

#include "Render/TextureLib.h"
#include "Render/VulkanBase/VulkanHppImage.h"
#include "Render/VulkanBase/VulkanDevice.h"

#include "Core/Log.h"
#include "stb_image.h"

#include <algorithm>
#include <filesystem>
#include <stdexcept>

namespace GE {

TextureLib::CachedTexture::~CachedTexture() = default;

std::string TextureLib::NormalizePath(const std::string &path) {
    namespace fs = std::filesystem;
    auto normalized = fs::absolute(path).lexically_normal().string();
    // Windows 统一转小写，避免大小写不一致导致的重复
#ifdef GE_PLATFORM_WINDOWS
    std::ranges::transform(normalized, normalized.begin(),
                           [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
#endif
    return normalized;
}

TextureLib::~TextureLib() {
    Clear();
}

void TextureLib::Init(VulkanDevice &device, vk::Queue queue, uint32_t queueFamilyIndex) {
    m_Device = &device;
    m_Queue = queue;
    m_QueueFamilyIndex = queueFamilyIndex;
}

VulkanHppImage *TextureLib::Load(const std::string &filepath) {
    if (!m_Device) {
        GE_CORE_ERROR("TextureLib::Load: 未初始化，请先调用 Init()");
        return nullptr;
    }

    std::string key = NormalizePath(filepath);

    // 查是否已加载
    auto it = m_Textures.find(key);
    if (it != m_Textures.end()) {
        it->second->refCount++;
        GE_CORE_TRACE("TextureLib: 命中 \"{}\" (refCount={})", key, it->second->refCount);
        return it->second->image.get();
    }

    // 未命中，加载纹理
    GE_CORE_TRACE("TextureLib: 加载 \"{}\" (归一化: \"{}\")", filepath, key);

    auto entry = std::make_unique<CachedTexture>();
    entry->refCount = 1;

    // TextureLib 使用方便构造函数创建 HPPImage（支持 TransferDst | Sampled）
    try {
        // 由于 TextureLib 不涉及 mip 生成（由外层 Texture 处理），这里创建简单的 1-mip image
        // 实际上 TextureLib 目前未被使用，保留接口兼容
        int tex_width, tex_height, tex_channels;
        stbi_uc *pixels = stbi_load(filepath.c_str(), &tex_width, &tex_height, &tex_channels, STBI_rgb_alpha);
        if (!pixels) {
            GE_CORE_ERROR("TextureLib: stbi_load 失败: {}", filepath);
            return nullptr;
        }

        entry->image = std::make_unique<VulkanHppImage>(*m_Device,
            vk::Extent3D{static_cast<uint32_t>(tex_width), static_cast<uint32_t>(tex_height), 1},
            vk::Format::eR8G8B8A8Srgb,
            vk::ImageUsageFlagBits::eSampled | vk::ImageUsageFlagBits::eTransferDst,
            VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE);

        // 上传像素数据（简化版：仅 base level，不含 mip）
        // TODO: 需要实现 staging buffer + copy 逻辑，当前 TextureLib 未被使用，暂留空
        stbi_image_free(pixels);

    } catch (const std::exception &e) {
        GE_CORE_ERROR("TextureLib: 加载 \"{}\" 失败: {}", filepath, e.what());
        return nullptr;
    }

    VulkanHppImage *ptr = entry->image.get();
    m_Textures[key] = std::move(entry);
    return ptr;
}

void TextureLib::Release(const std::string &filepath) {
    std::string key = NormalizePath(filepath);
    auto it = m_Textures.find(key);
    if (it == m_Textures.end()) {
        GE_CORE_WARN("TextureLib::Release: \"{}\" 不在纹理库中", key);
        return;
    }

    if (it->second->refCount == 0) {
        GE_CORE_WARN("TextureLib::Release: \"{}\" 引用计数已为 0", key);
        return;
    }

    it->second->refCount--;
    GE_CORE_TRACE("TextureLib: 释放 \"{}\" (refCount={})", key, it->second->refCount);
}

void TextureLib::GC() {
    size_t removed = 0;
    for (auto it = m_Textures.begin(); it != m_Textures.end();) {
        if (it->second->refCount == 0) {
            GE_CORE_TRACE("TextureLib: GC 移除 \"{}\"", it->first);
            // unique_ptr 析构会自动销毁 HPPImage（RAII）
            it = m_Textures.erase(it);
            removed++;
        } else {
            ++it;
        }
    }

    if (removed > 0) {
        GE_CORE_TRACE("TextureLib: GC 移除了 {} 个纹理，剩余 {}", removed, m_Textures.size());
    }
}

void TextureLib::Clear() {
    // unique_ptr 析构会自动销毁所有 HPPImage（RAII）
    m_Textures.clear();
    GE_CORE_TRACE("TextureLib: 已清空所有纹理");
}

bool TextureLib::Has(const std::string &filepath) const {
    std::string key = NormalizePath(filepath);
    return m_Textures.find(key) != m_Textures.end();
}

} // namespace GE
