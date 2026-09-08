//
// SceneHierarchyPanel 拆分后的内部共享头 —— 存放跨组件面板复用的辅助工具。
//
// 由 SceneHierarchyPanel*.cpp 各翻译单元各自 include；仅放需要跨 .cpp 共享的实现，
// 单一 .cpp 内才用的辅助仍留在各自文件中保持 static。
//

#pragma once

#include <imgui.h>
#include <imgui_internal.h>

#include <glm/glm.hpp>

#include <string>

namespace GE {

// ============================================================
// 辅助：Vec3 控件（带 X/Y/Z 重置按钮）
// ============================================================
inline bool DrawVec3Control(const std::string &label, glm::vec3 &values, float resetValue = 0.0f, float columnWidth = 100.0f) {
    ImGui::PushID(label.c_str());

    const auto boldFont = ImGui::GetIO().Fonts->Fonts[0];
    bool changed = false;

    ImGui::Columns(2);
    ImGui::SetColumnWidth(0, columnWidth);
    ImGui::Text("%s", label.c_str());
    ImGui::NextColumn();

    ImGui::PushMultiItemsWidths(3, ImGui::CalcItemWidth());
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2{0, 0});

    float lineHeight = GImGui->FontSize + GImGui->Style.FramePadding.y * 2.0f;
    ImVec2 buttonSize = {lineHeight + 3.0f, lineHeight};

    // ---- X ----
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4{0.8f, 0.1f, 0.15f, 1.0f});
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4{0.9f, 0.2f, 0.2f, 1.0f});
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4{0.8f, 0.1f, 0.15f, 1.0f});
    ImGui::PushFont(boldFont);
    if (ImGui::Button("X", buttonSize)) {
        values.x = resetValue;
        changed = true;
    }
    ImGui::PopStyleColor(3);
    ImGui::PopFont();

    ImGui::SameLine();
    // 输入框底色带 X 轴淡红，让「轴按钮 → 数值框」一眼对应（悬停/拖动时加深）
    ImGui::PushStyleColor(ImGuiCol_FrameBg,        ImVec4{0.8f, 0.1f, 0.15f, 0.08f});
    ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, ImVec4{0.8f, 0.1f, 0.15f, 0.16f});
    ImGui::PushStyleColor(ImGuiCol_FrameBgActive,  ImVec4{0.8f, 0.1f, 0.15f, 0.22f});
    changed |= ImGui::DragFloat("##X", &values.x, 0.1f, 0.0f, 0.0f, "%.2f");
    ImGui::PopStyleColor(3);
    ImGui::PopItemWidth();
    ImGui::SameLine();

    // ---- Y ----
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4{0.2f, 0.7f, 0.2f, 1.0f});
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4{0.3f, 0.8f, 0.3f, 1.0f});
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4{0.2f, 0.7f, 0.2f, 1.0f});
    ImGui::PushFont(boldFont);
    if (ImGui::Button("Y", buttonSize)) {
        values.y = resetValue;
        changed = true;
    }
    ImGui::PopStyleColor(3);
    ImGui::PopFont();

    ImGui::SameLine();
    // 输入框底色带 Y 轴淡绿
    ImGui::PushStyleColor(ImGuiCol_FrameBg,        ImVec4{0.2f, 0.7f, 0.2f, 0.08f});
    ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, ImVec4{0.2f, 0.7f, 0.2f, 0.16f});
    ImGui::PushStyleColor(ImGuiCol_FrameBgActive,  ImVec4{0.2f, 0.7f, 0.2f, 0.22f});
    changed |= ImGui::DragFloat("##Y", &values.y, 0.1f, 0.0f, 0.0f, "%.2f");
    ImGui::PopStyleColor(3);
    ImGui::PopItemWidth();
    ImGui::SameLine();

    // ---- Z ----
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4{0.1f, 0.25f, 0.8f, 1.0f});
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4{0.2f, 0.35f, 0.9f, 1.0f});
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4{0.1f, 0.25f, 0.8f, 1.0f});
    ImGui::PushFont(boldFont);
    if (ImGui::Button("Z", buttonSize)) {
        values.z = resetValue;
        changed = true;
    }
    ImGui::PopStyleColor(3);
    ImGui::PopFont();

    ImGui::SameLine();
    // 输入框底色带 Z 轴淡蓝
    ImGui::PushStyleColor(ImGuiCol_FrameBg,        ImVec4{0.1f, 0.25f, 0.8f, 0.08f});
    ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, ImVec4{0.1f, 0.25f, 0.8f, 0.16f});
    ImGui::PushStyleColor(ImGuiCol_FrameBgActive,  ImVec4{0.1f, 0.25f, 0.8f, 0.22f});
    changed |= ImGui::DragFloat("##Z", &values.z, 0.1f, 0.0f, 0.0f, "%.2f");
    ImGui::PopStyleColor(3);
    ImGui::PopItemWidth();

    ImGui::PopStyleVar();

    ImGui::Columns(1);

    ImGui::PopID();

    return changed;
}

} // namespace GE
