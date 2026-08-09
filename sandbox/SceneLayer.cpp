//
// 场景序列化测试层：创建带相机/光照/实体的场景，验证 .scene 文件的保存与加载
//

#include "SceneLayer.h"

#include "GE/Core/Application.h"
#include "GE/Render/Renderer.h"
#include "GE/Render/Renderer3D.h"
#include "GE/Render/TextureManager.h"
#include "GE/Render/MaterialManager.h"
#include "GE/Scene/Components.h"
#include "GE/Utils/PlatformUtils.h"

#include "imgui.h"

#include <glm/gtc/matrix_transform.hpp>

namespace GE {

SceneLayer::SceneLayer() : Layer("SceneLayer") {
}

SceneLayer::~SceneLayer() = default;

void SceneLayer::OnAttach() {
    auto &ctx = Application::GetVulkanContext();
    auto &device = ctx.GetDevice();

    // ── 1. 加载纹理（全局 TextureManager 去重持有） ─────────────────────
    auto &texMgr = Renderer::GetTextureManager();
    m_CheckerTexture = texMgr.Load("assets/textures/Checkerboard.png",
                                   vk::Format::eR8G8B8A8Srgb,
                                   vk::Filter::eNearest,
                                   vk::Filter::eNearest);
    m_NormalTexture = texMgr.Load("assets/textures/normal_stone.jpg",
                                  vk::Format::eR8G8B8A8Unorm,
                                  vk::Filter::eLinear,
                                  vk::Filter::eLinear);

    // ── 2. 注册带纹理的材质（含标量参数） ──────────────────────────────
    auto &matMgr = Renderer::GetMaterialManager();
    auto mat = std::make_unique<Material>();
    mat->SetTexture(Material::Albedo, m_CheckerTexture);
    mat->SetTexture(Material::Normal, m_NormalTexture);
    mat->SetFloat("shininess", 64.0f);
    mat->SetFloat("specularStrength", 0.6f);
    mat->SetDebugName("SceneLayer_CubeMat");
    m_TexturedMaterial = matMgr.Register("SceneLayer_CubeMat", std::move(mat));

    // ── 3. 内置网格 ────────────────────────────────────────────────────
    m_CubeMesh = Mesh::CreateBuiltin(device, "cube");
    m_SphereMesh = Mesh::CreateBuiltin(device, "sphere");

    // ── 4. 创建场景与实体 ──────────────────────────────────────────────
    m_Scene = std::make_unique<Scene>();

    // 相机
    m_CameraEntity = m_Scene->CreateEntity("MainCamera");
    auto &cameraComp = m_CameraEntity.AddComponent<CameraComponent>();
    cameraComp.CameraInstance.SetPerspective(60.0f, 16.0f / 9.0f, 0.1f, 100.0f);
    cameraComp.CameraInstance.SetMode(Camera::Mode::Orbit);
    cameraComp.CameraInstance.SetOrbit(0.0f, 0.0f, 5.0f); // 距离目标 5 个单位
    cameraComp.CameraInstance.SetTarget({0.0f, 0.0f, 0.0f});

    // 方向光
    auto dirLight = m_Scene->CreateEntity("DirectionalLight");
    dirLight.GetComponent<TransformComponent>().Rotation =
        glm::vec3(glm::radians(-45.0f), glm::radians(30.0f), 0.0f);
    dirLight.AddComponent<DirectionalLightComponent>(
        glm::vec4(1.0f, 0.95f, 0.85f, 1.2f));

    // 环境光
    auto ambient = m_Scene->CreateEntity("AmbientLight");
    ambient.AddComponent<AmbientLightComponent>(glm::vec4(0.3f, 0.3f, 0.35f, 1.0f));

    // 带纹理材质立方体
    auto cube1 = m_Scene->CreateEntity("TexturedCube");
    cube1.AddComponent<MeshComponent>(m_CubeMesh.get());
    cube1.AddComponent<MaterialComponent>(m_TexturedMaterial);

    // 纯色立方体（用 MeshComponent 的 color 做 tint，不绑定材质）
    auto cube2 = m_Scene->CreateEntity("ColoredCube");
    cube2.GetComponent<TransformComponent>().Translation = {1.5f, 0.0f, 0.0f};
    cube2.AddComponent<MeshComponent>(m_CubeMesh.get(), glm::vec4(0.2f, 0.6f, 1.0f, 1.0f));

    // 球体（复用带法线贴图的材质）
    auto sphere = m_Scene->CreateEntity("Sphere");
    sphere.GetComponent<TransformComponent>().Translation = {-1.5f, 0.0f, 0.0f};
    sphere.AddComponent<MeshComponent>(m_SphereMesh.get());
    sphere.AddComponent<MaterialComponent>(m_TexturedMaterial);

    // 点光源
    auto pointLight = m_Scene->CreateEntity("PointLight");
    pointLight.GetComponent<TransformComponent>().Translation = {2.0f, 2.0f, 2.0f};
    pointLight.AddComponent<PointLightComponent>(
        glm::vec4(1.0f, 0.2f, 0.2f, 1.5f), // 红色，强度 1.5
        0.4f                              // 半径倒数
        );

    // 设置场景层级面板上下文
    m_HierarchyPanel.SetContext(m_Scene.get());
}

void SceneLayer::OnDetach() {
    m_CameraEntity = {};
    m_HierarchyPanel.SetContext(nullptr);
    m_Scene.reset();
    m_TexturedMaterial = nullptr; // 由全局 MaterialManager 管理生命周期
    m_CheckerTexture = nullptr;   // 由全局 TextureManager 管理生命周期
    m_NormalTexture = nullptr;    // 由全局 TextureManager 管理生命周期
    m_CubeMesh.reset();
    m_SphereMesh.reset();
}

void SceneLayer::OnUpdate(Timestep &ts) {
    auto extent = Application::GetSwapchain().GetExtent();
    float aspect = static_cast<float>(extent.width) / static_cast<float>(extent.height);

    // 同步场景视口尺寸
    m_Scene->OnViewportResize(extent.width, extent.height);

    // 场景中没有相机实体时，使用默认视角清屏
    if (!m_CameraEntity) {
        glm::mat4 view(1.0f);
        glm::mat4 projection = glm::perspective(glm::radians(60.0f), aspect, 0.1f, 100.0f);
        projection[1][1] *= -1.0f; // Vulkan Y 翻转
        glm::vec3 cameraPos{0.0f, 0.0f, 3.0f};
        glm::vec4 clearColor{0.1f, 0.1f, 0.15f, 1.0f};
        m_Scene->OnUpdate3D(ts, view, projection, cameraPos, clearColor);
        return;
    }

    // 从相机组件获取视图与投影矩阵
    auto &cameraComp = m_CameraEntity.GetComponent<CameraComponent>();
    auto &camera = cameraComp.CameraInstance;

    // 同步宽高比
    if (!cameraComp.FixedAspectRatio) {
        camera.SetAspect(aspect);
    }

    glm::mat4 view = camera.GetView();
    glm::mat4 projection = camera.GetProj();
    glm::vec3 cameraPos = camera.GetPosition();
    glm::vec4 clearColor{0.1f, 0.1f, 0.15f, 1.0f};

    m_Scene->OnUpdate3D(ts, view, projection, cameraPos, clearColor);
}

void SceneLayer::OnEvent(Event &event) {
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

void SceneLayer::OnImGuiRender() {
    // 场景层级 + 属性面板
    m_HierarchyPanel.OnImGuiRender();

    ImGui::Begin("SceneLayer");
    ImGui::Text("场景序列化测试（Scene + SceneSerializer + Renderer3D）");
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

    // ---- 场景统计信息 ----
    if (m_Scene) {
        auto meshView = m_Scene->Reg().view<MeshComponent>();
        auto lightView = m_Scene->Reg().view<PointLightComponent>();
        ImGui::Text("3D 实体数：%zu", meshView.size());
        ImGui::Text("点光源数：%zu", lightView.size());
        ImGui::Text("FPS：%.1f", Application::Get().GetFPS());
        ImGui::TextDisabled("提示：先在左侧 Hierarchy/Properties 中调整实体，再保存/加载验证");
    }

    ImGui::End();
}

// ============================================================
// 场景文件操作：保存 / 加载 / 新建
// ============================================================

void SceneLayer::SaveScene() {
    if (!m_Scene) {
        return;
    }

    std::string filepath = FileDialogs::SaveFile("GE Scene (*.scene)\0*.scene\0All Files (*.*)\0*.*\0");
    if (filepath.empty()) {
        return;
    }

    // 保存不需要 VulkanDevice，只需持有场景指针
    m_SceneSerializer = std::make_unique<SceneSerializer>(m_Scene.get());
    m_SceneSerializer->Serialize(filepath);
}

void SceneLayer::LoadScene() {
    std::string filepath = FileDialogs::OpenFile("GE Scene (*.scene)\0*.scene\0All Files (*.*)\0*.*\0");
    if (filepath.empty()) {
        return;
    }

    auto &ctx = Application::GetVulkanContext();
    auto &device = ctx.GetDevice();

    // 先重置实体引用，避免悬空
    m_CameraEntity = {};

    // 如果场景不存在，先创建
    if (!m_Scene) {
        m_Scene = std::make_unique<Scene>();
    }

    // 创建带设备的序列化器（用于加载网格资源；纹理/材质使用全局管理器）
    m_SceneSerializer = std::make_unique<SceneSerializer>(m_Scene.get(), &device);

    if (!m_SceneSerializer->Deserialize(filepath)) {
        GE_CORE_WARN("SceneLayer: 加载场景失败: {0}", filepath);
        return;
    }

    // 更新层级面板上下文
    m_HierarchyPanel.SetContext(m_Scene.get());
    m_HierarchyPanel.SetSelectedEntity({});

    // 尝试重新绑定相机实体
    RebindCameraEntity();
}

void SceneLayer::NewScene() {
    // 重置实体引用
    m_CameraEntity = {};

    // 创建新场景
    m_Scene = std::make_unique<Scene>();

    // 创建新的序列化器
    auto &ctx = Application::GetVulkanContext();
    auto &device = ctx.GetDevice();
    m_SceneSerializer = std::make_unique<SceneSerializer>(m_Scene.get(), &device);

    // 更新层级面板
    m_HierarchyPanel.SetContext(m_Scene.get());
    m_HierarchyPanel.SetSelectedEntity({});
}

void SceneLayer::RebindCameraEntity() {
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

} // namespace GE