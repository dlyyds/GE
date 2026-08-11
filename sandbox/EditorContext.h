#pragma once

#include "GE/Scene/Scene.h"
#include "GE/Scene/Entity.h"
#include "GE/Scene/SceneSerializer.h"

#include <memory>

namespace GE {

/// 编辑器共享场景上下文 —— 被场景层与各面板层共同持有（shared_ptr）。
///
/// 场景层拥有场景生命周期（加载/新建），面板层只读或编辑其中的 Scene。
/// 新建/加载场景时会替换其 Scene 成员（重建对象），各持有者通过持同一
/// shared_ptr 自动感知到新场景，避免裸指针悬空。
struct EditorContext {
    std::unique_ptr<Scene> Scene;                 ///< 当前编辑的场景
    std::unique_ptr<SceneSerializer> Serializer;  ///< 场景序列化器（与 Scene 同生同灭）
    Entity CameraEntity;                          ///< 场景主相机实体
};

} // namespace GE