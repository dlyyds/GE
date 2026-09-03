#pragma once

#include "GE/Core/Base.h"

namespace GE {

/// 渲染统计面板 —— 显示引擎每帧 draw call / 三角形 / FPS。
///
/// draw call 数据源为 Renderer 帧统计（Renderer::GetStats）；
/// FPS 为宿主运行时数据，直接读 Application::GetFPS()。
/// 面板不拥有任何资源，仅做只读展示。
class RenderStatsPanel {
public:
    RenderStatsPanel() = default;

    /// 每帧 ImGui 渲染（停靠进主 DockSpace，与 DockSpaceLayer 一致）
    void OnImGuiRender();
};

} // namespace GE
