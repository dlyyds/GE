# Android 运行与打包流程（机制参考）

> 本文是**机制参考**：讲清「C++ 怎么在移动端跑起来」与「APK 怎么打出来」这两条流水线，
> 以及本工程在这两条链路上踩过的每一个坑。
>
> **与 `Android移植计划书.md` 的分工**：计划书管**分阶段做什么**（A–G、里程碑、验收判据）；
> 本文管**已经落地的那套东西是怎么转起来的**，供日后改构建、加平台、排查产物问题时查。
> 两者有重叠处以计划书为准（尤其是尚未完工的阶段 C/E/F）。
>
> 记录时间：2026-09-16（对应 `3d734b61` + 一次 EXT 动态状态崩溃修复）。

---

## 0. 一句话总览：移动端有两条并行的流水线

桌面端「编译出一个 exe，双击运行」的心智模型在 Android 上**彻底失效**。这里始终是两条线，
且必须**同时**正确才有画面：

```
流水线一（代码）  .cpp ──NDK/clang──> libmain.so ──塞进 APK 的 lib/<abi>/──┐
                                                                          ├──> APK
流水线二（数据）  assets/** ──gepack──> dist/ ──Gradle Sync──> assets/** ──┘
```

两条线各有各的「静默失败」模式：**流水线一错了**，Activity 照常显示、进程也在，只是 native
`main` 从不执行（看起来像"启动后自己退了"）；**流水线二错了**，程序照常运行，只是贴图全白、
模型发亮、音频无声。所以本文第 2、3 节末尾都各有一节「验收判据」，**不要用"构建成功"当判据**。

---

## 1. 与桌面端的四个根本差异

先把心智模型换掉，后面所有机制都是这四条的推论：

| # | 桌面 | Android | 推论 |
|---|---|---|---|
| 1 | `main()` 是入口，进程归你 | 进程归 **Activity**，代码在**它启动的线程**里跑 | 入口要被"重命名 + 反射调用"；生命周期是系统说了算（见 §2.4） |
| 2 | `fopen` / `std::ifstream` 读一切 | 资产在 **APK 压缩包内**，不是文件 | 读资产只能走 `AAssetManager`（见 §2.5） |
| 3 | 窗口系统 + 交换链 | **没有窗口系统**，只有一个 `ANativeWindow` | Vulkan 要 `VK_KHR_android_surface`；无 GLFW/Win32 后端 |
| 4 | 一个 ABI（x86_64） | 真机是 **arm64-v8a**，模拟器常是 x86_64 | 要么按 ABI 出多份 `.so`，要么挑一个（见 §2.3） |

外加第 5 条**只影响本工程**：引擎是 **Vulkan 1.3 硬门槛**（`SelectPhysicalDevice` 不达标直接抛），
而 Vulkan 1.3 的设备覆盖基本从 Android 13 起 —— 这条决定了 `minSdkVersion 33`（§2.2 末尾）。

---

## 2. 流水线一：C++ 在移动端跑起来的流程

### 2.1 工具链全貌

移动端的 C++ 编译器不是 MSVC，是 **NDK 里的 clang**（交叉编译到 Android）。全部版本：

| 组件 | 版本 | 位置 |
|---|---|---|
| NDK | `27.3.13750724`（r27d LTS） | `E:\software\android-sdk\ndk\27.3.13750724\` |
| SDK / build-tools | android-36 / `36.0.0` | `E:\software\android-sdk\` |
| JDK | Temurin 17.0.15 | `E:\software\jdk\jdk17` |
| AGP / Gradle | `8.13.2` / `8.14.5` | `platform/android/build.gradle`、`gradle/wrapper/` |

NDK 提供的三样东西决定了怎么编：

- **clang + sysroot**：sysroot 里自带 `vulkan/vulkan.h`（**注意路径是小写** `vulkan/`，
  不是 `Vulkan/` —— Windows 不区分大小写把这一点藏了很久）与 `android/asset_manager_jni.h`。
- **CMake toolchain file**：`$NDK/build/cmake/android.toolchain.cmake`，由 AGP 自动挂上，
  我们不需要手写 `-DCMAKE_TOOLCHAIN_FILE`。
- `ANDROID_PLATFORM` / `ANDROID_ABI` / `ANDROID_STL` 三个关键变量（见下）。

### 2.2 从 `.cpp` 到 `.so`：谁在什么时候调 CMake

构建是 **Gradle 驱动 CMake、CMake 驱动 Ninja、Ninja 驱动 clang** 的四层夹心：

```
gradlew assembleDebug
  └─ :app:externalNativeBuildDebug        ← AGP 的 task
       └─ cmake <仓库根>/CMakeLists.txt   ← 配置（path 写在 app/build.gradle:71）
            └─ ninja                      ← 编译
                 └─ clang++ (NDK)         ← 出 libmain.so
```

关键点：**Gradle 的 CMake 入口直接钉在仓库根**（`app/build.gradle:69-73` 的
`path '../../../CMakeLists.txt'`），而不是像 SDL 模板那样套一层 `jni/CMakeLists.txt`。
原因是引擎的 CMakeLists 里有 70+ 处 `${CMAKE_SOURCE_DIR}`，**只有让仓库根当顶层源目录它们才
解析正确** —— 不能再套一层 `add_subdirectory`。仓库根内部用 `if(ANDROID)` 分区，跳掉桌面专用目标
（`GE_Editor`、`gemesh`、`gepack`、`imguizmo`）。

三个编译变量（`app/build.gradle:33-46`）：

| 变量 | 取值 | 为什么 |
|---|---|---|
| `ANDROID_PLATFORM` | `android-33` | 与 `minSdkVersion` 一致；Vulkan 1.3 的现实下限 |
| `ANDROID_STL` | `c++_static` | 单个 `.so` 自包含，**不用往 APK 里塞 `libc++_shared.so`** |
| `abiFilters` | `arm64-v8a`, `x86_64` | arm64 是真机目标；x86_64 只为能在模拟器上验证"桌面测不出"的那条路径 |

**代价**：每个 ABI 各带一份 ~40 MB 的 `.so`（APK 因此从 101 MB 涨到 ~142 MB）。真机发行版
应只留 arm64。

Android 分支还在 CMake 里做了三件平台专属的事：

1. `VK_USE_PLATFORM_ANDROID_KHR`（`CMakeLists.txt:213-217`）—— 与桌面的
   `VK_USE_PLATFORM_WIN32_KHR` 二选一。`VulkanContext::ApplyDefaultExtensions` 里
   `VK_KHR_win32_surface` / `VK_KHR_android_surface` 两个分支**早就写好了**，这里选对宏即可。
2. `find_library(VULKAN_LIB vulkan REQUIRED)`（`:98-103`）—— Android 上
   `find_package(Vulkan)` 不可靠（NDK 不提供 Vulkan 的 CMake config 包），运行时是系统的
   `libvulkan.so`。
3. 额外链 `android`（`libandroid.so`，`:234-237`）—— `AAssetManager_fromJava` 在里面。

### 2.3 产物命名：必须叫 `libmain.so`

这是**最隐蔽的一个坑**，它的症状是「Activity 正常显示、进程活着、native `main` 一次都没跑」。

链路：SDL 的 Java 启动器 `SDLActivity.getMainSharedObject()` 取 `getLibraries()` 的**最后一项**，
拼成 `lib<名字>.so`，再从这个库里 `dlsym("SDL_main")`。SDL 默认给的是 `{"SDL3", …, "main"}`，
于是它找 `libmain.so`。

所以：

- CMake 侧 `set_target_properties(GE_Runtime PROPERTIES OUTPUT_NAME main)`（`CMakeLists.txt:375`）
  → 产出 `libmain.so`，而不是 `libGE_Runtime.so`。
- Java 侧**必须覆写 `getLibraries()`**（`platform/android/app/src/main/java/com/ge/runtime/GEActivity.java`）
  → 只返回 `{"main"}`。

第二条不是可选的。因为本工程把 SDL **静态链**进了自己的库（`SDL_SHARED=OFF` / `SDL_STATIC=ON`），
APK 里**没有 `libSDL3.so`**；而 `SDL.loadLibrary` 对 `UnsatisfiedLinkError` 是**抛出**不是忽略，
`loadLibraries()` 的 `for` 循环会在**第一个元素**就中断 —— `"main"` 从未被加载，SDLActivity 判定
`mBrokenLibraries=true` 后**永不启动 native 线程**。

### 2.4 入口点：`main` 被重命名，却依然是同一个 `main`

容易被"Android 要有 `android_main` / `android_native_app_glue`"的旧知识带偏。**SDL3 下不需要**：

```
Win32：  SDL 提供 main 与 WinMain，按链接子系统自动选，两者都转调 SDL_main
Android：`main`（即 SDL_main）由 SDLActivity 从 Java 侧经 JNI 调起，跑在 "SDLThread" 上
```

所以两端共用**同一个 `int main()`**：

- `GE/include/GE/Core/EntryPoint.h` 唯一 include `<SDL3/SDL_main.h>`（该头把 `main`/`WinMain`
  的**定义**直接写在头里，多个 TU 引入会重复符号 → **全程序只能有一个 TU 引它**）。
- 入口体只做转发：`return GE::Application::Main(argc, argv);`
  —— 转发而非直接写逻辑，是因为 `SDL_main.h` 会把 `main` 宏重命名成 `SDL_main`，
  而 `friend int ::main(...)` 是**按名字绑定**的友元，会和这个宏打架（C2248）。
- 桌面的 `#ifdef _WIN64` 守卫已可去掉（两端等价）。

线程上要记住一件事：**你的所有代码都跑在 `SDLThread` 上，不是 UI 线程** —— 崩溃日志里
`name: SDLThread` 就是它；UI 线程只属于 `SDLActivity`。

### 2.5 资产访问：`AAssetManager` + VFS

Android 上资产在 APK 里，**没有"exe 同级 assets"这回事，也无法用 `std::filesystem` 判存在性**。
本工程把这件事收成**唯一入口** `GE::VFS`：

```
GE/src/Core/Application.cpp:47-57
  Android: assetRoot = "assets"（纯虚拟根名，只为 ToCanonical 剥前缀用）
           VFS::InitAndroid(PlatformUtils::GetAndroidAssetManager())   ← JNI 取 AAssetManager
  桌面   : assetRoot = exe同级/assets，否则 CWD/assets；VFS::Init(assetRoot)
```

- 后端二选一：`DiskVFS`（桌面）/ `AndroidAssetVFS`（`AAssetManager_open` + `AAsset_read`）。
- 接口只有三个：`VFS::ReadAll` / `ReadText` / `Exists`。
- 路径一律是 `AssetPathUtil` 的**规范形**（`<相对资源根>/<子路径>`，正斜杠、无盘符、无根名）。
  `AssetManager::ResolvePath` 已拆成 `ResolveCanonical`（读）/ `ResolveWritePath`（写）。
- **`VFS::Init` 必须在任何资产读取之前**，所以它在 `Application` 构造里、`Renderer` 之前。
  宿主 CLI 工具（`gepack` / `gemesh`）不走 `Application` 构造，**也必须各自初始化**
  （踩过：漏了会把 7 个完好的 `.gemesh` 全报成"格式版本不符或已损坏"，报错方向被带偏）。

### 2.6 可写目录与日志

- 桌面 `GE.log` / `imgui.ini` 落 CWD；Android 走 `PlatformUtils::GetUserDataDirectory()`
  （`SDL_GetPrefPath`）→ 实测 `/data/data/com.ge.runtime/files/`。
- 另挂一个 `spdlog::sinks::android_sink`（tag **`GE`**）→ `adb logcat -s GE` 能直接看。
- **资产根只读**：`PlatformUtils::IsAssetRootWritable()` 在 Android 为假，5 处运行期烘焙
  写入点会安静跳过 → **要求资产在打包前烘好**。

### 2.7 平台差异清单（改这块代码前逐条核对）

| 类别 | 桌面 | Android | 备注 |
|---|---|---|---|
| Surface 扩展 | `VK_KHR_win32_surface` | `VK_KHR_android_surface` | 宏二选一，逻辑早就写好 |
| 已提升为核心的扩展 | 驱动通常仍列出扩展名 | **可以不列出**（实测模拟器不列 `VK_EXT_extended_dynamic_state`） | 见下面「坑 5」 |
| 验证层 | `VK_LAYER_KHRONOS_validation` 可用 | **没有验证层** | 故 debug utils 也要跟着降级 |
| 文件系统大小写 | 不敏感 | **敏感** | `fonts/opensans` vs `fonts/OpenSans` 只在 Android 现形 |
| `path::is_absolute()` | `F:\…` 为真 | **`F:\…` 为假**（POSIX 只认前导 `/`） | 见下面「坑 6」 |
| `path::string()` | 产出反斜杠 | —— | 会离开 Windows 的字符串一律 `generic_string()` |

### 2.8 本工程踩过的坑（全部桌面测不出）

1. **原生库必须叫 `libmain.so`** —— 见 §2.3。
2. **必须覆写 `getLibraries()`** —— 见 §2.3。
3. **已提升为核心的扩展不能按扩展名硬要求**：`VK_EXT_extended_dynamic_state`（→1.3）与
   `VK_EXT_descriptor_indexing`（→1.2）在 1.3 设备上可以合法消失，引擎却曾把它们列为
   Required → 直接启动失败，报错还指向一个"看起来必备"的扩展。修法：一张
   `PromotedExtension{name, coreVersion}` 表，判「扩展名 **or** 核心版本」二选一
   （`VulkanDevice.cpp:133-154`），特性启用也随之二选一。
4. **debug utils 要跟验证层走**：模拟器上 `VK_EXT_debug_utils` "可用且已启用"，于是用它给
   对象起名 → 在 `vkSetDebugUtilsObjectNameEXT` 里 SIGSEGV（驱动侧 `vk_common_*` 空指针）。
   改为「扩展可用 **且** 验证层已启用」才用真实实现（`VulkanInstance::IsLayerEnabled`），
   否则走 `DummyDebugUtils`。
5. **【本次新增】1.3 设备上不要调 EXT 后缀的入口点**：`flushDynamicStates` 原本调
   `setDepthTestEnableEXT` 等，而 EXT 名字的函数指针**只在扩展被启用时才由
   `vkGetDeviceProcAddr` 加载** —— 扩展被提升进核心后驱动可以不列出它，于是指针为 `null`，
   调用即**跳到地址 0**（`SIGSEGV`、`rip=0`，一帧内崩）。修法：一律用**核心 1.3 名字**
   （`setDepthTestEnable` / `setDepthWriteEnable` / `setDepthCompareOp` / `setStencilTestEnable`）。
   注意 `VkPhysicalDeviceVulkan13Features` **没有** `extendedDynamicState` 成员 ——
   提升后没有对应核心特性位，故 1.3 核心路径**什么都不用启用**。
   **桌面永远测不出**：桌面列出了该扩展，指针恰好是好的。
6. **【本次新增】绝对性判定不能只信 `path::is_absolute()`**：POSIX 上它只认前导 `/`，于是
   `F:\proj\assets\x.png` 在 Windows 判为绝对、在 Android 判为**相对** → 归一"成功"、无告警，
   最后静默读不到（症状：模型发白不报错）。这是 `.gemesh` 内嵌贴图槽残留开发机绝对路径
   那个老问题的**真因**。修法：按**字符串形态**判定盘符路径（`LooksLikeWindowsDrivePath`），
   与平台无关。
7. **大小写 + `IM_ASSERT` 叠加**：`AssetPaths::Fonts` 曾写 `fonts/opensans`，磁盘是
   `fonts/OpenSans`；而 `AddFontFromFileTTF` 在文件缺失时走 `IM_ASSERT`，**`GE_DEBUG` 无条件
   定义 → 直接 abort**，不是"降级成小字号"。修法：常量改对 + 字体走 VFS 读进内存
   （`AddFontFromMemoryTTF`），彻底不依赖路径能被 stdio 打开。

---

## 3. 流水线二：APK 打包流程

### 3.1 为什么资产要"先 gepack 再入包"

引擎在 Android 上只能从 **APK 内的 `assets/`** 读（经 AAssetManager）。而仓库**不含**
`models/materal/audio/environments/HDRI` 这些大资产（`.gitignore` 已排除），且场景只引用
其中一部分 —— 所以先由主机侧工具**按依赖图收全**，再交给 Gradle：

```
bin/gepack.exe --scene assets/scenes/2.scene --out dist
   └─ dist/assets/**  +  dist/game.cfg
```

`gepack` 的依赖图走 `SceneAssetScanner`（只吃 yaml-cpp，**不碰 GPU** —— 因为不存在无 GPU 的
读场景路径，`SceneSerializer::Deserialize` 全程经 `AssetManager` 触达 GPU）。

### 3.2 Gradle 侧：`copyGameAssets`（`Sync`）→ 生成目录 → `sourceSets.assets`

`platform/android/app/build.gradle:96-137` 是这段的全部：

```
dist/assets/**   ─┐
dist/game.cfg    ─┴─> Sync ─> app/build/generated/gepackAssets/   （该目录**本身就是 APK 的 assets 根**）
                                    │
                                    └─ sourceSets.main.assets.srcDir(...)  →  mergeDebugAssets → APK 的 assets/**
```

三个**很容易搞错**的点：

1. **注册进 `sourceSets` 的那个目录本身就是 assets 根**，所以 `dist/assets/**` 要拷到它的
   **顶层**，不能再套一层 `assets/` —— 否则 APK 里变成 `assets/assets/**`。构建照样"成功"，
   **只有解包才看得出**。
2. **必须用 `Sync` 而不是 `Copy`**：`Copy` **不 prune** 目标目录，改过结构后旧产物会继续留着
   （实测：APK 里 `assets/models/**` 与 `assets/assets/**` 同时存在，体积近乎翻倍）。
3. **拷到 `build/generated/` 而不是 `src/main/assets`**：不污染源码树（216 MB 的资产不该出现在
   `git status` 里），且 `dist/` 本身可重新生成。

任务顺序由 `applicationVariants.all` 钉住：`merge<Debug>Assets` **dependsOn**
`externalNativeBuild<Debug>` **与** `copyGameAssets`。

`copyGameAssets` 里还有两道**提前失败**的检查（值得保留的写法）：`dist/assets` 不是目录直接抛
`GradleException` 并打印"先跑 gepack"的完整命令；`dist/game.cfg` 缺失只 warn（运行时回退默认值）。

### 3.3 不被二次压缩的格式

`androidResources.noCompress`（`app/build.gradle:77-80`）列了 `ktx/ktx2/png/jpg/wav/gemesh/geanim/spv/ttf`
—— 这些格式**自身已经压缩过**，再塞进 zip 收益极低；而且这条也是将来做**流式读取**的前提
（`AAsset_seek` 只对未压缩资产有效）。

### 3.4 完整 task 链（一次 `assembleDebug` 的顺序）

```
externalNativeBuildDebug   → libmain.so（arm64-v8a + x86_64）
copyGameAssets (Sync)      → build/generated/gepackAssets/**
mergeDebugAssets           → 合并上面两者 → assets/**
compileDebugJavaWithJavac  → org.libsdl.app.* + com.ge.runtime.GEActivity
mergeDebugNativeLibs       → lib/<abi>/libmain.so
stripDebugDebugSymbols     → 剥符号（符号在 intermediates 里另有留存）
mergeDebugJniLibFolders / dexBuilderDebug / mergeProjectDexDebug / ...
packageDebug               → 打包 + 对齐 + 签名（debug keystore）
                           → app/build/outputs/apk/debug/app-debug.apk
```

### 3.5 构建命令（本机可用的确切写法）

本机 cmd **不搜索当前目录**找可执行文件（只写 `gradlew.bat` 会报 "is not recognized"，
**而 `echo $?` 仍可能是 0**，看起来像构建成功）；MSYS 又会吃掉 `/c`。故：

```bash
cd F:/yxy/project/GameEngine
rm -f platform/android/app/build/outputs/apk/debug/app-debug.apk   # 见 §3.7 孤儿字节
MSYS_NO_PATHCONV=1 JAVA_HOME='E:\software\jdk\jdk17' ANDROID_HOME='E:\software\android-sdk' \
  cmd.exe /c "cd /d F:\yxy\project\GameEngine\platform\android && .\gradlew.bat assembleDebug --console=plain"
```

不需要 `local.properties`；环境变量**内联覆盖**即可（不必等系统环境变量修好）。

### 3.6 装到设备/模拟器

```bash
ADB="E:/software/android-sdk/platform-tools/adb.exe"

# 模拟器（AVD 必须放 E: 盘：C: 只剩 5GB，而 dataPartition 要 6G）
#   -gpu host 是关键：把宿主 RTX 4060 直通给 guest，Vulkan 1.3 才可用
MSYS_NO_PATHCONV=1 ANDROID_AVD_HOME='E:\software\android-avd' ANDROID_SDK_ROOT='E:\software\android-sdk' \
  ./emulator.exe -avd medium_phone -gpu host

"$ADB" install -r -t platform/android/app/build/outputs/apk/debug/app-debug.apk
"$ADB" shell am start -n com.ge.runtime/.GEActivity
"$ADB" logcat -s GE              # 引擎的 android_sink
"$ADB" shell run-as com.ge.runtime cat files/GE.log   # 落盘日志
```

> **注意**：`adb shell` 的参数也会被 MSYS 改写（`/sdcard/x.png` 会变成 Windows 路径），
> 涉及设备内路径的命令前面要加 `MSYS_NO_PATHCONV=1`。

**注入输入事件做验证**（模拟器控制台支持真正的鼠标事件）：

```bash
"$ADB" emu event mouse <x> <y> <device> <buttonstate>   # device=0；buttonstate: 1=左 2=右
```

它的价值：这是**与宿主鼠标同一条路径**的注入，能复现"只有真机才出"的输入类问题。

### 3.7 验收判据（**不要**用"构建成功"当判据）

构建成功只说明 Gradle 没报错。产物的正确性要**解包看**：

```bash
# 1) 布局与关键条目
unzip -l app-debug.apk | grep -E "lib/.*/libmain.so|assets/game.cfg|assets/assets"   # 最后一项应无输出
unzip -l app-debug.apk | grep -c "^.*assets/"                                        # 资产数量对账

# 2) 孤儿字节（AGP 增量打包的坑，见下）
python -c "import zipfile;z=zipfile.ZipFile('app-debug.apk');print(z.start_dir - sum(i.compress_size for i in z.infolist()))"
#   非零 = 中央目录不索引的孤儿字节；unzip -l 完全看不出来

# 3) 非 ASCII 资产名是否原样（aapt2 实测逐字节保留，如 models/shayv/未命名.gemesh）

# 4) .so 的导出与引用
llvm-nm -C --defined-only libmain.so | grep -w SDL_main        # 应导出（T）
llvm-nm -C --undefined-only libmain.so | grep AAssetManager_fromJava   # 应有引用（U，证明链了 libandroid）
```

**静默漏收坑（写 build_android.bat 的对账时当场抓到）**：**AGP 的资源合并会忽略下划线开头的
目录** —— `environments/_default_cube/` 整目录被丢，构建照样成功，APK 里就是少一个文件。
已做对照实验确定规则边界：下划线开头的**文件**（`_topfile.txt`）**不受影响**，只有**目录**被忽略；
同一个 280 字节的文件改名为 `default_cube/` 后立刻入包（APK 条目 62 → 63）。
症状极具误导性：运行期日志只有一条 `Renderer3D: 加载默认天空盒纹理失败`，很容易被当成
VFS/路径问题去查 —— 而真因是那个文件**根本不在包里**。**结论：资产目录名一律不要以下划线开头。**

**孤儿字节坑**：`packageDebug` 是增量打包，**某个条目体积缩小时不回收旧空间、也不截断文件**。
把 192 MB 的 skybox 换成 48 MB 后，APK 实测仍是 245 MB，其中 **144 MB 是孤儿字节**。
做法：**改动会显著缩小资产时，先删掉旧 APK 再构建**（或 `gradlew clean`）。

### 3.8 真机/模拟器上排查原生崩溃

闪退**先分清两类**，判据完全不同：

| 现象 | 含义 | 去哪看 |
|---|---|---|
| `Fatal signal 11 (SIGSEGV)` + `libc` + tombstone | **原生崩溃** | `adb logcat` 的 crash buffer；`/data/tombstones/` |
| `System.exit called, status: 0`、`SDLActivity thread ends`、**无 tombstone** | **不是崩溃**，是 `main` 正常返回 / Activity 被销毁 | `logcat` 的生命周期行 |

第二轮要**把地址翻回源码行**（tombstone 里已带符号名与 `+offset`）：

```bash
NM="E:/software/android-sdk/ndk/27.3.13750724/toolchains/llvm/prebuilt/windows-x86_64/bin/llvm-nm.exe"
A2L=".../llvm-addr2line.exe"
SO="platform/android/app/build/intermediates/cxx/Debug/<hash>/obj/x86_64/libmain.so"   # 用**未 strip** 的

"$NM" -C --defined-only "$SO" | grep <崩溃的函数名>     # 取符号地址，例：0x119e750
# 崩溃地址 = 符号地址 + tombstone 里的 +offset（例：+1510 → 0x119ed36）
"$A2L" -e "$SO" -f -C -i 0x119ed36                    # → 精确到 file:line
```

**为什么值得这么做**：本次那条 `flushDynamicStates` 崩溃，靠"猜哪个调用是空指针"会猜错，
而 addr2line 一步给出 `VulkanPipelineState.cpp:501`。

---

## 4. 尚未完成（影响"能不能玩"，不影响"能不能跑"）

按 `Android移植计划书.md` 的阶段划分，当前真实状态：

| 阶段 | 状态 | 缺口的表现 |
|---|---|---|
| A 桌面 GLFW→SDL3 | 代码完成，待跑回归清单 | 桌面向后兼容风险 |
| B 构建骨架 | 完成 | —— |
| D 的**运行时读取侧** + G 的资产入包 | 完成（M2.5） | —— |
| **C 触摸 / 软键盘 IME** | 未开工 | **触屏无法操控相机**（相机仍为鼠标设计） |
| **E 生命周期与 Surface 重建** | 未开工 | **Activity 被销毁即退出**：`Android_OnDestroy` → `SDL_SendQuit()` → `main` 返回。切后台/旋转/系统回收都可能触发；且 `SDL_ANDROID_TRAP_BACK_BUTTON` 默认值 0 时，**右键/返回键会 finish 掉 Activity**（SDL 专门为这个场景留了该 hint，见 `SDLActivity.java:724-741`） |
| **F 特性门槛降级** | 未开工 | Vulkan 1.3 是硬门槛，低版本设备直接启动失败 |
| D 的**可写目录 overlay / `.gepak`** | 未开工 | 运行期烘焙在 Android 上被跳过 |

另有两处**已知但不致命**的问题：

- **音频初始化失败**：`AudioContext: failed to init sound "audio/…wav"`。miniaudio 仍用
  `ma_sound_init_from_file` 按**文件路径**打开，而 APK 内的资产不在文件系统上 → 应改为
  经 VFS 取内存（`ma_decoder_init_memory`）或实现 `ma_vfs` 后端。
- **`.gemesh` 内嵌贴图槽仍带开发机绝对路径**（历史资产）：运行期已有根无关兜底（§2.8 坑 6），
  但仍建议用 `bin/gemesh.exe` **重烘那 7 个 `.gemesh`**，让包内数据本身干净。

---

## 5. 文件与命令速查

| 用途 | 位置 |
|---|---|
| Gradle 模块配置（ABI / API / 资产入包） | `platform/android/app/build.gradle` |
| Activity（覆写 `getLibraries`） | `platform/android/app/src/main/java/com/ge/runtime/GEActivity.java` |
| 清单（`configChanges` / Vulkan 1.3 声明 / SDL_ENV hint） | `platform/android/app/src/main/AndroidManifest.xml` |
| 原生构建入口（**在仓库根**） | `CMakeLists.txt`（`:98-103`、`:213-240`、`:353-385`） |
| 入口点机制 | `GE/include/GE/Core/EntryPoint.h` |
| 资源根决策 / VFS 初始化 | `GE/src/Core/Application.cpp:45-82` |
| VFS 两个后端 | `GE/{include/GE,src}/FileSystem/VFS.*` |
| 路径归一（规范形） | `GE/{include/GE,src}/Render/AssetPathUtil.*` |
| 打包工具 | `tools/gepack/`（产出 `dist/`） |

```bash
# 资产 → dist（改过场景/资产后必跑）
bin/gepack.exe --scene assets/scenes/2.scene --out dist

# APK（见 §3.5）
# 装 + 跑 + 看日志（见 §3.6）
# 解包验收（见 §3.7）
```
