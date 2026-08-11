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

// 会话：连接/断开 Tracy 服务器。
// Tracy 没有“会话”概念，整个应用生命周期只需一个连接，
// 因此仅在程序启动/退出时调用一次。
// 注意：StartupProfiler/ShutdownProfiler 仅在 TRACY_ENABLE 下才有声明，
// 需单独判断（其余作用域/帧宏在关闭时 Tracy 会自行展开为空操作）。
#ifdef TRACY_ENABLE
#    define GE_PROFILE_BEGIN_SESSION() tracy::StartupProfiler()
#    define GE_PROFILE_END_SESSION() tracy::ShutdownProfiler()
#else
#    define GE_PROFILE_BEGIN_SESSION()
#    define GE_PROFILE_END_SESSION()
#endif

// 作用域：带自定义名字。
#define GE_PROFILE_SCOPE(name) ZoneScopedN(name)

// 作用域：自动取所在函数名。
#define GE_PROFILE_FUNCTION() ZoneScoped

// 帧标记：用于 Tracy 的帧时间统计。
#define GE_PROFILE_FRAME_MARK() FrameMark