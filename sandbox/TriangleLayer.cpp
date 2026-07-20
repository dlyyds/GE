//
// 不使用 Renderer 的纯 Vulkan 三角形绘制 —— 演示新 Pipeline/State API
//

#include "TriangleLayer.h"
#include "GE/Core/Application.h"

#include "GE/Render/VulkanBase/VulkanRenderingInfo.h"

#include "imgui.h"

namespace GE {

TriangleLayer::TriangleLayer() : Layer("TriangleLayer") {
}

TriangleLayer::~TriangleLayer() = default;

void TriangleLayer::OnAttach() {
    auto &ctx       = Application::GetVulkanContext();
    auto &device    = ctx.GetDevice();          // VulkanDevice &
    auto  allocator = ctx.GetVmaAllocator();
    auto &swapchain = Application::GetSwapchain();
    auto  colorFmt  = swapchain.GetFormat();

    // ── 1. 创建 ShaderModule（包含 SPIR-V 加载 + vk::ShaderModule 创建）─
    m_VertShader = std::make_unique<ShaderModule>(
        device, vk::ShaderStageFlagBits::eVertex,
        ShaderSource("assets/shaders/glsl/triangle_raw.vert.spv"),
        "main", ShaderVariant{});

    m_FragShader = std::make_unique<ShaderModule>(
        device, vk::ShaderStageFlagBits::eFragment,
        ShaderSource("assets/shaders/glsl/triangle_raw.frag.spv"),
        "main", ShaderVariant{});

    // ── 2. 创建 PipelineLayout（从着色器反射 descriptor set）─────────
    m_PipelineLayout = std::make_unique<VulkanPipelineLayout>(
        device,
        std::vector<ShaderModule *>{m_VertShader.get(), m_FragShader.get()});

    // ── 3. 配置 PipelineState ────────────────────────────────────────
    m_PipelineState.Reset();
    m_PipelineState.pipelineLayout          = m_PipelineLayout.get();
    m_PipelineState.colorAttachmentFormats  = {{colorFmt}};
    m_PipelineState.depthFormat             = {};
    m_PipelineState.stencilFormat           = {};

    // 顶点输入（匹配着色器的 vertex input 布局）
    // location 0: vec2 position (offset 0)
    // location 1: vec3 color   (offset 8)
    // stride: 20
    m_PipelineState.vertexBindingDescriptions = std::vector<vk::VertexInputBindingDescription>{
        {0, 20, vk::VertexInputRate::eVertex},
    };
    m_PipelineState.vertexAttributeDescriptions = std::vector<vk::VertexInputAttributeDescription>{
        {0, 0, vk::Format::eR32G32Sfloat,   0},                                      // position
        {1, 0, vk::Format::eR32G32B32Sfloat, static_cast<uint32_t>(2 * sizeof(float))},  // color
    };

    // 混合附件（与 colorAttachmentFormats 数量匹配）
    m_PipelineState.SetBlendAttachments({GE::BlendAttachment{}});

    // 标记为动态状态（运行时通过 vkCmdSet* 更新）
    // 注意：viewport/scissor 矩形已在 GetEnabledDynamicStates() 中始终启用，
    //       但 viewportCount/scissorCount 保持静态（值固定为 1），
    //       运行时用 vkCmdSetViewport / vkCmdSetScissor 更新矩形即可。
    m_PipelineState.cullMode.SetDynamic(true);
    m_PipelineState.frontFace.SetDynamic(true);
    m_PipelineState.topology.SetDynamic(true);
    m_PipelineState.depthTestEnable = VK_FALSE;    // 无 depth attachment
    m_PipelineState.depthWriteEnable = VK_FALSE;

    // ── 4. 创建图形管线 ──────────────────────────────────────────────
    m_Pipeline = std::make_unique<VulkanGraphicsPipeline>(
        device, VK_NULL_HANDLE, m_PipelineState);

    // ── 5. 创建顶点 buffer ───────────────────────────────────────────
    // 每个顶点：位置 vec2（8 字节）+ 颜色 vec3（12 字节）, stride = 20
    struct Vertex {
        float x, y;     // position (location 0)
        float r, g, b;  // color    (location 1)
    };

    Vertex vertices[] = {
        {-0.5f, -0.5f, 1.0f, 0.0f, 0.0f},   // 左下 — 红
        { 0.5f, -0.5f, 0.0f, 1.0f, 0.0f},   // 右下 — 绿
        { 0.0f,  0.5f, 0.0f, 0.0f, 1.0f},   // 顶部 — 蓝
    };

    m_VertexBuffer = std::make_unique<VulkanBuffer>(
        device, sizeof(vertices),
        vk::BufferUsageFlagBits::eVertexBuffer);
    m_VertexBuffer->update(vertices, sizeof(vertices));
}

void TriangleLayer::OnDetach() {
    // 按创建逆序销毁
    m_VertexBuffer.reset();

    // unique_ptr 自动析构，顺序：Pipeline → PipelineLayout → ShaderModules
    m_Pipeline.reset();
    m_PipelineLayout.reset();
    m_VertShader.reset();
    m_FragShader.reset();
}

void TriangleLayer::OnUpdate(Timestep &ts) {
    auto &cmd       = Application::GetFrameCmd();
    auto vkCmd      = cmd.GetHandle();
    auto extent     = Application::GetSwapchain().GetExtent();

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

    // ── 绑定管线（必须先 bind，再设动态状态 —— bind 会重置动态状态） ──
    m_Pipeline->Bind(vkCmd);

    // ── 动态状态 ──────────────────────────────────────────────────────
    vk::Viewport vp;
    vp.width  = static_cast<float>(extent.width);
    vp.height = static_cast<float>(extent.height);
    vp.minDepth = 0.0f;
    vp.maxDepth = 1.0f;
    vkCmd.setViewport(0, vp);

    vk::Rect2D scissor;
    scissor.extent.width  = extent.width;
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
    // 本层不需要处理事件
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
