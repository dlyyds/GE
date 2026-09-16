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

    /**
     * @brief **可写**的用户数据目录（绝对路径）。日志 / 配置 / imgui.ini 落在这里。
     *
     * 与只读的资产根是两回事，Android 上尤其如此：资产在 APK 内只读，而进程的
     * 当前工作目录是 `/`，**不可写**——所以任何以相对路径写盘的操作在 Android 上
     * 必然失败。桌面端返回当前工作目录（与换 Android 之前的行为逐字一致）。
     *
     * 查询失败返回空路径，调用方须容忍（不要拼出 "/GE.log" 这种根目录下的路径）。
     */
    static std::filesystem::path GetUserDataDirectory();
};

}