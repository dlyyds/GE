#pragma once

#include "GE/Render/Camera.h"
#include "ImGuizmo.h"

#include "EditorContext.h"

#include <glm/glm.hpp>
#include <memory>

namespace GE {

class HierarchyLayer;

/// ImGuizmo 变换 gizmo 控制器 —— 在场景视口上叠加 移动/旋转/缩放 操控。
///
/// 注意：ImGuizmo 使用 ImGui::GetWindowDrawList() 绘制，必须在 Scene 窗口的
/// 绘制范围内调用 Render()（由 SceneLayer 在 ImGui::Begin("Scene") 之后回调）。
/// 因此本类不是独立渲染的 Layer，只是一个被 SceneLayer 调用的 helper。
class GizmoController {
public:
    /// context: 共享场景上下文；hierarchy: 用于读取当前选中实体（非拥有）
    GizmoController(std::shared_ptr<EditorContext> context, HierarchyLayer *hierarchy);

    /// 在 Scene 窗口绘制范围内调用：渲染选中实体的 gizmo 并应用变换。
    /// viewportPos/Size 为 Scene 视口在屏幕上的位置与尺寸（供 SetRect 使用）。
    void Render(const Camera &camera, const glm::vec2 &viewportPos, const glm::vec2 &viewportSize);

private:
    std::shared_ptr<EditorContext> m_Context;  ///< 共享场景上下文
    HierarchyLayer *m_Hierarchy = nullptr;     ///< 读取当前选中实体（非拥有）

    ImGuizmo::OPERATION m_Operation = ImGuizmo::TRANSLATE; ///< 当前操作：平移/旋转/缩放
    ImGuizmo::MODE m_Mode = ImGuizmo::WORLD;               ///< 变换空间：世界/局部
    bool m_UseSnap = false;                                ///< 是否启用吸附

    float m_SnapTranslation = 0.5f;  ///< 平移吸附步长
    float m_SnapRotation = 15.0f;    ///< 旋转吸附步长（度，ImGuizmo 内部转弧度）
    float m_SnapScale = 0.5f;        ///< 缩放吸附步长
};

} // namespace GE