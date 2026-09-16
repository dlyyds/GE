/**
 * @file MaterialManager.cpp
 * @brief 全局材质管理器实现。
 */

#include "Render/MaterialManager.h"

#include "Render/MaterialSerializer.h"
#include "FileSystem/VFS.h"
#include "Core/Log.h"
#include "Utils/PlatformUtils.h"

#include <yaml-cpp/yaml.h>

#include <filesystem>
#include <fstream>
#include <utility>
#include <vector>

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
    // 同键替换：旧实例已销毁，其反查项必须摘掉，否则留下悬垂指针
    auto old = m_Materials.find(name);
    if (old != m_Materials.end()) {
        m_ByPath.erase(old->second->GetSourcePath());
    }
    m_Materials[name] = std::move(material);
    return raw;
}

void MaterialManager::Unload(const std::string &name) {
    auto it = m_Materials.find(name);
    if (it != m_Materials.end()) {
        m_ByPath.erase(it->second->GetSourcePath());
        m_Materials.erase(it);
    }
}

void MaterialManager::Clear() {
    m_Materials.clear();
    m_ByPath.clear();
}

std::vector<std::string> MaterialManager::GetAllNames() const {
    std::vector<std::string> names;
    names.reserve(m_Materials.size());
    for (const auto &pair : m_Materials) {
        names.push_back(pair.first);
    }
    return names;
}

Material *MaterialManager::GetBySourcePath(const std::string &resolvedPath) const {
    if (resolvedPath.empty()) {
        return nullptr;
    }
    auto it = m_ByPath.find(resolvedPath);
    return it != m_ByPath.end() ? it->second : nullptr;
}

Material *MaterialManager::Load(const std::string &resolvedPath) {
    if (resolvedPath.empty()) {
        return nullptr;
    }
    // 同路径只加载一次：已加载的实例可能被编辑器改过，重新读盘会把改动冲掉
    if (Material *cached = GetBySourcePath(resolvedPath)) {
        return cached;
    }

    YAML::Node root;
    try {
        // 经 VFS 读文本再解析：YAML::LoadFile 直接 fopen，在 Android 上读不到 APK 内资产
        root = YAML::Load(VFS::ReadText(resolvedPath));
    } catch (const std::exception &e) {
        GE_CORE_WARN("MaterialManager: 材质文件解析失败 {0}: {1}", resolvedPath, e.what());
        return nullptr;
    }

    // 文件根是单一包装键 Material（与 .scene 的 Scene 根同构，便于将来加元信息）
    YAML::Node node = root["Material"];
    if (!node) {
        GE_CORE_WARN("MaterialManager: 材质文件缺少 Material 根节点: {0}", resolvedPath);
        return nullptr;
    }

    // 版本前瞻：高版本文件里可能有本版读不懂的字段，读下去不报错会静默丢内容，
    // 至少留一条可追的日志（缺 Version 视为当前版本）。
    const int version = node["Version"]
                            ? node["Version"].as<int>(MaterialSerializer::kFormatVersion)
                            : MaterialSerializer::kFormatVersion;
    if (version > MaterialSerializer::kFormatVersion) {
        GE_CORE_WARN("MaterialManager: 材质文件版本 {0} 高于当前支持 {1}，按 {1} 读取: {2}",
                     version, MaterialSerializer::kFormatVersion, resolvedPath);
    }

    auto mat = std::make_unique<Material>();
    MaterialSerializer::ApplyMaterialNode(*mat, node);
    // 显示名优先取文件里的 Name，缺省退回文件名（保持非空，UI 才有个可读标签）
    if (node["Name"]) {
        mat->SetName(node["Name"].as<std::string>());
    } else {
        mat->SetName(std::filesystem::path(resolvedPath).stem().string());
    }
    mat->SetSourcePath(resolvedPath);

    const std::string key = std::string(kAssetKeyPrefix) + resolvedPath;
    Material *raw = Register(key, std::move(mat));
    m_ByPath[resolvedPath] = raw;
    raw->ClearDirty();  // 内容刚来自文件，视为已保存
    return raw;
}

bool MaterialManager::Save(Material &mat, const std::string &resolvedPath,
                           const std::string &assetRoot) {
    if (resolvedPath.empty()) {
        return false;
    }

    if (!PlatformUtils::IsAssetRootWritable()) {
        GE_CORE_WARN("MaterialManager: 资产根只读（Android 上资产在 APK 内），无法保存: {0}",
                     resolvedPath);
        return false;
    }

    YAML::Node root;
    YAML::Node node = root["Material"];
    node["Version"] = MaterialSerializer::kFormatVersion;
    MaterialSerializer::WriteMaterialNode(node, mat, assetRoot);

    // resolvedPath 是**规范形**（对内同时充当缓存键与材质反查表键，与 Load 一致），
    // 要落盘得拼回资源根变成真实文件路径。
    const std::filesystem::path writePath =
        assetRoot.empty()
            ? std::filesystem::path(resolvedPath)
            : (std::filesystem::path(assetRoot) / std::filesystem::path(resolvedPath))
                  .lexically_normal();

    std::error_code ec;
    if (!writePath.parent_path().empty()) {
        std::filesystem::create_directories(writePath.parent_path(), ec); // 首次另存为时目录可能不存在
    }

    std::ofstream out(writePath);
    if (!out) {
        GE_CORE_WARN("MaterialManager: 材质文件写入失败: {0}", writePath.string());
        return false;
    }
    out << root;
    out.close();
    if (!out) {
        GE_CORE_WARN("MaterialManager: 材质文件写出错: {0}", writePath.string());
        return false;
    }

    // 另存为：旧路径的反查项要摘掉，否则同一材质两个路径都可达
    if (mat.GetSourcePath() != resolvedPath) {
        m_ByPath.erase(mat.GetSourcePath());
    }
    mat.SetSourcePath(resolvedPath);
    m_ByPath[resolvedPath] = &mat;
    mat.ClearDirty();
    return true;
}

int MaterialManager::SaveAllDirty(const std::string &assetRoot) {
    // 先收集再写：Save 会回写 m_ByPath，遍历中改容器容易踩坑
    std::vector<std::pair<std::string, Material *>> pending;
    pending.reserve(m_ByPath.size());
    for (const auto &kv : m_ByPath) {
        if (kv.second && kv.second->IsDirty() && !kv.first.empty()) {
            pending.emplace_back(kv.first, kv.second);
        }
    }

    int written = 0;
    for (auto &[path, mat] : pending) {
        if (Save(*mat, path, assetRoot)) {
            GE_CORE_INFO("MaterialManager: 已保存材质资产 {0}", path);
            ++written;
        }
    }
    return written;
}

} // namespace GE
