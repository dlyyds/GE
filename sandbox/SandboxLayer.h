#pragma once

#include "GE/GE.h"
#include "Render/Material.h"
#include "Render/Mesh.h"
#include "Render/Camera.h"
#include "bgfx/bgfx.h"
#include "bx/bx.h"
#include "bx/timer.h"
#include "Render/Program.h"

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

    void DebugText();

    uint32_t m_Width{}, m_Height{};

    // Colored cube
    Mesh m_Mesh;
    Material m_Material;
    Camera m_Camera;

    float m_Color[4] = {0.8f, 0.2f, 0.2f, 1.0f};


};
}