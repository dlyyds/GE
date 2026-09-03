#pragma once

#include "GE/GE.h"

#include "Panels/RenderStatsPanel.h"

namespace GE {

/// 渲染统计层 —— 独立承载"渲染统计"窗口（Renderer 帧统计只读展示）。
class StatsLayer : public Layer {
public:
    StatsLayer();

    ~StatsLayer() override;

    void OnAttach() override;

    void OnDetach() override;

    void OnUpdate(Timestep &ts) override;

    void OnEvent(Event &event) override;

    void OnImGuiRender() override;

private:
    RenderStatsPanel m_Panel; ///< 渲染统计面板（ImGui）
};

} // namespace GE
