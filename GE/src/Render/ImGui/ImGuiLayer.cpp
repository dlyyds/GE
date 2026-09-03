#include "pch.h"

#include "Render/ImGui/ImGuiLayer.h"

#include "Core/Log.h"

#include "Render/Renderer.h"
#include "Render/AssetManager.h"
#include "Render/VulkanBase/VulkanRenderingInfo.h"
#include "Render/VulkanBase/VulkanContext.h"

#include "GLFW/glfw3.h"

#include <backends/imgui_impl_glfw.h>
#include <backends/imgui_impl_vulkan.h>

#include "imgui.h"
#include "ImGuizmo.h"

namespace GE {

ImGuiLayer::ImGuiLayer(Renderer &renderer) : m_Renderer(renderer) {
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

    float fontSize = 24.0f;

    // 加载英文字体（基础字体，包含 ASCII）
    ImFontConfig cfg;
    cfg.MergeMode = false;
    io.FontDefault = io.Fonts->AddFontFromFileTTF(
        Renderer::GetAssetManager().ResolvePath(std::string(AssetPaths::Fonts) + "/OpenSans-Regular.ttf").string().c_str(),
        fontSize, &cfg);

    // 合并中文字体（微软雅黑）— 使中文标点和 CJK 字符能正确显示
    cfg.MergeMode = true;
    static const ImWchar cjkRanges[] = {
        0x2000, 0x206F, // 通用标点
        0x3000, 0x303F, // CJK 符号和标点
        0x4E00, 0x9FFF, // CJK 统一表意文字
        0xFF00, 0xFFEF, // 全角/半角形式
        0
    };
    io.Fonts->AddFontFromFileTTF(
        "C:\\Windows\\Fonts\\msyh.ttc", fontSize, &cfg, cjkRanges);

    ImGui::StyleColorsDark();

    ImGuiStyle &style = ImGui::GetStyle();
    if (io.ConfigFlags & ImGuiConfigFlags_ViewportsEnable) {
        style.WindowRounding = 0.0f;
        style.Colors[ImGuiCol_WindowBg].w = 1.0f;
    }
    SetDarkThemeColors();

    // ---- GLFW platform backend ----
    auto *window = static_cast<GLFWwindow *>(m_Renderer.GetWindowRef().GetGlfwWindow());
    ImGui_ImplGlfw_InitForOther(window, true);

    // ---- Vulkan renderer backend ----
    auto &swapchain = Renderer::GetSwapchain();
    auto &ctx = Renderer::GetVulkanContext();

    auto color_format = static_cast<VkFormat>(swapchain.GetFormat());

    VkPipelineRenderingCreateInfoKHR pipeline_rendering_info{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO_KHR,
        .colorAttachmentCount = 1,
        .pColorAttachmentFormats = &color_format,
    };

    ImGui_ImplVulkan_InitInfo init_info{
        .ApiVersion = VK_API_VERSION_1_3,
        .Instance = static_cast<VkInstance>(ctx.GetVkInstance()),
        .PhysicalDevice = static_cast<VkPhysicalDevice>(ctx.GetVkGpu()),
        .Device = static_cast<VkDevice>(ctx.GetVkDevice()),
        .QueueFamily = static_cast<uint32_t>(ctx.GetGraphicsQueueIndex()),
        .Queue = static_cast<VkQueue>(ctx.GetVkQueue()),
        .DescriptorPoolSize = 1024,
        .MinImageCount = static_cast<uint32_t>(swapchain.GetImages().size()),
        .ImageCount = static_cast<uint32_t>(swapchain.GetImages().size()),
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
        auto vkGetInstanceProcAddr = ctx.GetInstance().GetVkGetInstanceProcAddr();
        VkInstance vk_instance = static_cast<VkInstance>(ctx.GetVkInstance());
        struct LoaderCtx {
            PFN_vkGetInstanceProcAddr get;
            VkInstance inst;
        } loader_ctx{vkGetInstanceProcAddr, vk_instance};
        ImGui_ImplVulkan_LoadFunctions(VK_API_VERSION_1_3,
                                       [](const char *name, void *user_data) -> PFN_vkVoidFunction {
                                           auto &d = *static_cast<LoaderCtx *>(user_data);
                                           return d.get(d.inst, name);
                                       },
                                       &loader_ctx);
    }

    ImGui_ImplVulkan_Init(&init_info);
}

void ImGuiLayer::OnDetach() {
    GE_CORE_INFO("ImGui Shutdown");
    ImGui_ImplVulkan_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();
}

void ImGuiLayer::Begin() {
    ImGui_ImplVulkan_NewFrame();
    ImGui_ImplGlfw_NewFrame();
    ImGui::NewFrame();
}

void ImGuiLayer::End() {
    ImGuiIO &io = ImGui::GetIO();
    io.DisplaySize = ImVec2(static_cast<float>(m_Renderer.GetWindowRef().GetWidth()),
                            static_cast<float>(m_Renderer.GetWindowRef().GetHeight()));

    ImGui::Render();

    auto &cmd = Renderer::GetFrameCmd();
    auto vkCmd = cmd.GetHandle();

    // Render ImGui on top with loadOp = eLoad to preserve the scene.
    VulkanRenderingInfo render_info;
    render_info.SetRenderArea(0, 0, static_cast<uint32_t>(io.DisplaySize.x),
                              static_cast<uint32_t>(io.DisplaySize.y));
    render_info.AddColorAttachment(Renderer::GetFrameImageView().GetHandle(),
                                   vk::AttachmentLoadOp::eLoad,
                                   vk::AttachmentStoreOp::eStore);
    render_info.Begin(vkCmd);
    ImGui_ImplVulkan_RenderDrawData(ImGui::GetDrawData(), vkCmd);
    render_info.End(vkCmd);
}

bool ImGuiLayer::WantCaptureImGuiInput() const {
    const ImGuiIO &io = ImGui::GetIO();
    return io.WantCaptureMouse || io.WantCaptureKeyboard;
}

void ImGuiLayer::OnImGuiRender() {
    // ── 渲染统计面板 ──────────────────────────────────────────────────
    // 停靠进主 DockSpace（根上下文取 "MainDockspace"，与 DockSpaceLayer 一致）
    ImGui::SetNextWindowDockID(ImGui::GetID("MainDockspace"), ImGuiCond_FirstUseEver);
    if (ImGui::Begin("渲染统计")) {
        const auto &stats = Renderer::GetStats();

        ImGui::Text("帧率: %.1f FPS", Renderer::GetFPS());

        ImGui::Separator();
        ImGui::Text("2D（精灵批处理）");
        ImGui::BulletText("Draw Calls: %u", stats.drawCalls2D);
        ImGui::BulletText("三角形: %u", stats.triangles2D);

        ImGui::Separator();
        ImGui::Text("3D（网格）");
        ImGui::BulletText("Draw Calls: %u", stats.drawCalls3D);
        ImGui::BulletText("排序批次: %u", stats.batches3D);
        ImGui::BulletText("三角形: %u", stats.triangles3D);

        ImGui::Separator();
        ImGui::Text("总计");
        ImGui::BulletText("Draw Calls: %u", stats.TotalDrawCalls());
        ImGui::BulletText("三角形: %u", stats.TotalTriangles());
    }
    ImGui::End();
}

void ImGuiLayer::OnSwapchainRecreated() {
    // ImGui 的 Vulkan backend 会在下次 ImGui_ImplVulkan_RenderDrawData 时
    // 主动查询当前 swapchain image 的 framebuffer；重建后 image view 已换新，
    // 无需额外刷新描述符。仅确保 resize 后 display size 来自新窗口尺寸。
    // （显式留空：backend 内部在 CreateDeviceObjects/NewFrame 阶段会处理。）
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
