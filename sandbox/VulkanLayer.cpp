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
    auto &device = ctx.GetDevice();
    auto queue = ctx.GetVkQueue();
    auto qfi = ctx.GetGraphicsQueueIndex();
    auto &r = Renderer::Get();
    auto &swapchain = r.GetSwapchain();

    // 相机
    m_Camera.SetAspect(static_cast<float>(swapchain.GetExtent().width) /
                       static_cast<float>(swapchain.GetExtent().height));

    // 默认纹理（1x1 白色 fallback）
    m_DefaultTexture.LoadFromColor(device, queue, qfi, {1.0f, 1.0f, 1.0f});

    // 旧：单纹理（后续可移除）
    m_Texture.LoadFromFile(device, queue, qfi, "assets/textures/Checkerboard.png",
                           vk::Filter::eNearest, vk::Filter::eNearest);

    // 3D 模型（现在会同时解析 .mtl 材质信息）
    m_Model.LoadFromFile(device.GetVmaAllocator(), "assets/models/cube.obj");

    // 先创建一个临时管线获取 set0/set2 layout
    auto fmt = swapchain.GetFormat();
    VulkanPipeline tempPipeline = r.CreateDefaultPipeline(device.GetHandle(), fmt, Renderer::DEPTH_FORMAT);
    r.InitDescriptorSets(device.GetHandle(), tempPipeline.GetSetLayout(0),
                         tempPipeline.GetSetLayout(2));
    tempPipeline.Cleanup();

    // 为每个 MTL 材质创建 GPU Material + Texture
    auto &materials = m_Model.GetMaterials();
    m_SubmeshMaterials.clear();
    m_SubmeshMaterials.reserve(materials.size());

    for (auto &matData : materials) {
        auto sm = std::make_unique<SubmeshMaterial>();
        sm->material.Init(device.GetHandle(), r.CreateDefaultPipeline(device.GetHandle(), fmt, Renderer::DEPTH_FORMAT));

        if (!matData.diffuseTexPath.empty()) {
            sm->texture.LoadFromFile(device, queue, qfi, matData.diffuseTexPath);
            sm->material.SetTexture("samplerColor", sm->texture);
            GE_CORE_INFO("  Loaded texture: {}", matData.diffuseTexPath);
        } else {
            // 用 Kd 颜色创建 1x1 纹理
            sm->texture.LoadFromColor(device, queue, qfi, matData.diffuse);
            sm->material.SetTexture("samplerColor", sm->texture);
            GE_CORE_INFO("  Created color texture from Kd ({:.2f}, {:.2f}, {:.2f})",
                         matData.diffuse.r, matData.diffuse.g, matData.diffuse.b);
        }

        m_SubmeshMaterials.push_back(std::move(sm));
    }

    // 旧：单材质 fallback
    m_Material.Init(device.GetHandle(), r.CreateDefaultPipeline(device.GetHandle(), fmt, Renderer::DEPTH_FORMAT));
    m_Material.SetTexture("samplerColor", m_Texture);
}

void VulkanLayer::OnDetach() {
    // 清理 per-submesh 材质
    for (auto &sm : m_SubmeshMaterials) {
        sm->material.Cleanup();
        sm->texture.Cleanup();
    }
    m_SubmeshMaterials.clear();
    m_DefaultTexture.Cleanup();
    m_Material.Cleanup();
    m_Model.Cleanup();
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
    ImGui::SliderFloat("LOD Bias", &m_LodBias, -8.0f, 8.0f, "%.2f");

    ImGui::SeparatorText("Lighting");
    ImGui::Checkbox("Enable All Lights", &m_LightEnabled);

    ImGui::Checkbox("Directional Light", &m_DirLightEnabled);
    if (ImGui::CollapsingHeader("Directional", ImGuiTreeNodeFlags_DefaultOpen)) {
        ImGui::DragFloat3("Dir Direction", glm::value_ptr(m_DirectionalLight.direction),
                          0.05f, -1.0f, 1.0f);
        ImGui::ColorEdit3("Dir Color", glm::value_ptr(m_DirectionalLight.color));
        ImGui::SliderFloat("Dir Intensity", &m_DirectionalLight.color.w, 0.0f, 2.0f, "%.2f");
    }

    ImGui::Checkbox("Point Light", &m_PointLightEnabled);
    if (ImGui::CollapsingHeader("Point", ImGuiTreeNodeFlags_DefaultOpen)) {
        ImGui::DragFloat3("Point Position", glm::value_ptr(m_PointLight.position),
                          0.1f, -10.0f, 10.0f);
        ImGui::ColorEdit3("Point Color", glm::value_ptr(m_PointLight.color));
        ImGui::SliderFloat("Point Intensity", &m_PointLight.color.w, 0.0f, 5.0f, "%.2f");
        ImGui::SliderFloat("Point Radius", &m_PointLight.position.w, 0.01f, 1.0f, "%.3f");
    }

    ImGui::Checkbox("Ambient Light", &m_AmbientEnabled);
    if (ImGui::CollapsingHeader("Ambient", ImGuiTreeNodeFlags_DefaultOpen)) {
        ImGui::ColorEdit3("Ambient Color", glm::value_ptr(m_Ambient));
        ImGui::SliderFloat("Ambient Intensity", &m_Ambient.w, 0.0f, 2.0f, "%.2f");
    }

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
        m_Camera.SetAspect(static_cast<float>(swapchain.GetExtent().width) /
                           static_cast<float>(swapchain.GetExtent().height));
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

    // 构造灯光参数（按独立开关控制）
    auto dirLight = m_DirectionalLight;
    auto pointLight = m_PointLight;
    auto ambient = m_Ambient;

    if (!m_LightEnabled || !m_DirLightEnabled)
        dirLight.color.w = 0.0f;
    if (!m_LightEnabled || !m_PointLightEnabled)
        pointLight.color.w = 0.0f;
    if (!m_LightEnabled || !m_AmbientEnabled)
        ambient.w = 0.0f;

    r.BeginScene(Application::GetFrameCmd(),
                 Application::GetFrameImageIndex(),
                 vk::Extent2D{swapchain.GetExtent().width, swapchain.GetExtent().height},
                 m_Camera.GetView(), m_Camera.GetProj(), m_Camera.GetPosition(),
                 dirLight, pointLight, ambient,
                 {0.01f, 0.01f, 0.033f, 1.0f});

    // 画模型（按 SubMesh 遍历）
    auto model = glm::mat4(1.0f);
    model = glm::translate(model, glm::vec3(m_Position, 0.0f));
    model = glm::rotate(model, glm::radians(m_Rotation), glm::vec3(0.0f, 1.0f, 0.0f));
    model = glm::scale(model, glm::vec3(m_Scale, 1.0f));

    auto &submeshes = m_Model.GetSubMeshes();
    if (!submeshes.empty()) {
        for (auto &submesh : submeshes) {
            int matIdx = submesh.materialIndex;
            Material *mat = nullptr;
            if (matIdx >= 0 && matIdx < (int)m_SubmeshMaterials.size()) {
                mat = &m_SubmeshMaterials[matIdx]->material;
            } else {
                mat = &m_Material; // fallback
            }
            r.Draw(m_Model.GetMesh(), *mat, model, m_TriangleColor, m_LodBias,
                   submesh.indexOffset, submesh.indexCount);
        }
    } else {
        // 无 SubMesh（如程序化球体），用旧 fallback
        r.Draw(m_Model.GetMesh(), m_Material, model, m_TriangleColor, m_LodBias);
    }

    r.EndScene();
}

} // namespace GE
