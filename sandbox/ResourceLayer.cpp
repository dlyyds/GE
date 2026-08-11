//
// 资源面板层实现 —— 独立承载 "Resource" ImGui 窗口。
//

#include "ResourceLayer.h"

namespace GE {

ResourceLayer::ResourceLayer() : Layer("ResourceLayer") {
}

ResourceLayer::~ResourceLayer() = default;

void ResourceLayer::OnAttach() {
}

void ResourceLayer::OnDetach() {
}

void ResourceLayer::OnUpdate(Timestep &ts) {
}

void ResourceLayer::OnEvent(Event &event) {
}

void ResourceLayer::OnImGuiRender() {
    m_Panel.OnImGuiRender();
}

} // namespace GE