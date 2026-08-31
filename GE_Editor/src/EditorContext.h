#pragma once

#include "GE/Scene/Scene.h"
#include "GE/Scene/Entity.h"
#include "GE/Render/Camera.h"

#include <memory>

namespace GE {

/// 编辑器共享场景上下文 —— 被场景层与各面板层共同持有（shared_ptr）。
///
/// 场景层拥有场景生命周期（加载/新建），面板层只读或编辑其中的 Scene。
/// 新建/加载场景时会替换其 Scene 成员（重建对象），各持有者通过持同一
/// shared_ptr 自动感知到新场景，避免裸指针悬空。
///
/// 编辑器导航相机（EditorCamera）独立于场景内容：它是工具的眼，不是场景实体，
/// 不参与序列化；场景里的 CameraComponent 实体是纯游戏相机（Play 时才生效）。
struct EditorContext {
    std::unique_ptr<Scene> Scene;                 ///< 当前编辑的场景
    Camera EditorCamera;                          ///< 编辑器导航相机（工具视角，不进场景、不序列化）
};

} // namespace GE