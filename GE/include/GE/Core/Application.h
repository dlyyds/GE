#pragma once

#include "Base.h"
#include "Events/ApplicationEvent.h"
#include "GEWindow.h"

#include "Layer.h"
#include "LayerStack.h"

#include <memory>

#include "Core/Timestep.h"
#include "Debug/Assert.h"

#include "Render/Renderer.h"

namespace GE {
class Shader;
namespace Audio { class AudioContext; }

struct ApplicationCommandLineArgs {
    int Count = 0;
    char **Args = nullptr;

    const char *operator[](int index) const {
        GE_CORE_ASSERT(index < Count);
        return Args[index];
    }
};

class Application {
public:
    explicit Application(const std::string &name = "GE App", ApplicationCommandLineArgs args = ApplicationCommandLineArgs());


    virtual ~Application();

    void OnEvent(Event &e);

    void PushLayer(const std::shared_ptr<Layer> &layer);

    void PushOverlay(const std::shared_ptr<Layer> &layer);

    [[nodiscard]] Window &GetWindow() const { return *m_Window; }

    void Close();

    /**
     * @brief 切换呈现模式（垂直同步）。
     *
     * @param mode  目标垂直同步模式（ON / OFF / Default）
     */
    void SetPresentMode(VsyncMode mode);

    /**
     * @brief Set the frame-rate limit (锁定帧率上限).
     *
     * A limit <= 0 disables frame locking. When enabled, the main loop sleeps
     * on frame boundaries so the engine runs close to the requested FPS.
     *
     * @param fps  Target FPS (>0 enables locking; <=0 disables it).
     */
    void SetFrameRateLimit(float fps);

    /// @return Current FPS limit; 0 or negative means frame locking is off.
    [[nodiscard]] float GetFrameRateLimit() const { return m_FrameRateLimit; }

    /// @return True if frame-rate locking is currently enabled.
    [[nodiscard]] bool IsFrameRateLimited() const { return m_FrameRateLimit > 0.0f; }

    static Application &Get() { return *s_Instance; }

    /**
     * @brief 进程入口的转发点：初始化日志 → 创建应用 → 跑主循环 → 收尾。
     *
     * 由 `Core/EntryPoint.h` 里的 `main()` 调用。
     *
     * 为什么不让 `main()` 直接调私有的 `Run()` 并靠 `friend` 开权限：SDL 会把入口
     * 函数**重命名**为 `SDL_main`（`SDL_main.h` 里的 `#define main SDL_main`），
     * 而友元是按**名字**绑定的。那个宏是否已生效取决于各入口 TU 的包含顺序
     * （`EditorApp.cpp` 是先 `<GE.h>`、`RuntimeApp.cpp` 是先 `Application.h`，
     * 两者都在 `EntryPoint.h` 之前），于是友元声明会展开成 `::main`，而函数实际
     * 叫 `SDL_main` → C2248「Run 是 private」。与其去追宏状态，不如给一个与入口名
     * 无关的转发点 —— 顺带也让这个核心头文件不必知道任何 SDL 的存在。
     */
    static int Main(int argc, char **argv);


    [[nodiscard]] ApplicationCommandLineArgs GetCommandLineArgs() const { return m_CommandLineArgs; }

    /// 全局音频上下文（音频后端），可能为 nullptr（初始化失败/未创建）。
    [[nodiscard]] Audio::AudioContext *GetAudioContext() const { return m_AudioContext.get(); }


    [[nodiscard]] float GetFPS() const { return m_FPS; }

    // ImGui 已归并 Renderer：帧渲染资源（cmd / image view / swapchain / Vulkan context）
    // 一律通过 Renderer 访问，Application 不再承担中转。需要时调 Renderer::GetXXX。

private:
    void Run();

    void RecreateSwapchain();

    bool OnWindowClose(WindowCloseEvent &e);

    bool OnWindowResized(const WindowResizeEvent &e);

private:
    ApplicationCommandLineArgs m_CommandLineArgs;

    bool m_Running = true;
    std::unique_ptr<Window> m_Window;
    LayerStack m_LayerStack;
    float m_LastFrameTime = 0.0f;
    bool m_Minimized = false;

    float m_FPS = 0.0f;
    float m_FrameTimeAccumulator = 0.0f;
    int m_FrameCount = 0;

    // -- frame rate lock: <=0 disabled, >0 target FPS --
    float m_FrameRateLimit = 0.0f;

    // -- 渲染器：统一管理所有 Vulkan 资源 --
    std::unique_ptr<Renderer> m_Renderer;

    // -- 音频后端：全局一份，与渲染解耦 --
    std::unique_ptr<Audio::AudioContext> m_AudioContext;

private:
    static Application *s_Instance;
};

extern Application *CreateApplication(ApplicationCommandLineArgs args);

} // namespace GE
