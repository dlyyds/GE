/**
 * @file AssetPathUtil.h
 * @brief 资产引用路径的规范形工具。
 *
 * 规范形（canonical form）= `<相对资源根>/<子路径>`：正斜杠分隔、无前导资源根目录名、
 * 不含盘符。伪键（`builtin:` / `solid:`）不是文件路径，原样保留不参与归一。
 *
 * 规范形是打包系统的基础：场景文件里只有写入规范形，资产才能脱离开发机的目录结构
 * 分发。反向地，历史场景中的绝对路径要靠 ToCanonical 归一后才可跨机器解析。
 */

#pragma once

#include <filesystem>
#include <optional>
#include <string>

namespace GE::AssetPathUtil {

/// 伪键（内置几何体 / 纯色纹理）——不是文件路径，不参与归一与文件解析。
bool IsPseudoKey(const std::string &path);

/// 分隔符归一：'\\' → '/'。
std::string NormalizeSeparators(const std::string &path);

/**
 * @brief abs 是否位于 root 之下（Windows 下大小写不敏感）。
 *
 * 相对 root 会先按当前工作目录绝对化，故 root 未显式设为绝对路径时同样可用。
 * 根目录自身不算"根之下"（它是目录引用，不是资产文件）。
 */
bool IsUnderRoot(const std::filesystem::path &abs, const std::filesystem::path &root);

/**
 * @brief 归一为规范形（相对资源根、正斜杠、无前导 "assets/"）。
 *
 * - 伪键 → 原样返回
 * - 相对路径：仅剥掉可能的资源根目录名前缀，其余视为已相对资源根
 * - 绝对路径：仅在资源根之下可归一，否则视为根外引用
 * - 空串 / 越界（".." 逃出根）/ 根外绝对路径 → nullopt
 *
 * @param raw       原始引用（相对资源根 / 带资源根前缀 / 绝对路径均可）
 * @param assetRoot 资源根目录
 */
std::optional<std::string> ToCanonical(const std::string &raw,
                                       const std::filesystem::path &assetRoot);

} // namespace GE::AssetPathUtil
