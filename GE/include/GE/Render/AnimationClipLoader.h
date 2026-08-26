/**
 * @file AnimationClipLoader.h
 * @brief 动画片段 .geanim 烘焙格式 —— 键帧小文件（与 .gemesh 同构的引擎内置格式）。
 *
 * glTF 动画键帧来自 .gltf/.glb 源文件；反序列化动画若每次都回读源文件，要主线程
 * 搬整份大 bin（如 lacrimosa 135MB）。烘焙成 .geanim 后，键帧只写一次小文件，
 * 后续加载只读小文件，绕开大 glTF。与 .gemesh 的分工一致：GLTFLoader 负责从
 * 外部格式装配，本模块负责引擎内置格式的序列化/反序列化。
 */

#pragma once

#include "Scene/Components.h"

#include <string>

namespace GE {

/**
 * @brief 把 AnimationClip 写成 .geanim 小文件（全 channel 键帧 + 元数据）。
 *
 * 仅序列化键帧与元数据（name / duration / channels），不涉及场景层 channelTargets
 * （目标实体是每实例状态，不入文件）。rotation 存引擎 glm::quat 内存序 (w,x,y,z)。
 *
 * @param outPath 输出路径（.geanim）
 * @param clip    要持久化的动画片段
 * @param err     非空时回填错误描述
 * @return 成功返回 true
 */
bool SerializeAnimationClip(const std::string &outPath, const AnimationClip &clip,
                            std::string *err = nullptr);

/**
 * @brief 从 .geanim 文件读取 AnimationClip（与 Serialize 完全对称）。
 *
 * 带完整字节边界 / 数量上限 / 枚举合法性校验，损坏文件返回 false（调用方据此回退
 * 重读源 glTF）。
 *
 * @param filepath .geanim 路径
 * @param out      输出（AnimationClip）
 * @param err      非空时回填错误描述
 * @return 成功返回 true
 */
bool ParseAnimationClip(const std::string &filepath, AnimationClip &out,
                        std::string *err = nullptr);

/**
 * @brief 由动画源键 "path#N" 推导烘焙输出路径（与 .gemesh 同目录同命名约定）。
 *
 *   - N == 0 → <dir>/foo.geanim
 *   - N >  0 → <dir>/foo_N.geanim
 *
 * @param sourceKey 动画源键（如 "assets/.../scene.gltf#0"）
 * @return .geanim 绝对路径
 */
std::string DeriveAnimationBakePath(const std::string &sourceKey);

} // namespace GE