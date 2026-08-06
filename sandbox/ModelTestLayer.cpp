//
// 3D 模型渲染测试层：加载立方体模型，使用透视摄像机 + Blinn-Phong 光照渲染
//

#include "ModelTestLayer.h"
#include "GE/Core/Application.h"
#include "GE/Core/GEInput.h"
#include "GE/Core/KeyCodes.h"
#include "GE/Render/Renderer.h"
#include "GE/Render/Renderer3D.h"
#include "GE/Render/TextureManager.h"
#include "GE/Render/MaterialManager.h"
#include "GE/Scene/Components.h"
#include "GE/Utils/PlatformUtils.h"

#include "imgui.h"
#include "ImGuizmo.h"
#include "glm/gtc/type_ptr.inl"

#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>
#include <cmath>

namespace GE {

ModelTestLayer::ModelTestLayer() : Layer("ModelTestLayer") {
}

ModelTestLayer::~ModelTestLayer() = default;

void ModelTestLayer::OnAttach() {
    auto &ctx = Application::GetVulkanContext();
    auto &device = ctx.GetDevice();
    auto &cache = device.GetResourceCache();

    // 从全局纹理管理器加载棋盘纹理（自动去重）
    auto &texMgr = Renderer::GetTextureManager();
    m_Texture = texMgr.Load("assets/textures/Checkerboard.png",
                            vk::Format::eR8G8B8A8Srgb,
                            vk::Filter::eNearest,
                            vk::Filter::eNearest);
    if (m_Texture) {
        m_Texture->SetDebugName("ModelTest_Checkerboard");
    }

    // 从全局材质管理器获取/创建单 Albedo 材质（自动按纹理路径去重）
    auto &matMgr = Renderer::GetMaterialManager();
    m_ModelMaterial = matMgr.GetOrCreateFromAlbedo(
        "assets/textures/Checkerboard.png");
    if (m_ModelMaterial) {
        m_ModelMaterial->SetDebugName("ModelTest_CubeMat");
    }

    {
        // 创建立方体网格（内置）
        m_CubeMesh = Mesh::CreateBuiltin(device, "cube");
        GE_CORE_ASSERT(m_CubeMesh, "创建立方体网格失败");
        m_CubeMesh->SetDebugName("ModelTest_Cube");

        // 创建球体网格（光源可视化用）
        m_SphereMesh = Mesh::CreateBuiltin(device, "sphere");
        GE_CORE_ASSERT(m_SphereMesh, "创建球体网格失败");
        m_SphereMesh->SetDebugName("ModelTest_Sphere");
    }

    // 创建场景与模型实体
    m_Scene = std::make_unique<Scene>();
    m_ModelEntity = m_Scene->CreateEntity("Cube");

    // 添加 3D 网格渲染组件
    m_ModelEntity.AddComponent<MeshComponent>(m_CubeMesh.get());
    // 添加材质组件（与网格解耦，独立管理）
    m_ModelEntity.AddComponent<MaterialComponent>(m_ModelMaterial);

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
        glm::vec4(1.0f, 0.2f, 0.2f, 1.5f), // 红色，强度 1.5
        0.4f // 半径倒数
        );
    // 给光源加一个可视化小球（球体，颜色和光源一致）
    m_RedLightEntity.AddComponent<MeshComponent>(
        m_SphereMesh.get(),
        glm::vec4(1.0f, 0.4f, 0.4f, 1.0f)
        );

    // 创建蓝色点光源（左下方）
    m_BlueLightEntity = m_Scene->CreateEntity("BluePointLight");
    m_BlueLightEntity.GetComponent<TransformComponent>().Translation = {-1.5f, -1.0f, 1.0f};
    m_BlueLightEntity.GetComponent<TransformComponent>().Scale = {0.15f, 0.15f, 0.15f};
    m_BlueLightEntity.AddComponent<PointLightComponent>(
        glm::vec4(0.2f, 0.3f, 1.0f, 1.5f), // 蓝色，强度 1.5
        0.4f // 半径倒数
        );
    // 给光源加一个可视化小球（球体，颜色和光源一致）
    m_BlueLightEntity.AddComponent<MeshComponent>(
        m_SphereMesh.get(),
        glm::vec4(0.4f, 0.5f, 1.0f, 1.0f)
        );
    m_BlueLightEntity.AddComponent<SphereColliderComponent>(1);
    auto &ballRbc = m_BlueLightEntity.AddComponent<RigidBodyComponent>(Physics::RigidBodyType::Dynamic);
    ballRbc.Mass = 1.0f;
    ballRbc.Restitution = 0.3f; // 有些弹性

    // 创建方向光
    m_DirLightEntity = m_Scene->CreateEntity("DirectionalLight");
    // 默认方向：从斜上方照向原点（先绕 X 转 -45° 让光朝下，再绕 Y 转 30° 给个水平角度）
    m_DirLightEntity.GetComponent<TransformComponent>().Rotation =
        glm::vec3(glm::radians(-45.0f), glm::radians(30.0f), 0.0f);
    m_DirLightEntity.AddComponent<DirectionalLightComponent>(
        glm::vec4(1.0f, 0.95f, 0.85f, 1.2f) // 暖白色，强度 1.2
        );

    // 创建环境光
    m_AmbientLightEntity = m_Scene->CreateEntity("AmbientLight");
    m_AmbientLightEntity.AddComponent<AmbientLightComponent>(
        glm::vec4(0.3f, 0.3f, 0.35f, 1.0f) // 偏冷灰，强度 1.0
        );

    // ── 物理测试：创建地板 + 动态球体 ──────────────────────────────────
    {
        // 静态地板（Box collider + Static rigid body）
        m_FloorEntity = m_Scene->CreateEntity("PhysicsFloor");
        auto &floorTc = m_FloorEntity.GetComponent<TransformComponent>();
        floorTc.Translation = {0.0f, -3.0f, 0.0f}; // 放在下方
        floorTc.Scale = {10.0f, 0.5f, 10.0f}; // 大而扁的盒子
        m_FloorEntity.AddComponent<BoxColliderComponent>(glm::vec3(1, 1, 1));
        m_FloorEntity.AddComponent<RigidBodyComponent>(Physics::RigidBodyType::Static);

        // 给地板加一个网格组件用于可视化
        m_FloorEntity.AddComponent<MeshComponent>(
            m_CubeMesh.get(),
            glm::vec4(0.5f, 0.5f, 0.5f, 1.0f)
            );

        // 动态球体（Sphere collider + Dynamic rigid body）
        m_PhysicsBallEntity = m_Scene->CreateEntity("PhysicsBall");
        auto &ballTc = m_PhysicsBallEntity.GetComponent<TransformComponent>();
        ballTc.Translation = {0.0f, 5.0f, 0.0f}; // 放在高处
        ballTc.Scale = {0.5f, 0.5f, 0.5f};
        m_PhysicsBallEntity.AddComponent<SphereColliderComponent>(1);
        auto &ballRbc = m_PhysicsBallEntity.AddComponent<RigidBodyComponent>(Physics::RigidBodyType::Kinematic);
        ballRbc.Mass = 1.0f;
        ballRbc.Restitution = 0.3f; // 有些弹性

        // 给球体加一个网格组件用于可视化
        m_PhysicsBallEntity.AddComponent<MeshComponent>(
            m_SphereMesh.get(),
            glm::vec4(0.2f, 0.6f, 1.0f, 1.0f)
            );
    }

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
    m_DirLightEntity = {};
    m_AmbientLightEntity = {};
    m_FloorEntity = {};
    m_PhysicsBallEntity = {};
    m_HierarchyPanel.SetContext(nullptr);
    m_Scene.reset();
    m_ModelMaterial = nullptr;  // 由全局 MaterialManager 管理生命周期
    m_Texture = nullptr;       // 由全局 TextureManager 管理生命周期
    m_CubeMesh.reset();
    m_SphereMesh.reset();
}

void ModelTestLayer::OnUpdate(Timestep &ts) {
    auto extent = Application::GetSwapchain().GetExtent();
    float aspect = static_cast<float>(extent.width) /
                   static_cast<float>(extent.height);

    // 同步场景视口尺寸（UI 精灵正交投影需要）
    m_Scene->OnViewportResize(extent.width, extent.height);

    // 场景中没有相机实体时，使用默认视角清屏
    if (!m_CameraEntity) {
        glm::mat4 view(1.0f);
        glm::mat4 projection = glm::perspective(glm::radians(60.0f), aspect, 0.1f, 100.0f);
        // Vulkan Y 翻转
        projection[1][1] *= -1.0f;
        glm::vec3 cameraPos{0.0f, 0.0f, 3.0f};
        glm::vec4 clearColor{0.1f, 0.1f, 0.15f, 1.0f};
        m_Scene->OnUpdate3D(ts, view, projection, cameraPos, clearColor);
        return;
    }

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
    // 快捷键切换 gizmo 模式（W/E/R/Q/T）
    if (event.GetEventType() == EventType::KeyPressed) {
        auto &keyEvent = static_cast<KeyPressedEvent &>(event);
        switch (keyEvent.GetKeyCode()) {
        case Key::Q: m_GizmoType = -1;
            return;
        case Key::W: m_GizmoType = ImGuizmo::TRANSLATE;
            return;
        case Key::E: m_GizmoType = ImGuizmo::ROTATE;
            return;
        case Key::R: m_GizmoType = ImGuizmo::SCALE;
            return;
        case Key::T: m_UseSnap = !m_UseSnap;
            return;
        default: break;
        }
    }

    // 将事件转发给场景（系统级处理 + ScriptComponent 事件回调）
    if (m_Scene && !event.Handled) {
        m_Scene->OnEvent(event);
    }

    // 将事件转发给相机（处理鼠标移动、滚轮、按键等交互）
    if (m_CameraEntity && !event.Handled) {
        auto &cameraComp = m_CameraEntity.GetComponent<CameraComponent>();
        cameraComp.CameraInstance.OnEvent(event);
    }
}

void ModelTestLayer::OnImGuiRender() {
    // 视口内 3D 变换 gizmo
    RenderImGuizmo();

    // 场景层级 + 属性面板
    m_HierarchyPanel.OnImGuiRender();

    ImGui::Begin("ModelTestLayer");
    ImGui::Text("3D 模型渲染测试（Scene + MeshComponent + Renderer3D）");
    ImGui::Separator();

    // ---- 场景文件控制 ----
    {
        ImGui::Text("场景文件");
        if (ImGui::Button("保存场景...")) {
            SaveScene();
        }
        ImGui::SameLine();
        if (ImGui::Button("加载场景...")) {
            LoadScene();
        }
        ImGui::SameLine();
        if (ImGui::Button("新建场景")) {
            NewScene();
        }
        ImGui::Separator();
    }

    // Gizmo 控制面板
    RenderImGuizmoPanel();

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

    ImGui::Separator();

    // MaterialComponent 组件参数
    bool hasMat = m_ModelEntity.HasComponent<MaterialComponent>();
    ImGui::Text("MaterialComponent 组件");
    if (!hasMat) {
        ImGui::Text("材质：(null)");
    } else {
        auto &matComp = m_ModelEntity.GetComponent<MaterialComponent>();
        ImGui::Text("材质：%s",
                    matComp.MaterialPtr
                        ? matComp.MaterialPtr->GetDebugName().c_str()
                        : "(null)");
        if (matComp.MaterialPtr) {
            ImGui::Text("  Albedo：%s",
                matComp.MaterialPtr->HasTexture(Material::Albedo)
                    ? "Checkerboard.png" : "(null)");
        }
    }

    ImGui::Separator();

    // 摄像机参数（通过 CameraComponent 控制）
    {
        ImGui::Text("摄像机 (CameraComponent)");
        if (!m_CameraEntity) {
            ImGui::TextDisabled("（场景中无相机实体）");
        } else {
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
        } // m_CameraEntity
    }

    ImGui::Separator();

    // 光照参数（由 ECS 组件管理）
    {
        ImGui::Text("光照参数（ECS 组件）");

        // 方向光：通过 DirectionalLightComponent + Transform 控制
        if (m_DirLightEntity) {
            auto &dlc = m_DirLightEntity.GetComponent<DirectionalLightComponent>();
            auto &tc = m_DirLightEntity.GetComponent<TransformComponent>();

            ImGui::Text("方向光");
            ImGui::ColorEdit4("颜色 + 强度", glm::value_ptr(dlc.Color));

            // 用欧拉角（度）编辑方向，内部存弧度
            glm::vec3 rotDeg = glm::degrees(tc.Rotation);
            if (ImGui::DragFloat3("旋转 (deg)", glm::value_ptr(rotDeg), 1.0f,
                                  -180.0f, 180.0f)) {
                tc.Rotation = glm::radians(rotDeg);
            }

            // 显示当前光线方向（前向向量）
            glm::vec3 forward = glm::quat(tc.Rotation) * glm::vec3(0.0f, 0.0f, -1.0f);
            ImGui::Text("光线方向: (%.2f, %.2f, %.2f)",
                        forward.x, forward.y, forward.z);
        }

        ImGui::Spacing();

        // 环境光：通过 AmbientLightComponent 控制
        if (m_AmbientLightEntity) {
            auto &alc = m_AmbientLightEntity.GetComponent<AmbientLightComponent>();

            ImGui::Text("环境光");
            ImGui::ColorEdit4("颜色 + 强度", glm::value_ptr(alc.Color));
        }
    }

    ImGui::Separator();

    // 统计信息
    auto meshView = m_Scene->Reg().view<MeshComponent>();
    auto scriptView = m_Scene->Reg().view<ScriptComponent>();
    ImGui::Text("场景 3D 实体数：%zu", meshView.size());
    ImGui::Text("场景脚本数：%zu", scriptView.size());
    ImGui::Text("FPS：%.1f", Application::Get().GetFPS());

    ImGui::Separator();

    // 垂直同步切换
    {
        auto &app = Application::Get();
        VsyncMode currentMode = app.GetWindow().GetVSync();

        const char *modeNames[] = {"Default", "ON (FIFO)", "OFF (Mailbox)"};
        int currentIndex = 0;
        if (currentMode == VsyncMode::ON)
            currentIndex = 1;
        else if (currentMode == VsyncMode::OFF)
            currentIndex = 2;

        if (ImGui::Combo("垂直同步", &currentIndex, modeNames, 3)) {
            VsyncMode newMode = VsyncMode::Default;
            if (currentIndex == 1)
                newMode = VsyncMode::ON;
            else if (currentIndex == 2)
                newMode = VsyncMode::OFF;
            app.SetPresentMode(newMode);
        }
    }

    // 重置按钮
    if (ImGui::Button("重置参数")) {
        tc.Translation = {0.0f, 0.0f, 0.0f};
        tc.Rotation = {0.0f, 0.0f, 0.0f};
        tc.Scale = {1.0f, 1.0f, 1.0f};
        mc.Color = {1.0f, 1.0f, 1.0f, 1.0f};

        // 重置相机
        if (m_CameraEntity) {
            auto &cameraComp = m_CameraEntity.GetComponent<CameraComponent>();
            auto &camera = cameraComp.CameraInstance;
            camera.SetMode(Camera::Mode::Orbit);
            camera.SetPerspective(60.0f, camera.GetAspect(), 0.1f, 100.0f);
            camera.SetTarget({0.0f, 0.0f, 0.0f});
            camera.SetOrbit(0.0f, 0.0f, 3.0f);
            cameraComp.Primary = true;
            cameraComp.FixedAspectRatio = false;
        }
    }

    ImGui::End();
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
        angle += ts.GetSeconds() * 1.2f; // 旋转速度
        auto &tc = entity.GetComponent<TransformComponent>();
        tc.Translation.x = std::cos(angle) * 1.8f;
        tc.Translation.z = std::sin(angle) * 1.8f;
        tc.Translation.y = 1.5f;
    };

    // 蓝色点光源：绕 Y 轴反方向旋转，半径 1.5，高度 0.5
    auto blueLightCallback = [angle = 0.0f](Timestep ts, Entity entity) mutable {
        angle -= ts.GetSeconds() * 0.8f; // 反向旋转，速度稍慢
        auto &tc = entity.GetComponent<TransformComponent>();
        tc.Translation.x = std::cos(angle) * 1.5f;
        tc.Translation.z = std::sin(angle) * 1.5f;
        tc.Translation.y = 0.5f;
    };

    // 红色光源脚本（旋转 + 空格切换颜色）
    auto redKeyCallback = [](Entity entity, KeyCode key, int /*repeatCount*/) -> bool {
        if (key == Key::Space) {
            auto &light = entity.GetComponent<PointLightComponent>();
            // 红色 ↔ 白色 切换，验证 ScriptComponent 按键回调
            if (light.Color.r > 0.9f && light.Color.g > 0.9f && light.Color.b > 0.9f) {
                light.Color = {1.0f, 0.2f, 0.2f, 1.0f};
            } else {
                light.Color = {1.0f, 1.0f, 1.0f, 1.0f};
            }
            return true; // 消费事件
        }
        return false;
    };

    if (m_RedLightEntity.HasComponent<ScriptComponent>()) {
        auto &sc = m_RedLightEntity.GetComponent<ScriptComponent>();
        sc.OnUpdate = std::move(redLightCallback);
        sc.OnKeyPressed = std::move(redKeyCallback);
    } else {
        ScriptComponent sc;
        sc.OnUpdate = std::move(redLightCallback);
        sc.OnKeyPressed = std::move(redKeyCallback);
        m_RedLightEntity.AddComponent<ScriptComponent>(std::move(sc));
    }

    // 蓝色光源脚本
    if (m_BlueLightEntity.HasComponent<ScriptComponent>()) {
        m_BlueLightEntity.GetComponent<ScriptComponent>().OnUpdate = std::move(blueLightCallback);
    } else {
        m_BlueLightEntity.AddComponent<ScriptComponent>(std::move(blueLightCallback));
    }
}

// ============================================================
// ImGuizmo：视口内 3D 变换 gizmo 渲染
// ============================================================
void ModelTestLayer::RenderImGuizmo() {
    Entity selected = m_HierarchyPanel.GetSelectedEntity();
    if (!selected || m_GizmoType == -1 || !m_CameraEntity) {
        return;
    }

    auto &cameraComp = m_CameraEntity.GetComponent<CameraComponent>();
    auto &camera = cameraComp.CameraInstance;

    glm::mat4 view = camera.GetView();
    // ImGuizmo 内部使用 OpenGL 约定（Y 向上为正），需要去掉 Vulkan 的 Y 翻转
    // （ImGui 绘制本身已正确适配 Vulkan 屏幕坐标，这里只是 3D 投影数学需要 OpenGL 风格）
    glm::mat4 projection = camera.GetProj();
    projection[1][1] *= -1.0f;

    auto &tc = selected.GetComponent<TransformComponent>();
    glm::mat4 transform = tc.GetTransform();

    ImGuizmo::SetOrthographic(false);
    // 画在前景 draw list（最顶层），不隶属于任何 ImGui 窗口，避免命中检测错位
    ImGuizmo::SetDrawlist(ImGui::GetForegroundDrawList());

    // gizmo 覆盖整个窗口（3D 场景渲染到整个 swapchain）
    auto &io = ImGui::GetIO();
    ImGuizmo::SetRect(0.0f, 0.0f, io.DisplaySize.x, io.DisplaySize.y);

    // 吸附参数
    float snap[3] = {m_SnapValue, m_SnapValue, m_SnapValue};
    float *snapPtr = m_UseSnap ? snap : nullptr;

    // 执行 gizmo 操作
    bool manipulated = ImGuizmo::Manipulate(
        glm::value_ptr(view),
        glm::value_ptr(projection),
        static_cast<ImGuizmo::OPERATION>(m_GizmoType),
        ImGuizmo::LOCAL,
        glm::value_ptr(transform),
        nullptr,
        snapPtr
        );

    // 如果用户拖拽了 gizmo，把结果写回 TransformComponent
    if (manipulated) {
        glm::vec3 translation, rotation, scale;
        ImGuizmo::DecomposeMatrixToComponents(
            glm::value_ptr(transform),
            glm::value_ptr(translation),
            glm::value_ptr(rotation),
            glm::value_ptr(scale)
            );
        tc.Translation = translation;
        // ImGuizmo 返回的旋转是欧拉角（度），内部存储弧度
        tc.Rotation = glm::radians(rotation);
        tc.Scale = scale;
    }
}

// ============================================================
// ImGuizmo：控制面板（提示 + 模式/吸附参数）
// ============================================================
void ModelTestLayer::RenderImGuizmoPanel() {
    ImGui::Text("Gizmo 快捷键：W=平移  E=旋转  R=缩放  Q=关闭  T=吸附");

    const char *modeStr = "关闭";
    switch (m_GizmoType) {
    case ImGuizmo::TRANSLATE: modeStr = "平移 (Translate)";
        break;
    case ImGuizmo::ROTATE: modeStr = "旋转 (Rotate)";
        break;
    case ImGuizmo::SCALE: modeStr = "缩放 (Scale)";
        break;
    default: modeStr = "关闭";
        break;
    }
    ImGui::Text("当前模式：%s", modeStr);
    ImGui::Checkbox("启用吸附 (Snap)", &m_UseSnap);
    if (m_UseSnap) {
        ImGui::DragFloat("吸附步长", &m_SnapValue, 0.05f, 0.01f, 10.0f);
    }
}

// ============================================================
// 场景文件操作：保存 / 加载 / 新建
// ============================================================

void ModelTestLayer::SaveScene() {
    if (!m_Scene) {
        return;
    }

    std::string filepath = FileDialogs::SaveFile("GE Scene (*.scene)\0*.scene\0All Files (*.*)\0*.*\0");
    if (filepath.empty()) {
        return;
    }

    // 如果没有序列化器就创建一个（保存不需要 VulkanDevice）
    if (!m_SceneSerializer) {
        m_SceneSerializer = std::make_unique<SceneSerializer>(m_Scene.get());
    } else {
        // 更新场景指针（防止场景被替换过）
        // 注意：SceneSerializer 没有提供 SetScene 方法，这里直接重建
        // 但会丢失已加载的资源。保存操作不影响资源，所以重建也没关系
        m_SceneSerializer = std::make_unique<SceneSerializer>(m_Scene.get());
    }

    m_SceneSerializer->Serialize(filepath);
}

void ModelTestLayer::LoadScene() {
    std::string filepath = FileDialogs::OpenFile("GE Scene (*.scene)\0*.scene\0All Files (*.*)\0*.*\0");
    if (filepath.empty()) {
        return;
    }

    auto &ctx = Application::GetVulkanContext();
    auto &device = ctx.GetDevice();
    auto &cache = device.GetResourceCache();

    // 先重置所有实体引用，避免悬空
    m_ModelEntity = {};
    m_CameraEntity = {};
    m_RedLightEntity = {};
    m_BlueLightEntity = {};
    m_DirLightEntity = {};
    m_AmbientLightEntity = {};

    // 如果场景不存在，先创建
    if (!m_Scene) {
        m_Scene = std::make_unique<Scene>();
    }

    // 创建新的序列化器（带设备，用于加载网格资源；纹理/材质使用全局管理器）
    m_SceneSerializer = std::make_unique<SceneSerializer>(m_Scene.get(), &device);

    if (!m_SceneSerializer->Deserialize(filepath)) {
        GE_CORE_WARN("加载场景失败: {0}", filepath);
        return;
    }

    // 更新层级面板上下文
    m_HierarchyPanel.SetContext(m_Scene.get());
    m_HierarchyPanel.SetSelectedEntity({});

    // 尝试重新绑定相机实体
    RebindCameraEntity();

    // 尝试重新绑定方向光和环境光实体
    RebindLightEntities();
}

void ModelTestLayer::NewScene() {
    // 重置所有实体引用
    m_ModelEntity = {};
    m_CameraEntity = {};
    m_RedLightEntity = {};
    m_BlueLightEntity = {};
    m_DirLightEntity = {};
    m_AmbientLightEntity = {};

    // 创建新场景
    m_Scene = std::make_unique<Scene>();

    // 创建新的序列化器（清空旧网格资源；纹理/材质由全局管理器管理）
    auto &ctx = Application::GetVulkanContext();
    auto &device = ctx.GetDevice();
    m_SceneSerializer = std::make_unique<SceneSerializer>(m_Scene.get(), &device);

    // 更新层级面板
    m_HierarchyPanel.SetContext(m_Scene.get());
    m_HierarchyPanel.SetSelectedEntity({});
}

void ModelTestLayer::RebindCameraEntity() {
    if (!m_Scene) {
        m_CameraEntity = {};
        return;
    }

    // 遍历场景，查找带有 CameraComponent 且 Primary=true 的实体
    auto &reg = m_Scene->Reg();
    auto view = reg.view<CameraComponent>();

    for (auto entityHandle : view) {
        Entity entity(entityHandle, m_Scene.get());
        const auto &cc = entity.GetComponent<CameraComponent>();
        if (cc.Primary) {
            m_CameraEntity = entity;
            return;
        }
    }

    // 没找到主相机，就取第一个有 CameraComponent 的
    for (auto entityHandle : view) {
        m_CameraEntity = Entity(entityHandle, m_Scene.get());
        return;
    }

    // 没有相机
    m_CameraEntity = {};
}

void ModelTestLayer::RebindLightEntities() {
    m_DirLightEntity = {};
    m_AmbientLightEntity = {};

    if (!m_Scene) {
        return;
    }

    auto &reg = m_Scene->Reg();

    // 找第一个方向光实体
    {
        auto view = reg.view<DirectionalLightComponent>();
        if (view.begin() != view.end()) {
            m_DirLightEntity = Entity(*view.begin(), m_Scene.get());
        }
    }

    // 找第一个环境光实体
    {
        auto view = reg.view<AmbientLightComponent>();
        if (view.begin() != view.end()) {
            m_AmbientLightEntity = Entity(*view.begin(), m_Scene.get());
        }
    }
}

} // namespace GE
