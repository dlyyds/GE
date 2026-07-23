//
// 显示棋盘纹理的 TextureLayer —— 演示纹理加载、采样器、描述符集
//

#include "TextureLayer.h"
#include "GE/Core/Application.h"

#include "GE/Render/VulkanBase/VulkanRenderingInfo.h"
#include "GE/Render/VulkanBase/VulkanResourceCache.h"
#include "GE/Render/VulkanBase/VulkanRenderFrame.h"

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
    auto &swapchain = Application::GetSwapchain();
    auto colorFmt = swapchain.GetFormat();

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

    // ── 3. 通过全局资源缓存创建 DescriptorSetLayout ───────────────────────
    // 从着色器反射信息中提取 set=0 的资源
    auto &shaderSets = m_PipelineLayout->GetShaderSets();
    auto setIt = shaderSets.find(0);
    const std::vector<ShaderResource> &set0Resources =
        (setIt != shaderSets.end()) ? setIt->second : std::vector<ShaderResource>{};
    m_DescriptorSetLayout = &cache.RequestDescriptorSetLayout(
        0, {m_VertShader, m_FragShader}, set0Resources);

    // ── 4. 配置 PipelineState ─────────────────────────────────────────────
    m_PipelineState.Reset();
    m_PipelineState.pipelineLayout = m_PipelineLayout;
    m_PipelineState.colorAttachmentFormats = {colorFmt};
    m_PipelineState.depthFormat = {};
    m_PipelineState.stencilFormat = {};

    // 顶点输入：位置 vec2 (offset 0) + UV vec2 (offset 8), stride = 16
    m_PipelineState.vertexBindingDescriptions = std::vector<vk::VertexInputBindingDescription>{
        {0, 16, vk::VertexInputRate::eVertex},
    };
    m_PipelineState.vertexAttributeDescriptions = std::vector<vk::VertexInputAttributeDescription>{
        {0, 0, vk::Format::eR32G32Sfloat, 0}, // position
        {1, 0, vk::Format::eR32G32Sfloat, static_cast<uint32_t>(2 * sizeof(float))}, // uv
    };

    // 混合附件
    m_PipelineState.SetBlendAttachments({GE::BlendAttachment{}});

    // 动态状态
    m_PipelineState.cullMode.SetDynamic(true);
    m_PipelineState.frontFace.SetDynamic(true);
    m_PipelineState.topology.SetDynamic(true);
    m_PipelineState.depthTestEnable = VK_FALSE;
    m_PipelineState.depthWriteEnable = VK_FALSE;

    // ── 5. 通过全局资源缓存创建图形管线 ──────────────────────────────────
    m_Pipeline = &cache.RequestGraphicsPipeline(m_PipelineState);

    // ── 6. 加载棋盘纹理───────────────────────────────────────
    m_Texture = Texture::LoadFromFile(device, cache, "assets/textures/Checkerboard.png");

    // ── 7. 创建顶点 buffer（全屏四边形：位置 + UV）────────────────────────
    struct Vertex {
        float x, y; // position (location 0)
        float u, v; // uv       (location 1)
    };

    // 覆盖 NDC 的四边形，UV 从 0 到 1
    Vertex vertices[] = {
        {-1.0f, -1.0f, 0.0f, 0.0f}, // 左下
        {1.0f, -1.0f, 1.0f, 0.0f}, // 右下
        {1.0f, 1.0f, 1.0f, 1.0f}, // 右上
        {-1.0f, 1.0f, 0.0f, 1.0f}, // 左上
    };

    m_VertexBuffer = std::make_unique<VulkanBuffer>(
        device, sizeof(vertices),
        vk::BufferUsageFlagBits::eVertexBuffer);
    m_VertexBuffer->update(vertices, sizeof(vertices));

    // ── 8. 创建索引 buffer（2 个三角形）───────────────────────────────────
    uint32_t indices[] = {
        0, 1, 2,
        2, 3, 0,
    };

    m_IndexBuffer = std::make_unique<VulkanBuffer>(
        device, sizeof(indices),
        vk::BufferUsageFlagBits::eIndexBuffer);
    m_IndexBuffer->update(indices, sizeof(indices));

    // ── 9. 创建 uniform buffer（MVP 矩阵）────────────────────────────────
    m_UniformBuffer = std::make_unique<VulkanBuffer>(
        device, sizeof(glm::mat4) * 3 + sizeof(glm::vec4),
        vk::BufferUsageFlagBits::eUniformBuffer,
        VMA_MEMORY_USAGE_AUTO,
        VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT);

    // 初始化 MVP 矩阵
    struct UniformBlock {
        glm::mat4 model;
        glm::mat4 view;
        glm::mat4 projection;
        glm::vec4 color;
    };

    UniformBlock ubo{};
    ubo.model = glm::mat4(1.0f);
    ubo.view = glm::lookAt(glm::vec3(0.0f, 0.0f, 2.0f),
                           glm::vec3(0.0f, 0.0f, 0.0f),
                           glm::vec3(0.0f, 1.0f, 0.0f));
    ubo.projection = glm::perspective(glm::radians(45.0f),
                                      static_cast<float>(swapchain.GetExtent().width) /
                                      static_cast<float>(swapchain.GetExtent().height),
                                      0.1f, 100.0f);
    ubo.projection[1][1] *= -1.0f; // Vulkan NDC: Y 轴向下
    ubo.color = glm::vec4(1.0f); // 不参与混合，纹理颜色全显示

    m_UniformBuffer->update(&ubo, sizeof(ubo));
}

void TextureLayer::OnDetach() {
    // 手动持有的资源
    m_UniformBuffer.reset();
    m_IndexBuffer.reset();
    m_VertexBuffer.reset();
    m_Texture.reset();

    // 由 VulkanResourceCache 管理的资源
    m_DescriptorSetLayout = nullptr;
    m_Pipeline = nullptr;
    m_PipelineLayout = nullptr;
    m_FragShader = nullptr;
    m_VertShader = nullptr;
}

void TextureLayer::OnUpdate(Timestep &ts) {
    auto &cmd = Application::GetFrameCmd();
    auto vkCmd = cmd.GetHandle();
    auto extent = Application::GetSwapchain().GetExtent();

    // 获取当前帧的 RenderFrame，用于分配 descriptor set
    auto &renderFrame = Application::GetRenderContext().GetActiveFrame();

    // ── 更新 uniform buffer（可选：每帧旋转）─────────────────────────────
    struct UniformBlock {
        glm::mat4 model;
        glm::mat4 view;
        glm::mat4 projection;
        glm::vec4 color;
    };

    // 简单旋转动画
    static float rotation = 0.0f;
    rotation += ts.GetSeconds() * 0.5f;

    UniformBlock ubo{};
    ubo.model = glm::rotate(glm::mat4(1.0f), rotation, glm::vec3(0.0f, 0.0f, 1.0f));
    ubo.view = glm::lookAt(glm::vec3(0.0f, 0.0f, 2.0f),
                           glm::vec3(0.0f, 0.0f, 0.0f),
                           glm::vec3(0.0f, 1.0f, 0.0f));
    ubo.projection = glm::perspective(glm::radians(45.0f),
                                      static_cast<float>(extent.width) /
                                      static_cast<float>(extent.height),
                                      0.1f, 100.0f);
    ubo.projection[1][1] *= -1.0f; // Vulkan NDC
    ubo.color = glm::vec4(1.0f);

    m_UniformBuffer->update(&ubo, sizeof(ubo));

    // ── 构建 descriptor set 信息 ──────────────────────────────────────────
    // binding 0: uniform buffer
    vk::DescriptorBufferInfo bufferInfo{};
    bufferInfo.buffer = m_UniformBuffer->GetHandle();
    bufferInfo.offset = 0;
    bufferInfo.range = sizeof(UniformBlock);

    BindingMap<vk::DescriptorBufferInfo> bufferInfos;
    bufferInfos[0][0] = bufferInfo;

    // binding 1: texture sampler（使用 Texture 封装获取描述符信息）
    BindingMap<vk::DescriptorImageInfo> imageInfos;
    if (m_Texture) {
        imageInfos[1][0] = m_Texture->GetDescriptorInfo();
    }

    // 从当前帧的 RenderFrame 获取 descriptor set
    auto &descriptorSet = renderFrame.RequestDescriptorSet(
        *m_DescriptorSetLayout, bufferInfos, imageInfos);

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

    // ── 绑定管线 ──────────────────────────────────────────────────────────
    vkCmd.bindPipeline(vk::PipelineBindPoint::eGraphics, m_Pipeline->GetHandle());

    // ── 动态状态 ──────────────────────────────────────────────────────────
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

    // ── 绑定 descriptor set ───────────────────────────────────────────────
    vkCmd.bindDescriptorSets(vk::PipelineBindPoint::eGraphics,
                             m_PipelineLayout->GetHandle(),
                             0, descriptorSet.GetHandle(), {});

    // ── 绑定顶点和索引 buffer ─────────────────────────────────────────────
    vk::Buffer vb = m_VertexBuffer->GetHandle();
    vkCmd.bindVertexBuffers(0, vb, {0});
    vkCmd.bindIndexBuffer(m_IndexBuffer->GetHandle(), 0, vk::IndexType::eUint32);

    // ── 绘制 6 个顶点（2 个三角形）────────────────────────────────────────
    vkCmd.drawIndexed(6, 1, 0, 0, 0);

    // ── 结束渲染 ──────────────────────────────────────────────────────────
    VulkanRenderingInfo::End(vkCmd);
}

void TextureLayer::OnEvent(Event &event) {
    // 本层不需要处理事件
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