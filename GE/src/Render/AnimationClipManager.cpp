/**
 * @file AnimationClipManager.cpp
 * @brief 动画片段管理器实现 —— weak_ptr 缓存 + 烘焙小文件优先 + GLTF 兜底构建。
 */

#include "pch.h"

#include "Render/AnimationClipManager.h"

#include "Render/GLTFLoader.h"
#include "Render/AnimationClipLoader.h"
#include "Core/Log.h"

#include "tinygltf/tiny_gltf.h"

#include <filesystem>

namespace GE {

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
    const std::string bakePath = DeriveAnimationBakePath(clip.source);
    if (std::filesystem::exists(bakePath)) {
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
    if (std::filesystem::exists(bakePath)) {
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
        std::error_code ec;
        std::filesystem::remove(bakePath, ec);
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
    // 源键格式 "path#N"：从最后的 '#' 拆出文件路径与动画索引
    const size_t hashPos = key.rfind('#');
    if (hashPos == std::string::npos) {
        GE_CORE_WARN("[Anim] clip 源键 '{}' 缺少 '#' 分隔", key);
        return nullptr;
    }
    const std::string filepath = key.substr(0, hashPos);
    size_t animIdx = 0;
    try {
        animIdx = static_cast<size_t>(std::stoull(key.substr(hashPos + 1)));
    } catch (...) {
        GE_CORE_WARN("[Anim] clip 源键 '{}' 动画索引非法", key);
        return nullptr;
    }
    return Load(filepath, animIdx);
}

} // namespace GE