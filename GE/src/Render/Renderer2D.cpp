//
// Created by Lenovo on 2026/6/5.
//

#include "Render/Renderer2D.h"
#include "Render/VulkanBase/VulkanRenderingInfo.h"
#include "Core/GEWindow.h"

#include <array>
#include <cstring>

namespace GE {

Renderer2D &Renderer2D::Get() {
    static Renderer2D instance;
    return instance;
}

void Renderer2D::Init(Window &window) {
    m_Instance.Init("GE App");
    m_Device.Init(m_Instance, window);

    m_VkDevice = m_Device.GetDevice();

    auto width = window.GetWidth();
    auto height = window.GetHeight();
    m_Swapchain.Init(m_VkDevice, m_Device.GetGpu(), m_Device.GetSurface(),
                     m_Device.GetQueue(), m_Device.GetGraphicsQueueIndex(), width, height);

    m_UniformBuffer.Init(m_Device.GetVmaAllocator(), sizeof(UniformData),
                         vk::BufferUsageFlagBits::eUniformBuffer);
}

void Renderer2D::Shutdown() {
    m_UniformBuffer.Destroy();
    m_VkDevice = nullptr;

    m_Swapchain.Destroy();
    m_Device.Destroy();
    m_Instance.Destroy();
}

VulkanPipeline Renderer2D::CreateDefaultPipeline(vk::Device dev, vk::Format color_format) {
    std::array<vk::DescriptorSetLayoutBinding, 2> bindings{{
        {.binding = 0,
         .descriptorType = vk::DescriptorType::eUniformBuffer,
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

    VertexInputState vertex_input;
    vertex_input.stride = sizeof(MeshVertex);
    vertex_input.attributes = {
        {.location = 0, .binding = 0,
         .format = vk::Format::eR32G32B32Sfloat,
         .offset = offsetof(MeshVertex, position)},
        {.location = 1, .binding = 0,
         .format = vk::Format::eR32G32Sfloat,
         .offset = offsetof(MeshVertex, uv)},
        {.location = 2, .binding = 0,
         .format = vk::Format::eR32G32B32Sfloat,
         .offset = offsetof(MeshVertex, normal)},
    };

    pipeline.Init(dev, color_format, vertex_input,
                  "mesh.vert.spv", "mesh.frag.spv", "assets/shaders/glsl");
    return pipeline;
}

void Renderer2D::BeginScene(const glm::mat4 &view, const glm::mat4 &projection,
                            const glm::vec3 &view_pos,
                            const glm::vec4 &clear_color) {
    auto cmd = m_Swapchain.GetCurrentCmd();
    auto dim = m_Swapchain.GetDimensions();
    uint32_t image_index = m_Swapchain.GetCurrentImageIndex();

    m_ActiveCmd = cmd;
    m_View = view;
    m_Projection = projection;
    m_ViewPos = view_pos;

    // Begin render pass
    vk::ClearValue clear_value;
    clear_value.color = std::array<float, 4>{{clear_color.r, clear_color.g, clear_color.b, clear_color.a}};

    VulkanRenderingInfo render_info;
    render_info.SetRenderArea(0, 0, dim.width, dim.height);
    render_info.AddColorAttachment(m_Swapchain.GetImageView(image_index),
                                   vk::AttachmentLoadOp::eClear,
                                   vk::AttachmentStoreOp::eStore,
                                   clear_value);
    render_info.Begin(cmd);

    // Dynamic state (shared across all draws in this frame)
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
    UniformData data{};
    data.projection = m_Projection;
    data.view = m_View;
    data.model = model;
    data.viewPos = glm::vec4(m_ViewPos, 1.0f);
    data.lodBias = 0.0f;
    m_UniformBuffer.Upload(&data, sizeof(data));

    vk::CommandBuffer cmd = m_ActiveCmd;

    // Bind per-material pipeline + descriptor set
    material.pipeline.Bind(cmd);

    cmd.setCullMode(vk::CullModeFlagBits::eNone);
    cmd.setFrontFace(vk::FrontFace::eClockwise);
    cmd.setPrimitiveTopology(vk::PrimitiveTopology::eTriangleList);

    vk::Buffer vb = mesh.vertices.GetBuffer();
    cmd.bindVertexBuffers(0, vb, {0});
    cmd.bindIndexBuffer(mesh.indices.GetBuffer(), 0, vk::IndexType::eUint16);

    cmd.bindDescriptorSets(vk::PipelineBindPoint::eGraphics, material.pipeline.GetLayout(),
                           0, material.descriptorSet.Get(), nullptr);

    cmd.drawIndexed(mesh.indexCount, 1, 0, 0, 0);
}

void Renderer2D::EndScene() {
    VulkanRenderingInfo::End(m_ActiveCmd);
    m_ActiveCmd = vk::CommandBuffer{nullptr};
}

} // namespace GE
