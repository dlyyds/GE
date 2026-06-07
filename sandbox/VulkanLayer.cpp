//
// Created by Lenovo on 2026/6/2.
//

#include "VulkanLayer.h"

#include "GE/Render/Renderer2D.h"
#include "GE/Render/VulkanBase/VulkanDevice.h"

#include "imgui.h"
#include "glm/gtc/type_ptr.hpp"
#include "glm/gtc/matrix_transform.hpp"

namespace GE {

VulkanLayer::VulkanLayer() : Layer("VulkanLayer") {
}

void VulkanLayer::OnAttach() {
    auto &r = Renderer2D::Get();
    auto device = r.GetVkDevice();
    auto gpu = r.GetVkGpu();
    auto queue = r.GetVkQueue();
    auto qfi = r.GetGraphicsQueueIndex();
    auto &swapchain = r.GetSwapchain();
    m_Camera.SetAspect(static_cast<float>(swapchain.GetDimensions().width) /
                       static_cast<float>(swapchain.GetDimensions().height));

    // Texture
    m_Texture.LoadFromFile(device, gpu, queue, qfi, "assets/textures/Checkerboard.png");
    m_Sampler.Init(device, vk::Filter::eNearest, vk::Filter::eNearest);
    Renderer2D::Get().SetTexture(m_Texture.GetView(), m_Sampler.Get());
}

void VulkanLayer::OnDetach() {
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
        auto &swapchain = Renderer2D::Get().GetSwapchain();
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
    // Compute model matrix
    auto model = glm::mat4(1.0f);
    model = glm::translate(model, glm::vec3(m_Position, 0.0f));
    model = glm::rotate(model, glm::radians(m_Rotation), glm::vec3(0.0f, 0.0f, 1.0f));
    model = glm::scale(model, glm::vec3(m_Scale, 1.0f));

    Renderer2D::Get().BeginScene(m_Camera.GetView(), m_Camera.GetProj(), m_Camera.GetPosition(),
                                  {0.01f, 0.01f, 0.033f, 1.0f});
    Renderer2D::Get().DrawRect(model, m_TriangleColor);
    Renderer2D::Get().EndScene();
}

} // namespace GE
