//
// Created by Lenovo on 2026/6/10.
//

#include "Render/TextureLib.h"
#include "Render/VulkanBase/VulkanImage.h"

#include "Core/Log.h"

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

void TextureLib::Init(VmaAllocator allocator, vk::Queue queue, uint32_t queueFamilyIndex) {
    m_Allocator = allocator;
    m_Queue = queue;
    m_QueueFamilyIndex = queueFamilyIndex;
}

VulkanImage *TextureLib::Load(const std::string &filepath) {
    if (!m_Allocator) {
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
    entry->image = std::make_unique<VulkanImage>();
    entry->refCount = 1;

    try {
        entry->image->LoadFromFile(m_Allocator, m_Queue, m_QueueFamilyIndex, filepath);
    } catch (const std::exception &e) {
        GE_CORE_ERROR("TextureLib: 加载 \"{}\" 失败: {}", filepath, e.what());
        return nullptr;
    }

    VulkanImage *ptr = entry->image.get();
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
            it->second->image->Cleanup();
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
    for (auto &[path, entry] : m_Textures) {
        entry->image->Cleanup();
    }
    m_Textures.clear();
    GE_CORE_TRACE("TextureLib: 已清空所有纹理");
}

bool TextureLib::Has(const std::string &filepath) const {
    std::string key = NormalizePath(filepath);
    return m_Textures.find(key) != m_Textures.end();
}

} // namespace GE
