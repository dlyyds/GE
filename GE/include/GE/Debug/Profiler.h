#pragma once

// Tracy 性能分析的统一封装。
//
// 引擎源码统一使用 GE_PROFILE_* 宏，不直接依赖 Tracy 的宏名，
// 便于日后替换分析库或统一开关。
//
// 是否生效由 Tracy 自身的编译期宏 TRACY_ENABLE 控制：
// 未定义 TRACY_ENABLE 时，Tracy 会把所有宏展开为空操作，本封装随之失效，
// 无需额外判断。

#include <tracy/Tracy.hpp>

// 作用域：带自定义名字。
#define GE_PROFILE_SCOPE(name) ZoneScopedN(name)

// 作用域：自动取所在函数名。
#define GE_PROFILE_FUNCTION() ZoneScoped

// 帧标记：用于 Tracy 的帧时间统计。
#define GE_PROFILE_FRAME_MARK() FrameMark