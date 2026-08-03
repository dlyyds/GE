//
// 场景层级面板实现 —— 展示场景实体树与组件属性编辑器
//

#include "SceneHierarchyPanel.h"

#include <imgui.h>
#include <imgui_internal.h>

#include "GE/Scene/Components.h"
#include "GE/Render/Camera.h"
#include "GE/Render/Mesh.h"

#include <glm/gtc/type_ptr.hpp>
#include <cstring>

namespace GE {

SceneHierarchyPanel::SceneHierarchyPanel(Scene *scene) {
    SetContext(scene);
}

void SceneHierarchyPanel::SetContext(Scene *scene) {
    m_Context = scene;
    m_SelectionContext = {};
}

void SceneHierarchyPanel::SetSelectedEntity(Entity entity) {
    m_SelectionContext = entity;
}

// ============================================================
// 顶层渲染：Scene Hierarchy 窗口 + Properties 窗口
// ============================================================
void SceneHierarchyPanel::OnImGuiRender() {

    if (!m_Context) {
        return;
    }

    ImGui::Begin("Scene Hierarchy");

    // 遍历所有实体（entt::entity 视图），逐个绘制节点
    m_Context->Reg().view<entt::entity>().each([&](auto entityID) {
        const Entity entity{entityID, m_Context};
        DrawEntityNode(entity);
    });

    // 点击空白处取消选中
    if (ImGui::IsMouseDown(0) && ImGui::IsWindowHovered())
        m_SelectionContext = {};

    // 窗口空白处右键菜单：创建空实体
    if (ImGui::BeginPopupContextWindow(nullptr, ImGuiPopupFlags_NoOpenOverItems | ImGuiPopupFlags_MouseButtonRight)) {
        if (ImGui::MenuItem("Create Empty Entity"))
            m_Context->CreateEntity("Empty Entity");
        ImGui::EndPopup();
    }

    ImGui::End();

    // 属性面板
    ImGui::Begin("Properties");
    if (m_SelectionContext) {
        DrawComponents(m_SelectionContext);
    }
    ImGui::End();
}

// ============================================================
// 实体节点绘制（树状列表）
// ============================================================
void SceneHierarchyPanel::DrawEntityNode(Entity entity) {
    auto &tag = entity.GetComponent<TagComponent>().Tag;

    ImGuiTreeNodeFlags flags = ((m_SelectionContext == entity) ? ImGuiTreeNodeFlags_Selected : 0)
                               | ImGuiTreeNodeFlags_OpenOnArrow
                               | ImGuiTreeNodeFlags_SpanAllColumns;
    // 叶子节点（无子实体）使用 Bullet 样式，避免展开箭头
    flags |= ImGuiTreeNodeFlags_Leaf;

    bool opened [[maybe_unused]] = ImGui::TreeNodeEx((void *)(uint64_t)(uint32_t)entity, flags, "%s", tag.c_str());
    if (ImGui::IsItemClicked()) {
        m_SelectionContext = entity;
    }

    bool entityDeleted = false;
    if (ImGui::BeginPopupContextItem()) {
        if (ImGui::MenuItem("Delete Entity"))
            entityDeleted = true;
        ImGui::EndPopup();
    }

    // 由于使用了 Leaf 标记，这里无需处理子节点，直接 TreePop 即可
    ImGui::TreePop();

    if (entityDeleted) {
        m_Context->DestroyEntity(entity);
        if (m_SelectionContext == entity)
            m_SelectionContext = {};
    }
}

// ============================================================
// 辅助：Vec3 控件（带 X/Y/Z 重置按钮）
// ============================================================
static void DrawVec3Control(const std::string &label, glm::vec3 &values, float resetValue = 0.0f, float columnWidth = 100.0f) {
    ImGui::PushID(label.c_str());

    const auto boldFont = ImGui::GetIO().Fonts->Fonts[0];

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
    if (ImGui::Button("X", buttonSize))
        values.x = resetValue;
    ImGui::PopStyleColor(3);
    ImGui::PopFont();

    ImGui::SameLine();
    ImGui::DragFloat("##X", &values.x, 0.1f, 0.0f, 0.0f, "%.2f");
    ImGui::PopItemWidth();
    ImGui::SameLine();

    // ---- Y ----
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4{0.2f, 0.7f, 0.2f, 1.0f});
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4{0.3f, 0.8f, 0.3f, 1.0f});
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4{0.2f, 0.7f, 0.2f, 1.0f});
    ImGui::PushFont(boldFont);
    if (ImGui::Button("Y", buttonSize))
        values.y = resetValue;
    ImGui::PopStyleColor(3);
    ImGui::PopFont();

    ImGui::SameLine();
    ImGui::DragFloat("##Y", &values.y, 0.1f, 0.0f, 0.0f, "%.2f");
    ImGui::PopItemWidth();
    ImGui::SameLine();

    // ---- Z ----
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4{0.1f, 0.25f, 0.8f, 1.0f});
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4{0.2f, 0.35f, 0.9f, 1.0f});
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4{0.1f, 0.25f, 0.8f, 1.0f});
    ImGui::PushFont(boldFont);
    if (ImGui::Button("Z", buttonSize))
        values.z = resetValue;
    ImGui::PopStyleColor(3);
    ImGui::PopFont();

    ImGui::SameLine();
    ImGui::DragFloat("##Z", &values.z, 0.1f, 0.0f, 0.0f, "%.2f");
    ImGui::PopItemWidth();

    ImGui::PopStyleVar();

    ImGui::Columns(1);

    ImGui::PopID();
}

// ============================================================
// 辅助：通用组件绘制模板（带折叠头 + 删除菜单）
// ============================================================
template <typename T, typename UIFunction>
static void DrawComponent(const char *name, Entity entity, UIFunction uiFunction) {

    constexpr ImGuiTreeNodeFlags treeNodeFlags =
        ImGuiTreeNodeFlags_DefaultOpen | ImGuiTreeNodeFlags_Framed | ImGuiTreeNodeFlags_SpanAvailWidth |
        ImGuiTreeNodeFlags_AllowOverlap | ImGuiTreeNodeFlags_FramePadding;

    if (entity.HasComponent<T>()) {
        ImGui::PushID(typeid(T).hash_code());
        const ImVec2 contentRegionAvailable = ImGui::GetContentRegionAvail();

        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2{4, 4});
        float lineHeight = GImGui->FontSize + GImGui->Style.FramePadding.y * 2.0f;
        ImGui::Separator();
        bool open = ImGui::TreeNodeEx((void *)typeid(T).hash_code(), treeNodeFlags, "%s", name);
        ImGui::PopStyleVar();
        ImGui::SameLine(contentRegionAvailable.x - lineHeight * 0.5f - 5);
        if (ImGui::Button("+", ImVec2{lineHeight, lineHeight})) {
            ImGui::OpenPopup("ComponentSettings");
        }

        bool removeComponent = false;
        if (ImGui::BeginPopup("ComponentSettings")) {
            if (ImGui::MenuItem("Remove component"))
                removeComponent = true;
            ImGui::EndPopup();
        }

        if (open) {
            uiFunction(entity.GetComponent<T>());
            ImGui::TreePop();
        }
        ImGui::PopID();

        if (removeComponent)
            entity.RemoveComponent<T>();
    }
}

// ============================================================
// 绘制选中实体的所有组件
// ============================================================
void SceneHierarchyPanel::DrawComponents(Entity entity) {

    // ---- Tag 组件（特殊处理：顶部名称 + Add Component 按钮） ----
    if (entity.HasComponent<TagComponent>()) {
        auto &tag = entity.GetComponent<TagComponent>().Tag;
        char buffer[256] = {};
        strncpy_s(buffer, sizeof(buffer), tag.c_str(), _TRUNCATE);
        if (ImGui::InputText("##Tag", buffer, sizeof(buffer))) {
            tag = std::string(buffer);
        }
    }
    ImGui::SameLine();
    ImGui::PushItemWidth(-1);

    if (ImGui::Button("Add Component"))
        ImGui::OpenPopup("AddComponent");

    if (ImGui::BeginPopup("AddComponent")) {

        if (ImGui::MenuItem("Camera")) {
            if (!m_SelectionContext.HasComponent<CameraComponent>())
                m_SelectionContext.AddComponent<CameraComponent>();
            else
                GE_CORE_WARN("This entity already has Camera Component!");
            ImGui::CloseCurrentPopup();
        }

        if (ImGui::MenuItem("Mesh Renderer")) {
            if (!m_SelectionContext.HasComponent<MeshComponent>())
                m_SelectionContext.AddComponent<MeshComponent>();
            else
                GE_CORE_WARN("This entity already has Mesh Component!");
            ImGui::CloseCurrentPopup();
        }

        if (ImGui::MenuItem("Sprite Renderer")) {
            if (!m_SelectionContext.HasComponent<SpriteRendererComponent>())
                m_SelectionContext.AddComponent<SpriteRendererComponent>();
            else
                GE_CORE_WARN("This entity already has Sprite Renderer Component!");
            ImGui::CloseCurrentPopup();
        }

        if (ImGui::MenuItem("Point Light")) {
            if (!m_SelectionContext.HasComponent<PointLightComponent>())
                m_SelectionContext.AddComponent<PointLightComponent>();
            else
                GE_CORE_WARN("This entity already has Point Light Component!");
            ImGui::CloseCurrentPopup();
        }

        ImGui::EndPopup();
    }
    ImGui::PopItemWidth();

    // ---- Transform 组件 ----
    DrawComponent<TransformComponent>("Transform", entity, [](auto &component) {
        DrawVec3Control("Translation", component.Translation, 0.0f, 120);

        // 旋转使用角度显示，内部存储弧度
        glm::vec3 rotationDeg = glm::degrees(component.Rotation);
        DrawVec3Control("Rotation", rotationDeg, 0.0f, 120);
        component.Rotation = glm::radians(rotationDeg);

        DrawVec3Control("Scale", component.Scale, 1.0f, 120);
    });

    // ---- Camera 组件 ----
    DrawComponent<CameraComponent>("Camera", entity, [](auto &component) {
        auto &camera = component.CameraInstance;

        ImGui::Checkbox("Primary", &component.Primary);
        ImGui::SameLine();
        ImGui::Checkbox("Fixed Aspect Ratio", &component.FixedAspectRatio);

        // 相机模式
        const char *modeStrings[] = {"Orbit", "FPS"};
        int currentMode = static_cast<int>(camera.GetMode());
        if (ImGui::BeginCombo("Mode", modeStrings[currentMode])) {
            for (int i = 0; i < 2; i++) {
                const bool isSelected = currentMode == i;
                if (ImGui::Selectable(modeStrings[i], isSelected)) {
                    camera.SetMode(static_cast<Camera::Mode>(i));
                }
                if (isSelected)
                    ImGui::SetItemDefaultFocus();
            }
            ImGui::EndCombo();
        }

        // FOV（Camera 内部使用度数）
        float fov = camera.GetFov();
        if (ImGui::SliderFloat("FOV (deg)", &fov, 10.0f, 120.0f)) {
            camera.SetPerspective(fov, camera.GetAspect(), 0.1f, 100.0f);
        }

        // 宽高比
        float aspect = camera.GetAspect();
        if (ImGui::DragFloat("Aspect Ratio", &aspect, 0.01f, 0.1f, 10.0f)) {
            camera.SetAspect(aspect);
        }

        ImGui::Separator();

        if (camera.GetMode() == Camera::Mode::Orbit) {
            // 轨道相机参数（Theta/Phi/Distance 内部均为度数）
            glm::vec3 target = camera.GetTarget();
            if (ImGui::DragFloat3("Target", &target.x, 0.1f))
                camera.SetTarget(target);

            float theta = camera.GetTheta();
            float phi   = camera.GetPhi();
            float dist  = camera.GetDistance();

            bool orbitChanged = false;
            orbitChanged |= ImGui::SliderFloat("Theta (deg)", &theta, -180.0f, 180.0f);
            orbitChanged |= ImGui::SliderFloat("Phi (deg)", &phi, -89.0f, 89.0f);
            orbitChanged |= ImGui::DragFloat("Distance", &dist, 0.1f, 0.5f, 50.0f);
            if (orbitChanged) {
                camera.SetOrbit(theta, phi, dist);
            }
        } else {
            // FPS 相机参数（Yaw/Pitch 内部均为度数）
            glm::vec3 pos = camera.GetPosition();
            if (ImGui::DragFloat3("Position", &pos.x, 0.1f))
                camera.SetPosition(pos);

            float yaw   = camera.GetYaw();
            float pitch = camera.GetPitch();
            bool fpChanged = false;
            fpChanged |= ImGui::SliderFloat("Yaw (deg)", &yaw, -180.0f, 180.0f);
            fpChanged |= ImGui::SliderFloat("Pitch (deg)", &pitch, -89.0f, 89.0f);
            if (fpChanged) {
                camera.SetYawPitch(yaw, pitch);
            }
        }

        ImGui::Separator();
        ImGui::DragFloat("Mouse Sensitivity", &camera.MouseSensitivity, 0.01f, 0.01f, 5.0f);
        ImGui::DragFloat("Scroll Sensitivity", &camera.ScrollSensitivity, 0.05f, 0.1f, 10.0f);
        ImGui::DragFloat("Move Speed", &camera.MoveSpeed, 0.1f, 0.1f, 20.0f);
    });

    // ---- Mesh 组件 ----
    DrawComponent<MeshComponent>("Mesh Renderer", entity, [](auto &component) {
        ImGui::ColorEdit4("Color", glm::value_ptr(component.Color));

        // 显示网格信息（只读）
        ImGui::Text("Mesh: %s", component.MeshPtr ? "(assigned)" : "(null)");
        if (component.MeshPtr) {
            ImGui::Text("  Vertices: %u", component.MeshPtr->GetVertexCount());
            ImGui::Text("  Indices:  %u", component.MeshPtr->GetIndexCount());
        }

        ImGui::Text("Texture: %s", component.BaseTexture ? "(assigned)" : "(null)");
    });

    // ---- Sprite Renderer 组件 ----
    DrawComponent<SpriteRendererComponent>("Sprite Renderer", entity, [](auto &component) {
        ImGui::ColorEdit4("Color", glm::value_ptr(component.Color));
        ImGui::Text("Texture: %s", component.SpriteTexture ? "(assigned)" : "(null)");
        ImGui::Checkbox("Is UI (screen space, no depth)", &component.IsUI);
    });

    // ---- Point Light 组件 ----
    DrawComponent<PointLightComponent>("Point Light", entity, [](auto &component) {
        // 颜色 + 强度（alpha 通道作为强度）
        ImGui::ColorEdit4("Color + Intensity", glm::value_ptr(component.Color));

        // 半径倒数（衰减系数）
        ImGui::DragFloat("Radius Inv (attenuation)", &component.RadiusInv, 0.01f, 0.01f, 5.0f);
        ImGui::Text("影响半径 ≈ %.2f", 1.0f / component.RadiusInv);
    });

    // ---- Script 组件 ----
    if (entity.HasComponent<ScriptComponent>()) {
        DrawComponent<ScriptComponent>("Script", entity, [](auto &component) {
            ImGui::Checkbox("Enabled", &component.Enabled);
            ImGui::Text("Status: %s", component.OnUpdate ? "Active" : "Empty");
            ImGui::TextDisabled("回调由代码逻辑管理，面板中不可编辑");
        });
    }
}

} // namespace GE
