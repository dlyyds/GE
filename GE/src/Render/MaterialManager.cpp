/**
 * @file MaterialManager.cpp
 * @brief 全局材质管理器实现。
 */

#include "Render/MaterialManager.h"

#include "Core/Log.h"

namespace GE {

namespace {
    /// 单 Albedo 材质的名称前缀（避免与用户命名冲突）
    constexpr const char *kAlbedoPrefix = "albedo:";
}

MaterialManager::MaterialManager(TextureManager &textureMgr)
    : m_TextureMgr(&textureMgr) {
    GE_CORE_INFO("MaterialManager initialized");
}

MaterialManager::~MaterialManager() {
    Clear();
    GE_CORE_INFO("MaterialManager shutdown");
}

Material *MaterialManager::Get(const std::string &name) const {
    auto it = m_Materials.find(name);
    if (it != m_Materials.end()) {
        return it->second.get();
    }
    return nullptr;
}

bool MaterialManager::Has(const std::string &name) const {
    return m_Materials.find(name) != m_Materials.end();
}

Material *MaterialManager::GetOrCreateFromAlbedo(const std::string &albedoPath) {
    if (albedoPath.empty()) {
        return nullptr;
    }

    // 内部名称：albedo:<path>
    std::string key = kAlbedoPrefix + albedoPath;

    // 已存在则直接返回
    auto it = m_Materials.find(key);
    if (it != m_Materials.end()) {
        return it->second.get();
    }

    // 先加载纹理
    Texture *albedo = m_TextureMgr->Load(albedoPath);
    if (!albedo) {
        GE_CORE_WARN("MaterialManager: 创建材质失败，纹理加载失败: {0}", albedoPath);
        return nullptr;
    }

    // 创建材质
    auto mat = std::make_unique<Material>();
    mat->SetTexture(Material::Albedo, albedo);
    mat->SetDebugName(key);

    Material *raw = mat.get();
    m_Materials[key] = std::move(mat);
    return raw;
}

Material *MaterialManager::Register(const std::string &name,
                                    std::unique_ptr<Material> material) {
    if (!material) {
        return nullptr;
    }
    Material *raw = material.get();
    m_Materials[name] = std::move(material);
    return raw;
}

void MaterialManager::Unload(const std::string &name) {
    m_Materials.erase(name);
}

void MaterialManager::Clear() {
    m_Materials.clear();
}

} // namespace GE
