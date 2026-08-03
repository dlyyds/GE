#pragma once

#include "Base.h"
#include "Events/ApplicationEvent.h"
#include "GEWindow.h"

#include "Layer.h"
#include "LayerStack.h"

#include <memory>

#include "Core/Timestep.h"
#include "Debug/Assert.h"
#include "ImGui/ImGuiLayer.h"

#include "Render/Renderer.h"

int main(int argc, char **argv);

namespace GE {
class Shader;

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

    static Application &Get() { return *s_Instance; }


    [[nodiscard]] ApplicationCommandLineArgs GetCommandLineArgs() const { return m_CommandLineArgs; }


    [[nodiscard]] float GetFPS() const { return m_FPS; }

    /// 访问 Vulkan 全局上下文（转发给 Renderer，提供给 Layer 等创建 Vulkan 资源用）。
    static VulkanContext &GetVulkanContext() { return Renderer::GetVulkanContext(); }

    static const VulkanSwapchain &GetSwapchain() { return Renderer::GetSwapchain(); }
    static VulkanRenderContext &GetRenderContext() { return Renderer::GetRenderContext(); }


    /// 帧渲染辅助：当前帧的 command buffer 和 image view（转发给 Renderer）。
    static VulkanCommandBuffer &GetFrameCmd() { return Renderer::GetFrameCmd(); }
    static uint32_t GetFrameImageIndex() { return Renderer::GetFrameImageIndex(); }
    static VulkanImageView &GetFrameImageView() { return Renderer::GetFrameImageView(); }

private:
    void Run();

    void RecreateSwapchain();

    bool OnWindowClose(WindowCloseEvent &e);

    bool OnWindowResized(const WindowResizeEvent &e);

private:
    ApplicationCommandLineArgs m_CommandLineArgs;

    std::shared_ptr<ImGuiLayer> m_ImGuiLayer;

    bool m_Running = true;
    std::unique_ptr<Window> m_Window;
    LayerStack m_LayerStack;
    float m_LastFrameTime = 0.0f;
    bool m_Minimized = false;

    float m_FPS = 0.0f;
    float m_FrameTimeAccumulator = 0.0f;
    int m_FrameCount = 0;

    // -- 渲染器：统一管理所有 Vulkan 资源 --
    std::unique_ptr<Renderer> m_Renderer;

private:
    static Application *s_Instance;

    friend int ::main(int argc, char **argv);
};

extern Application *CreateApplication(ApplicationCommandLineArgs args);

} // namespace GE
