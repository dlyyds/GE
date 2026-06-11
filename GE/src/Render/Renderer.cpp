//
// Created by Lenovo on 2026/6/5.
//

#include "Render/Renderer.h"
#include "Render/VulkanBase/VulkanContext.h"
#include "Render/VulkanBase/VulkanRenderingInfo.h"
#include "Core/GEWindow.h"

#include <array>
#include <cstring>

namespace GE {

Renderer &Renderer::Get() {
    static Renderer instance;
    return instance;
}

Renderer::~Renderer() = default;

void Renderer::Init(VulkanContext &ctx, VulkanSwapchain &swapchain) {
    m_Context = &ctx;
    m_Swapchain = &swapchain;

    auto vkDevice = ctx.GetVkDevice();
    auto vmaAllocator = ctx.GetVmaAllocator();

    // 查询硬件要求的 UBO 对齐值，空值回退 64
    auto props = ctx.GetVkGpu().getProperties();
    vk::DeviceSize uboAlignment = props.limits.minUniformBufferOffsetAlignment;
    if (uboAlignment == 0) uboAlignment = 64;

    // set=0: 创建 per-frame UBO buffer（静态大小，每帧 memcpy 覆盖）
    m_FrameBuffer.Init(vmaAllocator, sizeof(FrameUniformData),
                       vk::BufferUsageFlagBits::eUniformBuffer);

    // set=2: 创建 per-object ring buffer
    m_RingBuffer.Init(vmaAllocator, RING_BUFFER_SIZE, uboAlignment);
}

void Renderer::InitDescriptorSets(vk::Device device,
                                   vk::DescriptorSetLayout frameLayout,
                                   vk::DescriptorSetLayout objectLayout) {
    // 共用 pool：frame（1 个 eUniformBuffer）+ object（1 个 eUniformBufferDynamic）
    m_GlobalPool.Init(device, 2, {
        {vk::DescriptorType::eUniformBuffer, 1},
        {vk::DescriptorType::eUniformBufferDynamic, 1},
    });

    // set=0: frame descriptor set
    m_FrameSet.Init(device, m_GlobalPool, frameLayout);
    m_FrameSet.WriteBuffer(0, vk::DescriptorType::eUniformBuffer,
        vk::DescriptorBufferInfo{
            .buffer = m_FrameBuffer.GetBuffer(),
            .offset = 0,
            .range = sizeof(FrameUniformData),
        });

    // set=2: object descriptor set（指向 ring buffer，dynamic offset 在 Bind 时传入）
    m_ObjectSet.Init(device, m_GlobalPool, objectLayout);
    m_ObjectSet.WriteBuffer(0, vk::DescriptorType::eUniformBufferDynamic,
        vk::DescriptorBufferInfo{
            .buffer = m_RingBuffer.GetBuffer(),
            .offset = 0,
            .range = sizeof(ObjectUniformData),
        });
}

void Renderer::Shutdown() {
    m_ObjectSet.Destroy();
    m_FrameSet.Destroy();
    m_GlobalPool.Cleanup();
    m_FrameBuffer.Destroy();
    m_RingBuffer.Destroy();

    m_Context = nullptr;
    m_Swapchain = nullptr;
}

VulkanPipeline Renderer::CreateDefaultPipeline(vk::Device dev, vk::Format color_format) {
    // 创建着色器（ShaderModule 在管线创建后可安全销毁）
    VulkanShader vertShader, fragShader;
    vertShader.Init(dev, "assets/shaders/glsl/mesh.vert.spv", vk::ShaderStageFlagBits::eVertex);
    fragShader.Init(dev, "assets/shaders/glsl/mesh.frag.spv", vk::ShaderStageFlagBits::eFragment);

    // descriptor layout 在 VulkanPipeline::Init 内部通过反射自动创建，
    // 同时从 vertex shader 反射获取 input layout
    VulkanPipeline pipeline;
    pipeline.Init(dev, color_format, vertShader, fragShader, /*dynamicBindings=*/{{2, 0}});
    return pipeline;
}

void Renderer::BeginScene(vk::CommandBuffer cmd, uint32_t imageIndex,
                          vk::Extent2D dim,
                          const glm::mat4 &view, const glm::mat4 &projection,
                          const glm::vec3 &view_pos,
                          const glm::vec4 &clear_color) {
    // 重置 ring buffer（该帧内所有 Draw 共享同一段内存区域）
    m_RingBuffer.Reset();

    m_ActiveCmd = cmd;
    m_CurrentImageIndex = imageIndex;
    m_ActiveDim = dim;
    m_View = view;
    m_Projection = projection;
    m_ViewPos = view_pos;

    // 写入 set=0 的 frame UBO 数据（每帧覆盖，无 dynamic offset）
    FrameUniformData frameData{};
    frameData.projection = projection;
    frameData.view = view;
    frameData.viewPos = glm::vec4(view_pos, 1.0f);
    m_FrameBuffer.Upload(&frameData, sizeof(FrameUniformData));

    // 开始渲染 pass
    vk::ClearValue clear_value;
    clear_value.color = std::array<float, 4>{{clear_color.r, clear_color.g, clear_color.b, clear_color.a}};

    VulkanRenderingInfo render_info;
    render_info.SetRenderArea(0, 0, dim.width, dim.height);
    render_info.AddColorAttachment(m_Swapchain->GetImageView(imageIndex),
                                   vk::AttachmentLoadOp::eClear,
                                   vk::AttachmentStoreOp::eStore,
                                   clear_value);
    render_info.Begin(cmd);

    // 动态状态（本帧内所有 Draw 共享）
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
}

void Renderer::Draw(const Mesh &mesh, const Material &material, const glm::mat4 &model, const glm::vec4 &color) {
    // 将本次 Draw 的 ObjectUBO 数据写入 ring buffer
    ObjectUniformData objData{};
    objData.model = model;
    objData.lodBias = 0.0f;

    vk::DeviceSize offset = m_RingBuffer.Allocate(sizeof(ObjectUniformData));
    std::memcpy(static_cast<char *>(m_RingBuffer.GetMappedData()) + offset,
                &objData, sizeof(objData));

    vk::CommandBuffer cmd = m_ActiveCmd;

    // 绑定材质的管线
    material.pipeline.Bind(cmd);

    cmd.setCullMode(vk::CullModeFlagBits::eNone);
    cmd.setFrontFace(vk::FrontFace::eClockwise);
    cmd.setPrimitiveTopology(vk::PrimitiveTopology::eTriangleList);

    // 绑定顶点/索引 buffer
    vk::Buffer vb = mesh.vertices.GetBuffer();
    cmd.bindVertexBuffers(0, vb, {0});
    cmd.bindIndexBuffer(mesh.indices.GetBuffer(), 0, vk::IndexType::eUint16);

    // 绑定所有 3 个 descriptor set：
    //   set=0: FrameUBO（per-frame，静态）
    //   set=1: Material 纹理
    //   set=2: ObjectUBO（per-draw，dynamic offset）
    vk::DescriptorSet sets[] = {
        m_FrameSet.Get(),
        material.descriptorSet.Get(),
        m_ObjectSet.Get(),
    };
    uint32_t dynamicOffset = static_cast<uint32_t>(offset);
    cmd.bindDescriptorSets(vk::PipelineBindPoint::eGraphics, material.pipeline.GetLayout(),
                           0, 3, sets, 1, &dynamicOffset);

    cmd.drawIndexed(mesh.indexCount, 1, 0, 0, 0);
}

void Renderer::EndScene() {
    VulkanRenderingInfo::End(m_ActiveCmd);
    m_ActiveCmd = vk::CommandBuffer{nullptr};
}

} // namespace GE
