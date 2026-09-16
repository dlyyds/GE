#include "GameLayer.h"

#include "GE/Core/Application.h"
#include "GE/Core/EntryPoint.h"
#include "GE/Debug/Profiler.h"

#include <memory>

namespace GE {

/**
 * @brief 发行版播放器：只挂一个游戏层，不带任何编辑器 UI。
 *
 * 与 GE_Editor 共享 Application 基类的窗口 / 音频 / Renderer / ImGui 初始化链，
 * 差别只在 Layer 组成与渲染输出目标（背缓冲而非离屏视口）。
 */
class RuntimeApp : public Application {
public:
    explicit RuntimeApp(ApplicationCommandLineArgs args)
        : Application("GE Runtime", args) {
        GE_PROFILE_FUNCTION();
        PushLayer(std::make_shared<GameLayer>(LoadGameConfig(args)));
    }

    ~RuntimeApp() override = default;
};

Application *CreateApplication(const ApplicationCommandLineArgs args) { return new RuntimeApp(args); }

} // namespace GE
