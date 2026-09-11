/**
 * @file AssetManager.cpp
 * @brief 统一资源管理器实现。
 */

#include "Render/AssetManager.h"

#include "Render/AssetPathUtil.h"
#include "Render/TextureManager.h"
#include "Render/MeshManager.h"
#include "Render/MaterialManager.h"
#include "Audio/SoundManager.h"

#include "Render/VulkanBase/VulkanDevice.h"
#include "Render/VulkanBase/VulkanResourceCache.h"
#include "Core/Log.h"

#include <string>

namespace GE {

AssetManager::AssetManager(VulkanDevice &device, VulkanResourceCache &cache,
                           AsyncUploadManager &upload)
    : m_AsyncUpload(&upload) {
    m_TextureManager  = std::make_unique<TextureManager>(device, cache, upload);
    // 材质管理器先于网格管理器创建，供 MeshManager 在加载模型时创建子网格材质
    m_MaterialManager = std::make_unique<MaterialManager>();
    m_SoundManager    = std::make_unique<Audio::SoundManager>();
    m_MeshManager     = std::make_unique<MeshManager>(device, *m_MaterialManager,
                                                      *m_TextureManager, upload);
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
    if (AssetPathUtil::IsPseudoKey(path)) {
        return std::filesystem::path(path);
    }

    const std::filesystem::path p(AssetPathUtil::NormalizeSeparators(path));

    if (p.is_absolute()) {
        // 根内绝对路径 → 相对化后解析：历史场景写过的绝对路径无需改文件即可加载，
        // 且换机器/换安装目录仍有救（只要资产在包内的相对位置一致）。
        if (auto rel = AssetPathUtil::ToCanonical(path, m_AssetRoot)) {
            return (m_AssetRoot / *rel).lexically_normal();
        }
        // 根外绝对路径：无法随包分发，打包校验须拦下。此处报错但原样返回，不静默失败。
        GE_CORE_ERROR("AssetManager: 资产引用落在资源根之外，无法随包分发: {0}（资源根: {1}）",
                      path, m_AssetRoot.string());
        return p.lexically_normal();
    }

    // 相对路径：归一后拼接。归一同时剥掉可能的资源根目录名前缀（"assets/xxx"），
    // 避免资源根改为绝对路径后拼成 "assets/assets/xxx"。
    if (auto rel = AssetPathUtil::ToCanonical(path, m_AssetRoot)) {
        return (m_AssetRoot / *rel).lexically_normal();
    }
    // 归一失败（如越界 ".."）：保守按原相对路径拼接
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

AsyncUploadManager &AssetManager::GetAsyncUploadManager() {
    return *m_AsyncUpload;
}

Texture *AssetManager::LoadTexture(const std::string &path) {
    const std::filesystem::path resolved = ResolvePath(path);
    // 使用 TextureManager::Load 的默认格式 eR8G8B8A8Unorm 与线性采样
    return GetTextureManager().Load(resolved.string());
}

Texture *AssetManager::LoadTextureAsync(const std::string &path) {
    const std::filesystem::path resolved = ResolvePath(path);
    // 使用 TextureManager::LoadAsync 的默认格式 eR8G8B8A8Unorm 与线性采样
    return GetTextureManager().LoadAsync(resolved.string());
}

Mesh *AssetManager::LoadMesh(const std::string &path) {
    const std::filesystem::path resolved = ResolvePath(path);
    return GetMeshManager().Load(resolved.string());
}

Audio::SoundManager &AssetManager::GetSoundManager() {
    return *m_SoundManager;
}

Audio::SoundAsset *AssetManager::LoadSound(const std::string &path) {
    const std::filesystem::path resolved = ResolvePath(path);
    return GetSoundManager().Load(resolved.string());
}

bool AssetManager::PlayOneShot(const std::string &path, float volume) {
    const std::filesystem::path resolved = ResolvePath(path);
    return GetSoundManager().PlayOneShot(resolved.string(), volume);
}
} // namespace GE
