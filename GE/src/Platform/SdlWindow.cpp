#include "pch.h"

#include "Events/ApplicationEvent.h"
#include "Events/KeyEvent.h"
#include "Events/MouseEvent.h"

#include "Core/Log.h"
#include "Platform/SdlWindow.h"

// SDL.h 不含 SDL_vulkan.h —— 后者是 opt-in 的（与 SDL_main.h 同理），必须单独引入。
// ⚠️ 顺序有约束：必须在 vulkan_core.h 之后。SDL_vulkan.h 靠 `VULKAN_CORE_H_` 判断
//    要不要自己 typedef 一遍 Vulkan 句柄；若它先于 vulkan.h 被引入，会先定义一遍、
//    随后 vulkan_core.h 再定义一遍 → 重复定义。上面 "Platform/SdlWindow.h" →
//    "Core/GEWindow.h" 已经引入了 <Vulkan/vulkan.h>，故此处安全。
#include <SDL3/SDL.h>
#include <SDL3/SDL_vulkan.h>

#include "Debug/Assert.h"

namespace GE {

/// SDL 子系统引用计数：与 GLFW 版本一样，首次创建窗口时 SDL_Init，最后一个窗口
/// 析构时 SDL_Quit，允许多窗口共存而不互相提前关停。
static uint8_t s_SdlWindowCount = 0;

namespace {

/// 按键容量上限。KeyCode 现为 SDL scancode，合法值 0..511（`SDL_SCANCODE_COUNT == 512`），
/// 而 `InputState` 的 `kKeyCapacity` 正好是 512 —— 刚好覆盖、零余量。
/// 越界的 scancode 必须在这里丢掉：SDL 的 400..500 是动态键码保留区（Android 软键盘
/// 可能落在其中），一旦流到 `std::bitset::operator[]` 就是 UB。
constexpr int kMaxScancode = 512;

/// 按窗口模式拼创建标志。SDL3 的 `SDL_SetWindowFullscreen(true)` 默认走桌面模式
/// （无边框全屏），独占全屏需另配 `SDL_SetWindowFullscreenMode`；这里三种全屏模式
/// 统一用桌面全屏 —— 引擎实际只用 Default，且桌面全屏避免了切模式的闪烁与风险。
SDL_WindowFlags BuildWindowFlags(const WindowProperties &props) {
    SDL_WindowFlags flags = SDL_WINDOW_VULKAN;
    if (props.resizable) {
        flags |= SDL_WINDOW_RESIZABLE;
    }
    switch (props.mode) {
    case WindowMode::Fullscreen:
    case WindowMode::FullscreenStretch:
        flags |= SDL_WINDOW_FULLSCREEN;
        break;
    case WindowMode::FullscreenBorderless:
        flags |= SDL_WINDOW_FULLSCREEN | SDL_WINDOW_BORDERLESS;
        break;
    case WindowMode::Headless:
    case WindowMode::Default:
    default:
        break;
    }
    return flags;
}

} // namespace

SdlWindow::SdlWindow(const WindowProperties &props) {
    GE_PROFILE_FUNCTION();
    Init(props);
}

SdlWindow::~SdlWindow() { Shutdown(); }

void SdlWindow::Init(const WindowProperties &props) {
    GE_PROFILE_FUNCTION();

    properties = props;

    if (s_SdlWindowCount == 0) {
        GE_PROFILE_SCOPE("SDL_Init");
        if (!SDL_Init(SDL_INIT_VIDEO)) {
            GE_CORE_ERROR("SDL_Init 失败：{0}", SDL_GetError());
            GE_CORE_ASSERT(false, "Could not initialize SDL!");
            return;
        }
        GE_CORE_INFO("Initializing SDL3 (video driver: {0})", SDL_GetCurrentVideoDriver());
    }
    ++s_SdlWindowCount;

    {
        GE_PROFILE_SCOPE("SDL_CreateWindow");
        m_Window = SDL_CreateWindow(properties.title.c_str(),
                                    static_cast<int>(properties.extent.width),
                                    static_cast<int>(properties.extent.height),
                                    BuildWindowFlags(properties));
        if (!m_Window) {
            GE_CORE_ERROR("SDL_CreateWindow 失败：{0}", SDL_GetError());
            GE_CORE_ASSERT(false, "Could not create SDL window!");
            return;
        }
        GE_CORE_INFO("Creating window {0} ({1}, {2})",
                     properties.title, properties.extent.width, properties.extent.height);
    }

    // 创建后按真实像素尺寸回读一次：高 DPI 下 SDL 可能把请求的逻辑尺寸换算过，
    // 且 swapchain 必须按像素建。
    SyncExtentFromWindow();

    SetVSync(properties.vsync);
}

void SdlWindow::Shutdown() {
    if (m_Window) {
        SDL_DestroyWindow(m_Window);
        m_Window = nullptr;
    }
    if (s_SdlWindowCount > 0 && --s_SdlWindowCount == 0) {
        GE_CORE_INFO("Terminating SDL3");
        SDL_Quit();
    }
}

void SdlWindow::SyncExtentFromWindow() {
    if (!m_Window) {
        return;
    }
    int w = 0, h = 0;
    SDL_GetWindowSizeInPixels(m_Window, &w, &h);
    properties.extent.width = static_cast<uint32_t>(w);
    properties.extent.height = static_cast<uint32_t>(h);

    // 零尺寸是**硬错误**而不是"稍后再说"：Renderer 构造里创建的**首个** swapchain
    // 直接拿这个值当 extent，0 会被 Vulkan 拒绝（Application::RecreateSwapchain 的
    // 0 守卫在首帧之后才生效）。Android 上此值应为非零 —— SDLActivity 只在 Surface
    // 就绪且 Activity 已 resumed 时才启动 native main 线程（SDLActivity.java:858-866）。
    // 留着这条日志是为了万一真机上不是这样，能一眼看到根因，而不是猜 swapchain 为什么建不起来。
    if (w == 0 || h == 0) {
        GE_CORE_ERROR("SdlWindow: 窗口像素尺寸为 0（{0}x{1}）—— 随后创建 swapchain 必然失败", w, h);
    }
}

void SdlWindow::HandleEvent(const SDL_Event &event) {
    // 先给原始事件观察者（ImGui 平台后端），再走引擎自己的翻译。
    // 顺序与换库前一致：ImGui 的 GLFW 回调被装在链路前端，因此先于引擎回调拿到
    // 事件，从而能在同一帧内先算好 WantCaptureMouse / WantCaptureKeyboard。
    if (m_RawEventHook) {
        m_RawEventHook(&event);
    }

    switch (event.type) {
    case SDL_EVENT_QUIT:
    case SDL_EVENT_WINDOW_CLOSE_REQUESTED: {
        m_ShouldClose = true;
        WindowCloseEvent e;
        m_EventCallback(e);
        break;
    }

    case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED:
    case SDL_EVENT_WINDOW_RESIZED: {
        // 两个事件都会在缩放/跨屏时到达，且顺序不保证 —— 统一"回读实际像素尺寸，
        // 变了才发事件"，避免重复触发 swapchain 重建。
        const Extent before = properties.extent;
        SyncExtentFromWindow();
        if (properties.extent.width != before.width || properties.extent.height != before.height) {
            WindowResizeEvent e(static_cast<int>(properties.extent.width),
                                static_cast<int>(properties.extent.height));
            m_EventCallback(e);
        }
        break;
    }

    // 注意：SDL 的 SDL_EVENT_WINDOW_FOCUS_GAINED / LOST 故意不翻译成事件。
    // EventType 枚举里虽有 WindowFocus / WindowLostFocus 槽位，但**没有对应的类**，
    // 换库前的 GLFW 实现也从未生产过它们 —— 在此保持等价，不留无人消费的死代码。
    // 真需要时（阶段 E 的 Android 生命周期，切后台/回前台）再补类与生产者。

    case SDL_EVENT_KEY_DOWN: {
        if (event.key.scancode >= kMaxScancode) {
            break; // 越界丢弃，保护 InputState 的位集
        }
        // SDL 的 repeat 是布尔量，GLFW 的 repeatCount 是次数：按 GLFW 语义映射成 0/1
        KeyPressedEvent e(static_cast<KeyCode>(event.key.scancode), event.key.repeat ? 1 : 0);
        m_EventCallback(e);
        break;
    }

    case SDL_EVENT_KEY_UP: {
        if (event.key.scancode >= kMaxScancode) {
            break;
        }
        KeyReleasedEvent e(static_cast<KeyCode>(event.key.scancode));
        m_EventCallback(e);
        break;
    }

    case SDL_EVENT_TEXT_INPUT: {
        // GLFW 的字符回调每个码点回调一次；SDL 一次给整串 UTF-8，故这里逐码点拆发，
        // 保持与换库前一致的语义（中文输入、组合键输入都靠它）。
        const char *cursor = event.text.text;
        size_t remaining = SDL_strlen(event.text.text);
        while (remaining > 0) {
            const Uint32 codepoint = SDL_StepUTF8(&cursor, &remaining);
            if (codepoint == 0) {
                break;
            }
            // KeyCode 是 uint16_t，容不下 BMP 以外的码点；截断会产生乱码，直接跳过
            if (codepoint > 0xFFFF) {
                continue;
            }
            KeyTypedEvent e(static_cast<KeyCode>(codepoint));
            m_EventCallback(e);
        }
        break;
    }

    case SDL_EVENT_MOUSE_BUTTON_DOWN: {
        MouseButtonPressedEvent e(static_cast<MouseCode>(event.button.button));
        m_EventCallback(e);
        break;
    }

    case SDL_EVENT_MOUSE_BUTTON_UP: {
        MouseButtonReleasedEvent e(static_cast<MouseCode>(event.button.button));
        m_EventCallback(e);
        break;
    }

    case SDL_EVENT_MOUSE_MOTION: {
        // ⚠️ 相对模式（CursorMode::Disabled）下**不能**透传 event.motion.x/y。
        // SDL 文档原话：相对模式下 "the mouse position is constrained to the
        // window"，即 x/y 是**被钳制在窗口内**的绝对坐标 —— 鼠标推到边缘后就
        // 不再变化。而引擎（Scene 的自由视角相机）是用**逐帧坐标差**算鼠标增量
        // 的，于是转向到边缘就停住、没法一直朝一个方向转。
        // GLFW 的 GLFW_CURSOR_DISABLED 语义相反：坐标无界、可无限增长，原代码
        // 依赖的正是那个行为。故这里用 xrel/yrel 累加出虚拟坐标来复刻它。
        if (m_RelativeMouseMode) {
            m_CursorPosition.x += event.motion.xrel;
            m_CursorPosition.y += event.motion.yrel;
        } else {
            m_CursorPosition = {event.motion.x, event.motion.y};
        }
        MouseMovedEvent e(m_CursorPosition.x, m_CursorPosition.y);
        m_EventCallback(e);
        break;
    }

    case SDL_EVENT_MOUSE_WHEEL: {
        // SDL3 的 wheel 是浮点增量（且带 direction 表示翻转），x/y 可直接用
        MouseScrolledEvent e(event.wheel.x, event.wheel.y);
        m_EventCallback(e);
        break;
    }

    default:
        break;
    }
}

void SdlWindow::ProcessEvents() {
    SDL_Event event;
    while (SDL_PollEvent(&event)) {
        HandleEvent(event);
    }
}

void SdlWindow::OnUpdate() { ProcessEvents(); }

bool SdlWindow::ShouldClose() { return m_ShouldClose; }

void SdlWindow::Close() { m_ShouldClose = true; }

uint32_t SdlWindow::GetWidth() const { return properties.extent.width; }

uint32_t SdlWindow::GetHeight() const { return properties.extent.height; }

void SdlWindow::SetVSync(VsyncMode mode) {
    // 只记录：实际呈现模式由 swapchain 决定（见 VulkanRenderContext 的 present mode 选择）
    properties.vsync = mode;
}

VsyncMode SdlWindow::GetVSync() const { return properties.vsync; }

void SdlWindow::SetResizable(bool resizable) {
    properties.resizable = resizable;
    if (m_Window) {
        SDL_SetWindowResizable(m_Window, resizable);
    }
}

bool SdlWindow::IsResizable() const { return properties.resizable; }

WindowMode SdlWindow::GetWindowMode() const { return properties.mode; }

void SdlWindow::SetTitle(const std::string &title) {
    properties.title = title;
    if (m_Window) {
        SDL_SetWindowTitle(m_Window, title.c_str());
    }
}

Extent SdlWindow::Resize(const Extent &new_extent) {
    if (m_Window) {
        SDL_SetWindowSize(m_Window,
                          static_cast<int>(new_extent.width),
                          static_cast<int>(new_extent.height));
        SyncExtentFromWindow();
    }
    return {GetWidth(), GetHeight()};
}

void SdlWindow::SetMaximized(bool maximized) {
    if (!m_Window) {
        return;
    }
    if (maximized) {
        SDL_MaximizeWindow(m_Window);
    } else {
        SDL_RestoreWindow(m_Window);
    }
}

void SdlWindow::SetWindowMode(WindowMode mode) {
    if (mode == properties.mode || !m_Window) {
        return;
    }

    const bool wantsFullscreen = mode == WindowMode::Fullscreen ||
                                 mode == WindowMode::FullscreenBorderless ||
                                 mode == WindowMode::FullscreenStretch;

    // SDL3 的 true = 桌面全屏（无边框、不切显示模式）。独占全屏需要
    // SDL_SetWindowFullscreenMode 指定具体显示模式，引擎目前不需要，故不做。
    // 于是 Fullscreen 与 FullscreenBorderless 在 SDL 下表现相同 —— 已记入计划书。
    if (!SDL_SetWindowFullscreen(m_Window, wantsFullscreen)) {
        GE_CORE_WARN("SetWindowMode: SDL_SetWindowFullscreen 失败：{0}", SDL_GetError());
        return;
    }

    properties.mode = mode;
    SyncExtentFromWindow();
}

void *SdlWindow::GetNativeWindow() const {
    return m_Window;
}

void SdlWindow::SetCursorMode(CursorMode mode) {
    if (!m_Window) {
        return;
    }

    const bool want_relative = mode == CursorMode::Disabled;
    if (want_relative == m_RelativeMouseMode) {
        return;
    }

    // 切换前把虚拟坐标对齐到当前真实坐标：两种模式下的坐标来源不同（见
    // HandleEvent 的 MOUSE_MOTION 分支），不对齐会让切换瞬间出现一个巨大的坐标
    // 跳变，而引擎正是在切换前后调 ResetMouseBaseline 来防这个跳变的 —— 基准与
    // 随后的首个增量必须同源。
    float x = 0.0f, y = 0.0f;
    SDL_GetMouseState(&x, &y);
    m_CursorPosition = {x, y};

    // relative 模式 = 隐藏光标 + 只上报相对位移，正是 FreeLook 相机要的语义
    if (!SDL_SetWindowRelativeMouseMode(m_Window, want_relative)) {
        GE_CORE_WARN("SetCursorMode: SDL_SetWindowRelativeMouseMode 失败：{0}", SDL_GetError());
        return;
    }
    m_RelativeMouseMode = want_relative;
}

glm::vec2 SdlWindow::GetCursorPosition() const {
    if (!m_Window) {
        return {0.0f, 0.0f};
    }
    if (!m_RelativeMouseMode) {
        // 绝对模式：查实时坐标（与 MouseMovedEvent 的取值同源）
        float x = 0.0f, y = 0.0f;
        SDL_GetMouseState(&x, &y);
        return {x, y};
    }
    // 相对模式：窗口坐标被 SDL 钳制在窗口内、失去意义，只能返回累加出的虚拟坐标
    return m_CursorPosition;
}

float SdlWindow::GetDpiFactor() const {
    if (!m_Window) {
        return 1.0f;
    }
    const float scale = SDL_GetWindowDisplayScale(m_Window);
    return scale > 0.0f ? scale : 1.0f;
}

float SdlWindow::GetContentScaleFactor() const { return GetDpiFactor(); }

bool SdlWindow::GetDisplayPresentInfo(VkDisplayPresentInfoKHR *info,
                                     uint32_t src_width, uint32_t src_height) const {
    (void) info;
    (void) src_width;
    (void) src_height;
    // 默认实现：不提供额外呈现信息
    return false;
}

VkSurfaceKHR SdlWindow::CreateVulkanSurface(VkInstance instance) {
    if (instance == VK_NULL_HANDLE || !m_Window) {
        return VK_NULL_HANDLE;
    }
    VkSurfaceKHR surface = VK_NULL_HANDLE;
    if (!SDL_Vulkan_CreateSurface(m_Window, instance, nullptr, &surface)) {
        GE_CORE_ERROR("SDL_Vulkan_CreateSurface 失败：{0}", SDL_GetError());
        return VK_NULL_HANDLE;
    }
    return surface;
}

VkSurfaceKHR SdlWindow::CreateVulkanSurface(VkInstance instance, VkPhysicalDevice physical_device) {
    (void) physical_device;
    // SDL 创建表面不需要 physical_device，直接委托
    return CreateVulkanSurface(instance);
}

} // namespace GE
