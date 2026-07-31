#pragma once

#include "GE/GE.h"
#include "GE/Render/Texture.h"
#include "GE/Render/Mesh.h"
#include "GE/Scene/Scene.h"
#include "GE/Scene/Entity.h"

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

    // 脚本参数（通过 ImGui 调节）
    bool  m_AutoRotate{true};                 ///< 是否自动旋转
    float m_AutoRotateSpeed{0.5f};            ///< 自动旋转速度（弧度/秒），作用于 Y 轴

    /// 刷新模型上的 ScriptComponent
    void RefreshScript();
    /// 刷新相机上的鼠标控制 ScriptComponent
    void RefreshCameraScript();
};

} // namespace GE
