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

    /**
     * @brief 资产根是否可写（能否往资源根里落文件）。
     *
     * 桌面为 true；**Android 为 false** —— 资产在 APK 内，`AAssetManager` 是只读的。
     * 所有"往资产根写"的路径（材质另存为、`.gemesh` / `.geanim` 运行期烘焙、场景
     * 序列化）都必须先问这里，在只读环境下**跳过并告警**，而不是每帧让文件系统
     * 调用失败刷屏。
     */
    static bool IsAssetRootWritable();

#ifdef GE_PLATFORM_ANDROID
    /**
     * @brief 取本 APK 的 `AAssetManager`（资产读取的唯一入口），失败返回 nullptr。
     *
     * 返回类型是 `void*` 而非 `AAssetManager*`：这个头文件不该把 `<android/asset_manager.h>`
     * 拉进来（它只在 Android 构建里存在）。调用方按需 `static_cast`。
     *
     * SDL 只暴露 activity 与 JNIEnv（`SDL_GetAndroidActivity` / `SDL_GetAndroidJNIEnv`），
     * 资产管理器要自己反射调 `Activity.getAssets()` 再交给 `AAssetManager_fromJava`。
     * **不要求先 `SDL_Init`** —— 它只依赖 `nativeSetupJNI` 在 `SDL_main` 之前设好的
     * JavaVM 与 `mActivityClass`。
     *
     * 返回值在进程生命周期内有效（`AAssetManager_fromJava` 内部持全局引用），
     * 调用方**不要**释放。
     */
    static void *GetAndroidAssetManager();
#endif
};

}