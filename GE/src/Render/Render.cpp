
#include <Render/Render.h>
#include <bgfx/bgfx.h>


namespace GE {

Render::RenderAPI Render::API = OPEN_GL;


void Render::Init() {
    bgfx::renderFrame();
    bgfx::Init init;

    init.type = bgfx::RendererType::OpenGL;
    init.vendorId = BGFX_PCI_ID_NONE;
    init.platformData.nwh = Application::Get().GetWindow().GetNativeWindow();
    init.platformData.ndt = nullptr; //use in Linux
    init.platformData.type = bgfx::NativeWindowHandleType::Default;
    init.resolution.width = Application::Get().GetWindow().GetWidth();
    init.resolution.height = Application::Get().GetWindow().GetHeight();
    init.resolution.reset = BGFX_RESET_VSYNC;

    GE_ASSERT(bgfx::init(init));

}

void Render::Destroy() {
    bgfx::shutdown();
}


}

