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

std::string AssetManager::ResolveCanonical(const std::string &path) const {
    if (path.empty()) {
        return {};
    }

    // 伪路径（内置几何体 / 纯色纹理键）原样返回，不参与文件解析
    if (AssetPathUtil::IsPseudoKey(path)) {
        return path;
    }

    if (auto rel = AssetPathUtil::ToCanonical(path, m_AssetRoot)) {
        return *rel;
    }

    // 归一失败：根外绝对路径（无法随包分发，打包校验须拦下）或越界 ".."。
    // 不静默失败、也不猜一个替代路径：报错后原样返回，让后续 VFS 读取如实失败，
    // 现象是"这个资产读不到"而不是"读到了别的资产"。
    GE_CORE_ERROR("AssetManager: 资产引用无法归一为规范形，无法随包分发: {0}（资源根: {1}）",
                  path, m_AssetRoot.string());
    return AssetPathUtil::NormalizeSeparators(path);
}

std::filesystem::path AssetManager::ResolveWritePath(const std::string &path) const {
    const std::string canonical = ResolveCanonical(path);
    if (canonical.empty() || AssetPathUtil::IsPseudoKey(canonical)) {
        return {};
    }
    return (m_AssetRoot / std::filesystem::path(canonical)).lexically_normal();
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
    // 使用 TextureManager::Load 的默认格式 eR8G8B8A8Unorm 与线性采样
    return GetTextureManager().Load(ResolveCanonical(path));
}

Texture *AssetManager::LoadTextureAsync(const std::string &path, vk::Format format) {
    // 格式必须由调用方按贴图语义给：颜色贴图 sRGB、数据贴图 Unorm。
    // TextureManager::LoadAsync 按路径缓存，格式不一致的第二次请求会被忽略，
    // 于是"谁先加载谁定格式"——不一致就会让另一边拿到错误解码的颜色。
    return GetTextureManager().LoadAsync(ResolveCanonical(path), format);
}

Mesh *AssetManager::LoadMesh(const std::string &path) {
    return GetMeshManager().Load(ResolveCanonical(path));
}

Audio::SoundManager &AssetManager::GetSoundManager() {
    return *m_SoundManager;
}

Audio::SoundAsset *AssetManager::LoadSound(const std::string &path) {
    return GetSoundManager().Load(ResolveCanonical(path));
}

Material *AssetManager::LoadMaterial(const std::string &path) {
    if (path.empty()) {
        return nullptr;
    }
    return GetMaterialManager().Load(ResolveCanonical(path));
}

bool AssetManager::SaveMaterial(Material &mat, const std::string &path) {
    if (path.empty()) {
        return false;
    }
    return GetMaterialManager().Save(mat, ResolveCanonical(path), GetAssetRoot().string());
}

int AssetManager::SaveAllDirtyMaterials() {
    return GetMaterialManager().SaveAllDirty(GetAssetRoot().string());
}

bool AssetManager::PlayOneShot(const std::string &path, float volume) {
    return GetSoundManager().PlayOneShot(ResolveCanonical(path), volume);
}
} // namespace GE
