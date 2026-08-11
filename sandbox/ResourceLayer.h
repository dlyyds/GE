#pragma once

#include "GE/GE.h"

#include "Panels/ResourcePanel.h"

namespace GE {

/// 资源面板层 —— 独立承载 ResourcePanel（"Resource" 窗口）。
/// 自包含只读展示全局 纹理/材质/网格 资源，无需共享场景上下文。
class ResourceLayer : public Layer {
public:
    ResourceLayer();

    ~ResourceLayer() override;

    void OnAttach() override;

    void OnDetach() override;

    void OnUpdate(Timestep &ts) override;

    void OnEvent(Event &event) override;

    void OnImGuiRender() override;

private:
    ResourcePanel m_Panel;  ///< 资源面板（ImGui）
};

} // namespace GE