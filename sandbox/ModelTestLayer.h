#pragma once

#include "GE/GE.h"
#include "GE/Render/Texture.h"
#include "GE/Render/Mesh.h"
#include "GE/Scene/Scene.h"
#include "GE/Scene/Entity.h"

#include "Panels/SceneHierarchyPanel.h"

namespace GE {

/// 3D 模型渲染测试层。
/// 使用场景系统（Scene + Entity + 组件）渲染一个 3D 立方体，
/// 通过透视摄像机观察，支持 Blinn-Phong 光照，
/// 可通过 ImGui 面板调节模型 Transform、颜色、光照参数等。
class ModelTestLayer : public Layer {
public:
    ModelTestLayer();
    ~ModelTestLayer() override;

    void OnAttach() override;
    void OnDetach() override;
    void OnUpdate(Timestep &ts) override;
    void OnEvent(Event &event) override;
    void OnImGuiRender() override;

private:
    std::unique_ptr<Mesh>    m_CubeMesh;     ///< 立方体网格
    std::unique_ptr<Texture> m_Texture;      ///< 棋盘纹理
    std::unique_ptr<Scene>   m_Scene;        ///< 场景
    Entity m_ModelEntity;                    ///< 模型实体
    Entity m_CameraEntity;                   ///< 相机实体
    Entity m_RedLightEntity;                 ///< 红色点光源实体
    Entity m_BlueLightEntity;                ///< 蓝色点光源实体

    SceneHierarchyPanel m_HierarchyPanel;    ///< 场景层级面板（ImGui）

    // 脚本参数（通过 ImGui 调节）
    bool  m_AutoRotate{true};                 ///< 是否自动旋转
    float m_AutoRotateSpeed{0.5f};            ///< 自动旋转速度（弧度/秒），作用于 Y 轴

    // ImGuizmo 相关
    int   m_GizmoType{-1};                    ///< 当前 gizmo 操作类型（-1=关闭，对应 ImGuizmo::OPERATION）
    bool  m_UseSnap{false};                   ///< 是否启用吸附
    float m_SnapValue{0.5f};                  ///< 吸附步长
    bool  m_GizmoUsing{false};                ///< 上一帧是否正在使用 gizmo（用于阻挡相机事件）

    /// 绘制 ImGuizmo 3D 变换 gizmo（在 OnImGuiRender 中调用）
    void RenderImGuizmo();
    /// 绘制 ImGuizmo 控制面板（提示文字 + 模式/吸附参数）
    void RenderImGuizmoPanel();

    /// 刷新模型上的 ScriptComponent
    void RefreshScript();
    /// 刷新相机上的鼠标控制 ScriptComponent
    void RefreshCameraScript();
    /// 刷新点光源的旋转动画 ScriptComponent
    void RefreshLightScripts();
};

} // namespace GE
