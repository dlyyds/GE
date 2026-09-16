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

} // namespace GE
