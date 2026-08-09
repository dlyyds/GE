#pragma once

#include "GE/GE.h"
#include "GE/Render/Material.h"
#include "GE/Render/Texture.h"
#include "GE/Render/Mesh.h"
#include "GE/Scene/Scene.h"
#include "GE/Scene/Entity.h"
#include "GE/Scene/SceneSerializer.h"

#include "Panels/SceneHierarchyPanel.h"

#include <vector>

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
    Mesh *m_CubeMesh = nullptr;   ///< 立方体网格（由全局 MeshManager 持有）
    Mesh *m_SphereMesh = nullptr; ///< 球体网格（由全局 MeshManager 持有）
    Texture  *m_Texture      = nullptr;       ///< 棋盘纹理（由全局 TextureManager 持有）
    Texture  *m_NormalTexture = nullptr;      ///< 法线贴图纹理（由全局 TextureManager 持有）
    Material *m_ModelMaterial = nullptr;      ///< 模型材质（由全局 MaterialManager 持有）
    std::unique_ptr<Scene> m_Scene; ///< 场景
    std::unique_ptr<SceneSerializer> m_SceneSerializer; ///< 场景序列化器（纹理/材质/网格由全局管理器持有）
    Entity m_ModelEntity; ///< 模型实体
    Entity m_CameraEntity; ///< 相机实体
    Entity m_RedLightEntity; ///< 红色点光源实体
    Entity m_BlueLightEntity; ///< 蓝色点光源实体
    Entity m_DirLightEntity; ///< 方向光实体
    Entity m_AmbientLightEntity; ///< 环境光实体
    Entity m_FloorEntity; ///< 物理地板实体
    Entity m_PhysicsBallEntity; ///< 物理球体实体
    std::vector<Entity> m_InstancedCubes; ///< instancing 演示：共享同材质同 mesh 的立方体实例

    SceneHierarchyPanel m_HierarchyPanel; ///< 场景层级面板（ImGui）

    // ImGuizmo 相关
    int m_GizmoType{-1}; ///< 当前 gizmo 操作类型（-1=关闭，对应 ImGuizmo::OPERATION）
    bool m_UseSnap{false}; ///< 是否启用吸附
    float m_SnapValue{0.5f}; ///< 吸附步长

    /// 绘制 ImGuizmo 3D 变换 gizmo（在 OnImGuiRender 中调用）
    void RenderImGuizmo();

    /// 绘制 ImGuizmo 控制面板（提示文字 + 模式/吸附参数）
    void RenderImGuizmoPanel();

    /// 刷新相机上的鼠标控制 ScriptComponent
    void RefreshCameraScript();

    /// 刷新点光源的旋转动画 ScriptComponent
    void RefreshLightScripts();

    /// 保存场景到文件（弹出文件对话框）
    void SaveScene();

    /// 从文件加载场景（弹出文件对话框，会清空当前场景）
    void LoadScene();

    /// 新建空场景（清空当前场景内容）
    void NewScene();

    /// 场景中查找名为 MainCamera 的实体并绑定到 m_CameraEntity
    void RebindCameraEntity();

    /// 场景中查找方向光和环境光实体并绑定
    void RebindLightEntities();
};

} // namespace GE
