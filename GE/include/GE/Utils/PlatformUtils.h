#pragma once

#include <filesystem>
#include <string>

namespace GE {

class FileDialogs {
public:
    // These return empty strings if cancelled
    static std::string OpenFile(const char *filter, const char *initialDir = nullptr);

    static std::string SaveFile(const char *filter, const char *initialDir = nullptr);
};

class PlatformUtils {
public:
    /**
     * @brief 当前进程可执行文件所在目录（绝对路径，无尾部分隔符）。
     *
     * 用于以"exe 同级"定位资源根，使发行版不依赖启动时的工作目录。
     * 查询失败返回空路径。
     */
    static std::filesystem::path GetExecutableDirectory();
};

}