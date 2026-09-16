/**
 * @file VFS.h
 * @brief 只读资产虚拟文件系统 —— 屏蔽"资产在哪"这个平台差异。
 *
 * 引擎所有资产读取都应该经过这里，而不是直接 `std::ifstream` / `fopen`：
 *
 * - 桌面：资产是磁盘上的散目录（后端 = `DiskVFS`，根 = exe 同级或 CWD 下的 `assets/`）
 * - Android：资产在 APK 里，只能经 `AAssetManager` 读（后端 = `AndroidAssetVFS`，根 = `assets/`）
 *
 * **路径一律是规范形**（`<相对资源根>/<子路径>`，正斜杠、无盘符、无前导资源根名），
 * 由 `AssetPathUtil::ToCanonical` 产出。规范形本来就是与物理位置无关的虚拟路径，
 * 所以它同时是打包系统（gepack）写进场景与 manifest 的口径 —— 这里不另立一套。
 *
 * 不可变前提：本层**只读**。写盘（日志 / 配置 / 烘焙产物）走
 * `PlatformUtils::GetUserDataDirectory()`，因为 Android 上资产根是只读的。
 *
 * 线程安全：`Init` / `Shutdown` 只在启动与退出时单线程调用；`ReadAll` / `ReadText` /
 * `Exists` 可被后台加载线程并发调用（网格解析与纹理解码都在后台线程读盘）。
 */

#pragma once

#include "Core/Base.h"

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace GE::VFS {

/// 当前后端（仅用于日志与自检）。
enum class Backend {
    None,         ///< 未初始化
    Disk,         ///< 桌面：磁盘目录
    AndroidAsset, ///< Android：APK 内的 assets/
};

/**
 * @brief 初始化（桌面 / 磁盘后端）。
 *
 * @param assetRootAbs 资产根的绝对路径。传空路径表示 VFS 不可用（读取一律失败）——
 *                     这比"悄悄退回 CWD"好：读不到资产时现象是模型全白、贴图不换，
 *                     极具误导性，所以宁可在启动时就报出来。
 */
void Init(const std::filesystem::path &assetRootAbs);

#ifdef GE_PLATFORM_ANDROID
/**
 * @brief 初始化（Android / AAssetManager 后端）。
 *
 * @param assetManager 由 `AAssetManager_fromJava` 取得，进程生命周期内保持有效。
 *                     传 nullptr 表示 VFS 不可用。
 */
void InitAndroid(void *assetManager);
#endif

/// 释放后端状态（进程退出前调用；不调用也无害）。
void Shutdown();

/// 是否已初始化。
bool IsInitialized();

/// 当前后端。
Backend GetBackend();

/**
 * @brief 读入整个文件。
 *
 * @param canonical 规范形路径
 * @param out       成功时被整体覆盖；**失败时不改动**（不是清空）
 * @return 是否成功
 */
bool ReadAll(const std::string &canonical, std::vector<uint8_t> &out);

/// @brief 读入整个文件（便捷形式）；失败返回空 vector。
std::vector<uint8_t> ReadAll(const std::string &canonical);

/**
 * @brief 读入文本文件（YAML / Lua / 着色器源码）。
 *
 * @return 文件内容；读取失败返回空串。
 */
std::string ReadText(const std::string &canonical);

/// 资产是否存在且可读。
bool Exists(const std::string &canonical);

} // namespace GE::VFS
