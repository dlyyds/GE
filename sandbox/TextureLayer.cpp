//
// 显示 5 个棋盘纹理四边形的 TextureLayer —— 演示动态 UBO、纹理、描述符集
//

#include "TextureLayer.h"
#include "GE/Core/Application.h"

#include "GE/Render/VulkanBase/VulkanRenderingInfo.h"
#include "GE/Render/VulkanBase/VulkanResourceCache.h"

#include "imgui.h"

#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>

namespace GE {

namespace {
constexpr size_t kInstanceCount = 5;
}

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

    // ── 1.5 将 UBO 标记为动态描述符（使用 dynamic uniform buffer） ───────
    m_VertShader->set_resource_mode("UniformBlock", ShaderResourceMode::Dynamic);

    // ── 2. 通过全局资源缓存创建 PipelineLayout ────────────────────────────
    m_PipelineLayout = &cache.RequestPipelineLayout(
        {m_VertShader, m_FragShader});

    // ── 3. 加载棋盘纹理 ────────────────────────────────────────────────────
    m_Texture = Texture::LoadFromFile(device, cache,
                                      "assets/textures/Checkerboard.png",
                                      vk::Format::eR8G8B8A8Srgb,
                                      vk::Filter::eNearest,
                                      vk::Filter::eNearest);

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

}

void TextureLayer::OnDetach() {
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

    // ── 从当前帧的 buffer pool 分配 uniform buffer（5 个实例） ───────────
    struct UniformBlock {
        glm::mat4 model;
        glm::mat4 view;
        glm::mat4 projection;
        glm::vec4 color;
    };
    static_assert(sizeof(UniformBlock) % 16 == 0, "UBO 大小必须 16 字节对齐");

    static float rotation = 0.0f;
    rotation += ts.GetSeconds() * 0.5f;

    // 5 个实例的颜色和位置
    glm::vec3 instanceColors[kInstanceCount] = {
        {1.0f, 1.0f, 1.0f},   // 白
        {1.0f, 0.4f, 0.4f},   // 红
        {0.4f, 1.0f, 0.4f},   // 绿
        {0.4f, 0.6f, 1.0f},   // 蓝
        {1.0f, 0.9f, 0.3f},   // 黄
    };
    float instanceAngles[kInstanceCount] = {
        0.0f,
        glm::radians(72.0f),
        glm::radians(144.0f),
        glm::radians(216.0f),
        glm::radians(288.0f),
    };

    UniformBlock baseUbo{};
    baseUbo.view = glm::lookAt(glm::vec3(0.0f, 0.0f, 5.0f),
                               glm::vec3(0.0f, 0.0f, 0.0f),
                               glm::vec3(0.0f, 1.0f, 0.0f));
    baseUbo.projection = glm::perspectiveZO(glm::radians(45.0f),
                                            static_cast<float>(extent.width) /
                                            static_cast<float>(extent.height),
                                            0.1f, 100.0f);
    baseUbo.projection[1][1] *= -1.0f;

    auto &frame = Application::GetRenderContext().GetActiveFrame();

    // 为每个实例分配一个 UBO（动态 uniform buffer 用 offset 区分）
    BufferAllocation uboAllocs[kInstanceCount];
    for (size_t i = 0; i < kInstanceCount; ++i) {
        UniformBlock ubo = baseUbo;
        float angle = rotation + instanceAngles[i];
        glm::vec3 pos(std::cos(angle) * 1.5f, std::sin(angle) * 1.5f, 0.0f);
        ubo.model = glm::translate(glm::mat4(1.0f), pos)
                  * glm::rotate(glm::mat4(1.0f), rotation * 2.0f, glm::vec3(0.0f, 0.0f, 1.0f))
                  * glm::scale(glm::mat4(1.0f), glm::vec3(0.6f));
        ubo.color = glm::vec4(instanceColors[i], 1.0f);

        uboAllocs[i] = frame.AllocateBuffer(
            vk::BufferUsageFlagBits::eUniformBuffer, sizeof(UniformBlock));
        uboAllocs[i].update(ubo);
    }

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

    // 顶点输入：从顶点着色器反射自动生成
    ps.SetVertexInputFromShader(*m_VertShader);

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

    // ── 绑定顶点/索引 buffer ───────────────────────────────────────────────
    cmd.BindVertexBuffers(0, {std::ref(*m_VertexBuffer)}, {0});
    cmd.BindIndexBuffer(*m_IndexBuffer, 0, vk::IndexType::eUint32);

    // ── 绑定纹理（所有实例共享同一个 descriptor set 的 image 部分） ───────
    if (m_Texture) {
        cmd.BindImage(m_Texture->GetImageView(), m_Texture->GetSampler(), 0, 1);
    }

    // ── 循环绘制 5 个实例（每次更新 dynamic uniform buffer 的 offset） ────
    for (size_t i = 0; i < kInstanceCount; ++i) {
        // 动态 UBO：只改 offset，descriptor set 本身不变（缓存命中）
        cmd.BindBuffer(uboAllocs[i].get_buffer(), uboAllocs[i].get_offset(),
                       uboAllocs[i].get_size(), 0, 0);

        cmd.DrawIndexed(6, 1, 0, 0, 0);
    }

    // ── 结束渲染 ──────────────────────────────────────────────────────────
    VulkanRenderingInfo::End(vkCmd);
}

void TextureLayer::OnEvent(Event &event) {
}

void TextureLayer::OnImGuiRender() {
    ImGui::Begin("TextureLayer");
    ImGui::Text("显示 5 个带棋盘纹理的旋转四边形");
    ImGui::Separator();
    ImGui::Text("着色器：triangle.vert / triangle.frag");
    ImGui::Text("纹理：Checkerboard.png");
    ImGui::Text("管线：动态 UBO + 纹理采样器");
    ImGui::Text("实例数：%zu", kInstanceCount);
    if (m_Texture) {
        ImGui::Text("纹理尺寸：%d x %d",
                    m_Texture->GetExtent().width,
                    m_Texture->GetExtent().height);
    }
    ImGui::End();
}

} // namespace GE