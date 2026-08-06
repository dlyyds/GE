/**
 * @file TextureManager.cpp
 * @brief 全局纹理管理器实现。
 */

#include "Render/TextureManager.h"

#include "Render/VulkanBase/VulkanDevice.h"
#include "Render/VulkanBase/VulkanResourceCache.h"
#include "Core/Log.h"

#include <vector>

namespace GE {

TextureManager::TextureManager(VulkanDevice &device, VulkanResourceCache &cache)
    : m_Device(&device), m_Cache(&cache) {
    GE_CORE_INFO("TextureManager initialized");
}

TextureManager::~TextureManager() {
    Clear();
    GE_CORE_INFO("TextureManager shutdown");
}

Texture *TextureManager::Load(const std::string &filepath,
                              vk::Format format,
                              vk::Filter mag_filter,
                              vk::Filter min_filter) {
    if (filepath.empty()) {
        return nullptr;
    }

    // 已加载则直接返回
    auto it = m_Textures.find(filepath);
    if (it != m_Textures.end()) {
        return it->second.get();
    }

    // 加载纹理
    auto tex = Texture::LoadFromFile(*m_Device, *m_Cache, filepath,
                                     format, mag_filter, min_filter);
    if (!tex) {
        GE_CORE_WARN("TextureManager: 纹理加载失败: {0}", filepath);
        return nullptr;
    }

    Texture *raw = tex.get();
    m_Textures[filepath] = std::move(tex);
    return raw;
}

Texture *TextureManager::Get(const std::string &filepath) const {
    auto it = m_Textures.find(filepath);
    if (it != m_Textures.end()) {
        return it->second.get();
    }
    return nullptr;
}

bool TextureManager::Has(const std::string &filepath) const {
    return m_Textures.find(filepath) != m_Textures.end();
}

Texture *TextureManager::Register(const std::string &key,
                                  std::unique_ptr<Texture> texture) {
    if (!texture) {
        return nullptr;
    }
    Texture *raw = texture.get();
    m_Textures[key] = std::move(texture);
    return raw;
}

void TextureManager::Unload(const std::string &filepath) {
    m_Textures.erase(filepath);
}

void TextureManager::Clear() {
    m_Textures.clear();
}

std::vector<std::string> TextureManager::GetAllKeys() const {
    std::vector<std::string> keys;
    keys.reserve(m_Textures.size());
    for (const auto &pair : m_Textures) {
        keys.push_back(pair.first);
    }
    return keys;
}

} // namespace GE
