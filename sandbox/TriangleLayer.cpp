//
// 不使用 Renderer 的纯 Vulkan 三角形绘制 —— 演示新 Pipeline/State API
//

#include "TriangleLayer.h"
#include "GE/Core/Application.h"

#include "GE/Render/VulkanBase/VulkanRenderingInfo.h"
#include "GE/Render/VulkanBase/VulkanResourceCache.h"

#include "imgui.h"

namespace GE {

TriangleLayer::TriangleLayer() : Layer("TriangleLayer") {
}

TriangleLayer::~TriangleLayer() = default;

void TriangleLayer::OnAttach() {
    auto &ctx = Application::GetVulkanContext();
    auto &device = ctx.GetDevice(); // VulkanDevice &
    auto allocator = ctx.GetVmaAllocator();
    auto &swapchain = Application::GetSwapchain();
    auto colorFmt = swapchain.GetFormat();

    // ── 1. 通过全局资源缓存创建 ShaderModule（去重）──────────────
    auto &cache = device.GetResourceCache();
    m_VertShader = &cache.RequestShaderModule(
        vk::ShaderStageFlagBits::eVertex,
        ShaderSource("assets/shaders/glsl/triangle_raw.vert.spv"),
        "main", ShaderVariant{});

    m_FragShader = &cache.RequestShaderModule(
        vk::ShaderStageFlagBits::eFragment,
        ShaderSource("assets/shaders/glsl/triangle_raw.frag.spv"),
        "main", ShaderVariant{});

    // ── 2. 通过全局资源缓存创建 PipelineLayout（去重）────────────
    m_PipelineLayout = &cache.RequestPipelineLayout(
        {m_VertShader, m_FragShader});

    // ── 3. 配置 PipelineState ────────────────────────────────────────
    m_PipelineState = VulkanPipelineState{};
    m_PipelineState.pipelineLayout = m_PipelineLayout;
    m_PipelineState.colorAttachmentFormats = {colorFmt};
    m_PipelineState.depthFormat = {};
    m_PipelineState.stencilFormat = {};

    // 顶点输入：从顶点着色器反射自动生成（vec2 position + vec3 color，紧密打包，stride = 20）
    m_PipelineState.SetVertexInputFromShader(*m_VertShader);

    // 混合附件（必须显式设置 colorWriteMask，否则默认 0 导致不写入颜色）
    vk::PipelineColorBlendAttachmentState blendState{};
    blendState.colorWriteMask = vk::ColorComponentFlagBits::eR
                              | vk::ColorComponentFlagBits::eG
                              | vk::ColorComponentFlagBits::eB
                              | vk::ColorComponentFlagBits::eA;
    m_PipelineState.SetBlendAttachments({blendState});

    // 标记为动态状态（运行时通过 vkCmdSet* 更新）
    m_PipelineState.cullMode.SetDynamic(true);
    m_PipelineState.frontFace.SetDynamic(true);
    m_PipelineState.topology.SetDynamic(true);
    m_PipelineState.depthTestEnable = VK_FALSE;
    m_PipelineState.depthWriteEnable = VK_FALSE;

    // ── 4. 通过全局资源缓存创建图形管线（去重）────────────────────
    m_Pipeline = &cache.RequestGraphicsPipeline(m_PipelineState);

    // ── 5. 创建顶点 buffer ───────────────────────────────────────────
    struct Vertex {
        float x, y;
        float r, g, b;
    };

    Vertex vertices[] = {
        {-0.5f, -0.5f, 1.0f, 0.0f, 0.0f},
        {0.5f, -0.5f, 0.0f, 1.0f, 0.0f},
        {0.0f, 0.5f, 0.0f, 0.0f, 1.0f},
    };

    m_VertexBuffer = std::make_unique<VulkanBuffer>(
        device, sizeof(vertices),
        vk::BufferUsageFlagBits::eVertexBuffer);
    m_VertexBuffer->update(vertices, sizeof(vertices));
}

void TriangleLayer::OnDetach() {
    m_VertexBuffer.reset();
    m_VertShader = nullptr;
    m_FragShader = nullptr;
    m_PipelineLayout = nullptr;
    m_Pipeline = nullptr;
}

void TriangleLayer::OnUpdate(Timestep &ts) {
    auto &cmd = Application::GetFrameCmd();
    auto vkCmd = cmd.GetHandle();
    auto extent = Application::GetSwapchain().GetExtent();

    // ── 开始动态渲染 ──────────────────────────────────────────────────
    vk::ClearValue clearValue;
    clearValue.color = std::array<float, 4>{0.0f, 0.0f, 0.0f, 1.0f};

    VulkanRenderingInfo renderInfo;
    renderInfo.SetRenderArea(0, 0, extent.width, extent.height);
    renderInfo.AddColorAttachment(Application::GetFrameImageView().GetHandle(),
                                  vk::AttachmentLoadOp::eClear,
                                  vk::AttachmentStoreOp::eStore,
                                  clearValue);
    renderInfo.Begin(vkCmd);

    // ── 绑定管线 ──────────────────────────────────────────────────────
    vkCmd.bindPipeline(vk::PipelineBindPoint::eGraphics, m_Pipeline->GetHandle());

    // ── 动态状态 ──────────────────────────────────────────────────────
    vk::Viewport vp;
    vp.width = static_cast<float>(extent.width);
    vp.height = static_cast<float>(extent.height);
    vp.minDepth = 0.0f;
    vp.maxDepth = 1.0f;
    vkCmd.setViewport(0, vp);

    vk::Rect2D scissor;
    scissor.extent.width = extent.width;
    scissor.extent.height = extent.height;
    vkCmd.setScissor(0, scissor);

    vkCmd.setCullMode(vk::CullModeFlagBits::eNone);
    vkCmd.setFrontFace(vk::FrontFace::eCounterClockwise);
    vkCmd.setPrimitiveTopology(vk::PrimitiveTopology::eTriangleList);

    // ── 绑定顶点 buffer ───────────────────────────────────────────────
    vk::Buffer vb = m_VertexBuffer->GetHandle();
    vkCmd.bindVertexBuffers(0, vb, {0});

    // ── 绘制 3 个顶点 ─────────────────────────────────────────────────
    vkCmd.draw(3, 1, 0, 0);

    // ── 结束渲染 ──────────────────────────────────────────────────────
    VulkanRenderingInfo::End(vkCmd);
}

void TriangleLayer::OnEvent(Event &event) {
}

void TriangleLayer::OnImGuiRender() {
    ImGui::Begin("TriangleLayer");
    ImGui::Text("直接使用 Vulkan 绘制的彩色三角形");
    ImGui::Text("新 Pipeline/State API");
    ImGui::Separator();
    ImGui::Text("着色器：triangle_raw.vert / triangle_raw.frag");
    ImGui::Text("无 UBO、无纹理、无深度测试 — 最简管线");
    ImGui::End();
}

} // namespace GE