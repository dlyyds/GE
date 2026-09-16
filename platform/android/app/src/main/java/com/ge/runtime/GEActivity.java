package com.ge.runtime;

import org.libsdl.app.SDLActivity;

/**
 * GE_Runtime 的 Android Activity。
 *
 * 唯一的职责是告诉 SDL 该加载哪个 .so。**必须覆写 getLibraries()**，因为本工程把
 * SDL 静态链进了自己的库（CMake 里 SDL_SHARED=OFF / SDL_STATIC=ON），而 SDL 的
 * 默认实现返回 {"SDL3", …, "main"}，那套默认值只适用于"SDL 也编成 .so"的情形：
 *
 *   1. 它先 System.loadLibrary("SDL3") —— APK 里没有 libSDL3.so，必然失败；
 *   2. SDL.loadLibrary 对 UnsatisfiedLinkError 是**抛出**而不是忽略，
 *      loadLibraries() 的 for 循环因此在**第一个元素就中断**，"main" 从未被加载；
 *   3. SDLActivity 于是判定 mBrokenLibraries=true，走"库损坏"分支，
 *      **永远不启动那个调用 native main 的线程**。
 *
 * 症状极具误导性：Activity 正常显示、进程一直活着、logcat 里只有一条被 catch 掉的
 * "dlopen failed: library libSDL3.so not found" 警告，看起来像"启动后自己退了"。
 * 桌面构建与静态审查都测不到这一条 —— 只有在设备/模拟器上跑才会现形。
 *
 * 覆写成只加载 "main"，即 APK 里的 libmain.so（见 CMakeLists 把
 * GE_Runtime 的 OUTPUT_NAME 设为 main）。这也是 SDL 对"静态链接 SDL"的既定做法，
 * AndroidManifest.xml 里就写着这段指引。
 */
public class GEActivity extends SDLActivity {
    @Override
    protected String[] getLibraries() {
        // 只列自己那一个库：SDL 已静态链进 libmain.so，不需要（也没有）libSDL3.so。
        return new String[] { "main" };
    }
}
