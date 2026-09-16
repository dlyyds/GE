#pragma once

#include "GE/Core/Layer.h"
#include "GE/Render/Camera.h"

#include "GameConfig.h"
#include "TouchControls.h"

#include <memory>

namespace GE {

class Scene;

/**
 * @brief 运行时游戏层：加载入口场景并驱动仿真 + 渲染，无编辑器 UI。
 *
 * 渲染复用引擎共享的场景 pass 链（RecordScenePasses），颜色输出直接是 swapchain
 * 背缓冲（Renderer::SetSceneToBackbuffer(true) 让末尾 UIPass 改用 eLoad），因此画面
 * 与编辑器 Play 态同源、不会两侧漂移。物理 / 脚本 / 音频靠 Scene::Play() 进入模拟态。
 *
 * 触屏操作由 TouchControls 承担（Android 上唯一可用的操控方式）；它合成的是引擎既有
 * 的键/鼠标事件，喂进与真实输入完全相同的那条转发路径。
 */
class GameLayer : public Layer {
public:
    explicit GameLayer(GameConfig config);

    ~GameLayer() override;

    void OnAttach() override;

    void OnDetach() override;

    void OnUpdate(Timestep &ts) override;

    void OnEvent(Event &event) override;

    /// 运行时唯一的 ImGui 绘制入口：目前只画触屏摇杆
    void OnImGuiRender() override;

private:
    /// 选择本帧渲染相机并同步宽高比：场景主相机优先，无则内置自由视角。
    Camera &ActiveCamera(float aspect);

    /// Play 态跟随相机：锁定/解锁鼠标光标，并在切换瞬间重置输入增量基准，防首帧 delta 爆值。
    /// 场景没有跟随相机角色时不锁定（保持鼠标自由）。
    void UpdateMouseCapture();

    /// 把（真实的或触摸合成的）输入事件交给场景与兜底相机 —— 两条来源共用这一条路径，
    /// 否则"自由视角兜底"的分支条件会在两处各写一遍、迟早漂移。
    void DispatchInputToScene(Event &event);

    /// 本帧是否走"没有主相机"的兜底自由视角路径（触屏据此决定要不要合成左键，见 TouchControls）。
    bool UsesFallbackCamera();

    /// 画触屏摇杆（无触摸时什么都不画，桌面因此零影响）。
    void DrawTouchOverlay();

    /// 应用窗口配置：标题 / 尺寸 / 垂直同步 / 全屏。
    void ApplyWindowConfig();

    /// 应用渲染开关（只覆盖 game.cfg 显式写了的项）。
    void ApplyRenderingConfig();

    GameConfig m_Config;
    std::unique_ptr<Scene> m_Scene;
    Camera m_FallbackCamera;   ///< 场景没有主相机时的兜底自由视角
    bool m_MouseCaptured = false;

    /// 声明在 m_Scene **之后**：它持有 Scene 内 InputState 的引用，靠"后声明先析构"
    /// 保证它在 Scene 之前销毁。
    std::unique_ptr<TouchControls> m_Touch;
};

} // namespace GE

