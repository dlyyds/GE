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

#include <algorithm>
#include <array>

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

void SceneLayer::RecordScenePasses(RenderTarget &viewportRT, const glm::vec4 &clearColor) {
    // ── 向本帧渲染图注册场景 pass：Scene3D → Scene2D，或延迟链
    //    GBuffer → Lighting → Transparent(HDR) → Tonemap → Scene2D ──
    // 图对象与 Builder 由 Renderer 托管（GetFrameGraphBuilder），每帧 BeginFrame
    // 末尾已 Reset；这里只 Import 视口颜色/深度并声明 pass 读写，命令录制与
    // Execute 统一在 Renderer::EndFrame（ImGui 上屏前）完成。故此处不复位
    // defer / RenderTarget——复位也由 Renderer 在 Execute 后统一做。
    auto &b = Renderer::Get().GetFrameGraphBuilder();

    const auto extent = viewportRT.GetExtent();
    vk::Rect2D renderArea{{0, 0}, {extent.width, extent.height}};

    // 外部资源：离屏颜色 + 深度（RenderTarget 不拥有，图只编排同步）
    ResourceHandle hColor = b.Import(&viewportRT.GetColorView(), "ViewportColor");
    ResourceHandle hDepth = b.Import(&viewportRT.GetDepthView(), "ViewportDepth");

    if (Renderer::Get3DRenderer().IsDeferred()) {
        // GBuffer 虚拟资源：由渲染图池本帧解析分配，屏障/布局/生命周期由图承接。
        // 资源名按用途命名，RenderDoc 纹理视图一眼可认：
        //   Albedo 反照率(+着色模型哨兵) / Normal 世界法线 / WorldPos 世界坐标(+标量A) / Emissive 自发光(+标量B)
        RenderGraphResourceDesc gdesc;
        gdesc.extent = extent;
        gdesc.samples = vk::SampleCountFlagBits::e1;
        gdesc.format = vk::Format::eR8G8B8A8Unorm;
        ResourceHandle hG0 = b.CreateVirtualResource(gdesc, "GBuffer_Albedo");
        gdesc.format = vk::Format::eR16G16B16A16Sfloat;
        ResourceHandle hG1 = b.CreateVirtualResource(gdesc, "GBuffer_Normal");
        ResourceHandle hG2 = b.CreateVirtualResource(gdesc, "GBuffer_WorldPos");
        ResourceHandle hG3 = b.CreateVirtualResource(gdesc, "GBuffer_Emissive");

        // HDR 中间缓冲：Lighting 写入线性 RGBA16F，Tonemap 采样后写回视口颜色。
        // alpha 通道作为天空/几何元数据：天空=0，几何/透明合成=1（HDR 计划书 §5.3）。
        RenderGraphResourceDesc hdrDesc;
        hdrDesc.extent = extent;
        hdrDesc.samples = vk::SampleCountFlagBits::e1;
        hdrDesc.format = vk::Format::eR16G16B16A16Sfloat;
        ResourceHandle hHDR = b.CreateVirtualResource(hdrDesc, "Scene_HDR");

        // 方向光阴影（CSM C2）：逐级声明 ShadowMap_C0..C{N-1}，每级一张独立深度图
        // （虚拟资源，格式 D32F，尺寸 = GetCascadeShadowSize(c)：级 0 保持现状 4096²、
        // 其余默认 2048²，独立可配）。级数 = cascadeCount（默认 1 = 现状单级）；调大
        // 仅供 RenderDoc 验证多级（Lighting 尚未接入级联采样，主画面不变）。
        // 有方向光实体（castShadow 由 Scene::UpdateLightParams 如实反映）才声明并分配；
        // 无则保持 kInvalidResource（数组值初始化 = 0），Lighting 不追加读、FlushShadow
        // 永不执行，退回无阴影现状。阴影图尺寸与视口无关，池按 (desc, usage) 匹配复用
        // （阴影贴图计划 §5.2）。
        std::array<ResourceHandle, kMaxCascades> hShadow{};
        uint32_t shadowCascadeCount = 0;
        if (Renderer::Get3DRenderer().GetLightParams().castShadow) {
            const uint32_t cascadeCount = std::clamp(
                Renderer::Get3DRenderer().GetLightParams().cascadeCount, 1u, kMaxCascades);
            shadowCascadeCount = cascadeCount;
            for (uint32_t c = 0; c < cascadeCount; ++c) {
                const uint32_t size = Renderer::Get3DRenderer().GetCascadeShadowSize(c);
                RenderGraphResourceDesc shadowDesc;
                shadowDesc.extent = vk::Extent2D{size, size};
                shadowDesc.samples = vk::SampleCountFlagBits::e1;
                shadowDesc.format = vk::Format::eD32Sfloat;
                hShadow[c] = b.CreateVirtualResource(shadowDesc,
                                                     "ShadowMap_C" + std::to_string(c));

                // Pass "ShadowMap_C{c}"：零颜色 + 一深度，插在 GBuffer 之前（m_Meshes
                // 尚未消费，与 GBuffer 共享批次）。只画该级体积内的不透明段（Opaque +
                // Mask）深度。
                RenderPassDesc &shadowPass = b.AddPass("ShadowMap_C" + std::to_string(c));
                shadowPass.renderArea = vk::Rect2D{{0, 0}, shadowDesc.extent};
                AttachmentDesc shadowDepth;
                shadowDepth.resource = hShadow[c];
                shadowDepth.usage = ResourceUsage::DepthStencilAttachment;
                shadowDepth.loadOp = vk::AttachmentLoadOp::eClear;   // 每帧清空重画
                shadowDepth.storeOp = vk::AttachmentStoreOp::eStore; // 保留给 Lighting 采样
                shadowDepth.finalLayout = vk::ImageLayout::eDepthStencilAttachmentOptimal;
                shadowPass.depthAttachment = shadowDepth;
                shadowPass.execute = [c](PassExecuteContext &ctx) {
                    Renderer::Get3DRenderer().FlushShadow(ctx, c);
                };
            }
        }

        // Pass1 "GBuffer"：MRT 四张 + 深度清屏，只录制不透明段（Opaque + Mask）。
        RenderPassDesc &gbufferPass = b.AddPass("GBuffer");
        gbufferPass.renderArea = renderArea;
        AttachmentDesc g0Clear;
        g0Clear.resource = hG0;
        g0Clear.usage = ResourceUsage::ColorAttachment;
        g0Clear.loadOp = vk::AttachmentLoadOp::eClear;
        g0Clear.storeOp = vk::AttachmentStoreOp::eStore;
        g0Clear.clearValue.color = {0.0f, 0.0f, 0.0f, 0.0f};
        gbufferPass.colorAttachments.push_back(g0Clear);
        for (ResourceHandle hG : {hG1, hG2, hG3}) {
            AttachmentDesc gClear;
            gClear.resource = hG;
            gClear.usage = ResourceUsage::ColorAttachment;
            gClear.loadOp = vk::AttachmentLoadOp::eClear;
            gClear.storeOp = vk::AttachmentStoreOp::eStore;
            gClear.clearValue.color = {0.0f, 0.0f, 0.0f, 0.0f};
            gbufferPass.colorAttachments.push_back(gClear);
        }
        AttachmentDesc gbDepthClear;
        gbDepthClear.resource = hDepth;
        gbDepthClear.usage = ResourceUsage::DepthStencilAttachment;
        gbDepthClear.loadOp = vk::AttachmentLoadOp::eClear;
        gbDepthClear.storeOp = vk::AttachmentStoreOp::eStore;
        gbDepthClear.finalLayout = vk::ImageLayout::eDepthStencilAttachmentOptimal;
        gbufferPass.depthAttachment = gbDepthClear;
        gbufferPass.execute = [](PassExecuteContext &ctx) {
            Renderer::Get3DRenderer().FlushGBuffer(ctx);
        };

        // Pass2 "Lighting"：读 G0~G3，写 Scene_HDR（RGBA16F）；天空盒并入此 pass。
        RenderPassDesc &lightingPass = b.AddPass("Lighting");
        lightingPass.renderArea = renderArea;
        lightingPass.readImages.push_back(
            {hG0, ResourceUsage::ShaderRead, vk::ImageLayout::eShaderReadOnlyOptimal});
        lightingPass.readImages.push_back(
            {hG1, ResourceUsage::ShaderRead, vk::ImageLayout::eShaderReadOnlyOptimal});
        lightingPass.readImages.push_back(
            {hG2, ResourceUsage::ShaderRead, vk::ImageLayout::eShaderReadOnlyOptimal});
        lightingPass.readImages.push_back(
            {hG3, ResourceUsage::ShaderRead, vk::ImageLayout::eShaderReadOnlyOptimal});
        // 方向光阴影开启时追加读 hShadow_C0..C{N-1}（readImageViews 第 4..4+N-1 项 →
        // binding 7 暂绑第 0 级单图，C2 未接级联采样、主画面不变；C3 改数组描述符）。
        // 图据此在「ShadowMap 深度写 → Lighting 采样读」之间插屏障、把布局转到
        // ShaderReadOnlyOptimal（§4：深度独享附件 + readImages 的常规推导）。
        for (uint32_t c = 0; c < shadowCascadeCount; ++c) {
            lightingPass.readImages.push_back(
                {hShadow[c], ResourceUsage::ShaderRead, vk::ImageLayout::eShaderReadOnlyOptimal});
        }
        AttachmentDesc lightingColorClear;
        lightingColorClear.resource = hHDR;
        lightingColorClear.usage = ResourceUsage::ColorAttachment;
        lightingColorClear.loadOp = vk::AttachmentLoadOp::eClear;
        lightingColorClear.storeOp = vk::AttachmentStoreOp::eStore;
        lightingColorClear.clearValue.color = {clearColor.r, clearColor.g, clearColor.b, clearColor.a};
        lightingPass.colorAttachments.push_back(lightingColorClear);
        lightingPass.execute = [](PassExecuteContext &ctx) {
            Renderer::Get3DRenderer().FlushLighting(ctx);
        };

        // Pass2b "Transparent"：在 Tonemap 前写回 Scene_HDR，在线性 HDR 空间
        // 合成。颜色 eLoad（Lighting 已清/写 HDR）、深度 eLoad，叠在前向透明
        // 混合管线上；FlushTransparent 以 hdrTransparent=true 配置混合，强制
        // alpha 通道收敛到 1，避免透明覆盖天空后被 Tonemap 误判为天空。
        RenderPassDesc &transparentPass = b.AddPass("Transparent");
        transparentPass.renderArea = renderArea;
        AttachmentDesc transparentColorLoad;
        transparentColorLoad.resource = hHDR;
        transparentColorLoad.usage = ResourceUsage::ColorAttachment;
        transparentColorLoad.loadOp = vk::AttachmentLoadOp::eLoad;
        transparentColorLoad.storeOp = vk::AttachmentStoreOp::eStore;
        transparentPass.colorAttachments.push_back(transparentColorLoad);
        AttachmentDesc transparentDepthLoad;
        transparentDepthLoad.resource = hDepth;
        transparentDepthLoad.usage = ResourceUsage::DepthStencilAttachment;
        transparentDepthLoad.loadOp = vk::AttachmentLoadOp::eLoad;
        transparentDepthLoad.storeOp = vk::AttachmentStoreOp::eStore;
        transparentDepthLoad.finalLayout = vk::ImageLayout::eDepthStencilAttachmentOptimal;
        transparentPass.depthAttachment = transparentDepthLoad;
        transparentPass.execute = [](PassExecuteContext &ctx) {
            Renderer::Get3DRenderer().FlushTransparent(ctx);
        };

        // Pass2c "Tonemap"：采样 Scene_HDR，曝光 + ACES 后写入视口颜色。
        RenderPassDesc &tonemapPass = b.AddPass("Tonemap");
        tonemapPass.renderArea = renderArea;
        tonemapPass.readImages.push_back(
            {hHDR, ResourceUsage::ShaderRead, vk::ImageLayout::eShaderReadOnlyOptimal});
        AttachmentDesc tonemapColorClear;
        tonemapColorClear.resource = hColor;
        tonemapColorClear.usage = ResourceUsage::ColorAttachment;
        tonemapColorClear.loadOp = vk::AttachmentLoadOp::eClear;
        tonemapColorClear.storeOp = vk::AttachmentStoreOp::eStore;
        tonemapColorClear.clearValue.color = {0.0f, 0.0f, 0.0f, 0.0f};
        tonemapPass.colorAttachments.push_back(tonemapColorClear);
        tonemapPass.execute = [](PassExecuteContext &ctx) {
            Renderer::Get3DRenderer().FlushTonemap(ctx);
        };
    } else {
        // Pass0 "Scene3D"：清屏 + 深度 eClear；颜色/深度 eStore（深度须保留给 Scene2D 读）
        RenderPassDesc &scene3D = b.AddPass("Scene3D");
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
        // 深度写后停靠深度布局；finalLayout 默认是颜色态，不显式写回会给深度图
        // 追加一条非法的「深度 → Color」收尾转换（VUID-VkImageMemoryBarrier2-oldLayout-01208）。
        depthClear.finalLayout = vk::ImageLayout::eDepthStencilAttachmentOptimal;
        scene3D.depthAttachment = depthClear;
        scene3D.execute = [](PassExecuteContext &ctx) {
            Renderer::Get3DRenderer().FlushScene(ctx);
        };
    }

    // Scene2D：叠加世界/UI 精灵。颜色 eLoad；深度 eLoad（读前序 3D pass 深度做
    // 遮挡），UI 批 depthTest 关不读写；颜色收尾转 ShaderReadOnlyOptimal 供 ImGui 采样。
    RenderPassDesc &scene2D = b.AddPass("Scene2D");
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
    // 深度附件保持深度布局，同 Scene3D 的 finalLayout（深度写后停靠布局），
    // 避免收尾段给深度图追加非法的 Color 布局转换。
    depthLoad.finalLayout = vk::ImageLayout::eDepthStencilAttachmentOptimal;
    scene2D.depthAttachment = depthLoad;
    scene2D.execute = [](PassExecuteContext &ctx) {
        Renderer::Get2DRenderer().FlushScene(ctx);
    };
}

void SceneLayer::OnUpdate(Timestep &ts) {
    // Play 态第一人称相机：锁定鼠标（否则无法持续转向）
    UpdateMouseCapture();

    uint32_t vpWidth = 0, vpHeight = 0;
    if (!EnsureViewport(ts, vpWidth, vpHeight)) {
        return;
    }

    const float aspect = static_cast<float>(vpWidth) / static_cast<float>(vpHeight);

    // 同步场景视口尺寸
    m_Context->Scene->OnViewportResize(vpWidth, vpHeight);

    Camera &activeCam = GetRenderingViewCamera(aspect);
    const glm::mat4 view = activeCam.GetView();
    const glm::mat4 projection = activeCam.GetProj();
    const glm::vec3 cameraPos = activeCam.GetPosition();
    const glm::vec4 clearColor{0.1f, 0.1f, 0.15f, 1.0f};

    // 场景曝光接线：把本帧视口相机（编辑器相机 / 游戏主相机）的曝光系数
    // 传给 Renderer3D，Deferred 模式的 Tonemap pass 经 TonemapUBO.exposure.x 使用。
    Renderer::Get3DRenderer().SetExposure(activeCam.GetExposure());

    // Scene 只做仿真 + 采集（3D/2D 批次经 EndScene 延迟快照，不录制命令）
    m_Context->Scene->OnUpdate3D(ts, view, projection, cameraPos, clearColor);

    RecordScenePasses(*m_Viewport->GetRenderTarget(), clearColor);
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

            // 调试线框叠加（包围盒/碰撞体/第一人称视点）已拆到 DebugDrawLayer
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
