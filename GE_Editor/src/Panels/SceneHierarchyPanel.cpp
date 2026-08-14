//
// 场景层级面板实现 —— 展示场景实体树与组件属性编辑器
//

#include "SceneHierarchyPanel.h"

#include <imgui.h>
#include <imgui_internal.h>

#include "GE/Scene/Components.h"
#include "GE/Scene/Scene.h"
#include "GE/Physics/PhysicsWorld.h"
#include "GE/Render/Camera.h"
#include "GE/Render/TextureManager.h"
#include "GE/Render/Renderer.h"
#include "GE/Render/Mesh.h"
#include "GE/Render/MeshManager.h"
#include "GE/Utils/PlatformUtils.h"

#include <glm/gtc/type_ptr.hpp>
#include <cmath>
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

    // 根上下文取一次停靠目标 ID（与 DockSpaceLayer 中 GetID("MainDockspace") 一致）
    if (m_DockSpaceID == 0) {
        m_DockSpaceID = ImGui::GetID("MainDockspace");
    }

    ImGui::SetNextWindowDockID(m_DockSpaceID, ImGuiCond_FirstUseEver);
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
    ImGui::SetNextWindowDockID(m_DockSpaceID, ImGuiCond_FirstUseEver);
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
static bool DrawVec3Control(const std::string &label, glm::vec3 &values, float resetValue = 0.0f, float columnWidth = 100.0f) {
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
    changed |= ImGui::DragFloat("##X", &values.x, 0.1f, 0.0f, 0.0f, "%.2f");
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
    changed |= ImGui::DragFloat("##Y", &values.y, 0.1f, 0.0f, 0.0f, "%.2f");
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
    changed |= ImGui::DragFloat("##Z", &values.z, 0.1f, 0.0f, 0.0f, "%.2f");
    ImGui::PopItemWidth();

    ImGui::PopStyleVar();

    ImGui::Columns(1);

    ImGui::PopID();

    return changed;
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
// 绘制选中实体的所有组件（顶层编排函数）
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

    DrawAddComponentPopup();
    ImGui::PopItemWidth();

    // ---- 各组件按类别编排 ----
    DrawComponent<TransformComponent>("Transform", entity,
        [](auto &c) { DrawTransformComponent(c); });

    DrawComponent<CameraComponent>("Camera", entity,
        [](auto &c) { DrawCameraComponent(c); });

    DrawComponent<MeshComponent>("Mesh Renderer", entity,
        [](auto &c) { DrawMeshComponent(c); });

    DrawComponent<SpriteRendererComponent>("Sprite Renderer", entity,
        [](auto &c) { DrawSpriteRendererComponent(c); });

    DrawComponent<PointLightComponent>("Point Light", entity,
        [](auto &c) { DrawPointLightComponent(c); });

    DrawComponent<DirectionalLightComponent>("Directional Light", entity,
        [](auto &c) { DrawDirectionalLightComponent(c); });

    DrawComponent<AmbientLightComponent>("Ambient Light", entity,
        [](auto &c) { DrawAmbientLightComponent(c); });

    DrawComponent<RigidBodyComponent>("Rigid Body", entity,
        [&](auto &c) { DrawRigidBodyComponent(entity, c); });

    DrawComponent<BoxColliderComponent>("Box Collider", entity,
        [&](auto &c) { DrawBoxColliderComponent(entity, c); });

    DrawComponent<SphereColliderComponent>("Sphere Collider", entity,
        [&](auto &c) { DrawSphereColliderComponent(entity, c); });

    // ---- Script 组件（无模板外的特殊条件，这里仅保留特殊标记） ----
    if (entity.HasComponent<ScriptComponent>()) {
        DrawComponent<ScriptComponent>("Script", entity,
            [](auto &c) { DrawScriptComponent(c); });
    }
}

// ============================================================
// Add Component 弹窗
// ============================================================
void SceneHierarchyPanel::DrawAddComponentPopup() {
    if (!ImGui::BeginPopup("AddComponent"))
        return;

    // 每种组件一行：重复的 "判重 + 添加 + 警告 + 关闭弹窗" 模板收敛到 TryAddComponent
    TryAddComponent<CameraComponent>("Camera");
    TryAddComponent<MeshComponent>("Mesh Renderer");
    TryAddComponent<SpriteRendererComponent>("Sprite Renderer");
    TryAddComponent<PointLightComponent>("Point Light");
    TryAddComponent<DirectionalLightComponent>("Directional Light");
    TryAddComponent<AmbientLightComponent>("Ambient Light");
    TryAddComponent<RigidBodyComponent>("Rigid Body");
    TryAddComponent<BoxColliderComponent>("Box Collider");
    TryAddComponent<SphereColliderComponent>("Sphere Collider");

    ImGui::EndPopup();
}

/// 为当前选中实体尝试添加一个组件：
/// - 若不存在则添加并关闭弹窗，返回 true
/// - 若已存在则输出警告并关闭弹窗，返回 false
///
/// 注意：操作对象是 m_SelectionContext（弹窗操作的语义就是给"选中实体"加组件），
///       而非 DrawComponents 的参数 entity——两者在正常流程下是同一个实体。
template <typename T>
bool SceneHierarchyPanel::TryAddComponent(const char *name) {
    if (!ImGui::MenuItem(name))
        return false;

    if (!m_SelectionContext.HasComponent<T>()) {
        m_SelectionContext.AddComponent<T>();
    } else {
        GE_CORE_WARN("This entity already has {0}!", name);
    }
    ImGui::CloseCurrentPopup();
    return true;
}

// ============================================================
// Transform 组件
// ============================================================
void SceneHierarchyPanel::DrawTransformComponent(TransformComponent &component) {
    DrawVec3Control("Translation", component.Translation, 0.0f, 120);

    // 旋转使用角度显示，内部存储弧度
    glm::vec3 rotationDeg = glm::degrees(component.Rotation);
    DrawVec3Control("Rotation", rotationDeg, 0.0f, 120);
    component.Rotation = glm::radians(rotationDeg);

    DrawVec3Control("Scale", component.Scale, 1.0f, 120);
}

// ============================================================
// Camera 组件
// ============================================================
void SceneHierarchyPanel::DrawCameraComponent(CameraComponent &component) {
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
}

// ============================================================
// Mesh 组件
// ============================================================
void SceneHierarchyPanel::DrawMeshComponent(MeshComponent &component) {
    ImGui::ColorEdit4("Color", glm::value_ptr(component.Color));

    // ---- 网格选择下拉框 ----
    // 列出内置几何体 + 所有已加载网格，选择即替换组件的 MeshPtr。
    auto &meshMgr = Renderer::GetMeshManager();

    // 内置几何体（选择时按需加载并缓存）
    const char *builtins[] = {"cube", "sphere", "plane", "quad"};

    // 当前网格的显示标识（GetFilePath 为 builtin:xxx 或模型文件路径）
    std::string currentKey = "(null)";
    if (component.MeshPtr) {
        currentKey = component.MeshPtr->GetFilePath();
    }

    if (ImGui::BeginCombo("Mesh", currentKey.c_str())) {
        // None 选项
        if (ImGui::Selectable("(null)", component.MeshPtr == nullptr)) {
            component.MeshPtr = nullptr;
        }
        if (component.MeshPtr == nullptr) {
            ImGui::SetItemDefaultFocus();
        }

        // 内置几何体
        for (const char *type : builtins) {
            std::string key = "builtin:" + std::string(type);
            bool isSelected = (component.MeshPtr &&
                               component.MeshPtr->GetFilePath() == key);
            if (ImGui::Selectable(key.c_str(), isSelected)) {
                component.MeshPtr = meshMgr.GetBuiltin(type);
            }
            if (isSelected) {
                ImGui::SetItemDefaultFocus();
            }
        }

        // 已加载的文件模型（排除内置，避免列表重复）
        for (const auto &key : meshMgr.GetAllKeys()) {
            if (key.rfind("builtin:", 0) == 0) {
                continue; // 已由上面的内置项覆盖
            }
            Mesh *mesh = meshMgr.Get(key);
            bool isSelected = (mesh == component.MeshPtr);
            if (ImGui::Selectable(key.c_str(), isSelected)) {
                component.MeshPtr = mesh;
            }
            if (isSelected) {
                ImGui::SetItemDefaultFocus();
            }
        }

        ImGui::EndCombo();
    }

    // ---- 加载模型文件（.obj）----
    if (ImGui::Button("加载模型文件 (OBJ)...")) {
        std::string path = FileDialogs::OpenFile(
            "Wavefront OBJ (*.obj)\0*.obj\0All Files (*.*)\0*.*\0");
        if (!path.empty()) {
            Mesh *mesh = meshMgr.Load(path);
            if (mesh) {
                component.MeshPtr = mesh;
            } else {
                GE_CORE_WARN("SceneHierarchyPanel: 网格加载失败: {0}", path);
                ImGui::OpenPopup("MeshLoadFailed");
            }
        }
    }
    // 加载失败提示
    if (ImGui::BeginPopup("MeshLoadFailed")) {
        ImGui::Text("网格加载失败（请确认是合法的 .obj 文件）");
        if (ImGui::Button("OK")) {
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }

    // 网格信息（只读）
    if (component.MeshPtr) {
        ImGui::Separator();
        ImGui::Text("Path: %s", component.MeshPtr->GetFilePath().c_str());
        ImGui::Text("Vertices: %u", component.MeshPtr->GetVertexCount());
        ImGui::Text("Indices:  %u", component.MeshPtr->GetIndexCount());
    }
}

// ============================================================
// Sprite Renderer 组件
// ============================================================
void SceneHierarchyPanel::DrawSpriteRendererComponent(SpriteRendererComponent &component) {
    ImGui::ColorEdit4("Color", glm::value_ptr(component.Color));
    ImGui::Text("Texture: %s", component.SpriteTexture ? "(assigned)" : "(null)");
    ImGui::Checkbox("Is UI (screen space, no depth)", &component.IsUI);
}

// ============================================================
// Point Light 组件
// ============================================================
void SceneHierarchyPanel::DrawPointLightComponent(PointLightComponent &component) {
    // 颜色 + 强度（alpha 通道作为强度）
    ImGui::ColorEdit4("Color + Intensity", glm::value_ptr(component.Color));

    // 半径倒数（衰减系数）
    ImGui::DragFloat("Radius Inv (attenuation)", &component.RadiusInv, 0.01f, 0.01f, 5.0f);
    ImGui::Text("影响半径 ≈ %.2f", 1.0f / component.RadiusInv);
}

// ============================================================
// Directional Light 组件
// ============================================================
void SceneHierarchyPanel::DrawDirectionalLightComponent(DirectionalLightComponent &component) {
    // 颜色 + 强度（alpha 通道作为强度）
    ImGui::ColorEdit4("Color + Intensity", glm::value_ptr(component.Color));
    ImGui::TextDisabled("照射方向由 Transform 的 Rotation 决定");
}

// ============================================================
// Ambient Light 组件
// ============================================================
void SceneHierarchyPanel::DrawAmbientLightComponent(AmbientLightComponent &component) {
    // 颜色 + 强度（alpha 通道作为强度）
    ImGui::ColorEdit4("Color + Intensity", glm::value_ptr(component.Color));
    ImGui::TextDisabled("全局环境光，不依赖 Transform");
}

// ============================================================
// Rigid Body 组件
// ============================================================
void SceneHierarchyPanel::DrawRigidBodyComponent(Entity entity, RigidBodyComponent &component) {
    Physics::PhysicsWorld *physicsWorld = m_Context->GetPhysicsWorld();
    const auto entityHandle = (entt::entity)entity;

    // 刚体类型下拉框（类型改变需要重建 body）
    {
        const char *typeStrings[] = {"Static", "Kinematic", "Dynamic"};
        int currentType = static_cast<int>(component.Type);
        if (ImGui::BeginCombo("Type", typeStrings[currentType])) {
            for (int i = 0; i < 3; i++) {
                const bool isSelected = currentType == i;
                if (ImGui::Selectable(typeStrings[i], isSelected)) {
                    component.Type = static_cast<Physics::RigidBodyType>(i);
                    // 运动类型改变 → 销毁并重建刚体
                    if (physicsWorld)
                        physicsWorld->RebuildRigidBody(entityHandle);
                }
                if (isSelected)
                    ImGui::SetItemDefaultFocus();
            }
            ImGui::EndCombo();
        }
    }

    // 质量（仅动态体有效，改变后需要更新属性）
    bool massDisabled = (component.Type != Physics::RigidBodyType::Dynamic);
    if (massDisabled) ImGui::BeginDisabled();
    if (ImGui::DragFloat("Mass (kg)", &component.Mass, 0.1f, 0.01f, 10000.0f)) {
        if (physicsWorld)
            physicsWorld->UpdateRigidBodyProperties(entityHandle);
    }
    if (massDisabled) ImGui::EndDisabled();

    // 摩擦系数
    if (ImGui::DragFloat("Friction", &component.Friction, 0.01f, 0.0f, 1.0f)) {
        if (physicsWorld)
            physicsWorld->UpdateRigidBodyProperties(entityHandle);
    }

    // 弹性系数
    if (ImGui::DragFloat("Restitution", &component.Restitution, 0.01f, 0.0f, 1.0f)) {
        if (physicsWorld)
            physicsWorld->UpdateRigidBodyProperties(entityHandle);
    }

    // 线性阻尼
    if (ImGui::DragFloat("Linear Damping", &component.LinearDamping, 0.01f, 0.0f, 10.0f)) {
        if (physicsWorld)
            physicsWorld->UpdateRigidBodyProperties(entityHandle);
    }

    // 角阻尼
    if (ImGui::DragFloat("Angular Damping", &component.AngularDamping, 0.01f, 0.0f, 10.0f)) {
        if (physicsWorld)
            physicsWorld->UpdateRigidBodyProperties(entityHandle);
    }

    // 传感器标记（改变后需要更新属性）
    if (ImGui::Checkbox("Is Sensor (Trigger)", &component.IsSensor)) {
        if (physicsWorld)
            physicsWorld->UpdateRigidBodyProperties(entityHandle);
    }

    ImGui::Separator();
    ImGui::TextDisabled("Runtime Body ID: %s", component.IsInitialized ? "valid" : "uninitialized");
}

// ============================================================
// Box Collider 组件
// ============================================================
void SceneHierarchyPanel::DrawBoxColliderComponent(Entity entity, BoxColliderComponent &component) {
    Physics::PhysicsWorld *physicsWorld = m_Context->GetPhysicsWorld();
    const auto entityHandle = (entt::entity)entity;

    // 形状改变 → 重建刚体
    if (DrawVec3Control("Half Extents", component.HalfExtents, 0.5f, 120)) {
        if (physicsWorld)
            physicsWorld->RebuildRigidBody(entityHandle);
    }
    if (DrawVec3Control("Offset", component.Offset, 0.0f, 120)) {
        if (physicsWorld)
            physicsWorld->RebuildRigidBody(entityHandle);
    }
}

// ============================================================
// Sphere Collider 组件
// ============================================================
void SceneHierarchyPanel::DrawSphereColliderComponent(Entity entity, SphereColliderComponent &component) {
    Physics::PhysicsWorld *physicsWorld = m_Context->GetPhysicsWorld();
    const auto entityHandle = (entt::entity)entity;

    // 形状改变 → 重建刚体
    if (ImGui::DragFloat("Radius", &component.Radius, 0.05f, 0.001f, 1000.0f)) {
        if (physicsWorld)
            physicsWorld->RebuildRigidBody(entityHandle);
    }
    if (DrawVec3Control("Offset", component.Offset, 0.0f, 120)) {
        if (physicsWorld)
            physicsWorld->RebuildRigidBody(entityHandle);
    }
}

// ============================================================
// Script 组件
// ============================================================
void SceneHierarchyPanel::DrawScriptComponent(ScriptComponent &component) {
    ImGui::Checkbox("Enabled", &component.Enabled);
    ImGui::Text("Status: %s", component.OnUpdate ? "Active" : "Empty");
    ImGui::TextDisabled("回调由代码逻辑管理，面板中不可编辑");
}

} // namespace GE
