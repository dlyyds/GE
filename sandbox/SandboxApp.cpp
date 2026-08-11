#include <GE.h>
#include <GE/Core/EntryPoint.h>

#include "Render/Renderer.h"
#include "Render/Renderer3D.h"
#include "Render/TextureManager.h"
#include "Render/MeshManager.h"
#include "Render/MaterialManager.h"
#include "Render/Material.h"

#include "Scene/Scene.h"
#include "Scene/Components.h"

#include "imgui.h"

#include <glm/gtc/matrix_transform.hpp>

#include <cmath>
#include <memory>

namespace GE {

/// 自发光（Emissive）冒烟测试层：基于场景系统（Scene / Entity / Components）。
///
/// 场景布局（左 → 右）：
///   左：无自发光贴图（控制组，应被光照正常照亮、不发光）
///   中：棋盘格自发光贴图（应发出棋盘格光）
///   右：纯色暖橙自发光（应整体发出橙色光）
///
/// 相机为 Orbit 模式，绕目标点缓慢旋转；三个立方体各自自转。
/// 降低环境光后，自发光物体仍发光，差异更明显。
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
        auto &texMgr = Renderer::GetTextureManager();
        auto &matMgr = Renderer::GetMaterialManager();

        // 纹理：白色反照率让自发光对比更明显；棋盘格作自发光贴图；暖橙作纯色发光
        // （由全局 TextureManager 持有，不拥有）
        m_WhiteTex    = texMgr.GetSolidColor(glm::vec4(1.0f));
        m_EmissiveTex = texMgr.Load("assets/textures/Checkerboard.png");
        m_OrangeTex   = texMgr.GetSolidColor(glm::vec4(1.0f, 0.5f, 0.1f, 1.0f));

        // 材质注册到全局 MaterialManager（生命周期随 Renderer），
        // MaterialComponent 仅持裸指针引用，因此材质必须比场景存活更久。
        // 1. 无自发光（控制组）
        auto noEmissive = std::make_unique<Material>();
        noEmissive->SetTexture(Material::Albedo, m_WhiteTex);
        m_MatNoEmissive = matMgr.Register("Sandbox_NoEmissive", std::move(noEmissive));

        // 2. 棋盘格自发光贴图 + 强度
        auto checker = std::make_unique<Material>();
        checker->SetTexture(Material::Albedo, m_WhiteTex);
        checker->SetTexture(Material::Emissive, m_EmissiveTex);
        checker->SetFloat("emissiveStrength", m_CheckerStrength);
        m_MatChecker = matMgr.Register("Sandbox_Checker", std::move(checker));

        // 3. 纯色暖橙自发光
        auto orange = std::make_unique<Material>();
        orange->SetTexture(Material::Albedo, m_WhiteTex);
        orange->SetTexture(Material::Emissive, m_OrangeTex);
        orange->SetFloat("emissiveStrength", 1.0f);
        m_MatOrange = matMgr.Register("Sandbox_Orange", std::move(orange));

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

        // ── 三个立方体（MeshComponent + MaterialComponent） ───────────────
        auto makeCube = [&](const char *name, const glm::vec3 &pos, Material *mat) {
            Entity e = m_Scene->CreateEntity(name);
            e.GetComponent<TransformComponent>().Translation = pos;
            e.AddComponent<MeshComponent>(meshMgr.GetBuiltin("cube"));
            e.AddComponent<MaterialComponent>(mat);
            return e;
        };
        m_CubeEntities[0] = makeCube("Cube_NoEmissive", {-2.0f, 0.5f, 0.0f}, m_MatNoEmissive);
        m_CubeEntities[1] = makeCube("Cube_Checker",    { 0.0f, 0.5f, 0.0f}, m_MatChecker);
        m_CubeEntities[2] = makeCube("Cube_Orange",     { 2.0f, 0.5f, 0.0f}, m_MatOrange);
    }

    void OnDetach() override {
        // 网格/纹理由全局管理器持有；材质由 MaterialManager 持有，均无需释放。
        // 仅释放场景（材质引用随之失效，但材质本身仍存活于 MaterialManager）。
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

        // ── 三个立方体各自自转，便于观察自发光在不同面 ──────────────────
        for (int i = 0; i < 3; i++) {
            m_CubeEntities[i].GetComponent<TransformComponent>().Rotation.y +=
                ts.GetSeconds() * 0.5f;
        }

        // ── 交给场景系统渲染（脚本 / 物理 / 光源收集 / 网格材质） ───────
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
        ImGui::Begin("Emissive Test");
        ImGui::Text("FPS: %.1f", Application::Get().GetFPS());
        ImGui::Text("实体数：%zu (3 立方体 + 相机 + 2 光源)",
                    m_Scene ? m_Scene->Reg().size() : 0);
        ImGui::Separator();
        ImGui::SliderFloat("环境光强度", &m_Ambient, 0.0f, 0.5f);
        ImGui::SliderFloat("棋盘格自发光强度", &m_CheckerStrength, 0.0f, 5.0f);
        ImGui::Checkbox("启用中心立方体自发光", &m_EnableCheckerEmissive);
        ImGui::Separator();
        ImGui::Text("布局：左=无自发光(控制) | 中=棋盘格 | 右=纯色");
        ImGui::TextDisabled("把环境光调低，自发光物体仍发光，对比更明显");
        ImGui::End();

        // 把面板中的参数实时写入中心立方体的材质（下一帧生效）
        m_MatChecker->SetFloat("emissiveStrength", m_CheckerStrength);
        m_MatChecker->SetTexture(Material::Emissive,
                                 m_EnableCheckerEmissive ? m_EmissiveTex : nullptr);
    }

private:
    // 场景（本层所有）
    std::unique_ptr<Scene> m_Scene;

    // 网格 / 纹理由全局管理器持有，仅存非拥有指针
    Texture *m_WhiteTex    = nullptr;
    Texture *m_EmissiveTex = nullptr;
    Texture *m_OrangeTex   = nullptr;

    // 材质由 MaterialManager 持有，仅存非拥有指针
    Material *m_MatNoEmissive = nullptr;
    Material *m_MatChecker    = nullptr;
    Material *m_MatOrange     = nullptr;

    // 实体句柄（指向 m_Scene 内实体）
    Entity m_CameraEntity;
    Entity m_AmbientEntity;
    Entity m_CubeEntities[3];

    // 相机旋转角
    float m_Angle = 0.0f;

    // 面板参数
    float m_Ambient              = 0.15f;
    float m_CheckerStrength      = 1.5f;
    bool  m_EnableCheckerEmissive = true;
};

/// 沙盒应用：仅运行引擎主循环 + 自发光冒烟测试层。
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