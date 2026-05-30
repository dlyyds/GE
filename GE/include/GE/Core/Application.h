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

private:
    void Run();

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

private:
    static Application *s_Instance;

    friend int ::main(int argc, char **argv);
};

extern Application *CreateApplication(ApplicationCommandLineArgs args);

} // namespace GE
