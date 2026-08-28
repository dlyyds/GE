#include "pch.h"
#include "Utils/PlatformUtils.h"

// commdlg.h 依赖 Windows 类型（CALLBACK/HWND 等），必须先包含 windows.h。
#include <Windows.h>
#include <commdlg.h>

#include "Core/Application.h"

namespace GE {

std::string FileDialogs::OpenFile(const char *filter, const char *initialDir) {
    OPENFILENAMEA ofn;
    CHAR szFile[260] = {0};
    CHAR currentDir[256] = {0};
    ZeroMemory(&ofn, sizeof(OPENFILENAME));
    ofn.lStructSize = sizeof(OPENFILENAME);
    ofn.hwndOwner = static_cast<HWND>(Application::Get().GetWindow().GetNativeWindow());
    ofn.lpstrFile = szFile;
    ofn.nMaxFile = sizeof(szFile);
    if (initialDir && *initialDir) {
        ofn.lpstrInitialDir = initialDir;
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

std::string FileDialogs::SaveFile(const char *filter) {
    OPENFILENAMEA ofn;
    CHAR szFile[260] = {0};
    CHAR currentDir[256] = {0};
    ZeroMemory(&ofn, sizeof(OPENFILENAME));
    ofn.lStructSize = sizeof(OPENFILENAME);
    ofn.hwndOwner = static_cast<HWND>(Application::Get().GetWindow().GetNativeWindow());
    ofn.lpstrFile = szFile;
    ofn.nMaxFile = sizeof(szFile);
    if (GetCurrentDirectoryA(256, currentDir))
        ofn.lpstrInitialDir = currentDir;
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