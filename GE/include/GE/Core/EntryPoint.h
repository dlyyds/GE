#pragma once

#include "Application.h"
#include "Debug/Profiler.h"
#include "Log.h"

// SDL 的入口点机制。**只有本头文件可以 include 它** —— SDL_main_impl.h 里是把
// main / WinMain 的**定义**直接写在头里的，多个 TU 同时引入会砸出重复符号。
// 目前只有 GE_Editor / GE_Runtime 各自的入口 TU 引入本头，各成一个可执行文件，
// 满足"全程序恰好一个 main"。
//
// 两端如何落地（见 SDL_main_impl.h）：
//   - Win32：SDL 同时提供 main（控制台子系统）与 WinMain（窗口子系统），
//     按链接子系统自动生效，两者都转调被重命名的 SDL_main；
//   - Android：`main` 只是转调 SDL_RunApp，且 SDL 额外导出 SDL_main 供
//     SDLActivity 从 Java 侧调起。
// 因此**不需要** android_native_app_glue，也不再需要原先的 `#ifdef _WIN64` 守卫。
#include <SDL3/SDL_main.h>

int main(int argc, char **argv) {

    GE::Log::Init();

    auto app = GE::CreateApplication({argc, argv});
    app->Run();
    delete app;

    return 0;
}
