//
// 场景层实现：只负责场景渲染（离屏视口 + 相机）与文件操作（保存/加载/新建）。
// 面板（层级/资源）已拆分为独立 Layer，共享 EditorContext。
//

#include "SceneLayer.h"

#include "GE/Core/Application.h"
#include "GE/Render/Renderer.h"
#include "GE/Scene/Components.h"
#include "GE/Scene/SceneSerializer.h"
#include "GE/Utils/PlatformUtils.h"

#include "imgui.h"
#include "ImGuizmo.h"
#include "Render/Renderer2D.h"
#include "Render/Renderer3D.h"

#include "GizmoController.h"

#include <glm/gtc/matrix_transform.hpp>

namespace GE {

SceneLayer::SceneLayer(std::shared_ptr<EditorContext> context) : Layer("SceneLayer"), m_Context(std::move(context)) {
}

SceneLayer::~SceneLayer() = default;

void SceneLayer::OnAttach() {
    constexpr const char *kDefaultScene = "assets/scenes/2.scene";

    // 从文件加载默认场景（网格/纹理/材质由全局管理器加载持有）
    if (!LoadSceneFromFile(kDefaultScene)) {
        GE_CORE_WARN("SceneLayer: 启动加载默认场景失败: {0}", kDefaultScene);
    }
}

void SceneLayer::OnDetach() {
    m_Context->CameraEntity = {};
    m_Viewport.reset(); // 释放离屏渲染目标（GPU 资源）
    m_Context->Scene.reset();
}

void SceneLayer::OnUpdate(Timestep &ts) {
    // 视口窗口尺寸（上一帧由 OnImGuiRender 记录；首帧为 0 时跳过离屏渲染）
    uint32_t vpW = static_cast<uint32_t>(m_ViewportSize.x);
    uint32_t vpH = static_cast<uint32_t>(m_ViewportSize.y);
    if (vpW == 0 || vpH == 0) {
        return;
    }

    // 确保离屏渲染目标存在，且尺寸与视口一致（变化时重建）
    if (!m_Viewport) {
        m_Viewport = std::make_unique<SceneViewport>();
    }
    // 首次调用 Create 创建离屏目标；之后尺寸变化时用 OnResize 重建。
    // 重建会重新分配 GPU 图像 + 深度 + 采样器，开销大；拖拽视口时一帧一变，
    // 若每帧重建会刷屏日志并浪费资源。故对重建限流（约 4 次/秒）。
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

    // 同步场景视口尺寸
    m_Context->Scene->OnViewportResize(vpW, vpH);

    // 把本帧 3D 场景（网格 + 精灵）渲染进离屏视口目标
    Renderer::Get3DRenderer().SetRenderTarget(m_Viewport->GetRenderTarget());
    Renderer::Get2DRenderer().SetRenderTarget(m_Viewport->GetRenderTarget());

    // 场景中没有相机实体时，使用默认视角清屏
    if (!m_Context->CameraEntity) {
        glm::mat4 view(1.0f);
        glm::mat4 projection = glm::perspective(glm::radians(60.0f), aspect, 0.1f, 100.0f);
        projection[1][1] *= -1.0f; // Vulkan Y 翻转
        glm::vec3 cameraPos{0.0f, 0.0f, 3.0f};
        glm::vec4 clearColor{0.1f, 0.1f, 0.15f, 1.0f};
        m_Context->Scene->OnUpdate3D(ts, view, projection, cameraPos, clearColor);
        Renderer::Get3DRenderer().SetRenderTarget(nullptr);
        Renderer::Get2DRenderer().SetRenderTarget(nullptr);
        return;
    }

    // 从相机组件获取视图与投影矩阵
    auto &cameraComp = m_Context->CameraEntity.GetComponent<CameraComponent>();
    auto &camera = cameraComp.CameraInstance;

    // 同步宽高比（使用视口窗口比例）
    if (!cameraComp.FixedAspectRatio) {
        camera.SetAspect(aspect);
    }

    glm::mat4 view = camera.GetView();
    glm::mat4 projection = camera.GetProj();
    glm::vec3 cameraPos = camera.GetPosition();
    glm::vec4 clearColor{0.1f, 0.1f, 0.15f, 1.0f};

    m_Context->Scene->OnUpdate3D(ts, view, projection, cameraPos, clearColor);

    // 复位为 swapchain 目标（默认）
    Renderer::Get3DRenderer().SetRenderTarget(nullptr);
    Renderer::Get2DRenderer().SetRenderTarget(nullptr);
}

void SceneLayer::OnEvent(Event &event) {
    if (m_Context->Scene) {
        // 仅当鼠标悬停在 Scene 视口内、且光标不在 gizmo 上时才让相机接收输入，
        // 避免拖 gizmo 时相机跟着转、或误触发相机视角。这里只告知 Scene 本次
        // 是否允许相机输入，实际把事件路由给主相机的逻辑由 Scene 内部完成。
        const bool cameraActive = m_SceneWindowHovered && !(m_Gizmo && ImGuizmo::IsOver());
        m_Context->Scene->SetProcessCameraInput(cameraActive);

        if (!event.Handled) {
            m_Context->Scene->OnEvent(event);
        }
    }
}

void SceneLayer::OnImGuiRender() {
    // 根上下文取一次停靠目标 ID（与 DockSpaceLayer 中 GetID("MainDockspace") 一致）
    if (m_DockSpaceID == 0) {
        m_DockSpaceID = ImGui::GetID("MainDockspace");
    }

    // ---- 场景视口窗口（显示离屏渲染的 3D 场景）----
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
    ImGui::SetNextWindowDockID(m_DockSpaceID, ImGuiCond_FirstUseEver);
    ImGui::Begin("Scene");
    {
        // 记录鼠标是否悬停在 Scene 窗口内（供下帧 OnEvent 判断是否把输入给相机）
        m_SceneWindowHovered = ImGui::IsWindowHovered();

        // 记录视口尺寸（供下帧 OnUpdate 离屏渲染使用）
        ImVec2 avail = ImGui::GetContentRegionAvail();
        m_ViewportSize = {avail.x, avail.y};

        // 显示离屏渲染结果
        if (m_Viewport && m_Viewport->GetImGuiDescriptorSet() != VK_NULL_HANDLE) {
            ImGui::Image(m_Viewport->GetImGuiDescriptorSet(), avail);
        }

        // 在 Scene 窗口绘制范围内叠加变换 gizmo（ImGuizmo 须在此窗口内调用）。
        // 用 GetItemRectMin() 取图像自身的屏幕左上角 —— 它精确落在 Scene 窗口
        // 内容区（标题栏下方），若用 GetWindowPos() 会因标题栏偏移使 gizmo 偏高。
        if (m_Gizmo && m_Context->CameraEntity) {
            auto &cameraComp = m_Context->CameraEntity.GetComponent<CameraComponent>();
            const ImVec2 imagePos = ImGui::GetItemRectMin();
            m_Gizmo->Render(cameraComp.CameraInstance, glm::vec2(imagePos.x, imagePos.y), m_ViewportSize);
        }
    }
    ImGui::End();
    ImGui::PopStyleVar();

    ImGui::SetNextWindowDockID(m_DockSpaceID, ImGuiCond_FirstUseEver);
    ImGui::Begin("SceneLayer");
    ImGui::Text("场景（Scene + SceneSerializer + Renderer3D）");
    ImGui::Separator();

    // 场景的 保存 / 加载 / 新建 已移到顶部菜单「文件」中

    // ---- 场景统计信息 ----
    if (m_Context->Scene) {
        auto meshView = m_Context->Scene->Reg().view<MeshComponent>();
        auto lightView = m_Context->Scene->Reg().view<PointLightComponent>();
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

void SceneLayer::SetGizmoController(std::unique_ptr<GizmoController> gizmo) {
    m_Gizmo = std::move(gizmo);
}

void SceneLayer::SaveScene() {
    if (!m_Context->Scene) {
        return;
    }

    std::string filepath = FileDialogs::SaveFile("GE Scene (*.scene)\0*.scene\0All Files (*.*)\0*.*\0");
    if (filepath.empty()) {
        return;
    }

    // 复用已有序列化器（序列化器与场景总是同生同灭，其 m_Scene 必为当前场景；
    // 网格/纹理/材质由全局管理器持有，序列化器自身不拥有资源，无需重建）
    if (!m_Context->Serializer) {
        m_Context->Serializer = std::make_unique<SceneSerializer>(m_Context->Scene.get());
    }
    m_Context->Serializer->Serialize(filepath);
}

bool SceneLayer::LoadSceneFromFile(std::string_view filepath) {
    // 先重置实体引用，避免悬空
    m_Context->CameraEntity = {};

    // 如果场景不存在，先创建
    if (!m_Context->Scene) {
        m_Context->Scene = std::make_unique<Scene>();
    }

    // 创建序列化器（纹理/材质/网格由全局管理器加载，无需 device）
    m_Context->Serializer = std::make_unique<SceneSerializer>(m_Context->Scene.get());

    if (!m_Context->Serializer->Deserialize(filepath.data())) {
        return false;
    }

    // 重新绑定主相机实体（Primary=true，否则取第一个相机实体）
    m_Context->CameraEntity = m_Context->Scene->GetPrimaryCameraEntity();
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
    m_Context->CameraEntity = {};

    // 创建新场景
    m_Context->Scene = std::make_unique<Scene>();

    // 创建新的序列化器
    m_Context->Serializer = std::make_unique<SceneSerializer>(m_Context->Scene.get());
}

} // namespace GE