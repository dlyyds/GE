/**
 * @file MeshManager.cpp
 * @brief 全局网格管理器实现。
 */

#include "Render/MeshManager.h"

#include "Render/MaterialManager.h"

#include "Render/VulkanBase/VulkanDevice.h"
#include "Core/Log.h"

namespace GE {

MeshManager::MeshManager(VulkanDevice &device, MaterialManager &materialManager)
    : m_Device(&device), m_Materials(&materialManager) {
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

    // 为各子网格创建材质（放进 MaterialManager，随模型生命周期走）。
    // 材质 key 用「路径::材质名」避免跨模型同名材质冲突。
    // 有材质名（OBJ MTL）→ 绑对应材质；无材质名（内置几何体 / 无 MTL 的 OBJ / CPU 直建）
    // → 绑一个默认（空白）材质，保证每个子网格都有材质，而非走白色 fallback。
    if (m_Materials) {
        const auto &subMeshes = mesh->GetSubMeshes();
        for (size_t i = 0; i < subMeshes.size(); ++i) {
            const auto &name = subMeshes[i].materialName;
            const std::string matName = name.empty() ? "default" : name;
            const std::string key = filepath + "::" + matName;
            Material *mat = m_Materials->GetOrCreateDefault(key);
            mat->SetDebugName(matName);
            mesh->SetSubMeshDefaultMaterial(static_cast<uint32_t>(i), mat);
        }
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