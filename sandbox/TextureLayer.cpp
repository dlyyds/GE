//
// 显示棋盘纹理的 TextureLayer —— 演示纹理加载、采样器、描述符集
//

#include "TextureLayer.h"
#include "GE/Core/Application.h"

#include "GE/Render/VulkanBase/VulkanRenderingInfo.h"
#include "GE/Render/VulkanBase/VulkanResourceCache.h"
#include "GE/Render/VulkanBase/VulkanRenderFrame.h"

#include "stb_image.h"
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
    auto allocator = ctx.GetVmaAllocator();
    auto &swapchain = Application::GetSwapchain();
    auto colorFmt = swapchain.GetFormat();

    // 获取 graphics queue（用于上传 texture 后的 flush）
    auto &graphicsQueue = device.GetQueueByFlags(vk::QueueFlagBits::eGraphics, 0);
    vk::Queue gfxQueue = graphicsQueue.GetHandle();

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

    // ── 6. 加载棋盘纹理 ───────────────────────────────────────────────────
    const char *texturePath = "assets/textures/Checkerboard.png";
    int texWidth = 0, texHeight = 0, texChannels = 0;
    bool useFallback = false;

    // 2x2 棋盘格 fallback
    static unsigned char fallbackPixels[] = {
        255, 255, 255, 255, 0, 0, 0, 255,
        0, 0, 0, 255, 255, 255, 255, 255,
    };

    // 使用 stb_image 加载 PNG（强制 RGBA 4 通道）
    unsigned char *pixels = stbi_load(texturePath, &texWidth, &texHeight, &texChannels, STBI_rgb_alpha);
    if (!pixels) {
        GE_CORE_ERROR("无法加载纹理：{}", texturePath);
        texWidth = 2;
        texHeight = 2;
        pixels = fallbackPixels;
        useFallback = true;
    }

    vk::DeviceSize imageSize = static_cast<vk::DeviceSize>(texWidth * texHeight * 4);

    // 创建 staging buffer 上传纹理数据（create_staging_buffer 内部已拷贝数据）
    auto stagingBuffer = VulkanBuffer::create_staging_buffer(device, imageSize, pixels);

    // 释放 stb_image 加载的内存（fallback 是静态数组，不需要释放）
    if (!useFallback) {
        stbi_image_free(pixels);
    }

    // 创建目标纹理图像（GPU 本地）
    m_TextureImage = std::make_unique<VulkanImage>(
        device,
        vk::Extent3D{static_cast<uint32_t>(texWidth), static_cast<uint32_t>(texHeight), 1},
        vk::Format::eR8G8B8A8Unorm,
        vk::ImageUsageFlagBits::eTransferDst | vk::ImageUsageFlagBits::eSampled);

    // 使用临时 command buffer 上传纹理
    auto uploadCmd = device.RequestCommandBuffer(vk::CommandBufferLevel::ePrimary, true);

    // 将图像布局从 undefined 转换为 transfer-dst
    image_utils::TransitionLayout(uploadCmd->GetHandle(), m_TextureImage->GetHandle(),
                                  vk::ImageLayout::eUndefined,
                                  vk::ImageLayout::eTransferDstOptimal);

    // 拷贝 staging buffer 到纹理图像
    vk::BufferImageCopy copyRegion{};
    copyRegion.bufferOffset = 0;
    copyRegion.bufferRowLength = 0;
    copyRegion.bufferImageHeight = 0;
    copyRegion.imageSubresource.aspectMask = vk::ImageAspectFlagBits::eColor;
    copyRegion.imageSubresource.mipLevel = 0;
    copyRegion.imageSubresource.baseArrayLayer = 0;
    copyRegion.imageSubresource.layerCount = 1;
    copyRegion.imageOffset = vk::Offset3D{0, 0, 0};
    copyRegion.imageExtent = vk::Extent3D{static_cast<uint32_t>(texWidth), static_cast<uint32_t>(texHeight), 1};
    uploadCmd->GetHandle().copyBufferToImage(stagingBuffer.GetHandle(), m_TextureImage->GetHandle(),
                                             vk::ImageLayout::eTransferDstOptimal, copyRegion);

    // 将图像布局转换为 shader-read-only
    image_utils::TransitionLayout(uploadCmd->GetHandle(), m_TextureImage->GetHandle(),
                                  vk::ImageLayout::eTransferDstOptimal,
                                  vk::ImageLayout::eShaderReadOnlyOptimal);

    // 提交并等待完成（shared_ptr 超出作用域后自动归还到 pool）
    device.FlushCommandBuffer(uploadCmd, gfxQueue);

    // staging buffer 在 upload 完成后自动析构

    // ── 7. 创建纹理 ImageView ─────────────────────────────────────────────
    m_TextureView = std::make_unique<VulkanImageView>(
        *m_TextureImage,
        vk::ImageViewType::e2D,
        vk::Format::eR8G8B8A8Unorm);

    // ── 8. 通过缓存获取 Sampler ───────────────────────────────────────────
    m_TextureSampler = &cache.RequestSampler(
        vk::Filter::eLinear, // mag
        vk::Filter::eLinear, // min
        vk::SamplerMipmapMode::eLinear, // mipmap
        vk::SamplerAddressMode::eRepeat, // address U
        vk::SamplerAddressMode::eRepeat, // address V
        vk::SamplerAddressMode::eRepeat); // address W

    // ── 9. 创建顶点 buffer（全屏四边形：位置 + UV）────────────────────────
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

    // ── 10. 创建索引 buffer（2 个三角形）──────────────────────────────────
    uint32_t indices[] = {
        0, 1, 2,
        2, 3, 0,
    };

    m_IndexBuffer = std::make_unique<VulkanBuffer>(
        device, sizeof(indices),
        vk::BufferUsageFlagBits::eIndexBuffer);
    m_IndexBuffer->update(indices, sizeof(indices));

    // ── 11. 创建 uniform buffer（MVP 矩阵）───────────────────────────────
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
    m_TextureView.reset();
    m_TextureImage.reset();

    // 由 VulkanResourceCache 管理的资源
    m_TextureSampler = nullptr;
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

    // binding 1: texture sampler
    vk::DescriptorImageInfo imageInfo{};
    imageInfo.sampler = m_TextureSampler->GetHandle();
    imageInfo.imageView = m_TextureView->GetHandle();
    imageInfo.imageLayout = vk::ImageLayout::eShaderReadOnlyOptimal;

    BindingMap<vk::DescriptorImageInfo> imageInfos;
    imageInfos[1][0] = imageInfo;

    // 从当前帧的 RenderFrame 获取 descriptor set
    vk::DescriptorSet descriptorSet = renderFrame.RequestDescriptorSet(
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
    m_Pipeline->Bind(vkCmd);

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
                             0, descriptorSet, {});

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
    if (m_TextureImage) {
        ImGui::Text("纹理尺寸：%d x %d",
                    m_TextureImage->get_extent().width,
                    m_TextureImage->get_extent().height);
    }
    ImGui::End();
}

} // namespace GE