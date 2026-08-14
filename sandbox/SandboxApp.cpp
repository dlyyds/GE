#include <GE.h>
#include <GE/Core/EntryPoint.h>

#include "Render/Renderer.h"
#include "Render/Renderer3D.h"
#include "Render/AssetManager.h"
#include "Render/MeshManager.h"

#include "Scene/Scene.h"
#include "Scene/Components.h"

#include "imgui.h"

#include <glm/gtc/matrix_transform.hpp>

#include <memory>

namespace GE {

/// 沙盒冒烟测试层：基于场景系统（Scene / Entity / Components）。
///
/// 场景布局：三个立方体（材质由子网格绑定，内置几何体无材质 → 白色 fallback）
/// 相机为 Orbit 模式，绕目标点缓慢旋转；三个立方体各自自转。
class Sandbox3DLayer : public Layer {
public:
    Sandbox3DLayer() : Layer("Sandbox3DLayer") {}

    // ========================================================================
    // 生命周期
    // ========================================================================

    void OnAttach() override {
        // 创建场景（物理世界 + 实体注册表）
        m_Scene = std::make_unique<Scene>();

        auto &meshMgr = Renderer::GetMeshManager();

        // ── 相机实体（Orbit 模式，绕目标点旋转） ─────────────────────────
        auto camera = m_Scene->CreateEntity("Camera");
        auto &cc = camera.AddComponent<CameraComponent>();
        cc.Primary = true;
        cc.CameraInstance.SetMode(Camera::Mode::Orbit);
        cc.CameraInstance.SetTarget(glm::vec3(0.0f, 0.5f, 0.0f));
        cc.CameraInstance.SetOrbit(0.0f, 20.0f, 6.0f);
        m_CameraEntity = camera;

        // ── 方向光实体（旋转决定照射方向，-60° 绕 X 轴：从上前方照下） ──
        auto dirLight = m_Scene->CreateEntity("DirectionalLight");
        dirLight.GetComponent<TransformComponent>().Rotation =
            glm::vec3(glm::radians(-60.0f), 0.0f, 0.0f);
        dirLight.AddComponent<DirectionalLightComponent>(glm::vec4(1.0f, 1.0f, 1.0f, 1.0f));

        // ── 环境光实体（强度由 ImGui 面板调节） ─────────────────────────
        auto ambLight = m_Scene->CreateEntity("AmbientLight");
        ambLight.AddComponent<AmbientLightComponent>(
            glm::vec4(m_Ambient, m_Ambient, m_Ambient, 1.0f));
        m_AmbientEntity = ambLight;

        // ── 三个立方体（MeshComponent，材质走子网格绑定 / 白色 fallback） ──
        auto makeCube = [&](const char *name, const glm::vec3 &pos) {
            Entity e = m_Scene->CreateEntity(name);
            e.GetComponent<TransformComponent>().Translation = pos;
            e.AddComponent<MeshComponent>(meshMgr.GetBuiltin("cube"));
            return e;
        };
        m_CubeEntities[0] = makeCube("Cube_Left",  {-2.0f, 0.5f, 0.0f});
        m_CubeEntities[1] = makeCube("Cube_Center", { 0.0f, 0.5f, 0.0f});
        m_CubeEntities[2] = makeCube("Cube_Right",  { 2.0f, 0.5f, 0.0f});
    }

    void OnDetach() override {
        // 网格由全局管理器持有，仅释放场景即可
        m_Scene.reset();
    }

    // ========================================================================
    // 更新 / 渲染
    // ========================================================================

    void OnUpdate(Timestep &ts) override {
        auto &win = Application::Get().GetWindow();
        float w = static_cast<float>(win.GetWidth());
        float h = static_cast<float>(win.GetHeight());
        if (w == 0.0f || h == 0.0f) {
            return; // 窗口最小化时跳过
        }
        float aspect = w / h;

        // ── 相机：Orbit 绕目标旋转，同步宽高比 ──────────────────────────
        auto &camComp = m_CameraEntity.GetComponent<CameraComponent>();
        m_Angle += ts.GetSeconds() * 0.3f;
        camComp.CameraInstance.SetAspect(aspect);
        camComp.CameraInstance.SetOrbit(m_Angle,
                                        camComp.CameraInstance.GetPhi(),
                                        camComp.CameraInstance.GetDistance());

        // ── 环境光：把 ImGui 面板参数写入环境光组件 ────────────────────
        m_AmbientEntity.GetComponent<AmbientLightComponent>().Color =
            glm::vec4(m_Ambient, m_Ambient, m_Ambient, 1.0f);

        // ── 三个立方体各自自转 ──────────────────────────────────────────
        for (int i = 0; i < 3; i++) {
            m_CubeEntities[i].GetComponent<TransformComponent>().Rotation.y +=
                ts.GetSeconds() * 0.5f;
        }

        // ── 交给场景系统渲染（光源收集 / 网格子网格绘制） ───────────────
        glm::mat4 view = camComp.CameraInstance.GetView();
        glm::mat4 proj = camComp.CameraInstance.GetProj();
        glm::vec3 camPos = camComp.CameraInstance.GetPosition();
        glm::vec4 clearColor{0.05f, 0.05f, 0.1f, 1.0f};
        m_Scene->OnUpdate3D(ts, view, proj, camPos, clearColor);
    }

    void OnEvent(Event &) override {}

    // ========================================================================
    // ImGui 控制面板
    // ========================================================================

    void OnImGuiRender() override {
        ImGui::Begin("Sandbox");
        ImGui::Text("FPS: %.1f", Application::Get().GetFPS());
        ImGui::Text("实体数：%zu (3 立方体 + 相机 + 2 光源)",
                    m_Scene ? m_Scene->Reg().size() : 0);
        ImGui::Separator();
        ImGui::SliderFloat("环境光强度", &m_Ambient, 0.0f, 0.5f);
        ImGui::End();
    }

private:
    // 场景（本层所有）
    std::unique_ptr<Scene> m_Scene;

    // 实体句柄（指向 m_Scene 内实体）
    Entity m_CameraEntity;
    Entity m_AmbientEntity;
    Entity m_CubeEntities[3];

    // 相机旋转角
    float m_Angle = 0.0f;

    // 面板参数
    float m_Ambient = 0.15f;
};

/// 沙盒应用：仅运行引擎主循环 + 冒烟测试层。
class Sandbox : public Application {
public:
    explicit Sandbox(ApplicationCommandLineArgs args) : Application("Sandbox", args) {
        GE_PROFILE_FUNCTION();
        PushLayer(std::make_shared<Sandbox3DLayer>());
    }

    ~Sandbox() override = default;
};

Application *CreateApplication(const ApplicationCommandLineArgs args) { return new Sandbox(args); }

} // namespace GE