#pragma once

#include "GE/Core/Application.h"

#include <cstdint>
#include <optional>
#include <string>

namespace GE {

/**
 * @brief 运行时播放器启动配置（game.cfg + 命令行覆盖）。
 *
 * 优先级：命令行 > game.cfg > 内置默认。game.cfg 按「exe 同级」→「当前工作目录」
 * 顺序查找，都不存在时全部走默认值（开发期直接 `GE_Runtime.exe --scene ...` 也能跑）。
 * 发行版布局由 gepack 把 game.cfg 写到 exe 同级。
 */
struct GameConfig {
    struct WindowConfig {
        std::string title = "GE Runtime";
        uint32_t width = 1600;
        uint32_t height = 900;
        bool vsync = false;
        bool fullscreen = false;
    };

    /// 渲染开关：只有 cfg 里显式写了该键才覆盖引擎默认值（nullopt = 不覆盖），
    /// 免得把引擎默认值钉死在配置文件里、日后引擎改默认时运行时不跟着走。
    struct RenderingConfig {
        std::optional<bool> deferred;         ///< 延迟渲染（GBuffer 链）
        std::optional<bool> tonemap;          ///< 曝光 + ACES
        std::optional<bool> bloom;            ///< 泛光
        std::optional<std::string> culling;   ///< 视锥剔除粒度："mesh" | "submesh"
    };

    std::string scene = "scenes/2.scene";   ///< 入口场景（相对资源根，规范形）
    WindowConfig window;
    RenderingConfig rendering;
    bool freeCamera = false;   ///< --free-camera：忽略场景主相机，用内置自由视角
};

/**
 * @brief 解析启动配置：game.cfg（exe 同级优先，回退 CWD）+ 命令行覆盖。
 *
 * 配置文件不存在或解析失败都只打日志并退回默认值，不阻断启动。
 * 未知命令行参数打 GE_CORE_WARN 后忽略。
 */
GameConfig LoadGameConfig(ApplicationCommandLineArgs args);

} // namespace GE
