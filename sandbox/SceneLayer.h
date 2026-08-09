#pragma once

#include "GE/GE.h"
#include "GE/Render/Material.h"
#include "GE/Render/Texture.h"
#include "GE/Render/Mesh.h"
#include "GE/Scene/Scene.h"
#include "GE/Scene/Entity.h"
#include "GE/Scene/SceneSerializer.h"

#include "Panels/SceneHierarchyPanel.h"

#include <memory>

namespace GE {

/// 场景序列化测试层。
/// 创建一个带相机、光照、多个 3D 实体（含带材质的模型）的场景，
/// 通过 ImGui 面板进行 保存 / 加载 / 新建 操作，
/// 配合 SceneHierarchyPanel 编辑实体与组件，用于验证场景序列化与反序列化。
class SceneLayer : public Layer {
public:
    SceneLayer();

    ~SceneLayer() override;

    void OnAttach() override;

    void OnDetach() override;

    void OnUpdate(Timestep &ts) override;

    void OnEvent(Event &event) override;

    void OnImGuiRender() override;

private:
    std::unique_ptr<Mesh>     m_CubeMesh;       ///< 立方体网格
    std::unique_ptr<Mesh>     m_SphereMesh;     ///< 球体网格
    Texture  *m_CheckerTexture = nullptr;       ///< 棋盘纹理（由全局 TextureManager 持有）
    Texture  *m_NormalTexture   = nullptr;      ///< 法线贴图（由全局 TextureManager 持有）
    Material *m_TexturedMaterial = nullptr;     ///< 带棋盘纹理 + 法线贴图的材质（由全局 MaterialManager 持有）

    std::unique_ptr<Scene> m_Scene;                              ///< 场景
    std::unique_ptr<SceneSerializer> m_SceneSerializer;          ///< 场景序列化器（持有加载的网格资源）
    Entity m_CameraEntity;                                       ///< 相机实体
    SceneHierarchyPanel m_HierarchyPanel;                        ///< 场景层级面板（ImGui）

    /// 保存场景到文件（弹出文件对话框）
    void SaveScene();

    /// 从文件加载场景（弹出文件对话框，会清空当前场景）
    void LoadScene();

    /// 新建空场景（清空当前场景内容）
    void NewScene();

    /// 场景中查找主相机（Primary=true）并绑定到 m_CameraEntity
    void RebindCameraEntity();
};

} // namespace GE