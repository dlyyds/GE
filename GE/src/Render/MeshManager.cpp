/**
 * @file MeshManager.cpp
 * @brief 全局网格管理器实现。
 */

#include "Render/MeshManager.h"

#include "Render/VulkanBase/VulkanDevice.h"
#include "Core/Log.h"

namespace GE {

MeshManager::MeshManager(VulkanDevice &device) : m_Device(&device) {
}

MeshManager::~MeshManager() {
    Clear();
}

Mesh *MeshManager::Get(const std::string &filepath) const {
    auto it = m_Meshes.find(filepath);
    if (it != m_Meshes.end()) {
        return it->second.get();
    }
    return nullptr;
}

bool MeshManager::Has(const std::string &filepath) const {
    return m_Meshes.find(filepath) != m_Meshes.end();
}

Mesh *MeshManager::Load(const std::string &filepath) {
    if (filepath.empty()) {
        return nullptr;
    }

    // 已加载则直接返回缓存
    if (Mesh *existing = Get(filepath)) {
        return existing;
    }

    if (!m_Device) {
        GE_CORE_WARN("MeshManager: 无法加载网格 {}（未提供 VulkanDevice）", filepath);
        return nullptr;
    }

    std::unique_ptr<Mesh> mesh;

    // 内置几何体（builtin:cube, builtin:sphere 等）
    if (Mesh::IsBuiltinPath(filepath)) {
        mesh = Mesh::CreateBuiltin(*m_Device, Mesh::GetBuiltinType(filepath));
    } else {
        mesh = Mesh::LoadFromFile(*m_Device, filepath);
    }

    if (!mesh) {
        GE_CORE_WARN("MeshManager: 网格加载失败: {}", filepath);
        return nullptr;
    }

    Mesh *raw = mesh.get();
    m_Meshes[filepath] = std::move(mesh);
    return raw;
}

Mesh *MeshManager::GetBuiltin(const std::string &type) {
    return Load("builtin:" + type);
}

Mesh *MeshManager::Register(const std::string &key,
                            std::unique_ptr<Mesh> mesh) {
    if (!mesh) {
        return nullptr;
    }
    Mesh *raw = mesh.get();
    m_Meshes[key] = std::move(mesh);
    return raw;
}

void MeshManager::Unload(const std::string &filepath) {
    m_Meshes.erase(filepath);
}

void MeshManager::Clear() {
    m_Meshes.clear();
}

std::vector<std::string> MeshManager::GetAllKeys() const {
    std::vector<std::string> keys;
    keys.reserve(m_Meshes.size());
    for (const auto &pair : m_Meshes) {
        keys.push_back(pair.first);
    }
    return keys;
}

} // namespace GE