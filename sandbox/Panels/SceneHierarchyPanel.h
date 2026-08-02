#pragma once

#include "GE/Core/Base.h"
#include "GE/Core/Log.h"
#include "GE/Scene/Scene.h"
#include "GE/Scene/Entity.h"

#include "GE/Debug/Assert.h"

namespace GE {

/// 场景层级面板 —— 用 ImGui 展示场景实体树和选中实体的组件属性。
///
/// 功能：
/// - 左侧 "Scene Hierarchy" 窗口：列出场景中所有实体，支持点击选中、右键菜单（创建/删除实体）
/// - 右侧 "Properties" 窗口：显示选中实体的所有组件，可编辑组件属性，支持添加/删除组件
///
/// 适配当前项目的组件系统：
///   TagComponent / TransformComponent / SpriteRendererComponent /
///   MeshComponent / CameraComponent / PointLightComponent / ScriptComponent
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

private:
    Scene *m_Context = nullptr;     ///< 关联的场景（非拥有）
    Entity m_SelectionContext;      ///< 当前选中的实体
};

} // namespace GE
