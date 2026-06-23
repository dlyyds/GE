#pragma once

#include "GE/GE.h"

#include <memory>
#include <vector>

#include "GE/Render/Camera.h"
#include "GE/Render/Mesh.h"
#include "GE/Render/Model.h"
#include "GE/Render/Material.h"
#include "GE/Render/Texture.h"

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
    float m_LodBias = 0.0f;

    Camera m_Camera;
    Texture m_Texture;           // 旧：单纹理（之后可移除）
    Model m_Model;
    Material m_Material;         // 旧：单材质（无 MTL 时的 fallback）

    // === MTL 材质 ===
    struct SubmeshMaterial {
        Material material;
        Texture  texture;        // 若没纹理则为空，使用 m_DefaultTexture
    };
    std::vector<std::unique_ptr<SubmeshMaterial>> m_SubmeshMaterials;
    Texture m_DefaultTexture;    // 1x1 白色 fallback 纹理

    // === 灯光控制 ===
    bool m_LightEnabled = true;
    bool m_DirLightEnabled = true;
    bool m_PointLightEnabled = true;
    bool m_AmbientEnabled = true;

    // 方向光
    Renderer::DirectionalLight m_DirectionalLight{
        {0.0f, -1.0f, 0.5f, 0.0f},  // 方向（从斜上方照下）
        {1.0f, 0.95f, 0.9f, 0.6f},  // 暖白色，强度 0.6
    };

    // 点光源
    Renderer::PointLight m_PointLight{
        {2.0f, 3.0f, 1.0f, 0.15f},  // 位置 + 半径倒数 0.15
        {1.0f, 0.6f, 0.3f, 0.8f},   // 橙色，强度 0.8
    };

    // 环境光
    glm::vec4 m_Ambient{0.05f, 0.05f, 0.1f, 1.0f};

    // 镜面反射强度
    float m_SpecularStrength = 0.5f;
};

} // namespace GE
