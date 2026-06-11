//
// Created by Lenovo on 2026/6/2.
//

#include "VulkanLayer.h"

#include "GE/Render/Renderer.h"
#include "GE/Core/Application.h"

#include "imgui.h"
#include "glm/gtc/type_ptr.hpp"
#include "glm/gtc/matrix_transform.hpp"

namespace GE {

VulkanLayer::VulkanLayer() : Layer("VulkanLayer") {
}

void VulkanLayer::OnAttach() {
    auto &ctx = Application::GetVulkanContext();
    auto allocator = ctx.GetVmaAllocator();
    auto device = ctx.GetVkDevice();
    auto queue = ctx.GetVkQueue();
    auto qfi = ctx.GetGraphicsQueueIndex();
    auto &r = Renderer::Get();
    auto &swapchain = r.GetSwapchain();

    // 相机
    m_Camera.SetAspect(static_cast<float>(swapchain.GetDimensions().width) /
                       static_cast<float>(swapchain.GetDimensions().height));

    // 纹理
    m_Texture.LoadFromFile(allocator, queue, qfi, "assets/textures/Checkerboard.png");
    m_Sampler.Init(device, vk::Filter::eNearest, vk::Filter::eNearest);

    // 四边形网格
    m_Mesh.Init(allocator, Mesh::kVertices, sizeof(Mesh::kVertices),
                Mesh::kIndices, sizeof(Mesh::kIndices), 6);

    // 材质
    auto fmt = swapchain.GetDimensions().format;
    m_Material.Init(device, r.CreateDefaultPipeline(device, fmt));
    m_Material.SetTexture("samplerColor", m_Texture.GetView(), m_Sampler.Get());

    // Renderer 接管 set=0（FrameUBO）和 set=2（ObjectUBO）
    r.InitDescriptorSets(device,
                         m_Material.pipeline.GetSetLayout(0),
                         m_Material.pipeline.GetSetLayout(2));
}

void VulkanLayer::OnDetach() {
    m_Material.Cleanup();
    m_Mesh.Destroy();
    m_Sampler.Cleanup();
    m_Texture.Cleanup();
}

void VulkanLayer::OnUpdate(Timestep &ts) {
    RenderFrame();
}

void VulkanLayer::OnEvent(Event &event) {
    m_Camera.OnEvent(event);
}

void VulkanLayer::OnImGuiRender() {
    ImGui::Begin("VulkanLayer");
    ImGui::Text("FPS: %.1f  (%.2f ms)", Application::Get().GetFPS(),
                1000.0f / Application::Get().GetFPS());
    ImGui::Separator();

    ImGui::ColorEdit4("Color", glm::value_ptr(m_TriangleColor));

    ImGui::SeparatorText("Model");
    ImGui::SliderFloat("Rotation", &m_Rotation, -180.0f, 180.0f, "%.1f deg");
    ImGui::SliderFloat2("Position##Model", glm::value_ptr(m_Position), -1.0f, 1.0f);
    ImGui::SliderFloat2("Scale", glm::value_ptr(m_Scale), 0.1f, 3.0f);

    ImGui::SeparatorText("Camera");

    const char *modes[] = {"Orbit", "FPS"};
    int current = static_cast<int>(m_Camera.GetMode());
    if (ImGui::Combo("Mode", &current, modes, IM_ARRAYSIZE(modes))) {
        m_Camera.SetMode(static_cast<Camera::Mode>(current));
    }

    if (m_Camera.GetMode() == Camera::Mode::Orbit) {
        float theta = m_Camera.GetTheta();
        float phi = m_Camera.GetPhi();
        float dist = m_Camera.GetDistance();
        if (ImGui::SliderFloat("Theta", &theta, -180.0f, 180.0f, "%.1f deg") ||
            ImGui::SliderFloat("Phi", &phi, -89.0f, 89.0f, "%.1f deg") ||
            ImGui::SliderFloat("Distance", &dist, 0.5f, 50.0f, "%.1f")) {
            m_Camera.SetOrbit(theta, phi, dist);
        }
    } else {
        auto pos = m_Camera.GetPosition();
        float yaw = m_Camera.GetYaw();
        float pitch = m_Camera.GetPitch();
        if (ImGui::SliderFloat3("Position##Cam", glm::value_ptr(pos), -10.0f, 10.0f) ||
            ImGui::SliderFloat("Yaw", &yaw, -180.0f, 180.0f, "%.1f deg") ||
            ImGui::SliderFloat("Pitch", &pitch, -89.0f, 89.0f, "%.1f deg")) {
            m_Camera.SetPosition(pos);
            m_Camera.SetYawPitch(yaw, pitch);
        }
    }

    ImGui::SeparatorText("Sensitivity");
    ImGui::SliderFloat("Mouse", &m_Camera.MouseSensitivity, 0.05f, 2.0f, "%.2f");
    ImGui::SliderFloat("Scroll", &m_Camera.ScrollSensitivity, 0.1f, 5.0f, "%.1f");
    ImGui::SliderFloat("Move", &m_Camera.MoveSpeed, 0.5f, 20.0f, "%.1f");

    if (ImGui::Button("Reset Camera")) {
        m_Camera = Camera{};
        auto &swapchain = Renderer::Get().GetSwapchain();
        m_Camera.SetAspect(static_cast<float>(swapchain.GetDimensions().width) /
                           static_cast<float>(swapchain.GetDimensions().height));
    }

    ImGui::SeparatorText("Mouse Hint");
    ImGui::Text("Left drag:  rotate camera");
    ImGui::Text("Middle drag: pan");
    ImGui::Text("Scroll:     zoom / move");

    ImGui::End();
}

void VulkanLayer::RenderFrame() {
    auto &r = Renderer::Get();
    auto &swapchain = r.GetSwapchain();

    r.BeginScene(swapchain.GetCurrentCmd(),
                 swapchain.GetCurrentImageIndex(),
                 vk::Extent2D{swapchain.GetDimensions().width, swapchain.GetDimensions().height},
                 m_Camera.GetView(), m_Camera.GetProj(), m_Camera.GetPosition(),
                 {0.01f, 0.01f, 0.033f, 1.0f});

    // 画 5 个四边形，排成一行
    // 现在 Ring Buffer 保证每个 Draw 有自己的 UBO 空间，不会相互覆盖
    for (int i = 0; i < 6; i++) {
        float x = -0.8f + i * 0.4f; // -0.8, -0.4, 0.0, 0.4, 0.8
        auto model = glm::mat4(1.0f);
        model = glm::translate(model, glm::vec3(x, 0.0f, 0.0f));
        model = glm::scale(model, glm::vec3(0.35f, 0.35f, 1.0f));
        r.Draw(m_Mesh, m_Material, model, m_TriangleColor);
    }

    r.EndScene();
}

} // namespace GE
