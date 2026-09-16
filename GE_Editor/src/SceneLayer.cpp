//
// 场景层实现：只负责场景渲染（离屏视口 + 相机）与文件操作（保存/加载/新建）。
// 面板（层级/资源）已拆分为独立 Layer，共享 EditorContext。
//

#include "SceneLayer.h"

#include "GE/Core/Application.h"
#include "GE/Debug/Profiler.h"
#include "GE/Events/KeyEvent.h"
#include "GE/Events/MouseEvent.h"
#include "GE/Render/Renderer.h"
#include "GE/Render/Renderer2D.h"
#include "GE/Render/Renderer3D.h"
#include "GE/Render/SceneRenderPasses.h"
#include "GE/Render/AssetManager.h"
#include "GE/Render/MeshManager.h"
#include "GE/Render/Mesh.h"
#include "GE/Render/RenderGraph/RenderGraph.h"
#include "GE/Render/RenderTarget.h"
#include "GE/Scene/Components.h"
#include "GE/Scene/Entity.h"
#include "GE/Scene/SceneSerializer.h"
#include "GE/Scene/GLTFSceneImporter.h"
#include "GE/Utils/PlatformUtils.h"

#include "GE/Core/GEWindow.h"

#include <algorithm>
#include <array>
#include <fstream>
#include <map>
#include <sstream>

#include "imgui.h"
#include "ImGuizmo.h"
#include "Render/Renderer2D.h"
#include "Render/Renderer3D.h"

#include "GizmoController.h"
#include "DebugDrawLayer.h"

// 编译期开关：true = 启动时从代码程序化构建默认场景；false = 从 .scene 文件加载。
// 无需代码路径时，编辑器默认从文件加载（false）。
#define GE_EDITOR_BUILD_SCENE_FROM_CODE 0

#include <glm/gtc/matrix_transform.hpp>

namespace {
// 编辑器状态文件：放在工作目录（与 imgui.ini 同级）。
// 这是个人编辑器偏好而非场景内容，不入版本库（见 .gitignore）。
constexpr const char *kEditorSettingsFile = "editor_settings.cfg";
// 旧文件名（仅相机参数），仅作向后兼容读取，保存一律写新文件
constexpr const char *kEditorSettingsFileLegacy = "editor_camera.cfg";

// 读取整个状态文件为 key→value 表（空行与 '#' 注释行跳过；坏键忽略）
std::map<std::string, float> LoadEditorSettingsFile() {
    std::map<std::string, float> kv;
    std::ifstream in(kEditorSettingsFile);
    if (!in.is_open()) {
        // 旧版本只存过 editor_camera.cfg：新文件尚不存在时读它，避免丢上次相机视角
        in.open(kEditorSettingsFileLegacy);
    }
    std::string line;
    while (std::getline(in, line)) {
        std::istringstream ss(line);
        std::string key;
        if (!(ss >> key)) {
            continue; // 空行
        }
        if (key[0] == '#') {
            continue; // 注释
        }
        float value = 0.0f;
        if (ss >> value) {
            kv[key] = value;
        }
    }
    return kv;
}

// 从状态表取键值，缺失（首次运行/格式升级缺字段）时回退默认
float EditorSettingsValue(const std::map<std::string, float> &kv, const char *key, float fallback) {
    auto it = kv.find(key);
    return it != kv.end() ? it->second : fallback;
}
} // namespace

namespace GE {

SceneLayer::SceneLayer(std::shared_ptr<EditorContext> context) : Layer("SceneLayer"), m_Context(std::move(context)) {
}

SceneLayer::~SceneLayer() = default;

void SceneLayer::OnAttach() {
    // 编辑器导航相机初始化：工具视角，独立于场景内容（不进场景、不序列化）。
    // 宽高比每帧随视口尺寸更新，见 OnUpdate。
    m_Context->EditorCamera.SetMode(Camera::Mode::Orbit);
    m_Context->EditorCamera.SetPerspective(60.0f, 16.0f / 9.0f, 0.1, 500);
    m_Context->EditorCamera.SetTarget(glm::vec3(0.0f, 0.5f, 0.0f));
    m_Context->EditorCamera.SetOrbit(0.0f, 25.0f, 8.0f);

#if GE_EDITOR_BUILD_SCENE_FROM_CODE
    // 从代码程序化构建默认场景（网格/纹理/材质由全局管理器持有）
    BuildDefaultSceneFromCode();
#else
    const std::string kDefaultScene = std::string(AssetPaths::Scenes) + "/2.scene";

    // 从文件加载默认场景（网格/纹理/材质由全局管理器加载持有）
    const std::string defaultScenePath =
        Renderer::GetAssetManager().ResolveCanonical(kDefaultScene);
    if (!LoadSceneFromFile(defaultScenePath)) {
        GE_CORE_WARN("SceneLayer: 启动加载默认场景失败: {0}", defaultScenePath);
    }
#endif

    // 上次关闭保存的编辑器状态覆盖默认值：相机视角（覆盖上方默认）与场景渲染设置
    // （剔除/延迟/Tonemap/Bloom，作用在已加载的默认场景与 Renderer3D 上）。
    // 无状态文件时全部保持默认。
    RestoreEditorSettings();
}

// 从代码程序化构建一个用于测试 OBJ+MTL 加载的默认场景：方向光 + 环境光 +
// Datsun 280Z 车模。网格/纹理/材质均由全局管理器加载持有，场景组件仅持非拥有指针。
// 注意：不经此路径创建相机会让场景在 Play 态回退编辑器相机（无玩法相机）。
void SceneLayer::BuildDefaultSceneFromCode() {
    // 先重置实体引用，避免悬空
    m_Context->Scene = std::make_unique<Scene>();

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

    // 水面（阶段 1）：64×64 水格 + 默认 4 层 Gerstner 波。
    auto water = m_Context->Scene->CreateEntity("Water");
    water.GetComponent<TransformComponent>().Translation = {0.0f, -0.5f, 0.0f};
    water.AddComponent<WaterComponent>();
}

void SceneLayer::OnDetach() {
    // 关闭前保存编辑器相机与场景渲染设置（此后 Scene 与视口会被释放）
    SaveEditorSettings();

    m_Viewport.reset(); // 释放离屏渲染目标（GPU 资源）
    m_Context->Scene.reset();
}

void SceneLayer::UpdateMouseCapture() {
    Window &window = Application::Get().GetWindow();
    if (!window.GetNativeWindow() || !m_Context->Scene) {
        return;
    }

    // Play 态且场景存在挂 FollowCameraComponent 的角色才锁定
    const bool playing = m_Context->Scene->IsPlaying();
    auto followView = m_Context->Scene->Reg().view<TransformComponent, CharacterControllerComponent,
                                                   FollowCameraComponent>();
    const bool hasFollowCam = playing && (followView.begin() != followView.end());

    if (hasFollowCam && !m_MouseCaptured) {
        // 锁定瞬间：光标被锁到窗口中心、位置跳变 → 重置 InputState 增量基准，防首帧 delta 爆值
        m_Context->Scene->GetMutableInputState().ResetMouseBaseline(window.GetCursorPosition());
        window.SetCursorMode(CursorMode::Disabled);
        m_MouseCaptured = true;
    } else if (!hasFollowCam && m_MouseCaptured) {
        // 解锁瞬间：同样重置基准，避免恢复光标位置跳变造成 delta 爆值
        m_Context->Scene->GetMutableInputState().ResetMouseBaseline(window.GetCursorPosition());
        window.SetCursorMode(CursorMode::Normal);
        m_MouseCaptured = false;
    }
}

bool SceneLayer::EnsureViewport(Timestep &ts, uint32_t &vpWidth, uint32_t &vpHeight) {
    // 视口窗口尺寸（上一帧由 OnImGuiRender 记录；首帧为 0 时跳过离屏渲染）
    vpWidth = static_cast<uint32_t>(m_ViewportSize.x);
    vpHeight = static_cast<uint32_t>(m_ViewportSize.y);
    if (vpWidth == 0 || vpHeight == 0) {
        return false;
    }

    // 确保离屏渲染目标存在，且尺寸与视口一致（变化时重建）
    if (!m_Viewport) {
        m_Viewport = std::make_unique<SceneViewport>();
    }
    // 首次调用 Create 创建离屏目标；之后尺寸变化时用 OnResize 重建。
    // 重建会重新分配 GPU 图像 + 深度 + 采样器，开销大；拖拽视口时一帧一变，
    // 若每帧重建会刷屏日志并浪费资源。故对重建限流（约 4 次/秒）。
    if (!m_Viewport->GetRenderTarget()) {
        m_Viewport->Create(Renderer::GetVulkanContext().GetDevice(), vpWidth, vpHeight);
    } else {
        m_ResizeCooldown += ts.GetSeconds();
        if (m_ResizeCooldown >= 0.25f) {
            m_Viewport->OnResize(vpWidth, vpHeight);
            m_ResizeCooldown = 0.0f;
        }
    }
    return m_Viewport->GetRenderTarget() != nullptr;
}

Camera &SceneLayer::GetRenderingViewCamera(float aspect) {
    // 选取本帧视口相机与宽高比：
    //   Edit → 编辑器导航相机（EditorContext.EditorCamera，工具视角）；
    //   Play → 场景主玩法相机（CameraComponent 实体，游戏视角）；场景无相机则回退编辑器相机。
    if (m_Context->Scene && m_Context->Scene->IsPlaying()) {
        if (Entity gameCam = m_Context->Scene->GetPrimaryCameraEntity();
            gameCam && gameCam.HasComponent<CameraComponent>()) {
            auto &cc = gameCam.GetComponent<CameraComponent>();
            if (!cc.FixedAspectRatio) {
                cc.CameraInstance.SetAspect(aspect);
            }
            return cc.CameraInstance;
        }
    }

    m_Context->EditorCamera.SetAspect(aspect);
    return m_Context->EditorCamera;
}

void SceneLayer::OnUpdate(Timestep &ts) {
    GE_PROFILE_SCOPE("SceneLayer::OnUpdate");

    // Play 态跟随相机：锁定鼠标（否则无法持续转向）
    UpdateMouseCapture();

    // 视口准备独立成块：内层埋点若与函数级埋点同处一个作用域，会重复声明 Tracy 的
    // ___tracy_scoped_zone（C2374）。vpWidth/vpHeight 仍在外层声明，块内 return
    // 由 RAII 正常收尾。
    uint32_t vpWidth = 0, vpHeight = 0;
    {
        GE_PROFILE_SCOPE("ViewportPrepare");
        if (!EnsureViewport(ts, vpWidth, vpHeight)) {
            return;
        }
        // 同步场景视口尺寸
        m_Context->Scene->OnViewportResize(vpWidth, vpHeight);
    }

    const float aspect = static_cast<float>(vpWidth) / static_cast<float>(vpHeight);

    // ── 仿真推进（不含渲染）：脚本 / 物理 / 跟随相机 / 动画 / 世界矩阵 / 蒙皮 ──
    // 必须在取渲染相机矩阵之前调用，保证本帧 view / viewPos 与仿真后的游戏相机同帧，
    // 避免第三人称相机落后一帧造成的跟随抖动。
    {
        GE_PROFILE_SCOPE("Scene::OnUpdate3DSimulation");
        m_Context->Scene->OnUpdate3DSimulation(ts);
    }

    Camera &activeCam = GetRenderingViewCamera(aspect);
    const glm::mat4 view = activeCam.GetView();
    const glm::mat4 projection = activeCam.GetProj();
    const glm::vec3 cameraPos = activeCam.GetPosition();
    const glm::vec4 clearColor{0.1f, 0.1f, 0.15f, 1.0f};

    // 场景曝光接线：把本帧视口相机（编辑器相机 / 游戏主相机）的曝光系数
    // 传给 Renderer3D，Deferred 模式的 Tonemap pass 经 TonemapUBO.exposure.x 使用。
    Renderer::Get3DRenderer().SetExposure(activeCam.GetExposure());

    // 场景只做渲染采集（3D/2D 批次经 EndScene 延迟快照，不录制命令）
    {
        GE_PROFILE_SCOPE("Scene::Render3D");
        m_Context->Scene->Render3D(view, projection, cameraPos, clearColor);
    }

    // 向本帧渲染图注册场景 pass。颜色/深度取离屏视口目标（RenderTarget 不拥有，
    // 图只编排同步）；颜色收尾转 ShaderReadOnly 供 ImGui 视口窗口采样。
    {
        GE_PROFILE_SCOPE("RecordScenePasses");
        auto &builder = Renderer::Get().GetFrameGraphBuilder();
        RenderTarget &viewportRT = *m_Viewport->GetRenderTarget();
        ResourceHandle hColor = builder.Import(&viewportRT.GetColorView(), "ViewportColor");
        ResourceHandle hDepth = builder.Import(&viewportRT.GetDepthView(), "ViewportDepth");
        GE::RecordScenePasses(builder, hColor, hDepth, viewportRT.GetExtent(),
                              vk::ImageLayout::eShaderReadOnlyOptimal, clearColor);
    }
}

void SceneLayer::OnEvent(Event &event) {
    if (!m_Context->Scene) {
        return;
    }
    const bool inViewport = m_SceneWindowHovered;
    const bool playing = m_Context->Scene->IsPlaying();

    // Play 态按 ESC 退出运行：编辑器快捷键，不落入游戏脚本/相机输入。
    // Stop() 回滚到摆放姿态，随后 UpdateMouseCapture 因 hasFollowCam 变 false 自动解锁鼠标。
    if (playing) {
        EventDispatcher escDisp(event);
        bool escPressed = false;
        escDisp.Dispatch<KeyPressedEvent>([&](KeyPressedEvent &e) {
            if (e.GetKeyCode() == Key::Escape) {
                escPressed = true;
                return true;
            }
            return false;
        });
        if (escPressed) {
            m_Context->Scene->Stop();
            event.Handled = true;
            return;
        }
    }

    // 相机额外要求未在拖 gizmo，避免拖 gizmo 时相机跟着转。
    const bool cameraActive = inViewport && !(m_Gizmo && ImGuizmo::IsOver());
    // 相机导航输入目标：Edit → 编辑器相机（EditorContext 独立持有）；Play → 场景主玩法相机。
    // Edit 下场景相机实体一律不收输入，避免编辑导航越权到游戏相机。
    m_Context->Scene->SetProcessCameraInput(playing && cameraActive);

    if (event.Handled) {
        return;
    }

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

    // 输入路由：Scene 只记脚本输入快照（相机是否消费由 SetProcessCameraInput 门控）；
    // 编辑器相机的导航输入在场景之外直接喂给 EditorCamera。
    const bool mouseCaptured = m_MouseCaptured; // Play 态跟随相机已锁鼠标
    const auto routeInput = [&](bool feedCamera) {
        if (playing) {
            m_Context->Scene->SetProcessCameraInput(feedCamera);
            m_Context->Scene->OnEvent(event);
        } else {
            m_Context->Scene->OnEvent(event); // 仅脚本输入快照
            if (feedCamera) {
                m_Context->EditorCamera.OnEvent(event);
            }
        }
    };

    // 未悬停时直接不转发输入事件给场景。
    // 例外 1：松开的是视口内按下的按键时，仍回传释放事件，让相机按键状态复位。
    // 例外 2：跟随相机鼠标已锁定（CursorMode::Disabled）——ImGui 悬停判定失效，
    //         但光标被锁在窗口内、位置相对移动仍上报，必须无条件转发否则无法转向。
    if (!inViewport && event.IsInCategory(EventCategoryInput) && !mouseCaptured) {
        // 例外：松开的是视口内按下的按键时，仍回传释放事件，让相机按键状态复位。
        if (releaseStartedInViewport) {
            routeInput(true);
        }
        return;
    }
    routeInput(cameraActive);
}

Camera &SceneLayer::GetActiveViewCamera() {
    // Play：优先场景主玩法相机（CameraComponent.Primary==true，无则第一个相机实体）；
    // 场景没有相机实体则回退编辑器相机（保证视口不黑屏）。Edit：一律编辑器相机。
    if (m_Context->Scene && m_Context->Scene->IsPlaying()) {
        if (Entity gameCam = m_Context->Scene->GetPrimaryCameraEntity();
            gameCam && gameCam.HasComponent<CameraComponent>()) {
            return gameCam.GetComponent<CameraComponent>().CameraInstance;
        }
    }
    return m_Context->EditorCamera;
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
            // 阶段 C：Play（运行）中禁用 gizmo 拖拽——动态体每帧被物理写回 Transform，
            // 拖了也白拖还制造困惑，显式隐藏（决策 5.4）。
            if (m_Gizmo && m_Context->Scene && !m_Context->Scene->IsPlaying()) {
                m_Gizmo->Render(GetActiveViewCamera(), glm::vec2(imagePos.x, imagePos.y), m_ViewportSize);
            }

            // 调试线框叠加（包围盒/碰撞体/跟随相机视点）已拆到 DebugDrawLayer
            if (m_DebugDrawLayer) {
                m_DebugDrawLayer->RenderSceneOverlay(
                    GetActiveViewCamera(), glm::vec2(imagePos.x, imagePos.y), m_ViewportSize);
            }

            // 阶段 C：Play 运行中在视口左下角叠"▶ 运行中"徽标（gizmo 已隐藏，一眼可见当前处于模拟）
            if (m_Context->Scene && m_Context->Scene->IsPlaying()) {
                ImDrawList *dl = ImGui::GetWindowDrawList();
                const char *label = "▶ 运行中";
                const glm::vec2 pos{imagePos.x + 10.0f, imagePos.y + m_ViewportSize.y - 26.0f};
                const ImVec2 labelSize = ImGui::CalcTextSize(label);
                constexpr float kPad = 6.0f;
                dl->AddRectFilled(ImVec2(pos.x - kPad, pos.y - 3.0f),
                                  ImVec2(pos.x + labelSize.x + kPad, pos.y + labelSize.y + 3.0f),
                                  IM_COL32(0, 0, 0, 150), 4.0f);
                dl->AddText(ImVec2(pos.x, pos.y), IM_COL32(0x2E, 0xD0, 0x5E, 255), label);
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

    // ---- 阶段 C：编辑/运行时物理分离 —— ▶ Play / ⏹ Stop 运行控制 ----
    // Edit 态物理静止（可自由摆放），Playing 态物理接管模拟，Stop 回到摆放姿态。
    if (m_Context->Scene) {
        if (m_Context->Scene->IsPlaying()) {
            if (ImGui::Button("⏹ Stop")) {
                m_Context->Scene->Stop();
            }
            ImGui::SameLine();
            ImGui::TextDisabled("运行中（物理模拟中，编辑禁用）");
        } else {
            if (ImGui::Button("▶ Play")) {
                m_Context->Scene->Play();
            }
            ImGui::SameLine();
            ImGui::TextDisabled("编辑中（物理静止）");
        }
        ImGui::Separator();
    }

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

        // 延迟渲染开关：切换 SceneLayer 的 Scene3D 单 pass 与 GBuffer → Lighting
        // → Transparent(HDR) → Tonemap 四 pass 链，便于 RenderDoc / 视觉 A-B 对比。
        ImGui::Separator();
        bool deferred = Renderer::Get3DRenderer().IsDeferred();
        if (ImGui::Checkbox("延迟渲染", &deferred)) {
            Renderer::Get3DRenderer().SetDeferred(deferred);
        }
        ImGui::SameLine();
        // Tonemap 开关：仅延迟 HDR 链生效。关闭时 Tonemap pass 跳过 ACES，直接
        // 输出线性 HDR，便于在编辑器里看 HDR 原值（高光/自发光 > 1.0）。
        bool tonemap = Renderer::Get3DRenderer().IsTonemapEnabled();
        if (ImGui::Checkbox("Tonemap", &tonemap)) {
            Renderer::Get3DRenderer().SetTonemapEnabled(tonemap);
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("关闭后 Tonemap pass 跳过 ACES，直接透传 Scene_HDR（调试 HDR 原值用）");
        }

        // Bloom 开关与参数：仅延迟 HDR 链生效（Transparent → Bloom → Tonemap）。
        bool bloomEnabled = Renderer::Get3DRenderer().IsBloomEnabled();
        if (ImGui::Checkbox("Bloom", &bloomEnabled)) {
            Renderer::Get3DRenderer().SetBloomEnabled(bloomEnabled);
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("把超过阈值的高光扩散开，再交给 Tonemap；天空不受影响");
        }
        float bloomThreshold = Renderer::Get3DRenderer().GetBloomThreshold();
        if (ImGui::SliderFloat("Bloom 阈值", &bloomThreshold, 0.1f, 4.0f, "%.2f")) {
            Renderer::Get3DRenderer().SetBloomThreshold(bloomThreshold);
        }
        float bloomIntensity = Renderer::Get3DRenderer().GetBloomIntensity();
        if (ImGui::SliderFloat("Bloom 强度", &bloomIntensity, 0.0f, 2.0f, "%.2f")) {
            Renderer::Get3DRenderer().SetBloomIntensity(bloomIntensity);
        }
        int bloomMipLevels = static_cast<int>(Renderer::Get3DRenderer().GetBloomMipLevels());
        if (ImGui::SliderInt("Bloom mip", &bloomMipLevels, 1, 6)) {
            Renderer::Get3DRenderer().SetBloomMipLevels(
                static_cast<uint32_t>(bloomMipLevels));
        }

        // UnderwaterFX 总开关：仅延迟 HDR 链生效（Transparent → UnderwaterFX → Bloom → Tonemap）。
        bool underwaterFx = Renderer::Get3DRenderer().IsUnderwaterFxEnabled();
        if (ImGui::Checkbox("水下后处理", &underwaterFx)) {
            Renderer::Get3DRenderer().SetUnderwaterFxEnabled(underwaterFx);
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("相机入水后的全屏水雾/焦散；关闭后画面零变化");
        }

        ImGui::Separator();
        // ---- 编辑器相机：直接调整轨道/投影/灵敏度参数 ----
        // EditorCamera 是工具视角（不进场景、不序列化），默认靠视口内鼠标漫游。
        // 这里暴露其可调参数，便于在面板里精确摆位（角度/距离/裁剪面），
        // 拖动时相机输入不受影响（输入只在悬停 Scene 视口时路由给相机）。
        if (ImGui::CollapsingHeader("编辑器相机", ImGuiTreeNodeFlags_DefaultOpen)) {
            Camera &cam = m_Context->EditorCamera;
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("调整编辑器导航相机（Orbit）；目标点与距离受滚轮影响，改动即时生效");

            ImGui::TextDisabled("轨道");
            glm::vec3 target = cam.GetTarget();
            if (ImGui::DragFloat3("目标点", &target.x, 0.05f)) {
                cam.SetTarget(target);
            }
            float theta = cam.GetTheta();
            float phi = cam.GetPhi();
            float dist = cam.GetDistance();
            bool orbitChanged = false;
            orbitChanged |= ImGui::DragFloat("方位角 θ", &theta, 0.5f);
            orbitChanged |= ImGui::DragFloat("俯仰角 φ", &phi, 0.5f, -89.0f, 89.0f);
            orbitChanged |= ImGui::DragFloat("距离", &dist, 0.05f, cam.MinDistance, cam.MaxDistance);
            if (orbitChanged) {
                cam.SetOrbit(theta, phi, dist);
            }
            // 只读显示当前相机位置（轨道模式由 target/角度/距离推算）
            const glm::vec3 eyePos = cam.GetPosition();
            ImGui::Text("相机位置: (%.2f, %.2f, %.2f)", eyePos.x, eyePos.y, eyePos.z);

            ImGui::Separator();
            ImGui::TextDisabled("投影");
            float fov = cam.GetFov();
            float nearP = cam.GetNear();
            float farP = cam.GetFar();
            if (ImGui::SliderFloat("FOV", &fov, 20.0f, 120.0f, "%.1f°")
                || ImGui::DragFloat("近裁剪面", &nearP, 0.01f, 0.001f, 100.0f, "%.3f")
                || ImGui::DragFloat("远裁剪面", &farP, 1.0f, 1.0f, 5000.0f)) {
                cam.SetPerspective(fov, cam.GetAspect(), nearP, farP);
            }

            ImGui::Separator();
            ImGui::TextDisabled("曝光（HDR Tonemap，仅延迟渲染生效）");
            float exposure = cam.GetExposure();
            if (ImGui::SliderFloat("曝光", &exposure, 0.01f, 8.0f, "%.2f")) {
                cam.SetExposure(exposure);
            }
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("乘在 HDR 线性颜色上，ACES 之前；1.0 = 不改亮度");
            }

            ImGui::Separator();
            ImGui::TextDisabled("灵敏度");
            ImGui::SliderFloat("鼠标旋转", &cam.MouseSensitivity, 0.01f, 1.0f, "%.2f");
            ImGui::SliderFloat("滚轮缩放", &cam.ScrollSensitivity, 0.1f, 5.0f, "%.1f");

            if (ImGui::Button("重置为默认")) {
                cam.SetPerspective(60.0f, cam.GetAspect(), 0.1f, 500.0f);
                cam.SetTarget(glm::vec3(0.0f, 0.5f, 0.0f));
                cam.SetOrbit(0.0f, 25.0f, 8.0f);
            }
            ImGui::SameLine();
            ImGui::TextDisabled("60° FOV / 轨道 (0°,25°,8) / 目标 (0,0.5,0)");
        }

        ImGui::TextDisabled("提示：先在左侧 Hierarchy/Properties 中调整实体，再保存/加载验证");
    }

    // ---- glTF 导入失败提示（由顶部菜单「导入 glTF 场景...」触发，一次性弹窗）----
    // OpenPopup 与 BeginPopupModal 都位于本窗口作用域内，ID 一致才能匹配到同一弹窗
    if (m_GLTFImportFailed) {
        m_GLTFImportFailed = false;
        ImGui::OpenPopup("GLTFImportFailed");
    }
    if (ImGui::BeginPopupModal("GLTFImportFailed", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::Text("glTF 场景导入失败（请确认是合法的 .gltf / .glb 文件）");
        if (ImGui::Button("OK")) {
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }

    ImGui::End();
}

void SceneLayer::SetGizmoController(std::unique_ptr<GizmoController> gizmo) {
    m_Gizmo = std::move(gizmo);
}

void SceneLayer::SetDebugDrawLayer(DebugDrawLayer *debugDrawLayer) {
    m_DebugDrawLayer = debugDrawLayer;
}

void SceneLayer::SaveScene() {
    if (!m_Context->Scene) {
        return;
    }

    // 对话框初始目录跟随资源根（不依赖启动时的工作目录）
    const std::string sceneDir =
        (Renderer::GetAssetManager().GetAssetRoot() / AssetPaths::Scenes).string();
    std::string filepath = FileDialogs::SaveFile(
        "GE Scene (*.scene)\0*.scene\0All Files (*.*)\0*.*\0", sceneDir.c_str());
    if (filepath.empty()) {
        return;
    }

    // 保存按 Edit 视角（决策 5.5）：若正在 Play，先 Stop 回滚到摆放姿态再落盘，
    // 避免把模拟后的混乱 Transform 写进场景文件
    if (m_Context->Scene->IsPlaying()) {
        m_Context->Scene->Stop();
    }

    // 序列化器无状态、操作完即弃，按需创建为局部变量即可；
    // 网格/纹理/材质由全局管理器持有，序列化器自身不拥有资源
    SceneSerializer serializer(m_Context->Scene.get());
    serializer.Serialize(filepath);

    // 场景引用的材质资产是**独立文件**：只存场景不刷材质的话，重载时覆写引用的
    // .gemat 还是旧内容，用户刚改的东西看起来又"没存住"。故一并落盘。
    const int matSaved = Renderer::GetAssetManager().SaveAllDirtyMaterials();
    if (matSaved > 0) {
        GE_CORE_INFO("SceneLayer: 场景引用的材质资产已更新 {0} 个", matSaved);
    }
}

bool SceneLayer::LoadSceneFromFile(std::string_view filepath) {
    // 如果场景不存在，先创建
    if (!m_Context->Scene) {
        m_Context->Scene = std::make_unique<Scene>();
    }

    // 加载逻辑放 Scene（与运行时共享同一入口，避免两侧演化出差异）
    return m_Context->Scene->LoadFromFile(std::string(filepath));
}

void SceneLayer::SaveEditorSettings() {
    std::ofstream out(kEditorSettingsFile);
    if (!out) {
        GE_CORE_WARN("SaveEditorSettings: 无法写入编辑器状态文件 {0}", kEditorSettingsFile);
        return;
    }
    out.precision(9); // float 有效位写全，恢复时不致视角漂移
    out << "# GE 编辑器自动保存状态（勿手改；缺失字段恢复时回退默认）\n";

    // ---- 编辑器导航相机（Orbit：目标 + 角度 + 距离 + 投影 + 曝光 + 灵敏度）----
    const Camera &cam = m_Context->EditorCamera;
    const glm::vec3 target = cam.GetTarget();
    out << "mode " << static_cast<int>(cam.GetMode()) << '\n';
    out << "target_x " << target.x << '\n';
    out << "target_y " << target.y << '\n';
    out << "target_z " << target.z << '\n';
    out << "theta " << cam.GetTheta() << '\n';
    out << "phi " << cam.GetPhi() << '\n';
    out << "distance " << cam.GetDistance() << '\n';
    out << "fov " << cam.GetFov() << '\n';
    out << "near " << cam.GetNear() << '\n';
    out << "far " << cam.GetFar() << '\n';
    out << "exposure " << cam.GetExposure() << '\n';
    out << "mouse_sensitivity " << cam.MouseSensitivity << '\n';
    out << "scroll_sensitivity " << cam.ScrollSensitivity << '\n';
    out << "move_speed " << cam.MoveSpeed << '\n';

    // ---- 场景/渲染设置（SceneLayer 面板里的可调项）----
    if (m_Context->Scene) {
        out << "culling_mode "
            << static_cast<int>(m_Context->Scene->GetCullingMode()) << '\n';
    }
    const Renderer3D &r3d = Renderer::Get3DRenderer();
    out << "deferred " << (r3d.IsDeferred() ? 1 : 0) << '\n';
    out << "tonemap " << (r3d.IsTonemapEnabled() ? 1 : 0) << '\n';
    out << "bloom_enabled " << (r3d.IsBloomEnabled() ? 1 : 0) << '\n';
    out << "bloom_threshold " << r3d.GetBloomThreshold() << '\n';
    out << "bloom_intensity " << r3d.GetBloomIntensity() << '\n';
    out << "bloom_mip_levels " << r3d.GetBloomMipLevels() << '\n';
    out << "underwater_enabled " << (r3d.IsUnderwaterFxEnabled() ? 1 : 0) << '\n';
}

void SceneLayer::RestoreEditorSettings() {
    const auto kv = LoadEditorSettingsFile();
    if (kv.empty()) {
        return; // 无状态文件：全部保持 OnAttach/引擎默认值
    }

    // 状态文件中缺失的键回退当前值，保证老格式/手改坏文件也能恢复
    const auto get = [&kv](const char *key, float fallback) {
        return EditorSettingsValue(kv, key, fallback);
    };
    const auto getBool = [&kv](const char *key, bool fallback) {
        return EditorSettingsValue(kv, key, fallback ? 1.0f : 0.0f) > 0.5f;
    };

    // ---- 编辑器导航相机 ----
    Camera &cam = m_Context->EditorCamera;
    cam.SetMode(static_cast<Camera::Mode>(static_cast<int>(get("mode", 0.0f))));
    cam.SetTarget(glm::vec3(get("target_x", cam.GetTarget().x),
                            get("target_y", cam.GetTarget().y),
                            get("target_z", cam.GetTarget().z)));
    cam.SetOrbit(get("theta", cam.GetTheta()),
                 get("phi", cam.GetPhi()),
                 get("distance", cam.GetDistance()));
    cam.SetPerspective(get("fov", cam.GetFov()),
                       cam.GetAspect(), // 宽高比每帧随视口更新，不入文件
                       get("near", cam.GetNear()),
                       get("far", cam.GetFar()));
    cam.SetExposure(get("exposure", cam.GetExposure()));
    cam.MouseSensitivity = get("mouse_sensitivity", cam.MouseSensitivity);
    cam.ScrollSensitivity = get("scroll_sensitivity", cam.ScrollSensitivity);
    cam.MoveSpeed = get("move_speed", cam.MoveSpeed);

    // ---- 场景/渲染设置 ----
    if (m_Context->Scene) {
        m_Context->Scene->SetCullingMode(
            static_cast<Scene::CullingMode>(static_cast<int>(get("culling_mode", 0.0f))));
    }
    Renderer3D &r3d = Renderer::Get3DRenderer();
    r3d.SetDeferred(getBool("deferred", r3d.IsDeferred()));
    r3d.SetTonemapEnabled(getBool("tonemap", r3d.IsTonemapEnabled()));
    r3d.SetBloomEnabled(getBool("bloom_enabled", r3d.IsBloomEnabled()));
    r3d.SetBloomThreshold(get("bloom_threshold", r3d.GetBloomThreshold()));
    r3d.SetBloomIntensity(get("bloom_intensity", r3d.GetBloomIntensity()));
    r3d.SetBloomMipLevels(
        static_cast<uint32_t>(get("bloom_mip_levels", static_cast<float>(r3d.GetBloomMipLevels()))));
    r3d.SetUnderwaterFxEnabled(getBool("underwater_enabled", r3d.IsUnderwaterFxEnabled()));
}

void SceneLayer::LoadScene() {
    // 对话框初始目录跟随资源根（不依赖启动时的工作目录）
    const std::string sceneDir =
        (Renderer::GetAssetManager().GetAssetRoot() / AssetPaths::Scenes).string();
    std::string filepath = FileDialogs::OpenFile(
        "GE Scene (*.scene)\0*.scene\0All Files (*.*)\0*.*\0", sceneDir.c_str());
    if (filepath.empty()) {
        return;
    }

    if (!LoadSceneFromFile(filepath)) {
        GE_CORE_WARN("SceneLayer: 加载场景失败: {0}", filepath);
    }
}

void SceneLayer::NewScene() {
    // 创建新场景（序列化器无状态，仅在保存/加载时按需创建局部变量）
    m_Context->Scene = std::make_unique<Scene>();
}

void SceneLayer::ImportGLTFScene() {
    if (!m_Context->Scene) {
        GE_CORE_WARN("SceneLayer: 无活动场景，无法导入 glTF");
        return;
    }

    // 从文件对话框选 glTF 场景文件，导入到当前场景根（保留 node 层级与变换）。
    // 网格/纹理/材质由全局管理器持有，导入器只向场景落实体树。
    std::string filepath = FileDialogs::OpenFile(
        "glTF 场景 (*.gltf;*.glb)\0*.gltf;*.glb\0All Files (*.*)\0*.*\0");
    if (filepath.empty()) {
        return;
    }

    auto &meshMgr = Renderer::GetMeshManager();
    if (!GLTFSceneImporter::Import(*m_Context->Scene, meshMgr, filepath)) {
        GE_CORE_WARN("SceneLayer: glTF 场景导入失败: {0}", filepath);
        m_GLTFImportFailed = true;
    }
}

void SceneLayer::ReloadAllScripts() {
    if (m_Context && m_Context->Scene) {
        m_Context->Scene->GetScriptEngine().ReloadAll();
    }
}

} // namespace GE
