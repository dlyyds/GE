//
// 场景层实现：只负责场景渲染（离屏视口 + 相机）与文件操作（保存/加载/新建）。
// 面板（层级/资源）已拆分为独立 Layer，共享 EditorContext。
//

#include "SceneLayer.h"

#include "GE/Core/Application.h"
#include "GE/Events/MouseEvent.h"
#include "GE/Render/Renderer.h"
#include "GE/Render/MeshManager.h"
#include "GE/Render/TextureManager.h"
#include "GE/Render/MaterialManager.h"
#include "GE/Render/Material.h"
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
#define GE_EDITOR_BUILD_SCENE_FROM_CODE 1

#include <glm/gtc/matrix_transform.hpp>

namespace GE {

SceneLayer::SceneLayer(std::shared_ptr<EditorContext> context) : Layer("SceneLayer"), m_Context(std::move(context)) {
}

SceneLayer::~SceneLayer() = default;

void SceneLayer::OnAttach() {
#if GE_EDITOR_BUILD_SCENE_FROM_CODE
    // 从代码程序化构建默认场景（网格/纹理/材质由全局管理器持有）
    BuildDefaultSceneFromCode();
#else
    constexpr const char *kDefaultScene = "assets/scenes/2.scene";

    // 从文件加载默认场景（网格/纹理/材质由全局管理器加载持有）
    if (!LoadSceneFromFile(kDefaultScene)) {
        GE_CORE_WARN("SceneLayer: 启动加载默认场景失败: {0}", kDefaultScene);
    }
#endif
}

// 从代码程序化构建一个默认场景：相机 + 方向光 + 环境光 + 若干立方体。
// 网格/纹理/材质均由全局管理器加载持有，场景组件仅持非拥有指针。
void SceneLayer::BuildDefaultSceneFromCode() {
    // 先重置实体引用，避免悬空
    m_Context->CameraEntity = {};
    m_Context->Scene = std::make_unique<Scene>();

    auto &meshMgr = Renderer::GetMeshManager();
    auto &texMgr = Renderer::GetTextureManager();
    auto &matMgr = Renderer::GetMaterialManager();

    // 三个基础材质：各自用纯色 Albedo 纹理区分颜色（注册到全局 MaterialManager）
    const char *matNames[3] = {"Editor_Red", "Editor_Green", "Editor_Blue"};
    const glm::vec4 colors[3] = {
        glm::vec4(0.8f, 0.2f, 0.2f, 1.0f),
        glm::vec4(0.2f, 0.8f, 0.2f, 1.0f),
        glm::vec4(0.2f, 0.4f, 0.9f, 1.0f),
    };
    Material *mats[3];
    for (int i = 0; i < 3; ++i) {
        auto mat = std::make_unique<Material>();
        mat->SetTexture(Material::Albedo, texMgr.GetSolidColor(colors[i]));
        mats[i] = matMgr.Register(matNames[i], std::move(mat));
    }

    // 相机实体（Orbit 模式，绕场景中心观测）
    auto camera = m_Context->Scene->CreateEntity("Camera");
    auto &cc = camera.AddComponent<CameraComponent>();
    cc.Primary = true;
    cc.CameraInstance.SetMode(Camera::Mode::Orbit);
    cc.CameraInstance.SetTarget(glm::vec3(0.0f, 0.5f, 0.0f));
    cc.CameraInstance.SetOrbit(0.0f, 25.0f, 8.0f);
    m_Context->CameraEntity = camera;

    // 方向光实体（-60° 绕 X 轴：从上前方照下）
    auto dirLight = m_Context->Scene->CreateEntity("DirectionalLight");
    dirLight.GetComponent<TransformComponent>().Rotation =
        glm::vec3(glm::radians(-60.0f), 0.0f, 0.0f);
    dirLight.AddComponent<DirectionalLightComponent>(glm::vec4(1.0f, 1.0f, 1.0f, 1.0f));

    // 环境光实体
    auto ambLight = m_Context->Scene->CreateEntity("AmbientLight");
    ambLight.AddComponent<AmbientLightComponent>(glm::vec4(0.15f, 0.15f, 0.15f, 1.0f));

    // 三个立方体，横向排列
    auto makeCube = [&](const char *name, const glm::vec3 &pos, Material *mat) {
        Entity e = m_Context->Scene->CreateEntity(name);
        e.GetComponent<TransformComponent>().Translation = pos;
        e.AddComponent<MeshComponent>(meshMgr.GetBuiltin("cube"));
        e.AddComponent<MaterialComponent>(mat);
        return e;
    };
    makeCube("Cube_Red", {-1.5f, 0.5f, 0.0f}, mats[0]);
    makeCube("Cube_Green", {0.0f, 0.5f, 0.0f}, mats[1]);
    makeCube("Cube_Blue", {1.5f, 0.5f, 0.0f}, mats[2]);

    // 大量点光源网格：验证点光源已迁入 SSBO 无编译期上限（此处 49 个 > 原 8 上限）。
    // 在场景上方铺一层 7x7 点光源网格，颜色按位置渐变，便于观察多灯叠加效果。
    // 点光源"灯泡"可视化用的白色材质（灯球呈中性白，与其光色区分）
    auto whiteMat = std::make_unique<Material>();
    whiteMat->SetTexture(Material::Albedo, texMgr.GetSolidColor(glm::vec4(1.0f)));
    Material *matWhite = matMgr.Register("Editor_LightBulb", std::move(whiteMat));

    auto makePointLight = [&](const char *name, const glm::vec3 &pos,
                              const glm::vec3 &rgb, float radiusInv) {
        Entity e = m_Context->Scene->CreateEntity(name);
        auto &tc = e.GetComponent<TransformComponent>();
        tc.Translation = pos;
        e.AddComponent<PointLightComponent>(glm::vec4(rgb, 0.6f), radiusInv);

        // 挂一个缩小的球体作为"灯泡"可视化，便于在场景中看到每个灯的位置与颜色
        tc.Scale = {0.2f, 0.2f, 0.2f};
        e.AddComponent<MeshComponent>(meshMgr.GetBuiltin("sphere"));
        e.AddComponent<MaterialComponent>(matWhite);
        return e;
    };

    const int gridN = 7;        // 网格边长（7x7 = 49 个灯）
    const float extent = 4.0f;  // 网格范围半径
    for (int ix = 0; ix < gridN; ++ix) {
        for (int iz = 0; iz < gridN; ++iz) {
            float x = -extent + extent * 2.0f * ix / (gridN - 1);
            float z = -extent + extent * 2.0f * iz / (gridN - 1);
            // 颜色按位置渐变（X 通道→红，Z 通道→绿，蓝固定），强度较低避免过曝
            glm::vec3 rgb{
                (ix + 1.0f) / gridN,
                (iz + 1.0f) / gridN,
                0.5f};
            makePointLight("PointLight_Grid", {x, 1.6f, z}, rgb, 0.5f);
        }
    }

    // ── PBR 演示：金属度 × 粗糙度 梯度球阵 ───────────────────────────────
    // 用 PBR 管线（Material::Type::PBR）铺一张 metallic（列）× roughness（行）
    // 梯度球阵，直观展示金属-粗糙度工作流：金属度 0→1 从绝缘体渐变到全金属
    // 镜面；粗糙度升高高光变柔。配合上方的点光源网格，可观察多灯 PBR 高光叠加。
    // 无 MetallicRoughness 贴图，走标量 fallback（metallic/roughness 浮点参数）。
    const int   pMetallic = 5;      // 金属度列数
    const int   pRough    = 5;      // 粗糙度行数
    const float pSpacing  = 0.9f;   // 球心间距
    const float pScale    = 0.35f;  // 球缩放（内置 sphere 半径 1.0 → 直径 0.7）
    for (int mi = 0; mi < pMetallic; ++mi) {
        for (int ri = 0; ri < pRough; ++ri) {
            float metallic  = static_cast<float>(mi) / (pMetallic - 1);
            float roughness = static_cast<float>(ri) / (pRough - 1);

            std::string name = "PBR_Sphere_M" + std::to_string(mi)
                               + "_R" + std::to_string(ri);
            auto mat = std::make_unique<Material>();
            mat->SetType(Material::Type::PBR);
            // 暖橙 albedo，与红/绿/蓝立方体区分；金属度取该色为 F0
            mat->SetTexture(Material::Albedo,
                            texMgr.GetSolidColor(glm::vec4(0.9f, 0.5f, 0.3f, 1.0f)));
            mat->SetFloat("metallic", metallic);
            mat->SetFloat("roughness", roughness);
            Material *pbrMat = matMgr.Register(name, std::move(mat));

            Entity e = m_Context->Scene->CreateEntity(name);
            auto &tc = e.GetComponent<TransformComponent>();
            tc.Translation = {
                -pSpacing * (pMetallic - 1) / 2.0f + mi * pSpacing,
                pScale,
                2.5f + ri * pSpacing,
            };
            tc.Scale = {pScale, pScale, pScale};
            e.AddComponent<MeshComponent>(meshMgr.GetBuiltin("sphere"));
            e.AddComponent<MaterialComponent>(pbrMat);
        }
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