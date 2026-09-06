#pragma once

#include "GE/Core/Base.h"
#include "GE/Core/Log.h"
#include "GE/Scene/Scene.h"
#include "GE/Scene/Entity.h"
#include "GE/Scene/Components.h"

#include "GE/Debug/Assert.h"

#include <unordered_map>

#include "imgui.h"

namespace GE {

/// 场景层级面板 —— 用 ImGui 展示场景实体树和选中实体的组件属性。
///
/// 功能：
/// - 左侧 "Scene Hierarchy" 窗口：列出场景中所有实体，支持点击选中、右键菜单（创建/删除实体）
/// - 右侧 "Properties" 窗口：显示选中实体的所有组件，可编辑组件属性，支持添加/删除组件
///
/// 适配当前项目的组件系统：
///   TagComponent / TransformComponent / SpriteRendererComponent /
///   MeshRendererComponent / CameraComponent / PointLightComponent / ScriptComponent
///
/// 注意：面板不拥有场景的所有权，仅持有裸指针。
///      调用方需保证场景生命周期长于面板，或在场景销毁前调用 SetContext(nullptr)。
class SceneHierarchyPanel {
public:
    SceneHierarchyPanel() = default;

    explicit SceneHierarchyPanel(Scene *scene);

    /// 设置面板关联的场景（非拥有，传入 nullptr 可解除关联）
    void SetContext(Scene *scene);

    /// 设置当前选中的实体
    void SetSelectedEntity(Entity entity);

    /// 每帧 ImGui 渲染
    void OnImGuiRender();

    /// 获取当前选中的实体
    [[nodiscard]] Entity GetSelectedEntity() const { return m_SelectionContext; }

private:
    /// 绘制单个实体节点（树状）
    void DrawEntityNode(Entity entity);

    /// 绘制选中实体的所有组件属性
    void DrawComponents(Entity entity);

    // ---- 组件绘制辅助（每个复杂组件一个函数，保持 DrawComponents 简洁） ----

    /// 绘制 "Add Component" 弹窗
    void DrawAddComponentPopup();

    /// 尝试为当前选中实体添加组件，若已存在则输出警告
    template <typename T>
    bool TryAddComponent(const char *name);

    /// 绘制 Transform 组件属性
    static void DrawTransformComponent(TransformComponent &component);

    /// 绘制 Camera 组件属性
    static void DrawCameraComponent(CameraComponent &component);

    /// 绘制 Mesh Renderer 组件属性（scene 供「导入 glTF 场景」落实体树，静态函数无法访问 m_Context）
    static void DrawMeshRendererComponent(MeshRendererComponent &component, Scene *scene);

    /// 绘制 Joint（骨骼关节）组件属性
    static void DrawJointComponent(JointComponent &component);

    /// 绘制 Skin（皮肤）组件属性（需访问场景解析关节实体 Tag）
    void DrawSkinComponent(SkinComponent &component);

    /// 绘制 Animation（骨骼动画）组件属性（需读取同实体 ASM 状态做运行中占位 / 手动覆盖处置）
    static void DrawAnimationComponent(Entity entity, AnimationComponent &component, Scene *scene);

    /// 绘制 AnimStateMachine（动画状态机）组件属性：状态/转换/条件表 + 调试参数驱动
    void DrawAnimStateMachine(Entity entity, AnimStateMachineComponent &component);

    /// 绘制 SpriteRenderer 组件属性
    static void DrawSpriteRendererComponent(SpriteRendererComponent &component);

    /// 绘制 PointLight 组件属性
    static void DrawPointLightComponent(PointLightComponent &component);

    /// 绘制 DirectionalLight 组件属性
    static void DrawDirectionalLightComponent(DirectionalLightComponent &component);

    /// 绘制 AmbientLight 组件属性
    static void DrawAmbientLightComponent(AmbientLightComponent &component);

    /// 绘制 Environment 组件属性
    void DrawEnvironmentComponent(EnvironmentComponent &component);

    /// 获取环境预览图缩略图的 ImGui 纹理 ID（environments/<名称>/preview.png，按名称缓存）
    ImTextureID GetEnvironmentThumbnail(const std::string &envName);

    /// 绘制 RigidBody 组件属性
    void DrawRigidBodyComponent(Entity entity, RigidBodyComponent &component);

    /// 绘制 CharacterController（角色控制器）组件属性
    void DrawCharacterControllerComponent(Entity entity, CharacterControllerComponent &component);

    /// 绘制 FollowCamera（跟随相机）组件属性
    void DrawFollowCameraComponent(FollowCameraComponent &component);

    /// 绘制 BoxCollider 组件属性
    void DrawBoxColliderComponent(Entity entity, BoxColliderComponent &component);

    /// 绘制 SphereCollider 组件属性
    void DrawSphereColliderComponent(Entity entity, SphereColliderComponent &component);

    /// 绘制 CapsuleCollider 组件属性
    void DrawCapsuleColliderComponent(Entity entity, CapsuleColliderComponent &component);

    /// 绘制 BoundingBox（实体级粗剔除盒）组件属性
    static void DrawBoundingBoxComponent(Entity entity, BoundingBoxComponent &component, Scene *scene);

    /// 根据实体子树内全部蒙皮关节的位置自动计算本地包围盒并写回（盒罩住所有关节）。
    /// 子树内检测到动画组件时，扫描每个 clip 的逐帧关节极值取最大范围（播放一遍的效果）。
    /// 无关节时不修改，返回 false。
    static bool AutoFitBoundingBoxToJoints(Scene *scene, Entity entity, BoundingBoxComponent &bb);

    /// 绘制 Script 组件属性
    static void DrawScriptComponent(ScriptComponent &component, Entity entity);

private:
    Scene *m_Context = nullptr;     ///< 关联的场景（非拥有）
    Entity m_SelectionContext;      ///< 当前选中的实体

    /// 停靠目标 DockSpace ID（根上下文取 "MainDockspace"，首帧初始化一次）
    ImGuiID m_DockSpaceID = 0;

    /// 环境名 → 预览图 ImGui 纹理 ID 缓存（避免每帧重建描述符集）
    std::unordered_map<std::string, ImTextureID> m_EnvThumbnails;
};

} // namespace GE
