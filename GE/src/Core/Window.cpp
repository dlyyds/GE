#include "Core/GEWindow.h"
#include "pch.h"

#ifdef GE_PLATFORM_WINDOWS
#    include "Platform/Windows/GlfwWindow.h"
#endif

namespace GE {

std::unique_ptr<Window> Window::Create(const WindowProperties &props) {
    GE_PROFILE_FUNCTION();

#ifdef GE_PLATFORM_WINDOWS
    return std::make_unique<GlfwWindow>(props);
#else
    GE_CORE_ASSERT(false, "Unknown platform!");
    return nullptr;
#endif
}

} // namespace GE