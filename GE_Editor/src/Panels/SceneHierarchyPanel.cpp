//
// 场景层级面板实现 —— 核心编排：场景实体树 + 组件列表 + Add Component 弹窗。
//
// 各组件属性绘制函数按域拆分到：
//   SceneHierarchyPanel_Components.cpp（变换/相机/跟随相机/光照/水面/环境）
//   SceneHierarchyPanel_Mesh.cpp（网格/材质/精灵）
//   SceneHierarchyPanel_Animation.cpp（骨骼/动画/状态机）
//   SceneHierarchyPanel_Physics.cpp（刚体/角色控制器/碰撞体/包围盒）
//   SceneHierarchyPanel_AudioScript.cpp（音频/脚本）
// 跨文件共享辅助在 SceneHierarchyPanelInternal.h。
//

#include "SceneHierarchyPanel.h"

#include <imgui.h>
#include <imgui_internal.h>

#include "GE/Scene/Components.h"
#include "GE/Scene/Scene.h"

#include "SceneHierarchyPanelInternal.h"

#include <algorithm>
#include <cstring>
#include <functional>
#include <string>
#include <vector>

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
    ImGui::Begin("场景层级");

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
        if (ImGui::MenuItem("创建空实体"))
            m_Context->CreateEntity("空实体");
        ImGui::EndPopup();
    }

    ImGui::End();

    // 属性面板
    ImGui::SetNextWindowDockID(m_DockSpaceID, ImGuiCond_FirstUseEver);
    ImGui::Begin("属性");
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

    // 选中行：用蓝色调高亮整行，替代默认灰色，与 Properties 中的选中态呼应
    const bool isSelected = (m_SelectionContext == entity);
    if (isSelected) {
        ImGui::PushStyleColor(ImGuiCol_Header,        ImVec4{0.28f, 0.45f, 0.80f, 0.35f});
        ImGui::PushStyleColor(ImGuiCol_HeaderHovered, ImVec4{0.32f, 0.50f, 0.88f, 0.45f});
        ImGui::PushStyleColor(ImGuiCol_HeaderActive,  ImVec4{0.25f, 0.40f, 0.75f, 0.40f});
    }
    const bool opened = ImGui::TreeNodeEx((void *)(uint64_t)(uint32_t)entity, flags, "%s", tag.c_str());
    if (isSelected) {
        ImGui::PopStyleColor(3);
    }
    if (ImGui::IsItemClicked()) {
        m_SelectionContext = entity;
    }

    // ---- 右键菜单：删除实体（级联删子树） / 脱离父级 ----
    bool entityDeleted = false;
    bool detachRequested = false;
    if (ImGui::BeginPopupContextItem()) {
        if (ImGui::MenuItem("删除实体"))
            entityDeleted = true;
        if (ImGui::MenuItem("脱离父级"))
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
// 辅助：组件类别主题色（按组件名前缀归类，用于标题栏配色）
// ============================================================
static ImVec4 ComponentCategoryColor(const char *name) {
    if (!name) {
        return ImVec4{0.55f, 0.58f, 0.65f, 1.0f};
    }
    static const struct { const char *prefix; ImVec4 color; } kCategoryColors[] = {
        {"Transform",            {0.93f, 0.62f, 0.26f, 1.0f}}, // 橙：变换
        {"Camera",               {0.35f, 0.62f, 0.90f, 1.0f}}, // 天蓝：相机
        {"Follow Camera",        {0.35f, 0.62f, 0.90f, 1.0f}},
        {"Mesh Renderer",        {0.45f, 0.70f, 0.95f, 1.0f}}, // 钢蓝：网格/精灵渲染
        {"Sprite Renderer",      {0.45f, 0.70f, 0.95f, 1.0f}},
        {"Joint",                {0.82f, 0.45f, 0.75f, 1.0f}}, // 粉紫：骨骼/蒙皮
        {"Skin",                 {0.82f, 0.45f, 0.75f, 1.0f}},
        {"Animation",            {0.92f, 0.45f, 0.58f, 1.0f}}, // 玫红：动画
        {"Anim State Machine",   {0.92f, 0.45f, 0.58f, 1.0f}},
        {"Point Light",          {0.95f, 0.85f, 0.35f, 1.0f}}, // 金黄：光照
        {"Directional Light",    {0.95f, 0.85f, 0.35f, 1.0f}},
        {"Ambient Light",        {0.95f, 0.85f, 0.35f, 1.0f}},
        {"Environment",          {0.45f, 0.78f, 0.55f, 1.0f}}, // 草绿：环境
        {"Audio Source",         {0.72f, 0.55f, 0.92f, 1.0f}}, // 紫罗兰：音频
        {"Audio Listener",       {0.72f, 0.55f, 0.92f, 1.0f}},
        {"Rigid Body",           {0.38f, 0.82f, 0.48f, 1.0f}}, // 绿：物理
        {"Character Controller", {0.38f, 0.82f, 0.48f, 1.0f}},
        {"Box Collider",         {0.38f, 0.82f, 0.48f, 1.0f}},
        {"Sphere Collider",      {0.38f, 0.82f, 0.48f, 1.0f}},
        {"Capsule Collider",     {0.38f, 0.82f, 0.48f, 1.0f}},
        {"Bounding Box",         {0.38f, 0.82f, 0.48f, 1.0f}},
        {"Script",               {0.35f, 0.75f, 0.75f, 1.0f}}, // 青：脚本
    };
    for (const auto &entry : kCategoryColors) {
        if (strncmp(name, entry.prefix, strlen(entry.prefix)) == 0) {
            return entry.color;
        }
    }
    return ImVec4{0.55f, 0.58f, 0.65f, 1.0f}; // 默认灰蓝
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
        const ImVec4 accent = ComponentCategoryColor(name);

        // 组件标题栏：类别主题色分隔线 + 淡色底折叠头 + 主题色标题文字
        ImGui::PushStyleColor(ImGuiCol_Separator, ImVec4{accent.x, accent.y, accent.z, 0.30f});
        ImGui::Separator();
        ImGui::PopStyleColor();

        ImGui::PushStyleColor(ImGuiCol_Header,        ImVec4{accent.x, accent.y, accent.z, 0.16f});
        ImGui::PushStyleColor(ImGuiCol_HeaderHovered, ImVec4{accent.x, accent.y, accent.z, 0.28f});
        ImGui::PushStyleColor(ImGuiCol_HeaderActive,  ImVec4{accent.x, accent.y, accent.z, 0.24f});
        ImGui::PushStyleColor(ImGuiCol_Text,          accent);
        bool open = ImGui::TreeNodeEx((void *)typeid(T).hash_code(), treeNodeFlags, "%s", name);
        ImGui::PopStyleColor(4);
        ImGui::PopStyleVar();
        ImGui::SameLine(contentRegionAvailable.x - lineHeight * 0.5f - 5);
        // 右上角「…」设置按钮：同主题色淡底、悬停加深（点开 Remove component 菜单）
        ImGui::PushStyleColor(ImGuiCol_Button,        ImVec4{accent.x, accent.y, accent.z, 0.22f});
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4{accent.x, accent.y, accent.z, 0.42f});
        ImGui::PushStyleColor(ImGuiCol_ButtonActive,  ImVec4{accent.x, accent.y, accent.z, 0.55f});
        if (ImGui::Button("…", ImVec2{lineHeight, lineHeight})) {
            ImGui::OpenPopup("ComponentSettings");
        }
        ImGui::PopStyleColor(3);

        bool removeComponent = false;
        if (ImGui::BeginPopup("ComponentSettings")) {
            if (ImGui::MenuItem("移除组件"))
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
// 绘制选中实体的组件（顶层编排函数：下拉框单选一个组件展示）
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

    // 主操作按钮：高亮蓝底，一眼识别「为实体添加组件」入口
    ImGui::PushStyleColor(ImGuiCol_Button,        ImVec4{0.25f, 0.40f, 0.75f, 1.0f});
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4{0.32f, 0.48f, 0.85f, 1.0f});
    ImGui::PushStyleColor(ImGuiCol_ButtonActive,  ImVec4{0.20f, 0.34f, 0.65f, 1.0f});
    if (ImGui::Button("＋ 添加组件"))
        ImGui::OpenPopup("AddComponent");
    ImGui::PopStyleColor(3);

    DrawAddComponentPopup();
    ImGui::PopItemWidth();

    // ---- 组件下拉框：一次只显示一个组件，不再堆叠全部 ----
    // 收集实体上实际存在的组件（名字 + 绘制回调），选中项经下拉框切换；
    // 绘制仍复用 DrawComponent 模板，保留类别着色的标题栏与「…」移除菜单。
    struct ComponentEntry { const char *name; std::function<void()> draw; };
    std::vector<ComponentEntry> entries;

    if (entity.HasComponent<TransformComponent>())
        entries.push_back({"Transform", [&] { DrawComponent<TransformComponent>("Transform", entity, [](auto &c) { DrawTransformComponent(c); }); }});
    if (entity.HasComponent<CameraComponent>())
        entries.push_back({"Camera", [&] { DrawComponent<CameraComponent>("Camera", entity, [](auto &c) { DrawCameraComponent(c); }); }});
    if (entity.HasComponent<MeshRendererComponent>())
        entries.push_back({"Mesh Renderer", [&] { DrawComponent<MeshRendererComponent>("Mesh Renderer", entity, [](auto &c) { DrawMeshRendererComponent(c); }); }});
    if (entity.HasComponent<JointComponent>())
        entries.push_back({"Joint", [&] { DrawComponent<JointComponent>("Joint", entity, [](auto &c) { DrawJointComponent(c); }); }});
    if (entity.HasComponent<SkinComponent>())
        entries.push_back({"Skin", [&] { DrawComponent<SkinComponent>("Skin", entity, [this](auto &c) { DrawSkinComponent(c); }); }});
    if (entity.HasComponent<AnimationComponent>())
        entries.push_back({"Animation", [&] { DrawComponent<AnimationComponent>("Animation", entity, [&](auto &c) { DrawAnimationComponent(entity, c, m_Context); }); }});
    if (entity.HasComponent<AnimStateMachineComponent>())
        entries.push_back({"Anim State Machine", [&] { DrawComponent<AnimStateMachineComponent>("Anim State Machine", entity, [this, entity](auto &c) { DrawAnimStateMachine(entity, c); }); }});
    if (entity.HasComponent<SpriteRendererComponent>())
        entries.push_back({"Sprite Renderer", [&] { DrawComponent<SpriteRendererComponent>("Sprite Renderer", entity, [](auto &c) { DrawSpriteRendererComponent(c); }); }});
    if (entity.HasComponent<PointLightComponent>())
        entries.push_back({"Point Light", [&] { DrawComponent<PointLightComponent>("Point Light", entity, [](auto &c) { DrawPointLightComponent(c); }); }});
    if (entity.HasComponent<DirectionalLightComponent>())
        entries.push_back({"Directional Light", [&] { DrawComponent<DirectionalLightComponent>("Directional Light", entity, [](auto &c) { DrawDirectionalLightComponent(c); }); }});
    if (entity.HasComponent<AmbientLightComponent>())
        entries.push_back({"Ambient Light", [&] { DrawComponent<AmbientLightComponent>("Ambient Light", entity, [](auto &c) { DrawAmbientLightComponent(c); }); }});
    if (entity.HasComponent<EnvironmentComponent>())
        entries.push_back({"Environment", [&] { DrawComponent<EnvironmentComponent>("Environment", entity, [this](auto &c) { DrawEnvironmentComponent(c); }); }});
    if (entity.HasComponent<WaterComponent>())
        entries.push_back({"Water", [&] { DrawComponent<WaterComponent>("Water", entity, [](auto &c) { DrawWaterComponent(c); }); }});
    if (entity.HasComponent<AudioSourceComponent>())
        entries.push_back({"Audio Source", [&] { DrawComponent<AudioSourceComponent>("Audio Source", entity, [this](auto &c) { DrawAudioSourceComponent(c); }); }});
    if (entity.HasComponent<AudioListenerComponent>())
        entries.push_back({"Audio Listener", [&] { DrawComponent<AudioListenerComponent>("Audio Listener", entity, [](auto &c) { DrawAudioListenerComponent(c); }); }});
    if (entity.HasComponent<RigidBodyComponent>())
        entries.push_back({"Rigid Body", [&] { DrawComponent<RigidBodyComponent>("Rigid Body", entity, [&](auto &c) { DrawRigidBodyComponent(entity, c); }); }});
    if (entity.HasComponent<CharacterControllerComponent>())
        entries.push_back({"Character Controller", [&] { DrawComponent<CharacterControllerComponent>("Character Controller", entity, [&](auto &c) { DrawCharacterControllerComponent(entity, c); }); }});
    if (entity.HasComponent<FollowCameraComponent>())
        entries.push_back({"Follow Camera", [&] { DrawComponent<FollowCameraComponent>("Follow Camera", entity, [&](auto &c) { DrawFollowCameraComponent(c); }); }});
    if (entity.HasComponent<BoxColliderComponent>())
        entries.push_back({"Box Collider", [&] { DrawComponent<BoxColliderComponent>("Box Collider", entity, [&](auto &c) { DrawBoxColliderComponent(entity, c); }); }});
    if (entity.HasComponent<SphereColliderComponent>())
        entries.push_back({"Sphere Collider", [&] { DrawComponent<SphereColliderComponent>("Sphere Collider", entity, [&](auto &c) { DrawSphereColliderComponent(entity, c); }); }});
    if (entity.HasComponent<CapsuleColliderComponent>())
        entries.push_back({"Capsule Collider", [&] { DrawComponent<CapsuleColliderComponent>("Capsule Collider", entity, [&](auto &c) { DrawCapsuleColliderComponent(entity, c); }); }});
    if (entity.HasComponent<BoundingBoxComponent>())
        entries.push_back({"Bounding Box", [&] { DrawComponent<BoundingBoxComponent>("Bounding Box", entity, [this, entity](auto &c) { DrawBoundingBoxComponent(entity, c, m_Context); }); }});
    if (entity.HasComponent<ScriptComponent>())
        entries.push_back({"Script", [&] { DrawComponent<ScriptComponent>("Script", entity, [&](auto &c) { DrawScriptComponent(c, entity); }); }});

    if (entries.empty()) {
        ImGui::TextDisabled("该实体没有可编辑组件（仅 Tag）");
        return;
    }

    // 每个实体独立记住上次查看的组件：按实体句柄作为 ImGui 状态键，切换实体不串台。
    // 关键：GetStateStorage 返回「当前窗口」的存储，必须在打开下拉框之前（仍处于
    // Properties 窗口作用域）拿到指针；否则 BeginCombo 会把当前窗口切到弹出层，
    // SetInt 写进弹出层存储、下一帧读回 Properties 存储就找不到值 → 瞬间回到首项。
    ImGuiStorage *propsStorage = ImGui::GetStateStorage();
    ImGuiID selKey = ImGui::GetID((void *)(uintptr_t)(uint32_t)entity);
    int selected = propsStorage->GetInt(selKey, 0);
    // 越界钳位：删除当前查看的组件后，停留在末尾（而非跳回首项）
    if (selected < 0) {
        selected = 0;
    } else if (selected >= static_cast<int>(entries.size())) {
        selected = static_cast<int>(entries.size()) - 1;
    }

    ImGui::Separator();
    const char *preview = entries[selected].name;
    if (ImGui::BeginCombo("组件", preview)) {
        for (int i = 0; i < static_cast<int>(entries.size()); ++i) {
            const bool isSel = (i == selected);
            if (ImGui::Selectable(entries[i].name, isSel)) {
                selected = i;
                propsStorage->SetInt(selKey, selected);
            }
            if (isSel) {
                ImGui::SetItemDefaultFocus();
            }
        }
        ImGui::EndCombo();
    }
    ImGui::Separator();

    // 只绘制下拉框选中的那个组件
    entries[selected].draw();
}

// ============================================================
// Add Component 弹窗
// ============================================================
void SceneHierarchyPanel::DrawAddComponentPopup() {
    // 按类别分列展示：每个类别占一竖列，不同类别并排（表头即类别名）。
    // 组件项存「中文名 + 添加动作」，动作捕获 this 复用 TryAddComponent<T> 判重逻辑。
    struct AddEntry { const char *name; std::function<void()> add; };
    struct AddCategory { const char *name; std::vector<AddEntry> items; };

    const std::vector<AddCategory> categories = {
        { "渲染", {
            { "网格渲染器", [this] { TryAddComponent<MeshRendererComponent>("网格渲染器"); } },
            { "精灵渲染器", [this] { TryAddComponent<SpriteRendererComponent>("精灵渲染器"); } },
            { "环境",       [this] { TryAddComponent<EnvironmentComponent>("环境"); } },
            { "水面",       [this] { TryAddComponent<WaterComponent>("水面"); } },
            { "点光源",     [this] { TryAddComponent<PointLightComponent>("点光源"); } },
            { "平行光",     [this] { TryAddComponent<DirectionalLightComponent>("平行光"); } },
            { "环境光",     [this] { TryAddComponent<AmbientLightComponent>("环境光"); } },
            { "包围盒",     [this] { TryAddComponent<BoundingBoxComponent>("包围盒"); } },
        } },
        { "相机", {
            { "相机",       [this] { TryAddComponent<CameraComponent>("相机"); } },
            { "跟随相机",   [this] { TryAddComponent<FollowCameraComponent>("跟随相机"); } },
        } },
        { "动画", {
            { "动画",       [this] { TryAddComponent<AnimationComponent>("动画"); } },
            { "动画状态机", [this] { TryAddComponent<AnimStateMachineComponent>("动画状态机"); } },
            { "关节",       [this] { TryAddComponent<JointComponent>("关节"); } },
            { "蒙皮",       [this] { TryAddComponent<SkinComponent>("蒙皮"); } },
        } },
        { "物理", {
            { "刚体",       [this] { TryAddComponent<RigidBodyComponent>("刚体"); } },
            { "角色控制器", [this] { TryAddComponent<CharacterControllerComponent>("角色控制器"); } },
            { "盒碰撞体",   [this] { TryAddComponent<BoxColliderComponent>("盒碰撞体"); } },
            { "球碰撞体",   [this] { TryAddComponent<SphereColliderComponent>("球碰撞体"); } },
            { "胶囊碰撞体", [this] { TryAddComponent<CapsuleColliderComponent>("胶囊碰撞体"); } },
        } },
        { "音频", {
            { "音频源",     [this] { TryAddComponent<AudioSourceComponent>("音频源"); } },
            { "音频监听器", [this] { TryAddComponent<AudioListenerComponent>("音频监听器"); } },
        } },
        { "脚本", {
            { "脚本",       [this] { TryAddComponent<ScriptComponent>("脚本"); } },
        } },
    };

    // 显式给定弹窗宽度：由各列内容（表头/组件名最宽者 + 单元格内边距）求和。
    // 弹窗带 AlwaysAutoResize，首帧按上一帧内容尺寸（=0）布局，窗口先为 0 宽，
    // 表格在 0 宽宿主里布局列宽可能被钳制；SetNextWindowSize 从首帧就定宽，
    // 保证 SizingFixedFit 按内容正确算列宽。高度传 0 仍按内容自适应。
    const float cellPadX = ImGui::GetStyle().CellPadding.x;
    float totalWidth = 0.0f;
    for (const auto &cat : categories) {
        float w = ImGui::CalcTextSize(cat.name).x;              // 表头（类别名）
        for (const auto &item : cat.items)
            w = std::max(w, ImGui::CalcTextSize(item.name).x);  // 组件名
        w += cellPadX * 2.0f;                                   // 单元格左右内边距
        totalWidth += w;
    }
    totalWidth += static_cast<float>(categories.size()) + 1.0f; // 列间竖线 + 表框
    totalWidth += ImGui::GetStyle().WindowPadding.x * 2.0f;
    totalWidth += 16.0f;                                        // 余量，避免测量与表格实际列宽有出入
    ImGui::SetNextWindowSize(ImVec2(totalWidth, 0.0f));

    if (!ImGui::BeginPopup("AddComponent"))
        return;

    // SizingFixedFit：每列按内容自适应宽度；不设 NoHostExtendX，表格横向拉伸
    // 填满已定宽的弹窗（固定列保持内容宽度，多余宽度落尾部），杜绝右缘裁剪。
    const int columnCount = static_cast<int>(categories.size());
    constexpr ImGuiTableFlags kTableFlags =
        ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_BordersInnerV;
    if (ImGui::BeginTable("AddComponentTable", columnCount, kTableFlags)) {
        // 表头行：类别名。标签带 ## 后缀与组件项隔离 ID（相机/动画/脚本同名）。
        // 表头着淡蓝底 + 亮文字，与下方组件项拉开层级。
        ImGui::PushStyleColor(ImGuiCol_Header,        ImVec4{0.25f, 0.35f, 0.55f, 0.35f});
        ImGui::PushStyleColor(ImGuiCol_HeaderHovered, ImVec4{0.30f, 0.42f, 0.65f, 0.45f});
        ImGui::PushStyleColor(ImGuiCol_Text,          ImVec4{0.75f, 0.85f, 1.0f, 1.0f});
        for (const auto &cat : categories) {
            const std::string headerLabel = std::string(cat.name) + "##hdr";
            ImGui::TableSetupColumn(headerLabel.c_str());
        }
        ImGui::TableHeadersRow();
        ImGui::PopStyleColor(3);

        // 逐行放置组件项：行号超过某列条目数则该列留空，各列高度以最长列为准
        size_t maxRows = 0;
        for (const auto &cat : categories)
            maxRows = std::max(maxRows, cat.items.size());
        for (size_t row = 0; row < maxRows; ++row) {
            ImGui::TableNextRow();
            for (int col = 0; col < columnCount; ++col) {
                ImGui::TableSetColumnIndex(col);
                if (row < categories[col].items.size()) {
                    const auto &entry = categories[col].items[row];
                    if (ImGui::Selectable(entry.name))
                        entry.add();
                }
            }
        }
        ImGui::EndTable();
    }

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
    // 点击已由弹窗表格中的 Selectable 负责；这里只执行添加/判重，不能再调用
    // ImGui::Selectable，否则会在同一弹窗内生成重复控件 ID，导致点击被吞掉。
    const bool added = !m_SelectionContext.HasComponent<T>();
    if (added) {
        m_SelectionContext.AddComponent<T>();
    } else {
        GE_CORE_WARN("This entity already has {0}!", name);
    }
    ImGui::CloseCurrentPopup();
    return added;
}

} // namespace GE
