#include "SandboxLayer.h"

#include "GE/Core/Application.h"
#include "GE/Render/Renderer.h"
#include "GE/Render/TextureManager.h"
#include "GE/Render/MeshManager.h"
#include "GE/Render/MaterialManager.h"
#include "GE/Scene/Scene.h"
#include "GE/Scene/Components.h"

#include "imgui.h"
#include "Render/Renderer3D.h"

#include <glm/gtc/matrix_transform.hpp>

namespace GE {

SandboxLayer::SandboxLayer() : Layer("SandboxLayer") {
}

SandboxLayer::~SandboxLayer() = default;

// ============================================================
// 生命周期
// ============================================================

void SandboxLayer::OnAttach() {
    // 创建独立测试场景
    m_Scene = std::make_unique<Scene>();

    auto &meshMgr = Renderer::GetMeshManager();
    auto &texMgr = Renderer::GetTextureManager();
    auto &matMgr = Renderer::GetMaterialManager();

    // 纹理：白色反照率让自发光对比更明显；棋盘格作自发光贴图；暖橙作纯色发光
    m_WhiteTex = texMgr.GetSolidColor(glm::vec4(1.0f));
    m_EmissiveTex = texMgr.Load("assets/textures/Checkerboard.png");
    m_OrangeTex = texMgr.GetSolidColor(glm::vec4(1.0f, 0.5f, 0.1f, 1.0f));

    // 材质注册到全局 MaterialManager（生命周期随 Renderer），
    // MaterialComponent 仅持裸指针引用，材质必须比场景存活更久。
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

    // ── 相机实体（Orbit 模式，绕目标点旋转） ────────────────────────────
    auto camera = m_Scene->CreateEntity("Camera");
    auto &cc = camera.AddComponent<CameraComponent>();
    cc.Primary = true;
    cc.CameraInstance.SetMode(Camera::Mode::Orbit);
    cc.CameraInstance.SetTarget(glm::vec3(0.0f, 0.5f, 0.0f));
    cc.CameraInstance.SetOrbit(0.0f, 20.0f, 6.0f);
    m_CameraEntity = camera;

    // ── 方向光实体（-60° 绕 X 轴：从上前方照下） ────────────────────────
    auto dirLight = m_Scene->CreateEntity("DirectionalLight");
    dirLight.GetComponent<TransformComponent>().Rotation =
        glm::vec3(glm::radians(-60.0f), 0.0f, 0.0f);
    dirLight.AddComponent<DirectionalLightComponent>(glm::vec4(1.0f, 1.0f, 1.0f, 1.0f));

    // ── 环境光实体（强度由面板调节） ────────────────────────────────────
    auto ambLight = m_Scene->CreateEntity("AmbientLight");
    ambLight.AddComponent<AmbientLightComponent>(
        glm::vec4(m_Ambient, m_Ambient, m_Ambient, 1.0f));
    m_AmbientEntity = ambLight;

    // ── 三个立方体（MeshComponent + MaterialComponent） ──────────────────
    auto makeCube = [&](const char *name, const glm::vec3 &pos, Material *mat) {
        Entity e = m_Scene->CreateEntity(name);
        e.GetComponent<TransformComponent>().Translation = pos;
        e.AddComponent<MeshComponent>(meshMgr.GetBuiltin("cube"));
        e.AddComponent<MaterialComponent>(mat);
        return e;
    };
    m_CubeEntities[0] = makeCube("Cube_NoEmissive", {-2.0f, 0.5f, 0.0f}, m_MatNoEmissive);
    m_CubeEntities[1] = makeCube("Cube_Checker", {0.0f, 0.5f, 0.0f}, m_MatChecker);
    m_CubeEntities[2] = makeCube("Cube_Orange", {2.0f, 0.5f, 0.0f}, m_MatOrange);
}

void SandboxLayer::OnDetach() {
    // 网格/纹理由全局管理器持有，材质由 MaterialManager 持有，均无需释放。
    // 释放离屏渲染目标（GPU 资源）与测试场景。
    m_Viewport.reset();
    m_Scene.reset();
}

// ============================================================
// 更新 / 渲染
// ============================================================

void SandboxLayer::OnUpdate(Timestep &ts) {
    // 视口窗口尺寸（上帧 OnImGuiRender 记录；首帧为 0 时跳过）
    uint32_t vpW = static_cast<uint32_t>(m_ViewportSize.x);
    uint32_t vpH = static_cast<uint32_t>(m_ViewportSize.y);
    if (vpW == 0 || vpH == 0) {
        return;
    }

    // 确保离屏渲染目标存在，且尺寸与视口一致（变化时重建）
    if (!m_Viewport) {
        m_Viewport = std::make_unique<SceneViewport>();
    }
    if (!m_Viewport->GetRenderTarget()) {
        m_Viewport->Create(Renderer::GetVulkanContext().GetDevice(), vpW, vpH);
    } else {
        m_ResizeCooldown += ts.GetSeconds();
        if (m_ResizeCooldown >= 0.25f) {
            m_Viewport->OnResize(vpW, vpH);
            m_ResizeCooldown = 0.0f;
        }
    }
    if (!m_Viewport->GetRenderTarget()) {
        return; // 目标尚未创建成功
    }

    float aspect = static_cast<float>(vpW) / static_cast<float>(vpH);

    // 相机：Orbit 绕目标旋转，同步宽高比
    auto &camComp = m_CameraEntity.GetComponent<CameraComponent>();
    m_Angle += ts.GetSeconds() * 0.3f;
    camComp.CameraInstance.SetAspect(aspect);
    camComp.CameraInstance.SetOrbit(m_Angle,
                                    camComp.CameraInstance.GetPhi(),
                                    camComp.CameraInstance.GetDistance());

    // 环境光：把面板参数写入环境光组件
    m_AmbientEntity.GetComponent<AmbientLightComponent>().Color =
        glm::vec4(m_Ambient, m_Ambient, m_Ambient, 1.0f);

    // 三个立方体各自自转，便于观察自发光在不同面
    for (int i = 0; i < 3; i++) {
        m_CubeEntities[i].GetComponent<TransformComponent>().Rotation.y +=
            ts.GetSeconds() * 0.5f;
    }

    // 把测试场景渲染进离屏视口，再交给场景系统驱动
    Renderer::Get3DRenderer().SetRenderTarget(m_Viewport->GetRenderTarget());
    m_Scene->OnUpdate3D(ts,
                        camComp.CameraInstance.GetView(),
                        camComp.CameraInstance.GetProj(),
                        camComp.CameraInstance.GetPosition(),
                        glm::vec4(0.05f, 0.05f, 0.1f, 1.0f));
    Renderer::Get3DRenderer().SetRenderTarget(nullptr);
}

void SandboxLayer::OnEvent(Event &) {
}

// ============================================================
// ImGui 渲染
// ============================================================

void SandboxLayer::OnImGuiRender() {
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));

    // 停靠进主 DockSpace（与 DockSpaceLayer 中 GetID("MainDockspace") 一致）
    if (m_DockSpaceID == 0) {
        m_DockSpaceID = ImGui::GetID("MainDockspace");
    }
    ImGui::SetNextWindowDockID(m_DockSpaceID, ImGuiCond_FirstUseEver);

    // 测试视口窗口：显示离屏渲染的 3D 场景
    ImGui::Begin("Sandbox");
    {
        ImVec2 avail = ImGui::GetContentRegionAvail();
        m_ViewportSize = {avail.x, avail.y};

        if (m_Viewport && m_Viewport->GetImGuiDescriptorSet() != VK_NULL_HANDLE) {
            ImGui::Image(m_Viewport->GetImGuiDescriptorSet(), avail);
        }
    }
    ImGui::End();
    ImGui::PopStyleVar();

    // 控制面板
    ImGui::SetNextWindowDockID(m_DockSpaceID, ImGuiCond_FirstUseEver);
    ImGui::Begin("Sandbox Controls");
    {
        ImGui::Text("FPS: %.1f", Application::Get().GetFPS());
        ImGui::Separator();
        ImGui::SliderFloat("环境光强度", &m_Ambient, 0.0f, 0.5f);
        ImGui::SliderFloat("棋盘格自发光强度", &m_CheckerStrength, 0.0f, 5.0f);
        ImGui::Checkbox("启用中心立方体自发光", &m_EnableCheckerEmissive);
        ImGui::Separator();
        ImGui::Text("布局：左=无自发光(控制) | 中=棋盘格 | 右=纯色");
        ImGui::TextDisabled("把环境光调低，自发光物体仍发光，对比更明显");
    }
    ImGui::End();

    // 把面板参数实时写入中心立方体的材质（下一帧生效）
    m_MatChecker->SetFloat("emissiveStrength", m_CheckerStrength);
    m_MatChecker->SetTexture(Material::Emissive,
                             m_EnableCheckerEmissive ? m_EmissiveTex : nullptr);
}

} // namespace GE