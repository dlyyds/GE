//
// 场景序列化测试层：创建带相机/光照/实体的场景，验证 .scene 文件的保存与加载
//

#include "SceneLayer.h"

#include "GE/Core/Application.h"
#include "GE/Scene/Components.h"
#include "GE/Utils/PlatformUtils.h"

#include "imgui.h"

#include <glm/gtc/matrix_transform.hpp>

namespace GE {

SceneLayer::SceneLayer() : Layer("SceneLayer") {
}

SceneLayer::~SceneLayer() = default;

void SceneLayer::OnAttach() {
    constexpr const char *kDefaultScene = "assets/scenes/test.scene";

    // 从文件加载默认场景（网格/纹理/材质由全局管理器加载持有）
    if (!LoadSceneFromFile(kDefaultScene)) {
        GE_CORE_WARN("SceneLayer: 启动加载默认场景失败: {0}", kDefaultScene);
    }
}

void SceneLayer::OnDetach() {
    m_CameraEntity = {};
    m_HierarchyPanel.SetContext(nullptr);
    m_Scene.reset();
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

    // 复用已有序列化器（序列化器与场景总是同生同灭，其 m_Scene 必为当前场景；
    // 网格/纹理/材质由全局管理器持有，序列化器自身不拥有资源，无需重建）
    if (!m_SceneSerializer) {
        m_SceneSerializer = std::make_unique<SceneSerializer>(m_Scene.get());
    }
    m_SceneSerializer->Serialize(filepath);
}

bool SceneLayer::LoadSceneFromFile(std::string_view filepath) {
    // 先重置实体引用，避免悬空
    m_CameraEntity = {};

    // 如果场景不存在，先创建
    if (!m_Scene) {
        m_Scene = std::make_unique<Scene>();
    }

    // 创建序列化器（纹理/材质/网格由全局管理器加载，无需 device）
    m_SceneSerializer = std::make_unique<SceneSerializer>(m_Scene.get());

    if (!m_SceneSerializer->Deserialize(filepath)) {
        return false;
    }

    // 更新层级面板上下文
    m_HierarchyPanel.SetContext(m_Scene.get());
    m_HierarchyPanel.SetSelectedEntity({});

    // 尝试重新绑定相机实体
    RebindCameraEntity();
    return true;
}

void SceneLayer::LoadScene() {
    std::string filepath = FileDialogs::OpenFile("GE Scene (*.scene)\0*.scene\0All Files (*.*)\0*.*\0");
    if (filepath.empty()) {
        return;
    }

    if (!LoadSceneFromFile(filepath)) {
        GE_CORE_WARN("SceneLayer: 加载场景失败: {0}", filepath);
    }
}

void SceneLayer::NewScene() {
    // 重置实体引用
    m_CameraEntity = {};

    // 创建新场景
    m_Scene = std::make_unique<Scene>();

    // 创建新的序列化器
    m_SceneSerializer = std::make_unique<SceneSerializer>(m_Scene.get());

    // 更新层级面板
    m_HierarchyPanel.SetContext(m_Scene.get());
    m_HierarchyPanel.SetSelectedEntity({});
}

void SceneLayer::RebindCameraEntity() {
    // 交由场景查找主相机（Primary=true，否则取第一个相机实体）
    m_CameraEntity = m_Scene ? m_Scene->GetPrimaryCameraEntity() : Entity{};
}

} // namespace GE