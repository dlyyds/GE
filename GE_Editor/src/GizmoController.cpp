//
// Gizmo 控制器实现 —— 在场景视口上渲染 ImGuizmo 变换 gizmo 并写回 Transform。
//

#include "GizmoController.h"

#include "HierarchyLayer.h"
#include "GE/Scene/Components.h"

#include <glm/gtc/type_ptr.hpp>

namespace GE {

GizmoController::GizmoController(std::shared_ptr<EditorContext> context, HierarchyLayer *hierarchy)
    : m_Context(std::move(context)), m_Hierarchy(hierarchy) {
}

void GizmoController::Render(const Camera &camera, const glm::vec2 &viewportPos, const glm::vec2 &viewportSize) {
    if (!m_Context || !m_Hierarchy) {
        return;
    }

    // 无选中实体、或选中实体没有 Transform 组件时，不绘制 gizmo
    Entity selected = m_Hierarchy->GetSelectedEntity();
    if (!selected || !selected.HasComponent<TransformComponent>()) {
        return;
    }

    ImGuizmo::BeginFrame();
    // 关键：BeginFrame 内部新建的全屏 NoInputs "gizmo" 窗口不会被当成悬停窗口，
    // 若不重设 draw list，则 IsHoveringWindow() 命中失败导致 gizmo 拖不动。
    // 这里把 draw list 绑定到当前 Scene 窗口（本函数正是在其 Begin/End 内被调用），
    // 使 _OwnerName 为 "Scene"，鼠标悬停命中与拖拽正常。
    ImGuizmo::SetDrawlist(ImGui::GetWindowDrawList());

    // ---- 快捷键切换操作模式（W/E/R）----
    if (ImGui::IsKeyPressed(ImGuiKey_W)) m_Operation = ImGuizmo::TRANSLATE;
    if (ImGui::IsKeyPressed(ImGuiKey_E)) m_Operation = ImGuizmo::ROTATE;
    if (ImGui::IsKeyPressed(ImGuiKey_R)) m_Operation = ImGuizmo::SCALE;

    // ---- 视口左上角工具条：切换模式 + 吸附开关 ----
    ImGui::SetCursorScreenPos(ImVec2(viewportPos.x + 10.0f, viewportPos.y + 10.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(6, 3));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(4, 4));

    const ImVec4 activeColor{0.2f, 0.6f, 0.85f, 1.0f};
    const ImVec4 idleColor{0.25f, 0.25f, 0.28f, 1.0f};

    ImGui::PushStyleColor(ImGuiCol_Button, m_Operation == ImGuizmo::TRANSLATE ? activeColor : idleColor);
    if (ImGui::Button("Move")) m_Operation = ImGuizmo::TRANSLATE;
    ImGui::PopStyleColor();
    ImGui::SameLine();

    ImGui::PushStyleColor(ImGuiCol_Button, m_Operation == ImGuizmo::ROTATE ? activeColor : idleColor);
    if (ImGui::Button("Rotate")) m_Operation = ImGuizmo::ROTATE;
    ImGui::PopStyleColor();
    ImGui::SameLine();

    ImGui::PushStyleColor(ImGuiCol_Button, m_Operation == ImGuizmo::SCALE ? activeColor : idleColor);
    if (ImGui::Button("Scale")) m_Operation = ImGuizmo::SCALE;
    ImGui::PopStyleColor();
    ImGui::SameLine();

    ImGui::Checkbox("Snap", &m_UseSnap);
    ImGui::PopStyleVar(2);

    // ---- 取出选中实体的局部变换矩阵（gizmo 编辑的是局部 TRS，写回也只碰局部） ----
    auto &transformComp = selected.GetComponent<TransformComponent>();
    glm::mat4 transform = transformComp.GetLocalMatrix();

    ImGuizmo::SetRect(viewportPos.x, viewportPos.y, viewportSize.x, viewportSize.y);

    // ---- 吸附参数（按操作类型取对应步长）----
    float snapValues[3] = {m_SnapTranslation, m_SnapRotation, m_SnapScale};
    float *snap = m_UseSnap ? snapValues : nullptr;

    // ImGuizmo 按 OpenGL 惯例解析投影（NDC Y 向上，见 ImGuizmo.cpp worldToPos 的
    // trans.y = 1.f - trans.y）。引擎渲染用的 proj 已做 Vulkan Y 翻转（Camera::GetProj
    // 的 proj[1][1] *= -1），直接传入会让 gizmo 垂直镜像、拖拽跟随错位。
    // 故这里把 [1][1] 再翻转一次，还原成 OpenGL 投影再交给 ImGuizmo，使其与渲染对齐。
    glm::mat4 proj = camera.GetProj();
    proj[1][1] *= -1.0f;

    bool changed = ImGuizmo::Manipulate(
        glm::value_ptr(camera.GetView()),
        glm::value_ptr(proj),
        m_Operation, m_Mode,
        glm::value_ptr(transform),
        nullptr, // deltaMatrix：无需输出增量矩阵
        snap);   // snap：吸附步长（第 7 参）

    if (changed && ImGuizmo::IsUsing()) {
        // 从 gizmo 更新后的矩阵拆回 平移/旋转(度)/缩放 写回组件
        float translation[3], rotationDeg[3], scale[3];
        ImGuizmo::DecomposeMatrixToComponents(glm::value_ptr(transform), translation, rotationDeg, scale);
        transformComp.Translation = {translation[0], translation[1], translation[2]};
        // ImGuizmo 拆出欧拉角（角度），在此编辑器边界转成四元数写回
        transformComp.SetRotationEuler(glm::radians(glm::vec3(rotationDeg[0], rotationDeg[1], rotationDeg[2])));
        transformComp.Scale = {scale[0], scale[1], scale[2]};
    }
}

} // namespace GE