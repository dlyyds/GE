#include "GameLayer.h"

#include "GE/Core/Application.h"
#include "GE/Core/Base.h"
#include "GE/Core/GEWindow.h"
#include "GE/Core/KeyCodes.h"
#include "GE/Core/Log.h"
#include "GE/Debug/Profiler.h"
#include "GE/Events/Event.h"
#include "GE/Events/KeyEvent.h"
#include "GE/Render/AssetManager.h"
#include "GE/Render/Renderer.h"
#include "GE/Render/Renderer3D.h"
#include "GE/Render/SceneRenderPasses.h"
#include "GE/Scene/Components.h"
#include "GE/Scene/Entity.h"
#include "GE/Scene/Scene.h"

#include <glm/glm.hpp>
#include <imgui.h>

#include <string>
#include <utility>

namespace GE {

GameLayer::GameLayer(GameConfig config) : Layer("GameLayer"), m_Config(std::move(config)) {
}

GameLayer::~GameLayer() = default;

void GameLayer::OnAttach() {
    // 场景 pass 直画背缓冲：末尾 UIPass 改用 eLoad 保留场景结果。须在首帧 EndFrame 之前设置。
    Renderer::Get().SetSceneToBackbuffer(true);

    ApplyWindowConfig();

    // 兜底自由视角：从侧后方看向原点，便于场景没相机（或 --free-camera）时也能看到东西
    m_FallbackCamera.SetMode(Camera::Mode::FreeLook);
    m_FallbackCamera.SetPerspective(60.0f, 16.0f / 9.0f, 0.1f, 1000.0f);
    m_FallbackCamera.SetPosition(glm::vec3(0.0f, 2.0f, 6.0f));
    m_FallbackCamera.SetYawPitch(0.0f, -10.0f);

    // 场景构造需 Renderer 已就绪（脚本基准目录取自资源根），故放在 Renderer 构造之后
    m_Scene = std::make_unique<Scene>();

    const std::string scenePath =
        Renderer::GetAssetManager().ResolveCanonical(m_Config.scene);
    if (!m_Scene->LoadFromFile(scenePath)) {
        GE_CORE_ERROR("GameLayer: 入口场景加载失败，退出：{0}", scenePath);
        Application::Get().Close();
        return;
    }
    GE_CORE_INFO("GameLayer: 已加载入口场景 {0}", scenePath);

    ApplyRenderingConfig();

    // 进入运行时模拟：脚本 / 物理 / 跟随相机的总开关（Edit 态下这些一律不跑）
    m_Scene->Play();

    // 触屏控制。合成的事件经 DispatchInputToScene 走与真实输入完全相同的那条路径，
    // 所以它必须晚于场景创建（要拿 InputState），且早于第一帧事件处理。
    m_Touch = std::make_unique<TouchControls>(
        [this](Event &e) { DispatchInputToScene(e); },
        m_Scene->GetMutableInputState());
    {
        const Window &window = Application::Get().GetWindow();
        m_Touch->SetViewport(window.GetWidth(), window.GetHeight());
    }
}

void GameLayer::OnDetach() {
    // 先撤销触摸合成的按键/左键（还在按住的状态会导致场景 Stop 后仍有键被记为 held；
    // 且必须在 m_Scene 仍存活时做 —— 合成事件要经过场景）
    if (m_Touch) {
        m_Touch->ReleaseAll();
        m_Touch.reset();
    }

    // 先回滚到摆放姿态（停音频/物理），再释放场景；GPU 资源由全局管理器持有、随 Renderer 释放
    if (m_Scene) {
        m_Scene->Stop();
    }
    m_Scene.reset();

    // 退出时恢复光标，否则锁定的鼠标会留给下一次启动 / 其它程序
    if (m_MouseCaptured) {
        Application::Get().GetWindow().SetCursorMode(CursorMode::Normal);
        m_MouseCaptured = false;
    }
}

void GameLayer::ApplyWindowConfig() {
    Window &window = Application::Get().GetWindow();
    window.SetTitle(m_Config.window.title);

    // 基类构造里按编辑器习惯强制最大化过，不先撤销则下面的 Resize 不生效
    window.SetMaximized(false);

    if (m_Config.window.fullscreen) {
        window.SetWindowMode(WindowMode::Fullscreen);
    } else {
        window.Resize({m_Config.window.width, m_Config.window.height});
    }

    // 垂直同步：OFF 已是引擎启动时的呈现模式，不必重建 swapchain；只在要开时切
    if (m_Config.window.vsync) {
        Application::Get().SetPresentMode(VsyncMode::ON);
    }
}

void GameLayer::ApplyRenderingConfig() {
    Renderer3D &renderer3D = Renderer::Get3DRenderer();
    if (m_Config.rendering.deferred.has_value()) {
        renderer3D.SetDeferred(*m_Config.rendering.deferred);
    }
    if (m_Config.rendering.tonemap.has_value()) {
        renderer3D.SetTonemapEnabled(*m_Config.rendering.tonemap);
    }
    if (m_Config.rendering.bloom.has_value()) {
        renderer3D.SetBloomEnabled(*m_Config.rendering.bloom);
    }

    if (m_Config.rendering.culling.has_value() && m_Scene) {
        const std::string &mode = *m_Config.rendering.culling;
        if (mode == "mesh") {
            m_Scene->SetCullingMode(Scene::CullingMode::Mesh);
        } else if (mode == "submesh") {
            m_Scene->SetCullingMode(Scene::CullingMode::SubMesh);
        } else {
            GE_CORE_WARN("game.cfg: rendering.culling 取值 '{0}' 无效（应为 mesh / submesh），忽略", mode);
        }
    }
}

void GameLayer::UpdateMouseCapture() {
#ifdef GE_PLATFORM_ANDROID
    // 触屏设备上整段跳过。两个理由：
    //   1. 没有光标可锁/可恢复，SDL 的相对鼠标模式在这里没有意义；
    //   2. 更实际的是 ResetMouseBaseline —— 它把增量基准设成 SDL 的真实光标位置，
    //      而触屏下这个值恒为 (0,0)，与 TouchControls 自己维护的虚拟鼠标位置不是一回事，
    //      反而会在第一次拖视角时造出一次跳变。而它原本要防的"锁定瞬间坐标跳变"在触屏上
    //      根本不存在。
    return;
#else
    Window &window = Application::Get().GetWindow();
    if (!window.GetNativeWindow() || !m_Scene) {
        return;
    }

    // 有跟随相机角色的场景才锁鼠标，否则无法持续转向
    auto followView = m_Scene->Reg().view<TransformComponent, CharacterControllerComponent,
                                           FollowCameraComponent>();
    const bool hasFollowCam = (followView.begin() != followView.end());

    if (hasFollowCam && !m_MouseCaptured) {
        // 锁定瞬间：光标被锁到窗口中心、位置跳变 → 重置增量基准，防首帧 delta 爆值
        m_Scene->GetMutableInputState().ResetMouseBaseline(window.GetCursorPosition());
        window.SetCursorMode(CursorMode::Disabled);
        m_MouseCaptured = true;
    } else if (!hasFollowCam && m_MouseCaptured) {
        m_Scene->GetMutableInputState().ResetMouseBaseline(window.GetCursorPosition());
        window.SetCursorMode(CursorMode::Normal);
        m_MouseCaptured = false;
    }
#endif
}

Camera &GameLayer::ActiveCamera(float aspect) {
    if (!m_Config.freeCamera) {
        if (Entity gameCam = m_Scene->GetPrimaryCameraEntity();
            gameCam && gameCam.HasComponent<CameraComponent>()) {
            auto &cameraComponent = gameCam.GetComponent<CameraComponent>();
            if (!cameraComponent.FixedAspectRatio) {
                cameraComponent.CameraInstance.SetAspect(aspect);
            }
            return cameraComponent.CameraInstance;
        }
    }

    m_FallbackCamera.SetAspect(aspect);
    return m_FallbackCamera;
}

void GameLayer::OnUpdate(Timestep &ts) {
    GE_PROFILE_SCOPE("GameLayer::OnUpdate");

    if (!m_Scene) {
        return;
    }

    UpdateMouseCapture();

    const Window &window = Application::Get().GetWindow();
    const uint32_t width = window.GetWidth();
    const uint32_t height = window.GetHeight();
    if (width == 0 || height == 0) {
        return;   // 最小化：跳过本帧
    }

    // 触屏：先同步视口与视角策略，再把摇杆结算成键事件。必须排在仿真推进**之前** ——
    // 键事件由 Scene::OnUpdate3DSimulation 里的 BeginFrameInput 结算，晚一步就慢一帧。
    if (m_Touch) {
        m_Touch->SetViewport(width, height);
        m_Touch->SetLookEmitsLeftButton(UsesFallbackCamera());
        m_Touch->Update();
    }

    const float aspect = static_cast<float>(width) / static_cast<float>(height);
    m_Scene->OnViewportResize(width, height);

    // 仿真推进（脚本 / 物理 / 跟随相机 / 动画 / 世界矩阵 / 蒙皮）必须在取渲染相机矩阵之前，
    // 否则本帧 view 用的是上一帧的相机状态（第三人称会明显抖动）
    m_Scene->OnUpdate3DSimulation(ts);

    Camera &camera = ActiveCamera(aspect);
    const glm::mat4 view = camera.GetView();
    const glm::mat4 projection = camera.GetProj();
    const glm::vec3 cameraPos = camera.GetPosition();
    const glm::vec4 clearColor{0.1f, 0.1f, 0.15f, 1.0f};

    Renderer::Get3DRenderer().SetExposure(camera.GetExposure());

    // 渲染采集（3D/2D 批次先快照，不录制命令）
    m_Scene->Render3D(view, projection, cameraPos, clearColor);

    // 注册场景 pass：颜色 = swapchain 背缓冲（BeginFrame 缓存的句柄），深度 = 帧目标深度。
    // 末尾保持附件态，交给 Renderer::EndFrame 的 UIPass 收尾转 PresentSrc。
    auto &builder = Renderer::Get().GetFrameGraphBuilder();
    ResourceHandle hDepth = builder.Import(&Renderer::GetFrameDepthView(), "BackbufferDepth");
    RecordScenePasses(builder, Renderer::Get().GetFrameSwapchainHandle(), hDepth,
                      vk::Extent2D{width, height},
                      vk::ImageLayout::eColorAttachmentOptimal, clearColor);
}

void GameLayer::OnEvent(Event &event) {
    if (!m_Scene) {
        // OnAttach 期间窗口事件（如全屏切换触发的 resize）会走到这里
        return;
    }

    // 触屏事件先由触屏控制层仲裁（它合成的键/鼠标事件会经 DispatchInputToScene 回到
    // 下面这条路径，即"合成输入与真实输入走同一条路"）。触摸事件一律被消费
    if (m_Touch && m_Touch->OnEvent(event)) {
        return;
    }

    // ESC 退出：全屏下没有窗口边框，这是唯一出口
    EventDispatcher escDispatcher(event);
    bool escPressed = false;
    escDispatcher.Dispatch<KeyPressedEvent>([&](KeyPressedEvent &e) {
        if (e.GetKeyCode() == Key::Escape) {
            escPressed = true;
            return true;
        }
        return false;
    });
    if (escPressed) {
        Application::Get().Close();
        return;
    }

    DispatchInputToScene(event);
}

void GameLayer::DispatchInputToScene(Event &event) {
    // 运行时整窗即视口（等价编辑器「Play + 视口悬停」）：场景主相机收相机导航输入
    m_Scene->SetProcessCameraInput(true);
    m_Scene->OnEvent(event);

    // 场景没有主相机（或强制 --free-camera）时，内置自由视角接管导航
    if (UsesFallbackCamera()) {
        m_FallbackCamera.OnEvent(event);
    }
}

bool GameLayer::UsesFallbackCamera() {
    // 必须与 ActiveCamera 的选择保持一致：那边也是"非 freeCamera 且有主相机"才用场景相机
    return m_Config.freeCamera || !m_Scene->GetPrimaryCameraEntity();
}

void GameLayer::OnImGuiRender() {
    DrawTouchOverlay();
}

void GameLayer::DrawTouchOverlay() {
    if (!m_Touch || !m_Touch->IsStickActive()) {
        return;
    }

    // 用 foreground draw list 直接画，**不创建 ImGui 窗口** —— 窗口会参与命中测试、
    // 声称捕获鼠标，而这里画的只是个指示器，不需要也不应该拦截输入。
    ImDrawList *drawList = ImGui::GetForegroundDrawList();
    const glm::vec2 center = m_Touch->GetStickCenterPx();
    const float radius = m_Touch->GetStickRadiusPx();

    const ImVec2 c(center.x, center.y);
    drawList->AddCircleFilled(c, radius, IM_COL32(255, 255, 255, 28), 48);
    drawList->AddCircle(c, radius, IM_COL32(255, 255, 255, 90), 48, 2.0f);

    const glm::vec2 knob = m_Touch->GetStickKnobPx();
    drawList->AddCircleFilled(ImVec2(knob.x, knob.y), radius * 0.4f, IM_COL32(255, 255, 255, 110), 32);
}

} // namespace GE
