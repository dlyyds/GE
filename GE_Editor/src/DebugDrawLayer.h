#pragma once

#include "GE/GE.h"
#include "GE/Render/Camera.h"

#include "EditorContext.h"

#include "imgui.h"

#include <memory>

namespace GE {

/// 场景调试线框层 —— 独立承载 Scene 视口上的调试叠加绘制。
///
/// 包围盒、物理碰撞体和第一人称视点标记原本都在 SceneLayer 里绘制；
/// 这里把它们拆成单独 Layer，由 SceneLayer 在 Scene 窗口内回调叠加，
/// 本层自己维护显示开关和调试面板，避免 SceneLayer 继续膨胀。
class DebugDrawLayer : public Layer {
public:
    explicit DebugDrawLayer(std::shared_ptr<EditorContext> context);

    ~DebugDrawLayer() override;

    void OnAttach() override;

    void OnDetach() override;

    void OnUpdate(Timestep &ts) override;

    void OnEvent(Event &event) override;

    void OnImGuiRender() override;

    /// 从 SceneLayer 的 Scene 视口窗口内调用，负责叠加世界调试线框。
    /// imagePos 是视口图像左上角，viewportSize 是视口像素尺寸。
    void RenderSceneOverlay(const Camera &camera, const glm::vec2 &imagePos,
                            const glm::vec2 &viewportSize);

private:
    std::shared_ptr<EditorContext> m_Context;  ///< 共享场景上下文

    /// 视口是否叠加包围盒线框（静态盒白灰、蒙皮绑定盒红）
    bool m_ShowBounds = true;

    /// 包围盒上的关节露点红绿点是否显示（依赖 m_ShowBounds 开启）
    bool m_ShowJointDots = true;

    /// 视口是否叠加物理碰撞体线框（青绿色：盒子/球体）
    bool m_ShowColliders = false;

    /// 是否叠加第一人称视点标记（依赖 FirstPersonCameraComponent 实体存在）
    bool m_ShowFPSEyes = true;

    /// 是否叠加方向光调试图标（太阳盘 + 光线束，颜色取灯光色）
    bool m_ShowDirLights = true;

    void DrawWorldBounds(const Camera &camera, const glm::vec2 &imagePos,
                         const glm::vec2 &viewportSize);

    void DrawColliders(const Camera &camera, const glm::vec2 &imagePos,
                       const glm::vec2 &viewportSize);

    void DrawFirstPersonEyes(const Camera &camera, const glm::vec2 &imagePos,
                             const glm::vec2 &viewportSize);

    void DrawDirectionalLights(const Camera &camera, const glm::vec2 &imagePos,
                               const glm::vec2 &viewportSize);
};

} // namespace GE
