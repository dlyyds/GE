#include "pch.h"
#include "Utils/PlatformUtils.h"

// commdlg.h 依赖 Windows 类型（CALLBACK/HWND 等），必须先包含 windows.h。
#include <Windows.h>
#include <commdlg.h>

#include "Core/Application.h"
#include <filesystem>
#include <system_error>

namespace GE {

std::filesystem::path PlatformUtils::GetExecutableDirectory() {
    // 走宽字符 API：安装路径含中文时窄字符会截断。
    // 缓冲区不足时 GetModuleFileNameW 返回 buf.size()（截断信号），翻倍重试。
    std::wstring buf(MAX_PATH, L'\0');
    for (;;) {
        const DWORD n = GetModuleFileNameW(nullptr, buf.data(), static_cast<DWORD>(buf.size()));
        if (n == 0) {
            return {};
        }
        if (n < buf.size()) {
            buf.resize(n);
            break;
        }
        buf.resize(buf.size() * 2);
    }
    return std::filesystem::path(buf).parent_path();
}

std::string FileDialogs::OpenFile(const char *filter, const char *initialDir) {
    OPENFILENAMEA ofn;
    CHAR szFile[260] = {0};
    CHAR currentDir[256] = {0};
    ZeroMemory(&ofn, sizeof(OPENFILENAME));
    ofn.lStructSize = sizeof(OPENFILENAME);
    ofn.hwndOwner = static_cast<HWND>(Application::Get().GetWindow().GetNativeWindow());
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
    ofn.hwndOwner = static_cast<HWND>(Application::Get().GetWindow().GetNativeWindow());
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

}