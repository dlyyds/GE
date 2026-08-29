//
// 场景层级面板实现 —— 展示场景实体树与组件属性编辑器
//

#include "SceneHierarchyPanel.h"

#include <imgui.h>
#include <imgui_internal.h>
#include <backends/imgui_impl_vulkan.h>

#include "GE/Scene/Components.h"
#include "GE/Scene/Scene.h"
#include "GE/Physics/PhysicsWorld.h"
#include "GE/Render/Camera.h"
#include "GE/Render/Material.h"
#include "GE/Render/MaterialManager.h"
#include "GE/Render/TextureManager.h"
#include "GE/Render/AssetManager.h"
#include "GE/Render/Renderer.h"
#include "GE/Render/Mesh.h"
#include "GE/Render/MeshManager.h"
#include "GE/Scene/GLTFSceneImporter.h"
#include "GE/Utils/PlatformUtils.h"

#include <glm/gtc/type_ptr.hpp>
#include <glm/gtc/matrix_inverse.hpp>
#include <cmath>
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <vector>
#include <functional>
#include <unordered_set>

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

    // 遍历所有根实体（parent 为空的实体），逐层递归绘制子树。
    // 注意一：view<TransformComponent> 的 each() 会对单参 lambda 传组件而非实体句柄，
    //         故用显式范围 for 取实体句柄（解引用即 entt::entity）。
    // 注意二：树内右键「删除实体」会级联 DestroyEntity 销毁整个子树，直接改写 registry
    //         的 packed 数组；若一边画一边对 view 做范围 for，销毁当前帧的实体后迭代器
    //         随即指向越界下标（debug 下在 vector::operator[] 断言，此前删除首个带子
    //         实体的根必现崩溃）。故先快照全部根句柄，画树期间遍历独立副本。
    std::vector<entt::entity> rootSnapshot;
    {
        auto view = m_Context->Reg().view<TransformComponent>();
        for (auto entityID : view) {
            const auto &tc = view.get<TransformComponent>(entityID);
            // 有父者由父的递归绘制；父已失效（异常状态）按根处理，避免实体从树上消失
            if (tc.parent != entt::null && m_Context->Reg().valid(tc.parent))
                continue;
            rootSnapshot.push_back(entityID);
        }
    }
    for (const entt::entity entityID : rootSnapshot) {
        // 防御性兜底：同帧内先前节点删除若波及本根（正常级联不会），跳过已失效句柄
        if (!m_Context->Reg().valid(entityID))
            continue;
        DrawEntityNode(Entity{entityID, m_Context});
    }

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

    auto children = m_Context->GetChildren(entity);
    const bool hasChildren = !children.empty();

    ImGuiTreeNodeFlags flags = ((m_SelectionContext == entity) ? ImGuiTreeNodeFlags_Selected : 0)
                               | ImGuiTreeNodeFlags_OpenOnArrow
                               | ImGuiTreeNodeFlags_SpanAllColumns;
    // 叶子节点（无子实体）使用 Bullet 样式，避免展开箭头
    flags |= (hasChildren ? 0 : ImGuiTreeNodeFlags_Leaf);

    const bool opened = ImGui::TreeNodeEx((void *)(uint64_t)(uint32_t)entity, flags, "%s", tag.c_str());
    if (ImGui::IsItemClicked()) {
        m_SelectionContext = entity;
    }

    // ---- 右键菜单：删除实体（级联删子树） / 脱离父级 ----
    bool entityDeleted = false;
    bool detachRequested = false;
    if (ImGui::BeginPopupContextItem()) {
        if (ImGui::MenuItem("Delete Entity"))
            entityDeleted = true;
        if (ImGui::MenuItem("Detach from Parent"))
            detachRequested = true;
        ImGui::EndPopup();
    }

    // ---- 拖拽源：本实体可被拖到另一个节点上 → 重设父级 ----
    if (ImGui::BeginDragDropSource(ImGuiDragDropFlags_SourceNoHoldToOpenOthers)) {
        ImGui::SetDragDropPayload("ENTITY_TREE", &entity, sizeof(Entity));
        ImGui::Text("%s", tag.c_str());
        ImGui::EndDragDropSource();
    }

    // ---- 拖放目标：接受被拖来的实体作为自己的子实体（Scene::SetParent 内部做环检测） ----
    if (ImGui::BeginDragDropTarget()) {
        if (const ImGuiPayload *payload = ImGui::AcceptDragDropPayload("ENTITY_TREE")) {
            Entity dragged = *static_cast<const Entity *>(payload->Data);
            if (dragged != entity && !m_Context->SetParent(dragged, entity)) {
                GE_CORE_WARN("SceneHierarchyPanel: 无法把 {0} 设为 {1} 的子实体（可能构成环引用）",
                             dragged.GetComponent<TagComponent>().Tag, tag);
            }
        }
        ImGui::EndDragDropTarget();
    }

    if (opened) {
        // 递归绘制子树（子实体顺序 = 反向索引的插入序）
        for (auto &child : children) {
            DrawEntityNode(child);
        }
        ImGui::TreePop();
    }

    if (entityDeleted) {
        // 删除父实体 → 级联删除整个子树（Scene::DestroyEntity 负责递归）
        m_Context->DestroyEntity(entity);
        if (m_SelectionContext == entity)
            m_SelectionContext = {};
    } else if (detachRequested) {
        // 空父 = 脱离为根节点
        m_Context->SetParent(entity, {});
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
// Joint / Skin 组件（骨骼蒙皮阶段 B 新增）
// ============================================================
void SceneHierarchyPanel::DrawJointComponent(JointComponent &component) {
    // 关节序号由导入器按 skin.joints 顺序写入，手改无意义，只读展示
    ImGui::Text("关节索引: %d", component.jointIndex);
}

void SceneHierarchyPanel::DrawSkinComponent(SkinComponent &component) {
    ImGui::Text("关联网格: %s", component.MeshPtr ? "有" : "无");

    // 未接入皮肤定义（手动 Add Component / 导入期皮肤被跳过）：展示占位
    if (!component.skin) {
        ImGui::TextWrapped("未关联皮肤定义（导入期由 GLTFSceneImporter 填充）");
        return;
    }

    const auto &joints = component.joints();
    const auto &ibm = component.inverseBindMatrices();
    ImGui::Text("关节数量: %d", static_cast<int>(joints.size()));
    ImGui::Text("IBM 数量: %d", static_cast<int>(ibm.size()));

    // 关节实体列表（可点击选中对应实体，便于定位骨骼/后续驱动某根骨头）。
    // 同一条 glTF skin 被多 node 引用时共享同一关节表（SkinDef），各面板展示一致。
    if (!joints.empty()) {
        const std::string header = "关节实体列表(" + std::to_string(joints.size()) + ")";
        if (ImGui::TreeNode(header.c_str())) {
            auto &reg = m_Context->Reg();
            for (size_t i = 0; i < joints.size(); ++i) {
                std::string label = "[" + std::to_string(i) + "] ";
                const entt::entity handle = joints[i];
                Entity jointEntity{handle, m_Context};
                if (reg.valid(handle)) {
                    if (const auto *tag = reg.try_get<TagComponent>(handle)) {
                        label += tag->Tag;
                    } else {
                        label += "(无 Tag)";
                    }
                } else {
                    label += "(无效句柄)";
                }
                const bool selected = (m_SelectionContext == jointEntity);
                if (ImGui::Selectable(label.c_str(), selected)) {
                    m_SelectionContext = jointEntity;
                }
            }
            ImGui::TreePop();
        }
    }

    ImGui::Checkbox("需重传关节矩阵", &component.RequiresJointUpload);
}

// ============================================================
// Animation 组件（骨骼动画阶段 B/C 新增）
// ============================================================
void SceneHierarchyPanel::DrawAnimationComponent(AnimationComponent &component) {
    const AnimationClip *clip = component.activeClip();

    // 无动画片段（手动 Add Component / 空组件）：占位说明
    if (component.clips.empty() || !clip) {
        ImGui::TextWrapped("无动画片段（导入带骨骼动画的 glTF 角色会自动挂载）");
        return;
    }

    ImGui::Text("片段源: %s", clip->source.c_str());

    // 多 clip 下拉切换（单片段时隐藏，Active 指向唯一 clip）
    if (component.clips.size() > 1) {
        const std::string preview = component.clips[component.active].clip
            ? component.clips[component.active].clip->name
            : ("Clip " + std::to_string(component.active));
        if (ImGui::BeginCombo("播放片段", preview.c_str())) {
            for (size_t i = 0; i < component.clips.size(); ++i) {
                const bool selected = (i == component.active);
                const std::string label = component.clips[i].clip
                    ? component.clips[i].clip->name
                    : ("Clip " + std::to_string(i));
                if (ImGui::Selectable(label.c_str(), selected)) {
                    component.active = i;
                    component.time = 0.0f; // 切换后从头播放
                }
                if (selected) {
                    ImGui::SetItemDefaultFocus();
                }
            }
            ImGui::EndCombo();
        }
    }

    // 播放控制行：播放/暂停按钮 + 循环开关
    if (ImGui::Button(component.playing ? "暂停" : "播放")) {
        component.playing = !component.playing;
    }
    ImGui::SameLine();
    ImGui::Checkbox("循环", &component.loop);

    // 速度滑条（0~3x，允许负速倒放需要输入框，此处滑条取正向）
    float speed = component.speed;
    if (ImGui::SliderFloat("速度", &speed, 0.0f, 3.0f, "%.2fx")) {
        component.speed = speed;
    }

    // 时间轴 Scrubber：手动拖动即设播放时间（便于逐帧/定点验证姿态），
    // 播放状态不受拖动影响，后续帧从该时间继续推进。
    const float duration = (clip->duration > 0.0f) ? clip->duration : 1.0f;
    float time = component.time;
    if (ImGui::SliderFloat("时间", &time, 0.0f, duration, "%.3fs")) {
        component.time = time;
    }
    ImGui::Text("时长: %.3fs, channel 数: %d",
                clip->duration, static_cast<int>(clip->channels.size()));
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

    DrawComponent<MeshRendererComponent>("Mesh Renderer", entity,
        [this](auto &c) { DrawMeshRendererComponent(c, m_Context); });

    DrawComponent<JointComponent>("Joint", entity,
        [](auto &c) { DrawJointComponent(c); });

    DrawComponent<SkinComponent>("Skin", entity,
        [this](auto &c) { DrawSkinComponent(c); });

    DrawComponent<AnimationComponent>("Animation", entity,
        [](auto &c) { DrawAnimationComponent(c); });

    DrawComponent<SpriteRendererComponent>("Sprite Renderer", entity,
        [](auto &c) { DrawSpriteRendererComponent(c); });

    DrawComponent<PointLightComponent>("Point Light", entity,
        [](auto &c) { DrawPointLightComponent(c); });

    DrawComponent<DirectionalLightComponent>("Directional Light", entity,
        [](auto &c) { DrawDirectionalLightComponent(c); });

    DrawComponent<AmbientLightComponent>("Ambient Light", entity,
        [](auto &c) { DrawAmbientLightComponent(c); });

    DrawComponent<EnvironmentComponent>("Environment", entity,
        [this](auto &c) { DrawEnvironmentComponent(c); });

    DrawComponent<RigidBodyComponent>("Rigid Body", entity,
        [&](auto &c) { DrawRigidBodyComponent(entity, c); });

    DrawComponent<BoxColliderComponent>("Box Collider", entity,
        [&](auto &c) { DrawBoxColliderComponent(entity, c); });

    DrawComponent<SphereColliderComponent>("Sphere Collider", entity,
        [&](auto &c) { DrawSphereColliderComponent(entity, c); });

    DrawComponent<BoundingBoxComponent>("Bounding Box", entity,
        [this, entity](auto &c) { DrawBoundingBoxComponent(entity, c, m_Context); });

    // ---- Script 组件（无模板外的特殊条件，这里仅保留特殊标记） ----
    if (entity.HasComponent<ScriptComponent>()) {
        DrawComponent<ScriptComponent>("Script", entity,
            [&](auto &c) { DrawScriptComponent(c, entity); });
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
    TryAddComponent<MeshRendererComponent>("Mesh Renderer");
    TryAddComponent<JointComponent>("Joint");
    TryAddComponent<SkinComponent>("Skin");
    TryAddComponent<AnimationComponent>("Animation");
    TryAddComponent<SpriteRendererComponent>("Sprite Renderer");
    TryAddComponent<PointLightComponent>("Point Light");
    TryAddComponent<DirectionalLightComponent>("Directional Light");
    TryAddComponent<AmbientLightComponent>("Ambient Light");
    TryAddComponent<EnvironmentComponent>("Environment");
    TryAddComponent<RigidBodyComponent>("Rigid Body");
    TryAddComponent<BoxColliderComponent>("Box Collider");
    TryAddComponent<SphereColliderComponent>("Sphere Collider");
    TryAddComponent<BoundingBoxComponent>("Bounding Box");
    TryAddComponent<ScriptComponent>("Script");

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

    // 旋转用角度显示，内部存四元数；仅在编辑器边界转成欧拉角
    glm::vec3 rotationDeg = glm::degrees(component.GetRotationEuler());
    DrawVec3Control("Rotation", rotationDeg, 0.0f, 120);
    component.SetRotationEuler(glm::radians(rotationDeg));

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

/// 绘制单个纹理槽位的下拉选择器（从 TextureManager 选纹理绑定到材质槽位）。
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
        for (const auto &key : allKeys) {
            if (texMgr.Get(key) == currentTex) {
                currentPreview = key;
                break;
            }
        }
        if (currentPreview == "(none)" && !currentTex->GetFilePath().empty()) {
            currentPreview = currentTex->GetFilePath();
        } else if (currentPreview == "(none)") {
            currentPreview = "(unnamed texture)";
        }
    }

    bool changed = false;
    std::string comboLabel = std::string(label) + "##slot_" + std::to_string(slot);
    if (ImGui::BeginCombo(comboLabel.c_str(), currentPreview.c_str())) {
        // None 选项。独立压栈避免与某个 key 恰好为 "(none)" 的纹理撞 ID。
        ImGui::PushID("none");
        if (ImGui::Selectable("(none)", currentTex == nullptr)) {
            material->SetTexture(slot, nullptr);
            changed = true;
        }
        ImGui::PopID();
        if (currentTex == nullptr) {
            ImGui::SetItemDefaultFocus();
        }

        // 列出所有已加载纹理；按 key（唯一）压栈隔离，杜绝同名项 ID 冲突
        for (const auto &key : allKeys) {
            Texture *tex = texMgr.Get(key);
            bool isSelected = (tex == currentTex);
            ImGui::PushID(key.c_str());
            if (ImGui::Selectable(key.c_str(), isSelected)) {
                material->SetTexture(slot, tex);
                changed = true;
            }
            ImGui::PopID();
            if (isSelected) {
                ImGui::SetItemDefaultFocus();
            }
        }

        ImGui::EndCombo();
    }

    return changed;
}

/// 绘制材质内容编辑器：类型 / 纹理槽位 / 标量参数 / 渲染状态。
///
/// 直接编辑 MaterialManager 持有的 Material 对象；同一材质被多个子网格共享时
/// 改动对所有引用方即时生效。
static void DrawMaterialEditor(Material *material) {
    if (!material) {
        return;
    }

    // 材质显示名（独立字段，不改变 manager 注册 key）
    char nameBuf[128];
    snprintf(nameBuf, sizeof(nameBuf), "%s", material->GetName().c_str());
    if (ImGui::InputText("名称 (Name)", nameBuf, sizeof(nameBuf))) {
        material->SetName(nameBuf);
    }

    // 材质类型（Blinn-Phong / PBR），决定渲染管线
    const char *typeNames[] = {"Blinn-Phong", "PBR"};
    int typeIdx = static_cast<int>(material->GetType());
    if (ImGui::Combo("Type", &typeIdx, typeNames, 2)) {
        material->SetType(typeIdx == 1 ? Material::Type::PBR : Material::Type::BlinnPhong);
    }

    ImGui::Separator();

    // 纹理槽位
    DrawTextureSlot("Albedo",   material, Material::Albedo);
    DrawTextureSlot("Normal",   material, Material::Normal);
    DrawTextureSlot("Emissive", material, Material::Emissive);
    // PBR 专属：金属-粗糙度贴图（glTF 惯例：B=metallic, G=roughness）
    if (material->GetType() == Material::Type::PBR) {
        bool mrChanged = DrawTextureSlot("Metallic Roughness", material, Material::MetallicRoughness);
        // 绑定贴图时让贴图如实驱动金属度/粗糙度：标量系数自动归 1
        // （否则贴图 B/G 通道会被默认的 metallic=0、roughness=0.5 乘掉）。
        // 仅在本帧发生了"绑定"（仍是贴图）时触发，取消绑定（回到 none）不干预。
        if (mrChanged && material->GetTexture(Material::MetallicRoughness)) {
            material->SetFloat("metallic", 1.0f);
            material->SetFloat("roughness", 1.0f);
        }
    }

    ImGui::Separator();

    // 标量参数（按材质类型分流）
    if (material->GetType() == Material::Type::PBR) {
        float metallic = material->GetFloat("metallic", 0.0f);
        if (ImGui::SliderFloat("Metallic (金属度)", &metallic, 0.0f, 1.0f)) {
            material->SetFloat("metallic", metallic);
        }
        float roughness = material->GetFloat("roughness", 0.5f);
        if (ImGui::SliderFloat("Roughness (粗糙度)", &roughness, 0.0f, 1.0f)) {
            material->SetFloat("roughness", roughness);
        }
    } else {
        // Blinn-Phong：高光指数（对数刻度 0~8 → shininess = 2^位置）
        float shininess = material->GetFloat("shininess", 32.0f);
        float logShininess = std::log2(std::max(shininess, 1.0f));
        if (ImGui::SliderFloat("Shininess (高光指数, 对数刻度)", &logShininess,
                               0.0f, 8.0f)) {
            material->SetFloat("shininess", std::pow(2.0f, logShininess));
        }
        float specularStrength = material->GetFloat("specularStrength", 0.5f);
        if (ImGui::SliderFloat("Specular Strength (镜面强度)", &specularStrength,
                               0.0f, 2.0f)) {
            material->SetFloat("specularStrength", specularStrength);
        }
    }

    // 自发光颜色因子（两种类型共用，glTF emissiveFactor）
    // 最终自发光颜色 = 自发光贴图颜色 × 该因子；[0,0,0] 表示不发光
    glm::vec3 emissiveFactor = material->GetEmissiveFactor();
    if (ImGui::ColorEdit3("Emissive Factor (自发光颜色)", glm::value_ptr(emissiveFactor))) {
        material->SetEmissiveFactor(emissiveFactor);
    }

    // 纹理平铺 / UV 缩放密度（两种类型共用，采样前乘 inUV）
    float uvTiling = material->GetFloat("uvTiling", 1.0f);
    if (ImGui::SliderFloat("UV Tiling (纹理平铺)", &uvTiling,
                           0.1f, 10.0f)) {
        material->SetFloat("uvTiling", uvTiling);
    }

    ImGui::Separator();

    // 渲染状态
    ImGui::Checkbox("Alpha Test", &material->alphaTest);
    ImGui::Checkbox("Double Sided", &material->doubleSided);
}

/// 绘制子网格材质选择器：选材质即写入 MeshRendererComponent 的覆写表。
///
/// 当前生效材质 = 覆写（materialOverrides）优先，否则子网格默认材质。
/// 选择某材质 → 生成覆写（每实体独立）；选 "(use default)" → 清除覆写回退默认。
static void DrawSubMeshMaterialEditor(MeshRendererComponent &comp, size_t index, const SubMesh &sub) {
    auto &matMgr = Renderer::GetMaterialManager();
    auto allMats = matMgr.GetAllNames();

    // 当前覆写与生效材质
    Material *override = nullptr;
    auto it = comp.materialOverrides.find(static_cast<uint32_t>(index));
    if (it != comp.materialOverrides.end()) {
        override = it->second;
    }
    Material *effective = override ? override : sub.defaultMaterial;

    // 显示名：用材质的显示名（GetName），不显示 manager 注册 key
    std::string currentName = "(use default)";
    if (effective) {
        currentName = effective->GetName();
        if (currentName.empty()) {
            currentName = "(unnamed)";
        }
        if (!override) {
            currentName += " [default]";
        }
    }

    std::string label = "Material##sub_" + std::to_string(index);
    if (ImGui::BeginCombo(label.c_str(), currentName.c_str())) {
        // 使用默认（清除覆写）。独立压栈避免与某个恰好叫 "(use default)"
        // 的材质显示名撞 ID。
        ImGui::PushID("use_default");
        if (ImGui::Selectable("(use default)", override == nullptr)) {
            comp.materialOverrides.erase(static_cast<uint32_t>(index));
        }
        ImGui::PopID();
        if (override == nullptr) {
            ImGui::SetItemDefaultFocus();
        }

        // 列出 MaterialManager 中所有已加载材质（显示名 GetName，选即生成覆写）。
        // 同一显示名会对应多个材质对象（如内置几何 / 无 MTL 网格都叫 "default"）：
        // 若直接以显示名作标签，弹窗内会出现多条同名 Selectable → ID 冲突。
        // 故每条按注册 key（唯一）PushID 隔离，显示名重复也不撞 ID。
        for (const auto &name : allMats) {
            Material *mat = matMgr.Get(name);
            bool isSelected = (mat == effective);
            std::string displayName = mat ? mat->GetName() : name;
            if (displayName.empty()) {
                displayName = name;  // 兜底：无显示名时退回 key
            }
            ImGui::PushID(name.c_str());
            if (ImGui::Selectable(displayName.c_str(), isSelected)) {
                comp.materialOverrides[static_cast<uint32_t>(index)] = mat;
            }
            ImGui::PopID();
            if (isSelected) {
                ImGui::SetItemDefaultFocus();
            }
        }

        ImGui::EndCombo();
    }

    // 有覆写 → 编辑覆写材质（每实体独立）；无覆写 → 只读提示
    if (override) {
        ImGui::Indent();
        DrawMaterialEditor(override);
        ImGui::Unindent();
    } else if (sub.defaultMaterial) {
        ImGui::TextDisabled("using default (read-only) — 选一个材质以覆写");
    }
}

void SceneHierarchyPanel::DrawMeshRendererComponent(MeshRendererComponent &component,
                                                    Scene *scene) {
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

    // ---- 加载模型文件（.obj / .gemesh / .gltf / .glb）----
    if (ImGui::Button("加载模型文件 (OBJ / GEMESH / GLTF)...")) {
        std::string path = FileDialogs::OpenFile(
            "模型文件 (*.obj;*.gemesh;*.gltf;*.glb)\0*.obj;*.gemesh;*.gltf;*.glb\0"
            "Wavefront OBJ (*.obj)\0*.obj\0"
            ".gemesh 内置格式 (*.gemesh)\0*.gemesh\0"
            "glTF (*.gltf;*.glb)\0*.gltf;*.glb\0"
            "All Files (*.*)\0*.*\0");
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
        ImGui::Text("网格加载失败（请确认是合法的 .obj / .gemesh / .gltf / .glb 文件）");
        if (ImGui::Button("OK")) {
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }

    // ---- 导入 glTF 场景（保留 node 层级与变换，挂到当前选中实体下）----
    ImGui::Separator();
    if (ImGui::Button("导入 glTF 场景...")) {
        std::string path = FileDialogs::OpenFile(
            "glTF 场景 (*.gltf;*.glb)\0*.gltf;*.glb\0"
            "All Files (*.*)\0*.*\0");
        if (!path.empty()) {
            if (scene && !GLTFSceneImporter::Import(*scene, meshMgr, path)) {
                GE_CORE_WARN("SceneHierarchyPanel: glTF 场景导入失败: {0}", path);
                ImGui::OpenPopup("GLTFImportFailed");
            }
        }
    }
    if (ImGui::BeginPopup("GLTFImportFailed")) {
        ImGui::Text("glTF 场景导入失败（请确认是合法的 .gltf / .glb 文件）");
        if (ImGui::Button("OK")) {
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }

    // 网格信息 + 子网格材质编辑
    if (component.MeshPtr) {
        ImGui::Separator();
        ImGui::Text("Path: %s", component.MeshPtr->GetFilePath().c_str());

        // 异步加载中尚未就绪：不显示 0 统计 / 空子网格（避免误导），就绪后自动显示
        if (!component.MeshPtr->IsReady()) {
            ImGui::TextDisabled("加载中（异步）...");
        } else {
            ImGui::Text("Vertices: %u", component.MeshPtr->GetVertexCount());
            ImGui::Text("Indices:  %u", component.MeshPtr->GetIndexCount());

            // ---- 子网格列表：每个子网格一个可折叠下拉框，展开后绑定/编辑材质 ----
            const auto &subMeshes = component.MeshPtr->GetSubMeshes();
            ImGui::Separator();
            ImGui::Text("SubMeshes: %zu", subMeshes.size());
            for (size_t i = 0; i < subMeshes.size(); ++i) {
                // 折叠标题：显示子网格索引 + 索引数量
                std::string header = "SubMesh " + std::to_string(i) +
                                     " (" + std::to_string(subMeshes[i].indexCount) + " indices)";
                if (ImGui::CollapsingHeader(header.c_str())) {
                    // CollapsingHeader 带 NoTreePushOnOpen，内容不会推入 ID 栈：若不在
                    // 这里按子网格索引 PushID，各子网格材质编辑器内同名的 Combo/Slider/
                    // InputText（如 Albedo##slot_0）在 Properties 窗口共享同一 ID 作用域，
                    // 会产生大量「ID 已被占用」的告警。故此处显式按索引开作用域。
                    ImGui::Indent();
                    ImGui::PushID(static_cast<int>(i));
                    DrawSubMeshMaterialEditor(component, i, subMeshes[i]);
                    ImGui::PopID();
                    ImGui::Unindent();
                }
            }
        }
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
// Environment 组件
// ============================================================
void SceneHierarchyPanel::DrawEnvironmentComponent(EnvironmentComponent &component) {
    // 扫描 assets/environments/ 下的子文件夹，作为可选环境列表
    std::vector<std::string> envNames;
    const auto envRoot = Renderer::GetAssetManager().GetAssetRoot() / "environments";
    std::error_code ec;
    if (std::filesystem::is_directory(envRoot, ec)) {
        for (const auto &entry : std::filesystem::directory_iterator(envRoot, ec)) {
            if (entry.is_directory(ec)) {
                envNames.push_back(entry.path().filename().string());
            }
        }
    }
    std::sort(envNames.begin(), envNames.end());

    // 缩略图尺寸 = 行高 × 行高（1:1，且正好贴合每行高度，不超出）
    const float thumbSize = GImGui->FontSize + GImGui->Style.FramePadding.y * 2.0f;
    const ImVec2 thumbSizeVec(thumbSize, thumbSize);

    // 当前选中环境的预览缩略图（显示在下拉框左侧）
    if (!component.Name.empty()) {
        if (ImTextureID tid = GetEnvironmentThumbnail(component.Name)) {
            ImGui::Image(tid, thumbSizeVec);
            ImGui::SameLine();
        }
    }

    // 环境名下拉框：从扫到的子文件夹中选择，选即切换环境
    std::string currentPreview = component.Name.empty() ? "(none)" : component.Name;
    if (ImGui::BeginCombo("Name", currentPreview.c_str())) {
        // None 选项（环境名为空）
        if (ImGui::Selectable("(none)", component.Name.empty())) {
            component.Name.clear();
        }
        if (component.Name.empty()) {
            ImGui::SetItemDefaultFocus();
        }

        // 列出 environments/ 下所有子文件夹，每项右侧带预览缩略图
        for (const auto &name : envNames) {
            bool isSelected = (component.Name == name);
            bool itemSelected = ImGui::Selectable(name.c_str(), isSelected);
            // 预览图放在名称右侧同一行
            if (ImTextureID tid = GetEnvironmentThumbnail(name)) {
                ImGui::SameLine();
                ImGui::Image(tid, thumbSizeVec);
            }
            if (itemSelected) {
                component.Name = name;
            }
            if (isSelected) {
                ImGui::SetItemDefaultFocus();
            }
        }

        ImGui::EndCombo();
    }

    // 环境总开关（关则天空盒 + IBL 一并关闭）
    ImGui::Checkbox("Enabled", &component.Enabled);
    // 天空盒背景开关
    ImGui::Checkbox("Skybox", &component.SkyboxEnabled);
    // IBL 环境光开关
    ImGui::Checkbox("IBL", &component.IBLEnabled);
    ImGui::TextDisabled("环境（天空盒 + IBL）来自 environments/<Name>/，不依赖 Transform");
}

// ============================================================
// Environment 预览图缩略图
// ============================================================
ImTextureID SceneHierarchyPanel::GetEnvironmentThumbnail(const std::string &envName) {
    // 已缓存则直接返回
    auto it = m_EnvThumbnails.find(envName);
    if (it != m_EnvThumbnails.end()) {
        return it->second;
    }

    ImTextureID id = 0;
    // 加载 environments/<名称>/preview.png（不存在则返回 0，不显示缩略图）
    Texture *tex = Renderer::GetTextureManager().Load("assets/environments/" + envName + "/preview.png");
    if (tex) {
        // 用采样器 + ImageView 注册为 ImGui 图片（与 ResourcePanel::GetThumbnail 一致）
        VkDescriptorSet set = ImGui_ImplVulkan_AddTexture(
            tex->GetSampler().GetHandle(),
            tex->GetImageView().GetHandle(),
            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        id = reinterpret_cast<ImTextureID>(set);
    }
    m_EnvThumbnails[envName] = id;
    return id;
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

    // 碰撞体调试线框单独开关（仅影响视口叠加显示，不涉及物理）
    ImGui::Checkbox("Draw Debug", &component.DrawDebug);
    ImGui::Separator();

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

    // 碰撞体调试线框单独开关（仅影响视口叠加显示，不涉及物理）
    ImGui::Checkbox("Draw Debug", &component.DrawDebug);
    ImGui::Separator();

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
// Bounding Box 组件（实体级粗剔除盒，gizmo 手动摆放）
// ============================================================

// ---- 动画扫描评估助手：用于「播放一遍取关节极值」。以下采样逻辑与
// GE/src/Scene/Scene.cpp 的 SampleVec3Channel/SampleQuatChannel 保持同步
// （CUBICSPLINE 目前两边都按 LINEAR 近似；引擎若升级 Hermite，此处须同步）。----

/// 定位时间 t 所在的键帧区间左端 k0（划入 active key，同 Scene.cpp FindActiveKey 带缓存版）
static size_t AnimFindActiveKey(const std::vector<float> &times, float t, uint32_t &hint) {
    if (hint >= times.size()) {
        hint = static_cast<uint32_t>(std::upper_bound(times.begin(), times.end(), t)
                                     - times.begin()) - 1u;
        return hint;
    }
    size_t k = hint;
    while (k + 1 < times.size() && times[k + 1] <= t) {
        ++k; // 时间前进：向右扫
    }
    while (k > 0 && times[k] > t) {
        --k; // 时间后退：向左扫
    }
    hint = static_cast<uint32_t>(k);
    return k;
}

/// vec3 通道采样：LINEAR 线性插值；STEP 取前一键值；CUBICSPLINE 按 LINEAR 近似
static glm::vec3 SampleAnimVec3(const AnimationChannel &ch, float t, uint32_t &hint) {
    const size_t n = ch.times.size();
    if (n == 0) {
        return glm::vec3(0.0f);
    }
    if (t <= ch.times.front()) {
        hint = 0;
        return ch.vecKeys.front();
    }
    if (t >= ch.times.back()) {
        hint = static_cast<uint32_t>(n - 1u);
        return ch.vecKeys.back();
    }
    const size_t k0 = AnimFindActiveKey(ch.times, t, hint);
    if (ch.interp == AnimationChannel::Interp::Step) {
        return ch.vecKeys[k0]; // STEP：保持前一键值
    }
    const float t01 = (t - ch.times[k0]) / (ch.times[k0 + 1] - ch.times[k0]);
    return glm::mix(ch.vecKeys[k0], ch.vecKeys[k0 + 1], t01);
}

/// quat 通道采样：LINEAR 用 slerp；STEP 取前一键值；CUBICSPLINE 近似 slerp
static glm::quat SampleAnimQuat(const AnimationChannel &ch, float t, uint32_t &hint) {
    const size_t n = ch.times.size();
    if (n == 0) {
        return glm::quat(1.0f, 0.0f, 0.0f, 0.0f);
    }
    if (t <= ch.times.front()) {
        hint = 0;
        return ch.quatKeys.front();
    }
    if (t >= ch.times.back()) {
        hint = static_cast<uint32_t>(n - 1u);
        return ch.quatKeys.back();
    }
    const size_t k0 = AnimFindActiveKey(ch.times, t, hint);
    if (ch.interp == AnimationChannel::Interp::Step) {
        return ch.quatKeys[k0];
    }
    const float t01 = (t - ch.times[k0]) / (ch.times[k0 + 1] - ch.times[k0]);
    return glm::slerp(ch.quatKeys[k0], ch.quatKeys[k0 + 1], t01);
}

/// 动画评估用局部 TRS（只拷作者字段，不碰缓存 worldMatrix）
struct AnimLocalTrs {
    glm::vec3 Translation;
    glm::quat Rotation;
    glm::vec3 Scale;
};

static glm::mat4 AnimLocalMatrix(const AnimLocalTrs &trs) {
    return glm::translate(glm::mat4(1.0f), trs.Translation)
           * glm::toMat4(trs.Rotation)
           * glm::scale(glm::mat4(1.0f), trs.Scale);
}

void SceneHierarchyPanel::DrawBoundingBoxComponent(Entity entity, BoundingBoxComponent &component, Scene *scene) {
    // Center/Size 为模型局部空间；Size 任一轴重置为 0 会令盒失效（不参与剔除）
    DrawVec3Control("Center", component.Center, 0.0f, 120);
    DrawVec3Control("Size", component.Size, 1.0f, 120);
    if (!component.IsValid()) {
        ImGui::TextDisabled("盒未摆放（Size 需全部 > 0）时不参与剔除");
    }
    if (ImGui::Button("从关节自动适配")) {
        if (!AutoFitBoundingBoxToJoints(scene, entity, component)) {
            GE_CORE_WARN("该实体子树内没有蒙皮关节，无法自动适配包围盒");
        }
    }
    ImGui::SameLine();
    ImGui::TextDisabled("按子树内全部蒙皮关节计算并写回；检测到动画时逐帧播放取最大范围");
}

bool SceneHierarchyPanel::AutoFitBoundingBoxToJoints(Scene *scene, Entity entity, BoundingBoxComponent &bb) {
    if (!scene || !entity) {
        return false;
    }
    entt::registry &reg = scene->Reg();
    const auto *tc = reg.try_get<TransformComponent>(static_cast<entt::entity>(entity));
    if (!tc) {
        return false;
    }
    // 关节是世界实体，先经逆世界矩阵变换到本实体局部空间，再取本地 AABB。
    // 这样写回的 Center/Size 经世界矩阵还原（视锥剔除用的 world 盒）后必能罩住全部关节。
    const glm::mat4 invWorld = glm::inverse(tc->GetWorldMatrix());

    // ── 1) DFS 子树：收集蒙皮关节 + 动画组件 ──
    std::vector<entt::entity> joints;
    std::vector<AnimationComponent *> anims;
    std::vector<Entity> stack;
    stack.push_back(entity);
    while (!stack.empty()) {
        const Entity n = stack.back();
        stack.pop_back();
        const entt::entity h = static_cast<entt::entity>(n);
        if (const auto *sc = reg.try_get<SkinComponent>(h)) {
            joints.insert(joints.end(), sc->joints().begin(), sc->joints().end());
        }
        if (auto *ac = reg.try_get<AnimationComponent>(h)) {
            anims.push_back(ac); // 有动画则后续按帧扫关节极值
        }
        for (const auto &child : scene->GetChildren(n)) {
            stack.push_back(child); // 继续下钻同棵子树
        }
    }
    if (joints.empty()) {
        return false; // 子树内无蒙皮关节，无从适配
    }

    glm::vec3 localMin(0.0f), localMax(0.0f);
    bool found = false;
    // invW 逐帧变化：当前姿态用 invWorld，动画扫描用「该帧盒子自身世界矩阵的逆」，
    // 这样即使盒子随动画移动/旋转，度量尺度始终与盒子本地空间一致。
    const auto expandLocal = [&](const glm::mat4 &invW, const glm::vec3 &wpos) {
        const glm::vec3 jp = glm::vec3(invW * glm::vec4(wpos, 1.0f));
        if (!found) {
            localMin = localMax = jp;
            found = true;
        } else {
            localMin = glm::min(localMin, jp);
            localMax = glm::max(localMax, jp);
        }
    };

    // ── 2) 当前姿态打底（无动画时仅此一步） ──
    for (const entt::entity jh : joints) {
        const auto *jtc = reg.try_get<TransformComponent>(jh);
        if (jtc) {
            expandLocal(invWorld, glm::vec3(jtc->GetWorldMatrix()[3]));
        }
    }
    if (!found) {
        return false;
    }

    // ── 3) 动画扫描：把子树内的每个 clip 从头到尾播一遍，各帧关节极值并入最大盒 ──
    // 关节世界位置只受「关节到根祖先链」上实体局部 TRS 影响，先收集整条链
    std::unordered_set<entt::entity> chainSet;
    for (const entt::entity jh : joints) {
        entt::entity cur = jh;
        while (cur != entt::null) {
            chainSet.insert(cur);
            const auto *curTc = reg.try_get<TransformComponent>(cur);
            cur = curTc ? curTc->parent : entt::null;
        }
    }

    for (const AnimationComponent *ac : anims) {
        for (const ClipInstance &inst : ac->clips) {
            const auto &clip = inst.clip;
            if (!clip || clip->channels.empty()) {
                continue;
            }
            const auto &targets = inst.channelTargets;

            // 采样时间轴单调递增 → 每 channel 独立缓存键帧下界（阶段 A：扫帧整体受益最大）。
            // 每个 clip 从头独立扫，故用本地数组、不复用 ClipInstance 的运行时缓存。
            std::vector<uint32_t> keyHints(clip->channels.size(), 0u);

            // 采样时刻：均匀网格（限流，含首尾）+ 全部键帧时刻
            // （键帧时刻覆盖 STEP 跳变与旋转弧极值不在网格上的情形）
            std::vector<float> times;
            const float duration = clip->duration;
            if (duration > 0.0f) {
                const float step = std::max(1.0f / 30.0f, duration / 1200.0f);
                for (float t = 0.0f; t < duration; t += step) {
                    times.push_back(t);
                }
            }
            times.push_back(duration);
            for (const auto &ch : clip->channels) {
                times.insert(times.end(), ch.times.begin(), ch.times.end());
            }
            std::sort(times.begin(), times.end());
            times.erase(std::unique(times.begin(), times.end()), times.end());

            for (const float t : times) {
                // 局部 TRS：默认取场景当前值，被本 clip 指定 path 驱动的分量用采样覆盖
                // （同一目标只改写驱动分量，其余分量保持绑定姿态）
                std::unordered_map<entt::entity, AnimLocalTrs> localTrs;
                for (const entt::entity c : chainSet) {
                    const auto *cTc = reg.try_get<TransformComponent>(c);
                    if (cTc) {
                        localTrs[c] = AnimLocalTrs{cTc->Translation, cTc->Rotation, cTc->Scale};
                    }
                }
                for (size_t ci = 0; ci < clip->channels.size(); ++ci) {
                    const AnimationChannel &ch = clip->channels[ci];
                    const entt::entity target = (ci < targets.size()) ? targets[ci] : entt::null;
                    if (target == entt::null || !chainSet.count(target)) {
                        continue; // 空洞目标、或不在本骨架链上（不影响这些关节），跳过
                    }
                    auto &lt = localTrs[target];
                    switch (ch.path) {
                    case AnimationChannel::Path::Translation:
                        lt.Translation = SampleAnimVec3(ch, t, keyHints[ci]);
                        break;
                    case AnimationChannel::Path::Rotation:
                        lt.Rotation = SampleAnimQuat(ch, t, keyHints[ci]);
                        break;
                    case AnimationChannel::Path::Scale:
                        lt.Scale = SampleAnimVec3(ch, t, keyHints[ci]);
                        break;
                    }
                }

                // 自顶向下累乘世界矩阵（链已含根，父先于子），滚到每个关节取世界位置。
                // 度量尺度用该帧盒子自身的逆世界矩阵：盒子随动画位移/旋转时仍得本地空间极值。
                std::unordered_map<entt::entity, glm::mat4> worldMat;
                std::function<glm::mat4(entt::entity)> calcWorld = [&](entt::entity e) -> glm::mat4 {
                    auto it = worldMat.find(e);
                    if (it != worldMat.end()) {
                        return it->second;
                    }
                    const auto ltIt = localTrs.find(e);
                    glm::mat4 m = (ltIt != localTrs.end()) ? AnimLocalMatrix(ltIt->second) : glm::mat4(1.0f);
                    const auto *eTc = reg.try_get<TransformComponent>(e);
                    if (eTc && eTc->parent != entt::null) {
                        m = calcWorld(eTc->parent) * m;
                    }
                    worldMat[e] = m;
                    return m;
                };
                const entt::entity boxH = static_cast<entt::entity>(entity);
                const glm::mat4 invWorldT = glm::inverse(calcWorld(boxH));
                for (const entt::entity jh : joints) {
                    expandLocal(invWorldT, glm::vec3(calcWorld(jh)[3]));
                }
            }
        }
    }

    bb.Center = (localMin + localMax) * 0.5f;
    bb.Size = localMax - localMin;
    return true;
}

// ============================================================
// Script 组件
// ============================================================

// 把对话框返回的绝对路径转成 assets/scripts/ 相对路径（ScriptEngine 基准目录）
static std::string ScriptPathFromDialog(const std::string &absPath) {
    std::string p = absPath;
    for (auto &c : p)
        if (c == '\\')
            c = '/';
    const std::string marker = "assets/scripts/";
    const size_t pos = p.rfind(marker);
    if (pos != std::string::npos)
        return p.substr(pos + marker.size());
    // 兜底：路径不在 assets/scripts 下，退化为仅文件名
    const size_t slash = p.rfind('/');
    return (slash == std::string::npos) ? p : p.substr(slash + 1);
}

void SceneHierarchyPanel::DrawScriptComponent(ScriptComponent &component, Entity entity) {
    ImGui::Checkbox("Enabled", &component.Enabled);

    // 当前脚本路径（只读显示，选择走对话框）
    ImGui::Text("Script: %s", component.ScriptPath.empty() ? "(未选择)" : component.ScriptPath.c_str());

    if (ImGui::Button("浏览脚本...")) {
        std::string absPath = FileDialogs::OpenFile(
            "Lua Script (*.lua)\0*.lua\0All Files (*.*)\0*.*\0", "assets/scripts");
        if (!absPath.empty()) {
            component.ScriptPath = ScriptPathFromDialog(absPath);
            // 路径选定即重挂（旧实例卸载 + 新脚本载入）
            if (Scene *scene = entity.GetScene()) {
                scene->GetScriptEngine().OnComponentAdded(static_cast<entt::entity>(entity));
            }
        }
    }

    if (!component.ScriptPath.empty()) {
        ImGui::SameLine();
        if (ImGui::Button("清除脚本")) {
            component.ScriptPath.clear();
            if (Scene *scene = entity.GetScene())
                scene->GetScriptEngine().OnEntityDestroyed(static_cast<entt::entity>(entity));
        }
        ImGui::SameLine();
        if (ImGui::Button("重载")) {
            // 热重载：换逻辑不换状态（实例字段保留，见计划书 9.8）
            if (Scene *scene = entity.GetScene())
                scene->GetScriptEngine().Reload(component.ScriptPath);
        }
    }

    // 状态行：加载状态 + 最近错误（含限频自动禁用提示）
    if (Scene *scene = entity.GetScene()) {
        auto &eng = scene->GetScriptEngine();
        const bool mounted = eng.HasInstance(static_cast<entt::entity>(entity));
        const std::string lastErr = eng.GetLastError(static_cast<entt::entity>(entity));
        if (!mounted) {
            ImGui::TextColored(ImVec4(0.8f, 0.4f, 0.4f, 1.0f), "状态: 未加载");
        } else if (!component.Enabled && !lastErr.empty()) {
            ImGui::TextColored(ImVec4(0.85f, 0.45f, 0.28f, 1.0f), "状态: 已暂停（连续报错自动禁用）");
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("连续 OnUpdate 报错超过 3 次自动置 Enabled=false；再勾选即重新计数");
        } else if (!component.Enabled) {
            ImGui::TextColored(ImVec4(0.85f, 0.8f, 0.5f, 1.0f), "状态: 已暂停（手动禁用）");
        } else {
            ImGui::TextColored(ImVec4(0.35f, 0.75f, 0.35f, 1.0f), "状态: 已加载");
        }
        if (!lastErr.empty())
            ImGui::TextWrapped("最近错误: %s", lastErr.c_str());
    }

    // ---- public 字段（脚本 PUBLIC_FIELDS 声明 → 输入控件；值写回组件，保存时落盘）----
    if (Scene *scene = entity.GetScene()) {
        auto &eng = scene->GetScriptEngine();
        const auto schema = eng.GetPublicFieldSchema(static_cast<entt::entity>(entity));
        if (!schema.empty()) {
            ImGui::Separator();
            ImGui::TextDisabled("Public Fields");
            ImGui::Indent();
            for (const auto &meta : schema) {
                auto it = component.PublicFields.find(meta.Name);
                if (it == component.PublicFields.end() || it->second.Type != meta.Type) {
                    // 缺失（脚本新增字段）或类型已变 → 按 schema 重置默认
                    it = component.PublicFields.insert_or_assign(
                             meta.Name,
                             ScriptPublicField{meta.Type, meta.NumberDefault,
                                               meta.BoolDefault, meta.StringDefault})
                             .first;
                }
                switch (meta.Type) {
                case ScriptFieldType::Number:
                    ImGui::DragFloat(meta.Name.c_str(), &it->second.Number, 0.1f);
                    break;
                case ScriptFieldType::Bool:
                    ImGui::Checkbox(meta.Name.c_str(), &it->second.Bool);
                    break;
                case ScriptFieldType::String: {
                    char buf[256];
                    std::strncpy(buf, it->second.String.c_str(), sizeof(buf) - 1);
                    buf[sizeof(buf) - 1] = '\0';
                    if (ImGui::InputText(meta.Name.c_str(), buf, sizeof(buf)))
                        it->second.String = buf;
                    break;
                }
                default:
                    break;
                }
            }
            ImGui::Unindent();
        }
    }

    ImGui::TextDisabled("脚本位于 assets/scripts/（相对该目录，含 .lua 后缀）");
}

} // namespace GE
