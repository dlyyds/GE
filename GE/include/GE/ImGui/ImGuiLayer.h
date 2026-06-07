#pragma once

#include "Core/Base.h"
#include "Core/Layer.h"

#include "Events/ApplicationEvent.h"
#include "Events/KeyEvent.h"
#include "Events/MouseEvent.h"

namespace GE {

class ImGuiLayer : public Layer {
public:
    ImGuiLayer();

    ~ImGuiLayer() override;

    void OnAttach() override;

    void OnDetach() override;

    void OnUpdate(Timestep &ts) override {
    }

    void OnImGuiRender() override;

    void OnEvent(Event &e) override;

    static void Begin();

    static void End();

    void BlockEvents(const bool block) { m_BlockEvents = block; }

    static void SetDarkThemeColors();

private:
    bool m_BlockEvents = false;
};

} // namespace GE
