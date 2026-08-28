#pragma once

#include "GE/GE.h"
#include "GE/Scene/Scene.h"
#include "GE/Scene/Entity.h"

#include "EditorContext.h"
#include "SceneViewport.h"

#include "imgui.h"

#include <memory>

namespace GE {

class GizmoController; // 前向声明，避免在头文件引入实现

/// 场景层 —— 只负责场景渲染（离屏视口 + 相机）与 文件操作（保存/加载/新建）。
///
/// 场景状态（Scene / 序列化器 / 相机实体）放在共享的 EditorContext 中，
/// 层级面板等由各自的 Layer 承载并共享该上下文。启动时从文件加载默认场景。
/// 网格/纹理/材质由全局管理器加载持有，场景本身不拥有资源。
class SceneLayer : public Layer {
public:
    explicit SceneLayer(std::shared_ptr<EditorContext> context);

    ~SceneLayer() override;

    void OnAttach() override;

    void OnDetach() override;

    void OnUpdate(Timestep &ts) override;

    void OnEvent(Event &event) override;

    void OnImGuiRender() override;

    /// 保存场景到文件（弹出文件对话框），供顶部菜单调用
    void SaveScene();

    /// 从文件加载场景（弹出文件对话框，会清空当前场景），供顶部菜单调用
    void LoadScene();

    /// 新建空场景（清空当前场景内容），供顶部菜单调用
    void NewScene();

    /// 绑定 gizmo 控制器（在 Scene 视口内渲染变换 gizmo）
    void SetGizmoController(std::unique_ptr<GizmoController> gizmo);

private:
    std::shared_ptr<EditorContext> m_Context;  ///< 共享场景上下文

    /// 变换 gizmo 控制器（在 Scene 窗口内叠加，由本层回调其 Render）
    std::unique_ptr<GizmoController> m_Gizmo;

    /// 场景视口（离屏渲染目标 + ImGui 图片），场景渲染进它再贴到窗口
    std::unique_ptr<SceneViewport> m_Viewport;
    /// 视口窗口尺寸（上一帧由 OnImGuiRender 记录，供 OnUpdate 离屏渲染使用）
    glm::vec2 m_ViewportSize{0.0f, 0.0f};

    /// 离屏目标重建限流计时器（避免拖拽视口时每帧重建 GPU 资源）
    float m_ResizeCooldown = 0.0f;

    /// 停靠目标 DockSpace ID（根上下文取 "MainDockspace"）
    ImGuiID m_DockSpaceID = 0;

    /// 鼠标是否悬停在 Scene 视口窗口内（上一帧 OnImGuiRender 记录，供 OnEvent 判断）
    bool m_SceneWindowHovered = false;

    /// 在视口内按下且尚未释放的鼠标按键位掩码（bit = 1 << MouseCode）。
    /// 用于把「拖出视口后松开」的释放事件仍回传相机，避免相机按键状态卡住。
    uint32_t m_ViewportCapturedButtons = 0;

    /// 在 Scene 视口内叠加绘制网格世界包围盒线框（调试剔除/蒙皮用）。
    /// 与 GizmoController 同一套 OpenGL 投影 + 屏幕映射，保证线与画面/gizmo 对齐。
    void DrawWorldBounds(const glm::vec2 &imagePos);

    /// 在 Scene 视口内叠加绘制物理碰撞体线框（盒子 12 棱 + 球体正交圆环）。
    /// 变换语义与 PhysicsWorld::BuildShapeForEntity 一致：局部 TRS，
    /// 半尺寸/半径乘比例烘焙进形状，Offset 不乘比例。
    void DrawColliders(const glm::vec2 &imagePos);

    /// 视口是否叠加包围盒线框（静态盒白灰、蒙皮绑定盒红）
    bool m_ShowBounds = true;

    /// 包围盒上的关节露点红绿点是否显示（依赖 m_ShowBounds 开启）
    bool m_ShowJointDots = true;

    /// 视口是否叠加物理碰撞体线框（青绿色：盒子/球体）
    bool m_ShowColliders = false;

    /// 从文件加载场景（会重建场景并重新绑定相机）
    bool LoadSceneFromFile(std::string_view filepath);

    /// 从代码程序化构建默认场景（编译期开关 GE_EDITOR_BUILD_SCENE_FROM_CODE 控制）
    void BuildDefaultSceneFromCode();
};

} // namespace GE