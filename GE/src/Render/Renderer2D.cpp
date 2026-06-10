//
// Created by Lenovo on 2026/6/5.
//

#include "Render/Renderer2D.h"
#include "Render/VulkanBase/VulkanContext.h"
#include "Render/VulkanBase/VulkanRenderingInfo.h"
#include "Core/GEWindow.h"

#include <array>
#include <cstring>

namespace GE {

Renderer2D &Renderer2D::Get() {
    static Renderer2D instance;
    return instance;
}

void Renderer2D::Init(VulkanContext &ctx, VulkanSwapchain &swapchain) {
    m_Context = &ctx;
    m_Swapchain = &swapchain;

    auto vkDevice = ctx.GetVkDevice();
    auto vmaAllocator = ctx.GetVmaAllocator();

    // 为每个 swapchain image 创建 ring buffer（三重缓冲）
    uint32_t imageCount = swapchain.GetImageCount();
    m_RingBuffers.reserve(imageCount);
    for (uint32_t i = 0; i < imageCount; i++) {
        m_RingBuffers.emplace_back();
        m_RingBuffers.back().Init(vmaAllocator, RING_BUFFER_SIZE);
    }
}

void Renderer2D::Shutdown() {
    for (auto &rb : m_RingBuffers)
        rb.Destroy();
    m_RingBuffers.clear();

    m_Context = nullptr;
    m_Swapchain = nullptr;
}

VulkanPipeline Renderer2D::CreateDefaultPipeline(vk::Device dev, vk::Format color_format) {
    std::array<vk::DescriptorSetLayoutBinding, 2> bindings{{
        {.binding = 0,
         .descriptorType = vk::DescriptorType::eUniformBufferDynamic,
         .descriptorCount = 1,
         .stageFlags = vk::ShaderStageFlagBits::eVertex | vk::ShaderStageFlagBits::eFragment},
        {.binding = 1,
         .descriptorType = vk::DescriptorType::eCombinedImageSampler,
         .descriptorCount = 1,
         .stageFlags = vk::ShaderStageFlagBits::eFragment},
    }};

    vk::DescriptorSetLayoutCreateInfo layout_info{
        .bindingCount = static_cast<uint32_t>(bindings.size()),
        .pBindings = bindings.data()
    };

    vk::DescriptorSetLayout layout = dev.createDescriptorSetLayout(layout_info);

    VulkanPipeline pipeline;
    pipeline.SetDescriptorSetLayout(layout);

    // 创建着色器（ShaderModule 在管线创建后可安全销毁）
    VulkanShader vertShader, fragShader;
    vertShader.Init(dev, "assets/shaders/glsl/mesh.vert.spv", vk::ShaderStageFlagBits::eVertex);
    fragShader.Init(dev, "assets/shaders/glsl/mesh.frag.spv", vk::ShaderStageFlagBits::eFragment);

    pipeline.Init(dev, color_format, vertShader, fragShader);
    return pipeline;
}

void Renderer2D::BeginScene(vk::CommandBuffer cmd, uint32_t imageIndex,
                            vk::Extent2D dim,
                            const glm::mat4 &view, const glm::mat4 &projection,
                            const glm::vec3 &view_pos,
                            const glm::vec4 &clear_color) {
    // 重置当前帧的 ring buffer
    m_RingBuffers[imageIndex].Reset();

    m_ActiveCmd = cmd;
    m_CurrentImageIndex = imageIndex;
    m_ActiveDim = dim;
    m_View = view;
    m_Projection = projection;
    m_ViewPos = view_pos;

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

void Renderer2D::Draw(const Mesh &mesh, const Material &material, const glm::mat4 &model, const glm::vec4 &color) {
    uint32_t image_index = m_CurrentImageIndex;
    VulkanRingBuffer &ringBuffer = m_RingBuffers[image_index];

    // 将本次 Draw 的 UBO 数据写入 ring buffer
    UniformData data{};
    data.projection = m_Projection;
    data.view = m_View;
    data.model = model;
    data.viewPos = glm::vec4(m_ViewPos, 1.0f);
    data.lodBias = 0.0f;

    vk::DeviceSize offset = ringBuffer.Allocate(sizeof(UniformData));
    std::memcpy(static_cast<char *>(ringBuffer.GetMappedData()) + offset,
                &data, sizeof(data));

    vk::CommandBuffer cmd = m_ActiveCmd;

    // 绑定材质的管线
    material.pipeline.Bind(cmd);

    cmd.setCullMode(vk::CullModeFlagBits::eNone);
    cmd.setFrontFace(vk::FrontFace::eClockwise);
    cmd.setPrimitiveTopology(vk::PrimitiveTopology::eTriangleList);

    vk::Buffer vb = mesh.vertices.GetBuffer();
    cmd.bindVertexBuffers(0, vb, {0});
    cmd.bindIndexBuffer(mesh.indices.GetBuffer(), 0, vk::IndexType::eUint16);

    // 绑定当前 image 对应的 descriptor set，用动态偏移指定 UBO 数据
    uint32_t dynamicOffset = static_cast<uint32_t>(offset);
    cmd.bindDescriptorSets(vk::PipelineBindPoint::eGraphics, material.pipeline.GetLayout(),
                           0, material.descriptorSets[image_index].Get(), dynamicOffset);

    cmd.drawIndexed(mesh.indexCount, 1, 0, 0, 0);
}

void Renderer2D::EndScene() {
    VulkanRenderingInfo::End(m_ActiveCmd);
    m_ActiveCmd = vk::CommandBuffer{nullptr};
}

} // namespace GE
