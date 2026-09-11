#include "GameConfig.h"

#include "GE/Core/Log.h"
#include "GE/Utils/PlatformUtils.h"

#include <filesystem>

#include <yaml-cpp/yaml.h>

namespace GE {
namespace {

/// 定位 game.cfg：exe 同级优先（发行版布局），回退当前工作目录（开发期从仓库根启动）。
std::filesystem::path FindConfigFile() {
    std::error_code ec;
    const std::filesystem::path exeDir = PlatformUtils::GetExecutableDirectory();
    if (!exeDir.empty()) {
        const std::filesystem::path beside = exeDir / "game.cfg";
        if (std::filesystem::exists(beside, ec)) {
            return beside;
        }
    }
    const std::filesystem::path cwd = std::filesystem::current_path(ec);
    if (!ec) {
        const std::filesystem::path local = cwd / "game.cfg";
        if (std::filesystem::exists(local, ec)) {
            return local;
        }
    }
    return {};
}

/// 读一个可选 bool：键缺失/非标量/类型不符都返回 nullopt（= 不覆盖默认）。
std::optional<bool> ReadOptBool(const YAML::Node &node) {
    if (!node || !node.IsScalar()) {
        return std::nullopt;
    }
    try {
        return node.as<bool>();
    } catch (const YAML::Exception &) {
        return std::nullopt;
    }
}

/// 从 YAML 节点读一个值，键缺失或解析失败都保留原值（不阻断启动）。
template <typename T>
void ReadInto(const YAML::Node &node, const char *key, T &out) {
    if (!node || !node[key]) {
        return;
    }
    try {
        out = node[key].as<T>(out);   // 带兜底值的形式：类型不符时返回 out 自身
    } catch (const YAML::Exception &e) {
        GE_CORE_WARN("game.cfg: {0} 取值无效（{1}），沿用默认值", key, e.what());
    }
}

} // namespace

GameConfig LoadGameConfig(ApplicationCommandLineArgs args) {
    GameConfig cfg;

    const std::filesystem::path path = FindConfigFile();
    if (path.empty()) {
        GE_CORE_WARN("未找到 game.cfg（查了 exe 同级与当前工作目录），使用内置默认配置");
    } else {
        try {
            const YAML::Node root = YAML::LoadFile(path.string());

            ReadInto(root, "scene", cfg.scene);

            if (const YAML::Node window = root["window"]) {
                ReadInto(window, "title", cfg.window.title);
                ReadInto(window, "width", cfg.window.width);
                ReadInto(window, "height", cfg.window.height);
                if (auto v = ReadOptBool(window["vsync"])) {
                    cfg.window.vsync = *v;
                }
                if (auto v = ReadOptBool(window["fullscreen"])) {
                    cfg.window.fullscreen = *v;
                }
            }

            if (const YAML::Node rendering = root["rendering"]) {
                cfg.rendering.deferred = ReadOptBool(rendering["deferred"]);
                cfg.rendering.tonemap = ReadOptBool(rendering["tonemap"]);
                cfg.rendering.bloom = ReadOptBool(rendering["bloom"]);
                if (const YAML::Node culling = rendering["culling"]; culling && culling.IsScalar()) {
                    cfg.rendering.culling = culling.as<std::string>();
                }
            }

            GE_CORE_INFO("运行时配置已加载: {0}", path.string());
        } catch (const YAML::Exception &e) {
            GE_CORE_ERROR("game.cfg 解析失败（{0}）：{1}；改用内置默认配置", path.string(), e.what());
            cfg = GameConfig{};
        }
    }

    // 命令行覆盖（最后应用，优先级最高）
    for (int i = 1; i < args.Count; ++i) {
        const std::string arg = args.Args[i] ? args.Args[i] : "";
        if (arg == "--scene") {
            if (i + 1 < args.Count && args.Args[i + 1] && args.Args[i + 1][0] != '\0') {
                cfg.scene = args.Args[++i];
            } else {
                GE_CORE_WARN("--scene 缺少参数，忽略");
            }
        } else if (arg == "--fullscreen") {
            cfg.window.fullscreen = true;
        } else if (arg == "--free-camera") {
            cfg.freeCamera = true;
        } else {
            GE_CORE_WARN("未知命令行参数：{0}（忽略）", arg);
        }
    }

    return cfg;
}

} // namespace GE
