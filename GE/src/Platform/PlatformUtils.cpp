#include "pch.h"

#include "Utils/PlatformUtils.h"

#include <SDL3/SDL.h>

namespace GE {

/**
 * @brief 跨平台平台工具实现（取代原先的 `Platform/Windows/WindowsPlatformUtils.cpp`）。
 *
 * 换用 `SDL_GetBasePath()` 后有两个实质变化：
 *
 * 1. **编码更稳**。原实现特意走宽字符 API（`GetModuleFileNameW`）并在注释里写明
 *    「安装路径含中文时窄字符会截断」。SDL 返回的是 **UTF-8**，中文路径因此天然可用，
 *    不再需要 wchar 往返。
 * 2. **返回值带尾部分隔符**。SDL 文档明确保证结尾有分隔符（Windows 是 `\\`），
 *    而原先的 `GetModuleFileNameW(...).parent_path()` 不带。调用方用
 *    `exeDir / "assets"` 拼接，带尾巴也能正确拼接，但为保持与旧行为一致（以及
 *    避免日志里出现 `bin\assets` 这类双斜杠），这里用 `parent_path()` 去掉尾分隔符。
 *
 * Android 注意（阶段 D 的作业，先记在这里）：SDL 文档说明 Android 上
 * `SDL_GetBasePath()` 返回 `"./"`，**不是 APK 路径** —— 因为 APK 内的资产要经
 * `AAssetManager` 而非文件系统。所以 `Application` 里「exe 同级 / CWD 找 assets」
 * 的那套决策在 Android 上必然失效，资产访问必须走 VFS，这与计划书阶段 D 一致。
 */
std::filesystem::path PlatformUtils::GetExecutableDirectory() {
    const char *base = SDL_GetBasePath();
    if (!base) {
        return {};
    }
    // path("F:/a/b/") 的 filename() 是空串、parent_path() 是 "F:/a/b"，正好去掉尾分隔符
    return std::filesystem::path(base).parent_path();
}

/**
 * @brief 可写用户数据目录。
 *
 * 桌面端刻意返回**当前工作目录**而不是 exe 目录：`GE.log` / `imgui.ini` 一直是
 * CWD 相对的（`dist/` 里能看到这两份文件，正是因为发行版从该目录启动），
 * 改成 exe 目录会是一次静默的桌面行为变更。要改应该单独成 PR。
 *
 * Android 端 CWD 是 `/`（不可写），必须换成应用私有目录。用 `SDL_GetPrefPath`
 * 而不是自己走 JNI：SDL 已封装好 Android 的 `getFilesDir()` 语义。
 * 注意它**不需要先 `SDL_Init`** —— Android 实现只依赖 `mActivityClass` 与 JavaVM，
 * 二者由 `nativeSetupJNI` 在 `SDL_main` 之前就设好了（见 SDL_android.c 的
 * `nativeSetupJNI` / `SDL_GetAndroidInternalStoragePath`）。
 */
std::filesystem::path PlatformUtils::GetUserDataDirectory() {
    std::error_code ec;

#ifdef GE_PLATFORM_ANDROID
    // 返回值由 SDL 分配、**带尾部分隔符**，调用方负责 SDL_free。
    // parent_path() 正好去掉尾分隔符（与 GetExecutableDirectory 同一手法）。
    char *pref = SDL_GetPrefPath("GE", "Runtime");
    if (!pref) {
        return {};
    }
    const std::filesystem::path dir = std::filesystem::path(pref).parent_path();
    SDL_free(pref);
    return dir;
#else
    const std::filesystem::path cwd = std::filesystem::current_path(ec);
    return ec ? std::filesystem::path{} : cwd;
#endif
}

bool PlatformUtils::IsAssetRootWritable() {
#ifdef GE_PLATFORM_ANDROID
    // 资产在 APK 内，AAssetManager 只读。运行期烘焙（.gemesh/.geanim）与材质另存为
    // 在 Android 上必须整体让位 —— 要求资产在打包前（gepack）就烘好。
    return false;
#else
    return true;
#endif
}

} // namespace GE
