#include "pch.h"

#include "ImGui/ImGuiLayer.h"

#include "GLFW/glfw3.h"

#include "Core/Application.h"

#include "GLFW/glfw3.h"

#include <backends/imgui_impl_glfw.h>
#include <backends/imgui_impl_opengl3.h>

#include "imgui.h"
//#include "ImGuizmo.h"

#include "Core/Log.h"
#define IMGUI_ENABLE_DOCKING

namespace GE {

ImGuiLayer::ImGuiLayer() : Layer("ImGuiLayer") {
}

ImGuiLayer::~ImGuiLayer() = default;

void ImGuiLayer::OnAttach() {

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO &io = ImGui::GetIO();
    (void)io;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard; // Enable Keyboard Controls
    //io.ConfigFlags |= ImGuiConfigFlags_NavEnableGamepad; // Enable Gamepad Controls
    io.ConfigFlags |= ImGuiConfigFlags_DockingEnable; // Enable Docking
    // io.ConfigFlags |= ImGuiConfigFlags_ViewportsEnable; // Enable Multi-Viewport / Platform Windows

    ImFontConfig config;
    config.MergeMode = false;

    float fontSize = 24.0f; // *2.0f;
    io.Fonts->AddFontFromFileTTF("assets/fonts/opensans/OpenSans-Bold.ttf", fontSize);
    io.FontDefault = io.Fonts->AddFontFromFileTTF("assets/fonts/opensans/OpenSans-Regular.ttf", fontSize);

    //  io.FontDefault = io.Fonts->AddFontFromFileTTF("C:/Windows/Fonts/msyh.ttc", 28.0f, &config,
    //                              io.Fonts->GetGlyphRangesChineseFull());

    // Setup Dear ImGui style
    ImGui::StyleColorsDark();

    ImGuiStyle &style = ImGui::GetStyle();
    if (io.ConfigFlags & ImGuiConfigFlags_ViewportsEnable) {
        style.WindowRounding = 0.0f;
        style.Colors[ImGuiCol_WindowBg].w = 1.0f;
    }
    SetDarkThemeColors();

    Application &app = Application::Get();
    auto *window = static_cast<GLFWwindow *>(app.GetWindow().GetNativeWindow());

    ImGui_ImplGlfw_InitForOther(window, true);

    // CGIMBGFX::InitConfig initConfig;
    // initConfig.autoShaderSampler = true;
    // initConfig.startingViewId = 1; // 从哪个 view ID 开始用，默认1（避免占用0）
    // initConfig.maxViews = 16;
    // GE_CORE_ASSERT(ImGui_Implbgfx_Init(initConfig));
    ImGui_ImplOpenGL3_Init("#version 410");
}


void ImGuiLayer::OnDetach() {
    GE_CORE_INFO("ImGui Shutdown");
    ImGui_ImplOpenGL3_Shutdown();
    //ImGui_Implbgfx_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();
}

void ImGuiLayer::OnEvent(Event &e) {
    if (m_BlockEvents) {
        const ImGuiIO &io = ImGui::GetIO();
        e.Handled |= e.IsInCategory(EventCategoryMouse) & io.WantCaptureMouse;
        e.Handled |= e.IsInCategory(EventCategoryKeyboard) & io.WantCaptureKeyboard;
    }
}


void ImGuiLayer::Begin() {

    ImGui_ImplOpenGL3_NewFrame();
    ImGui_ImplGlfw_NewFrame();
    ImGui::NewFrame();
    //   ImGuizmo::BeginFrame();
}

void ImGuiLayer::End() {
    ImGuiIO &io = ImGui::GetIO();
    Application &app = Application::Get();
    io.DisplaySize = ImVec2(static_cast<float>(app.GetWindow().GetWidth()),
                            static_cast<float>(app.GetWindow().GetHeight()));

    // Rendering
    ImGui::Render();
    //ImGui_Implbgfx_RenderDrawData(ImGui::GetDrawData());
    ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());

    // if (io.ConfigFlags & ImGuiConfigFlags_ViewportsEnable) {
    //     GLFWwindow *backup_current_context = glfwGetCurrentContext();
    //     ImGui::UpdatePlatformWindows();
    //     ImGui::RenderPlatformWindowsDefault();
    //     glfwMakeContextCurrent(backup_current_context);
    // }
}

void ImGuiLayer::OnImGuiRender() {
}

void ImGuiLayer::SetDarkThemeColors() {
    auto &colors = ImGui::GetStyle().Colors;
    colors[ImGuiCol_WindowBg] = ImVec4{0.1f, 0.105f, 0.11f, 1.0f};

    // Headers
    colors[ImGuiCol_Header] = ImVec4{0.2f, 0.205f, 0.21f, 1.0f};
    colors[ImGuiCol_HeaderHovered] = ImVec4{0.3f, 0.305f, 0.31f, 1.0f};
    colors[ImGuiCol_HeaderActive] = ImVec4{0.15f, 0.1505f, 0.151f, 1.0f};

    // Buttons
    colors[ImGuiCol_Button] = ImVec4{0.2f, 0.205f, 0.21f, 1.0f};
    colors[ImGuiCol_ButtonHovered] = ImVec4{0.3f, 0.305f, 0.31f, 1.0f};
    colors[ImGuiCol_ButtonActive] = ImVec4{0.15f, 0.1505f, 0.151f, 1.0f};

    // Frame BG
    colors[ImGuiCol_FrameBg] = ImVec4{0.2f, 0.205f, 0.21f, 1.0f};
    colors[ImGuiCol_FrameBgHovered] = ImVec4{0.3f, 0.305f, 0.31f, 1.0f};
    colors[ImGuiCol_FrameBgActive] = ImVec4{0.15f, 0.1505f, 0.151f, 1.0f};

    // Tabs
    colors[ImGuiCol_Tab] = ImVec4{0.15f, 0.1505f, 0.151f, 1.0f};
    colors[ImGuiCol_TabHovered] = ImVec4{0.38f, 0.3805f, 0.381f, 1.0f};
    colors[ImGuiCol_TabActive] = ImVec4{0.28f, 0.2805f, 0.281f, 1.0f};
    colors[ImGuiCol_TabUnfocused] = ImVec4{0.15f, 0.1505f, 0.151f, 1.0f};
    colors[ImGuiCol_TabUnfocusedActive] = ImVec4{0.2f, 0.205f, 0.21f, 1.0f};

    // Title
    colors[ImGuiCol_TitleBg] = ImVec4{0.15f, 0.1505f, 0.151f, 1.0f};
    colors[ImGuiCol_TitleBgActive] = ImVec4{0.15f, 0.1505f, 0.151f, 1.0f};
    colors[ImGuiCol_TitleBgCollapsed] = ImVec4{0.15f, 0.1505f, 0.151f, 1.0f};
}


} // namespace GE