#pragma once

#include "memory"
#include <cstdint>

#include "PlatformDetection.h"


#define BIT(x) (1 << x)


#ifdef GE_DEBUG
#if defined(GE_PLATFORM_WINDOWS)
#define GE_DEBUGBREAK() __debugbreak()
#elif defined(GE_PLATFORM_LINUX)
#include <signal.h>
#define GE_DEBUGBREAK() raise(SIGTRAP)
#else
#error "Platform doesn't support debugbreak yet!"
#endif
#define GE_ENABLE_ASSERTS
#else
#define GE_DEBUGBREAK()
#endif

#define GE_EXPAND_MACRO(x) x
#define GE_STRINGIFY_MACRO(x) #x


#ifdef GE_PLATFORM_WINDOWS
#    include <Windows.h>
#endif

#define GE_BIND_EVENT_FN(fn) [this](auto&&... args) -> decltype(auto) \
{ return this->fn(std::forward<decltype(args)>(args)...); }

namespace GE {


} // namespace GE