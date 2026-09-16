#include "Core/Application.h"
#include "Core/GEInput.h"
#include "Core/GEWindow.h"
#include "Core/Log.h"

#include "Core/KeyCodes.h"
#include "Core/MouseCodes.h"
#include "Core/Timestep.h"
#include "Debug/Assert.h"

#include "Debug/Profiler.h"

#include <Events/ApplicationEvent.h>

#include "Audio/AudioContext.h"
#include "Utils/PlatformUtils.h"
#include <chrono>
#include <filesystem>
#include <functional>
#include <memory>
#include <system_error>
#include <thread>

namespace GE {
Application *Application::s_Instance = nullptr;

Application::Application(const std::string &name, ApplicationCommandLineArgs args)
    : m_CommandLineArgs(args) {

    GE_CORE_ASSERT(!s_Instance, "Application already exists!");
    s_Instance = this;

    m_Window = Window::Create(WindowProperties(name, 1600, 900));
    m_Window->SetVSync(VsyncMode::OFF);

    // 音频后端：全局一份，与 Vulkan 解耦（失败不阻塞启动，仅打日志）。
    m_AudioContext = std::make_unique<Audio::AudioContext>();
    if (!m_AudioContext->Init()) {
        GE_CORE_WARN("AudioContext init failed; audio features disabled");
        m_AudioContext.reset();
    }
    m_Window->SetEventCallback(GE_BIND_EVENT_FN(Application::OnEvent));

    // 资源根决策：优先 exe 同级 assets（发行版布局，与启动时的工作目录无关），
    // 不存在则回退 CWD/assets（开发期从仓库根启动）。
    // 必须在 Renderer 构造之前定好：Renderer 构造尾部会初始化 ImGui 并加载字体。
    const std::filesystem::path exeDir = PlatformUtils::GetExecutableDirectory();
    std::error_code ec;
    std::filesystem::path assetRoot = exeDir / "assets";
    if (exeDir.empty() || !std::filesystem::exists(assetRoot, ec)) {
        const std::filesystem::path cwd = std::filesystem::current_path(ec);
        assetRoot = ec ? std::filesystem::path("assets") : cwd / "assets";
    }

    // 根不存在时**必须**显式报错。此时相对资源引用（纹理/网格/材质/音频）会全部
    // 解析到不存在的路径而加载失败，表现为"模型全白、贴图面板里换贴图没反应"，
    // 而绝对路径引用却因绕过根而照旧可用——现象极具误导性，故在启动时就点明。
    if (!std::filesystem::exists(assetRoot, ec)) {
        GE_CORE_ERROR("Application: 资源根不存在：{0}（相对资源引用将全部加载失败）",
                      assetRoot.string());
    }

    // 初始化渲染器（内部完成 VulkanContext → RenderContext → Prepare → ImGui 初始化）
    m_Renderer = std::make_unique<Renderer>(*m_Window, assetRoot);

    // ImGui 已归并 Renderer：把"遍历各 Layer 的 OnImGuiRender"作为回调注入，
    // 由 Renderer::EndFrame 在 ImGui Begin 之后、上屏之前逐帧调用。
    m_Renderer->SetFrameUI([this]() {
        for (auto &layer : m_LayerStack)
            layer->OnImGuiRender();
    });

    m_Window->SetMaximized(true);
}

Application::~Application() {

    GE_CORE_INFO("Application Shoutdown");

    // 1. 等待 GPU 完成所有未完成的工作（必须在释放 Layer 的 GPU 资源之前）
    m_Renderer->WaitIdle();

    // 2. Detach 所有层（层中的 Material/Mesh/Texture 持有 GPU 资源）
    m_LayerStack.Clear();

    // 3. 销毁渲染器（内部再次 waitIdle + 释放 ImGui 与所有 Vulkan 资源）
    m_Renderer.reset();

    // 4. 销毁音频上下文（在 Window 销毁前，Render/Asset 已释放资源）
    m_AudioContext.reset();

    s_Instance = nullptr;
}

int Application::Main(int argc, char **argv) {
    Log::Init();

    auto *app = CreateApplication({argc, argv});
    app->Run();
    delete app;

    return 0;
}

void Application::Run() {

    while (m_Running) {
        GE_PROFILE_SCOPE("MainLoop");
        const auto frameStart = std::chrono::steady_clock::now();
        // 主循环墙钟原先取 glfwGetTime()。主循环不该依赖窗口库（换 SDL 后也没这个
        // 函数了），改用 std::chrono —— 上面这行本来就取了一次 steady_clock，直接
        // 复用同一个时钟。函数内静态常量充当时间原点，使 m_LastFrameTime 保持
        // "自启动起的秒数"这一原有语义。
        static const auto s_AppStartTime = frameStart;
        const auto time = std::chrono::duration<float>(frameStart - s_AppStartTime).count();
        Timestep timestep = time - m_LastFrameTime;

        // 帧率计算
        {
            GE_PROFILE_SCOPE("FPSUpdate");
            m_FrameTimeAccumulator += timestep;
            m_FrameCount++;

            // 每累计 0.2 秒更新一次 FPS（平滑显示）
            if (m_FrameTimeAccumulator >= 0.2f) {
                m_FPS = (float)m_FrameCount / m_FrameTimeAccumulator;

                // 重置
                m_FrameTimeAccumulator = 0.0f;
                m_FrameCount = 0;
            }
        }

        m_LastFrameTime = time;

        if (!m_Minimized) {
            GE_PROFILE_SCOPE("RenderFrame");

            // 1. Begin frame — acquire + begin cmd + layout → ColorAttachment
            m_Renderer->BeginFrame();

            // 2. OnUpdate
            {
                GE_PROFILE_SCOPE("OnUpdate");
                for (auto &layer : m_LayerStack)
                    layer->OnUpdate(timestep);
            }

            // 3. End frame — layout → Present + end cmd + submit + present
            //    （ImGui 已归并 Renderer：其 UI 提交回调在 EndFrame 内部被调用）
            m_Renderer->EndFrame();
        }
        m_Window->OnUpdate();
        GE_PROFILE_FRAME_MARK();

        // Frame rate lock: if this frame finished faster than the target interval,
        // sleep until the ideal frame boundary so the next timestep includes the sleep.
        if (m_FrameRateLimit > 0.0f) {
            GE_PROFILE_SCOPE("FrameRateLimit");
            const auto targetFrameTime = std::chrono::duration<float>(1.0f / m_FrameRateLimit);
            const auto targetDuration = std::chrono::duration_cast<std::chrono::steady_clock::duration>(targetFrameTime);
            const auto elapsed = std::chrono::steady_clock::now() - frameStart;
            if (elapsed < targetDuration) {
                std::this_thread::sleep_until(frameStart + targetDuration);
            }
        }
    }

}

void Application::OnEvent(Event &e) {
    GE_PROFILE_FUNCTION();
    EventDispatcher dispatcher(e);
    dispatcher.Dispatch<WindowCloseEvent>(GE_BIND_EVENT_FN(Application::OnWindowClose));
    dispatcher.Dispatch<WindowResizeEvent>(GE_BIND_EVENT_FN(Application::OnWindowResized));

    for (auto it = m_LayerStack.rbegin(); it != m_LayerStack.rend(); ++it) {
        if (e.Handled)
            break;
        (*it)->OnEvent(e);
    }
}

void Application::Close() { m_Running = false; }

void Application::SetFrameRateLimit(float fps) {
    m_FrameRateLimit = fps;
    if (fps > 0.0f) {
        GE_CORE_INFO("Frame rate locked to {0:.1f} FPS", fps);
    } else {
        GE_CORE_INFO("Frame rate limit disabled");
    }
}

void Application::SetPresentMode(VsyncMode mode) {
    if (!m_Renderer) {
        return;
    }

    // 将 VsyncMode 映射到 Vulkan 呈现模式（与 VulkanRenderContext 构造中的逻辑一致）
    vk::PresentModeKHR present_mode;
    switch (mode) {
    case VsyncMode::ON: present_mode = vk::PresentModeKHR::eFifo;
        break;
    case VsyncMode::OFF: present_mode = vk::PresentModeKHR::eMailbox;
        break;
    case VsyncMode::Default:
    default: present_mode = vk::PresentModeKHR::eMailbox;
        break;
    }

    // 同步更新 Window 的 vsync 属性，保持状态一致
    m_Window->SetVSync(mode);

    m_Renderer->SetPresentMode(present_mode);
}

void Application::RecreateSwapchain() {
    auto windowWidth = m_Window->GetWidth();
    auto windowHeight = m_Window->GetHeight();

    if (windowWidth == 0 || windowHeight == 0) {
        m_Minimized = true;
        return;
    }
    m_Minimized = false;

    // 委托给 Renderer 重建 swapchain
    m_Renderer->RecreateSwapchain(windowWidth, windowHeight);
}

bool Application::OnWindowResized(const WindowResizeEvent &e) {
    if (e.GetWidth() == 0 || e.GetHeight() == 0) {
        m_Minimized = true;
        return false;
    }
    m_Minimized = false;
    RecreateSwapchain();
    return false;
}

bool Application::OnWindowClose(WindowCloseEvent &e) {
    m_Running = false;
    return true;
}

void Application::PushLayer(const std::shared_ptr<Layer> &layer) {

    m_LayerStack.PushLayer(layer);
    layer->OnAttach();
}

void Application::PushOverlay(const std::shared_ptr<Layer> &layer) {

    m_LayerStack.PushOverlay(layer);
    layer->OnAttach();
}


} // namespace GE
