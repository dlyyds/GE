//
// 资源面板层实现 —— 独立承载 "Resource" ImGui 窗口。
//

#include "ResourceLayer.h"

namespace GE {

ResourceLayer::ResourceLayer(std::shared_ptr<EditorContext> context)
    : Layer("ResourceLayer"), m_Context(std::move(context)) {
}

ResourceLayer::~ResourceLayer() = default;

void ResourceLayer::OnAttach() {
}

void ResourceLayer::OnDetach() {
    // 解绑面板，避免场景销毁后悬空
    m_Panel.SetContext(nullptr);
}

void ResourceLayer::OnUpdate(Timestep &ts) {
}

void ResourceLayer::OnEvent(Event &event) {
}

void ResourceLayer::OnImGuiRender() {
    // 场景对象被新建/加载替换时重新绑定（面板只做只读遍历，无需清选中态）
    if (m_Context->Scene.get() != m_LastScene) {
        m_Panel.SetContext(m_Context->Scene.get());
        m_LastScene = m_Context->Scene.get();
    }
    m_Panel.OnImGuiRender();
}

} // namespace GE
