#include <GE.h>
#include <GE/Core/EntryPoint.h>

#include "Render/Renderer.h"
#include "Render/Renderer3D.h"
#include "Render/TextureManager.h"
#include "Render/MeshManager.h"
#include "Render/Material.h"

#include "imgui.h"

#include <glm/gtc/matrix_transform.hpp>

#include <cmath>
#include <memory>

namespace GE {

/// 自发光（Emissive）冒烟测试层：渲染 3 个立方体对比自发光效果。
///
/// 布局（左 → 右）：
///   左：无自发光贴图（控制组，应被光照正常照亮、不发光）
///   中：棋盘格自发光贴图（应发出棋盘格光）
///   右：纯色暖橙自发光（应整体发出橙色光）
///
/// 降低环境光后，自发光物体仍会发光，差异更明显。
class Sandbox3DLayer : public Layer {
public:
    Sandbox3DLayer() : Layer("Sandbox3DLayer") {}

    // ========================================================================
    // 生命周期
    // ========================================================================

    void OnAttach() override {
        // 网格：内置立方体（由全局 MeshManager 持有，不拥有）
        auto &meshMgr = Renderer::GetMeshManager();
        m_Cube = meshMgr.GetBuiltin("cube");

        // 纹理：白色反照率让自发光对比更明显；棋盘格作自发光贴图；暖橙作纯色发光
        auto &texMgr = Renderer::GetTextureManager();
        m_WhiteTex    = texMgr.GetSolidColor(glm::vec4(1.0f));
        m_EmissiveTex = texMgr.Load("assets/textures/Checkerboard.png");
        m_OrangeTex   = texMgr.GetSolidColor(glm::vec4(1.0f, 0.5f, 0.1f, 1.0f));

        // 材质 1：无自发光（控制组）
        m_MatNoEmissive = std::make_unique<Material>();
        m_MatNoEmissive->SetTexture(Material::Albedo, m_WhiteTex);

        // 材质 2：棋盘格自发光贴图 + 强度
        m_MatChecker = std::make_unique<Material>();
        m_MatChecker->SetTexture(Material::Albedo, m_WhiteTex);
        m_MatChecker->SetTexture(Material::Emissive, m_EmissiveTex);
        m_MatChecker->SetFloat("emissiveStrength", m_CheckerStrength);

        // 材质 3：纯色暖橙自发光
        m_MatOrange = std::make_unique<Material>();
        m_MatOrange->SetTexture(Material::Albedo, m_WhiteTex);
        m_MatOrange->SetTexture(Material::Emissive, m_OrangeTex);
        m_MatOrange->SetFloat("emissiveStrength", 1.0f);

        // 三个立方体对应的材质（左中右）
        m_MaterialForIndex[0] = m_MatNoEmissive.get();
        m_MaterialForIndex[1] = m_MatChecker.get();
        m_MaterialForIndex[2] = m_MatOrange.get();
    }

    void OnDetach() override {
        // 网格 / 纹理由全局管理器持有，仅置空引用；材质归本层所有，需释放
        m_Cube = nullptr;
        m_WhiteTex = nullptr;
        m_EmissiveTex = nullptr;
        m_OrangeTex = nullptr;
        m_MatNoEmissive.reset();
        m_MatChecker.reset();
        m_MatOrange.reset();
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

        // 相机绕原点缓慢旋转，便于从多角度观察自发光
        m_Angle += ts.GetSeconds() * 0.3f;
        glm::vec3 camPos = glm::vec3(std::cos(m_Angle) * 5.0f, 2.0f, std::sin(m_Angle) * 5.0f);
        glm::mat4 view = glm::lookAt(camPos, glm::vec3(0.0f, 0.5f, 0.0f), glm::vec3(0.0f, 1.0f, 0.0f));
        glm::mat4 proj = glm::perspective(glm::radians(60.0f), aspect, 0.1f, 100.0f);
        proj[1][1] *= -1.0f; // Vulkan Y 翻转

        auto &r3d = Renderer::Get3DRenderer();
        // 环境光强度可调（调低后自发光物体仍发光，对比明显）
        r3d.GetLightParams().ambient = glm::vec4(m_Ambient, m_Ambient, m_Ambient, 1.0f);
        r3d.GetLightParams().dirLightColor = glm::vec4(1.0f, 1.0f, 1.0f, 1.0f);

        r3d.BeginScene(view, proj, camPos, glm::vec4(0.05f, 0.05f, 0.1f, 1.0f));

        // 三个立方体并排（左：无自发光 / 中：棋盘格自发光 / 右：纯色自发光）
        const glm::vec3 positions[3] = {
            {-2.0f, 0.5f, 0.0f}, {0.0f, 0.5f, 0.0f}, {2.0f, 0.5f, 0.0f}};
        for (int i = 0; i < 3; i++) {
            glm::mat4 model = glm::translate(glm::mat4(1.0f), positions[i]);
            model = glm::rotate(model, m_Angle * 0.5f, glm::vec3(0.0f, 1.0f, 0.0f));
            r3d.DrawMesh(model, m_Cube, m_MaterialForIndex[i]);
        }

        r3d.EndScene();
    }

    void OnEvent(Event &) override {}

    // ========================================================================
    // ImGui 控制面板
    // ========================================================================

    void OnImGuiRender() override {
        ImGui::Begin("Emissive Test");
        ImGui::Text("FPS: %.1f", Application::Get().GetFPS());
        ImGui::Separator();
        ImGui::SliderFloat("环境光强度", &m_Ambient, 0.0f, 0.5f);
        ImGui::SliderFloat("棋盘格自发光强度", &m_CheckerStrength, 0.0f, 5.0f);
        ImGui::Checkbox("启用中心立方体自发光", &m_EnableCheckerEmissive);
        ImGui::Separator();
        ImGui::Text("布局：左=无自发光(控制) | 中=棋盘格 | 右=纯色");
        ImGui::TextDisabled("把环境光调低，自发光物体仍发光，对比更明显");
        ImGui::End();

        // 把面板中的参数实时写入材质（下一帧生效）
        m_MatChecker->SetFloat("emissiveStrength", m_CheckerStrength);
        m_MatChecker->SetTexture(Material::Emissive,
                                 m_EnableCheckerEmissive ? m_EmissiveTex : nullptr);
    }

private:
    // 网格 / 纹理（由全局管理器持有，不拥有）
    Mesh    *m_Cube        = nullptr;
    Texture *m_WhiteTex    = nullptr;
    Texture *m_EmissiveTex = nullptr;
    Texture *m_OrangeTex   = nullptr;

    // 材质（本层所有）
    std::unique_ptr<Material> m_MatNoEmissive;
    std::unique_ptr<Material> m_MatChecker;
    std::unique_ptr<Material> m_MatOrange;

    // 三个立方体对应的材质索引（左中右）
    Material *m_MaterialForIndex[3] = {};

    // 相机旋转角
    float m_Angle = 0.0f;

    // 面板参数
    float m_Ambient          = 0.15f;
    float m_CheckerStrength  = 1.5f;
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