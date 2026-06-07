#include "pch.h"

#include "ImGui/ImGuiLayer.h"

#include "Core/Application.h"
#include "Core/Log.h"
#include "Render/Renderer2D.h"
#include "../../include/GE/Render/VulkanBase/VulkanRenderingInfo.h"

#include "GLFW/glfw3.h"

#include <backends/imgui_impl_glfw.h>
#include <backends/imgui_impl_vulkan.h>

#include "imgui.h"

namespace GE {

ImGuiLayer::ImGuiLayer() : Layer("ImGuiLayer") {
}

ImGuiLayer::~ImGuiLayer() = default;

void ImGuiLayer::OnAttach() {

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO &io = ImGui::GetIO();
    (void)io;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
    // io.ConfigFlags |= ImGuiConfigFlags_ViewportsEnable;

    ImFontConfig config;
    config.MergeMode = false;

    float fontSize = 24.0f;
    io.Fonts->AddFontFromFileTTF("assets/fonts/opensans/OpenSans-Bold.ttf", fontSize);
    io.FontDefault = io.Fonts->AddFontFromFileTTF("assets/fonts/opensans/OpenSans-Regular.ttf", fontSize);

    ImGui::StyleColorsDark();

    ImGuiStyle &style = ImGui::GetStyle();
    if (io.ConfigFlags & ImGuiConfigFlags_ViewportsEnable) {
        style.WindowRounding = 0.0f;
        style.Colors[ImGuiCol_WindowBg].w = 1.0f;
    }
    SetDarkThemeColors();

    // ---- GLFW platform backend ----
    Application &app = Application::Get();
    auto *window = static_cast<GLFWwindow *>(app.GetWindow().GetGlfwWindow());
    ImGui_ImplGlfw_InitForOther(window, true);

    // ---- Vulkan renderer backend ----
    auto &swapchain = Renderer2D::Get().GetSwapchain();

    auto color_format = static_cast<VkFormat>(swapchain.GetDimensions().format);

    VkPipelineRenderingCreateInfoKHR pipeline_rendering_info{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO_KHR,
        .colorAttachmentCount = 1,
        .pColorAttachmentFormats = &color_format,
    };

    auto &r = Renderer2D::Get();
    ImGui_ImplVulkan_InitInfo init_info{
        .ApiVersion = VK_API_VERSION_1_3,
        .Instance = static_cast<VkInstance>(r.GetVkInstance()),
        .PhysicalDevice = static_cast<VkPhysicalDevice>(r.GetVkGpu()),
        .Device = static_cast<VkDevice>(r.GetVkDevice()),
        .QueueFamily = static_cast<uint32_t>(r.GetGraphicsQueueIndex()),
        .Queue = static_cast<VkQueue>(r.GetVkQueue()),
        .DescriptorPoolSize = 1024,
        .MinImageCount = swapchain.GetImageCount(),
        .ImageCount = swapchain.GetImageCount(),
        .PipelineInfoMain = {
            .PipelineRenderingCreateInfo = pipeline_rendering_info,
        },
        .UseDynamicRendering = true,
        .CheckVkResultFn = [](VkResult err) {
            if (err != VK_SUCCESS)
                GE_CORE_ERROR("ImGui Vulkan error: {}", static_cast<int>(err));
        },

    };

    // ---- Load Vulkan function pointers for ImGui ----
    // VK_NO_PROTOTYPES (set globally in CMake) forces the ImGui backend into
    // IMGUI_IMPL_VULKAN_NO_PROTOTYPES mode, so we must populate its function
    // pointer table before it can do anything.
    {
        auto vkGetInstanceProcAddr = r.GetInstance().GetVkGetInstanceProcAddr();
        VkInstance vk_instance = static_cast<VkInstance>(r.GetVkInstance());
        struct LoaderCtx {
            PFN_vkGetInstanceProcAddr get;
            VkInstance inst;
        } ctx{vkGetInstanceProcAddr, vk_instance};
        ImGui_ImplVulkan_LoadFunctions(VK_API_VERSION_1_3,
                                       [](const char *name, void *user_data) -> PFN_vkVoidFunction {
                                           auto &d = *static_cast<LoaderCtx *>(user_data);
                                           return d.get(d.inst, name);
                                       },
                                       &ctx);
    }

    ImGui_ImplVulkan_Init(&init_info);
}

void ImGuiLayer::OnDetach() {
    GE_CORE_INFO("ImGui Shutdown");
    ImGui_ImplVulkan_Shutdown();
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
    ImGui_ImplVulkan_NewFrame();
    ImGui_ImplGlfw_NewFrame();
    ImGui::NewFrame();
}

void ImGuiLayer::End() {
    ImGuiIO &io = ImGui::GetIO();
    Application &app = Application::Get();
    io.DisplaySize = ImVec2(static_cast<float>(app.GetWindow().GetWidth()),
                            static_cast<float>(app.GetWindow().GetHeight()));

    ImGui::Render();

    auto &swapchain = Renderer2D::Get().GetSwapchain();
    auto cmd = swapchain.GetCurrentCmd();

    // Render ImGui on top with loadOp = eLoad to preserve the scene.
    VulkanRenderingInfo render_info;
    render_info.SetRenderArea(0, 0, swapchain.GetDimensions().width, swapchain.GetDimensions().height);
    render_info.AddColorAttachment(swapchain.GetCurrentImageView(),
                                   vk::AttachmentLoadOp::eLoad,
                                   vk::AttachmentStoreOp::eStore);
    render_info.Begin(cmd);
    ImGui_ImplVulkan_RenderDrawData(ImGui::GetDrawData(), cmd);
    render_info.End(cmd);
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
