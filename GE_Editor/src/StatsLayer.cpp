//
// 渲染统计层实现 —— 独立承载 "渲染统计" ImGui 窗口。
//

#include "StatsLayer.h"

namespace GE {

StatsLayer::StatsLayer() : Layer("StatsLayer") {
}

StatsLayer::~StatsLayer() = default;

void StatsLayer::OnAttach() {
}

void StatsLayer::OnDetach() {
}

void StatsLayer::OnUpdate(Timestep &ts) {
}

void StatsLayer::OnEvent(Event &event) {
}

void StatsLayer::OnImGuiRender() {
    m_Panel.OnImGuiRender();
}

} // namespace GE
