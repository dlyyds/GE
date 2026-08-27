//
// 场景层实现：只负责场景渲染（离屏视口 + 相机）与文件操作（保存/加载/新建）。
// 面板（层级/资源）已拆分为独立 Layer，共享 EditorContext。
//

#include "SceneLayer.h"

#include "GE/Core/Application.h"
#include "GE/Events/MouseEvent.h"
#include "GE/Render/Renderer.h"
#include "GE/Render/AssetManager.h"
#include "GE/Render/MeshManager.h"
#include "GE/Render/Mesh.h"
#include "GE/Scene/Components.h"
#include "GE/Scene/SceneSerializer.h"
#include "GE/Utils/PlatformUtils.h"

#include "imgui.h"
#include "ImGuizmo.h"
#include "Render/Renderer2D.h"
#include "Render/Renderer3D.h"

#include "GizmoController.h"

// 编译期开关：true = 启动时从代码程序化构建默认场景；false = 从 .scene 文件加载。
// 无需代码路径时，编辑器默认从文件加载（false）。
#define GE_EDITOR_BUILD_SCENE_FROM_CODE 0

#include <glm/gtc/matrix_transform.hpp>

namespace GE {

namespace {

/**
 * @brief 世界坐标 → 视口屏幕坐标（ImGui 像素，Y 向下）。
 *
 * 与 GizmoController 同约定：传入 OpenGL 语义投影（投影 [1][1] 未做 Vulkan 翻转），
 * 屏幕映射 sy = (0.5 - ndc.y*0.5)*h 与 ImGuizmo worldToPos 的 y=1-y 一致，
 * 保证包围盒线与画面 / gizmo 精确对齐。
 *
 * @return false = 角点在相机背面（clip.w<=0），调用方按边跳过，避免投影发散
 */
bool ProjectWorldToScreen(const glm::mat4 &viewProjGL, const glm::vec3 &world,
                          const glm::vec2 &origin, const glm::vec2 &size, glm::vec2 &out) {
    const glm::vec4 clip = viewProjGL * glm::vec4(world, 1.0f);
    if (clip.w <= 0.0f) {
        return false; // 相机背面
    }
    const glm::vec2 ndc{clip.x / clip.w, clip.y / clip.w};
    out = glm::vec2(origin.x + (0.5f + ndc.x * 0.5f) * size.x,
                    origin.y + (0.5f - ndc.y * 0.5f) * size.y);
    return true;
}

} // namespace

SceneLayer::SceneLayer(std::shared_ptr<EditorContext> context) : Layer("SceneLayer"), m_Context(std::move(context)) {
}

SceneLayer::~SceneLayer() = default;

void SceneLayer::OnAttach() {
#if GE_EDITOR_BUILD_SCENE_FROM_CODE
    // 从代码程序化构建默认场景（网格/纹理/材质由全局管理器持有）
    BuildDefaultSceneFromCode();
#else
    const std::string kDefaultScene = std::string(AssetPaths::Scenes) + "/2.scene";

    // 从文件加载默认场景（网格/纹理/材质由全局管理器加载持有）
    const std::string defaultScenePath =
        Renderer::GetAssetManager().ResolvePath(kDefaultScene).string();
    if (!LoadSceneFromFile(defaultScenePath)) {
        GE_CORE_WARN("SceneLayer: 启动加载默认场景失败: {0}", defaultScenePath);
    }
#endif
}

// 从代码程序化构建一个用于测试 OBJ+MTL 加载的默认场景：相机 + 方向光 + 环境光 +
// Datsun 280Z 车模。网格/纹理/材质均由全局管理器加载持有，场景组件仅持非拥有指针。
void SceneLayer::BuildDefaultSceneFromCode() {
    // 先重置实体引用，避免悬空
    m_Context->CameraEntity = {};
    m_Context->Scene = std::make_unique<Scene>();

    // 相机实体（Orbit 模式，绕场景中心观测）
    auto camera = m_Context->Scene->CreateEntity("Camera");
    auto &cc = camera.AddComponent<CameraComponent>();
    cc.Primary = true;
    cc.CameraInstance.SetMode(Camera::Mode::Orbit);
    cc.CameraInstance.SetTarget(glm::vec3(0.0f, 0.5f, 0.0f));
    cc.CameraInstance.SetOrbit(0.0f, 25.0f, 8.0f);
    m_Context->CameraEntity = camera;

    // 方向光实体（-60° 绕 X 轴：从上前方照下）。强度提至 2.0，给金属提供
    // 一个明显的锐利高光（金属无漫反射，靠高光与环境显形）。
    auto dirLight = m_Context->Scene->CreateEntity("DirectionalLight");
    dirLight.GetComponent<TransformComponent>().SetRotationEuler(
        glm::vec3(glm::radians(-60.0f), 0.0f, 0.0f));
    dirLight.AddComponent<DirectionalLightComponent>(glm::vec4(1.0f, 1.0f, 1.0f, 2.0f));

    // 环境光实体
    auto ambLight = m_Context->Scene->CreateEntity("AmbientLight");
    ambLight.AddComponent<AmbientLightComponent>(glm::vec4(0.15f, 0.15f, 0.15f, 1.0f));

    // 环境（天空盒背景 + IBL 环境光，统一由环境名推导）
    ambLight.AddComponent<EnvironmentComponent>("DaySkyHDRI065B");

    // 加载 Datsun 280Z 车模（OBJ + 自动解析 MTL 材质：颜色/法线/自发光贴图）
    Mesh *carMesh = Renderer::GetAssetManager().LoadMesh("models/car/source/Datsun_280Z.obj");
    if (carMesh) {
        auto car = m_Context->Scene->CreateEntity("Datsun_280Z");
        car.GetComponent<TransformComponent>().Translation = {0.0f, 0.0f, 0.0f};
        car.AddComponent<MeshRendererComponent>(carMesh);
    } else {
        GE_CORE_WARN("SceneLayer: 车模加载失败");
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
        // 仅当鼠标悬停在 Scene 视口内时才把输入事件转发给场景（影响脚本 + 相机）；
        // 窗口尺寸变化等非输入事件始终会转发给场景。
        const bool inViewport = m_SceneWindowHovered;
        // 相机额外要求未在拖 gizmo，避免拖 gizmo 时相机跟着转。
        const bool cameraActive = inViewport && !(m_Gizmo && ImGuizmo::IsOver());
        m_Context->Scene->SetProcessCameraInput(cameraActive);

        if (!event.Handled) {
            // 记录在视口内按下的鼠标按键并跟踪释放：用于把「拖出视口后松开」的释放事件
            // 仍回传给相机，避免相机内部按键状态（m_LeftDown）卡在按下态，导致之后在
            // 视口内移动鼠标时相机持续旋转（此时鼠标其实已松开）。
            bool releaseStartedInViewport = false;
            EventDispatcher disp(event);
            disp.Dispatch<MouseButtonPressedEvent>([&](MouseButtonPressedEvent &e) {
                if (inViewport) {
                    m_ViewportCapturedButtons |= (1u << e.GetMouseButton());
                }
                return false;
            });
            disp.Dispatch<MouseButtonReleasedEvent>([&](MouseButtonReleasedEvent &e) {
                uint16_t bit = 1u << e.GetMouseButton();
                releaseStartedInViewport = (m_ViewportCapturedButtons & bit) != 0;
                m_ViewportCapturedButtons &= ~bit;
                return false;
            });

            // 未悬停时直接不转发输入事件给场景
            if (!inViewport && event.IsInCategory(EventCategoryInput)) {
                // 例外：松开的是视口内按下的按键时，仍回传释放事件，让相机按键状态复位。
                if (releaseStartedInViewport) {
                    m_Context->Scene->SetProcessCameraInput(true);
                    m_Context->Scene->OnEvent(event);
                }
                return;
            }
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

            // 屏幕原点 = 视口图像左上角（gizmo 与包围盒叠加共用；须图像已绘制）
            const ImVec2 imagePos = ImGui::GetItemRectMin();

            // 变换 gizmo（ImGuizmo 须在此窗口内调用）。用 GetItemRectMin() 取图像
            // 自身的屏幕左上角 —— 精确落在 Scene 窗口内容区（标题栏下方），若用
            // GetWindowPos() 会因标题栏偏移使 gizmo 偏高。
            if (m_Gizmo && m_Context->CameraEntity) {
                auto &cameraComp = m_Context->CameraEntity.GetComponent<CameraComponent>();
                m_Gizmo->Render(cameraComp.CameraInstance, glm::vec2(imagePos.x, imagePos.y), m_ViewportSize);
            }

            // 包围盒线框叠加：世界 AABB（静态盒 + 蒙皮绑定盒，与视锥剔除用同一盒）
            if (m_ShowBounds && m_Context->CameraEntity) {
                DrawWorldBounds(glm::vec2(imagePos.x, imagePos.y));
            }
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
        auto meshView = m_Context->Scene->Reg().view<MeshRendererComponent>();
        auto lightView = m_Context->Scene->Reg().view<PointLightComponent>();
        ImGui::Text("3D 实体数：%zu", meshView.size());
        ImGui::Text("点光源数：%zu", lightView.size());
        ImGui::Text("FPS：%.1f", Application::Get().GetFPS());

        ImGui::Separator();
        // 视锥剔除粒度开关：整网格级 / 子网格级（基于 SubMesh::aabb 细剔除）
        const char *kCullingNames[] = {"整网格（Mesh）", "子网格（SubMesh）"};
        int cull = static_cast<int>(m_Context->Scene->GetCullingMode());
        if (ImGui::Combo("视锥剔除", &cull, kCullingNames, IM_ARRAYSIZE(kCullingNames))) {
            m_Context->Scene->SetCullingMode(static_cast<Scene::CullingMode>(cull));
        }

        // 包围盒线框调试开关（叠加在视口上：静态盒灰、蒙皮绑定盒红）
        ImGui::Checkbox("显示包围盒", &m_ShowBounds);

        ImGui::TextDisabled("提示：先在左侧 Hierarchy/Properties 中调整实体，再保存/加载验证");
    }

    ImGui::End();
}

// ============================================================
// 场景文件操作：保存 / 加载 / 新建
// ============================================================

void SceneLayer::DrawWorldBounds(const glm::vec2 &imagePos) {
    if (!m_Context->Scene || !m_Context->CameraEntity) {
        return;
    }
    auto &cc = m_Context->CameraEntity.GetComponent<CameraComponent>();
    const Camera &camera = cc.CameraInstance;

    // 还原 OpenGL 投影（渲染/剔除用 Vulkan Y 翻转投影；ImGuizmo 同款还原保证对齐）
    glm::mat4 projGL = camera.GetProj();
    projGL[1][1] *= -1.0f;
    const glm::mat4 viewProjGL = projGL * camera.GetView();

    const glm::vec2 origin{imagePos.x, imagePos.y};
    const glm::vec2 size{m_ViewportSize.x, m_ViewportSize.y};
    ImDrawList *dl = ImGui::GetWindowDrawList();

    // 立方体 12 条边：0/1/2/3 = z=min 面，4/5/6/7 = z=max 面
    static const int kEdges[12][2] = {
        {0, 1}, {1, 3}, {3, 2}, {2, 0},
        {4, 5}, {5, 7}, {7, 6}, {6, 4},
        {0, 4}, {1, 5}, {2, 6}, {3, 7},
    };

    // 画一个世界空间 AABB 的 12 条边线框（供网格盒与实体盒共用）
    const auto drawWorldAabb = [&](const AABB &world, ImU32 color) {
        const glm::vec3 c[8] = {
            {world.min.x, world.min.y, world.min.z},
            {world.max.x, world.min.y, world.min.z},
            {world.min.x, world.max.y, world.min.z},
            {world.max.x, world.max.y, world.min.z},
            {world.min.x, world.min.y, world.max.z},
            {world.max.x, world.min.y, world.max.z},
            {world.min.x, world.max.y, world.max.z},
            {world.max.x, world.max.y, world.max.z},
        };
        glm::vec2 scr[8];
        bool front[8] = {};
        for (int i = 0; i < 8; ++i) {
            front[i] = ProjectWorldToScreen(viewProjGL, c[i], origin, size, scr[i]);
        }
        for (int e = 0; e < 12; ++e) {
            const int a = kEdges[e][0], b = kEdges[e][1];
            // 边跨相机背面则不画（避免投影发散）；两端都在正面才连线
            if (front[a] && front[b]) {
                dl->AddLine(ImVec2(scr[a].x, scr[a].y), ImVec2(scr[b].x, scr[b].y), color, 1.2f);
            }
        }
    };

    // 1) 网格级盒：仅静态网格灰蓝（蒙皮绑定盒覆盖不了动画变形，且红显易误导，
    //    其可靠表示统一由下方的 BoundingBoxComponent 黄盒承担）
    auto meshView = m_Context->Scene->Reg().view<TransformComponent, MeshRendererComponent>();
    const ImU32 meshColor = ImGui::ColorConvertFloat4ToU32(ImVec4(0.55f, 0.60f, 0.70f, 1.0f));
    for (auto entity : meshView) {
        const auto &tc = meshView.get<TransformComponent>(entity);
        const auto &mc = meshView.get<MeshRendererComponent>(entity);
        if (!mc.MeshPtr) {
            continue;
        }
        // 蒙皮实体不画（无可靠盒可示），改由用户手动摆放的黄盒表示
        if (m_Context->Scene->Reg().try_get<SkinComponent>(entity)) {
            continue;
        }
        const AABB &local = mc.MeshPtr->GetAABB();
        if (!local.IsValid()) {
            continue;
        }
        drawWorldAabb(local.Transformed(tc.GetWorldMatrix()), meshColor);
    }

    // 2) 实体级手动摆放盒（BoundingBoxComponent）用黄叠出，便于对照剔除覆盖范围
    auto boundsView = m_Context->Scene->Reg().view<TransformComponent, BoundingBoxComponent>();
    const ImU32 bbColor = ImGui::ColorConvertFloat4ToU32(ImVec4(1.0f, 0.80f, 0.10f, 1.0f));
    for (auto entity : boundsView) {
        const auto &tc = boundsView.get<TransformComponent>(entity);
        const auto &bb = boundsView.get<BoundingBoxComponent>(entity);
        if (!bb.IsValid()) {
            continue; // 未摆放，不参与剔除也不画
        }
        AABB local;
        local.min = bb.minCorner();
        local.max = bb.maxCorner();
        drawWorldAabb(local.Transformed(tc.GetWorldMatrix()), bbColor);
    }
}

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

    // 序列化器无状态、操作完即弃，按需创建为局部变量即可；
    // 网格/纹理/材质由全局管理器持有，序列化器自身不拥有资源
    SceneSerializer serializer(m_Context->Scene.get());
    serializer.Serialize(filepath);
}

bool SceneLayer::LoadSceneFromFile(std::string_view filepath) {
    // 先重置实体引用，避免悬空
    m_Context->CameraEntity = {};

    // 如果场景不存在，先创建
    if (!m_Context->Scene) {
        m_Context->Scene = std::make_unique<Scene>();
    }

    // 序列化器无状态、按需创建为局部变量（纹理/材质/网格由全局管理器加载，无需 device）
    SceneSerializer serializer(m_Context->Scene.get());

    if (!serializer.Deserialize(filepath.data())) {
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

    // 创建新场景（序列化器无状态，仅在保存/加载时按需创建局部变量）
    m_Context->Scene = std::make_unique<Scene>();
}

} // namespace GE