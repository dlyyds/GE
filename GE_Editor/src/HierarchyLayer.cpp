//
// 场景层级面板层实现 —— 独立承载 "Scene Hierarchy" + "Properties" 两个 ImGui 窗口。
//

#include "HierarchyLayer.h"

namespace GE {

HierarchyLayer::HierarchyLayer(std::shared_ptr<EditorContext> context) : Layer("HierarchyLayer"), m_Context(std::move(context)) {
}

HierarchyLayer::~HierarchyLayer() = default;

void HierarchyLayer::OnAttach() {
}

void HierarchyLayer::OnDetach() {
    // 解绑面板，避免场景销毁后悬空
    m_Panel.SetContext(nullptr);
}

void HierarchyLayer::OnUpdate(Timestep &ts) {
}

void HierarchyLayer::OnEvent(Event &event) {
}

void HierarchyLayer::OnImGuiRender() {
    // 场景对象被新建/加载替换时，重新绑定面板并清空选中（SetContext 内部已清空选中）
    if (m_Context->Scene.get() != m_LastScene) {
        m_Panel.SetContext(m_Context->Scene.get());
        m_LastScene = m_Context->Scene.get();
    }
    m_Panel.OnImGuiRender();
}

} // namespace GE