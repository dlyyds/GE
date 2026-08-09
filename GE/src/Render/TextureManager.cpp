/**
 * @file TextureManager.cpp
 * @brief 全局纹理管理器实现。
 */

#include "Render/TextureManager.h"

#include "Render/VulkanBase/VulkanDevice.h"
#include "Render/VulkanBase/VulkanResourceCache.h"
#include "Core/Log.h"

#include <vector>
#include <cstdio>

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

Texture *TextureManager::GetSolidColor(const glm::vec4 &color,
                                       vk::Format format,
                                       vk::Filter mag_filter,
                                       vk::Filter min_filter) {
    // 颜色分量量化到 8bit，用它生成唯一的缓存键
    auto toByte = [](float c) -> uint8_t {
        return static_cast<uint8_t>(glm::clamp(c, 0.0f, 1.0f) * 255.0f + 0.5f);
    };
    uint8_t r = toByte(color.r);
    uint8_t g = toByte(color.g);
    uint8_t b = toByte(color.b);
    uint8_t a = toByte(color.a);

    // 缓存键：solid:RRGGBBAA
    char keyBuf[32];
    std::snprintf(keyBuf, sizeof(keyBuf), "solid:%02X%02X%02X%02X", r, g, b, a);
    std::string key = keyBuf;

    // 已缓存则直接返回
    auto it = m_Textures.find(key);
    if (it != m_Textures.end()) {
        return it->second.get();
    }

    // 打包为 RGBA8 像素（内存小端序：A 在高字节）
    uint32_t pixel = (static_cast<uint32_t>(a) << 24)
                   | (static_cast<uint32_t>(b) << 16)
                   | (static_cast<uint32_t>(g) << 8)
                   | static_cast<uint32_t>(r);

    auto tex = Texture::LoadFromMemory(*m_Device, *m_Cache, &pixel, 1, 1,
                                       format, mag_filter, min_filter);
    if (!tex) {
        GE_CORE_WARN("TextureManager: 创建纯色纹理失败 (r={0},g={1},b={2},a={3})",
                     r, g, b, a);
        return nullptr;
    }

    tex->SetDebugName(key);

    Texture *raw = tex.get();
    m_Textures[key] = std::move(tex);
    return raw;
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
