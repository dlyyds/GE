#pragma once

#include "GE/GE.h"
#include "GE/Scene/Scene.h"
#include "GE/Scene/Entity.h"
#include "GE/Render/Camera.h"

#include "EditorContext.h"
#include "SceneViewport.h"

#include "imgui.h"

#include <memory>

namespace GE {

class GizmoController; // 前向声明，避免在头文件引入实现
class RenderTarget;    // 仅用于离屏渲染私有方法签名
class DebugDrawLayer;  // 调试线框叠加层：Scene 视口内回调其 RenderSceneOverlay

/// 场景层 —— 只负责场景渲染（离屏视口 + 相机）与 文件操作（保存/加载/新建）。
///
/// 场景状态（Scene / 序列化器）放在共享的 EditorContext 中，编辑器导航相机
/// 由 EditorContext.EditorCamera 独立持有（工具视角，不进场景）。层级面板等由
/// 各自的 Layer 承载并共享该上下文。启动时从文件加载默认场景。
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

    /// 当前视口使用的相机：Play → 场景主玩法相机（无则回退编辑器相机）；Edit → 编辑器相机。
    /// 渲染、gizmo、包围盒/碰撞体线框叠加共用同一来源，保证叠加与画面一致。
    Camera &GetActiveViewCamera();

    /// 保存场景到文件（弹出文件对话框），供顶部菜单调用
    void SaveScene();

    /// 从文件加载场景（弹出文件对话框，会清空当前场景），供顶部菜单调用
    void LoadScene();

    /// 新建空场景（清空当前场景内容），供顶部菜单调用
    void NewScene();

    /// 从文件导入 glTF 场景（保留 node 层级与变换，导入到场景根），供顶部菜单调用
    void ImportGLTFScene();

    /// 重载当前场景内所有 Lua 脚本（Ctrl+R / 顶部菜单）
    void ReloadAllScripts();

    /// 绑定 gizmo 控制器（在 Scene 视口内渲染变换 gizmo）
    void SetGizmoController(std::unique_ptr<GizmoController> gizmo);

    /// 绑定调试线框叠加层（用于包围盒/碰撞体/第一人称视点标记）
    void SetDebugDrawLayer(DebugDrawLayer *debugDrawLayer);

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

    /// 最近一次 glTF 导入是否失败（在 OnImGuiRender 弹窗提示，一次性消费）
    bool m_GLTFImportFailed = false;

    /// 停靠目标 DockSpace ID（根上下文取 "MainDockspace"）
    ImGuiID m_DockSpaceID = 0;

    /// 鼠标是否悬停在 Scene 视口窗口内（上一帧 OnImGuiRender 记录，供 OnEvent 判断）
    bool m_SceneWindowHovered = false;

    /// 在视口内按下且尚未释放的鼠标按键位掩码（bit = 1 << MouseCode）。
    /// 用于把「拖出视口后松开」的释放事件仍回传相机，避免相机按键状态卡住。
    uint32_t m_ViewportCapturedButtons = 0;

    /// 调试线框叠加层（由 EditorApp 创建并 PushLayer，SceneLayer 只持有指针）
    DebugDrawLayer *m_DebugDrawLayer = nullptr;

    /// 从文件加载场景（会重建场景并重新绑定相机）
    bool LoadSceneFromFile(std::string_view filepath);

    /// 编辑器状态持久化：关闭时把编辑器相机 + 场景渲染设置写入状态文件（工作目录下）。
    void SaveEditorSettings();

    /// 编辑器状态持久化：启动时从状态文件恢复编辑器相机与渲染设置（无文件则保持默认）。
    void RestoreEditorSettings();

    /// 确保离屏渲染视口在本帧可用，并返回视口像素尺寸。
    /// 首帧无尺寸、创建/重建失败时返回 false。
    bool EnsureViewport(Timestep &ts, uint32_t &vpWidth, uint32_t &vpHeight);

    /// 选择本帧离屏渲染相机并同步宽高比（非固定纵横比的玩法相机）。
    Camera &GetRenderingViewCamera(float aspect);

    /// 向当前帧 RenderGraph 注册场景 3D/2D pass（前向/延迟渲染共用入口）。
    void RecordScenePasses(RenderTarget &viewportRT, const glm::vec4 &clearColor);

    /// 从代码程序化构建默认场景（编译期开关 GE_EDITOR_BUILD_SCENE_FROM_CODE 控制）
    void BuildDefaultSceneFromCode();

    /// Play 态跟随相机：锁定/解锁鼠标光标（GLFW_CURSOR_DISABLED），
    /// 并在切换瞬间重置 InputState 增量基准，防首帧 delta 爆值。
    /// 无 FollowCamera 组件的场景不锁定（保持编辑器鼠标自由）。
    void UpdateMouseCapture();

    /// 当前是否已锁定鼠标（防重复 glfwSetInputMode）
    bool m_MouseCaptured = false;
};

} // namespace GE
