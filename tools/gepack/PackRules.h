/**
 * @file PackRules.h
 * @brief gepack 的打包规则：排除项与等级策略（外部 yaml，不打进代码）。
 *
 * 规则的边界：这里只放**策略**（哪些文件不收、哪些告警算致命）；
 * **字段名知识**（场景里哪些字段是资产引用）在 GE::ScanSceneAssetRefs，
 * 归一口径在 GE::AssetPathUtil。三者互不重复。
 */

#pragma once

#include <filesystem>
#include <string>
#include <vector>

namespace gepack {

struct PackRules {
    /// 排除模式（相对资源根的 glob：`**` 跨目录、`*` 段内、`?` 单字符）
    std::vector<std::string> excludes;

    /// 规则文件来源（写进 manifest 便于追溯）
    std::string sourcePath;
};

/**
 * @brief 从 yaml 读规则；文件不存在或读失败时返回内置默认规则（并回填 err 供告警）。
 */
PackRules LoadPackRules(const std::string &yamlPath, std::string &err);

/// 内置默认规则（yaml 缺失时的兜底，与 tools/gepack/gepack_rules.yaml 保持一致）
PackRules DefaultPackRules();

/**
 * @brief glob 匹配（对 '/' 分段的路径，`**` 可跨段）。
 *
 * 全路径与文件名都要能匹配：模式 `shaders/glsl/**` 匹配该目录下一切，
 * `**/*.pdb` 匹配任意层级的 .pdb。
 */
bool GlobMatch(const std::string &pattern, const std::string &path);

/// 是否被任一排除模式命中
bool IsExcluded(const PackRules &rules, const std::string &canonicalPath);

} // namespace gepack
