//
// Created by Lenovo on 2026/6/2.
//

#include "VulkanLayer.h"

#include <cassert>

namespace GE {

VulkanLayer::VulkanLayer() : Layer("VulkanLayer") {
}

void VulkanLayer::OnAttach() {
    auto &render_system = Application::Get().GetRenderSystem();
    m_Context = &render_system.GetContext();
    m_Renderer = &render_system.GetRenderer();

    vk::DeviceSize buffer_size = sizeof(vertices[0]) * vertices.size();
    m_VertexBuffer.Init(m_Context->GetRaiiDevice(), m_Context->GetGpu(), buffer_size,
                        vk::BufferUsageFlagBits::eVertexBuffer,
                        vk::MemoryPropertyFlagBits::eHostVisible | vk::MemoryPropertyFlagBits::eHostCoherent);
    m_VertexBuffer.Upload(vertices.data(), buffer_size);

    VertexInputState vertex_input{
        .stride = sizeof(Vertex),
        .attributes = {
            {.location = 0, .binding = 0, .format = vk::Format::eR32G32Sfloat, .offset = offsetof(Vertex, position)},
            {.location = 1, .binding = 0, .format = vk::Format::eR32G32B32Sfloat, .offset = offsetof(Vertex, color)},
        },
    };

    m_Pipeline.Init(m_Context->GetRaiiDevice(), m_Renderer->GetColorFormat(), vertex_input);
}

void VulkanLayer::OnDetach() {
    m_Pipeline.Cleanup();
    m_VertexBuffer.Cleanup();
    m_Context = nullptr;
    m_Renderer = nullptr;
}

void VulkanLayer::OnUpdate(Timestep &ts) {
    if (m_Renderer->BeginFrame()) {
        RenderTriangle(m_Renderer->Cmd(), m_Renderer->GetImageIndex());
        m_Renderer->EndFrame();
    }
}

void VulkanLayer::OnImGuiRender() {
}

void VulkanLayer::RenderTriangle(vk::CommandBuffer cmd, uint32_t swapchain_index) {
    // Set clear color values.
    vk::ClearValue clear_value;
    clear_value.color = std::array<float, 4>({{0.01f, 0.01f, 0.033f, 1.0f}});

    // Set up the rendering attachment info
    vk::RenderingAttachmentInfo color_attachment{.imageView = m_Renderer->GetCurrentImageView(),
                                                 .imageLayout = vk::ImageLayout::eColorAttachmentOptimal,
                                                 .loadOp = vk::AttachmentLoadOp::eClear,
                                                 .storeOp = vk::AttachmentStoreOp::eStore,
                                                 .clearValue = clear_value};

    // Begin rendering
    vk::RenderingInfo rendering_info{
        .renderArea = {.offset = {0, 0},
                       .extent = {.width = m_Renderer->GetDimensions().width, .height = m_Renderer->GetDimensions().height}},
        .layerCount = 1,
        .colorAttachmentCount = 1,
        .pColorAttachments = &color_attachment};

    cmd.beginRendering(rendering_info);

    // Bind the graphics pipeline.
    cmd.bindPipeline(vk::PipelineBindPoint::eGraphics, m_Pipeline.GetPipeline());

    // Set dynamic states
    vk::Viewport vp{.width = static_cast<float>(m_Renderer->GetDimensions().width),
                    .height = static_cast<float>(m_Renderer->GetDimensions().height),
                    .minDepth = 0.0f,
                    .maxDepth = 1.0f};
    cmd.setViewport(0, vp);

    vk::Rect2D scissor{
        .extent = {.width = m_Renderer->GetDimensions().width, .height = m_Renderer->GetDimensions().height}};
    cmd.setScissor(0, scissor);

    cmd.setCullMode(vk::CullModeFlagBits::eNone);
    cmd.setFrontFace(vk::FrontFace::eClockwise);
    cmd.setPrimitiveTopology(vk::PrimitiveTopology::eTriangleList);

    vk::Buffer vb = m_VertexBuffer.GetBuffer();
    cmd.bindVertexBuffers(0, vb, {0});

    cmd.draw(vertices.size(), 1, 0, 0);

    cmd.endRendering();
}

} // namespace GE
