#include "GameConfig.h"

#include "GE/Core/Log.h"
#include "GE/Utils/PlatformUtils.h"
#include "GE/FileSystem/VFS.h"

#include <filesystem>
#include <fstream>
#include <sstream>

#include <yaml-cpp/yaml.h>

namespace GE {
namespace {

/// 读一个**真实文件系统**路径的全部内容（用户目录 / exe 同级 —— 这两处可能落在
/// 资源根之外，所以不能走 VFS）。失败返回空串。
std::string ReadRealFile(const std::filesystem::path &p) {
    std::ifstream in(p, std::ios::binary);
    if (!in) {
        return {};
    }
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

/**
 * @brief 定位并读取 game.cfg 的文本。
 *
 * 查找顺序：
 * - 桌面：exe 同级 → 当前工作目录。**与引入 VFS 之前逐字一致**（发行版布局优先，
 *   回退开发期从仓库根启动）。这两处是真实文件路径，可能不在资源根之下。
 * - Android：用户私有目录（允许玩家覆盖）→ 资产根里的随包 `game.cfg`。
 *   进程 CWD 是 `/`，前两者之外没有可探索的位置。
 *
 * @param outSource 回填来源描述（日志用）；未找到则保持空
 */
std::string ReadConfigText(std::string &outSource) {
    std::error_code ec;

#ifdef GE_PLATFORM_ANDROID
    if (const std::filesystem::path userDir = PlatformUtils::GetUserDataDirectory();
        !userDir.empty()) {
        const std::filesystem::path p = userDir / "game.cfg";
        if (std::filesystem::exists(p, ec)) {
            if (std::string text = ReadRealFile(p); !text.empty()) {
                outSource = p.string();
                return text;
            }
        }
    }
    // 回退：APK 内的资产根（gepack 产出的那份）
    if (std::string text = VFS::ReadText("game.cfg"); !text.empty()) {
        outSource = "APK assets/game.cfg";
        return text;
    }
    return {};
#else
    if (const std::filesystem::path exeDir = PlatformUtils::GetExecutableDirectory();
        !exeDir.empty()) {
        const std::filesystem::path p = exeDir / "game.cfg";
        if (std::filesystem::exists(p, ec)) {
            if (std::string text = ReadRealFile(p); !text.empty()) {
                outSource = p.string();
                return text;
            }
        }
    }
    if (const std::filesystem::path cwd = std::filesystem::current_path(ec); !ec) {
        const std::filesystem::path p = cwd / "game.cfg";
        if (std::filesystem::exists(p, ec)) {
            if (std::string text = ReadRealFile(p); !text.empty()) {
                outSource = p.string();
                return text;
            }
        }
    }
    return {};
#endif
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

    std::string source;
    const std::string configText = ReadConfigText(source);
    if (configText.empty()) {
        GE_CORE_WARN("未找到 game.cfg（查了用户目录 / exe 同级 / 当前工作目录 / 资产根），使用内置默认配置");
    } else {
        try {
            const YAML::Node root = YAML::Load(configText);

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

            GE_CORE_INFO("运行时配置已加载: {0}", source);
        } catch (const YAML::Exception &e) {
            GE_CORE_ERROR("game.cfg 解析失败（{0}）：{1}；改用内置默认配置", source, e.what());
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
