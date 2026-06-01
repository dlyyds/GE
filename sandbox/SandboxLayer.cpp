#include "SandboxLayer.h"

#include <fstream>
#include <bx/bx.h>
#include <bgfx/bgfx.h>
#include "logo.h"
#include "imgui.h"
#include <bx/math.h>
#include <bx/allocator.h>
#include <bimg/decode.h>


namespace GE {

struct PosColorVertex {
    float m_x;
    float m_y;
    float m_z;
    uint32_t m_abgr;

    static void init() {
        ms_layout
            .begin()
            .add(bgfx::Attrib::Position, 3, bgfx::AttribType::Float)
            .add(bgfx::Attrib::Color0, 4, bgfx::AttribType::Uint8, true)
            .end();
    };

    static bgfx::VertexLayout ms_layout;
};

bgfx::VertexLayout PosColorVertex::ms_layout;

static PosColorVertex s_cubeVertices[] =
{
    {-1.0f, 1.0f, 1.0f, 0xff000000},
    {1.0f, 1.0f, 1.0f, 0xff0000ff},
    {-1.0f, -1.0f, 1.0f, 0xff00ff00},
    {1.0f, -1.0f, 1.0f, 0xff00ffff},
    {-1.0f, 1.0f, -1.0f, 0xffff0000},
    {1.0f, 1.0f, -1.0f, 0xffff00ff},
    {-1.0f, -1.0f, -1.0f, 0xffffff00},
    {1.0f, -1.0f, -1.0f, 0xffffffff},
};

static const uint16_t s_cubeTriList[] =
{
    0, 1, 2, // 0
    1, 3, 2,
    4, 6, 5, // 2
    5, 6, 7,
    0, 2, 4, // 4
    4, 2, 6,
    1, 5, 3, // 6
    5, 7, 3,
    0, 4, 1, // 8
    4, 5, 1,
    2, 3, 6, // 10
    6, 3, 7,
};

static const uint16_t s_cubeLineList[] =
{
    0, 1,
    0, 2,
    0, 4,
    1, 3,
    1, 5,
    2, 3,
    2, 6,
    3, 7,
    4, 5,
    4, 6,
    5, 7,
    6, 7,
};


static bool s_showStats = false;


SandboxLayer::SandboxLayer() : Layer("SandboxLayer") {
    GE_PROFILE_FUNCTION();

}

constexpr bgfx::ViewId kClearView = 0;


void SandboxLayer::OnAttach() {
    GE_PROFILE_FUNCTION();
    m_Width = Application::Get().GetWindow().GetWidth();
    m_Height = Application::Get().GetWindow().GetHeight();

    bgfx::setViewClear(1, BGFX_CLEAR_COLOR);
    bgfx::setViewRect(1, 0, 0, bgfx::BackbufferRatio::Equal);

    bgfx::setViewClear(kClearView
                       , BGFX_CLEAR_COLOR | BGFX_CLEAR_DEPTH
                       , 0x303030ff
                       , 1.0f
                       , 0
        );
    PosColorVertex::init();

    bgfx::setViewRect(kClearView, 0, 0, bgfx::BackbufferRatio::Equal);

    m_Mesh.SetVertices(s_cubeVertices, sizeof(s_cubeVertices), PosColorVertex::ms_layout);
    m_Mesh.SetIndices(s_cubeTriList, sizeof(s_cubeTriList));

    m_Material.CreateUniform("u_color", bgfx::UniformType::Vec4);
    Program program;
    program.Load("vs_cubes.bin", "fs_cubes.bin");
    m_Material.SetProgram(std::move(program));

    m_Camera.SetPerspective(60.0f, (float)m_Width / (float)m_Height, 0.1f, 100.0f);
    m_Camera.LookAt({0.0f, 0.0f, -35.0f}, {0.0f, 0.0f, 0.0f});
    // --- Texture quad ---

}

void SandboxLayer::OnDetach() {

}

void SandboxLayer::DebugText() {
    bgfx::dbgTextClear();
    bgfx::dbgTextPrintf(0, 0, 0x0f, "Press F1 to toggle stats.");
    bgfx::dbgTextPrintf(0, 1, 0x0f, "Color can be changed with ANSI \x1b[9;me\x1b[10;ms\x1b[11;mc\x1b[12;ma\x1b[13;mp\x1b[14;me\x1b[0m code too.");
    bgfx::dbgTextPrintf(80, 1, 0x0f, "\x1b[;0m    \x1b[;1m    \x1b[; 2m    \x1b[; 3m    \x1b[; 4m    \x1b[; 5m    \x1b[; 6m    \x1b[; 7m    \x1b[0m");
    bgfx::dbgTextPrintf(80, 2, 0x0f, "\x1b[;8m    \x1b[;9m    \x1b[;10m    \x1b[;11m    \x1b[;12m    \x1b[;13m    \x1b[;14m    \x1b[;15m    \x1b[0m");
    const bgfx::Stats *stats = bgfx::getStats();
    bgfx::dbgTextPrintf(0, 2, 0x0f, "Backbuffer %dW x %dH in pixels, debug text %dW x %dH in characters.", stats->width, stats->height,
                        stats->textWidth, stats->textHeight);
}

void SandboxLayer::OnUpdate(GE::Timestep &ts) {
    //bgfx::touch(1);
    bgfx::touch(kClearView);
    DebugText();
    bgfx::setDebug(s_showStats ? BGFX_DEBUG_STATS : BGFX_DEBUG_TEXT);

    m_Camera.ApplyToView(0);
    bgfx::setViewRect(0, 0, 0, bgfx::BackbufferRatio::Equal);

    float mtx[16];
    static float time = 0.0f;
    time += ts;

    float rot[16], scale[16];
    bx::mtxRotateXY(rot, time * 0.21f, time * 0.37f);
    bx::mtxScale(scale, 5.0f);

    bx::mtxMul(mtx, scale, rot);
    bgfx::setTransform(mtx);

    m_Mesh.Bind();

    // Set color uniform

    m_Material.SetParam("u_color", m_Color);

    // Set render states.
    bgfx::setState(m_Material.m_state);

    m_Material.Commit();
    // Submit primitive for rendering to view 0.
    bgfx::submit(0, m_Material.m_program.GetHandle());

    bgfx::frame();
}

void SandboxLayer::OnImGuiRender() {

    ImGui::Begin("Sandbox Layer");
    ImGui::ColorEdit4("Cube Color", m_Color);
    ImGui::TextColored(ImVec4(1, 0, 0, 1), "Sandbox");
    ImGui::End();

    ImGui::Begin("Sandbox Layer1");
    ImGui::TextColored(ImVec4(0, 1, 1, 1), "Hello word");
    ImGui::End();

}

void SandboxLayer::OnEvent(Event &e) {
    GE_PROFILE_FUNCTION();
    EventDispatcher dispatcher(e);
    dispatcher.Dispatch<KeyPressedEvent>(GE_BIND_EVENT_FN(SandboxLayer::OnKeyPressed));
    dispatcher.Dispatch<WindowResizeEvent>(GE_BIND_EVENT_FN(SandboxLayer::OnWindowResize));
}

bool SandboxLayer::OnKeyPressed(KeyPressedEvent &e) {
    if (e.GetKeyCode() == Key::F1)
        s_showStats = !s_showStats;
    return false;
}

bool SandboxLayer::OnWindowResize(WindowResizeEvent &e) {
    m_Width = e.GetWidth();
    m_Height = e.GetHeight();
    bgfx::reset(m_Width, m_Height, BGFX_RESET_VSYNC);

    bgfx::setViewRect(1, 0, 0, bgfx::BackbufferRatio::Equal);
    bgfx::setViewRect(kClearView, 0, 0, bgfx::BackbufferRatio::Equal);

    m_Camera.SetPerspective(60.0f, (float)m_Width / (float)m_Height, 0.1f, 100.0f);
    m_Camera.LookAt({0.0f, 0.0f, -35.0f}, {0.0f, 0.0f, 0.0f});
    return false;
}
}
