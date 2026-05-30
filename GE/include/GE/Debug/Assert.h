#pragma once

#include "Core/Base.h"
#include "Core/Log.h"

#define GE_ENABLE_ASSERTS

#ifdef GE_ENABLE_ASSERTS

namespace GE::Assert {
// Returns the simple file name rather than full path as suggested by LovelySanta
constexpr const char *CurrentFileName(const char *path) {
    const char *file = path;
    while (*path) {
        if (*path == '/' || *path == '\\')
            file = ++path;
        else
            path++;
    }
    return file;
}
}

// Alteratively we could use the same "default" message for both "WITH_MSG" and "NO_MSG" and
// provide support for custom formatting by concatenating the formatting string instead of having the format inside the default message
#define GE_INTERNAL_ASSERT_IMPL(type, check, msg, ...) { if(!(check)) { GE##type##ERROR(msg, __VA_ARGS__); GE_DEBUGBREAK(); } }
#define GE_INTERNAL_ASSERT_WITH_MSG(type, check, ...) GE_INTERNAL_ASSERT_IMPL(type, check, "Assertion failed: {0}", __VA_ARGS__)
#define GE_INTERNAL_ASSERT_NO_MSG(type, check) GE_INTERNAL_ASSERT_IMPL(type, check, "Assertion '{0}' failed at {1}:{2}", GE_STRINGIFY_MACRO(check), ::GE::Assert::CurrentFileName(__FILE__), __LINE__)

#define GE_INTERNAL_ASSERT_GET_MACRO_NAME(arg1, arg2, macro, ...) macro
#define GE_INTERNAL_ASSERT_GET_MACRO(...) GE_EXPAND_MACRO( GE_INTERNAL_ASSERT_GET_MACRO_NAME(__VA_ARGS__, GE_INTERNAL_ASSERT_WITH_MSG, GE_INTERNAL_ASSERT_NO_MSG) )

// Currently accepts at least the condition and one additional parameter (the message) being optional
#define GE_ASSERT(...) GE_EXPAND_MACRO( GE_INTERNAL_ASSERT_GET_MACRO(__VA_ARGS__)(_, __VA_ARGS__) )
#define GE_CORE_ASSERT(...) GE_EXPAND_MACRO( GE_INTERNAL_ASSERT_GET_MACRO(__VA_ARGS__)(_CORE_, __VA_ARGS__) )
#else
#define GE_ASSERT(...)
#define GE_CORE_ASSERT(...)
#endif