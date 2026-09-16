#include "Core/GEWindow.h"
#include "pch.h"

#include "Platform/SdlWindow.h"

namespace GE {

std::unique_ptr<Window> Window::Create(const WindowProperties &props) {
    GE_PROFILE_FUNCTION();

    // 不再需要平台分支：SdlWindow 一份实现覆盖桌面与 Android。
    // 原先这里是 `#ifdef GE_PLATFORM_WINDOWS → GlfwWindow #else 断言`。
    return std::make_unique<SdlWindow>(props);
}

} // namespace GE
