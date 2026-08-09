#pragma once

#include "GE/GE.h"
#include "GE/Scene/Scene.h"
#include "GE/Scene/Entity.h"
#include "GE/Scene/SceneSerializer.h"

#include "Panels/SceneHierarchyPanel.h"
#include "Panels/ResourcePanel.h"
#include "SceneViewport.h"

#include <memory>

namespace GE {

/// 场景序列化测试层。
/// 启动时从文件加载默认场景（assets/scenes/test.scene），
/// 通过 ImGui 面板进行 保存 / 加载 / 新建 操作，
/// 配合 SceneHierarchyPanel 编辑实体与组件，用于验证场景序列化与反序列化。
/// 网格/纹理/材质由全局管理器加载持有，场景本身不拥有资源。
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
    std::unique_ptr<Scene> m_Scene;                              ///< 场景
    std::unique_ptr<SceneSerializer> m_SceneSerializer;          ///< 场景序列化器（纹理/材质/网格由全局管理器持有）
    Entity m_CameraEntity;                                       ///< 相机实体
    SceneHierarchyPanel m_HierarchyPanel;                        ///< 场景层级面板（ImGui）
    ResourcePanel m_ResourcePanel;                               ///< 资源面板（ImGui）

    /// 场景视口（离屏渲染目标 + ImGui 图片），场景渲染进它再贴到窗口
    std::unique_ptr<SceneViewport> m_Viewport;
    /// 视口窗口尺寸（上一帧由 OnImGuiRender 记录，供 OnUpdate 离屏渲染使用）
    glm::vec2 m_ViewportSize{0.0f, 0.0f};

    /// 离屏目标重建限流计时器（避免拖拽视口时每帧重建 GPU 资源）
    float m_ResizeCooldown = 0.0f;

    /// 保存场景到文件（弹出文件对话框）
    void SaveScene();

    /// 从文件加载场景（不清空当前场景句柄，会重建场景并重新绑定相机）
    bool LoadSceneFromFile(std::string_view filepath);

    /// 从文件加载场景（弹出文件对话框，会清空当前场景）
    void LoadScene();

    /// 新建空场景（清空当前场景内容）
    void NewScene();
};

} // namespace GE