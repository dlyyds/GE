/**
 * @file MaterialManager.cpp
 * @brief 全局材质管理器实现。
 */

#include "Render/MaterialManager.h"

#include "Core/Log.h"

namespace GE {

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

Material *MaterialManager::GetOrCreateDefault(const std::string &name) {
    if (Material *existing = Get(name)) {
        return existing;
    }
    return Register(name, std::make_unique<Material>());
}

Material *MaterialManager::Register(const std::string &name,
                                    std::unique_ptr<Material> material) {
    if (!material) {
        return nullptr;
    }
    Material *raw = material.get();
    // 显示名默认 = 注册名；调用方已显式改名则保留（Name 是独立字段，与 key 解耦）
    if (raw->GetName().empty()) {
        raw->SetName(name);
    }
    m_Materials[name] = std::move(material);
    return raw;
}

void MaterialManager::Unload(const std::string &name) {
    m_Materials.erase(name);
}

void MaterialManager::Clear() {
    m_Materials.clear();
}

std::vector<std::string> MaterialManager::GetAllNames() const {
    std::vector<std::string> names;
    names.reserve(m_Materials.size());
    for (const auto &pair : m_Materials) {
        names.push_back(pair.first);
    }
    return names;
}

} // namespace GE
