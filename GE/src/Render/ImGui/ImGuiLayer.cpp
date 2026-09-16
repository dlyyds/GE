#include "pch.h"

#include "Render/ImGui/ImGuiLayer.h"

#include "Core/Log.h"

#include "Render/Renderer.h"
#include "Render/AssetManager.h"
#include "Render/VulkanBase/VulkanCommandBuffer.h"
#include "Render/VulkanBase/VulkanContext.h"

#include <SDL3/SDL.h>

#include <backends/imgui_impl_sdl3.h>
#include <backends/imgui_impl_vulkan.h>

#include "imgui.h"

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
    // 中文字体仍取系统字体，**这是已知的不可移植点**：路径写死了 Windows。
    // 换成随包字体是计划书阶段 D 的作业（届时经 VFS 读，并需先决定是否接受
    // 十兆级的 CJK 字体进 APK）。此处只加平台守卫，让它在非 Windows 上安静跳过
    // 而不是硬失败 —— 桌面行为一字未变。
#ifdef GE_PLATFORM_WINDOWS
    io.Fonts->AddFontFromFileTTF(
        "C:\\Windows\\Fonts\\msyh.ttc", fontSize, &cfg, cjkRanges);
#else
    GE_CORE_WARN("ImGuiLayer: 中文字体未随包，非 Windows 平台跳过 CJK 字体合并（界面中文将显示为方块）");
#endif

    ImGui::StyleColorsDark();

    ImGuiStyle &style = ImGui::GetStyle();
    if (io.ConfigFlags & ImGuiConfigFlags_ViewportsEnable) {
        style.WindowRounding = 0.0f;
        style.Colors[ImGuiCol_WindowBg].w = 1.0f;
    }
    SetDarkThemeColors();

    // ---- SDL3 platform backend ----
    // 与 GLFW 版的关键差异：SDL 后端没有"自己装回调"的模式，输入必须逐事件投递。
    // 这里挂到窗口的原始事件钩子上（见 Window::SetRawPlatformEventHook），由
    // SdlWindow 在翻译引擎事件之前回调过来，顺序与 GLFW 版 ImGui 回调链一致。
    auto *window = static_cast<SDL_Window *>(m_Renderer.GetWindowRef().GetNativeWindow());
    ImGui_ImplSDL3_InitForVulkan(window);
    m_Renderer.GetWindowRef().SetRawPlatformEventHook([](const void *raw_event) {
        ImGui_ImplSDL3_ProcessEvent(static_cast<const SDL_Event *>(raw_event));
    });

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
    // 先摘掉原始事件钩子再关后端：否则关停后若仍有事件轮询，会投递到已销毁的
    // ImGui 上下文。窗口在 ImGuiLayer 之前构造、之后析构，故这一步是必要的。
    m_Renderer.GetWindowRef().SetRawPlatformEventHook(nullptr);
    ImGui_ImplVulkan_Shutdown();
    ImGui_ImplSDL3_Shutdown();
    ImGui::DestroyContext();
}

void ImGuiLayer::Begin() {
    ImGui_ImplVulkan_NewFrame();
    ImGui_ImplSDL3_NewFrame();
    ImGui::NewFrame();
}

void ImGuiLayer::EndUI() {
    ImGuiIO &io = ImGui::GetIO();
    io.DisplaySize = ImVec2(static_cast<float>(m_Renderer.GetWindowRef().GetWidth()),
                            static_cast<float>(m_Renderer.GetWindowRef().GetHeight()));

    // 结束 CPU 侧 UI 帧数据生成；命令录制(RenderDrawData)延后到渲染图 UIPass 的
    // execute 回调里由 DrawUI 完成（那时动态渲染区间已由图打开）。
    ImGui::Render();
}

void ImGuiLayer::DrawUI(VulkanCommandBuffer &cmd) {
    // 渲染图 UIPass execute 回调内调用：区间已由图打开（颜色附件 = swapchain，
    // loadOp/store 已声明），这里只裸画。backend 不查布局、不发屏障、不自开区间。
    auto vkCmd = cmd.GetHandle();
    ImGui_ImplVulkan_RenderDrawData(ImGui::GetDrawData(), vkCmd);
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
