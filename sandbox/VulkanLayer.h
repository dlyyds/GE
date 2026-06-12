#pragma once

#include "GE/GE.h"
#include "GE/Render/Camera.h"
#include "GE/Render/VulkanBase/VulkanImage.h"
#include "GE/Render/VulkanBase/VulkanSampler.h"
#include "GE/Render/Mesh.h"
#include "GE/Render/Model.h"
#include "GE/Render/Material.h"

namespace GE {

class VulkanLayer : public Layer {
public:
    VulkanLayer();
    ~VulkanLayer() override = default;

    void OnAttach() override;
    void OnDetach() override;
    void OnUpdate(Timestep &ts) override;
    void OnEvent(Event &event) override;
    void OnImGuiRender() override;

private:
    void RenderFrame();

private:
    glm::vec4 m_TriangleColor{0.3f, 0.6f, 0.9f, 1.0f};

    float m_Rotation = 0.0f;
    glm::vec2 m_Position{0.0f, 0.0f};
    glm::vec2 m_Scale{1.0f, 1.0f};

    Camera m_Camera;
    VulkanImage m_Texture;
    VulkanSampler m_Sampler;
    Model m_Model;
    Material m_Material;
};

} // namespace GE
