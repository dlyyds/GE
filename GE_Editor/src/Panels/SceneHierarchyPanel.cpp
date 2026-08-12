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
#include "GE/Render/Material.h"
#include "GE/Render/MaterialManager.h"
#include "GE/Render/TextureManager.h"
#include "GE/Render/Renderer.h"
#include "GE/Render/Mesh.h"

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

    DrawComponent<MaterialComponent>("Material", entity,
        [](auto &c) { DrawMaterialComponent(c); });

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
    TryAddComponent<MaterialComponent>("Material");
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

    // 显示网格信息（只读）
    ImGui::Text("Mesh: %s", component.MeshPtr ? "(assigned)" : "(null)");
    if (component.MeshPtr) {
        // 文件路径（内置几何体为 builtin:xxx 前缀）
        ImGui::Text("  Path: %s", component.MeshPtr->GetFilePath().c_str());
        ImGui::Text("  Vertices: %u", component.MeshPtr->GetVertexCount());
        ImGui::Text("  Indices:  %u", component.MeshPtr->GetIndexCount());
    }
}

// ============================================================
// Material 组件
// ============================================================

/**
 * @brief 绘制单个纹理槽位的下拉选择器。
 *
 * 从 TextureManager 获取所有已加载纹理，展示为下拉列表供选择。
 * 选择 "None" 会清除该槽位的纹理。
 *
 * @param label     槽位标签（如 "Albedo"）
 * @param material  材质指针
 * @param slot      纹理槽位
 * @return 是否有修改
 */
static bool DrawTextureSlot(const char *label, Material *material, Material::TextureSlot slot) {
    if (!material) {
        ImGui::Text("%s: (no material)", label);
        return false;
    }

    auto &texMgr = Renderer::GetTextureManager();
    auto allKeys = texMgr.GetAllKeys();

    // 当前纹理的 key（用于在下拉框中显示预览文本）
    Texture *currentTex = material->GetTexture(slot);
    std::string currentPreview = "(none)";
    if (currentTex) {
        // 尝试在管理器中找到该纹理的 key
        for (const auto &key : allKeys) {
            if (texMgr.Get(key) == currentTex) {
                currentPreview = key;
                break;
            }
        }
        // 如果在管理器中没找到（可能是未注册的程序化纹理），显示路径或占位符
        if (currentPreview == "(none)" && !currentTex->GetFilePath().empty()) {
            currentPreview = currentTex->GetFilePath();
        } else if (currentPreview == "(none)") {
            currentPreview = "(unnamed texture)";
        }
    }

    bool changed = false;
    std::string comboLabel = std::string(label) + "##slot_" + std::to_string(slot);
    if (ImGui::BeginCombo(comboLabel.c_str(), currentPreview.c_str())) {
        // None 选项
        if (ImGui::Selectable("(none)", currentTex == nullptr)) {
            material->SetTexture(slot, nullptr);
            changed = true;
        }
        if (currentTex == nullptr) {
            ImGui::SetItemDefaultFocus();
        }

        // 列出所有已加载纹理
        for (const auto &key : allKeys) {
            Texture *tex = texMgr.Get(key);
            bool isSelected = (tex == currentTex);
            if (ImGui::Selectable(key.c_str(), isSelected)) {
                material->SetTexture(slot, tex);
                changed = true;
            }
            if (isSelected) {
                ImGui::SetItemDefaultFocus();
            }
        }

        ImGui::EndCombo();
    }

    return changed;
}

void SceneHierarchyPanel::DrawMaterialComponent(MaterialComponent &component) {
    // ---- 材质选择下拉框 ----
    auto &matMgr = Renderer::GetMaterialManager();
    auto allMats = matMgr.GetAllNames();

    std::string currentMatName = "(null)";
    if (component.MaterialPtr) {
        // 尝试在管理器中找到该材质的名称
        for (const auto &name : allMats) {
            if (matMgr.Get(name) == component.MaterialPtr) {
                currentMatName = name;
                break;
            }
        }
        // 未在管理器中找到则使用 DebugName
        if (currentMatName == "(null)") {
            std::string debugName = component.MaterialPtr->GetDebugName();
            if (!debugName.empty()) {
                currentMatName = debugName + " (unmanaged)";
            } else {
                currentMatName = "(unnamed, unmanaged)";
            }
        }
    }

    if (ImGui::BeginCombo("Material", currentMatName.c_str())) {
        // None 选项
        if (ImGui::Selectable("(null)", component.MaterialPtr == nullptr)) {
            component.MaterialPtr = nullptr;
        }
        if (component.MaterialPtr == nullptr) {
            ImGui::SetItemDefaultFocus();
        }

        // 列出所有已加载材质
        for (const auto &name : allMats) {
            Material *mat = matMgr.Get(name);
            bool isSelected = (mat == component.MaterialPtr);
            if (ImGui::Selectable(name.c_str(), isSelected)) {
                component.MaterialPtr = mat;
            }
            if (isSelected) {
                ImGui::SetItemDefaultFocus();
            }
        }

        ImGui::EndCombo();
    }

    if (component.MaterialPtr) {
        // ---- 材质类型（可切换：Blinn-Phong / PBR） ----
        // 枚举值恰为 0/1，可直接作为下拉框索引。切换后由 Renderer3D 按类型
        // 路由到对应管线（BlinnPhong=0 / PBR=1）。
        const char *typeNames[] = {"Blinn-Phong", "PBR"};
        int typeIdx = static_cast<int>(component.MaterialPtr->GetType());
        if (ImGui::Combo("Type", &typeIdx, typeNames, 2)) {
            component.MaterialPtr->SetType(
                typeIdx == 1 ? Material::Type::PBR : Material::Type::BlinnPhong);
        }

        ImGui::Separator();

        // ---- 纹理槽位选择器 ----
        DrawTextureSlot("Albedo",   component.MaterialPtr, Material::Albedo);
        DrawTextureSlot("Normal",   component.MaterialPtr, Material::Normal);
        DrawTextureSlot("Emissive", component.MaterialPtr, Material::Emissive);
        // PBR 专属：金属-粗糙度贴图（glTF 惯例：B=metallic, G=roughness）。
        // 留空则走标量 metallic/roughness 回退。
        if (component.MaterialPtr->GetType() == Material::Type::PBR) {
            DrawTextureSlot("Metallic Roughness", component.MaterialPtr,
                            Material::MetallicRoughness);
        }

        ImGui::Separator();

        // ---- 标量参数（按材质类型分流） ----
        if (component.MaterialPtr->GetType() == Material::Type::PBR) {
            // 金属度（0=绝缘体 1=金属，写入 "metallic"，PBR 管线的 scalar 系数）
            float metallic = component.MaterialPtr->GetFloat("metallic", 0.0f);
            if (ImGui::SliderFloat("Metallic (金属度)", &metallic, 0.0f, 1.0f)) {
                component.MaterialPtr->SetFloat("metallic", metallic);
            }

            // 粗糙度（0=镜面 1=漫，写入 "roughness"）
            float roughness = component.MaterialPtr->GetFloat("roughness", 0.5f);
            if (ImGui::SliderFloat("Roughness (粗糙度)", &roughness, 0.0f, 1.0f)) {
                component.MaterialPtr->SetFloat("roughness", roughness);
            }
        } else {
            // Blinn-Phong：高光指数（写入材质 "shininess" 参数，Renderer3D 每帧读取）
            // 用对数刻度：滑块位置 0~8 对应 shininess = 2^位置（1~256），
            // 避免线性滑块在高指数区间因 pow() 高光塌缩到亚像素而"看似没反应"。
            float shininess = component.MaterialPtr->GetFloat("shininess", 32.0f);
            float logShininess = std::log2(std::max(shininess, 1.0f));
            if (ImGui::SliderFloat("Shininess (高光指数, 对数刻度)", &logShininess,
                                   0.0f, 8.0f)) {
                component.MaterialPtr->SetFloat("shininess", std::pow(2.0f, logShininess));
            }

            // 镜面强度系数（写入材质 "specularStrength" 参数，独立控制高光亮暗，
            // 与 shininess 的高光形态解耦）
            float specularStrength = component.MaterialPtr->GetFloat("specularStrength", 0.5f);
            if (ImGui::SliderFloat("Specular Strength (镜面强度)", &specularStrength,
                                   0.0f, 2.0f)) {
                component.MaterialPtr->SetFloat("specularStrength", specularStrength);
            }
        }

        // 自发光强度（两种类型共用；写入材质 "emissiveStrength" 参数，
        // 缩放 Emissive 槽位纹理颜色；未设置时默认 0，不发光，故必须通过
        // 这里调高才能看到自发光效果）
        float emissiveStrength = component.MaterialPtr->GetFloat("emissiveStrength", 0.0f);
        if (ImGui::SliderFloat("Emissive Strength (自发光强度)", &emissiveStrength,
                               0.0f, 5.0f)) {
            component.MaterialPtr->SetFloat("emissiveStrength", emissiveStrength);
        }

        ImGui::Separator();

        // ---- 渲染状态 ----
        ImGui::Checkbox("Alpha Test", &component.MaterialPtr->alphaTest);
        ImGui::Checkbox("Double Sided", &component.MaterialPtr->doubleSided);
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
