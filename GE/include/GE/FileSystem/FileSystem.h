#pragma once

#include "Core/Base.h"
#include <cstdint>
#include <string>
#include <vector>

namespace GE {

/**
 * @brief 资产二进制的读取工具。
 *
 * 实现走 `VFS`，所以桌面（磁盘）与 Android（APK 内 assets）行为一致。
 * 入参是 `AssetManager::ResolveCanonical` 产出的**规范形**路径。
 *
 * 只保留 SPIR-V 模块需要的 `uint32_t` 视图 —— 通用整文件读请直接用
 * `VFS::ReadAll` / `VFS::ReadText`。
 */
class FileSystem {
public:
    /// 读取二进制文件为 uint32_t 序列（SPIR-V 模块专用；长度非 4 的倍数时截尾）。
    static std::vector<uint32_t> ReadBinaryU32(const std::string &canonicalPath);
};

}
