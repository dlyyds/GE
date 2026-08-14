/**
 * @file AssetManager.cpp
 * @brief 统一资源管理器实现。
 */

#include "Render/AssetManager.h"

#include "Render/TextureManager.h"
#include "Render/MeshManager.h"
#include "Render/MaterialManager.h"

#include "Render/VulkanBase/VulkanDevice.h"
#include "Render/VulkanBase/VulkanResourceCache.h"
#include "Core/Log.h"

#include <string>

namespace GE {

AssetManager::AssetManager(VulkanDevice &device, VulkanResourceCache &cache) {
    m_TextureManager  = std::make_unique<TextureManager>(device, cache);
    // 材质管理器先于网格管理器创建，供 MeshManager 在加载模型时创建子网格材质
    m_MaterialManager = std::make_unique<MaterialManager>();
    m_MeshManager     = std::make_unique<MeshManager>(device, *m_MaterialManager);
    GE_CORE_INFO("AssetManager initialized (asset root: {0})", m_AssetRoot.string());
}

AssetManager::~AssetManager() {
    // 各子管理器析构时自动 Clear，按成员声明逆序销毁
    GE_CORE_INFO("AssetManager shutdown");
}

void AssetManager::SetAssetRoot(const std::filesystem::path &root) {
    m_AssetRoot = root.lexically_normal();
    GE_CORE_INFO("AssetManager: 资源根目录已设置为 {0}", m_AssetRoot.string());
}

std::filesystem::path AssetManager::ResolvePath(const std::string &path) const {
    if (path.empty()) {
        return {};
    }

    // 伪路径（内置几何体 / 纯色纹理键）原样返回，不参与文件解析
    if (path.rfind("builtin:", 0) == 0 || path.rfind("solid:", 0) == 0) {
        return std::filesystem::path(path);
    }

    std::filesystem::path p(path);
    if (p.is_absolute()) {
        return p.lexically_normal();
    }

    // 已带资源根前缀则规范化后原样返回，避免重复拼接
    const std::string rootStr = m_AssetRoot.lexically_normal().string();
    const std::string pStr    = p.lexically_normal().string();
    if (!rootStr.empty() && pStr.rfind(rootStr, 0) == 0) {
        return p.lexically_normal();
    }

    return (m_AssetRoot / p).lexically_normal();
}

TextureManager &AssetManager::GetTextureManager() {
    return *m_TextureManager;
}

MeshManager &AssetManager::GetMeshManager() {
    return *m_MeshManager;
}

MaterialManager &AssetManager::GetMaterialManager() {
    return *m_MaterialManager;
}

Texture *AssetManager::LoadTexture(const std::string &path) {
    const std::filesystem::path resolved = ResolvePath(path);
    // 使用 TextureManager::Load 的默认格式 eR8G8B8A8Unorm 与线性采样
    return GetTextureManager().Load(resolved.string());
}

Mesh *AssetManager::LoadMesh(const std::string &path) {
    const std::filesystem::path resolved = ResolvePath(path);
    return GetMeshManager().Load(resolved.string());
}

} // namespace GE