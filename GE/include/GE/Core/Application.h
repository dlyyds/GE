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

#include "Render/VulkanBase/VulkanContext.h"
#include "Render/VulkanBase/VulkanRenderContext.h"

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

    void PushLayer(const Ref<Layer> &layer);

    void PushOverlay(const Ref<Layer> &layer);

    [[nodiscard]] Window &GetWindow() const { return *m_Window; }

    void Close();

    static Application &Get() { return *s_Instance; }


    [[nodiscard]] ApplicationCommandLineArgs GetCommandLineArgs() const { return m_CommandLineArgs; }


    [[nodiscard]] float GetFPS() const { return m_FPS; }

    /// 访问 Vulkan 全局上下文（提供给 Layer 等创建 Vulkan 资源用）。
    static VulkanContext &GetVulkanContext() { return *Get().m_VulkanContext; }

    static const VulkanSwapchain &GetSwapchain() { return Get().m_RenderContext->GetSwapchain(); }
    static VulkanRenderContext &GetRenderContext() { return *Get().m_RenderContext; }


    /// 帧渲染辅助：当前帧的 command buffer 和 image view。
    static vk::CommandBuffer GetFrameCmd() { return GetRenderContext().GetActiveFrameCmd(); }
    static uint32_t GetFrameImageIndex() { return GetRenderContext().GetActiveFrameIndex(); }
    static vk::ImageView GetFrameImageView() { return GetRenderContext().GetActiveFrame().GetRenderTarget().GetSwapchainView(); }

private:
    void Run();

    void RecreateSwapchain();

    bool OnWindowClose(WindowCloseEvent &e);

    bool OnWindowResized(const WindowResizeEvent &e);

private:
    ApplicationCommandLineArgs m_CommandLineArgs;

    Ref<ImGuiLayer> m_ImGuiLayer;

    bool m_Running = true;
    Scope<Window> m_Window;
    LayerStack m_LayerStack;
    float m_LastFrameTime = 0.0f;
    bool m_Minimized = false;

    float m_FPS = 0.0f;
    float m_FrameTimeAccumulator = 0.0f;
    int m_FrameCount = 0;

    // -- Vulkan 资源 --
    std::unique_ptr<VulkanContext> m_VulkanContext;
    std::unique_ptr<VulkanRenderContext> m_RenderContext;

private:
    static Application *s_Instance;

    friend int ::main(int argc, char **argv);
};

extern Application *CreateApplication(ApplicationCommandLineArgs args);

} // namespace GE
