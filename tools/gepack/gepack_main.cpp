/**
 * @file gepack_main.cpp
 * @brief 打包 CLI：读入口场景 → 收集依赖 → 校验 → 拷进 dist/ → 写 manifest 与 game.cfg。
 *
 * 纯 CPU 工具：不初始化 Vulkan / 窗口 / Renderer。场景解析走 GE::ScanSceneAssetRefs
 * （只读 YAML 扫描器），因为引擎的 SceneSerializer::Deserialize 全程依赖 Renderer
 * （`Renderer::GetAssetManager()`），在没有显卡的构建机上根本起不来。
 *
 * 用法：
 *   gepack [--project game.cfg] [--scene scenes/2.scene] [--asset-root assets]
 *          [--out dist] [--runtime bin/GE_Runtime.exe] [--rules gepack_rules.yaml]
 *          [--dry-run] [--strict]
 *
 * 退出码：0 = 成功（--dry-run 时表示校验通过）；1 = 失败（参数错 / 前置条件不满足 /
 * 校验有错误 / 拷贝出错）。
 */

#include "AssetDependencyGraph.h"
#include "PackRules.h"
#include "Packager.h"

#include "Core/Log.h"
#include "FileSystem/VFS.h"
#include "Render/AssetPathUtil.h"
#include "Utils/PlatformUtils.h"

#include <yaml-cpp/yaml.h>

#include <iostream>
#include <string>

namespace fs = std::filesystem;
using namespace gepack;

namespace {

void PrintUsage() {
    std::cout <<
        "用法: gepack [选项]\n\n"
        "把入口场景依赖的全部资产收集、校验、拷进输出目录，产出可直接分发的发行目录：\n"
        "    <out>/GE_Runtime.exe + game.cfg + manifest.json + assets/\n\n"
        "选项:\n"
        "  --project <cfg>      启动配置（默认 game.cfg），入口场景取自其中的 scene 键\n"
        "  --scene <路径>       直接指定入口场景（规范形，如 scenes/2.scene），覆盖配置\n"
        "  --asset-root <目录>  资源根（默认 <project 所在目录>/assets）\n"
        "  --out <目录>         输出目录（默认 dist）；先建 <out>.staging，就绪后整体换入\n"
        "  --runtime <exe>      要一并拷入的播放器（默认同 gepack 同级的 GE_Runtime.exe）\n"
        "  --rules <yaml>       打包规则（默认 exe 同级 / 当前目录的 gepack_rules.yaml）\n"
        "  --dry-run            只收集与校验，不写盘\n"
        "  --strict             把告警也当作错误（校验更严，CI 用）\n"
        "  -h, --help           显示帮助\n"
        "  -v, --version        显示版本\n";
}

void PrintVersion() {
    std::cout << "gepack 0.1.0（manifest 格式版本 1）\n";
}

/// 从 game.cfg 读入口场景（缺省与 GE_Runtime 的 GameConfig 一致）
std::string ReadEntrySceneFromCfg(const fs::path &cfgPath) {
    try {
        const YAML::Node root = YAML::LoadFile(cfgPath.string());
        if (root["scene"] && root["scene"].IsScalar()) {
            return root["scene"].as<std::string>("");
        }
    } catch (const std::exception &e) {
        std::cerr << "[gepack] 配置读取失败: " << cfgPath.string() << "（" << e.what() << "）\n";
        return {};
    }
    return {};
}

} // namespace

int main(int argc, char **argv) {
    // GE_CORE_* 宏依赖（.gemesh / .gemat 解析器内部会打日志）
    GE::Log::Init();

    const fs::path exeDir = GE::PlatformUtils::GetExecutableDirectory();

    std::string projectCfg = "game.cfg";
    std::string sceneOverride;
    std::string assetRootArg;
    std::string outDir = "dist";
    std::string runtimeArg;
    std::string rulesArg;
    bool dryRun = false;
    bool strict = false;

    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        const auto next = [&](std::string &dst) {
            if (i + 1 < argc) {
                dst = argv[++i];
            }
        };
        if (a == "-h" || a == "--help") {
            PrintUsage();
            return 0;
        }
        if (a == "-v" || a == "--version") {
            PrintVersion();
            return 0;
        }
        if (a == "--project") { next(projectCfg); continue; }
        if (a == "--scene") { next(sceneOverride); continue; }
        if (a == "--asset-root") { next(assetRootArg); continue; }
        if (a == "--out") { next(outDir); continue; }
        if (a == "--runtime") { next(runtimeArg); continue; }
        if (a == "--rules") { next(rulesArg); continue; }
        if (a == "--dry-run") { dryRun = true; continue; }
        if (a == "--strict") { strict = true; continue; }

        std::cerr << "[gepack] 未知参数: " << a << "\n";
        PrintUsage();
        return 1;
    }

    // ---- 资源根：默认在配置文件旁边 ----
    fs::path assetRoot = assetRootArg.empty()
                             ? (fs::path(projectCfg).parent_path() / "assets")
                             : fs::path(assetRootArg);

    // 初始化 VFS 的**磁盘后端**。这是必需的：gepack 与引擎共用同一批格式解析器
    // （GEMeshLoader / GLTFLoader / OBJLoader …），它们统一经 VFS 读资产，而 VFS 是
    // 进程级单例，不初始化则一切读取静默失败 —— 现象是 gepack 把完好的 .gemesh 报成
    // 「格式版本不符或已损坏」。主机工具同样有真实的磁盘资源根，初始化即可。
    GE::VFS::Init(assetRoot);

    // ---- 入口场景：--scene 覆盖配置；配置缺失退回与 GameConfig 相同的默认值 ----
    std::string rawScene = sceneOverride;
    if (rawScene.empty()) {
        rawScene = ReadEntrySceneFromCfg(projectCfg);
        if (rawScene.empty()) {
            std::cerr << "[gepack] 无法确定入口场景：请用 --scene 指定，或在 "
                      << projectCfg << " 里写 scene 键\n";
            return 1;
        }
    }
    const auto entryScene = GE::AssetPathUtil::ToCanonical(rawScene, assetRoot);
    if (!entryScene) {
        std::cerr << "[gepack] 入口场景不在资源根之下: " << rawScene << "（资源根 "
                  << assetRoot.string() << "）\n";
        return 1;
    }

    // ---- 播放器：默认与 gepack 同级（同一次构建产出都在 bin/）----
    const fs::path runtimeExe =
        runtimeArg.empty() ? (exeDir / "GE_Runtime.exe") : fs::path(runtimeArg);

    // ---- 规则：显式指定 → exe 同级 → 当前目录 → 内置默认 ----
    std::string rulesPath = rulesArg;
    if (rulesPath.empty()) {
        const fs::path besideExe = exeDir / "gepack_rules.yaml";
        std::error_code ec;
        if (fs::is_regular_file(besideExe, ec)) {
            rulesPath = besideExe.string();
        } else if (fs::is_regular_file("gepack_rules.yaml", ec)) {
            rulesPath = "gepack_rules.yaml";
        }
    }
    std::string rulesErr;
    const PackRules rules = LoadPackRules(rulesPath, rulesErr);
    if (!rulesErr.empty()) {
        std::cout << "[信息] " << rulesErr << "\n";
    }

    std::cout << "[gepack] 入口场景: " << *entryScene << "\n"
              << "[gepack] 资源根  : " << assetRoot.string() << "\n"
              << "[gepack] 输出目录: " << (dryRun ? std::string("(dry-run，不写盘)")
                                                  : outDir)
              << "\n\n";

    // ---- 收集 + 校验 ----
    AssetDependencyGraph graph(assetRoot, rules);
    std::string fatal;
    if (!graph.Build(*entryScene, fatal)) {
        std::cerr << "[gepack] " << fatal << "\n";
        return 1;
    }

    PackOptions options;
    options.assetRoot = assetRoot;
    options.outDir = outDir;
    options.runtimeExe = runtimeExe;
    options.projectCfg = projectCfg;
    options.entryScene = *entryScene;
    options.strict = strict;
    options.dryRun = dryRun;

    Packager packager(options, std::move(graph));
    PackResult result;
    if (!packager.Run(result)) {
        std::cerr << "\n[gepack] 失败: " << result.error << "\n";
        return 1;
    }

    if (dryRun) {
        std::cout << "\n[gepack] 校验通过（dry-run）。去掉 --dry-run 即产出 "
                  << outDir << "/\n";
    } else {
        std::cout << "\n[gepack] 完成：" << result.fileCount << " 个资产 / "
                  << result.bytes << " 字节 → " << outDir << "/\n"
                  << "[gepack] 清单: " << result.manifestPath << "\n";
    }
    return 0;
}
