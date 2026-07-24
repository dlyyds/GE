//
// 显示棋盘纹理的 TextureLayer —— 演示纹理加载、采样器、描述符集
//

#include "TextureLayer.h"
#include "GE/Core/Application.h"

#include "GE/Render/VulkanBase/VulkanRenderingInfo.h"
#include "GE/Render/VulkanBase/VulkanResourceCache.h"

#include "imgui.h"

#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>

namespace GE {

TextureLayer::TextureLayer() : Layer("TextureLayer") {
}

TextureLayer::~TextureLayer() = default;

void TextureLayer::OnAttach() {
    auto &ctx = Application::GetVulkanContext();
    auto &device = ctx.GetDevice();

    // ── 1. 通过全局资源缓存创建 ShaderModule ──────────────────────────────
    auto &cache = device.GetResourceCache();
    m_VertShader = &cache.RequestShaderModule(
        vk::ShaderStageFlagBits::eVertex,
        ShaderSource("assets/shaders/glsl/triangle.vert.spv"),
        "main", ShaderVariant{});

    m_FragShader = &cache.RequestShaderModule(
        vk::ShaderStageFlagBits::eFragment,
        ShaderSource("assets/shaders/glsl/triangle.frag.spv"),
        "main", ShaderVariant{});

    // ── 2. 通过全局资源缓存创建 PipelineLayout ────────────────────────────
    m_PipelineLayout = &cache.RequestPipelineLayout(
        {m_VertShader, m_FragShader});

    // ── 3. 加载棋盘纹理 ────────────────────────────────────────────────────
    m_Texture = Texture::LoadFromFile(device, cache, "assets/textures/Checkerboard.png");

    // ── 4. 创建顶点 buffer（全屏四边形：位置 + UV） ───────────────────────
    struct Vertex {
        float x, y;
        float u, v;
    };

    Vertex vertices[] = {
        {-1.0f, -1.0f, 0.0f, 0.0f},
        {1.0f, -1.0f, 1.0f, 0.0f},
        {1.0f, 1.0f, 1.0f, 1.0f},
        {-1.0f, 1.0f, 0.0f, 1.0f},
    };

    m_VertexBuffer = std::make_unique<VulkanBuffer>(
        device, sizeof(vertices),
        vk::BufferUsageFlagBits::eVertexBuffer);
    m_VertexBuffer->update(vertices, sizeof(vertices));

    // ── 5. 创建索引 buffer（2 个三角形） ──────────────────────────────────
    uint32_t indices[] = {
        0, 1, 2,
        2, 3, 0,
    };

    m_IndexBuffer = std::make_unique<VulkanBuffer>(
        device, sizeof(indices),
        vk::BufferUsageFlagBits::eIndexBuffer);
    m_IndexBuffer->update(indices, sizeof(indices));

    // ── 6. 创建 uniform buffer（MVP 矩阵） ───────────────────────────────
    m_UniformBuffer = std::make_unique<VulkanBuffer>(
        device, sizeof(glm::mat4) * 3 + sizeof(glm::vec4),
        vk::BufferUsageFlagBits::eUniformBuffer,
        VMA_MEMORY_USAGE_AUTO,
        VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT);

}

void TextureLayer::OnDetach() {
    m_UniformBuffer.reset();
    m_IndexBuffer.reset();
    m_VertexBuffer.reset();
    m_Texture.reset();

    m_PipelineLayout = nullptr;
    m_FragShader = nullptr;
    m_VertShader = nullptr;
}

void TextureLayer::OnUpdate(Timestep &ts) {
    auto &cmd = Application::GetFrameCmd();
    auto vkCmd = cmd.GetHandle();
    auto extent = Application::GetSwapchain().GetExtent();

    // ── 更新 uniform buffer ──────────────────────────────────────────────
    struct UniformBlock {
        glm::mat4 model;
        glm::mat4 view;
        glm::mat4 projection;
        glm::vec4 color;
    };

    static float rotation = 0.0f;
    rotation += ts.GetSeconds() * 0.5f;

    UniformBlock ubo{};
    ubo.model = glm::rotate(glm::mat4(1.0f), rotation, glm::vec3(0.0f, 0.0f, 1.0f));
    ubo.view = glm::lookAt(glm::vec3(0.0f, 0.0f, 4.0f),
                           glm::vec3(0.0f, 0.0f, 0.0f),
                           glm::vec3(0.0f, 1.0f, 0.0f));
    ubo.projection = glm::perspectiveZO(glm::radians(45.0f),
                                        static_cast<float>(extent.width) /
                                        static_cast<float>(extent.height),
                                        0.1f, 100.0f);
    ubo.projection[1][1] *= -1.0f;
    ubo.color = glm::vec4(1.0f);

    m_UniformBuffer->update(&ubo, sizeof(ubo));

    // ── 开始动态渲染 ──────────────────────────────────────────────────────
    vk::ClearValue clearValue;
    clearValue.color = std::array<float, 4>{0.1f, 0.1f, 0.1f, 1.0f};

    VulkanRenderingInfo renderInfo;
    renderInfo.SetRenderArea(0, 0, extent.width, extent.height);
    renderInfo.AddColorAttachment(Application::GetFrameImageView().GetHandle(),
                                  vk::AttachmentLoadOp::eClear,
                                  vk::AttachmentStoreOp::eStore,
                                  clearValue);
    renderInfo.Begin(vkCmd);

    // ── 设置 pipeline layout（Draw 时自动从 ResourceCache 获取管线并绑定） ──
    cmd.BindPipelineLayout(*m_PipelineLayout);

    auto &ps = cmd.GetPipelineState();
    auto colorFmt = Application::GetSwapchain().GetFormat();

    // 附件格式
    ps.colorAttachmentFormats = {colorFmt};
    ps.depthFormat = {};
    ps.stencilFormat = {};

    // 顶点输入：位置 vec2 (offset 0) + UV vec2 (offset 8), stride = 16
    ps.vertexBindingDescriptions = std::vector<vk::VertexInputBindingDescription>{
        {0, 16, vk::VertexInputRate::eVertex},
    };
    ps.vertexAttributeDescriptions = std::vector<vk::VertexInputAttributeDescription>{
        {0, 0, vk::Format::eR32G32Sfloat, 0}, // position
        {1, 0, vk::Format::eR32G32Sfloat, static_cast<uint32_t>(2 * sizeof(float))}, // uv
    };

    // 混合附件（必须显式设置 colorWriteMask，否则默认 0 导致不写入颜色）
    vk::PipelineColorBlendAttachmentState blendState{};
    blendState.colorWriteMask = vk::ColorComponentFlagBits::eR
                              | vk::ColorComponentFlagBits::eG
                              | vk::ColorComponentFlagBits::eB
                              | vk::ColorComponentFlagBits::eA;
    ps.SetBlendAttachments({blendState});

    // 启用动态状态
    ps.cullMode.SetDynamic(true);
    ps.frontFace.SetDynamic(true);
    ps.topology.SetDynamic(true);
    ps.depthTestEnable = VK_FALSE;
    ps.depthWriteEnable = VK_FALSE;

    // ── 动态状态 ──────────────────────────────────────────────────────────
    vk::Viewport vp;
    vp.width = static_cast<float>(extent.width);
    vp.height = static_cast<float>(extent.height);
    vp.minDepth = 0.0f;
    vp.maxDepth = 1.0f;
    cmd.SetViewport(0, {vp});

    vk::Rect2D scissor;
    scissor.extent.width = extent.width;
    scissor.extent.height = extent.height;
    cmd.SetScissor(0, {scissor});

    // 动态管线状态写入 pipeline state（flush 时会创建对应管线）
    cmd.GetPipelineState().cullMode = vk::CullModeFlagBits::eNone;
    cmd.GetPipelineState().frontFace = vk::FrontFace::eCounterClockwise;
    cmd.GetPipelineState().topology = vk::PrimitiveTopology::eTriangleList;

    // ── 绑定资源（惰性，DrawIndexed 时自动 flush 分配 descriptor set） ────
    cmd.BindBuffer(*m_UniformBuffer, 0, sizeof(UniformBlock), 0, 0);

    if (m_Texture) {
        cmd.BindImage(m_Texture->GetImageView(), m_Texture->GetSampler(), 0, 1);
    }

    // ── 绑定顶点/索引 buffer 并绘制（DrawIndexed 内部自动 Flush） ─────────
    cmd.BindVertexBuffers(0, {std::ref(*m_VertexBuffer)}, {0});
    cmd.BindIndexBuffer(*m_IndexBuffer, 0, vk::IndexType::eUint32);
    cmd.DrawIndexed(6, 1, 0, 0, 0);

    // ── 结束渲染 ──────────────────────────────────────────────────────────
    VulkanRenderingInfo::End(vkCmd);
}

void TextureLayer::OnEvent(Event &event) {
}

void TextureLayer::OnImGuiRender() {
    ImGui::Begin("TextureLayer");
    ImGui::Text("显示棋盘纹理的四边形");
    ImGui::Separator();
    ImGui::Text("着色器：triangle.vert / triangle.frag");
    ImGui::Text("纹理：Checkerboard.png");
    ImGui::Text("管线：UBO + 纹理采样器");
    if (m_Texture) {
        ImGui::Text("纹理尺寸：%d x %d",
                    m_Texture->GetExtent().width,
                    m_Texture->GetExtent().height);
    }
    ImGui::End();
}

} // namespace GE