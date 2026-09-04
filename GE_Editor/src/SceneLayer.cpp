//
// 场景层实现：只负责场景渲染（离屏视口 + 相机）与文件操作（保存/加载/新建）。
// 面板（层级/资源）已拆分为独立 Layer，共享 EditorContext。
//

#include "SceneLayer.h"

#include "GE/Core/Application.h"
#include "GE/Events/KeyEvent.h"
#include "GE/Events/MouseEvent.h"
#include "GE/Render/Renderer.h"
#include "GE/Render/Renderer2D.h"
#include "GE/Render/Renderer3D.h"
#include "GE/Render/AssetManager.h"
#include "GE/Render/MeshManager.h"
#include "GE/Render/Mesh.h"
#include "GE/Render/RenderGraph/RenderGraph.h"
#include "GE/Render/RenderTarget.h"
#include "GE/Scene/Components.h"
#include "GE/Scene/Entity.h"
#include "GE/Scene/SceneSerializer.h"
#include "GE/Utils/PlatformUtils.h"

#include <GLFW/glfw3.h>

#include "imgui.h"
#include "ImGuizmo.h"
#include "Render/Renderer2D.h"
#include "Render/Renderer3D.h"

#include "GizmoController.h"

// 编译期开关：true = 启动时从代码程序化构建默认场景；false = 从 .scene 文件加载。
// 无需代码路径时，编辑器默认从文件加载（false）。
#define GE_EDITOR_BUILD_SCENE_FROM_CODE 0

#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <unordered_set>

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
    // 编辑器导航相机初始化：工具视角，独立于场景内容（不进场景、不序列化）。
    // 宽高比每帧随视口尺寸更新，见 OnUpdate。
    m_Context->EditorCamera.SetMode(Camera::Mode::Orbit);
    m_Context->EditorCamera.SetPerspective(60.0f, 16.0f / 9.0f);
    m_Context->EditorCamera.SetTarget(glm::vec3(0.0f, 0.5f, 0.0f));
    m_Context->EditorCamera.SetOrbit(0.0f, 25.0f, 8.0f);

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
}

void SceneLayer::OnDetach() {
    m_Viewport.reset(); // 释放离屏渲染目标（GPU 资源）
    m_Context->Scene.reset();
}

void SceneLayer::UpdateMouseCapture() {
    auto *window = static_cast<GLFWwindow *>(Application::Get().GetWindow().GetGlfwWindow());
    if (!window || !m_Context->Scene) {
        return;
    }

    // Play 态且场景存在挂 FirstPersonCameraComponent 的角色才锁定
    const bool playing = m_Context->Scene->IsPlaying();
    auto fpView = m_Context->Scene->Reg().view<TransformComponent, CharacterControllerComponent,
                                               FirstPersonCameraComponent>();
    const bool hasFps = playing && (fpView.begin() != fpView.end());

    if (hasFps && !m_MouseCaptured) {
        // 锁定瞬间：光标被锁到窗口中心、位置跳变 → 重置 InputState 增量基准，防首帧 delta 爆值
        double cx = 0.0, cy = 0.0;
        glfwGetCursorPos(window, &cx, &cy);
        m_Context->Scene->GetMutableInputState().ResetMouseBaseline({(float)cx, (float)cy});
        glfwSetInputMode(window, GLFW_CURSOR, GLFW_CURSOR_DISABLED);
        m_MouseCaptured = true;
    } else if (!hasFps && m_MouseCaptured) {
        // 解锁瞬间：同样重置基准，避免恢复光标位置跳变造成 delta 爆值
        double cx = 0.0, cy = 0.0;
        glfwGetCursorPos(window, &cx, &cy);
        m_Context->Scene->GetMutableInputState().ResetMouseBaseline({(float)cx, (float)cy});
        glfwSetInputMode(window, GLFW_CURSOR, GLFW_CURSOR_NORMAL);
        m_MouseCaptured = false;
    }
}

void SceneLayer::OnUpdate(Timestep &ts) {
    // Play 态第一人称相机：锁定鼠标（否则无法持续转向）
    UpdateMouseCapture();

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

    // 渲染图路径（S2）：3D/2D 走延迟录制。目标是离屏视口，但录制动作延后到
    // 下方两张 pass 的 execute 回调（RenderGraph 已打开动态渲染、转好布局）。
    // Scene 的 RenderMeshes3D / RenderSprites2D 仍按现状调 BeginScene/Draw/EndScene，
    // EndScene 在 defer 模式下只收尾采集（3D 批次留 m_Meshes、2D 快照进 m_Sessions）。
    auto &r3d = Renderer::Get3DRenderer();
    auto &r2d = Renderer::Get2DRenderer();
    RenderTarget *viewportRT = m_Viewport->GetRenderTarget();
    r3d.SetRenderTarget(viewportRT);
    r2d.SetRenderTarget(viewportRT);
    r3d.SetDeferRecording(true);
    r2d.SetDeferRecording(true);

    // 选取本帧视口相机与宽高比：
    //   Edit → 编辑器导航相机（EditorContext.EditorCamera，工具视角）；
    //   Play → 场景主玩法相机（CameraComponent 实体，游戏视角）；场景无相机则回退编辑器相机。
    Camera *activeCam = nullptr;
    if (m_Context->Scene->IsPlaying()) {
        if (Entity gameCam = m_Context->Scene->GetPrimaryCameraEntity();
            gameCam && gameCam.HasComponent<CameraComponent>()) {
            auto &cc = gameCam.GetComponent<CameraComponent>();
            if (!cc.FixedAspectRatio) {
                cc.CameraInstance.SetAspect(aspect);
            }
            activeCam = &cc.CameraInstance;
        }
    }
    if (!activeCam) {
        m_Context->EditorCamera.SetAspect(aspect);
        activeCam = &m_Context->EditorCamera;
    }

    glm::mat4 view = activeCam->GetView();
    glm::mat4 projection = activeCam->GetProj();
    glm::vec3 cameraPos = activeCam->GetPosition();
    glm::vec4 clearColor{0.1f, 0.1f, 0.15f, 1.0f};

    // Scene 只做仿真 + 采集（3D/2D 批次经 EndScene 延迟快照，不录制命令）
    m_Context->Scene->OnUpdate3D(ts, view, projection, cameraPos, clearColor);

    // ── 两 pass 声明 + Execute：Scene3D（清屏含深度）→ Scene2D（叠加世界/UI 精灵） ──
    RenderGraph graph("SceneGraph");
    RenderGraphBuilder builder(graph);

    auto &cmd = Renderer::GetFrameCmd();
    auto &frame = Renderer::GetRenderContext().GetActiveFrame();
    const auto extent = viewportRT->GetExtent();
    vk::Rect2D renderArea{{0, 0}, {extent.width, extent.height}};

    // 外部资源：离屏颜色 + 深度（RenderTarget 不拥有，图只编排同步）
    ResourceHandle hColor = builder.Import(&viewportRT->GetColorView(), "ViewportColor");
    ResourceHandle hDepth = builder.Import(&viewportRT->GetDepthView(), "ViewportDepth");

    // Pass0 "Scene3D"：清屏 + 深度 eClear；颜色/深度 eStore（深度须保留给 Scene2D 读）
    RenderPassDesc &scene3D = builder.AddPass("Scene3D");
    scene3D.renderArea = renderArea;
    AttachmentDesc colorClear;
    colorClear.resource = hColor;
    colorClear.usage = ResourceUsage::ColorAttachment;
    colorClear.loadOp = vk::AttachmentLoadOp::eClear;
    colorClear.storeOp = vk::AttachmentStoreOp::eStore;
    colorClear.clearValue.color = {clearColor.r, clearColor.g, clearColor.b, clearColor.a};
    scene3D.colorAttachments.push_back(colorClear);
    AttachmentDesc depthClear;
    depthClear.resource = hDepth;
    depthClear.usage = ResourceUsage::DepthStencilAttachment;
    depthClear.loadOp = vk::AttachmentLoadOp::eClear;
    depthClear.storeOp = vk::AttachmentStoreOp::eStore;
    scene3D.depthAttachment = depthClear;
    scene3D.execute = [](PassExecuteContext &ctx) {
        Renderer::Get3DRenderer().FlushScene(*ctx.cmd, *ctx.frame);
    };

    // Pass1 "Scene2D"：叠加世界/UI 精灵。颜色 eLoad；深度 eLoad（读 Scene3D 深度做
    // 遮挡），UI 批 depthTest 关不读写；颜色收尾转 ShaderReadOnlyOptimal 供 ImGui 采样。
    RenderPassDesc &scene2D = builder.AddPass("Scene2D");
    scene2D.renderArea = renderArea;
    AttachmentDesc colorLoad;
    colorLoad.resource = hColor;
    colorLoad.usage = ResourceUsage::ColorAttachment;
    colorLoad.loadOp = vk::AttachmentLoadOp::eLoad;
    colorLoad.storeOp = vk::AttachmentStoreOp::eStore;
    colorLoad.finalLayout = vk::ImageLayout::eShaderReadOnlyOptimal;
    scene2D.colorAttachments.push_back(colorLoad);
    AttachmentDesc depthLoad;
    depthLoad.resource = hDepth;
    depthLoad.usage = ResourceUsage::DepthStencilAttachment;
    depthLoad.loadOp = vk::AttachmentLoadOp::eLoad;
    depthLoad.storeOp = vk::AttachmentStoreOp::eStore;
    scene2D.depthAttachment = depthLoad;
    scene2D.execute = [](PassExecuteContext &ctx) {
        Renderer::Get2DRenderer().FlushScene(*ctx.cmd, *ctx.frame);
    };

    graph.Compile();
    graph.Execute(cmd, frame);

    // 复位为 swapchain 目标（默认）+ 退出延迟录制
    r3d.SetDeferRecording(false);
    r2d.SetDeferRecording(false);
    r3d.SetRenderTarget(nullptr);
    r2d.SetRenderTarget(nullptr);
}

void SceneLayer::OnEvent(Event &event) {
    if (!m_Context->Scene) {
        return;
    }
    const bool inViewport = m_SceneWindowHovered;
    const bool playing = m_Context->Scene->IsPlaying();

    // Play 态按 ESC 退出运行：编辑器快捷键，不落入游戏脚本/相机输入。
    // Stop() 回滚到摆放姿态，随后 UpdateMouseCapture 因 hasFps 变 false 自动解锁鼠标。
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
    const bool mouseCaptured = m_MouseCaptured; // Play 态第一人称已锁鼠标
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
    // 例外 2：第一人称鼠标已锁定（GLFW_CURSOR_DISABLED）——ImGui 悬停判定失效，
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

            // 包围盒线框叠加：世界 AABB（静态盒 + 蒙皮绑定盒，与视锥剔除用同一盒）
            if (m_ShowBounds) {
                DrawWorldBounds(glm::vec2(imagePos.x, imagePos.y));
            }

            // 物理碰撞体线框叠加：盒子 12 棱 + 球体正交圆环（青绿色）
            if (m_ShowColliders) {
                DrawColliders(glm::vec2(imagePos.x, imagePos.y));
            }

            // 第一人称视点标记叠加：十字 + 到脚底的虚线（世界 EyeOffset 投影）
            if (m_ShowFPSEyes) {
                DrawFirstPersonEyes(glm::vec2(imagePos.x, imagePos.y));
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

        // 包围盒线框调试开关（叠加在视口上：静态盒灰、蒙皮绑定盒红）
        ImGui::Checkbox("显示包围盒", &m_ShowBounds);
        // 关节露点红绿开关（叠加在包围盒上，仅在开启显示包围盒时生效）
        ImGui::Checkbox("关节露点", &m_ShowJointDots);
        // 物理碰撞体线框开关（叠加在视口上：盒子/球体青绿色）
        ImGui::Checkbox("显示碰撞体", &m_ShowColliders);
        // 第一人称视点标记开关（叠加在视口上：EyeOffset 十字 + 到脚底虚线）
        ImGui::Checkbox("显示视点", &m_ShowFPSEyes);

        ImGui::TextDisabled("提示：先在左侧 Hierarchy/Properties 中调整实体，再保存/加载验证");
    }

    ImGui::End();
}

// ============================================================
// 场景文件操作：保存 / 加载 / 新建
// ============================================================

void SceneLayer::DrawWorldBounds(const glm::vec2 &imagePos) {
    if (!m_Context->Scene) {
        return;
    }
    const Camera &camera = GetActiveViewCamera();

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

    // 2) 实体级手动摆放盒（BoundingBoxComponent）用黄叠出，便于对照剔除覆盖范围。
    //    额外两层可视化解决「单方向看不出是否完全包围」：
    //      a. 半透明盒面（前亮后暗），直读前后重叠与深度关系；
    //      b. 盒覆盖子树的全部蒙皮关节投影点（盒内绿 / 盒外红），一眼见露骨。
    auto boundsView = m_Context->Scene->Reg().view<TransformComponent, BoundingBoxComponent>();
    const ImU32 bbColor = ImGui::ColorConvertFloat4ToU32(ImVec4(1.0f, 0.80f, 0.10f, 1.0f));
    const ImU32 bbFaceFront = ImGui::ColorConvertFloat4ToU32(ImVec4(1.0f, 0.80f, 0.10f, 0.16f));
    const ImU32 bbFaceBack = ImGui::ColorConvertFloat4ToU32(ImVec4(1.0f, 0.80f, 0.10f, 0.05f));
    const glm::vec3 camPos = camera.GetPosition();

    // 世界空间 AABB 的 6 个面（下标引用 cW[8] 角点 + 向外法线）
    struct BoxFace {
        int idx[4];
        glm::vec3 n;
    };
    static const BoxFace kBoxFaces[6] = {
        {{4, 5, 7, 6}, {0.0f, 0.0f, 1.0f}}, // +Z
        {{0, 1, 3, 2}, {0.0f, 0.0f, -1.0f}}, // -Z
        {{2, 3, 7, 6}, {0.0f, 1.0f, 0.0f}}, // +Y
        {{0, 1, 5, 4}, {0.0f, -1.0f, 0.0f}}, // -Y
        {{1, 3, 7, 5}, {1.0f, 0.0f, 0.0f}}, // +X
        {{0, 2, 6, 4}, {-1.0f, 0.0f, 0.0f}}, // -X
    };

    // 蒙皮关节查重（多个盒共享同一子树时避免重复画点）
    std::unordered_set<entt::entity> jointDrawn;

    for (auto entity : boundsView) {
        const auto &tc = boundsView.get<TransformComponent>(entity);
        const auto &bb = boundsView.get<BoundingBoxComponent>(entity);
        if (!bb.IsValid()) {
            continue; // 未摆放，不参与剔除也不画
        }
        AABB local;
        local.min = bb.minCorner();
        local.max = bb.maxCorner();
        const AABB world = local.Transformed(tc.GetWorldMatrix());

        // a. 半透明面：面法线朝向相机（看到的是外表面）→ 亮，背向 → 暗，
        //    区分前后两层，解决单视角下「前后两面重叠分不清」的问题
        const glm::vec3 wc[8] = {
            {world.min.x, world.min.y, world.min.z}, {world.max.x, world.min.y, world.min.z},
            {world.min.x, world.max.y, world.min.z}, {world.max.x, world.max.y, world.min.z},
            {world.min.x, world.min.y, world.max.z}, {world.max.x, world.min.y, world.max.z},
            {world.min.x, world.max.y, world.max.z}, {world.max.x, world.max.y, world.max.z},
        };
        for (const BoxFace &f : kBoxFaces) {
            glm::vec3 faceCenter(0.0f);
            ImVec2 pts[4];
            bool frontAll = true;
            for (int k = 0; k < 4; ++k) {
                const glm::vec3 &p = wc[f.idx[k]];
                faceCenter += p;
                glm::vec2 scr;
                if (!ProjectWorldToScreen(viewProjGL, p, origin, size, scr)) {
                    frontAll = false;
                    break; // 面有角点在相机背面，跳过避免投影发散
                }
                pts[k] = ImVec2(scr.x, scr.y);
            }
            if (!frontAll) {
                continue;
            }
            faceCenter *= 0.25f;
            const bool facingCam = glm::dot(f.n, camPos - faceCenter) > 0.0f;
            dl->AddConvexPolyFilled(pts, 4, facingCam ? bbFaceFront : bbFaceBack);
        }

        // 黄线框最后画（压在面上，线清晰可读）
        drawWorldAabb(world, bbColor);

        // b. 露点检查：盒覆盖子树的全部蒙皮关节（盒内绿 / 盒外红）。
        //    Skeleton 关节是世界实体（SkinDef::joints），取世界矩阵平移列即骨骼枢轴点。
        if (m_ShowJointDots) {
            std::vector<Entity> stack;
            stack.push_back(Entity(entity, m_Context->Scene.get()));
            while (!stack.empty()) {
                const Entity n = stack.back();
                stack.pop_back();
                const entt::entity h = static_cast<entt::entity>(n);
                if (const auto *sc = m_Context->Scene->Reg().try_get<SkinComponent>(h)) {
                    for (const entt::entity jh : sc->joints()) {
                        if (jointDrawn.count(jh)) {
                            continue; // 已被别的盒画过，跳过
                        }
                        jointDrawn.insert(jh);
                        const auto *jtc = m_Context->Scene->Reg().try_get<TransformComponent>(jh);
                        if (!jtc) {
                            continue;
                        }
                        const glm::vec3 jp = glm::vec3(jtc->GetWorldMatrix()[3]);
                        const bool inside = jp.x >= world.min.x && jp.x <= world.max.x
                                            && jp.y >= world.min.y && jp.y <= world.max.y
                                            && jp.z >= world.min.z && jp.z <= world.max.z;
                        glm::vec2 scr;
                        if (!ProjectWorldToScreen(viewProjGL, jp, origin, size, scr)) {
                            continue;
                        }
                        const ImU32 jc = ImGui::ColorConvertFloat4ToU32(
                            inside
                                ? ImVec4(0.20f, 0.90f, 0.30f, 1.0f) // 盒内：绿
                                : ImVec4(0.95f, 0.30f, 0.25f, 1.0f)); // 盒外：红
                        // 圆点 + 深色描边，保证在亮/暗背景上都可读
                        dl->AddCircleFilled(ImVec2(scr.x, scr.y), 4.0f, jc);
                        dl->AddCircle(ImVec2(scr.x, scr.y), 4.0f, IM_COL32(0, 0, 0, 200));
                    }
                }
                for (const auto &child : m_Context->Scene->GetChildren(n)) {
                    stack.push_back(child); // 继续下钻同棵子树
                }
            }
        }
    }
}

// ============================================================
// 物理碰撞体线框叠加
// ============================================================

void SceneLayer::DrawColliders(const glm::vec2 &imagePos) {
    if (!m_Context->Scene) {
        return;
    }
    const Camera &camera = GetActiveViewCamera();

    // 还原 OpenGL 投影（与 DrawWorldBounds 同款，保证线与画面/gizmo 对齐）
    glm::mat4 projGL = camera.GetProj();
    projGL[1][1] *= -1.0f;
    const glm::mat4 viewProjGL = projGL * camera.GetView();

    const glm::vec2 origin{imagePos.x, imagePos.y};
    const glm::vec2 size{m_ViewportSize.x, m_ViewportSize.y};
    ImDrawList *dl = ImGui::GetWindowDrawList();

    // 立方体 12 条边（下标约定与 DrawWorldBounds 一致）
    static const int kEdges[12][2] = {
        {0, 1}, {1, 3}, {3, 2}, {2, 0},
        {4, 5}, {5, 7}, {7, 6}, {6, 4},
        {0, 4}, {1, 5}, {2, 6}, {3, 7},
    };
    constexpr float kPi = 3.14159265358979f;

    // 碰撞体线框颜色（青绿，与灰网格盒/黄实体盒区分）
    const ImU32 colliderColor =
        ImGui::ColorConvertFloat4ToU32(ImVec4(0.25f, 0.80f, 0.90f, 1.0f));

    // 线段 → 屏幕坐标：先裁剪到相机近平面再投影，不做相机背面裁剪。
    // 一端在相机背面时把端点裁到近平面上，整段在视线内的照常画出，
    // 避免大地这类大框在相机靠近/进入时线被整批丢弃。
    // 返回 false 表示线段完全在相机背面，无可见部分。
    const auto projectSegment = [&](const glm::vec3 &a, const glm::vec3 &b,
                                    glm::vec2 &sa, glm::vec2 &sb) -> bool {
        constexpr float kNearW = 1e-3f;
        const glm::vec4 ca = viewProjGL * glm::vec4(a, 1.0f);
        const glm::vec4 cb = viewProjGL * glm::vec4(b, 1.0f);
        const bool aFront = ca.w > kNearW;
        const bool bFront = cb.w > kNearW;
        if (!aFront && !bFront) {
            return false; // 整段在相机背面
        }
        glm::vec4 cA = ca, cB = cb;
        if (aFront && !bFront) { // b 在背面：沿线段插值到近平面
            const float t = (kNearW - cb.w) / (ca.w - cb.w);
            cB = cb + (ca - cb) * t;
            cB.w = kNearW;
        } else if (!aFront && bFront) { // a 在背面：沿线段插值到近平面
            const float t = (kNearW - ca.w) / (cb.w - ca.w);
            cA = ca + (cb - ca) * t;
            cA.w = kNearW;
        }
        const auto toPos = [&](const glm::vec4 &c) {
            return glm::vec2(origin.x + (0.5f + c.x / c.w * 0.5f) * size.x,
                             origin.y + (0.5f - c.y / c.w * 0.5f) * size.y);
        };
        sa = toPos(cA);
        sb = toPos(cB);
        return true;
    };

    // 画一条世界坐标线段（内部经过近平面裁剪）
    const auto drawWorldSegment = [&](const glm::vec3 &a, const glm::vec3 &b) {
        glm::vec2 sa, sb;
        if (projectSegment(a, b, sa, sb)) {
            dl->AddLine(ImVec2(sa.x, sa.y), ImVec2(sb.x, sb.y), colliderColor, 1.2f);
        }
    };

    // 画胶囊线框：N 条经线剖面（上球帽弧 + 圆柱母线 + 下球帽弧）+ 三条圆环（±H 赤道 / y=0 中段）。
    // center 为胶囊中心，capsuleRot 为胶囊轴向旋转（沿局部 Y），radius 半径，halfHeight 圆柱段半高。
    // 刚体胶囊与角色控制器胶囊共用，保证两处线框形态一致。
    const auto drawCapsuleMesh = [&](const glm::vec3 &center, const glm::quat &capsuleRot,
                                     float radius, float halfHeight) {
        constexpr int kMeri = 8;    // 经线数量
        constexpr int kArcSegs = 8; // 每条半球帽弧的分段数
        for (int m = 0; m < kMeri; ++m) {
            const float ang = (2.0f * kPi * m) / kMeri;
            const float dx = std::cos(ang), dz = std::sin(ang);
            // 上球帽弧：极点 (0, +H+R) → 赤道 (R, +H)
            glm::vec3 prev = center + capsuleRot * glm::vec3(0.0f, halfHeight + radius, 0.0f);
            for (int i = 1; i <= kArcSegs; ++i) {
                const float a = (static_cast<float>(i) / kArcSegs) * kPi * 0.5f;
                const glm::vec3 cur = center + capsuleRot * glm::vec3(
                    std::sin(a) * radius * dx,
                    halfHeight + std::cos(a) * radius,
                    std::sin(a) * radius * dz);
                drawWorldSegment(prev, cur);
                prev = cur;
            }
            // 圆柱母线：下赤道 → 上赤道
            drawWorldSegment(
                center + capsuleRot * glm::vec3(radius * dx, +halfHeight, radius * dz),
                center + capsuleRot * glm::vec3(radius * dx, -halfHeight, radius * dz));
            // 下球帽弧：赤道 (R, -H) → 极点 (0, -H-R)
            glm::vec3 prev2 = center + capsuleRot * glm::vec3(radius * dx, -halfHeight, radius * dz);
            for (int i = 1; i <= kArcSegs; ++i) {
                const float a = (static_cast<float>(i) / kArcSegs) * kPi * 0.5f;
                const glm::vec3 cur = center + capsuleRot * glm::vec3(
                    std::sin(a) * radius * dx,
                    -halfHeight - std::cos(a) * radius,
                    std::sin(a) * radius * dz);
                drawWorldSegment(prev2, cur);
                prev2 = cur;
            }
        }
        // 圆环绕 y = ±H（赤道/圆柱边）与 y = 0（圆柱中段）各画一圈
        constexpr int kSegs = 24; // 圆环分段数
        for (int ring = 0; ring < 3; ++ring) {
            const float y = (ring == 0) ? -halfHeight : ((ring == 1) ? 0.0f : halfHeight);
            for (int i = 0; i < kSegs; ++i) {
                const float a0 = (2.0f * kPi * i) / kSegs;
                const float a1 = (2.0f * kPi * (i + 1)) / kSegs;
                drawWorldSegment(
                    center + capsuleRot * glm::vec3(std::cos(a0) * radius, y, std::sin(a0) * radius),
                    center + capsuleRot * glm::vec3(std::cos(a1) * radius, y, std::sin(a1) * radius));
            }
        }
    };

    // 遍历刚体实体，仅绘制真正进入了物理世界的碰撞体（需同时具备刚体 + 碰撞体）。
    // 变换语义与 PhysicsWorld::BuildShapeForEntity 一致：用实体局部 TRS，
    // 半尺寸/半径乘比例烘焙进形状，Offset 只旋转不乘比例。
    const auto rbView = m_Context->Scene->Reg().view<TransformComponent, RigidBodyComponent>();
    for (auto entity : rbView) {
        const auto &tc = rbView.get<TransformComponent>(entity);
        const auto *box = m_Context->Scene->Reg().try_get<BoxColliderComponent>(entity);
        const auto *sphere = m_Context->Scene->Reg().try_get<SphereColliderComponent>(entity);
        const auto *capsule = m_Context->Scene->Reg().try_get<CapsuleColliderComponent>(entity);
        if (!box && !sphere && !capsule) {
            continue; // 无碰撞体，未创建刚体
        }
        const glm::quat &rot = tc.Rotation;

        // ---- 盒子碰撞体：中心 = T + R*Offset，半尺寸含比例；DrawDebug 关闭则不画 ----
        if (box && box->DrawDebug) {
            const glm::vec3 center = tc.Translation + rot * box->Offset;
            const glm::vec3 half = box->HalfExtents * tc.Scale;

            // 外棱：12 条边（角点序与 kEdges 一致：bit0=X, bit1=Y, bit2=Z）
            const glm::vec3 wc[8] = {
                center + rot * (half * glm::vec3(-1.0f, -1.0f, -1.0f)),
                center + rot * (half * glm::vec3( 1.0f, -1.0f, -1.0f)),
                center + rot * (half * glm::vec3(-1.0f,  1.0f, -1.0f)),
                center + rot * (half * glm::vec3( 1.0f,  1.0f, -1.0f)),
                center + rot * (half * glm::vec3(-1.0f, -1.0f,  1.0f)),
                center + rot * (half * glm::vec3( 1.0f, -1.0f,  1.0f)),
                center + rot * (half * glm::vec3(-1.0f,  1.0f,  1.0f)),
                center + rot * (half * glm::vec3( 1.0f,  1.0f,  1.0f)),
            };
            for (int e = 0; e < 12; ++e) {
                drawWorldSegment(wc[kEdges[e][0]], wc[kEdges[e][1]]);
            }
        }

        // ---- 球体碰撞体：中心 = T + R*Offset，半径取比例最大值（与 Jolt 一致）；DrawDebug 关闭则不画 ----
        if (sphere && sphere->DrawDebug) {
            const glm::vec3 center = tc.Translation + rot * sphere->Offset;
            const float radius = sphere->Radius
                                 * std::max({tc.Scale.x, tc.Scale.y, tc.Scale.z});
            // 3 个正交大圆环（XY/XZ/YZ 平面）构成线框球；环旋转随刚体取向
            constexpr int kSegs = 24;
            for (int plane = 0; plane < 3; ++plane) {
                for (int i = 0; i < kSegs; ++i) {
                    const float a0 = (2.0f * kPi * i) / kSegs;
                    const float a1 = (2.0f * kPi * (i + 1)) / kSegs;
                    glm::vec3 d0{0.0f, 0.0f, 0.0f}, d1{0.0f, 0.0f, 0.0f};
                    if (plane == 0) {
                        d0 = {std::cos(a0), std::sin(a0), 0.0f};
                        d1 = {std::cos(a1), std::sin(a1), 0.0f};
                    } else if (plane == 1) {
                        d0 = {std::cos(a0), 0.0f, std::sin(a0)};
                        d1 = {std::cos(a1), 0.0f, std::sin(a1)};
                    } else {
                        d0 = {0.0f, std::cos(a0), std::sin(a0)};
                        d1 = {0.0f, std::cos(a1), std::sin(a1)};
                    }
                    drawWorldSegment(center + rot * (d0 * radius),
                                     center + rot * (d1 * radius));
                }
            }
        }

        // ---- 胶囊碰撞体：主轴沿实体局部某轴（默认 Y），旋转跟随 Transform 并叠加轴向烘焙；DrawDebug 关闭则不画 ----
        if (capsule && capsule->DrawDebug) {
            const glm::vec3 center = tc.Translation + rot * capsule->Offset;
            const float radius = capsule->Radius
                                 * std::max({tc.Scale.x, tc.Scale.y, tc.Scale.z});
            // 半高缩放跟随胶囊主轴对应的轴分量（与 PhysicsWorld::BuildShapeForEntity 一致）
            float heightScale = tc.Scale.y;
            if (capsule->Axis == CapsuleAxis::X)
                heightScale = tc.Scale.x;
            else if (capsule->Axis == CapsuleAxis::Z)
                heightScale = tc.Scale.z;
            const float halfHeight = capsule->HalfHeight * heightScale;

            // 轴向烘焙（与形状构建一致：X: 绕局部 Z -90°，Z: 绕局部 X +90°），与实体旋转复合后作用于局部坐标
            glm::quat axisRot(1.0f, 0.0f, 0.0f, 0.0f);
            constexpr float kSqrtHalf = 0.707106781f;
            if (capsule->Axis == CapsuleAxis::X)
                axisRot = glm::quat(kSqrtHalf, 0.0f, 0.0f, -kSqrtHalf);
            else if (capsule->Axis == CapsuleAxis::Z)
                axisRot = glm::quat(kSqrtHalf, kSqrtHalf, 0.0f, 0.0f);
            const glm::quat capsuleRot = rot * axisRot;

            drawCapsuleMesh(center, capsuleRot, radius, halfHeight);
        }
    }

    // ---- 角色控制器胶囊（CharacterVirtual）：角色实体不挂 RigidBodyComponent，单独遍历 ----
    // 形状语义与 PhysicsWorld::ProcessPendingCharacters 一致：半径 = cc.Radius（不乘实体比例），
    // 圆柱半高 = H/2 - R，胶囊底部对齐脚底（Transform.Translation），中心在脚底上方 H/2 处。
    const auto charView = m_Context->Scene->Reg().view<
        TransformComponent, CharacterControllerComponent>();
    for (auto entity : charView) {
        const auto &tc = charView.get<TransformComponent>(entity);
        const auto &cc = charView.get<CharacterControllerComponent>(entity);

        const float cylHalf = std::max(cc.Height * 0.5f - cc.Radius, 0.0f);
        // 轴向烘焙 + 沿所选轴把底部抬到脚底（与 ProcessPendingCharacters 的形状构建一致）
        glm::quat axisRot(1.0f, 0.0f, 0.0f, 0.0f);
        glm::vec3 shift(0.0f);
        constexpr float kSqrtHalf = 0.707106781f;
        switch (cc.Axis) {
        case CapsuleAxis::X:
            axisRot = glm::quat(kSqrtHalf, 0.0f, 0.0f, -kSqrtHalf);
            shift = {cc.Height * 0.5f, 0.0f, 0.0f};
            break;
        case CapsuleAxis::Z:
            axisRot = glm::quat(kSqrtHalf, kSqrtHalf, 0.0f, 0.0f);
            shift = {0.0f, 0.0f, cc.Height * 0.5f};
            break;
        default: // Y（默认）
            shift = {0.0f, cc.Height * 0.5f, 0.0f};
            break;
        }
        const glm::vec3 center = tc.Translation + tc.Rotation * (shift + cc.Offset);
        drawCapsuleMesh(center, tc.Rotation * axisRot, cc.Radius, cylHalf);
    }
}

void SceneLayer::DrawFirstPersonEyes(const glm::vec2 &imagePos) {
    if (!m_Context->Scene) {
        return;
    }
    const Camera &camera = GetActiveViewCamera();

    // 还原 OpenGL 投影（与 DrawColliders/DrawWorldBounds 同款，保证标记与画面/gizmo 对齐）
    glm::mat4 projGL = camera.GetProj();
    projGL[1][1] *= -1.0f;
    const glm::mat4 viewProjGL = projGL * camera.GetView();

    const glm::vec2 origin{imagePos.x, imagePos.y};
    const glm::vec2 size{m_ViewportSize.x, m_ViewportSize.y};
    ImDrawList *dl = ImGui::GetWindowDrawList();

    // 视点世界坐标投影到屏幕。坐标 = 角色脚底 + EyeOffset（与 Scene::UpdateFirstPersonCamera
    // 一致：角色 yaw 每帧与相机同步后，绕 up 旋转 EyeOffset 恒等于原向量）。
    // 单个投影点不裁剪（点若在相机背面，投影后屏幕坐标越界、AddLine 自然不画）。
    const auto projectPoint = [&](const glm::vec3 &world) -> glm::vec2 {
        const glm::vec4 c = viewProjGL * glm::vec4(world, 1.0f);
        return glm::vec2(origin.x + (0.5f + c.x / c.w * 0.5f) * size.x,
                         origin.y + (0.5f - c.y / c.w * 0.5f) * size.y);
    };

    // 视点标记颜色（橙，区分青绿碰撞体/灰盒/红绑定盒）
    const ImU32 eyeColor = ImGui::ColorConvertFloat4ToU32(ImVec4(1.00f, 0.65f, 0.10f, 1.0f));
    const ImU32 footColor = ImGui::ColorConvertFloat4ToU32(ImVec4(1.00f, 0.65f, 0.10f, 0.45f));

    const auto fpsView = m_Context->Scene->Reg().view<TransformComponent, CharacterControllerComponent,
                                                        FirstPersonCameraComponent>();
    for (auto entity : fpsView) {
        const auto &tc = fpsView.get<TransformComponent>(entity);
        const auto &fp = fpsView.get<FirstPersonCameraComponent>(entity);
        if (!fp.Enabled) {
            continue; // 关闭视点的角色不画
        }
        const glm::vec3 foot = tc.Translation;
        const glm::vec3 eye = foot + fp.EyeOffset;
        const glm::vec2 sEye = projectPoint(eye);
        const glm::vec2 sFoot = projectPoint(foot);

        // 到脚底的虚线：先画粗的深色底（压场景高亮），再叠半透明橙色，视觉更清楚
        dl->AddLine(ImVec2(sFoot.x, sFoot.y), ImVec2(sEye.x, sEye.y), IM_COL32(0, 0, 0, 160), 3.0f);
        dl->AddLine(ImVec2(sFoot.x, sFoot.y), ImVec2(sEye.x, sEye.y), footColor, 1.5f);

        // 视点十字（水平 12px × 垂直 12px），随屏幕朝向、不随角色旋转
        constexpr float kHalf = 6.0f;
        dl->AddLine(ImVec2(sEye.x - kHalf, sEye.y), ImVec2(sEye.x + kHalf, sEye.y), eyeColor, 2.0f);
        dl->AddLine(ImVec2(sEye.x, sEye.y - kHalf), ImVec2(sEye.x, sEye.y + kHalf), eyeColor, 2.0f);
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

    // 保存按 Edit 视角（决策 5.5）：若正在 Play，先 Stop 回滚到摆放姿态再落盘，
    // 避免把模拟后的混乱 Transform 写进场景文件
    if (m_Context->Scene->IsPlaying()) {
        m_Context->Scene->Stop();
    }

    // 序列化器无状态、操作完即弃，按需创建为局部变量即可；
    // 网格/纹理/材质由全局管理器持有，序列化器自身不拥有资源
    SceneSerializer serializer(m_Context->Scene.get());
    serializer.Serialize(filepath);
}

bool SceneLayer::LoadSceneFromFile(std::string_view filepath) {
    // 如果场景不存在，先创建
    if (!m_Context->Scene) {
        m_Context->Scene = std::make_unique<Scene>();
    }

    // 序列化器无状态、按需创建为局部变量（纹理/材质/网格由全局管理器加载，无需 device）
    SceneSerializer serializer(m_Context->Scene.get());

    if (!serializer.Deserialize(filepath.data())) {
        return false;
    }
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
    // 创建新场景（序列化器无状态，仅在保存/加载时按需创建局部变量）
    m_Context->Scene = std::make_unique<Scene>();
}

void SceneLayer::ReloadAllScripts() {
    if (m_Context && m_Context->Scene) {
        m_Context->Scene->GetScriptEngine().ReloadAll();
    }
}

} // namespace GE