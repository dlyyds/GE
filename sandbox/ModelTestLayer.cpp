//
// 3D 模型渲染测试层：加载立方体模型，使用透视摄像机 + Blinn-Phong 光照渲染
//

#include "ModelTestLayer.h"
#include "GE/Core/Application.h"
#include "GE/Core/GEInput.h"
#include "GE/Core/KeyCodes.h"
#include "GE/Render/Renderer.h"
#include "GE/Render/Renderer3D.h"
#include "GE/Scene/Components.h"

#include "imgui.h"

#include <glm/gtc/matrix_transform.hpp>
#include <cmath>

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

    // 用代码生成立方体网格（带法线和 UV，避免 OBJ 缺少顶点属性）
    {
        // 6 个面，每个面 4 个顶点，共 24 个顶点（每个面独立顶点以便面法线 + 面 UV）
        std::vector<Vertex> vertices;
        vertices.reserve(24);
        std::vector<uint32_t> indices;
        indices.reserve(36);

        // 辅助 lambda：添加一个四边形（两个三角形）
        auto add_quad = [&](const glm::vec3 &p0, const glm::vec3 &p1,
                            const glm::vec3 &p2, const glm::vec3 &p3,
                            const glm::vec3 &normal) {
            uint32_t base = static_cast<uint32_t>(vertices.size());
            // UV 按顶点顺序：左下、右下、右上、左上
            glm::vec2 uvs[] = {{0.0f, 0.0f}, {1.0f, 0.0f}, {1.0f, 1.0f}, {0.0f, 1.0f}};
            glm::vec3 pos[] = {p0, p1, p2, p3};
            for (int i = 0; i < 4; ++i) {
                vertices.push_back({pos[i], normal, uvs[i]});
            }
            // 两个三角形：0-1-2 和 0-2-3
            indices.push_back(base + 0);
            indices.push_back(base + 1);
            indices.push_back(base + 2);
            indices.push_back(base + 0);
            indices.push_back(base + 2);
            indices.push_back(base + 3);
        };

        // 立方体 6 个面（边长为 2，中心在原点，面向各轴向）
        // +Z 面（前）
        add_quad({-1.0f, -1.0f,  1.0f}, { 1.0f, -1.0f,  1.0f},
                 { 1.0f,  1.0f,  1.0f}, {-1.0f,  1.0f,  1.0f},
                 { 0.0f,  0.0f,  1.0f});
        // -Z 面（后）
        add_quad({ 1.0f, -1.0f, -1.0f}, {-1.0f, -1.0f, -1.0f},
                 {-1.0f,  1.0f, -1.0f}, { 1.0f,  1.0f, -1.0f},
                 { 0.0f,  0.0f, -1.0f});
        // +X 面（右）
        add_quad({ 1.0f, -1.0f,  1.0f}, { 1.0f, -1.0f, -1.0f},
                 { 1.0f,  1.0f, -1.0f}, { 1.0f,  1.0f,  1.0f},
                 { 1.0f,  0.0f,  0.0f});
        // -X 面（左）
        add_quad({-1.0f, -1.0f, -1.0f}, {-1.0f, -1.0f,  1.0f},
                 {-1.0f,  1.0f,  1.0f}, {-1.0f,  1.0f, -1.0f},
                 {-1.0f,  0.0f,  0.0f});
        // +Y 面（上）
        add_quad({-1.0f,  1.0f,  1.0f}, { 1.0f,  1.0f,  1.0f},
                 { 1.0f,  1.0f, -1.0f}, {-1.0f,  1.0f, -1.0f},
                 { 0.0f,  1.0f,  0.0f});
        // -Y 面（下）
        add_quad({-1.0f, -1.0f, -1.0f}, { 1.0f, -1.0f, -1.0f},
                 { 1.0f, -1.0f,  1.0f}, {-1.0f, -1.0f,  1.0f},
                 { 0.0f, -1.0f,  0.0f});

        m_CubeMesh = Mesh::Create(device, vertices, indices);
        GE_CORE_ASSERT(m_CubeMesh, "创建立方体网格失败");
        m_CubeMesh->SetDebugName("ModelTest_Cube");
    }

    // 创建场景与模型实体
    m_Scene = std::make_unique<Scene>();
    m_ModelEntity = m_Scene->CreateEntity("Cube");

    // 添加 3D 网格渲染组件（关联立方体网格 + 棋盘纹理）
    m_ModelEntity.AddComponent<MeshComponent>(m_CubeMesh.get(), m_Texture.get());

    // 创建相机实体并添加相机组件
    m_CameraEntity = m_Scene->CreateEntity("MainCamera");
    auto &cameraComp = m_CameraEntity.AddComponent<CameraComponent>();
    cameraComp.CameraInstance.SetPerspective(60.0f, 16.0f / 9.0f, 0.1f, 100.0f);
    cameraComp.CameraInstance.SetMode(Camera::Mode::Orbit);
    cameraComp.CameraInstance.SetOrbit(0.0f, 0.0f, 3.0f); // 距离目标 3 个单位
    cameraComp.CameraInstance.SetTarget({0.0f, 0.0f, 0.0f});

    // 创建红色点光源（右上方）
    m_RedLightEntity = m_Scene->CreateEntity("RedPointLight");
    m_RedLightEntity.GetComponent<TransformComponent>().Translation = {1.5f, 1.5f, 1.0f};
    m_RedLightEntity.GetComponent<TransformComponent>().Scale = {0.15f, 0.15f, 0.15f};
    m_RedLightEntity.AddComponent<PointLightComponent>(
        glm::vec4(1.0f, 0.2f, 0.2f, 1.5f),  // 红色，强度 1.5
        0.4f                                 // 半径倒数
    );
    // 给光源加一个可视化小球（用立方体缩小代替，颜色和光源一致）
    m_RedLightEntity.AddComponent<MeshComponent>(
        m_CubeMesh.get(),
        glm::vec4(1.0f, 0.4f, 0.4f, 1.0f)
    );

    // 创建蓝色点光源（左下方）
    m_BlueLightEntity = m_Scene->CreateEntity("BluePointLight");
    m_BlueLightEntity.GetComponent<TransformComponent>().Translation = {-1.5f, -1.0f, 1.0f};
    m_BlueLightEntity.GetComponent<TransformComponent>().Scale = {0.15f, 0.15f, 0.15f};
    m_BlueLightEntity.AddComponent<PointLightComponent>(
        glm::vec4(0.2f, 0.3f, 1.0f, 1.5f),  // 蓝色，强度 1.5
        0.4f                                 // 半径倒数
    );
    // 给光源加一个可视化小球（用立方体缩小代替，颜色和光源一致）
    m_BlueLightEntity.AddComponent<MeshComponent>(
        m_CubeMesh.get(),
        glm::vec4(0.4f, 0.5f, 1.0f, 1.0f)
    );

    // 添加脚本组件（自动旋转逻辑）
    RefreshScript();
    // 添加相机鼠标控制脚本
    RefreshCameraScript();
    // 添加点光源旋转动画脚本
    RefreshLightScripts();

    // 设置场景层级面板的上下文
    m_HierarchyPanel.SetContext(m_Scene.get());
}

void ModelTestLayer::OnDetach() {
    m_ModelEntity = {};
    m_CameraEntity = {};
    m_RedLightEntity = {};
    m_BlueLightEntity = {};
    m_HierarchyPanel.SetContext(nullptr);
    m_Scene.reset();
    m_Texture.reset();
    m_CubeMesh.reset();
}

void ModelTestLayer::OnUpdate(Timestep &ts) {
    auto extent = Application::GetSwapchain().GetExtent();
    float aspect = static_cast<float>(extent.width) /
                   static_cast<float>(extent.height);

    // 从相机组件获取视图与投影矩阵
    auto &cameraComp = m_CameraEntity.GetComponent<CameraComponent>();
    auto &camera = cameraComp.CameraInstance;

    // 同步宽高比（非固定宽高比时随窗口调整）
    if (!cameraComp.FixedAspectRatio) {
        camera.SetAspect(aspect);
    }

    glm::mat4 view = camera.GetView();
    glm::mat4 projection = camera.GetProj();
    glm::vec3 cameraPos = camera.GetPosition();

    // 清屏颜色（深蓝灰色背景）
    glm::vec4 clearColor{0.1f, 0.1f, 0.15f, 1.0f};

    // 场景更新 + 3D 渲染
    m_Scene->OnUpdate3D(ts, view, projection, cameraPos, clearColor);
}

void ModelTestLayer::OnEvent(Event &event) {
    // 将事件转发给相机（处理鼠标移动、滚轮、按键等交互）
    if (m_CameraEntity) {
        auto &cameraComp = m_CameraEntity.GetComponent<CameraComponent>();
        cameraComp.CameraInstance.OnEvent(event);
    }
}

void ModelTestLayer::OnImGuiRender() {
    // 场景层级 + 属性面板
    m_HierarchyPanel.OnImGuiRender();

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
                mc.BaseTexture ? "Checkerboard.png" : "(null)");

    ImGui::Separator();

    // 摄像机参数（通过 CameraComponent 控制）
    {
        ImGui::Text("摄像机 (CameraComponent)");
        auto &cameraComp = m_CameraEntity.GetComponent<CameraComponent>();
        auto &camera = cameraComp.CameraInstance;

        // 相机模式切换
        int mode = static_cast<int>(camera.GetMode());
        if (ImGui::Combo("模式", &mode, "Orbit\0FPS\0")) {
            camera.SetMode(static_cast<Camera::Mode>(mode));
        }

        // FOV（通过临时变量修改，因为 GetFov 返回值拷贝）
        float fov = camera.GetFov();
        if (ImGui::SliderFloat("FOV (度)", &fov, 10.0f, 120.0f)) {
            camera.SetPerspective(fov, camera.GetAspect(), 0.1f, 100.0f);
        }

        ImGui::Checkbox("主相机", &cameraComp.Primary);
        ImGui::Checkbox("固定宽高比", &cameraComp.FixedAspectRatio);

        if (camera.GetMode() == Camera::Mode::Orbit) {
            // 轨道相机参数
            glm::vec3 target = camera.GetTarget();
            float theta = camera.GetTheta();
            float phi = camera.GetPhi();
            float distance = camera.GetDistance();

            if (ImGui::DragFloat3("目标点 (Target)", &target.x, 0.1f,
                                  -10.0f, 10.0f)) {
                camera.SetTarget(target);
            }
            if (ImGui::SliderFloat("方位角 (Theta, deg)", &theta,
                                   -180.0f, 180.0f)) {
                camera.SetOrbit(theta, phi, distance);
            }
            if (ImGui::SliderFloat("俯仰角 (Phi, deg)", &phi,
                                   -89.0f, 89.0f)) {
                camera.SetOrbit(theta, phi, distance);
            }
            if (ImGui::DragFloat("距离 (Distance)", &distance, 0.1f,
                                 0.5f, 50.0f)) {
                camera.SetOrbit(theta, phi, distance);
            }
        } else {
            // FPS 相机参数
            glm::vec3 pos = camera.GetPosition();
            float yaw = camera.GetYaw();
            float pitch = camera.GetPitch();

            if (ImGui::DragFloat3("位置 (Position)", &pos.x, 0.1f,
                                  -20.0f, 20.0f)) {
                camera.SetPosition(pos);
            }
            if (ImGui::SliderFloat("偏航角 (Yaw, deg)", &yaw,
                                   -180.0f, 180.0f)) {
                camera.SetYawPitch(yaw, pitch);
            }
            if (ImGui::SliderFloat("俯仰角 (Pitch, deg)", &pitch,
                                   -89.0f, 89.0f)) {
                camera.SetYawPitch(yaw, pitch);
            }
        }

        // 相机灵敏度
        ImGui::DragFloat("鼠标灵敏度", &camera.MouseSensitivity,
                         0.01f, 0.01f, 5.0f);
        ImGui::DragFloat("滚轮灵敏度", &camera.ScrollSensitivity,
                         0.05f, 0.1f, 10.0f);
        ImGui::DragFloat("移动速度", &camera.MoveSpeed,
                         0.1f, 0.1f, 20.0f);
    }

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

        // 点光源（支持多个）
        int plCount = static_cast<int>(light.pointLightCount);
        if (ImGui::SliderInt("点光源数量", &plCount, 0,
                             static_cast<int>(Renderer3D::MAX_POINT_LIGHTS))) {
            light.pointLightCount = static_cast<size_t>(plCount);
        }

        for (size_t i = 0; i < light.pointLightCount; i++) {
            auto &pl = light.pointLights[i];
            char label[64];
            snprintf(label, sizeof(label), "点光源 %zu 位置", i);
            ImGui::DragFloat3(label, &pl.position.x, 0.1f, -10.0f, 10.0f);

            snprintf(label, sizeof(label), "点光源 %zu 颜色 + 强度", i);
            ImGui::ColorEdit4(label, &pl.color.r);

            snprintf(label, sizeof(label), "点光源 %zu 半径倒数", i);
            ImGui::DragFloat(label, &pl.radiusInv, 0.01f, 0.01f, 5.0f);
        }

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

        // 重置相机
        auto &cameraComp = m_CameraEntity.GetComponent<CameraComponent>();
        auto &camera = cameraComp.CameraInstance;
        camera.SetMode(Camera::Mode::Orbit);
        camera.SetPerspective(60.0f, camera.GetAspect(), 0.1f, 100.0f);
        camera.SetTarget({0.0f, 0.0f, 0.0f});
        camera.SetOrbit(0.0f, 0.0f, 3.0f);
        cameraComp.Primary = true;
        cameraComp.FixedAspectRatio = false;

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

void ModelTestLayer::RefreshCameraScript() {
    if (!m_CameraEntity) {
        return;
    }

    // 相机控制脚本：每帧处理键盘输入（FPS 模式下 WASD 移动）
    auto callback = [](Timestep ts, Entity entity) {
        auto &cameraComp = entity.GetComponent<CameraComponent>();
        auto &camera = cameraComp.CameraInstance;

        // 仅在 FPS 模式下处理键盘移动
        if (camera.GetMode() != Camera::Mode::FPS) {
            return;
        }

        float speed = camera.MoveSpeed * ts.GetSeconds();

        if (Input::IsKeyPressed(Key::W)) {
            camera.MoveForward(speed);
        }
        if (Input::IsKeyPressed(Key::S)) {
            camera.MoveForward(-speed);
        }
        if (Input::IsKeyPressed(Key::A)) {
            camera.MoveRight(-speed);
        }
        if (Input::IsKeyPressed(Key::D)) {
            camera.MoveRight(speed);
        }
        if (Input::IsKeyPressed(Key::Q)) {
            camera.MoveUp(-speed);
        }
        if (Input::IsKeyPressed(Key::E)) {
            camera.MoveUp(speed);
        }
    };

    if (m_CameraEntity.HasComponent<ScriptComponent>()) {
        m_CameraEntity.GetComponent<ScriptComponent>().OnUpdate = std::move(callback);
    } else {
        m_CameraEntity.AddComponent<ScriptComponent>(std::move(callback));
    }
}

void ModelTestLayer::RefreshLightScripts() {
    if (!m_RedLightEntity || !m_BlueLightEntity) {
        return;
    }

    // 红色点光源：绕 Y 轴正方向旋转，半径 1.8，高度 1.5
    auto redLightCallback = [angle = 0.0f](Timestep ts, Entity entity) mutable {
        angle += ts.GetSeconds() * 1.2f;  // 旋转速度
        auto &tc = entity.GetComponent<TransformComponent>();
        tc.Translation.x = std::cos(angle) * 1.8f;
        tc.Translation.z = std::sin(angle) * 1.8f;
        tc.Translation.y = 1.5f;
    };

    // 蓝色点光源：绕 Y 轴反方向旋转，半径 1.5，高度 0.5
    auto blueLightCallback = [angle = 0.0f](Timestep ts, Entity entity) mutable {
        angle -= ts.GetSeconds() * 0.8f;  // 反向旋转，速度稍慢
        auto &tc = entity.GetComponent<TransformComponent>();
        tc.Translation.x = std::cos(angle) * 1.5f;
        tc.Translation.z = std::sin(angle) * 1.5f;
        tc.Translation.y = 0.5f;
    };

    // 红色光源脚本
    if (m_RedLightEntity.HasComponent<ScriptComponent>()) {
        m_RedLightEntity.GetComponent<ScriptComponent>().OnUpdate = std::move(redLightCallback);
    } else {
        m_RedLightEntity.AddComponent<ScriptComponent>(std::move(redLightCallback));
    }

    // 蓝色光源脚本
    if (m_BlueLightEntity.HasComponent<ScriptComponent>()) {
        m_BlueLightEntity.GetComponent<ScriptComponent>().OnUpdate = std::move(blueLightCallback);
    } else {
        m_BlueLightEntity.AddComponent<ScriptComponent>(std::move(blueLightCallback));
    }
}

} // namespace GE
