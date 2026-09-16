# Android 移植计划书

> 状态：**阶段 A（桌面 GLFW→SDL3）代码完成、构建通过，待用户跑完 `GE_Runtime` 的转向/相机回归；阶段 B（构建骨架）完成 —— 已产出可安装 APK 并逐项验证产物；阶段 D 的运行时部分 + 阶段 G 的资产入包已落地（M2.5，代码完成待真机验证，见 §6.0.1）；阶段 C / E / F 未开工**
> 目标：让 `GE_Runtime`（发行版播放器）能作为 APK 装在 Android 设备上运行。
> **窗口库决策（2026-09-16 定）：换掉 GLFW，桌面与 Android 统一走 SDL3。** 理由见 §1.3。原「保留 GLFW + 手写 Android 后端」方案已否决——它把 Android 特有的 glue / 生命周期 / 触摸 / IME 全部留给手写，那部分是写一次、无人测、会腐烂的代码；SDL3 直接提供。
> 范围（已定）：**只移 `GE_Runtime`**。`GE_Editor` 不上 Android（依赖 ImGuizmo / node-editor / 多视口停靠 / `FileDialogs`），但它必须跟着走完 SDL3 迁移。
> 前置（**已完成**）：打包系统阶段 A 的「资产根决定论 + 路径规范形」是本计划 §6 的地基。
> 关联：`游戏打包系统计划书.md`（**阶段 D 的 VFS 与本计划 §6 是同一个东西，必须合并设计**）、`项目架构总览.md`

---

## 0. 一句话架构

```
   ══════════════ 阶段 A：桌面 GLFW → SDL3（独立可验证，先做）══════════════
   桌面: GlfwWindow.cpp ──► SdlWindow.cpp     ImGui_ImplGlfw_* ──► ImGui_ImplSdl3_*
   编辑器 + GE_Runtime 全量回归通过后，才进入 Android 阶段

   ┌──────────────────────────────── APK ────────────────────────────────┐
   │  lib/arm64-v8a/libGE_Runtime.so   ← GE + GE_Runtime 全部静态编进来   │
   │  assets/                          ← gepack 产出的 dist/assets 整棵   │
   │    ├─ game.cfg                                                     │
   │    ├─ shaders/glsl/*.spv   fonts/   textures/   models/   ...       │
   │    └─ scenes/*.scene                                                │
   │  AndroidManifest.xml + org.libsdl.app.SDLActivity（SDL 自带）        │
   └───────────────────────────────┬─────────────────────────────────────┘
                                   │ AAssetManager（只读）
                                   ▼
   int main() ──► SDL_main ──► SDLActivity ──► SDL_Window（SDL_WINDOW_VULKAN）
        │                                          │
        │                                          ▼
        │                          SDL_Vulkan_CreateSurface ──► VkSurfaceKHR
        │                                                     (VK_KHR_android_surface)
        ▼
   Application（沿用现有 ctor / Run 语义）
        ├─ Window      ← SdlWindow，桌面 / Android 同一份
        ├─ Input       ← SdlInput（SDL scancode 直映射，无 AKEYCODE 间接层）
        ├─ VFS         ← 新增：Disk 后端（桌面）/ AAsset 后端（Android）
        └─ Renderer    ← 复用现有 Vulkan 栈，仅补 surface 重建 + SURFACE_LOST 处理
```

**关键判断**：这不是「移植」，是**换一个跨平台窗口库 + 补一层资源访问 + 重做生命周期模型**。引擎的抽象缝已经切好了（`Window` / `GEInput` / `AssetManager`），所以改动是**收敛的**。

**关键发现（决定了整个计划的形态）**：

1. `GE/include/GE/Core/PlatformDetection.h:31-33` —— `GE_PLATFORM_ANDROID` 已定义，下一行就是 `#error "Android is not supported!"`。
2. `GE/src/Render/VulkanBase/VulkanContext.cpp:83-84` —— **`VK_KHR_android_surface` 分支已写好**，由 `VK_USE_PLATFORM_ANDROID_KHR` 选中。
3. `GE/third_party/imgui/backends/imgui_impl_sdl3.{h,cpp}` —— **已在树内**，平台后端切换只是改 include 与三处调用。
4. **打包计划书的「路径规范形」恰好就是 VFS 路径** —— 规范形是 `<相对资源根>/<子路径>`，本来就是与物理位置无关的虚拟路径。**本计划的 VFS 应当就是打包计划书阶段 D 的 VFS，只做一次，不要为 Android 另起一套。**

---

## 1. 现状盘点与差距定位

| 能力 | 开工前现状 | 本计划归属 |
|---|---|---|
| 窗口库 | GLFW（Windows only）；**无 Android 后端** | 阶段 A 换 SDL3 |
| 平台检测 | `GE_PLATFORM_ANDROID` 已定义但紧接 `#error`（`PlatformDetection.h:31-33`） | 阶段 B |
| 窗口抽象 | `Window` 纯虚接口 25 个方法完备（`GEWindow.h`），工厂是 `#ifdef GE_PLATFORM_WINDOWS` 单分支（`Window.cpp:10-16`） | 阶段 A/C |
| 窗口实现 | `GlfwWindow.cpp`（314 行）；切 SDL 后**桌面与 Android 共用一份 `SdlWindow`** | 阶段 A |
| 输入 | `GEInput.h` 是 5 个静态方法门面；唯一 GLFW 绑定 TU 是 `WindowsInput.cpp`（52 行）；`InputState.h` 完全平台中立 | 阶段 A/C |
| 键码 | `KeyCodes.h` / `MouseCodes.h` **是 GLFW 常量的逐字复制**（`Space=32, A=65, Escape=256, F1=290, LeftShift=340`） | 阶段 A 重编号为 SDL scancode |
| Surface 扩展 | `VulkanContext.cpp:83-84` 已有 Android 分支 | ✅ 无需改动 |
| Surface 生命周期 | 创建一次（`VulkanContext.cpp:120-124`），仅 `Destroy()` 释放（`:259-262`）；**无重建路径** | 阶段 E |
| `VK_ERROR_SURFACE_LOST_KHR` | **全代码库无处理**（仅 `VulkanCommon.cpp:50` 枚举→字符串）。`OUT_OF_DATE` 已处理（`VulkanRenderContext.cpp:151-153, 210-212`） | 阶段 E |
| 应用生命周期 | `Application::Run()` 是「窗口关闭 = 退出」（`Application.cpp:96-155`） | 阶段 E |
| 资源访问 | `AssetManager::ResolvePath` 返回**绝对文件系统路径**（`AssetManager.cpp:44`）；~7 处 `std::ifstream`；20+ 文件用 `<filesystem>` 直读 | 阶段 D |
| VFS | **不存在**。唯一候选缝是 `FileSystem::ReadBinaryU32`（`FileSystem.h:11`，36 行 ifstream 包装，只有着色器加载在用） | 阶段 D（= 打包计划书阶段 D） |
| 着色器 | `.spv` **不内嵌**，运行期从资源根读（`Renderer3D_Lifecycle.cpp` / `Renderer2D.cpp` 28 处硬编码文件名） | 阶段 D 走 VFS |
| 可写目录 | 全部写 CWD：`GE.log`（`Log.cpp:15`）、`imgui.ini`（ImGui 默认，`io.IniFilename` 未覆盖）、`game.cfg` | 阶段 D |
| 入口点 | `EntryPoint.h:7,19` 整个 `main` 包在 `#ifdef _WIN64` 里 | 阶段 B |
| 构建 | CMake + MSVC/Ninja（`build.bat`），Windows-only 目标一堆 | 阶段 B |

### 1.1 迁移点实测（改动面到底有多大）

GLFW 泄漏到抽象层外的位置**有限且已知**，共 5 处。阶段 A 全部处理：

| 位置 | 用法 | 处理 |
|---|---|---|
| `GE/src/Core/Application.cpp:101` | `glfwGetTime()` 取主循环墙钟 | 换 `std::chrono::steady_clock`（同函数已有 `frameStart`，直接复用）；**不要**换 SDL 的 tick API，主循环不该依赖窗口库 |
| `GE/src/Render/ImGui/ImGuiLayer.cpp:67-68,128,134` | `ImGui_ImplGlfw_InitForOther/Shutdown/NewFrame` | `ImGui_ImplSdl3_*`（树内已有） |
| `GE_Runtime/src/GameLayer.cpp:70-71,121,134-142` | 鼠标捕获：`glfwSetInputMode(GLFW_CURSOR_*)` + `glfwGetCursorPos` | 抽成 `Window::SetCursorMode()` / `GetCursorPosition()`，SDL 侧用 `SDL_SetWindowRelativeMouseMode` / `SDL_GetMouseState` |
| `GE_Editor/src/SceneLayer.cpp:27,170-193` | 同样的鼠标捕获模式 | 同上，复用新接口 |
| `GE/include/GE/GE.h:24` | 公开伞头里 include 了 `Platform/Windows/GlfwWindow.h` | 改为 `Platform/SdlWindow.h`（或条件编译） |

另有 `GEWindow.h` 的 `GetGlfwWindow()` 是接口上的**具名泄漏**。阶段 A 改名为 `GetNativeWindow()`（桌面返回 `SDL_Window*`，Android 返回 `ANativeWindow*`）—— 调用点只有 `ImGuiLayer` / `GameLayer` / `SceneLayer`，这三处阶段 A/C 本来就要动。

**顺带发现的死代码**：`Window::GetRequiredSurfaceExtensions()`（`GEWindow.h:120`）在 GLFW 侧用 `glfwGetRequiredInstanceExtensions` 实现（`GlfwWindow.cpp:305-312`），但**全代码库无人调用**——实例扩展实际由 `VulkanContext::ApplyDefaultExtensions()` 自己决定。SDL 侧可用 `SDL_Vulkan_GetInstanceExtensions` 实现（顺带补齐一致性），或直接删除。

### 1.2 第三方依赖就绪度

| 状态 | 依赖 | 说明 |
|---|---|---|
| ✅ 已就绪 | **SDL3** | `imgui_impl_sdl3` 已在树内；SDL3 是当前活跃线（SDL2 进维护期）。**SDL 本体尚未 vendored**，见 §3.1 |
| ✅ 开箱可用 | Vulkan + VMA、Jolt Physics、EnTT、glm、yaml-cpp、Lua 5.4、KTX-Software、SPIRV-Cross、stb、tinygltf、tinyobjloader、Tracy | 均有 Android 支持或纯头文件 |
| ⚠️ 需接线（不改库） | **spdlog 1.17.0** — `sinks/android_sink.h` **已在 vendored 副本内**，换 sink 即可 | `Log.cpp:9-30` |
| ⚠️ 需换调用方式 | **miniaudio 0.11.25** — 自动选 AAudio/OpenSL ES；但 `ma_sound_init_from_file`（`AudioContext.cpp:121`）/ `ma_decoder_init_file`（`SoundAsset.cpp:22`）走 `fopen`，读不到 APK 内资源 | 阶段 D |
| ⚠️ 需换后端 | **ImGui 1.92.8 WIP** — 平台后端 GLFW → SDL3（Vulkan 后端复用） | 阶段 A |
| ❌ 淘汰 | **GLFW** | 阶段 A 后从 `CMakeLists.txt:36` 移除 |

**构建侧**：CMake 已在，NDK toolchain 直接吃。`CMAKE_MSVC_RUNTIME_LIBRARY`（`CMakeLists.txt:8`）对非 MSVC 无害；`find_package(Vulkan REQUIRED)`（`:65`）换 NDK 自带 `libvulkan.so`；`VK_USE_PLATFORM_WIN32_KHR`（`:162`）换 `VK_USE_PLATFORM_ANDROID_KHR`。**`glslc` 是主机工具**，着色器编译流程一行不用改。

### 1.3 为什么换 SDL3（决策记录）

`Window` 接口 25 个方法、`GlfwWindow.cpp` 314 行就能实现完，所以**窗口实现本身不是难点**。难点在于窗口实现**之外**的、Android 特有的那部分：

| 问题 | 手写 Android 后端 | SDL3 |
|---|---|---|
| `android_native_app_glue` 集成 + Activity | 全自己写，含 Java | SDL 自带 `org.libsdl.app.SDLActivity`，零 Java |
| 生命周期状态机（9 个 `APP_CMD_*`） | 自己写（原 §7.5 那张表） | 归一为 `SDL_EVENT_WINDOW_*` / `SDL_EVENT_*` |
| 触摸 → 鼠标合成 | 自己写 | `SDL_HINT_TOUCH_MOUSE_EVENTS` 内置 |
| IME / 软键盘 | 自己写 JNI | `SDL_StartTextInput` + `SDL_EVENT_TEXT_INPUT` |
| 键码映射 | 需 `AKEYCODE_* → GLFW 数字` 的**别扭间接层** | SDL scancode 一套值，两端共用 |

那部分是**写一次、无人测、会腐烂**的代码；换成 SDL 就变成一个有人维护的依赖。

**代价（必须承认）**：桌面也得跟着换，最大风险面是编辑器——docking、node-editor、ImGuizmo 都坐在 ImGui 平台后端上。**但这个风险可以在完全不碰 Android 的情况下独立验证**，这正是阶段 A 存在的意义。

**已否决的两个方案**：
- **原方案：保留 GLFW（桌面）+ 手写 Android 后端** —— 桌面零风险，但永久两套后端，Android 后端无测试覆盖、会腐烂。
- **GLFW（桌面）+ SDL3（仅 Android）** —— 树里永久两个窗口库，两边都要维护，是没人会来清理的技术债。

**不选 SDL2**：SDL2 已进维护期，`SDL_EVENT_*`/scancode/窗口 API 都更旧。SDL3 是当前线，且 `imgui_impl_sdl3` 已在树内。

**边界**：**SDL 只用于窗口 / 输入 / 生命周期。不要蔓延到音频**——miniaudio 已接好且 Android 侧 AAudio 支持良好，没有理由替换。

---

## 2. 阶段总览

| 阶段 | 内容 | 依赖 | 优先级 |
|---|---|---|---|
| **A** | 桌面 GLFW → SDL3 迁移（**独立可验证，不碰 Android**） | — | **P0（地基）** |
| **B** | 构建骨架：CMake/NDK/Gradle/Manifest/入口点 | A | **P0** |
| **C** | Android 平台后端（SDL3 之上）+ 触摸/IME | A、B | **P0** |
| **D** | 资源 VFS（AAssetManager）+ 可写目录分流 | B | **P0** |
| **E** | 应用生命周期与 Surface 重建 | C、D | **P0** |
| **F** | 渲染特性门槛与降级 | E | P1 |
| **G** | 打包、部署与调试（gepack → APK / adb / Tracy） | D | P1 |

**推荐开工顺序：A → B → C → D → E（这五个做完才能看到画面），随后 F → G。**

---

## 3. 阶段 A：桌面 GLFW → SDL3 迁移

**这个阶段不碰 Android，唯一目标是把换库的风险在已知可测的环境里消化掉。** 做完后桌面行为应与现在完全一致。

### 3.0 落地记录（2026-09-16，`e9b8c305` + `213b14b6`）

代码已全部写完，GLFW 已从一方代码与构建中彻底移除。**待用户构建与回归验证**（按 `no-auto-build` 惯例未自行构建）。与计划的偏差及实施中的新发现：

| 项 | 结果 |
|---|---|
| SDL 版本 | 取 **3.4.16**（`release-3.4.16`），vendored 到 `GE/third_party/SDL/`，1642 文件入库 |
| 目录形态 | 新代码放 `GE/src/Platform/`（**不按平台分目录**）；`SdlWindow.h` 按旧 `GlfwWindow.h` 的位置约定放 `GE/include/GE/Platform/`，因为 `Renderer3DInternal.h` 那种同目录裸名 include 无法跨目录 |
| 入口点 | 已核实 `SDL_main_impl.h` 在 Win32 **同时提供 `main`（控制台）与 `WinMain`（窗口）**，故 `EntryPoint.h` 去掉 `_WIN64` 守卫、写普通 `int main()` 即可两端通吃。**约束：全程序只能有一个 TU include `SDL_main.h`**（定义在头文件里） |
| 键码 | 已按 `SDL_SCANCODE_*` 重编号，枚举名一字未改。`SDL_SCANCODE_COUNT == 512` 与 `kKeyCapacity = 512` **刚好持平、零余量**，故按键翻译层加了越界丢弃 |
| 鼠标码 | 编号随 SDL 变为 左=1/中=2/右=3。**这是唯一静默的语义变化**：Lua 里 `Mouse.Button1` 由右键变左键 |
| `F25` | **已移除**（SDL scancode 集合只有 F1–F24） |
| 事件类 | `WindowFocusEvent` / `WindowLostFocusEvent` **只有枚举槽位、没有类**（换库前也没有生产者），故焦点事件暂不翻译，留到阶段 E 有真正消费者时再补 |
| 顺带删除 | `Window::GetRequiredSurfaceExtensions()` —— 已核实全代码库无调用者 |
| ImGui 输入 | SDL3 后端没有 GLFW 那种「自己装回调」的模式，必须逐事件喂 `ProcessEvent`。为此在 `Window` 上开了 `SetRawPlatformEventHook`（`const void*` 转交，避免窗口层反向依赖 ImGui），钩子在引擎事件翻译**之前**调用以复刻 GLFW 回调链的先后 |

### 3.0.1 构建与实测暴露的三处问题（均已修）

| 症状 | 根因 | 修法 |
|---|---|---|
| C2248「`Run` 是 private」 | `Application.h` 用 `friend int ::main(...)` 给入口开 `Run()` 的私有权，而 SDL 的 `#define main SDL_main` 会**改名**入口函数 —— 友元按名字绑定，于是对不上。且入口 TU 都是先包含 `Application.h`，靠调 include 顺序救不了 | 改为与入口名无关的公开转发点 `Application::Main`（`3d76dbd8`） |
| C3861「找不到 `SDL_Vulkan_CreateSurface`」 | `SDL.h` **不含** `SDL_vulkan.h`（与 `SDL_main.h` 同理是 opt-in，因它依赖 Vulkan 类型） | 补 `#include <SDL3/SDL_vulkan.h>`，并注明必须排在 `vulkan.h` 之后（该头靠 `VULKAN_CORE_H_` 判断要不要自己 typedef Vulkan 句柄）（`c8d9a2af`） |
| **转向到边缘停住** | **锁定态光标的坐标语义与 GLFW 相反**：GLFW `GLFW_CURSOR_DISABLED` 是 *unbounded*（坐标无限增长），SDL 相对模式是 *constrained to the window*（钳制在窗口内）。引擎用逐帧坐标差算鼠标增量，坐标停住则差值为 0 | 相对模式改用 `event.motion.xrel/yrel` 累加出虚拟坐标复刻 GLFW 语义（`0480cd2c`） |

**另一处顺带修的既有 bug（与 SDL 无关）**：`tools/gepack/PackRules.h` 的 `GlobMatch` 文档注释里写了示例模式 `` `**/*.pdb` ``，其中 `**/` 含 `*/` 提前终止了块注释，后面整行被当代码解析（C2018/C3872/C2059，编译器另给 C4138 佐证）。改成行注释。**此错自打包阶段 C 起就存在，只因 `gepack` 从未被构建过而没暴露**（`4eeb1331`）。

### 3.1 引入 SDL3

**`SDL3` 尚未 vendored，且仓库的 submodule 机制已经腐烂**——`.gitmodules` 里的路径是 `engin/vendor/*`（指向不存在的目录），而实际第三方在 `GE/third_party/`，导致 `git submodule status` 直接报错：

```
fatal: no submodule mapping found in .gitmodules for path 'GE/third_party/tracy'
```

现状是两种模式并存：`GLFW` / `spdlog` / `glm` 带 `.git`（嵌套仓库），`imgui` / `entt` / `JoltPhysics` 等是普通拷贝。

**建议：普通拷贝到 `GE/third_party/SDL/`**，与占多数的普通拷贝模式一致，离线可构建、版本明确、不依赖已腐烂的 submodule 管道。CMake 用 `add_subdirectory(GE/third_party/SDL)`（SDL3 自带 CMake，需关掉 `SDL_TESTS` / `SDL_EXAMPLES`）。

> `.gitmodules` 的腐烂本身是笔独立的小债，**不要在阶段 A 里顺手修**（会牵连所有第三方目录的重建）。记在 §10。

### 3.2 `SdlWindow`

新文件 `GE/src/Platform/SdlWindow.{h,cpp}`（**不再按平台分目录**——一份实现两端共用）。核心映射：

| `Window` 接口 | SDL3 实现 |
|---|---|
| 创建 | `SDL_Init(SDL_INIT_VIDEO)` + `SDL_CreateWindow(title, w, h, SDL_WINDOW_VULKAN \| SDL_WINDOW_RESIZABLE)` |
| `CreateVulkanSurface(instance)` | `SDL_Vulkan_CreateSurface(window, instance, nullptr, &surface)` |
| `GetNativeWindow()` | 返回 `SDL_Window*`（Android 上可再取 `ANativeWindow*`） |
| `GetWidth/GetHeight` | **必须用 `SDL_GetWindowSizeInPixels`**（Vulkan 要像素尺寸，不是逻辑尺寸——高分屏上两者不同） |
| `GetDpiFactor` / `GetContentScaleFactor` | `SDL_GetWindowDisplayScale` |
| `SetVSync` / `GetVSync` | 仅记录状态，实际由 swapchain 呈现模式决定 |
| `SetMaximized` | `SDL_MaximizeWindow`（Android 上空操作）。注意 `Application.cpp:74` 调了它，必须保证 Android 侧是安静的空操作而不是断言 |
| `SetTitle` / `SetWindowMode` / `SetResizable` / `Resize` | 对应 SDL 调用；Android 上空操作 |
| `SetCursorMode` / `GetCursorPosition`（新增） | `SDL_SetWindowRelativeMouseMode` / `SDL_GetMouseState` |
| `ProcessEvents` | `SDL_PollEvent` 循环 → 翻译成引擎事件 |
| `ShouldClose` / `Close` | `SDL_EVENT_QUIT` / `SDL_EVENT_WINDOW_CLOSE_REQUESTED` |

`Window.cpp:10-16` 的工厂改成不带平台分支的单一路径（`SdlWindow` 两端通用）。

**事件翻译**（阶段 A 只需覆盖桌面需要的）：`SDL_EVENT_WINDOW_RESIZED` → `WindowResizeEvent`；`SDL_EVENT_WINDOW_CLOSE_REQUESTED` → `WindowCloseEvent`；`SDL_EVENT_KEY_DOWN/UP` → `KeyPressed/ReleasedEvent`；`SDL_EVENT_TEXT_INPUT` → `KeyTypedEvent`；`SDL_EVENT_MOUSE_BUTTON_DOWN/UP` → `MouseButtonPressed/ReleasedEvent`；`SDL_EVENT_MOUSE_MOTION` → `MouseMovedEvent`；`SDL_EVENT_MOUSE_WHEEL` → `MouseScrolledEvent`；`SDL_EVENT_WINDOW_FOCUS_GAINED/LOST` → `WindowFocus/LostFocusEvent`（事件枚举**已存在**但当前无生产者，`Event.h:15-30`）。

### 3.3 键码重编号

`KeyCodes.h` / `MouseCodes.h` 现在注释就写着 `// From glfw3.h`。改为按 `SDL_SCANCODE_*` / `SDL_BUTTON_*` 取值。

**名称一个字都不能改，只改数值。** 因为 `ScriptEngine.cpp:51-72+` 有一张大表把 `{"Space", Key::Space}, {"Escape", Key::Escape}, ...` 注入 Lua 的 `Key` / `Mouse` 表——**枚举名是脚本 API**，数值不是。

三个已核实的有利事实：
- 全代码库 37 处 `KeyCode::` / `MouseCode::` 具名引用 → 只改数值不影响
- **没有任何持久化的键码**：`game.cfg` 只有场景/窗口/渲染开关，`editor_settings.cfg` 只有相机与渲染参数——重编号不会让既有配置失效
- `KeyCode = uint16_t` 容得下 SDL scancode

⚠️ **必须复核 `InputState.h:25-26` 的容量**：注释写明「KeyCode 枚举到 348（GLFW 布局）取 512 宽松」，`kKeyCapacity = 512` 是照着 GLFW 的 348 上界留的余量。**SDL scancode 的上界（`SDL_SCANCODE_COUNT`）可能就贴着 512**，需按实际值重算并放宽（否则个别键会写越界，且是静默的）。

### 3.4 ImGui 平台后端

`ImGuiLayer.cpp` 三处调用换 `ImGui_ImplSdl3_InitForVulkan` / `ImGui_ImplSdl3_Shutdown` / `ImGui_ImplSdl3_NewFrame`。Vulkan 后端（`ImGui_ImplVulkan_Init`，`UseDynamicRendering = true`）不变。

**同时必须修掉一处 Windows 假设**：`ImGuiLayer.cpp:54-55` 硬编码了 `C:\Windows\Fonts\msyh.ttc` 作 CJK 字体——Android 上必然失败，且即便在桌面也是不可移植的。改为从资源根加载随包字体（阶段 D 后自动走 VFS）。

`ImGuiLayer.cpp:41-43` 的 `OpenSans-Regular.ttf` 走 `ResolvePath`，阶段 D 后自动走 VFS，本阶段无需专门改。

### 3.5 验收

**代码已就绪，以下全部待跑。** 除计划中的项外，补三条本轮实施新增的必验项（都在 §10 风险里）：

- [ ] `GE_Editor` 完整回归：三个场景加载、视口、Hierarchy、Gizmo、node-editor、Docking 布局、`editor_settings.cfg` 读写
- [ ] `GE_Runtime` 回归：`game.cfg` 启动、自由相机鼠标捕获（`GameLayer` 的 cursor lock/unlock）
- [ ] 键位全量自测：字母数字、方向键、Esc/Enter/Tab/Backspace、F1–F12、Shift/Ctrl/Alt、小键盘
- [ ] **【新增】中文输入**：软键盘之外的 `SDL_EVENT_TEXT_INPUT` 路径（SDL 一次给整串 UTF-8，翻译层逐码点拆发）在中文输入法下确实生效，且 ImGui 输入框与脚本 `KeyTypedEvent` 都收到
- [ ] **【新增】鼠标按键语义**：左/中/右键在编辑器与 `GE_Runtime` 下都对应正确（SDL 是 左=1/中=2/右=3，与 GLFW 不同）。这是本轮唯一静默的语义变化
- [ ] **【新增】跟随相机切换键**：`2/3/4.scene` 已把 `ToggleKey` 从 86 改为 25，运行时应能按 **V** 在第一/第三人称间切换（若按不出来说明该字段没生效）
- [ ] **【新增】自由视角连续转向**：Play 态锁定鼠标后，朝一个方向持续拖拽必须能**一直转**、推到窗口边缘也不停（这是 SDL 相对模式坐标被钳制导致过的 bug，见 §3.0.1）；另留意解锁瞬间是否视角跳变
- [ ] 窗口缩放、最大化、DPI 缩放（多显示器不同 DPI 下 UI 尺寸正确）。**注意 `GetWidth/GetHeight` 已改为像素尺寸**，高 DPI 下与换库前不同，重点看视口宽高比与 ImGui UI 缩放是否仍正确
- [ ] `GE.log` / `imgui.ini` / `game.cfg` 行为不变（`game.cfg` 里的窗口宽高现在是逻辑尺寸、实际按像素创建，确认无明显偏差）
- [ ] GLFW 已从 `CMakeLists.txt` 移除且一方代码零残留引用（已静态核对通过，构建后复查警告）

---

## 4. 阶段 B：构建骨架

### 4.0 落地记录（2026-09-16，`0953972b`）

**已完成，首次产出可安装 APK。** `gradlew assembleDebug` 通过，产物：

```
platform/android/app/build/outputs/apk/debug/app-debug.apk   39 MB   仅 arm64-v8a
  └─ lib/arm64-v8a/libGE_Runtime.so                          40.7 MB  （打包后 strip 到 38.8 MB）
  └─ classes.dex + classes2.dex                             SDL 的 12 个 Java 类
  └─ AndroidManifest.xml + res/
```

**已验证的产物事实**（不是"应该能跑"，是逐项查过）：

| 验证项 | 方法 | 结果 |
|---|---|---|
| `SDL_main` 从 .so 导出 | `llvm-nm -D --defined-only` | `T SDL_main`，全局动态符号 —— SDLActivity 找得到 |
| 引擎确实链进去了 | 在 .so 里找引擎特有字符串 | `shaders/glsl` / `game.cfg` / `GE Runtime` / `scenes/2.scene` 全部命中 |
| 不依赖外部 libc++ | 查未定义符号 | 无（用了 `ANDROID_STL=c++_static`，APK 里只有一个 .so） |
| manifest 最终态 | 读 AGP 合并后的 manifest | `package=com.ge.runtime`、`minSdk=33`、`targetSdk=36`、activity=`org.libsdl.app.SDLActivity`、声明 `android.hardware.vulkan.version=0x00403000`(=1.3)、**GLES 声明已移除** |

**工具链版本（实测可用组合）**：AGP 8.13.2 + Gradle 8.14.5 + JDK 17.0.15 + NDK 27.3.13750724 + compileSdk 36 / buildTools 36.0.0。

**工程骨架取自 SDL 自带的 `android-project`**（含 gradle wrapper 与 `org.libsdl.app` 的 12 个 Java 源，后者**逐字节未改**，原作者是 SDL 项目 / Sam Lantinga，zlib 许可）。Gradle 与 manifest 是按本项目重写的，不是模板原样。

**CMake 接线的一个关键决定**：Gradle 的 `externalNativeBuild.cmake.path` 直接指向**仓库根** `CMakeLists.txt`，而不是套一层 `platform/android/app/jni/CMakeLists.txt`。原因是引擎 CMakeLists 里有 73 处 `${CMAKE_SOURCE_DIR}`，只有让仓库根当顶层源目录它们才解析正确。仓库根内部按 `if(ANDROID)` 分区。

**四个阻塞（三个是既有缺陷，非 Android 专有）**：

| 症状 | 根因 | 性质 |
|---|---|---|
| `add_dependencies` 报 non-existent target | `assets/shaders/glsl/CMakeLists.txt` 硬挂 `GE_Editor` 依赖，而 Android 下该目标不存在 | 桌面专属假设 |
| `Base.h: "Platform doesn't support debugbreak yet!"` | `GE_DEBUGBREAK()` 只有 Windows / Linux 分支 | 平台分支缺失 |
| `vulkan_core.h: 找不到 vk_video/...` | **`GE/third_party/vulkan` 是不完整的 vendor 副本**：只拷了 `include/vulkan/`、漏了兄弟目录 `vk_video/`。Windows 上侥幸通过是因为能回退到 Vulkan SDK 的**同版本**（350）副本；Android 回退到 NDK 的 `vk_video` 却是 **275 版、无 av1**，与 350 版 `vulkan_core.h` 版本错配 | **既有 vendor 缺陷** |
| `ImGuizmo.h not found` | `ImGuiLayer.cpp` include 了它却全无使用（grep 全仓库仅 1 处 = 那行 include） | **死依赖**，gizmo 本就属编辑器 |

**已知的两道运行时坎（阶段 B 不负责，但装上必崩）**：`Log::Init` 的文件 sink 在 Android 上不可写（见 §10 对应条目），以及 **APK 里 `assets/` 是 0 个文件**（资产入包是阶段 G）。详见下面各节。

### 4.1 平台检测

移除 `PlatformDetection.h:31-33` 的 `#error "Android is not supported!"`，保留 `#define GE_PLATFORM_ANDROID`。`NDEBUG` 与 ARM NEON 相关宏由 NDK toolchain 自带。

### 4.2 入口点

`EntryPoint.h` 的 `main` 保持 `#ifdef _WIN64` 不变（桌面零回归）。**好消息：SDL 把入口统一了**——SDL3 通过 `SDL_main.h` 重命名你的 `main`，桌面与 Android 都是 `int main(int argc, char** argv)`，Android 侧由 `SDLActivity` 调用。所以**不需要单独的 `android_main` 或 glue 代码**。

只需：
- 新增 `GE/include/GE/Core/AndroidEntryPoint.h`，复用同一份 `Log::Init()` + `CreateApplication` 契约
- 新目录 `platform/android/`：`build.gradle`、`AndroidManifest.xml`、`gradle.properties`、`CMakeLists.txt`（Gradle 侧 `externalNativeBuild` 入口）
- **并入 SDL3 自带的 `android-project/app/src/main/java/org/libsdl/app/`**（`SDLActivity` 等 Java 源）——这是 SDL 的既定集成方式，manifest 的 Activity 指到 `org.libsdl.app.SDLActivity`

**必须让 Android 走与桌面完全相同的 `CreateApplication` 路径**，否则 `RuntimeApp` / `GameLayer` 状态机会漂移（打包计划书 §9.7 记录过同类风险）。

### 4.3 CMake 改造

Windows-only 目标在 Android 下必须整体跳过：

| 目标 | Android 下 |
|---|---|
| `add_subdirectory(GE/third_party/imgui)` 系列（`:38-40`） | 保留 ImGui，跳过 `ImGuizmo` / `imgui-node-editor`（编辑器专用） |
| `add_subdirectory(GE/third_party/tracy)`（`:43`） | 保留（可关） |
| `GE_Editor`（`:252`）、`gemesh`（`:268`）、`gepack`（`:284`） | **跳过**（`gepack` 是主机侧工具，见 §9） |
| `GE_Runtime`（`:302`） | **改为 SHARED**，产出 `libGE_Runtime.so` |
| `find_package(Vulkan REQUIRED)`（`:65`） | 换 `find_library(VULKAN_LIB vulkan)` + NDK 头 |
| `VK_USE_PLATFORM_WIN32_KHR`（`:162`） | 换 `VK_USE_PLATFORM_ANDROID_KHR` |
| `assets/shaders/glsl`（`:311`） | 保留（`glslc` 主机侧执行，产物进 assets） |

`GE_SRC` 的 `file(GLOB ...)` 里 `${CMAKE_SOURCE_DIR}/GE/src/Platform/Windows/*.cpp`（`:109`）改为 `GE/src/Platform/*.cpp`（阶段 A 后不再按平台分目录）。

### 4.4 API level 与 ABI

**建议 `ANDROID_PLATFORM=android-33`（Android 13）、`ANDROID_ABI=arm64-v8a`。** 依据：引擎要求 `apiVersion >= 1.3`（`VulkanContext.cpp:195` 无条件硬门槛），并把 `dynamicRendering` / `synchronization2` 当**核心 1.3 特性**启用（`:217-222`），没有扩展回退路径。

> ⚠️ **待核实（本轮网络受限未能查证）**：Vulkan 1.3 在 Android 上的设备覆盖范围，以及 CDD 是否强制 API 33+ 设备支持 1.3。开工第一步应在真机跑 `vulkaninfo` 确认。SDL3 本身要求 Android 5.0+（API 21），所以门槛由 Vulkan 而非 SDL 决定。若需下探到 API 31 或更低，**必须补 `VK_KHR_dynamic_rendering` / `VK_KHR_synchronization2` 扩展回退**——那会牵动 `RenderGraph.cpp`（`:41-99`）与 `VulkanImage.cpp`（`:250-291`）里大量的 `*FlagBits2` 用法，是独立的一大块，见阶段 F。

### 4.5 验收

- [ ] Gradle 产出 APK，`libGE_Runtime.so` 正确打入 `lib/arm64-v8a/`，SDL 的 Java 源正确并入
- [ ] 桌面构建（`build.bat`）**零回归**——`GE_Editor` / `GE_Runtime` / `gemesh` / `gepack` 全部照常

---

## 5. 阶段 C：Android 平台后端

**因为走了 SDL3，本阶段比原方案薄得多**：没有 glue、没有 Java、没有生命周期状态机。

### 5.1 `SdlWindow` 的 Android 分支

阶段 A 的 `SdlWindow` 已两端通用，本阶段只补 Android 特有行为：

- `GetWidth/GetHeight`：继续用 `SDL_GetWindowSizeInPixels`（Android 上必须，缩放/旋转后逻辑尺寸与像素尺寸差异显著）
- `GetDpiFactor` / `GetContentScaleFactor`：`SDL_GetWindowDisplayScale`——**Android 密度差异大（1.0~4.0），ImGui 字号与所有 UI 尺寸都要按它缩放**，否则高 DPI 设备上 UI 小到不可用。`ImGuiLayer` 需加 `style.ScaleAllSizes(dpi)`
- `SetMaximized` / `SetTitle` / `SetWindowMode` / `SetResizable` / `Resize`：安静空操作

### 5.2 输入

`GEInput.h` 是 5 个静态方法门面，实现从 `WindowsInput.cpp` 迁到 `SdlInput.cpp`（**两端共用**，不再按平台分目录）。**事件→状态的转换路径已平台中立**（`Scene::OnEvent` → `InputState::Record*`，`Scene.cpp:1509-1534`），这一层只需忠实翻译 SDL 状态。

两件必须做对的事：

1. **键码**：SDL scancode 直映射（阶段 A 已重编号，本阶段零额外映射表）。Windows 上 `SDL_EVENT_KEY_DOWN` 给的是 scancode，Android 上 SDL 已经帮你把 `AKEYCODE_*` 归一成同一套 scancode —— **这正是换 SDL 省下的那层别扭间接层**。软键盘不出键码而是走 IME：用 `SDL_StartTextInput` + `SDL_EVENT_TEXT_INPUT`（SDL 已处理 JNI 细节）。
2. **触摸 → 鼠标合成**：`Scene` 与 `InputState` 的交互模型是鼠标（`MouseMovedEvent` / `MouseButtonPressed` / `MouseScrolled`）。开 `SDL_HINT_TOUCH_MOUSE_EVENTS` 让 SDL 自动合成单指触摸为鼠标事件；双指捏合需自己从 `SDL_EVENT_FINGER_*` 合成为 `MouseScrolledEvent`。

**双投递顺序**：SDL 事件要**同时**喂给两条通路——ImGui（UI 优先吃掉）和引擎事件系统。ImGui 的 `WantCaptureMouse` / `WantCaptureKeyboard` 作为「是否继续投递给引擎」的门闸，否则点 UI 会同时转相机。

### 5.3 验收

- [ ] Android 上能创建 SDL 窗口 + Vulkan surface，`SDL_Vulkan_CreateSurface` 不报错
- [ ] 触摸拖拽能驱动 `InputState`（用日志或 ImGui 面板观测），点 UI 不穿透到相机
- [ ] 软键盘弹出能输入文本（ImGui 输入框）
- [ ] 桌面输入零回归（阶段 A 已覆盖）

---

## 6. 阶段 D：资源 VFS + 可写目录

### 6.0 与打包计划书阶段 D 合并（重要）

打包计划书 §6 的「阶段 D（可选）：单文件资源包 `.gepak` + 虚拟文件系统」与本阶段**要的是同一个 VFS**。Android 的 `AAssetManager` 后端不过是**第一个非磁盘 VFS 实现**。

**因此：不要为 Android 另起一套资源读取层。** 直接落地打包计划书阶段 D 的 VFS 抽象，Android 后端作为它的第二个实现（磁盘 / AAsset）。收益：

- 打包计划书阶段 D 从「可选」变成被 Android 需求倒逼落地，顺带把 `.gepak` 单文件分发能力铺好
- 打包计划书 §9.2 警告的「Lua `require` 绕过 VFS 直接命中真实文件系统」是**同一个坑**，一次修好两处受益

### 6.0.1 M2.5 落地记录（2026-09-16，`41648f33` + `c1aa0c6c` + `bbaf48e6`）

**这是 M2.5（可启动 + 有画面）的代码部分。** 覆盖的是阶段 D 的**运行时读取侧**，外加阶段 G 里"资产入包"那一环；阶段 D 的其余部分（可写目录 overlay、`.gepak` 单文件包）仍未做。**待用户构建 + 真机验证。**

做的四件事：

| 项 | 落点 |
|---|---|
| 日志不再写 CWD | `Log::Init` 永不抛出 + `PlatformUtils::GetUserDataDirectory()`（Android 走 `SDL_GetPrefPath`）+ `android_sink`（tag `GE`） |
| 只读 VFS | 新增 `GE/{include/GE,src}/FileSystem/VFS.*`，`DiskVFS` / `AndroidAssetVFS` 两个后端，三个函数 `ReadAll` / `ReadText` / `Exists` |
| 路径语义翻转 | `AssetManager::ResolvePath` → `ResolveCanonical`（规范形，读）+ `ResolveWritePath`（绝对路径，写）。调用面约 44 处 |
| 资产入包 | `platform/android/app/build.gradle` 的 `copyGameAssets`：`dist/assets/**` + `dist/game.cfg` → `build/generated/gepackAssets`（注册进 `sourceSets.main.assets`） |

**接进 VFS 的读取点共 20 处**，覆盖运行时全部资产 I/O：二进制（`GEMeshLoader` / `AnimationClipLoader` / `VulkanShaderModule` / `FileSystem`）、YAML（`MaterialManager` / `SceneSerializer` / `GameConfig`）、贴图（`stbi_load_from_memory` + `ktxTexture_CreateFromMemory`）、tinyobj（istream 重载 + 自定义 `MaterialReader`）、tinygltf（`LoadBinaryFromMemory` / `LoadASCIIFromString`）、Lua。

**三个只有换平台才会暴露的缺陷，本轮一并修掉**（都已写进 §10）：

1. **`.gemesh` 内嵌贴图槽是开发机绝对路径**。实测 `dist/assets/models/shayv/未命名.gemesh` 里是 `F:\yxy\project\GameEngine\assets\models\shayv\Ellen_Body_Map1_D.png`。`CanonicalizeGEMeshEmbeddedRefs` 只在**烘焙**路径被调用（`ModelLoader::ConvertToGEMesh`、`SceneSerializer::BakeSourceToGemesh`），运行期从不调用；而它依赖资源根的**绝对形**，Android 上不存在 → 归一必然失败 → 贴图全丢且**不报错**（模型发白）。修法见 §10 第 19 条。
2. **`AssetPaths::Fonts` 大小写与磁盘不符**（`fonts/opensans` vs `assets/fonts/OpenSans`）+ **`AddFontFromFileTTF` 失败即 abort**。两者叠加使字体加载从"降级"变成"崩溃"，故字体现经 VFS 读进内存走 `AddFontFromMemoryTTF`。见 §10 第 20 条。
3. **`std::filesystem::path::string()` 在 Windows 上产出反斜杠**，而规范形一律正斜杠。凡是"路径写进场景 / 交给 VFS"的地方都改用 `generic_string()`。这条**不经真机测不出来**（Windows 上反斜杠照样能打开文件），性质与 §3.0.1 那三类同族。见 §10 第 21 条。

**实施中的两处判断**（有意为之，不是遗漏）：

- **不做 VFS 流式 `Reader` 接口、不做 `ma_vfs`**。首版"看到画面"的路径上没有任何东西需要流式；音频用 `VFS::ReadAll` + 内存解码即可（计划书风险 11 也是这么建议的）。等真要做音频流式再补，那时它是**第一次**被需要，而不是现在为想象中的调用方预留。
- **不做"启动期零尺寸窗口"的提前等待**。`SDLActivity` 只在 Surface 就绪且 Activity 已 resumed 时才启动 native main 线程（`SDLActivity.java:858-866`），窗口尺寸理应为非零；而提前 `SDL_PumpEvents` 会在 `Application` 装好事件回调**之前**派发事件（`SdlWindow::HandleEvent` 里 `m_EventCallback` 没有空值守卫），为一个小概率问题引入一个真实崩溃点不划算。改为在 `SyncExtentFromWindow` 里对 0 尺寸打**错误日志**，真机上若真发生能一眼看到根因。

### 6.1 VFS 接口

现在唯一候选缝是 `FileSystem::ReadBinaryU32`（`FileSystem.h:11`）——太窄（只有读、无 stat、无 open/seek）。扩成最小的只读接口：

```
VFS::ReadAll(path)  -> std::vector<std::byte>     // 整文件读，覆盖绝大多数调用点
VFS::ReadText(path) -> std::string
VFS::Exists(path)   -> bool
VFS::OpenRead(path) -> std::unique_ptr<Reader>    // 流式读（音频用）
```

两个后端：`DiskVFS`（`std::filesystem` + `ifstream`，桌面）/ `AndroidAssetVFS`（`AAssetManager_open` + `AAsset_read`，Android）。进程启动时装全局实例。

**路径语义**：VFS 路径就是 `AssetPathUtil` 的**规范形**（`<相对资源根>/<子路径>`，正斜杠、无前导资源根名、无盘符）。`AssetPathUtil` 是**纯字符串逻辑、不访问文件系统**（`AssetPathUtil.cpp:41-48` 明确注明），**一行都不用改**。

⚠️ **`AssetManager::ResolvePath` 返回绝对路径（`AssetManager.cpp:44`）这个语义要改**：Android 上不存在「资源的绝对路径」。拆成两层——`ResolveCanonical(raw) -> 规范形`（供 VFS）与 `ResolveWritePath(raw) -> 绝对路径`（供可写目录）。

### 6.2 需要改造的读取点

> **本节已落地（M2.5 的代码部分），见 §6.0.1。** 实际改动面比下表略大：除下表所列，还包含 `AnimationClipLoader`、`ScriptEngine` 的文件读取与 Lua `package.searchers`、以及 `FileSystem::ReadBinaryU32`（已收编为 VFS 之上的 SPIR-V 取 uint32_t 视图，不再是独立的 ifstream 包装）。

| 类别 | 位置 | 改法 |
|---|---|---|
| 二进制整读 | `FileSystem.cpp:10`、`VulkanShaderModule.cpp:43`、`GEMeshLoader.cpp:417` | `VFS::ReadAll` |
| YAML | `SceneSerializer.cpp:1163`、`SceneAssetScanner.cpp:117`、`MaterialManager.cpp:104`、`GameConfig.cpp:68` | `YAML::LoadFile(p)` → `YAML::Load(VFS::ReadText(p))`（4 处） |
| tinyobj | `OBJLoader.cpp:37` `tinyobj::LoadObj` | 读进内存 + `std::istringstream`，用 tinyobj 的 stream 重载 |
| tinygltf | `GLTFLoader.cpp:389-391` `LoadBinaryFromFile`/`LoadASCIIFromFile` | 换 `LoadBinaryFromMemory` / `LoadASCIIFromString`（tinygltf 原生支持） |
| Lua | `ScriptEngine.cpp:116-123`；`:921-922` `package.path` | **必须补自定义 searcher**：VFS 读 + `luaL_loadbuffer`。这是打包计划书 §9.2 点名的坑 |
| miniaudio | `AudioContext.cpp:121`、`SoundAsset.cpp:22` | 优先自定义 `ma_vfs`（保留流式解码，不把整首 BGM 读进内存）；简单起见可先 `ma_decoder_init_memory` |
| 存在性判断 | `MeshManager.cpp:334`、`Texture.cpp:87`、`AnimationClipManager.cpp:72,108` | `VFS::Exists` |
| 环境贴图 | `EnvironmentMap.cpp:29-35`（3 个散文件 cubemap + BRDF LUT） | 走现有 `Texture::LoadFromFile*Async`，底层换 VFS |

### 6.3 可写目录（必须与 VFS 分开的第二概念）

> **已落地部分（M2.5，见 §6.0.1）**：新增 `PlatformUtils::GetUserDataDirectory()`（桌面 = CWD，保持原行为；Android = `SDL_GetPrefPath`）；`GE.log` 与 `imgui.ini` 落到该目录；`game.cfg` 在 Android 上按「用户目录 → APK 资产根」读取。**未做**：烘焙产物到用户目录的 overlay —— 按下面自己的建议，首版**禁掉 Android 运行期烘焙**（`PlatformUtils::IsAssetRootWritable()` 为假时 5 处写入点安静跳过），要求资产在打包前烘好。

**只读资产根**与**可写用户目录**是两回事，Android 上后者只能是 `app->activity->internalDataPath`。新增 `PlatformUtils::GetUserDataDirectory()`：

- Windows：返回现状路径（exe 目录 / CWD），**桌面行为零变化**
- Android：SDL 可用 `SDL_GetPrefPath(org, app)`（SDL 已封装好 Android 的 internal data 路径），或直接取 `internalDataPath`

迁到该目录的东西：

| 文件 | 现状 | 去向 |
|---|---|---|
| `GE.log` | `Log.cpp:15` 相对 CWD | 用户目录；Android 另加 `spdlog::sinks::android_sink`（vendored spdlog 1.17.0 **已含** `sinks/android_sink.h`），可保留文件 sink 便于 adb 拉取 |
| `imgui.ini` | ImGui 默认相对 CWD（`io.IniFilename` 未覆盖） | 显式设成用户目录下的绝对路径 |
| `game.cfg` | `GameConfig.cpp:19,26,68` exe 同级 → CWD | **读取顺序改为**：用户目录（允许玩家覆盖）→ APK assets（随包发布） |
| 烘焙产物 `.geanim` / `.gemesh` / `.gemat` | `AnimationClipLoader.cpp:134`、`GEMeshLoader.cpp:394`、`MaterialManager.cpp:158-161` **写到资产根** | Android 上资产根只读，**必须重定向到用户目录**；读取时用户目录优先（overlay 语义） |

> **overlay 设计**：读取时「用户目录 → 资产 VFS」顺序回落，写入一律进用户目录。这既解决 Android 只读问题，也让桌面端「运行时烘焙缓存」不再污染仓库。**首版建议干脆禁掉 Android 上的运行期烘焙**（要求资产预先烘焙好，`gepack` 已有校验能力），把 overlay 作为后续增强——烘焙产物是 30+MB 级数据，移动端存储与耗时都难接受。

### 6.4 验收

**代码已就绪（§6.0.1），以下全部待跑。** 前三条按 `no-auto-build` 惯例由用户构建 + 真机验证：

- [ ] **桌面零回归**：`build.bat` 四目标全绿；仓库根启动 `GE_Runtime` 能加载 `2.scene`（贴图/网格/动画/音频/Lua 全在位）；编辑器三场景 + 材质另存为正常
- [ ] **桌面 dist 隔离**（打包计划书一直没做的判据，也是风险 19 的直接验证）：把 `dist/` 整个拷到无关目录再启动 `dist/GE_Runtime.exe` —— 修好前会因内嵌绝对路径而"读开发机上的那份资产"，看起来正常但实际没走包
- [ ] Android 上能完整加载场景：`.scene` + `.gemesh` + `.ktx2` + `.geanim` + `.gemat` + `.spv` + 音频 + Lua 脚本，全部从 APK 内读出（`unzip -l app-debug.apk` 先确认 62 个资产与 `game.cfg` 确实在包里）
- [ ] `adb logcat -s GE` 能看到 VFS 后端与资源根两行启动横幅，且**无 abort**（abort 的第一嫌疑是字形加载，见风险 20）
- [ ] `GE.log` / `imgui.ini` 落在用户目录（`adb shell run-as com.ge.runtime ls files/`），重启后配置保持
- [ ] Lua `require` 在 VFS 下可用（自定义 searcher 生效）
- [ ] `game.cfg` 的读取顺序生效：用户目录优先，回退 APK 内的资产根

---

## 7. 阶段 E：应用生命周期与 Surface 重建

**这是风险最高、也最容易被低估的部分。** SDL 替你解决了 Activity 与事件胶水，但**渲染器侧的责任一点没少**——SDL 给的是「窗口变了」的事件，怎么重建 `VkSurfaceKHR` 与 swapchain 仍要自己做。

### 7.1 启动时序

现状：`Application` ctor 里 `Window::Create`（`Application.cpp:34`）→ 立刻 `Renderer`（`:65`）→ Renderer ctor 内部创建 surface + swapchain（`Renderer.cpp:28,32-37,40`）。

Android 上 `main` 跑起来时**不保证已有可用窗口**。**方案**：在 `Application` 构造前阻塞等首个可用窗口的 SDL 事件（`SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED` 且像素尺寸非零），拿到后再构造 `Application`。这样 `Application` ctor 语义**完全不变**，新逻辑收敛在平台层与渲染上下文内。备选（把 `Application` 改成生命周期感知+延迟初始化）侵入性大得多，不推荐。

### 7.2 Surface 重建

现状：surface 在 `VulkanContext.cpp:120-124` 创建一次，只在 `Destroy()`（`:259-262`）释放，**无重建路径**。`VkSurfaceKHR` 由 `VulkanContext` 持有（`GetSurface()`，`VulkanContext.h:67`），**不在** `VulkanRenderContext` 里存成员（只在 ctor 收下后传给 swapchain，`Renderer.cpp:33`）。

需要新增：

1. `VulkanContext::RecreateSurface()` —— 销毁旧 surface、以同一 `VkInstance` 重建。物理设备/队列族不需重选
2. **`VulkanRenderContext::UpdateSwapchain` 需要新重载** —— 现有 `UpdateSwapchain(const vk::Extent2D&, ...)`（`VulkanRenderContext.cpp:380-387`）是「拿旧 swapchain 构造新的、把旧的当 `oldSwapchain`」，**会继承旧 surface 句柄**。surface 真被销毁后这是悬垂句柄。必须加带 `vk::SurfaceKHR` 参数的重载，让新 swapchain 用新 surface
3. 重建链路：窗口销毁事件 → `waitIdle` → 释放 swapchain + 各 image 的 `RenderTarget` + surface；窗口重建事件 → `RecreateSurface` → 重建 swapchain → 重建 RenderTarget → 通知 ImGui
4. **ImGui 侧**：`ImGuiLayer::OnSwapchainRecreated`（`ImGuiLayer.cpp:155-160`）目前刻意的空实现（Vulkan 后端每帧重查 swapchain）。需确认 `ImGui_ImplVulkan` 在 image count / format 变化时能自愈；若不能，需在重建时 `ImGui_ImplVulkan_Shutdown` + 重 `Init`

> ⚠️ **真机待确认**：SDL3 在 Android 上窗口销毁/重建时，**`VkSurfaceKHR` 是 SDL 自动重建还是需要应用显式重建**？我无法离线确认 SDL3 的具体行为。这决定 §7.2 第 1 步是必需还是冗余。**开工时应先写个最小用例验证**，别照抄假设。

### 7.3 `VK_ERROR_SURFACE_LOST_KHR`

**全代码库无处理**。当前若发生，`vk::SurfaceLostKHRError` 会从 `acquireNextImageKHR` 直接抛出、无人接管。

`VK_ERROR_OUT_OF_DATE_KHR` 的处理已在 `VulkanRenderContext::BeginFrame`（`:147-170`）与 `Present`（`:210-212`）。Android 上 surface lost 是**常态**（切后台、锁屏、旋转），必须与 out-of-date 同级别对待：捕获 → 走 §7.2 重建链路。

### 7.4 顺带修一个既有隐患

`VulkanRenderContext.cpp:166-170`：acquire 重试后仍非 `eSuccess` 时，函数释放信号量并 `return`，**但没有设置 `m_FrameActive`**。下一行 `Begin()` 就调 `GetActiveFrame()`，而它在 `m_FrameActive` 为假时断言失败（`:233`）。

桌面端这是难触发的潜伏路径，**Android 上 surface lost / 频繁重建会让它变成可复现的崩溃**。阶段 E 必须一并修掉。

### 7.5 生命周期策略

SDL 把 Android 的 `APP_CMD_*` 归一成了 SDL 事件，需要映射的策略如下：

| SDL 事件 | 处理 |
|---|---|
| 窗口像素尺寸变为非零 / `SDL_EVENT_WINDOW_RESTORED` | 建/重建 surface + swapchain |
| `SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED` | 走现有 `RecreateSwapchain` 路径（`Renderer.cpp:198-212`），等价于桌面 `WindowResizeEvent` |
| 窗口销毁（`SDL_EVENT_WINDOW_DESTROYED` / 尺寸变零） | 释放 swapchain（**保留** device / 资产 / 场景） |
| `SDL_EVENT_WINDOW_FOCUS_GAINED` / `LOST` | 合成 `WindowFocusEvent` / `WindowLostFocusEvent`（事件枚举**已存在**，阶段 A 已接线） |
| `SDL_EVENT_WINDOW_MINIMIZED` / 进后台 | 停渲染循环、`AudioContext` 暂停；**不要**析构 `Application`（进程可能随时被恢复） |
| `SDL_EVENT_WINDOW_RESTORED` / 回前台 | 恢复循环 |
| 低内存（SDL3 无直接事件） | 打日志（后续可触发资产缓存回收） |
| `SDL_EVENT_QUIT` / 返回键 | 唯一真正的退出路径 → 析构 `Application` |

**`Application::Run()` 的语义要改**：现在是 `while (m_Running)` + `ShouldClose()`。Android 上「窗口关闭」不等于「退出」，需要与窗口解耦的退出信号（如 `Application::RequestExit()`），由 `SDL_EVENT_QUIT` 触发。

### 7.6 验收

- [ ] 切后台 → 切回，画面正常恢复（surface 重建链路走通）
- [ ] 旋转屏幕 → 分辨率跟随、画面不崩
- [ ] 锁屏 → 解锁，恢复渲染
- [ ] 连续 20 次切后台/切回无泄漏（`adb shell dumpsys meminfo` 或 Tracy）
- [ ] 返回键能干净退出（`Application` 析构链完整，无 Vulkan 校验层警告）

---

## 8. 阶段 F：渲染特性门槛与降级

### 8.1 设备能力硬门槛盘点

引擎**无条件要求**下列能力，任何一项缺失都会直接抛异常：

| 要求 | 位置 | Android 覆盖风险 |
|---|---|---|
| `apiVersion >= 1.3` | `VulkanContext.cpp:195` | **主要风险**，见 §4.4 |
| `VK_KHR_swapchain` | `VulkanContext.cpp:104`（Required） | 全覆盖 |
| `VK_EXT_extended_dynamic_state` | `:105`（Required）+ 特性 `:224-225` | Vulkan 1.3 设备标配 |
| `VK_EXT_descriptor_indexing` + `shaderSampledImageArrayNonUniformIndexing` | `:108` + `:230-231`（Required） | CSM 逐片元选片依赖，Vulkan 1.3 设备标配 |
| `dynamicRendering` + `synchronization2` | `:217-222`（核心 1.3） | 同上 |
| 呈现队列 == 图形队列 | `VulkanRenderContext.cpp:45` 直接取 `GetQueueByFlags(eGraphics, 0)` | Android 单队列设备普遍成立，但**没有校验** |

**建议**：做成启动期自检并给出**清晰的人话报错**（「设备不支持 Vulkan 1.3 动态渲染，本游戏需要 Android 13+」），而不是让异常裸奔。同时给物理设备选择补上**呈现支持校验**（`VulkanContext.cpp:191-200` 现在只按 `apiVersion` 挑第一个，不查 present 支持）。

### 8.2 呈现模式

`Application.cpp:191` 把 `VsyncMode::OFF` 映射到 `eMailbox`。很多 Android 设备**只保证 `eFifo`**。

好消息：`VulkanSwapchain.cpp:66-90` 的 `choose_present_mode` **已有降级链**（按优先级找第一个支持的，全不支持则硬回退 `eFifo`）。不会崩，只是刷警告。

**建议**：Android 侧把 `VsyncMode::OFF` 也映射到 `eFifo`（移动端本来就必须靠 vsync 控功耗与帧率），消灭警告并拿到正确帧节奏。

### 8.3 其他移动端相关

- **`GetDpiFactor` 驱动 UI 缩放**：见 §5.1
- **`Application::SetFrameRateLimit`**：桌面休眠式限帧（`Application.cpp:144-152`）在移动端不节能，应交给 vsync 节流
- **热**：移动端长时高负载会降频，桌面调参出的画质档位需按设备重调

---

## 9. 阶段 G：打包、部署与调试

### 9.1 构建流水线

```
glslc（主机）──► assets/shaders/glsl/*.spv
                      │
gepack（主机 CLI）────► dist/{game.cfg, manifest.json, assets/**}
                      │
Gradle 任务 ──────────► 拷贝 dist/assets → APK 的 src/main/assets/
                      │
externalNativeBuild ──► CMake → libGE_Runtime.so → APK 的 lib/arm64-v8a/
```

`gepack` 产出的是**散目录树**（`Packager.cpp:157-170` 按规范形拷到 `staging/assets/<canonical>`，`:285` 提交为 `dist/`），**正好可以直接喂给 APK 的 `assets/`**，无需额外解包。等打包计划书阶段 D 的 `.gepak` 落地后，也可改成打一个 `.gepak` 进 assets，由 VFS 的 AAsset backend 再套一层 pak 后端（两层 VFS 组合）。

> **已落地（M2.5，见 §6.0.1）**：Gradle 侧新增 `copyGameAssets` 任务，把 `dist/assets/**` 与 `dist/game.cfg` 拷到 `build/generated/gepackAssets`，该目录注册进 `sourceSets.main.assets`，并挂在 `merge*Assets.dependsOn` 上（与 `externalNativeBuild` 一同先于资产合并完成）。用生成目录而非 `src/main/assets`：不污染源码树。`dist/` 缺失时**明确报错**并给出 gepack 命令 —— 不静默产出空 assets 的 APK，因为"APK 出来了"与"能出画面"是两回事。`dist/` 仍待在**真机验证通过后**才从 `.gitignore` 里解除（APK 体积问题见 §10 风险 14）。

**需要新增 Gradle task 做拷贝**，并让它依赖 `gepack` 与着色器编译产物，保证顺序。

### 9.2 部署

- `adb install -r app-debug.apk`
- 日志：`adb logcat`（配合 `android_sink`）+ `adb pull /data/data/<pkg>/files/GE.log`
- **Tracy**：Android 支持，但需 manifest 声明 `INTERNET` 权限，并 `adb forward tcp:8086 tcp:8086` 后由桌面 Profiler 连接。符号解析需**未 strip 的 `.so`**——Release 打包注意保留符号或单独产出符号文件
- 现有 `build.bat` 只覆盖 MSVC；新增 `build_android.bat`（或直接靠 Gradle）

### 9.3 验收

- [ ] 一条命令产出可安装 APK（含 assets 与着色器）
- [ ] 真机安装启动，能进场景、能操作、能退出
- [ ] Tracy 能从桌面连上真机，看到 zone 与帧时间

---

## 10. 风险与开放问题

1. **【阶段 A 已发生】场景文件里的 `ToggleKey` 是 GLFW 键码，无法自动迁移**：`FollowCameraComponent::ToggleKey` 是**唯一**被序列化的键码字段（`SceneSerializer.cpp:822` 写、`:1484` 读），默认 `Key::V` 在 GLFW 是 **86**、在 SDL 是 **25**。场景格式**没有版本号**，故无法在加载时判定"这份场景是换库前还是换库后存的"，也就无法自动重映射。仓库内 `2/3/4.scene` 已就地更新为 25 并随本阶段提交；**仓库外的场景需手工改，或在编辑器检查器里重新设一次切换键**。若日后还想加别的序列化键码字段，先给场景格式加版本号。（不要用启发式猜：86 与 25 都是合法 scancode，猜错会静默绑到错误的键。）

2. **【阶段 A 已发生】`GetWidth/GetHeight` 的语义由逻辑尺寸改为像素尺寸**：改用 `SDL_GetWindowSizeInPixels`，因为引擎拿这个值当 swapchain 的 extent（`Application.cpp` 的 `RecreateSwapchain`）。GLFW 的 `glfwGetWindowSize` 返回逻辑尺寸，所以**高 DPI 显示器上现在会按真实像素渲染**——这是修正而非回归，100% 缩放下无差异。回归验证时请在非 100% 缩放的多显示器环境上确认 UI 尺寸与视口仍正确（`GetDpiFactor`/`GetContentScaleFactor` 已改为 `SDL_GetWindowDisplayScale`）。

3. **Vulkan 1.3 设备门槛（最高风险）**：引擎把 `dynamicRendering` / `synchronization2` 当核心 1.3 特性无条件启用，`apiVersion >= 1.3` 是硬门槛（`VulkanContext.cpp:195, 217-222`）。**Android 上的实际覆盖率本轮未能查证（网络受限）**，开工第一步应真机 `vulkaninfo` 验证。若覆盖率不足，需补 `VK_KHR_dynamic_rendering` / `VK_KHR_synchronization2` 扩展回退——**会牵动 `RenderGraph.cpp`（`:41-99`）与 `VulkanImage.cpp`（`:250-291`）里大量 `*FlagBits2` 用法，是独立的一大块，不要与移植混在一个阶段。**

4. **阶段 A 的编辑器回归（SDL 迁移的主要风险）**：editor 的 docking、node-editor、ImGuizmo 全部坐在 ImGui 平台后端上。**缓解手段就是这个风险可以在完全不碰 Android 的情况下独立验证**——所以阶段 A 必须**先独立做完并全量回归**，再开 Android。不要为了"省一个阶段"把换库和上新平台揉在一起。

5. **SDL3 在 Android 上的 surface 归属待真机确认**：窗口销毁/重建时 `VkSurfaceKHR` 是 SDL 自动重建还是需应用显式重建？**我无法离线确认**。见 §7.2，开工时先用最小用例验证。

6. **【阶段 A 已解决】`InputState` 容量与 SDL scancode 上界**：已核实 `SDL_SCANCODE_COUNT == 512`，合法 scancode 为 0..511，而 `kKeyCapacity = 512` 的位集索引范围正是 0..511 —— **刚好覆盖、零余量**。因零余量且 SDL 的 400..500 是动态键码保留区（Android 软键盘可能落在其中），按键事件翻译层已加越界丢弃（`SdlWindow` 里 `kMaxScancode` 那道判断），`Input::IsKeyPressed` 也做了双重防护。**这是不能动的一处脆弱平衡**：日后若有人改小 `kKeyCapacity` 会静默越界。

7. **`.gitmodules` 已腐烂**：路径指向不存在的 `engin/vendor/*`，`git submodule status` 直接报错（`no submodule mapping found ... 'GE/third_party/tracy'`）。阶段 A 已按计划**走普通拷贝绕开它**（SDL 作为普通文件入库，与 imgui/entt 一致）。修 `.gitmodules` 本身是笔独立小债，**别顺手修**（会牵连所有第三方目录）。另：SDL 上游自带的 `CLAUDE.md` / `AGENTS.md` 已改名 `UPSTREAM-*.disabled` 并忽略 —— 那是 SDL 对「向其上游提交」的贡献政策，对消费方无约束力，但嵌套的 `CLAUDE.md` 会被当作项目指令文件自动加载。

8. **Surface 重建的句柄继承陷阱**：`VulkanRenderContext::UpdateSwapchain`（`:380-387`）通过「旧 swapchain 当 `oldSwapchain`」构造新 swapchain，**会继承旧 surface 句柄**。surface 真被销毁后是悬垂。必须加重载，见 §7.2。这是最容易「桌面上永远测不出来、上真机就随机崩」的一类问题。

9. **`m_FrameActive` 未设置的潜伏断言**（`VulkanRenderContext.cpp:166-170`）：桌面难触发，Android 会变成可复现崩溃。阶段 E 必修。

10. **Lua `require` 绕过 VFS**（与打包计划书 §9.2 同一条）：`package.path` 是运行期机制（`ScriptEngine.cpp:921-922`），静态扫描收不全，打进 APK 后 stdio loader 直接失效。**必须补自定义 searcher**（VFS 读 + `luaL_loadbuffer`）。漏了的表现是「脚本静默不执行」，极难定位。

11. **miniaudio 读不到 APK 内资源**：`ma_sound_init_from_file`（`AudioContext.cpp:121`）/ `ma_decoder_init_file`（`SoundAsset.cpp:22`）走 `fopen`。`ma_decoder_init_memory` 最省事但会把整首 BGM 读进内存；要保留流式必须自定义 `ma_vfs`。**建议首版先 memory**，音频资产体积上来了再换 `ma_vfs`。

12. **`assets/` 大部分在 `.gitignore` 里**（打包计划书 §9.1）：`assets/models|materal|audio|environments|HDRI` 均未入库。**克隆下来的仓库没有这些资产**，Android 打包前必须先本地备齐，否则 `gepack` 会如实报「文件不存在」。

13. **CJK 字体仍不可移植（阶段 A 只做了止血）**：`ImGuiLayer.cpp` 硬编码 `C:\Windows\Fonts\msyh.ttc`。阶段 A **没有**换成随包字体 —— 仓库里没有任何 CJK 字体，而选一个随包字体（动辄 10MB+，要子集化、要决定是否接受进 APK）是产品取舍，不该由换库顺手决定。故只加了 `#ifdef GE_PLATFORM_WINDOWS` 守卫：非 Windows 上安静跳过并告警，而不是硬失败。**真正的解法在阶段 D**：字体随包 → 经 VFS 用 `AddFontFromMemoryTTF` 读入。

14. **APK 体积 —— 已有实测，比预估更严峻**：打包系统首次实跑（见 `游戏打包系统计划书.md` §5.7）得到 `2.scene` 的完整资产树为 **216 MB**，其中**单个 `environments/DaySkyHDRI065B/skybox.ktx2` 就占 192 MB（80%）**。加上已产出的 39 MB APK（`libGE_Runtime.so` 38.8 MB），**APK 会到 ~250 MB**。
    - 这对于 Google Play 是超限风险（常规 APK 上限 150 MB，超出需走 Asset Delivery / 分包），且 `assets/` 在 APK 内默认为压缩存储，**安装后解压 + 首次加载都会明显变慢**。
    - **主因是资产本身**（6.5K 级 HDRI 用在天空盒上属过采样），不是引擎或打包流程。**建议在开阶段 G 之前先处理**：降分辨率重烘（体积可掉一个数量级、肉眼几乎无差）→ 必要时再上 ASTC 压缩（引擎已链 KTX + astcenc）→ 最后才考虑 Play Asset Delivery 分包。
    - 这条与 `游戏打包系统计划书.md` §9.12 是同一条，两边都要盯。

15. **`ImGui` 在 Android 上是否必要**：`ImGuiLayer.cpp:33-34` 已把 `ViewportsEnable` 注释掉、只开 docking，所以没有多视口问题。但 `GE_Runtime` 本来不带编辑器 UI，**运行时几乎用不到 ImGui**——可考虑 Android 上直接关掉，省一大块复杂度和启动耗时。**值得评估**（若 `GameLayer` 依赖 ImGui 做调试面板则不能关）。

16. **触摸交互模型**：引擎的相机控制（`GameLayer.cpp` 的鼠标捕获 + `Window::GetCursorPosition`，阶段 A 已从 GLFW 收编到窗口接口）是为鼠标设计的。Android 上自由视角相机需要重做成虚拟摇杆 / 拖拽手势，**这是产品层的设计工作，不是移植工作**，但会挡住「能跑起来之后真的能玩」这一步。

17. **桌面回归风险**：本计划全程要求桌面零回归。最大风险点是阶段 A（换窗口库，动 `Application` / `ImGuiLayer` / `GameLayer` / `SceneLayer`）与阶段 D（VFS，动所有 loader）。**阶段 A 结束必须完整跑一遍编辑器 + `GE_Runtime`**。

18. **换平台输入后端时的核对清单（阶段 A 实测教训）**：这次「转向到边缘停住」的 bug 不是编译期能发现的，是**语义差异**类的。日后 阶段 E 再动输入路径时，逐条核对：

    - **锁定/相对态的坐标是否有界**？GLFW 无界（`GLFW_CURSOR_DISABLED`）、SDL 钳制在窗口内（`SDL_SetWindowRelativeMouseMode`）。引擎是用逐帧坐标差算增量的，**坐标一停增量就归零** —— 这是最容易漏且症状最迷惑的一项。
    - **模式切换瞬间的坐标来源是否突变**？切换前后的增量基准必须与首个增量同源，否则 delta 爆值。SDL 侧已在 `SetCursorMode` 里对齐累加器；但 `GameLayer` / `SceneLayer` 是**先** `ResetMouseBaseline(GetCursorPosition())` **再** `SetCursorMode`，跨越了来源切换 —— 当前未观察到问题故未改，若解锁时视角跳变，调换这两步顺序即可。
    - **事件是回调还是轮询**？GLFW 回调 / SDL 轮询，决定了事件泵放在哪一层。
    - **键码/按钮编号的基准**是否与既有持久化数据一致（见风险 1）。

19. **【M2.5 发现并已修】`.gemesh` 内嵌贴图槽可能是开发机绝对路径 —— 桌面 `dist/` 隔离跑同样复现**：`.gemesh` 不是自包含格式，材质贴图槽以字符串存盘。`CanonicalizeGEMeshEmbeddedRefs`（`GEMeshLoader.cpp:637`）**只在烘焙路径被调用**（`ModelLoader::ConvertToGEMesh`、`SceneSerializer::BakeSourceToGemesh`），运行期从不调用；而它依赖资源根的**绝对形**（`AssetPathUtil::IsUnderRoot` 要求入参是绝对路径且落在根下），Android 上不存在这个锚点 → 归一失败 → 贴图槽保持绝对路径 → VFS 读不到 → **模型发白且不报错**。实测 `dist/assets/models/shayv/未命名.gemesh` 内的 `albedoMap` 就是 `F:\yxy\project\GameEngine\assets\models\shayv\Ellen_Body_Map1_D.png`，说明这 7 个网格是在归一代码落地之前烘的。
    - **修法**：新增 `AssetPathUtil::CanonicalCandidates`（**根无关**，按 `/<资源根名>/` 段切出候选，不碰文件系统），由 `ModelLoader::CanonicalizeAssetRef` 用 `VFS::Exists` 定夺（能否读到是地面真值，不是"取第一个还是最后一个"的启发式）。并在 `MeshManager` 的异步 decode 与 `FinalizeGLTFMesh` 里收口 —— 这是运行期唯一把引用交给 `TextureManager` 的地方。
    - **注意这条在桌面也复现**：把 `dist/` 整个拷到无关目录再启动 `dist/GE_Runtime.exe`，精确判定会失败（绝对路径指向开发机而不是当前包），旧代码会**静默去读开发机上的那份资产**、看起来"正常"。这正是打包计划书里一直没做的「dist 隔离验证」会抓到的 bug，**它同时也是 M2.5 的地面判据之一**。
    - **建议的彻底修法（未做）**：用 `bin/gemesh.exe` 重烘那 7 个 `.gemesh`，让包内数据本身干净。代码侧的兜底仍应保留 —— 第三方/用户自烘的网格同样会带绝对路径。

20. **【M2.5 发现并已修】字体大小写不符 + `AddFontFromFileTTF` 失败是 abort 不是降级**：两件事叠加才致命。
    - `AssetPaths::Fonts` 是 `"fonts/opensans"`，磁盘上是 `assets/fonts/OpenSans`。Windows 大小写不敏感把它藏住了；Android 的 AAssetManager **大小写敏感**，直接查不到。这是全仓库**唯一**一处大小写不符（已用脚本核对 `AssetPaths` 全部常量与磁盘目录）。三个消费者同源：`ImGuiLayer.cpp`、`tools/gepack/AssetDependencyGraph.cpp:452`（固定集合），以及 gepack 产出的包内布局。
    - ImGui 的 `AddFontFromFileTTF` 在文件缺失时走 `IM_ASSERT_USER_ERROR(0, "Could not load font file!")`（`imgui_draw.cpp:3205`），而 `IM_ASSERT` 就是 `assert`（`imgui.h:98`），`GE_DEBUG` 在 `CMakeLists.txt:197` **无条件定义** —— 所以 Android Debug 构建下**字体加载失败会把进程 abort**，不是"UI 用小字号兜底"。
    - **修法**：`AssetPaths::Fonts` 改为 `"fonts/OpenSans"`；字体改用 `VFS::ReadAll` + `AddFontFromMemoryTTF`（缓冲区交给 atlas 托管），彻底不依赖路径能被 stdio 打开。
    - **一般教训**：换到大小写敏感的文件系统时，"从常量拼路径"的地方要逐个与磁盘比对，而不是等真机报"文件不存在" —— 这里连"文件不存在"都报不出来，是 abort。

21. **【M2.5 发现并已修】`std::filesystem::path::string()` 在 Windows 上产出反斜杠**：规范形一律正斜杠，凡是"路径会写进场景文件 / 交给 VFS"的地方都必须用 `generic_string()`。改动点：`DeriveAnimationBakePath`（`AnimationClipLoader.cpp`）、`DeriveGemeshOutPath` 与碰撞兜底（`SceneSerializer.cpp`）、OBJ 的 MTL 贴图解析、glTF 的 image uri 解析。**这条在 Windows 上永远测不出来**（反斜杠照样能打开文件），但会让 Android 上的资产查询全部落空 —— 与风险 19/20 同属"桌面测不出、真机才暴露"的语义差异类，也和 §3.0.1 那三类同族。**日后凡是"拼路径"的代码，先问一句：这个字符串会离开 Windows 吗？**

---

## 11. 里程碑

| 里程碑 | 内容 | 依赖 | 交付判据 |
|---|---|---|---|
| **M1** | 阶段 A：桌面 GLFW → SDL3 —— **代码完成、构建通过（四目标零错零警告），待跑完 §3.5 回归清单** | — | 编辑器 + `GE_Runtime` 在 SDL3 下**全量回归通过**（§3.5 清单）；GLFW 从依赖中移除 ✅；**此时仍未碰 Android** ✅ |
| **M2** | 阶段 B：构建骨架 —— **完成（`0953972b`）** | M1 | Gradle 产出可安装 APK ✅；`.so` 导出 `SDL_main` ✅；manifest 声明 Vulkan 1.3 ✅；**但装上必崩**（§4.0 尾部的两道坎） |
| **M2.5** | **【新增】可启动 + 有画面** —— 这是原 M2 里「真机能出画面」的真实依赖。**代码完成（`41648f33` + `c1aa0c6c` + `bbaf48e6`），待真机验证** | M2 + 阶段 D/G | 修 `Log::Init` 崩溃 → 真有资产可加载（VFS + 入包）→ 真机启动看到场景 |
| **M2b** | 阶段 C 剩余：触摸 / 软键盘 IME | M2.5 | 真机可触摸操作，软键盘能输入 |
| **M3** | 阶段 D：VFS + 可写目录 | M2 | 真机能完整加载并渲染一个场景（含贴图/网格/动画/音频/Lua）；桌面零回归 |
| **M4** | 阶段 E：生命周期与 Surface 重建 | M3 | 切后台/切回/旋转/锁屏均不崩，连续 20 次无泄漏 |
| **M5** | 阶段 F + G：特性门槛 + 打包部署 | M4 | 一条命令产出可安装 APK；Tracy 能从桌面连上真机 |
| **M6** | 触摸交互（产品层） | M5 | 能在触屏上实际操控相机与游戏 |

**里程碑订正说明（2026-09-16）**：原 M2 把「Gradle 产出可安装 APK」与「真机能出画面」并列为同一格的判据，**这是错的**——本轮实测下来，产出 APK 只是构建骨架的事（阶段 B），而「能出画面」还卡在两处**与构建无关**的前提上：`Log::Init` 的启动崩溃（几行就能修）和**资产入包**（要大动 VFS + Gradle 接线）。故拆出 M2.5 作为真实的分界，避免下次又用「APK 出来了」误判成「快能玩了」。

**文档载体自注**：本文是阶段 A–G 的蓝图，开工时每阶段单独细化。**阶段 A（换窗口库）必须独立成 PR**——它是唯一一个在桌面侧横跨 `Application` / `ImGuiLayer` / `GameLayer` / `SceneLayer` 的改动，且与 Android 完全解耦，混在别的改动里回滚成本极高。**阶段 D 必须与打包计划书阶段 D 协同设计**（同一个 VFS，别做两套），建议合并成一个 PR 系列。**阶段 E 建议独立成 PR**（跨平台层与渲染层）。（与 `auto-git-commit` 惯例一致。）
