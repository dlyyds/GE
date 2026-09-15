/**
 * @file Packager.h
 * @brief gepack 的落盘侧：校验结论 → staging 拷贝 → 换入 → manifest / game.cfg。
 *
 * 拷贝策略是「先建 staging 目录、全部就绪后再换入」：原地拷贝一旦中途失败，半成品
 * dist 与完整 dist 无法区分，玩家拿到的是缺文件的包。manifest.json 在 staging 内
 * 最后写，作为「这份包完整」的标记。
 */

#pragma once

#include "AssetDependencyGraph.h"

#include <cstdint>
#include <filesystem>
#include <string>

namespace gepack {

struct PackOptions {
    std::filesystem::path assetRoot;  ///< 资源根
    std::filesystem::path outDir;     ///< 输出目录（dist/）
    std::filesystem::path runtimeExe; ///< 要一并拷入的 GE_Runtime.exe
    std::filesystem::path projectCfg; ///< 源 game.cfg（可空 = 不读，只写入口场景）
    std::string entryScene;           ///< 入口场景规范形
    bool strict = false;              ///< Warn 升 Error
    bool dryRun = false;              ///< 只收集+校验，不写盘
};

struct PackResult {
    bool success = false;
    std::string error;               ///< 失败原因（成功时为空）
    std::size_t fileCount = 0;       ///< 拷入的资产文件数
    std::uintmax_t bytes = 0;        ///< 资产总字节
    std::string manifestPath;        ///< 成功时的 manifest 路径
};

class Packager {
public:
    Packager(PackOptions options, AssetDependencyGraph graph);

    /// 打印校验报告 + （非 dry-run 时）落盘。返回值给出结论。
    bool Run(PackResult &result);

private:
    /// 校验报告；返回 false = 有 Error（或 strict 下有 Warn），不应产出 dist
    bool Validate() const;

    bool StageAssets(const std::filesystem::path &staging, PackResult &result);
    bool CopyRuntime(const std::filesystem::path &staging, std::string &error);
    bool WriteGameConfig(const std::filesystem::path &staging, std::string &error);
    bool WriteManifest(const std::filesystem::path &staging, const PackResult &result,
                       std::string &error);
    /// staging → outDir 的换入（Windows 不能原子替换非空目录，走改名）
    bool Commit(const std::filesystem::path &staging, std::string &error);

    PackOptions m_Options;
    AssetDependencyGraph m_Graph;
};

} // namespace gepack
