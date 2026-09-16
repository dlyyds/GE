#include "pch.h"

#include "Utils/PlatformUtils.h"

#include "Core/PlatformDetection.h"

#ifdef GE_PLATFORM_WINDOWS

// commdlg.h 依赖 Windows 类型（CALLBACK/HWND 等），必须先包含 windows.h。
#include <Windows.h>
#include <commdlg.h>

#include <SDL3/SDL.h>

#include "Core/Application.h"
#include <filesystem>
#include <system_error>

namespace GE {

namespace {

/**
 * @brief 取窗口的 Win32 HWND，用作文件对话框的 owner。
 *
 * SDL3 **取消了 `SDL_syswm.h`**（SDL2 的 `SDL_GetWindowWMInfo` 那套），原生句柄改为
 * 通过窗口 properties 提供。所以 `GetNativeWindow()` 返回的 `SDL_Window*` 需要这样
 * 再转一层才能拿到 HWND。
 */
HWND GetWindowHwnd() {
    auto *window = static_cast<SDL_Window *>(Application::Get().GetWindow().GetNativeWindow());
    if (!window) {
        return nullptr;
    }
    const SDL_PropertiesID props = SDL_GetWindowProperties(window);
    return static_cast<HWND>(SDL_GetPointerProperty(props, SDL_PROP_WINDOW_WIN32_HWND_POINTER, nullptr));
}

} // namespace

std::string FileDialogs::OpenFile(const char *filter, const char *initialDir) {
    OPENFILENAMEA ofn;
    CHAR szFile[260] = {0};
    CHAR currentDir[256] = {0};
    ZeroMemory(&ofn, sizeof(OPENFILENAME));
    ofn.lStructSize = sizeof(OPENFILENAME);
    ofn.hwndOwner = GetWindowHwnd();
    ofn.lpstrFile = szFile;
    ofn.nMaxFile = sizeof(szFile);
    std::string absInitialDir; // keep alive during dialog call; resolve relative paths against cwd
    if (initialDir && *initialDir) {
        std::error_code ec;
        std::filesystem::path p = std::filesystem::absolute(initialDir, ec);
        absInitialDir = ec ? initialDir : p.string();
        ofn.lpstrInitialDir = absInitialDir.c_str();
    } else if (GetCurrentDirectoryA(256, currentDir)) {
        ofn.lpstrInitialDir = currentDir;
    }
    ofn.lpstrFilter = filter;
    ofn.nFilterIndex = 1;
    ofn.Flags = OFN_PATHMUSTEXIST | OFN_FILEMUSTEXIST | OFN_NOCHANGEDIR;
    if (GetOpenFileNameA(&ofn) == TRUE) {
        return ofn.lpstrFile;
    }
    return {};
}

std::string FileDialogs::SaveFile(const char *filter, const char *initialDir) {
    OPENFILENAMEA ofn;
    CHAR szFile[260] = {0};
    CHAR currentDir[256] = {0};
    ZeroMemory(&ofn, sizeof(OPENFILENAME));
    ofn.lStructSize = sizeof(OPENFILENAME);
    ofn.hwndOwner = GetWindowHwnd();
    ofn.lpstrFile = szFile;
    ofn.nMaxFile = sizeof(szFile);
    std::string absInitialDir; // keep alive during dialog call; resolve relative paths against cwd
    if (initialDir && *initialDir) {
        std::error_code ec;
        std::filesystem::path p = std::filesystem::absolute(initialDir, ec);
        absInitialDir = ec ? initialDir : p.string();
        ofn.lpstrInitialDir = absInitialDir.c_str();
    } else if (GetCurrentDirectoryA(256, currentDir)) {
        ofn.lpstrInitialDir = currentDir;
    }
    ofn.lpstrFilter = filter;
    ofn.nFilterIndex = 1;

    ofn.lpstrDefExt = strchr(filter, '\0') + 1;
    ofn.Flags = OFN_PATHMUSTEXIST | OFN_OVERWRITEPROMPT | OFN_NOCHANGEDIR;
    if (GetSaveFileNameA(&ofn) == TRUE) {
        return ofn.lpstrFile;
    }
    return {};
}

} // namespace GE

#else // !GE_PLATFORM_WINDOWS

namespace GE {

// 非 Windows 平台的占位实现：文件对话框是纯桌面编辑器功能，运行时播放器不用它。
// Android 上「打开文件」没有意义（资产在 APK 内、由 gepack 决定），真正需要的是
// 系统文件选择器，属于阶段 G 之后的事。返回空串 = 用户取消，调用方已按此处理。
std::string FileDialogs::OpenFile(const char *, const char *) { return {}; }

std::string FileDialogs::SaveFile(const char *, const char *) { return {}; }

} // namespace GE

#endif // GE_PLATFORM_WINDOWS
