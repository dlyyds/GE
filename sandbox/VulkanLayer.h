#pragma once

#include "GE/GE.h"
#include "GE/Render/RenderSystem.h"
#include "GE/Render/VulkanPipeline.h"
#include "GE/Render/VulkanBuffer.h"


struct Vertex {
    glm::vec2 position;
    glm::vec3 color;
};

// Define the vertex data
const std::vector<Vertex> vertices = {
    {{0.5f, -0.5f}, {1.0f, 0.0f, 0.0f}},
    {{0.5f, 0.5f}, {0.0f, 1.0f, 0.0f}},
    {{-0.5f, 0.5f}, {0.0f, 0.0f, 1.0f}}
};


namespace GE {


class VulkanLayer : public Layer {
public:
    VulkanLayer();

    ~VulkanLayer() override = default;

    void OnAttach() override;
    void OnDetach() override;
    void OnUpdate(Timestep &ts) override;
    void OnEvent(Event &event) override {}
    void OnImGuiRender() override;

private:
    void RenderTriangle(vk::CommandBuffer cmd, uint32_t swapchain_index);

private:
    VulkanContext *m_Context = nullptr;
    VulkanRenderer *m_Renderer = nullptr;
    VulkanPipeline m_Pipeline;
    VulkanBuffer m_VertexBuffer;
};

} // namespace GE
