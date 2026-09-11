#pragma once

#include "GE/Core/Layer.h"
#include "GE/Render/Camera.h"

#include "GameConfig.h"

#include <memory>

namespace GE {

class Scene;

/**
 * @brief 运行时游戏层：加载入口场景并驱动仿真 + 渲染，无任何编辑器 UI。
 *
 * 渲染复用引擎共享的场景 pass 链（RecordScenePasses），颜色输出直接是 swapchain
 * 背缓冲（Renderer::SetSceneToBackbuffer(true) 让末尾 UIPass 改用 eLoad），因此画面
 * 与编辑器 Play 态同源、不会两侧漂移。物理 / 脚本 / 音频靠 Scene::Play() 进入模拟态。
 */
class GameLayer : public Layer {
public:
    explicit GameLayer(GameConfig config);

    ~GameLayer() override;

    void OnAttach() override;

    void OnDetach() override;

    void OnUpdate(Timestep &ts) override;

    void OnEvent(Event &event) override;

    void OnImGuiRender() override {}   ///< 运行时无 UI（ImGui 帧照常构建，draw data 为空）

private:
    /// 选择本帧渲染相机并同步宽高比：场景主相机优先，无则内置自由视角。
    Camera &ActiveCamera(float aspect);

    /// Play 态跟随相机：锁定/解锁鼠标光标，并在切换瞬间重置输入增量基准，防首帧 delta 爆值。
    /// 场景没有跟随相机角色时不锁定（保持鼠标自由）。
    void UpdateMouseCapture();

    /// 应用窗口配置：标题 / 尺寸 / 垂直同步 / 全屏。
    void ApplyWindowConfig();

    /// 应用渲染开关（只覆盖 game.cfg 显式写了的项）。
    void ApplyRenderingConfig();

    GameConfig m_Config;
    std::unique_ptr<Scene> m_Scene;
    Camera m_FallbackCamera;   ///< 场景没有主相机时的兜底自由视角
    bool m_MouseCaptured = false;
};

} // namespace GE
