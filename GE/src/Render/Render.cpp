
#include <Render/Render.h>
#include <bx/bx.h>
#include <bgfx/bgfx.h>


namespace GE {

Render::RenderAPI Render::API = OPEN_GL;


constexpr bgfx::ViewId kClearView = 0;

void Render::Init() {
    bgfx::renderFrame();
    bgfx::Init init;
    init.platformData.nwh = Application::Get().GetWindow().GetNativeWindow();
    init.resolution.width = Application::Get().GetWindow().GetWidth();
    init.resolution.height = Application::Get().GetWindow().GetHeight();
    init.resolution.reset = BGFX_RESET_VSYNC;
    init.type = bgfx::RendererType::OpenGL;
    GE_ASSERT(bgfx::init(init));
    bgfx::setViewClear(kClearView, BGFX_CLEAR_COLOR);
    bgfx::setViewRect(kClearView, 0, 0, bgfx::BackbufferRatio::Equal);
}

void Render::Destroy() {
    bgfx::shutdown();
}


}

