#pragma once

#include "GE/GE.h"


#include <d3d11.h>
#include <winrt/base.h>

namespace GE {
class SandboxLayer : public Layer {
public:
    SandboxLayer();

    virtual ~SandboxLayer() = default;

    virtual void OnAttach() override;

    virtual void OnDetach() override;

    void OnUpdate(GE::Timestep &ts) override;

    virtual void OnImGuiRender() override;

    void OnEvent(GE::Event &e) override;

private:
    bool OnKeyPressed(KeyPressedEvent &e);

    bool OnWindowResize(WindowResizeEvent &e);

    uint32_t m_Width, m_Height;

};
}
