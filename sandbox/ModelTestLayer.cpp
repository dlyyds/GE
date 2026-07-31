//
// 3D 模型渲染测试层：加载立方体模型，使用透视摄像机 + Blinn-Phong 光照渲染
//

#include "ModelTestLayer.h"
#include "GE/Core/Application.h"
#include "GE/Render/Renderer.h"
#include "GE/Render/Renderer3D.h"
#include "GE/Scene/Components.h"

#include "imgui.h"

#include <glm/gtc/matrix_transform.hpp>

namespace GE {

ModelTestLayer::ModelTestLayer() : Layer("ModelTestLayer") {
}

ModelTestLayer::~ModelTestLayer() = default;

void ModelTestLayer::OnAttach() {
    auto &ctx = Application::GetVulkanContext();
    auto &device = ctx.GetDevice();
    auto &cache = device.GetResourceCache();

    // 加载棋盘纹理（用于立方体表面）
    m_Texture = Texture::LoadFromFile(device, cache,
                                      "assets/textures/Checkerboard.png",
                                      vk::Format::eR8G8B8A8Srgb,
                                      vk::Filter::eNearest,
                                      vk::Filter::eNearest);
    m_Texture->SetDebugName("ModelTest_Checkerboard");

    // 加载立方体 OBJ 模型
    m_CubeMesh = Mesh::LoadFromFile(device, "assets/models/cube.obj");
    GE_CORE_ASSERT(m_CubeMesh, "加载立方体模型失败：assets/models/cube.obj");
    m_CubeMesh->SetDebugName("ModelTest_Cube");

    // 创建场景与模型实体
    m_Scene = std::make_unique<Scene>();
    m_ModelEntity = m_Scene->CreateEntity("Cube");

    // 添加 3D 网格渲染组件（关联立方体网格 + 棋盘纹理）
    m_ModelEntity.AddComponent<MeshComponent>(m_CubeMesh.get());

    // 添加脚本组件（自动旋转逻辑）
    RefreshScript();
}

void ModelTestLayer::OnDetach() {
    m_ModelEntity = {};
    m_Scene.reset();
    m_Texture.reset();
    m_CubeMesh.reset();
}

void ModelTestLayer::OnUpdate(Timestep &ts) {
    auto extent = Application::GetSwapchain().GetExtent();
    float aspect = static_cast<float>(extent.width) /
                   static_cast<float>(extent.height);

    // 构建透视投影矩阵
    glm::mat4 projection = glm::perspective(
        glm::radians(m_Fov),
        aspect,
        0.1f,   // 近裁剪面
        100.0f  // 远裁剪面
    );
    // Vulkan Y 轴翻转
    projection[1][1] *= -1.0f;

    // 构建视图矩阵（LookAt）
    glm::mat4 view = glm::lookAt(
        m_CameraPos,
        m_CameraTarget,
        glm::vec3(0.0f, 1.0f, 0.0f)  // 上向量
    );

    // 清屏颜色（深蓝灰色背景）
    glm::vec4 clearColor{0.1f, 0.1f, 0.15f, 1.0f};

    // 场景更新 + 3D 渲染
    m_Scene->OnUpdate3D(ts, view, projection, m_CameraPos, clearColor);
}

void ModelTestLayer::OnEvent(Event &event) {
}

void ModelTestLayer::OnImGuiRender() {
    ImGui::Begin("ModelTestLayer");
    ImGui::Text("3D 模型渲染测试（Scene + MeshComponent + Renderer3D）");
    ImGui::Separator();

    if (!m_ModelEntity) {
        ImGui::TextDisabled("实体未创建");
        ImGui::End();
        return;
    }

    auto &tc = m_ModelEntity.GetComponent<TransformComponent>();
    auto &mc = m_ModelEntity.GetComponent<MeshComponent>();

    // Tag
    auto &tag = m_ModelEntity.GetComponent<TagComponent>().Tag;
    ImGui::Text("实体名称：%s", tag.c_str());
    ImGui::Separator();

    // Transform 组件参数
    ImGui::Text("Transform 组件");
    ImGui::DragFloat3("位置 (Translation)", &tc.Translation.x, 0.05f,
                      -10.0f, 10.0f);
    ImGui::SliderFloat3("旋转 (Rotation, rad)", &tc.Rotation.x,
                        -3.14159f * 2.0f, 3.14159f * 2.0f);
    ImGui::DragFloat3("缩放 (Scale)", &tc.Scale.x, 0.05f,
                      0.01f, 10.0f);

    ImGui::Separator();

    // MeshComponent 组件参数
    ImGui::Text("MeshComponent 组件");
    ImGui::ColorEdit4("颜色 (Color)", &mc.Color.r);
    ImGui::Text("网格顶点数：%u", m_CubeMesh ? m_CubeMesh->GetVertexCount() : 0);
    ImGui::Text("网格索引数：%u", m_CubeMesh ? m_CubeMesh->GetIndexCount() : 0);
    ImGui::Text("纹理：%s",
                m_Texture ? "Checkerboard.png" : "(null)");

    ImGui::Separator();

    // 摄像机参数
    ImGui::Text("摄像机");
    ImGui::DragFloat3("位置 (CameraPos)", &m_CameraPos.x, 0.1f,
                      -20.0f, 20.0f);
    ImGui::DragFloat3("目标 (CameraTarget)", &m_CameraTarget.x, 0.1f,
                      -10.0f, 10.0f);
    ImGui::SliderFloat("FOV (度)", &m_Fov, 10.0f, 120.0f);

    ImGui::Separator();

    // Script 组件参数
    ImGui::Text("Script 组件（自动绕 Y 轴旋转）");
    bool scriptChanged = false;
    scriptChanged |= ImGui::Checkbox("启用", &m_AutoRotate);
    if (m_AutoRotate) {
        scriptChanged |= ImGui::DragFloat("旋转速度", &m_AutoRotateSpeed,
                                          0.05f, -10.0f, 10.0f,
                                          "%.3f rad/s");
    }
    if (scriptChanged) {
        RefreshScript();
    }

    ImGui::Separator();

    // 光照参数
    {
        ImGui::Text("光照参数");
        auto &r3d = Renderer::Get3DRenderer();
        auto &light = r3d.GetLightParams();

        // 方向光
        ImGui::DragFloat3("方向光方向", &light.dirLightDirection.x, 0.05f,
                          -1.0f, 1.0f);
        ImGui::ColorEdit4("方向光颜色 + 强度", &light.dirLightColor.r);

        // 点光源
        ImGui::DragFloat3("点光源位置", &light.pointLightPosition.x, 0.1f,
                          -10.0f, 10.0f);
        ImGui::ColorEdit4("点光源颜色 + 强度", &light.pointLightColor.r);
        ImGui::DragFloat("点光源半径倒数", &light.pointLightRadiusInv,
                         0.01f, 0.01f, 5.0f);

        // 环境光
        ImGui::ColorEdit4("环境光 + 强度", &light.ambient.r);
    }

    ImGui::Separator();

    // 统计信息
    auto meshView = m_Scene->Reg().view<MeshComponent>();
    auto scriptView = m_Scene->Reg().view<ScriptComponent>();
    ImGui::Text("场景 3D 实体数：%zu", meshView.size());
    ImGui::Text("场景脚本数：%zu", scriptView.size());

    // 重置按钮
    if (ImGui::Button("重置参数")) {
        tc.Translation = {0.0f, 0.0f, 0.0f};
        tc.Rotation = {0.0f, 0.0f, 0.0f};
        tc.Scale = {1.0f, 1.0f, 1.0f};
        mc.Color = {1.0f, 1.0f, 1.0f, 1.0f};
        m_CameraPos = {0.0f, 0.0f, 3.0f};
        m_CameraTarget = {0.0f, 0.0f, 0.0f};
        m_Fov = 60.0f;
        m_AutoRotate = true;
        m_AutoRotateSpeed = 0.5f;
        RefreshScript();
    }

    ImGui::End();
}

void ModelTestLayer::RefreshScript() {
    if (!m_ModelEntity) {
        return;
    }

    if (!m_AutoRotate) {
        // 关闭自动旋转：移除脚本组件
        if (m_ModelEntity.HasComponent<ScriptComponent>()) {
            m_ModelEntity.RemoveComponent<ScriptComponent>();
        }
        return;
    }

    // 开启自动旋转：添加 / 更新脚本组件（绕 Y 轴旋转）
    float speed = m_AutoRotateSpeed;
    auto callback = [speed](Timestep ts, Entity entity) {
        auto &tc = entity.GetComponent<TransformComponent>();
        tc.Rotation.y += ts.GetSeconds() * speed;
    };

    if (m_ModelEntity.HasComponent<ScriptComponent>()) {
        m_ModelEntity.GetComponent<ScriptComponent>().OnUpdate = std::move(callback);
    } else {
        m_ModelEntity.AddComponent<ScriptComponent>(std::move(callback));
    }
}

} // namespace GE
