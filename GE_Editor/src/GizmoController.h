#pragma once

#include "GE/Render/Camera.h"
#include "imgui.h" // ImGuizmo.h 需要先包含 imgui.h 才能识别 ImVec2/ImDrawList 等类型
#include "ImGuizmo.h"

#include "EditorContext.h"

#include <glm/glm.hpp>
#include <memory>

namespace GE {

class HierarchyLayer;
class Entity;

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
    /// 包围盒编辑模式：拖拽 BoundingBoxComponent 的角/边调整尺寸与中心。
    /// 复用 ImGuizmo 的 localBounds 模式（HandleAndDrawLocalBounds 自带盒线框手柄）。
    void EditBounds(Entity entity, const Camera &camera,
                    const glm::vec2 &viewportPos, const glm::vec2 &viewportSize);

    std::shared_ptr<EditorContext> m_Context;  ///< 共享场景上下文
    HierarchyLayer *m_Hierarchy = nullptr;     ///< 读取当前选中实体（非拥有）

    ImGuizmo::OPERATION m_Operation = ImGuizmo::TRANSLATE; ///< 当前操作：平移/旋转/缩放
    ImGuizmo::MODE m_Mode = ImGuizmo::WORLD;               ///< 变换空间：世界/局部
    bool m_UseSnap = false;                                ///< 是否启用吸附
    bool m_EditingBounds = false;                          ///< 是否处于包围盒编辑模式

    float m_SnapTranslation = 0.5f;  ///< 平移吸附步长
    float m_SnapRotation = 15.0f;    ///< 旋转吸附步长（度，ImGuizmo 内部转弧度）
    float m_SnapScale = 0.5f;        ///< 缩放吸附步长

    // == 包围盒拖拽起始状态缓存 ==
    // ImGuizmo 盒手柄的缩放比例是「相对拖拽起点」的绝对值；若每帧拿最新 bb.Size 去乘，
    // 会逐帧重复叠加比例造成指数漂移（盒按 比例^帧数 爆炸，灵敏度失控）。故拖拽首帧
    // 定格起始尺寸/中心，之后每帧由「起始值 × 阻尼比例」推出，详见 EditBounds。
    bool     m_BoundsDragActive        = false;               ///< 盒拖拽进行中（首帧定格起始态）
    glm::vec3 m_BoundsStartSize        = {1.0f, 1.0f, 1.0f};  ///< 拖拽起始尺寸
    glm::vec3 m_BoundsStartCenterWorld = {0.0f, 0.0f, 0.0f};  ///< 拖拽起始盒世界中心
};

} // namespace GE