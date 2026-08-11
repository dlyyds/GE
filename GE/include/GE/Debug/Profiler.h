#pragma once

// Tracy 性能分析的统一封装。
//
// 引擎源码统一使用 GE_PROFILE_* 宏，不直接依赖 Tracy 的宏名，
// 便于日后替换分析库或统一开关。
//
// 两层开关叠加生效：
//   - GE_PROFILE_ENABLE（本层）：置 0 时 GE_PROFILE_* 全部展开为空操作。
//     默认 1，可在编译期用 -DGE_PROFILE_ENABLE=0 覆盖。
//   - TRACY_ENABLE（Tracy 层）：未定义时 Tracy 自身的宏即失效，
//     本封装随之失效，无需额外判断。

#include <tracy/Tracy.hpp>

#ifndef GE_PROFILE_ENABLE
#    define GE_PROFILE_ENABLE 1
#endif

#if GE_PROFILE_ENABLE
// 作用域：带自定义名字。
#    define GE_PROFILE_SCOPE(name) ZoneScopedN(name)

// 作用域：自动取所在函数名。
#    define GE_PROFILE_FUNCTION() ZoneScoped

// 帧标记：用于 Tracy 的帧时间统计。
#    define GE_PROFILE_FRAME_MARK() FrameMark
#else
#    define GE_PROFILE_SCOPE(name)
#    define GE_PROFILE_FUNCTION()
#    define GE_PROFILE_FRAME_MARK()
#endif