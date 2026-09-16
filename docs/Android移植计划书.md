# Android 移植计划书

> 状态：**计划中，未开工**
> 目标：让 `GE_Runtime`（发行版播放器）能作为 APK 装在 Android 设备上运行——即把引擎从「Windows + GLFW + 磁盘 assets」迁到「Android + ANativeWindow + APK 内嵌 assets」。
> 范围（已定）：**只移 `GE_Runtime`**。`GE_Editor` 不上 Android（依赖 ImGuizmo / node-editor / 多视口停靠 / `FileDialogs`，且其使用场景就是桌面）。
> 前置（**已完成**）：打包系统阶段 A 的「资产根决定论 + 路径规范形」是本计划的地基——见 §5.0。
> 关联：`游戏打包系统计划书.md`（**阶段 D 的 VFS 与本计划的 VFS 是同一个东西，必须合并设计**，见 §5.0）、`项目架构总览.md`

---

## 0. 一句话架构

```
   ┌──────────────────────────────── APK ────────────────────────────────┐
   │  lib/arm64-v8a/libGE_Runtime.so   ← GE + GE_Runtime 全部静态编进来   │
   │  assets/                          ← gepack 产出的 dist/assets 整棵   │
   │    ├─ game.cfg                                                     │
   │    ├─ shaders/glsl/*.spv   fonts/   textures/   models/   ...       │
   │    └─ scenes/*.scene                                                │
   │  AndroidManifest.xml                                                │
   └───────────────────────────────┬─────────────────────────────────────┘
                                   │ AAssetManager（只读）
                                   ▼
   android_main ──► android_native_app_glue ──► 等首个 APP_CMD_INIT_WINDOW
        │                                              │
        │                                              ▼
        │                                    ANativeWindow ──► VkSurfaceKHR
        │                                                        (VK_KHR_android_surface)
        ▼
   Application（沿用现有 ctor / Run 语义）
        ├─ Window      ← 新增 GE/src/Platform/Android/AndroidWindow.cpp
        ├─ Input       ← 新增 AndroidInput.cpp（AInputEvent → 引擎事件 + ImGui 双投递）
        ├─ VFS         ← 新增：Disk 后端（桌面）/ AAsset 后端（Android）
        └─ Renderer    ← 复用现有 Vulkan 栈，仅补 surface 重建 + SURFACE_LOST 处理
```

**关键判断**：这不是「移植」，是**新增一个平台后端 + 补一层资源访问 + 重做生命周期模型**。引擎的抽象缝已经切好了（`Window` / `GEInput` / `AssetManager`），所以改动是**收敛的**，不是散弹式的。

**关键发现（决定了整个计划的形态）**：

1. `GE/include/GE/Core/PlatformDetection.h:31-33` —— `GE_PLATFORM_ANDROID` 已经定义好了，下一行就是 `#error "Android is not supported!"`。缝是留着的。
2. `GE/src/Render/VulkanBase/VulkanContext.cpp:83-84` —— **`VK_KHR_android_surface` 分支已经写好了**，由 `VK_USE_PLATFORM_ANDROID_KHR` 选中。实例创建几乎零改动。
3. `GE/third_party/imgui/backends/imgui_impl_android.{h,cpp}` —— **仓库里已经有 ImGui 官方 Android 后端**（`ImGui_ImplAndroid_Init/HandleInputEvent/Shutdown/NewFrame`）。不用手写 ImGui 平台层。
4. **打包计划书的「路径规范形」恰好就是 VFS 路径**——`AssetPathUtil` 的规范形是 `<相对资源根>/<子路径>`，本来就是「与物理位置无关的虚拟路径」。所以 VFS 不需要新的路径体系，直接复用。**这意味着本计划的 VFS 应该就是打包计划书阶段 D 的 VFS，只做一次，不要为 Android 另起一套。**

---

## 1. 现状盘点与差距定位

| 能力 | 开工前现状 | 本计划归属 |
|---|---|---|
| 平台检测 | `GE_PLATFORM_ANDROID` 已定义但紧接 `#error`（`PlatformDetection.h:31-33`） | 阶段 A ✅ 移除 `#error` |
| 窗口抽象 | `Window` 纯虚接口完备（`GEWindow.h`），工厂是 `#ifdef GE_PLATFORM_WINDOWS` 单分支（`Window.cpp:10-16`） | 阶段 B 加 Android 分支 |
| 窗口实现 | 只有 `GlfwWindow`；**GLFW 无 Android 后端** | 阶段 B 新增 `AndroidWindow` |
| 输入 | `GEInput.h` 是 5 个静态方法的门面；唯一 GLFW 绑定 TU 是 `WindowsInput.cpp`；`InputState.h`（bitset 状态机）完全平台中立 | 阶段 B 新增 `AndroidInput` |
| 键码 | `KeyCodes.h` / `MouseCodes.h` **是 GLFW 常量的逐字复制**（`Space=32, A=65, Escape=256, F1=290, LeftShift=340`） | 阶段 B 补 `AKEYCODE_* → KeyCode` 映射表 |
| Surface 扩展 | `VulkanContext.cpp:83-84` 已有 Android 分支 | ✅ 无需改动 |
| Surface 生命周期 | 创建一次（`VulkanContext.cpp:120-124`），仅在 `Destroy()` 释放（`:259-262`）；**无重建路径** | 阶段 D 新增 `RecreateSurface` |
| `VK_ERROR_SURFACE_LOST_KHR` | **全代码库无处理**（只有 `VulkanCommon.cpp:50` 的枚举→字符串映射）。`OUT_OF_DATE` 已处理（`VulkanRenderContext.cpp:151-153, 210-212`） | 阶段 D |
| 应用生命周期 | `Application::Run()` 是「窗口关闭 = 退出」（`Application.cpp:96-155`）；`ShouldClose()` 驱动 | 阶段 D |
| 资源访问 | `AssetManager::ResolvePath` 返回**绝对文件系统路径**（`AssetManager.cpp:44`）；~7 处 `std::ifstream`；20+ 文件用 `<filesystem>` 直读 | 阶段 C |
| VFS | **不存在**。唯一候选缝是 `FileSystem::ReadBinaryU32`（`FileSystem.h:11`，一个 36 行的 ifstream 包装，只被着色器加载用） | 阶段 C（= 打包计划书阶段 D） |
| 着色器 | `.spv` **不内嵌**，运行期从资源根读（`Renderer3D_Lifecycle.cpp` / `Renderer2D.cpp` 28 处硬编码文件名） | 阶段 C 走 VFS |
| 可写目录 | 全部写 CWD：`GE.log`（`Log.cpp:15`）、`imgui.ini`（ImGui 默认，`io.IniFilename` 未覆盖）、`game.cfg` | 阶段 C |
| 入口点 | `EntryPoint.h:7,19` 整个 `main` 包在 `#ifdef _WIN64` 里 | 阶段 A |
| 构建 | CMake + MSVC/Ninja（`build.bat`），Windows-only 目标一堆 | 阶段 A |

### 1.1 平台耦合点实测（改动面到底有多大）

好消息：**GLFW 泄漏到抽象层外的位置是有限且已知的**，共 4 处：

| 位置 | 用法 | 处理 |
|---|---|---|
| `GE/src/Core/Application.cpp:101` | `glfwGetTime()` 取主循环墙钟 | 换成 `std::chrono::steady_clock`（已有 `frameStart` 就是它，直接复用） |
| `GE/src/Render/ImGui/ImGuiLayer.cpp:67-68,128,134` | `ImGui_ImplGlfw_InitForOther/Shutdown/NewFrame` | `#ifdef` 分支到 `ImGui_ImplAndroid_*` |
| `GE_Runtime/src/GameLayer.cpp:70-71,121,134-142` | 鼠标捕获（`glfwSetInputMode(GLFW_CURSOR_*)` + `glfwGetCursorPos`） | 抽成 `Window::SetCursorMode()/GetCursorPos()`，两端各自实现 |
| `GE/include/GE/GE.h:24` | 公开头里 include 了 `Platform/Windows/GlfwWindow.h` | 条件编译 |

另有 `GE/include/GE/Core/GEWindow.h` 的 `GetGlfwWindow()` 是接口上的**具名泄漏**（按名字暴露了 GLFW）。阶段 B 应把它降级为平台内部的 `GetNativeWindow()`，或保留但在 Android 返回 `ANativeWindow*`——**建议保留 `GetGlfwWindow()` 不动**，因为改它要动 `ImGuiLayer` / `GameLayer` / `SceneLayer`，而阶段 B 本来就要动这三处，一起改更干净。

**顺带发现的死代码**：`Window::GetRequiredSurfaceExtensions()`（`GEWindow.h:120`）在 GLFW 侧用 `glfwGetRequiredInstanceExtensions` 实现（`GlfwWindow.cpp:305-312`），但**全代码库无人调用**——实例扩展实际由 `VulkanContext::ApplyDefaultExtensions()` 自己决定。Android 侧实现可留空/返回空 vector，不必照抄。

### 1.2 第三方依赖就绪度

| 状态 | 依赖 | 说明 |
|---|---|---|
| ✅ 开箱可用 | Vulkan + VMA、Jolt Physics、EnTT、glm、yaml-cpp、Lua 5.4、KTX-Software、SPIRV-Cross、stb、tinygltf、tinyobjloader、Tracy | 均有 Android 支持或纯头文件 |
| ⚠️ 需接线（不改库） | **spdlog 1.17.0** — `sinks/android_sink.h` **已存在于 vendored 副本**，换 sink 即可 | `Log.cpp:9-30` |
| ⚠️ 需换调用方式 | **miniaudio 0.11.25** — 自动选 AAudio/OpenSL ES 后端；但 `ma_sound_init_from_file`（`AudioContext.cpp:121`）/ `ma_decoder_init_file`（`SoundAsset.cpp:22`）走的是 `fopen`，读不到 APK 内资源 | 阶段 C：`ma_decoder_init_memory` 或自定义 `ma_vfs` |
| ⚠️ 需换后端 | **ImGui 1.92.8 WIP** — 平台后端换 Android（`imgui_impl_android` 已在树内），Vulkan 后端复用 | `ImGuiLayer.cpp` |
| ❌ 不可用 | **GLFW** — 官方只支持 Win32/Cocoa/X11/Wayland，**没有 Android 后端** | 保留给桌面，Android 另写 |

**构建侧反而简单**：CMake 已在，NDK toolchain 直接吃。`CMAKE_MSVC_RUNTIME_LIBRARY`（`CMakeLists.txt:8`）对非 MSVC 无害；`find_package(Vulkan REQUIRED)`（`:65`）换成 NDK 自带 `libvulkan.so`；`VK_USE_PLATFORM_WIN32_KHR`（`:162`）换成 `VK_USE_PLATFORM_ANDROID_KHR`。**`glslc` 是主机工具**，着色器编译流程一行不用改。

---

## 2. 阶段总览

| 阶段 | 内容 | 依赖 | 优先级 |
|---|---|---|---|
| **A** | 构建骨架：CMake/NDK/Gradle/Manifest/入口点 | — | **P0（地基）** |
| **B** | Android 平台后端：Window / Input / 工具类 / 去 GLFW 化 | A | **P0** |
| **C** | 资源 VFS（AAssetManager）+ 可写目录分流 | A | **P0** |
| **D** | 应用生命周期与 Surface 重建 | B、C | **P0** |
| **E** | 渲染特性门槛与降级 | D | P1 |
| **F** | 打包、部署与调试（gepack → APK / adb / Tracy） | C | P1 |

**推荐开工顺序：A → B → C → D（这四个做完才能看到画面），随后 E → F。**

---

## 3. 阶段 A：构建骨架

### 3.1 平台检测

移除 `PlatformDetection.h:31-33` 的 `#error "Android is not supported!"`，保留 `#define GE_PLATFORM_ANDROID`。`NDEBUG` 与 ARM NEON 相关宏由 NDK toolchain 自带，无需干预。

### 3.2 入口点

`EntryPoint.h` 的 `main` 保持 `#ifdef _WIN64` 不变（桌面零回归）。新增：

- `GE/include/GE/Core/AndroidEntryPoint.h` —— 复用同一份 `Log::Init()` + `CreateApplication` 契约，只是宿主从 `main` 换成 `android_main`
- 新目录 `platform/android/`：
  - `AndroidMain.cpp` —— `android_main(struct android_app*)` + 事件泵（阶段 D 细化）
  - `AndroidManifest.xml`、`build.gradle`、`gradle.properties`、`CMakeLists.txt`（Gradle 侧的 `externalNativeBuild` 入口）

**必须让 Android 走与桌面完全相同的 `CreateApplication` 路径**，否则 `RuntimeApp` / `GameLayer` 的状态机会漂移（打包计划书 §9.7 记录过同类风险）。

### 3.3 CMake 改造

现有 `CMakeLists.txt` 里的 Windows-only 目标在 Android 下必须整体跳过。用 `if(ANDROID)` / `if(NOT ANDROID)` 分区：

| 目标 | Android 下 |
|---|---|
| `add_subdirectory(GE/third_party/glfw)`（`:36`） | **跳过** |
| `add_subdirectory(GE/third_party/imgui)` 系列（`:38-40`） | 保留 ImGui，跳过 `ImGuizmo` / `imgui-node-editor`（编辑器专用） |
| `add_subdirectory(GE/third_party/tracy)`（`:43`） | 保留（可关） |
| `GE_Editor`（`:252`）、`gemesh`（`:268`）、`gepack`（`:284`） | **跳过**（`gepack` 是主机侧工具，见 §8） |
| `GE_Runtime`（`:302`） | **改为 SHARED**，产出 `libGE_Runtime.so` |
| `find_package(Vulkan REQUIRED)`（`:65`） | 换 `find_library(VULKAN_LIB vulkan)` + NDK 头 |
| `VK_USE_PLATFORM_WIN32_KHR`（`:162`） | 换 `VK_USE_PLATFORM_ANDROID_KHR` |
| `assets/shaders/glsl`（`:311`） | 保留（`glslc` 主机侧执行，产物进 assets） |

`GE_SRC` 的 `file(GLOB ...)` 里 `${CMAKE_SOURCE_DIR}/GE/src/Platform/Windows/*.cpp`（`:109`）需改为按平台选目录。

### 3.4 API level 与 ABI

**建议 `ANDROID_PLATFORM=android-33`（Android 13）、`ANDROID_ABI=arm64-v8a`。** 依据：引擎要求 `apiVersion >= 1.3`（`VulkanContext.cpp:195` 无条件硬门槛），并把 `dynamicRendering` / `synchronization2` 当**核心 1.3 特性**启用（`:217-222`），没有扩展回退路径。

> ⚠️ **待核实（本轮网络受限未能查证）**：Vulkan 1.3 在 Android 上的设备覆盖范围，以及 CDD 是否强制 API 33+ 设备支持 1.3。开工第一步应在真机跑 `vulkaninfo` 确认。若需下探到 API 31 或更低，**必须补 `VK_KHR_dynamic_rendering` / `VK_KHR_synchronization2` 扩展回退**——那是阶段 E 的主要工作量，且会牵动 `RenderGraph.cpp` / `VulkanImage.cpp` 里大量的 `*FlagBits2` 用法。**建议先用 33 把链路跑通，不要把降级和移植混在一个阶段。**

### 3.5 验收

- [ ] `ndk-build`/Gradle 能产出 APK，且 `libGE_Runtime.so` 正确打入 `lib/arm64-v8a/`
- [ ] 桌面构建（`build.bat`）**零回归**——`GE_Editor` / `GE_Runtime` / `gemesh` / `gepack` 全部照常

---

## 4. 阶段 B：Android 平台后端

### 4.1 `AndroidWindow`

新文件 `GE/src/Platform/Android/AndroidWindow.{h,cpp}`，实现 `GEWindow.h` 的 `Window` 接口。核心映射：

| `Window` 接口 | Android 实现 |
|---|---|
| `CreateVulkanSurface(instance)` | `ANativeWindow*` → `VkAndroidSurfaceCreateInfoKHR`（`vk::createAndroidSurfaceKHR`） |
| `GetNativeWindow()` | 返回 `ANativeWindow*` |
| `GetWidth/GetHeight` | 来自 `ANativeWindow_getWidth/Height`，随 `APP_CMD_WINDOW_RESIZED` 更新 |
| `GetDpiFactor` / `GetContentScaleFactor` | 来自 `AConfiguration` 密度（ImGui 字号缩放与 UI 尺寸依赖它） |
| `SetVSync` / `GetVSync` | 仅记录状态，实际由 swapchain 呈现模式决定 |
| `SetMaximized` / `SetTitle` / `SetWindowMode` / `SetResizable` / `Resize` | **空实现**（Android 无对应概念）。注意 `Application.cpp:74` 调了 `SetMaximized(true)`，必须保证 Android 侧是安静的空操作而不是断言 |
| `OnUpdate` / `ProcessEvents` | 空实现——事件由 `android_main` 的事件泵驱动，不是 GLFW 那种 `glfwPollEvents` |
| `ShouldClose` / `Close` | 由 Activity 生命周期驱动（返回键 / `APP_CMD_DESTROY`），见阶段 D |

`Window.cpp:10-16` 的工厂加 `#elif defined(GE_PLATFORM_ANDROID)` 分支。

新增接口方法（阶段 B 顺带做，桌面侧用 GLFW 实现）：
- `SetCursorMode(CursorMode)` / `GetCursorPos()` —— 收编 `GameLayer.cpp:70-71,134-142` 的 `glfwSetInputMode(GLFW_CURSOR_*)` / `glfwGetCursorPos`

### 4.2 `AndroidInput`

`GEInput.h` 是 5 个静态方法的门面，实现放 `GE/src/Platform/Android/AndroidInput.cpp`。**事件→状态的转换路径已经是平台中立的**（`Scene::OnEvent` → `InputState::Record*`，`Scene.cpp:1509-1534`），所以这一层只需忠实翻译。

两件必须做对的事：

1. **键码映射表**：`KeyCodes.h` / `MouseCodes.h` 是 GLFW 常量（`InputState.h:25-26` 按 512/16 的容量硬编码，不能动）。补 `AKEYCODE_* → KeyCode` 映射，重点覆盖：字母数字、方向键、`Escape`(256)、`Enter`(257)、`Shift/Ctrl/Alt`(340+)、`F1`(290+)。Android 软键盘不出键码而是走 IME，要单独处理（喂给 ImGui 的 IME 通路）。
2. **触摸 → 鼠标合成**：`Scene` 与 `InputState` 的交互模型是鼠标（`MouseMovedEvent` / `MouseButtonPressed` / `MouseScrolled`）。单指触摸合成为左键 + 绝对坐标移动，双指捏合合成为 `MouseScrolledEvent`。

**双投递顺序**：`AInputEvent` 必须**同时**喂给两条通路——ImGui（`ImGui_ImplAndroid_HandleInputEvent`，UI 优先吃掉）和引擎事件系统。ImGui 有 `WantCaptureMouse` / `WantCaptureKeyboard`，应作为「是否继续投递给引擎」的门闸，否则点 UI 会同时转相机。

### 4.3 去 GLFW 化（4 处泄漏）

按 §1.1 表格逐项处理。其中 `ImGuiLayer.cpp` 的分支形态：

```
#ifdef GE_PLATFORM_ANDROID
    ImGui_ImplAndroid_Init(nativeWindow);
#else
    ImGui_ImplGlfw_InitForOther(window, true);
#endif
```

Vulkan 后端两侧共用（`ImGui_ImplVulkan_Init`，`UseDynamicRendering = true`）。

**同时必须修的两处 Windows 假设**：
- `ImGuiLayer.cpp:54-55` 硬编码了 `C:\Windows\Fonts\msyh.ttc` 作为 CJK 字体——Android 上必然失败。改为从资源根加载字体（字体文件进 assets）
- `ImGuiLayer.cpp:41-43` 的 `OpenSans-Regular.ttf` 走 `ResolvePath`，阶段 C 后自动走 VFS，无需专门改

### 4.4 验收

- [ ] Android 上能创建 Vulkan 实例 + surface，`vkCreateAndroidSurfaceKHR` 不报错
- [ ] 触摸/按键能驱动 `InputState`（可用日志或 ImGui 面板观测）
- [ ] 桌面输入零回归

---

## 5. 阶段 C：资源 VFS + 可写目录

### 5.0 与打包计划书阶段 D 合并（重要）

打包计划书 §6 的「阶段 D（可选）：单文件资源包 `.gepak` + 虚拟文件系统」与本阶段**要的是同一个 VFS**。Android 的 `AAssetManager` 后端不过是**第一个非磁盘 VFS 实现**。

**因此：不要为 Android 另起一套资源读取层。** 直接落地打包计划书阶段 D 的 VFS 抽象，Android 后端作为它的第二个实现（磁盘 / AAsset）。收益：

- 打包计划书的阶段 D 从「可选」变成「已被 Android 需求倒逼落地」，顺带把 `.gepak` 单文件分发能力也铺好
- 打包计划书 §9.2 警告的「Lua `require` 绕过 VFS 直接命中真实文件系统」是**同一个坑**，一次修好两处受益

### 5.1 VFS 接口

现在唯一候选缝是 `FileSystem::ReadBinaryU32`（`FileSystem.h:11`）——太窄（只有读、无 stat、无 open/seek）。扩成一个最小的只读接口：

```
VFS::ReadAll(path) -> std::vector<std::byte>     // 整文件读，覆盖绝大多数调用点
VFS::ReadText(path) -> std::string
VFS::Exists(path)   -> bool
VFS::OpenRead(path) -> std::unique_ptr<Reader>   // 流式读（音频用）
```

两个后端：`DiskVFS`（`std::filesystem` + `ifstream`，桌面）/ `AndroidAssetVFS`（`AAssetManager_open` + `AAsset_read`，Android）。进程启动时装一个全局实例。

**路径语义很重要**：VFS 路径就是 `AssetPathUtil` 的**规范形**（`<相对资源根>/<子路径>`，正斜杠、无前导资源根名、无盘符）——见 `AssetPathUtil.h` 的规范形定义。`AssetPathUtil` 本身是**纯字符串逻辑、不访问文件系统**（`AssetPathUtil.cpp:41-48` 明确注明），所以**一行都不用改**，原样复用。

⚠️ **`AssetManager::ResolvePath` 返回绝对文件系统路径（`AssetManager.cpp:44`）这个语义要改**：Android 上不存在「资源的绝对路径」。需要把它拆成两层——`ResolveCanonical(raw) -> 规范形`（供 VFS 用）与 `ResolveWritePath(raw) -> 绝对路径`（供可写目录用）。

### 5.2 需要改造的读取点

| 类别 | 位置 | 改法 |
|---|---|---|
| 二进制整读 | `FileSystem.cpp:10`、`VulkanShaderModule.cpp:43`、`GEMeshLoader.cpp:417` | `VFS::ReadAll` |
| YAML | `SceneSerializer.cpp:1163`、`SceneAssetScanner.cpp:117`、`MaterialManager.cpp:104`、`GameConfig.cpp:68` | `YAML::LoadFile(p)` → `YAML::Load(VFS::ReadText(p))`（4 处） |
| tinyobj | `OBJLoader.cpp:37` `tinyobj::LoadObj` | 读进内存 + `std::istringstream`，用 tinyobj 的 stream 重载 |
| tinygltf | `GLTFLoader.cpp:389-391` `LoadBinaryFromFile`/`LoadASCIIFromFile` | 换 `LoadBinaryFromMemory` / `LoadASCIIFromString`（tinygltf 原生支持） |
| Lua | `ScriptEngine.cpp:116-123` `ReadFileContents`；`:921-922` `package.path` | **必须补自定义 searcher**：用 VFS 读脚本 + `luaL_loadbuffer`。这是打包计划书 §9.2 点名的坑 |
| miniaudio | `AudioContext.cpp:121`、`SoundAsset.cpp:22` | 优先自定义 `ma_vfs`（保留流式解码，不把整首 BGM 读进内存）；简单起见可先用 `ma_decoder_init_memory` |
| 存在性判断 | `MeshManager.cpp:334`、`Texture.cpp:87`、`AnimationClipManager.cpp:72,108` | `VFS::Exists` |
| 环境贴图 | `EnvironmentMap.cpp:29-35`（3 个散文件 cubemap + BRDF LUT） | 走现有 `Texture::LoadFromFile*Async`，但底层换 VFS |

### 5.3 可写目录（必须与 VFS 分开的第二概念）

**只读资产根**与**可写用户目录**是两回事，Android 上后者只能是 `app->activity->internalDataPath`。新增 `PlatformUtils::GetUserDataDirectory()`：

- Windows：返回现状路径（exe 目录 / CWD），**桌面行为零变化**
- Android：`internalDataPath`

迁到该目录的东西：

| 文件 | 现状 | 去向 |
|---|---|---|
| `GE.log` | `Log.cpp:15` 相对 CWD | 用户目录；Android 另加 `spdlog::sinks::android_sink`（vendored spdlog 1.17.0 **已含** `sinks/android_sink.h`），可保留文件 sink 便于 adb 拉取 |
| `imgui.ini` | ImGui 默认相对 CWD（`io.IniFilename` 未覆盖） | 显式设成用户目录下的绝对路径 |
| `game.cfg` | `GameConfig.cpp:19,26,68` exe 同级 → CWD | **读取顺序改为**：用户目录（允许玩家覆盖）→ APK assets（随包发布） |
| 烘焙产物 `.geanim` / `.gemesh` / `.gemat` | `AnimationClipLoader.cpp:134`、`GEMeshLoader.cpp:394`、`MaterialManager.cpp:158-161` **写到资产根** | Android 上资产根只读，**必须重定向到用户目录**；读取时用户目录优先（overlay 语义） |

> **overlay 设计**：读取时「用户目录 → 资产 VFS」顺序回落，写入一律进用户目录。这既解决 Android 只读问题，也让桌面端「运行时烘焙缓存」不再污染仓库。**首版建议干脆禁掉 Android 上的运行期烘焙**（要求资产预先烘焙好，`gepack` 已有校验能力），把 overlay 作为后续增强——因为烘焙路径会写入 30+MB 级数据，移动端存储与耗时都难接受。

### 5.4 验收

- [ ] Android 上能完整加载场景：`.scene` + `.gemesh` + `.ktx2` + `.geanim` + `.gemat` + `.spv` + 音频 + Lua 脚本，全部从 APK 内读出
- [ ] `GE.log` / `imgui.ini` / `game.cfg` 落在用户目录，重启后配置保持
- [ ] 桌面端行为零回归（VFS 走 Disk 后端）
- [ ] Lua `require` 在 VFS 下可用（自定义 searcher 生效）

---

## 6. 阶段 D：应用生命周期与 Surface 重建

**这是本次移植风险最高、也最容易被低估的部分。** Android 的模型与 Windows 有本质差异。

### 6.1 启动时序重构

现状：`Application` ctor 里 `Window::Create`（`Application.cpp:34`）→ 立刻 `Renderer`（`:65`）→ Renderer ctor 内部创建 surface + swapchain（`Renderer.cpp:28,32-37,40`）。

问题：`android_main` 跑起来时**不保证已有可用窗口**，`ANativeWindow` 由 `APP_CMD_INIT_WINDOW` 送达。

**方案**：`android_main` 阻塞等首个 `APP_CMD_INIT_WINDOW`，拿到 `ANativeWindow` 后再构造 `Application`。这样 `Application` ctor 的语义**完全不变**，新逻辑全部收敛在平台层与渲染上下文内。备选（把 `Application` 改成生命周期感知、延迟初始化）侵入性大得多，不推荐。

### 6.2 Surface 重建

现状：surface 在 `VulkanContext.cpp:120-124` 创建一次，只在 `Destroy()`（`:259-262`）释放，**没有重建路径**。`VkSurfaceKHR` 由 `VulkanContext` 持有（`GetSurface()`，`VulkanContext.h:67`），**不在** `VulkanRenderContext` 里存成员（只在 ctor 收下后传给 swapchain，`Renderer.cpp:33`）。

需要新增：

1. `VulkanContext::RecreateSurface(ANativeWindow*)` —— 销毁旧 surface、以同一 `VkInstance` 建新 surface。物理设备/队列族不需要重选（同一设备，surface 变了但队列族能力不变）
2. **`VulkanRenderContext::UpdateSwapchain` 需要新重载** —— 现有 `UpdateSwapchain(const vk::Extent2D&, ...)`（`VulkanRenderContext.cpp:380-387`）是「拿旧 swapchain 构造新的、把旧的当 `oldSwapchain`」，**会继承旧的 surface 句柄**。surface 真被销毁时这是悬垂句柄。必须加一个带 `vk::SurfaceKHR` 参数的重载，让新 swapchain 用新 surface
3. 重建链路：`APP_CMD_TERM_WINDOW` → `waitIdle` → 释放 swapchain + 各 image 的 `RenderTarget` + surface；`APP_CMD_INIT_WINDOW` → `RecreateSurface` → 重建 swapchain → 重建 RenderTarget → 通知 ImGui
4. **ImGui 侧**：`ImGuiLayer::OnSwapchainRecreated`（`ImGuiLayer.cpp:155-160`）目前是刻意的空实现（Vulkan 后端每帧重查 swapchain）。需确认 `ImGui_ImplVulkan` 在 image count / format 变化时能自愈；若不能，需在重建时 `ImGui_ImplVulkan_Shutdown` + 重 `Init`

### 6.3 `VK_ERROR_SURFACE_LOST_KHR`

**全代码库无处理**。当前若发生，`vk::SurfaceLostKHRError` 会从 `acquireNextImageKHR` 直接抛出、无人接管。

`VK_ERROR_OUT_OF_DATE_KHR` 的处理已在 `VulkanRenderContext::BeginFrame`（`:147-170`）与 `Present`（`:210-212`）。Android 上 surface lost 是**常态**（切后台、锁屏、旋转），必须与 out-of-date 同级别对待：捕获 → 走 §6.2 的重建链路。

### 6.4 顺带修一个既有隐患

`VulkanRenderContext.cpp:166-170`：acquire 重试后仍非 `eSuccess` 时，函数释放信号量并 `return`，**但没有设置 `m_FrameActive`**。下一行 `Begin()` 就调 `GetActiveFrame()`，而它在 `m_FrameActive` 为假时断言失败（`:233`）。

桌面端这是难触发的潜伏路径，**Android 上 surface lost / 频繁重建会让它变成可复现的崩溃**。阶段 D 必须一并修掉。

### 6.5 生命周期状态机

| Android 命令 | 处理 |
|---|---|
| `APP_CMD_INIT_WINDOW` | 建/重建 surface + swapchain |
| `APP_CMD_TERM_WINDOW` | 释放 surface + swapchain（**保留** device / 资产 / 场景） |
| `APP_CMD_WINDOW_RESIZED` | 走现有 `RecreateSwapchain` 路径（`Renderer.cpp:198-212`），等价于桌面 `WindowResizeEvent` |
| `APP_CMD_GAINED_FOCUS` / `LOST_FOCUS` | 合成 `WindowFocusEvent` / `WindowLostFocusEvent`（事件枚举**已存在**但当前无生产者，`Event.h:15-30`） |
| `APP_CMD_PAUSE` / `STOP` | 停渲染循环、`AudioContext` 暂停；**不要**析构 `Application`（进程可能随时被恢复） |
| `APP_CMD_RESUME` | 恢复循环 |
| `APP_CMD_LOW_MEMORY` | 打日志（后续可触发资产缓存回收） |
| `APP_CMD_DESTROY` / 返回键 | 唯一真正的退出路径 → 析构 `Application` |

**`Application::Run()` 的语义要改**：现在是 `while (m_Running)` + `ShouldClose()`。Android 上「窗口关闭」不等于「退出」，需要一个与窗口解耦的退出信号（如 `Application::RequestExit()`），由 `APP_CMD_DESTROY` / 返回键触发。

### 6.6 验收

- [ ] 切后台 → 切回，画面正常恢复（surface 重建链路走通）
- [ ] 旋转屏幕 → 分辨率跟随、画面不崩
- [ ] 锁屏 → 解锁，恢复渲染
- [ ] 连续 20 次切后台/切回无泄漏（配合 Tracy 或 `adb shell dumpsys meminfo`）
- [ ] 返回键能干净退出（`Application` 析构链完整，无 Vulkan 校验层警告）

---

## 7. 阶段 E：渲染特性门槛与降级

### 7.1 设备能力硬门槛盘点

引擎**无条件要求**下列能力，任何一项缺失都会直接抛异常：

| 要求 | 位置 | Android 覆盖风险 |
|---|---|---|
| `apiVersion >= 1.3` | `VulkanContext.cpp:195` | **主要风险**，见 §3.4 |
| `VK_KHR_swapchain` | `VulkanContext.cpp:104`（Required） | 全覆盖 |
| `VK_EXT_extended_dynamic_state` | `:105`（Required）+ 特性 `:224-225` | Vulkan 1.3 设备标配 |
| `VK_EXT_descriptor_indexing` + `shaderSampledImageArrayNonUniformIndexing` | `:108` + `:230-231`（Required） | CSM 逐片元选片依赖，Vulkan 1.3 设备标配 |
| `dynamicRendering` + `synchronization2` | `:217-222`（核心 1.3） | 同上 |
| 呈现队列 == 图形队列 | `VulkanRenderContext.cpp:45` 直接取 `GetQueueByFlags(eGraphics, 0)` | Android 单队列设备普遍成立，但**没有校验** |

**建议**：把上表做成启动期自检并给出**清晰的人话报错**（「设备不支持 Vulkan 1.3 动态渲染，本游戏需要 Android 13+」），而不是让异常裸奔。同时给 `PhysicalDevice` 选择补上**呈现支持校验**（`VulkanContext.cpp:191-200` 现在只按 `apiVersion` 挑第一个，不查 present 支持）。

### 7.2 呈现模式

`Application.cpp:191` 把 `VsyncMode::OFF` 映射到 `eMailbox`。很多 Android 设备**只保证 `eFifo`**。

好消息：`VulkanSwapchain.cpp:66-90` 的 `choose_present_mode` **已有降级链**（按优先级找第一个支持的，全不支持则硬回退 `eFifo`）。所以不会崩，只是会刷警告。

**建议**：Android 侧把 `VsyncMode::OFF` 也映射到 `eFifo`（移动端本来就必须靠 vsync 控功耗与帧率），消灭警告并拿到正确的帧节奏。

### 7.3 其他移动端相关

- **`GetDpiFactor` 驱动 UI 缩放**：Android 密度差异大（1.0~4.0），ImGui 字号与所有 UI 尺寸都要按它缩放，否则在高 DPI 设备上 UI 小到不可用。`ImGuiLayer` 需加 `style.ScaleAllSizes(dpi)`
- **`Application::SetFrameRateLimit`**：桌面休眠式限帧（`Application.cpp:144-152`）在移动端不节能，应交给 vsync / `Choreographer` 节流
- **热**：移动端长时高负载会降频，桌面调参出的画质档位需要按设备重调

---

## 8. 阶段 F：打包、部署与调试

### 8.1 构建流水线

```
glslc（主机）──► assets/shaders/glsl/*.spv
                      │
gepack（主机 CLI）────► dist/{game.cfg, manifest.json, assets/**}
                      │
Gradle 任务 ──────────► 拷贝 dist/assets → APK 的 src/main/assets/
                      │
externalNativeBuild ──► CMake → libGE_Runtime.so → APK 的 lib/arm64-v8a/
```

`gepack` 产出的是**散目录树**（`Packager.cpp:157-170` 按规范形拷到 `staging/assets/<canonical>`，`:285` 提交为 `dist/`），不是归档文件——**这正好可以直接喂给 APK 的 `assets/`**，不需要额外解包步骤。等打包计划书阶段 D 的 `.gepak` 落地后，也可以改成打一个 `.gepak` 进 assets，由 VFS 的 AAsset 后端再套一层 pak 后端读取（两层 VFS 组合）。

**需要新增一个 Gradle task 做拷贝**，并让它依赖 `gepack` 与着色器编译的产物，保证顺序。

### 8.2 部署

- `adb install -r app-debug.apk`
- 日志：`adb logcat`（配合 `android_sink`）+ `adb pull /data/data/<pkg>/files/GE.log`
- **Tracy**：Android 支持，但需在 manifest 声明 `INTERNET` 权限，并 `adb forward tcp:8086 tcp:8086` 后由桌面端 Profiler 连接。符号解析需要**未 strip 的 `.so`**——Release 打包时注意保留符号或单独产出符号文件
- 现有 `build.bat` 只覆盖 MSVC；需要新增 `build_android.bat`（或直接靠 Gradle）

### 8.3 验收

- [ ] 一条命令产出可安装 APK（含 assets 与着色器）
- [ ] 真机安装启动，能进场景、能操作、能退出
- [ ] Tracy 能从桌面连上真机，看到 zone 与帧时间

---

## 9. 风险与开放问题

1. **Vulkan 1.3 设备门槛（最高风险）**：引擎把 `dynamicRendering` / `synchronization2` 当核心 1.3 特性无条件启用，`apiVersion >= 1.3` 是硬门槛（`VulkanContext.cpp:195, 217-222`）。**Android 上的实际覆盖率本轮未能查证（网络受限）**，开工第一步应真机 `vulkaninfo` 验证。若覆盖率不足，需补 `VK_KHR_dynamic_rendering` / `VK_KHR_synchronization2` 扩展回退——**这会牵动 `RenderGraph.cpp`（`:41-99`）与 `VulkanImage.cpp`（`:250-291`）里大量的 `PipelineStageFlagBits2` / `AccessFlagBits2` 用法，是独立的一大块工作量，不要与移植混在一个阶段。**

2. **Surface 重建的句柄继承陷阱**：`VulkanRenderContext::UpdateSwapchain`（`:380-387`）通过「旧 swapchain 当 `oldSwapchain`」构造新 swapchain，**会继承旧 surface 句柄**。surface 真被销毁后这是悬垂。必须加重载，见 §6.2。这是最容易「在桌面上永远测不出来、上了真机就随机崩」的一类问题。

3. **`m_FrameActive` 未设置的潜伏断言**（`VulkanRenderContext.cpp:166-170`）：桌面难触发，Android 会变成可复现崩溃。阶段 D 必修。

4. **Lua `require` 绕过 VFS**（与打包计划书 §9.2 同一条）：`package.path` 是运行期机制（`ScriptEngine.cpp:921-922`），静态扫描收不全，且打进 APK 后 stdio loader 直接失效。**必须补自定义 searcher**（VFS 读 + `luaL_loadbuffer`）。这条如果漏了，表现是「脚本静默不执行」，极难定位。

5. **miniaudio 读不到 APK 内资源**：`ma_sound_init_from_file`（`AudioContext.cpp:121`）/ `ma_decoder_init_file`（`SoundAsset.cpp:22`）走 `fopen`。用 `ma_decoder_init_memory` 最省事但会把整首 BGM 读进内存；要保留流式必须自定义 `ma_vfs`。**建议首版先 memory**，音频资产体积上来了再换 `ma_vfs`。

6. **`assets/` 大部分在 `.gitignore` 里**（打包计划书 §9.1）：`assets/models|materal|audio|environments|HDRI` 均未入库。**克隆下来的仓库没有这些资产**，Android 打包前必须先本地备齐，否则 `gepack` 会如实报「文件不存在」。多人协作时这是个提交/分发策略问题。

7. **CJK 字体**：`ImGuiLayer.cpp:54-55` 硬编码 `C:\Windows\Fonts\msyh.ttc`，必须换成随包字体。中文 UI 字体动辄 10MB+，需要考虑子集化，否则直接推高 APK 体积。

8. **APK 体积**：Vulkan 引擎 + 中文字体 + 资产，未优化前轻松几百 MB。分包（`assets` 用 Play Asset Delivery）或资产压缩留到后续。

9. **`ImGui` 多视口**：`ImGuiLayer.cpp:33-34` 已把 `ViewportsEnable` 注释掉、只开 docking，所以没有多视口问题。但 docking 在移动端意义有限，`GE_Runtime` 本来也不带编辑器 UI，**运行时几乎用不到 ImGui**——可以考虑 Android 上直接关掉 ImGui 以省一大块复杂度和启动耗时。**这是个值得评估的简化点**（`GameLayer` 若依赖 ImGui 做调试面板则不能关）。

10. **触摸交互模型**：引擎的相机控制（`GameLayer.cpp` 的鼠标捕获 + `glfwGetCursorPos`）是为鼠标设计的。Android 上自由视角相机需要重做成虚拟摇杆 / 拖拽手势，**这是产品层面的设计工作，不是移植工作**，但会挡住「能跑起来之后真的能玩」这一步。

11. **桌面回归风险**：本计划全程要求桌面零回归。最大风险点是 §5 的 VFS 改造（动了所有 loader）与 §4.3 的去 GLFW 化（动了 `Application` / `ImGuiLayer` / `GameLayer`）。**每阶段结束都应完整跑一遍桌面编辑器 + `GE_Runtime`**。

---

## 10. 里程碑

| 里程碑 | 内容 | 依赖 | 交付判据 |
|---|---|---|---|
| **M1** | 阶段 A：构建骨架 | — | Gradle 产出能装进设备的空壳 APK；桌面 `build.bat` 零回归 |
| **M2** | 阶段 B + D：平台后端 + 生命周期 | M1 | 真机能出画面（纯色清屏即可），切后台/切回/旋转不崩 |
| **M3** | 阶段 C：VFS + 可写目录 | M1 | 真机能完整加载并渲染一个场景（含贴图/网格/动画/音频/Lua）；桌面零回归 |
| **M4** | 阶段 E + F：特性门槛 + 打包部署 | M2、M3 | 一条命令产出可安装 APK；Tracy 能从桌面连上真机 |
| **M5** | 触摸交互（产品层） | M4 | 能在触屏上实际操控相机与游戏 |

**文档载体自注**：本文是阶段 A–F 的蓝图，开工时每阶段单独细化。**阶段 C 必须与打包计划书阶段 D 协同设计**（同一个 VFS，别做两套），建议合并成一个 PR 系列；阶段 D 的 surface 重建建议独立成 PR，因为它跨平台层与渲染层，混在别的改动里回滚成本高（与 `auto-git-commit` 惯例一致）。
