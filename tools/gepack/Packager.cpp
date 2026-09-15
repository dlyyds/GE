/**
 * @file Packager.cpp
 * @brief 校验报告 + staging 落盘 + 换入 + manifest/game.cfg 生成实现。
 */

#include "Packager.h"

#include "tinygltf/json.hpp"

#include <yaml-cpp/yaml.h>

#include <chrono>
#include <ctime>
#include <fstream>
#include <iostream>
#include <sstream>

namespace gepack {

namespace {

constexpr int kFormatVersion = 1;

/// 引擎尚未在构建期提供版本宏（CMake 的 project() 没带 VERSION），暂以常量占位：
/// manifest 里的版本用于将来「运行时自检拒绝不匹配的包」，接线到真实版本留到阶段 F。
constexpr const char *kEngineVersion = "0.1.0";

std::string NowIso8601() {
    const std::time_t now = std::time(nullptr);
    std::tm tm{};
#if defined(_WIN32)
    localtime_s(&tm, &now);
#else
    tm = *std::localtime(&now);
#endif
    char buf[32] = {0};
    std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%S", &tm);
    return buf;
}

const char *LevelName(ProblemLevel level) {
    switch (level) {
        case ProblemLevel::Error: return "错误";
        case ProblemLevel::Warn:  return "告警";
        default:                  return "信息";
    }
}

} // namespace

Packager::Packager(PackOptions options, AssetDependencyGraph graph)
    : m_Options(std::move(options)), m_Graph(std::move(graph)) {}

bool Packager::Validate() const {
    bool hasError = false;
    bool hasWarn = false;
    for (const Problem &p : m_Graph.Problems()) {
        if (p.level == ProblemLevel::Error) {
            hasError = true;
        } else if (p.level == ProblemLevel::Warn) {
            hasWarn = true;
        }
    }
    // 有 Error 一律不产出 dist（半成品包比没有包更危险）；--strict 额外把告警当错误
    if (hasError) {
        return false;
    }
    return !(m_Options.strict && hasWarn);
}

bool Packager::Run(PackResult &result) {
    std::size_t errorCount = 0;
    std::size_t warnCount = 0;
    for (const Problem &p : m_Graph.Problems()) {
        if (p.level == ProblemLevel::Info) {
            continue;
        }
        std::cout << "[" << LevelName(p.level) << "] " << p.message << "\n";
        if (p.level == ProblemLevel::Error) {
            ++errorCount;
        } else if (p.level == ProblemLevel::Warn) {
            ++warnCount;
        }
    }

    // 未被引用的资产：列出来但不当错误（动态加载/脚本拼路径的资产静态看不见）
    const auto &unused = m_Graph.Unused();
    if (!unused.empty()) {
        std::cout << "[告警] 未引用资产明细（前 20 条，共 " << unused.size() << " 条）:\n";
        for (std::size_t i = 0; i < unused.size() && i < 20; ++i) {
            std::cout << "        " << unused[i] << "\n";
        }
    }

    std::cout << "[信息] 资产 " << m_Graph.Nodes().size() << " 个 / "
              << m_Graph.TotalBytes() << " 字节；错误 " << errorCount << "、告警 "
              << warnCount << "\n";

    // 按类型分布（对账用：哪一类收多了/收少了，看这行最直观）
    static const AssetKind kKinds[] = {
        AssetKind::Scene,  AssetKind::Mesh,        AssetKind::Texture,     AssetKind::Audio,
        AssetKind::Script, AssetKind::Clip,        AssetKind::Material,    AssetKind::Environment,
        AssetKind::Shader, AssetKind::Font,        AssetKind::SourceModel, AssetKind::Other,
    };
    std::cout << "[信息] 类型分布:";
    for (AssetKind kind : kKinds) {
        const std::size_t count = m_Graph.CountByKind(kind);
        if (count > 0) {
            std::cout << " " << AssetKindName(kind) << " " << count;
        }
    }
    std::cout << "\n";

    if (!Validate()) {
        result.error = m_Options.strict && errorCount == 0
                           ? "校验未通过：--strict 下告警视为错误"
                           : "校验未通过：存在 " + std::to_string(errorCount) + " 个错误";
        return false;
    }
    if (m_Options.dryRun) {
        std::cout << "[信息] --dry-run：只收集与校验，未写盘\n";
        result.success = true;
        return true;
    }

    const std::filesystem::path staging(m_Options.outDir.string() + ".staging");
    std::error_code ec;
    std::filesystem::remove_all(staging, ec);
    std::filesystem::create_directories(staging, ec);
    if (ec) {
        result.error = "无法创建 staging 目录: " + staging.string() + "（" + ec.message() + "）";
        return false;
    }

    if (!StageAssets(staging, result)) {
        return false;
    }
    if (!CopyRuntime(staging, result.error)) {
        return false;
    }
    if (!WriteGameConfig(staging, result.error)) {
        return false;
    }
    if (!WriteManifest(staging, result, result.error)) {
        return false; // 最后写：它是「这份包完整」的标记
    }
    if (!Commit(staging, result.error)) {
        return false;
    }

    result.success = true;
    result.manifestPath = (m_Options.outDir / "manifest.json").string();
    return true;
}

bool Packager::StageAssets(const std::filesystem::path &staging, PackResult &result) {
    const std::filesystem::path assetsOut = staging / "assets";
    std::size_t copied = 0;
    std::uintmax_t bytes = 0;

    for (const auto &kv : m_Graph.Nodes()) {
        const std::string &canonical = kv.first;
        const std::filesystem::path src = m_Graph.FullPath(canonical);
        std::error_code ec;
        if (!std::filesystem::is_regular_file(src, ec)) {
            // 走到这里说明它既没被报错也没被收到——保守跳过并说明，不静默
            std::cout << "[告警] 跳过不存在的资产: " << canonical << "\n";
            continue;
        }
        const std::filesystem::path dst = assetsOut / std::filesystem::path(canonical);
        std::filesystem::create_directories(dst.parent_path(), ec);
        if (ec) {
            result.error = "无法创建目录: " + dst.parent_path().string();
            return false;
        }
        std::filesystem::copy_file(src, dst,
                                   std::filesystem::copy_options::overwrite_existing, ec);
        if (ec) {
            result.error = "拷贝失败: " + canonical + "（" + ec.message() + "）";
            return false;
        }
        result.fileCount = ++copied;
        result.bytes = bytes += kv.second.size;
    }

    std::cout << "[信息] 已拷入 " << copied << " 个资产 / " << bytes << " 字节 → "
              << assetsOut.string() << "\n";
    return true;
}

bool Packager::CopyRuntime(const std::filesystem::path &staging, std::string &error) {
    std::error_code ec;
    if (!std::filesystem::is_regular_file(m_Options.runtimeExe, ec)) {
        error = "找不到运行时播放器: " + m_Options.runtimeExe.string() +
                "（用 --runtime 指定 GE_Runtime.exe 的位置）";
        return false;
    }
    const std::filesystem::path dst = staging / m_Options.runtimeExe.filename();
    std::filesystem::copy_file(m_Options.runtimeExe, dst,
                               std::filesystem::copy_options::overwrite_existing, ec);
    if (ec) {
        error = "拷贝运行时失败: " + ec.message();
        return false;
    }
    std::cout << "[信息] 已拷入播放器: " << dst.filename().string() << "\n";
    return true;
}

bool Packager::WriteGameConfig(const std::filesystem::path &staging, std::string &error) {
    // 只搬运源 cfg 里**实际存在**的键：GEWindowConfig/rendering 的字段语义是
    // 「缺省即不覆盖引擎默认值」，把默认值物化出来会静默改变行为
    YAML::Node source;
    if (!m_Options.projectCfg.empty()) {
        try {
            source = YAML::LoadFile(m_Options.projectCfg.string());
        } catch (const std::exception &e) {
            std::cout << "[告警] 源配置读不了，只写入口场景: " << e.what() << "\n";
        }
    }

    YAML::Node cfg;
    cfg["scene"] = m_Options.entryScene;
    if (source["window"]) {
        cfg["window"] = source["window"];
    }
    if (source["rendering"]) {
        cfg["rendering"] = source["rendering"];
    }

    std::ostringstream text;
    text << "# 由 gepack 生成 —— 发行版启动配置\n"
            "# 查找顺序：exe 同级 → 当前工作目录；命令行覆盖优先级最高：\n"
            "#   --scene <路径> / --fullscreen / --free-camera\n"
            "# 天空盒与 IBL 环境由场景内的 EnvironmentComponent 决定，不在这里配置。\n\n"
         << YAML::Dump(cfg);

    const std::filesystem::path dst = staging / "game.cfg";
    std::ofstream out(dst, std::ios::binary | std::ios::trunc);
    if (!out.is_open()) {
        error = "无法写出 game.cfg: " + dst.string();
        return false;
    }
    out << text.str();
    out.close();
    return true;
}

bool Packager::WriteManifest(const std::filesystem::path &staging, const PackResult &result,
                             std::string &error) {
    nlohmann::json doc;
    doc["formatVersion"] = kFormatVersion;
    doc["engineVersion"] = kEngineVersion;
    doc["builtAt"] = NowIso8601();
    doc["entryScene"] = m_Options.entryScene;
    doc["assetRoot"] = "assets";
    doc["scripts"] = "whole-dir";
    doc["totals"] = {{"files", result.fileCount}, {"bytes", result.bytes}};

    // 注：本阶段 manifest 不带 sha256（仓库无第一方哈希实现），增量打包与运行时
    // 启动自检要等到阶段 D；现在只记路径/字节/种类，够用于「资产数与实际文件数对账」
    nlohmann::json assets = nlohmann::json::array();
    for (const auto &kv : m_Graph.Nodes()) {
        std::error_code ec;
        if (!std::filesystem::is_regular_file(m_Graph.FullPath(kv.first), ec)) {
            continue;
        }
        assets.push_back({{"path", kv.first},
                          {"size", kv.second.size},
                          {"kind", AssetKindName(kv.second.kind)}});
    }
    doc["assets"] = std::move(assets);
    doc["notes"] = m_Graph.Notes();

    const std::filesystem::path dst = staging / "manifest.json";
    std::ofstream out(dst, std::ios::binary | std::ios::trunc);
    if (!out.is_open()) {
        error = "无法写出 manifest.json: " + dst.string();
        return false;
    }
    out << doc.dump(2) << "\n";
    out.close();
    return true;
}

bool Packager::Commit(const std::filesystem::path &staging, std::string &error) {
    const std::filesystem::path out = m_Options.outDir;
    const std::filesystem::path old(out.string() + ".old");
    std::error_code ec;

    std::filesystem::remove_all(old, ec);
    if (std::filesystem::exists(out, ec)) {
        std::filesystem::rename(out, old, ec);
        if (ec) {
            error = "旧输出目录无法让位（可能正被运行中的程序占用）: " + out.string() + "（" +
                    ec.message() + "）";
            return false;
        }
    }

    std::filesystem::rename(staging, out, ec);
    if (ec) {
        // 换入失败：把旧目录换回去，别留下「既没有 dist、旧产物也被挪走」的状态
        std::error_code rollbackEc;
        if (std::filesystem::exists(old, rollbackEc)) {
            std::filesystem::rename(old, out, rollbackEc);
        }
        error = "换入输出目录失败: " + ec.message();
        return false;
    }

    std::filesystem::remove_all(old, ec); // 尽力清理，失败不影响结果
    return true;
}

} // namespace gepack
