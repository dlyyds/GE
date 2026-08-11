#pragma once

#include "GE/Core/Layer.h"
#include "GE/Scene/Entity.h"
#include "GE/Render/Material.h"
#include "SceneViewport.h"

#include <glm/glm.hpp>
#include <memory>

namespace GE {

class Scene;
class Texture;
class Mesh;

/// 自发光（Emissive）冒烟测试层 —— 编辑器内的独立测试窗口。
///
/// 拥有一个独立的测试 Scene 与离屏 SceneViewport，渲染到停靠的
/// "Sandbox" 窗口中，与主编辑场景互不干扰。测试内容：
///   左：无自发光贴图（控制组，被光照正常照亮）
///   中：棋盘格自发光贴图（发出棋盘格光）
///   右：纯色暖橙自发光（整体发橙色光）
class SandboxLayer : public Layer {
public:
    SandboxLayer();
    ~SandboxLayer() override;

    void OnAttach() override;
    void OnDetach() override;
    void OnUpdate(Timestep &ts) override;
    void OnEvent(Event &event) override;
    void OnImGuiRender() override;

private:
    // 测试场景与离屏视口
    std::unique_ptr<Scene>        m_Scene;
    std::unique_ptr<SceneViewport> m_Viewport;
    glm::vec2 m_ViewportSize{0.0f, 0.0f}; ///< 视口窗口尺寸（上帧 OnImGuiRender 记录）
    float     m_ResizeCooldown = 0.0f;    ///< 视口重建限流计时
    uint64_t  m_DockSpaceID = 0;          ///< 主停靠区 ID（与 DockSpaceLayer 一致）

    // 资源共享（由全局管理器持有，仅存非拥有指针）
    Texture  *m_WhiteTex    = nullptr;
    Texture  *m_EmissiveTex = nullptr;
    Texture  *m_OrangeTex   = nullptr;
    Material *m_MatNoEmissive = nullptr;
    Material *m_MatChecker    = nullptr;
    Material *m_MatOrange     = nullptr;

    // 测试场景实体句柄
    Entity m_CameraEntity;
    Entity m_AmbientEntity;
    Entity m_CubeEntities[3];

    // 相机轨道角
    float m_Angle = 0.0f;

    // 面板参数
    float m_Ambient              = 0.15f;
    float m_CheckerStrength      = 1.5f;
    bool  m_EnableCheckerEmissive = true;
};

} // namespace GE