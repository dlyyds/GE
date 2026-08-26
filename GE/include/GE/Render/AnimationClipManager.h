/**
 * @file AnimationClipManager.h
 * @brief 全局动画片段管理器 —— 按 "path#N" 键去重共享 AnimationClip。
 *
 * 与 MeshManager 的 "path#N" 网格缓存（GLTF 场景导入用）同构：动画键帧
 * （AnimationClip）是模型级不变量，跨导入 / 跨实例共享，多角色实例只存一份。
 * 用 weak_ptr 缓存：无任何实体引用时 clip 自动释放，再次引用同键时重建并回归缓存。
 *
 * 生命周期：纯 CPU 资源（键帧），不属于某个渲染上下文，故为进程级静态单例，
 * 不随 Scene 销毁，供场景导入器与将来的 SceneSerializer 反序列化共用。
 */

#pragma once

#include "Scene/Components.h"

#include <memory>
#include <string>
#include <unordered_map>

namespace tinygltf { struct Model; }

namespace GE {

/**
 * @brief 全局动画片段管理器。
 *
 * 键 = filepath + "#" + animIdx（与 glTF 场景网格的 "path#N" 复合键同构）。
 * 命中缓存返回既有 shared_ptr；未命中则经 GLTF::BuildAnimations 构建后缓存。
 * 构建失败或 clip 无合法 channel 时返回 nullptr（不入缓存，调用方容错跳过）。
 */
class AnimationClipManager {
public:
    /// 进程级单例（动画片段是无场景状态、跨 Scene 共享的纯 CPU 资源）
    static AnimationClipManager &Get();

    AnimationClipManager(const AnimationClipManager &) = delete;
    AnimationClipManager &operator=(const AnimationClipManager &) = delete;

    /// 缓存键："path#N"
    static std::string MakeKey(const std::string &filepath, size_t animIdx);

    /**
     * @brief 取（或构建并缓存）指定动画的共享 clip，复用调用方已解析的 model。
     *
     * 场景导入器已 LoadModel 过一次，调用此重载避免再次读盘解析。
     *
     * @param filepath glTF 文件路径（缓存键前缀）
     * @param animIdx  animation 索引（缓存键后缀）
     * @param model    已解析的 tinygltf::Model（调用方持有完整生命周期）
     * @return 共享 clip；无合法 channel / 解析失败返回 nullptr
     */
    std::shared_ptr<AnimationClip> Load(const std::string &filepath, size_t animIdx,
                                        const tinygltf::Model &model);

    /**
     * @brief 同 Load，但内部自行 LoadModel（供仅持源键的反序列化路径使用）。
     */
    std::shared_ptr<AnimationClip> Load(const std::string &filepath, size_t animIdx);

    /**
     * @brief 按完整源键 "path#N" 加载（拆出 filepath + animIdx 后走 Load）。
     *
     * 供 SceneSerializer 反序列化按剪贴板 Clip 键回取共享 clip；键格式不合法
     * / 源文件不存在 / 无合法 channel 时返回 nullptr，调用方容错跳过。
     */
    std::shared_ptr<AnimationClip> LoadByKey(const std::string &key);

private:
    AnimationClipManager() = default;

    /// 从 model 构建 clip 并缓存（无合法 channel 返回 nullptr，不入缓存）
    std::shared_ptr<AnimationClip> BuildAndCache(const std::string &filepath, size_t animIdx,
                                                 const tinygltf::Model &model);

    /// 把 clip 烘焙为 .geanim 小文件（写一次免读大 glTF；产物存在则跳过，失败仅告警）
    void BakeIfNotExists(const AnimationClip &clip);

    /// 缓存：键 -> 弱引用（无实体持有即释放，重建后回归缓存）
    std::unordered_map<std::string, std::weak_ptr<AnimationClip>> m_Clips;
};

} // namespace GE