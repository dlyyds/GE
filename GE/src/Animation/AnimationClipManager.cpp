/**
 * @file AnimationClipManager.cpp
 * @brief 动画片段管理器实现 —— weak_ptr 缓存 + 烘焙小文件优先 + GLTF 兜底构建。
 */

#include "pch.h"

#include "Animation/AnimationClipManager.h"

#include "Render/GLTFLoader.h"
#include "Render/Renderer.h"
#include "Render/AssetManager.h"
#include "Animation/AnimationClipLoader.h"
#include "FileSystem/VFS.h"
#include "Core/Log.h"
#include "Utils/PlatformUtils.h"

#include "tinygltf/tiny_gltf.h"

#include <filesystem>

namespace GE {

namespace {

/// 拆分源键 "path#N" → (filepath, animIdx)；格式非法返回 false（告警已打）
bool SplitSourceKey(const std::string &key, std::string &outFilepath, size_t &outAnimIdx) {
    const size_t hashPos = key.rfind('#');
    if (hashPos == std::string::npos) {
        GE_CORE_WARN("[Anim] clip 源键 '{}' 缺少 '#' 分隔", key);
        return false;
    }
    outFilepath = key.substr(0, hashPos);
    try {
        outAnimIdx = static_cast<size_t>(std::stoull(key.substr(hashPos + 1)));
    } catch (...) {
        GE_CORE_WARN("[Anim] clip 源键 '{}' 动画索引非法", key);
        return false;
    }
    return true;
}

} // namespace

AnimationClipManager &AnimationClipManager::Get() {
    static AnimationClipManager instance;
    return instance;
}

std::string AnimationClipManager::MakeKey(const std::string &filepath, size_t animIdx) {
    return filepath + "#" + std::to_string(animIdx);
}

std::shared_ptr<AnimationClip> AnimationClipManager::BuildAndCache(
    const std::string &filepath, size_t animIdx, const tinygltf::Model &model) {
    AnimationClip clip;
    std::string err;
    if (!GLTF::BuildAnimations(model, animIdx, clip, &err) || clip.channels.empty()) {
        // 无合法 channel 的 clip 没有用处：不入缓存（下次再导入会重建，代价可接受）
        if (!err.empty()) {
            GE_CORE_WARN("[Anim] clip '{}' 构建失败: {}", MakeKey(filepath, animIdx), err);
        }
        return nullptr;
    }
    clip.source = MakeKey(filepath, animIdx); // 持久化回读用的源键
    BakeIfNotExists(clip);                    // 写一次 .geanim，以后免读整份大 glTF
    auto shared = std::make_shared<AnimationClip>(std::move(clip));
    m_Clips[MakeKey(filepath, animIdx)] = shared;
    return shared;
}

void AnimationClipManager::BakeIfNotExists(const AnimationClip &clip) {
    // Android 上资产根只读（APK 内），整个运行期烘焙让位：要求资产在打包前就烘好，
    // gepack 已有校验能力。在这里跳过而不是让下游每帧写盘失败刷屏。
    if (!PlatformUtils::IsAssetRootWritable()) {
        return;
    }
    const std::string bakePath = DeriveAnimationBakePath(clip.source);
    if (VFS::Exists(bakePath)) {
        return; // 已烘焙：跳过（与 .gemesh「产物已存在则跳过」同约定）
    }
    std::string err;
    if (!SerializeAnimationClip(bakePath, clip, &err)) {
        GE_CORE_WARN("[Anim] clip '{}' 烘焙 .geanim 失败: {}", clip.source, err);
    }
}

std::shared_ptr<AnimationClip> AnimationClipManager::Load(const std::string &filepath,
                                                          size_t animIdx,
                                                          const tinygltf::Model &model) {
    const std::string key = MakeKey(filepath, animIdx);
    auto it = m_Clips.find(key);
    if (it != m_Clips.end()) {
        if (auto existing = it->second.lock()) {
            return existing; // 缓存命中：多实例共享同一份键帧
        }
        m_Clips.erase(it); // 弱引用已过期（无实体持有），重建
    }
    return BuildAndCache(filepath, animIdx, model);
}

std::shared_ptr<AnimationClip> AnimationClipManager::Load(const std::string &filepath,
                                                          size_t animIdx) {
    const std::string key = MakeKey(filepath, animIdx);
    auto it = m_Clips.find(key);
    if (it != m_Clips.end()) {
        if (auto existing = it->second.lock()) {
            return existing;
        }
        m_Clips.erase(it);
    }

    // 优先读取烘焙小文件：反序列化动画时避免主线程搬整份大 glTF（如 lacrimosa 135MB）
    const std::string bakePath = DeriveAnimationBakePath(key);
    if (VFS::Exists(bakePath)) {
        AnimationClip clip;
        std::string err;
        if (ParseAnimationClip(bakePath, clip, &err)) {
            clip.source = key; // 与 BuildAndCache 一致，保证回读后源键可用
            auto shared = std::make_shared<AnimationClip>(std::move(clip));
            m_Clips[key] = shared;
            return shared;
        }
        // 烘焙文件损坏：删掉让下方 BuildAndCache 重烘焙自愈，避免每次都回退读大源文件
        GE_CORE_WARN("[Anim] clip '{}' 烘焙文件损坏，重烘焙: {}", key, err);
        if (PlatformUtils::IsAssetRootWritable()) {
            std::error_code ec;
            std::filesystem::remove(Renderer::GetAssetManager().ResolveWritePath(bakePath), ec);
        }
    }

    // 兜底：读源 glTF（一次性；读完后 BuildAndCache 会再次烘焙写盘）
    tinygltf::Model model;
    std::string err;
    if (!GLTF::LoadModel(filepath, model, &err)) {
        GE_CORE_ERROR("[Anim] clip '{}' 源文件加载失败: {}", key, err);
        return nullptr;
    }
    return BuildAndCache(filepath, animIdx, model);
}

std::shared_ptr<AnimationClip> AnimationClipManager::LoadByKey(const std::string &key) {
    std::string filepath;
    size_t animIdx = 0;
    if (!SplitSourceKey(key, filepath, animIdx)) {
        return nullptr;
    }
    // 源键是资产引用：场景文件里存的是相对资源根的规范形，本类全程按规范形走
    // （GLTF::LoadModel 与 .geanim 烘焙产物都经 VFS），故此处只做归一。
    return Load(Renderer::GetAssetManager().ResolveCanonical(filepath), animIdx);
}

std::shared_ptr<AnimationClip> AnimationClipManager::Reload(const std::string &filepath,
                                                            size_t animIdx) {
    tinygltf::Model model;
    std::string err;
    if (!GLTF::LoadModel(filepath, model, &err)) {
        GE_CORE_ERROR("[Anim] clip '{}' 源文件加载失败: {}", MakeKey(filepath, animIdx), err);
        return nullptr;
    }
    return Reload(filepath, animIdx, model);
}

std::shared_ptr<AnimationClip> AnimationClipManager::Reload(const std::string &filepath,
                                                            size_t animIdx,
                                                            const tinygltf::Model &model) {
    const std::string key = MakeKey(filepath, animIdx);
    // 清缓存键与烘焙产物，强制从源重建；旧 clip 若仍被实体持有会继续存活，
    // 但管理器/烘焙此后指向新版本（旧键帧只在持有者内存中留存）。
    m_Clips.erase(key);
    const std::string bakePath = DeriveAnimationBakePath(key);
    if (PlatformUtils::IsAssetRootWritable()) {
        std::error_code ec;
        // 删旧烘焙，让 BuildAndCache 重写新数据
        std::filesystem::remove(Renderer::GetAssetManager().ResolveWritePath(bakePath), ec);
    }
    return BuildAndCache(filepath, animIdx, model);
}

std::shared_ptr<AnimationClip> AnimationClipManager::ReloadByKey(const std::string &key) {
    std::string filepath;
    size_t animIdx = 0;
    if (!SplitSourceKey(key, filepath, animIdx)) {
        return nullptr;
    }
    return Reload(filepath, animIdx);
}

} // namespace GE