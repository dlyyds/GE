#include "Core/GEWindow.h"
#include "pch.h"

#ifdef GE_PLATFORM_WINDOWS
#    include "Platform/Windows/WindowsWindow.h"
#endif

namespace GE {

Scope<Window> Window::Create(const WindowProps &props) {
    GE_PROFILE_FUNCTION();

#ifdef GE_PLATFORM_WINDOWS
    return CreateScope<WindowsWindow>(props);
#else
    GE_CORE_ASSERT(false, "Unknown platform!");
    return nullptr;
#endif
}

} // namespace GE