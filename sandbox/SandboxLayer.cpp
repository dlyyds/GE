#include "SandboxLayer.h"

#include <bx/bx.h>
#include <bgfx/bgfx.h>
#include "logo.h"
#include "imgui.h"


namespace GE {

static bool s_showStats = false;


SandboxLayer::SandboxLayer() : Layer("SandboxLayer") {
    GE_PROFILE_FUNCTION();

}


void SandboxLayer::OnAttach() {
    GE_PROFILE_FUNCTION();
    m_Width = Application::Get().GetWindow().GetWidth();
    m_Height = Application::Get().GetWindow().GetHeight();
}

void SandboxLayer::OnDetach() {

}

constexpr bgfx::ViewId kClearView = 0;

void SandboxLayer::OnUpdate(GE::Timestep &ts) {
    bgfx::touch(kClearView);
    bgfx::dbgTextClear();
    bgfx::dbgTextImage(bx::max<uint16_t>(uint16_t(m_Width / 2 / 8), 20) - 20, bx::max<uint16_t>(uint16_t(m_Height / 2 / 16), 6) - 6, 40, 12, s_logo,
                       160);
    bgfx::dbgTextPrintf(0, 0, 0x0f, "Press F1 to toggle stats.");
    bgfx::dbgTextPrintf(0, 1, 0x0f, "Color can be changed with ANSI \x1b[9;me\x1b[10;ms\x1b[11;mc\x1b[12;ma\x1b[13;mp\x1b[14;me\x1b[0m code too.");
    bgfx::dbgTextPrintf(80, 1, 0x0f, "\x1b[;0m    \x1b[;1m    \x1b[; 2m    \x1b[; 3m    \x1b[; 4m    \x1b[; 5m    \x1b[; 6m    \x1b[; 7m    \x1b[0m");
    bgfx::dbgTextPrintf(80, 2, 0x0f, "\x1b[;8m    \x1b[;9m    \x1b[;10m    \x1b[;11m    \x1b[;12m    \x1b[;13m    \x1b[;14m    \x1b[;15m    \x1b[0m");
    const bgfx::Stats *stats = bgfx::getStats();
    bgfx::dbgTextPrintf(0, 2, 0x0f, "Backbuffer %dW x %dH in pixels, debug text %dW x %dH in characters.", stats->width, stats->height,
                        stats->textWidth, stats->textHeight);
    bgfx::setDebug(s_showStats ? BGFX_DEBUG_STATS : BGFX_DEBUG_TEXT);

    // imgui 帧

    bgfx::frame();
}

void SandboxLayer::OnImGuiRender() {

    ImGui::Begin("Sandbox Layer");
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
    bgfx::setViewRect(kClearView, 0, 0, bgfx::BackbufferRatio::Equal);
    return false;
}
}
