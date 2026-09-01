//
// 动画状态机节点图面板实现 —— 阶段 A：占位窗口。
// 阶段 B 起把 AnimStateMachineComponent 画成节点图（imgui-node-editor）。
//

#include "Panels/ASMGraphPanel.h"

#include "HierarchyLayer.h"
#include "GE/Scene/Components.h"

#include "imgui.h"

namespace GE {

ASMGraphPanel::ASMGraphPanel(std::shared_ptr<EditorContext> context, HierarchyLayer *hierarchy)
    : m_Context(std::move(context)), m_Hierarchy(hierarchy) {
}

void ASMGraphPanel::OnImGuiRender() {
    // 根上下文取一次停靠目标 ID（与 DockSpaceLayer 中 GetID("MainDockspace") 一致）
    if (m_DockSpaceID == 0) {
        m_DockSpaceID = ImGui::GetID("MainDockspace");
    }

    ImGui::SetNextWindowDockID(m_DockSpaceID, ImGuiCond_FirstUseEver);
    ImGui::Begin("动画状态机图");

    // 无场景 / 无选中实体 / 选中实体无 ASM 组件 → 占位提示
    if (!m_Context || !m_Hierarchy) {
        ImGui::TextDisabled("未绑定场景与层级面板");
        ImGui::End();
        return;
    }

    Entity selected = m_Hierarchy->GetSelectedEntity();
    if (!selected || !selected.HasComponent<AnimStateMachineComponent>()) {
        ImGui::TextDisabled("请先在 Scene Hierarchy 中选中带「Anim State Machine」组件的实体");
        ImGui::End();
        return;
    }

    ImGui::TextUnformatted("（阶段 A 占位：节点图渲染将在阶段 B 落地）");
    ImGui::End();
}

} // namespace GE
