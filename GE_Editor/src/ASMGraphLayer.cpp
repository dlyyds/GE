//
// 动画状态机节点图面板层实现 —— 独立承载 "动画状态机图" ImGui 窗口。
//

#include "ASMGraphLayer.h"

#include "HierarchyLayer.h"

namespace GE {

ASMGraphLayer::ASMGraphLayer(std::shared_ptr<EditorContext> context, HierarchyLayer *hierarchy)
    : Layer("ASMGraphLayer"), m_Context(std::move(context)),
      m_Panel(m_Context, hierarchy) {
}

ASMGraphLayer::~ASMGraphLayer() = default;

void ASMGraphLayer::OnAttach() {
}

void ASMGraphLayer::OnDetach() {
}

void ASMGraphLayer::OnUpdate(Timestep &ts) {
}

void ASMGraphLayer::OnEvent(Event &event) {
}

void ASMGraphLayer::OnImGuiRender() {
    m_Panel.OnImGuiRender();
}

} // namespace GE
