//
// 不使用 Renderer 的纯 Vulkan 三角形绘制 —— 演示直接操作 Vulkan API
//

#include "TriangleLayer.h"
#include "GE/Core/Application.h"
#include "GE/Render/Renderer.h"
#include "GE/Render/VulkanBase/VulkanRenderingInfo.h"

#include "imgui.h"

namespace GE {

TriangleLayer::TriangleLayer() : Layer("TriangleLayer") {
}

void TriangleLayer::OnAttach() {
    auto &ctx = Application::GetVulkanContext();
    auto device = ctx.GetVkDevice();
    auto allocator = ctx.GetVmaAllocator();
    auto &swapchain = Application::GetSwapchain();
    auto colorFmt = swapchain.GetDimensions().format;

    // ── 1. 加载着色器 ────────────────────────────────────────────
    m_VertShader.Init(device, "assets/shaders/glsl/triangle_raw.vert.spv",
                      vk::ShaderStageFlagBits::eVertex);
    m_FragShader.Init(device, "assets/shaders/glsl/triangle_raw.frag.spv",
                      vk::ShaderStageFlagBits::eFragment);

    // ── 2. 创建管线 ──────────────────────────────────────────────
    // 我们的着色器没有 descriptor binding，所以管线没有任何 set layout。
    // 也不开启深度测试。
    m_Pipeline.Init(device, colorFmt, m_VertShader, m_FragShader);

    // ── 3. 创建顶点 buffer ───────────────────────────────────────
    // 每个顶点：位置 vec2（8 字节）+ 颜色 vec3（12 字节）, stride = 20
    struct Vertex {
        float x, y; // position (location 0)
        float r, g, b; // color    (location 1)
    };

    Vertex vertices[] = {
        {-0.5f, -0.5f, 1.0f, 0.0f, 0.0f}, // 左下 — 红
        {0.5f, -0.5f, 0.0f, 1.0f, 0.0f}, // 右下 — 绿
        {0.0f, 0.5f, 0.0f, 0.0f, 1.0f}, // 顶部 — 蓝
    };

    m_VertexBuffer.Init(allocator, sizeof(vertices),
                        vk::BufferUsageFlagBits::eVertexBuffer);
    m_VertexBuffer.Upload(vertices, sizeof(vertices));
}

void TriangleLayer::OnDetach() {
    // 按创建逆序销毁资源
    m_VertexBuffer.Destroy();
    m_Pipeline.Cleanup();
    m_FragShader.Cleanup();
    m_VertShader.Cleanup();
}

void TriangleLayer::OnUpdate(Timestep &ts) {
    auto &swapchain = Application::GetSwapchain();
    auto cmd = swapchain.GetCurrentCmd();
    auto dim = swapchain.GetDimensions();

    // ── 开始动态渲染 ──────────────────────────────────────────────
    vk::ClearValue clearValue;
    clearValue.color = std::array<float, 4>{0.0f, 0.0f, 0.0f, 1.0f};

    VulkanRenderingInfo renderInfo;
    renderInfo.SetRenderArea(0, 0, dim.width, dim.height);
    renderInfo.AddColorAttachment(swapchain.GetCurrentImageView(),
                                  vk::AttachmentLoadOp::eClear,
                                  vk::AttachmentStoreOp::eStore,
                                  clearValue);
    renderInfo.Begin(cmd);

    // ── 动态状态 ──────────────────────────────────────────────────
    vk::Viewport vp;
    vp.width = static_cast<float>(dim.width);
    vp.height = static_cast<float>(dim.height);
    vp.minDepth = 0.0f;
    vp.maxDepth = 1.0f;
    cmd.setViewport(0, vp);

    vk::Rect2D scissor;
    scissor.extent.width = dim.width;
    scissor.extent.height = dim.height;
    cmd.setScissor(0, scissor);

    cmd.setCullMode(vk::CullModeFlagBits::eNone);
    cmd.setFrontFace(vk::FrontFace::eCounterClockwise);
    cmd.setPrimitiveTopology(vk::PrimitiveTopology::eTriangleList);

    // ── 绑定管线 ──────────────────────────────────────────────────
    m_Pipeline.Bind(cmd);

    // ── 绑定顶点 buffer ───────────────────────────────────────────
    vk::Buffer vb = m_VertexBuffer.GetBuffer();
    cmd.bindVertexBuffers(0, vb, {0});

    // ── 绘制 3 个顶点 ─────────────────────────────────────────────
    cmd.draw(3, 1, 0, 0);

    // ── 结束渲染 ──────────────────────────────────────────────────
    VulkanRenderingInfo::End(cmd);
}

void TriangleLayer::OnEvent(Event &event) {
    // 本层不需要处理事件
}

void TriangleLayer::OnImGuiRender() {
    ImGui::Begin("TriangleLayer");
    ImGui::Text("直接使用 Vulkan 绘制的彩色三角形");
    ImGui::Text("不使用 Renderer，直接操作 Vulkan API");
    ImGui::Separator();
    ImGui::Text("着色器：triangle_raw.vert / triangle_raw.frag");
    ImGui::Text("无 UBO、无纹理、无深度测试 — 最简管线");
    ImGui::End();
}

} // namespace GE
