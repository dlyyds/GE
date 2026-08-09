//
// 全屏 DockSpace 宿主层实现：创建主停靠区并建立默认编辑器布局。
//

#include "DockSpaceLayer.h"

#include "imgui.h"
#include "imgui_internal.h" // DockBuilder 系列 API

namespace GE {

DockSpaceLayer::DockSpaceLayer() : Layer("DockSpaceLayer") {
}

DockSpaceLayer::~DockSpaceLayer() = default;

void DockSpaceLayer::OnAttach() {
}

void DockSpaceLayer::OnDetach() {
}

void DockSpaceLayer::OnUpdate(Timestep &ts) {
}

void DockSpaceLayer::OnEvent(Event &event) {
}

// ============================================================
// 默认布局：
//   左列(22%)：Scene Hierarchy(上) + Properties(下)
//   中列：Scene(视口)在上，SceneLayer 占底部 25%
//   右列(24%)：Resource + 渲染统计（tab 并列）
// ============================================================
static void BuildDefaultLayout(ImGuiID dockspace_id, const ImVec2 &size) {
    ImGui::DockBuilderRemoveNode(dockspace_id);
    ImGui::DockBuilderAddNode(dockspace_id, ImGuiDockNodeFlags_DockSpace);
    ImGui::DockBuilderSetNodeSize(dockspace_id, size);

    ImGuiID dock_main = dockspace_id;
    ImGuiID dock_right, dock_left, dock_bottom;

    // 右列：Resource（24%）
    dock_right = ImGui::DockBuilderSplitNode(dock_main, ImGuiDir_Right, 0.24f, nullptr, &dock_main);
    // 左列：Hierarchy + Properties（22%）
    dock_left  = ImGui::DockBuilderSplitNode(dock_main, ImGuiDir_Left, 0.22f, nullptr, &dock_main);
    // 底部：SceneLayer（25%）
    dock_bottom = ImGui::DockBuilderSplitNode(dock_main, ImGuiDir_Down, 0.25f, nullptr, &dock_main);

    // 左列上下分：上 Hierarchy / 下 Properties
    ImGuiID dock_left_top, dock_left_bottom;
    dock_left_top = ImGui::DockBuilderSplitNode(dock_left, ImGuiDir_Down, 0.5f, nullptr, &dock_left_bottom);

    ImGui::DockBuilderDockWindow("Scene Hierarchy", dock_left_top);
    ImGui::DockBuilderDockWindow("Properties", dock_left_bottom);
    ImGui::DockBuilderDockWindow("Scene", dock_main);
    ImGui::DockBuilderDockWindow("SceneLayer", dock_bottom);
    ImGui::DockBuilderDockWindow("Resource", dock_right);
    ImGui::DockBuilderDockWindow("渲染统计", dock_right);

    ImGui::DockBuilderFinish(dockspace_id);
}

void DockSpaceLayer::OnImGuiRender() {
    // 稳定 ID：根上下文取 "MainDockspace"（各窗口在同一根上下文取同值，保证一致）
    const ImGuiID dockspace_id = ImGui::GetID("MainDockspace");

    // 让宿主窗口铺满主视口工作区
    const ImGuiViewport *viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(viewport->WorkPos);
    ImGui::SetNextWindowSize(viewport->WorkSize);
    ImGui::SetNextWindowViewport(viewport->ID);

    ImGuiWindowFlags host_flags =
        ImGuiWindowFlags_MenuBar | ImGuiWindowFlags_NoDocking |
        ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoCollapse |
        ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
        ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoNavFocus;

    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
    ImGui::Begin("##MainDockHost", nullptr, host_flags);

    // 菜单栏
    if (ImGui::BeginMenuBar()) {
        if (ImGui::BeginMenu("文件")) {
            if (ImGui::MenuItem("重置布局")) {
                // 清掉旧停靠节点，下一帧重建默认布局
                ImGui::DockBuilderRemoveNode(dockspace_id);
                m_FirstFrame = true;
            }
            ImGui::EndMenu();
        }
        ImGui::EndMenuBar();
    }

    ImGui::DockSpace(dockspace_id, ImVec2(0.0f, 0.0f), ImGuiDockNodeFlags_None);

    // 首帧（或"重置布局"后）重建默认布局
    if (m_FirstFrame) {
        m_FirstFrame = false;
        BuildDefaultLayout(dockspace_id, viewport->WorkSize);
    }

    ImGui::End();
    ImGui::PopStyleVar(3);
}

} // namespace GE