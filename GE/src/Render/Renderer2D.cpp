/**
 * @file Renderer2D.cpp
 * @brief 2D 精灵批处理渲染器实现。
 */

#include "Render/Renderer2D.h"

#include "Core/Log.h"
#include "Render/Renderer.h"
#include "Render/TextureManager.h"
#include "Render/VulkanBase/VulkanCommandBuffer.h"
#include "Render/VulkanBase/VulkanPipelineLayout.h"
#include "Render/VulkanBase/VulkanRenderContext.h"
#include "Render/VulkanBase/VulkanRenderFrame.h"
#include "Render/VulkanBase/VulkanRenderingInfo.h"
#include "Render/VulkanBase/VulkanResourceCache.h"
#include "Render/VulkanBase/VulkanShaderModule.h"

#include "Debug/Profiler.h"

#include <glm/gtc/matrix_transform.hpp>

namespace GE {

// ============================================================================
// 构造 / 析构
// ============================================================================

Renderer2D::Renderer2D() {
    GE_PROFILE_SCOPE("Renderer2D::Init");

    auto &device = Renderer::GetVulkanContext().GetDevice();
    auto &cache = device.GetResourceCache();

    // ── 1. 通过全局资源缓存请求精灵着色器 ──────────────────────────────
    m_VertShader = &cache.RequestShaderModule(
        vk::ShaderStageFlagBits::eVertex,
        ShaderSource("assets/shaders/glsl/sprite.vert.spv"),
        "main", ShaderVariant{});

    m_FragShader = &cache.RequestShaderModule(
        vk::ShaderStageFlagBits::eFragment,
        ShaderSource("assets/shaders/glsl/sprite.frag.spv"),
        "main", ShaderVariant{});

    // ── 2. 请求 PipelineLayout ─────────────────────────────────────────
    m_PipelineLayout = &cache.RequestPipelineLayout(
        {m_VertShader, m_FragShader});
    m_PipelineLayout->SetDebugName("Sprite2D_PipelineLayout");

    // ── 3. 默认 1x1 白色纹理（无纹理时的 fallback），从全局纹理管理器获取 ──
    //    纹理由 TextureManager 去重缓存并持有，这里仅保存非拥有指针。
    m_DefaultWhiteTexture =
        Renderer::GetTextureManager().GetSolidColor(
            glm::vec4(1.0f), vk::Format::eR8G8B8A8Unorm,
            vk::Filter::eLinear, vk::Filter::eLinear);
    if (!m_DefaultWhiteTexture) {
        GE_CORE_ERROR("Renderer2D: 获取默认白色纹理失败！");
    }

    GE_CORE_INFO("Renderer2D initialized");
}

Renderer2D::~Renderer2D() {
    GE_CORE_INFO("Renderer2D Shutdown");

    // 着色器和 pipeline layout 由全局资源缓存管理，不需要手动释放
    m_VertShader = nullptr;
    m_FragShader = nullptr;
    m_PipelineLayout = nullptr;
}

// ============================================================================
// 场景接口
// ============================================================================

void Renderer2D::BeginScene(const glm::mat4 &view,
                            const glm::mat4 &projection,
                            bool useDepth,
                            const glm::vec4 &clearColor) {
    GE_CORE_ASSERT(!m_InScene, "Renderer2D::BeginScene called without EndScene!");
    m_InScene = true;
    m_View = view;
    m_Projection = projection;
    m_UseDepth = useDepth;
    m_ClearColor = clearColor;

    // 清空上一帧的批处理数据
    for (auto &[tex, verts] : m_Batches) {
        verts.clear();
    }
}

void Renderer2D::DrawSprite(const glm::vec2 &position,
                            const glm::vec2 &size,
                            float rotation,
                            Texture *texture,
                            const glm::vec4 &color) {
    DrawSprite(glm::vec3(position, 0.0f), size, rotation, texture, color);
}

void Renderer2D::DrawSprite(const glm::vec3 &position,
                            const glm::vec2 &size,
                            float rotation,
                            Texture *texture,
                            const glm::vec4 &color) {
    GE_CORE_ASSERT(m_InScene, "DrawSprite called outside BeginScene/EndScene!");

    // 构建模型变换矩阵：先缩放，再旋转，再平移
    glm::mat4 transform = glm::translate(glm::mat4(1.0f), position)
                          * glm::rotate(glm::mat4(1.0f), rotation, glm::vec3(0.0f, 0.0f, 1.0f))
                          * glm::scale(glm::mat4(1.0f), glm::vec3(size * 0.5f, 1.0f));

    AppendQuad(texture, transform, color);
}

void Renderer2D::DrawSprite(const glm::mat4 &transform,
                            Texture *texture,
                            const glm::vec4 &color) {
    GE_CORE_ASSERT(m_InScene, "DrawSprite called outside BeginScene/EndScene!");
    AppendQuad(texture, transform, color);
}

void Renderer2D::EndScene() {
    GE_PROFILE_SCOPE("Renderer2D::EndScene");

    GE_CORE_ASSERT(m_InScene, "EndScene called without BeginScene!");
    m_InScene = false;

    // 没有精灵需要绘制，直接返回
    if (m_Batches.empty()) {
        return;
    }

    // ── 统计总顶点数 ──────────────────────────────────────────────────
    size_t totalVertices = 0;
    for (const auto &[tex, verts] : m_Batches) {
        totalVertices += verts.size();
    }
    if (totalVertices == 0) {
        return;
    }

    auto &cmd = Renderer::GetFrameCmd();
    auto vkCmd = cmd.GetHandle();
    auto &frame = Renderer::GetRenderContext().GetActiveFrame();

    // 有效渲染目标：优先使用外部指定的目标（离屏），否则使用当前帧的 swapchain 目标
    auto &target = m_RenderTargetOverride ? *m_RenderTargetOverride
                                          : frame.GetRenderTarget();
    auto extent = target.GetExtent();

    // ── 1. 从帧资源池分配顶点 buffer ──────────────────────────────────
    BufferAllocation vertexAlloc = frame.AllocateBuffer(
        vk::BufferUsageFlagBits::eVertexBuffer,
        totalVertices * sizeof(SpriteVertex));

    // ── 2. 将所有批次的顶点数据收集到临时 buffer，一次性上传 ───────────
    // 记录每个纹理批次在顶点 buffer 中的起始偏移和顶点数
    struct BatchInfo {
        Texture *texture;
        uint32_t vertexOffset; // 以顶点数为单位的偏移
        uint32_t vertexCount;
    };
    std::vector<BatchInfo> batchInfos;
    batchInfos.reserve(m_Batches.size());

    std::vector<SpriteVertex> allVertices;
    allVertices.reserve(totalVertices);
    uint32_t vertexOffset = 0;

    for (auto &[tex, verts] : m_Batches) {
        if (verts.empty())
            continue;

        allVertices.insert(allVertices.end(), verts.begin(), verts.end());
        batchInfos.push_back({tex, vertexOffset, static_cast<uint32_t>(verts.size())});
        vertexOffset += static_cast<uint32_t>(verts.size());
    }

    // 一次性上传所有顶点数据
    vertexAlloc.update(allVertices);

    // ── 3. 分配 UBO（UniformBlock） ───────────────────────────────────
    // 模型变换已在 CPU 端烘焙到顶点位置，UBO 只存 view + projection + color
    UniformBlock ubo{};
    ubo.view = m_View;
    ubo.projection = m_Projection;
    ubo.color = glm::vec4(1.0f);

    BufferAllocation uboAlloc = frame.AllocateBuffer(
        vk::BufferUsageFlagBits::eUniformBuffer, sizeof(UniformBlock));
    uboAlloc.update(ubo);

    // ── 4. 开始动态渲染 ───────────────────────────────────────────────
    // 根据 m_ClearColor 决定是否清屏：r < 0 表示不清屏（eLoad），否则清屏（eClear）
    bool shouldClear = m_ClearColor.r >= 0.0f;
    vk::ClearValue clearValue{};
    clearValue.color = std::array<float, 4>{
        m_ClearColor.r, m_ClearColor.g, m_ClearColor.b, m_ClearColor.a};

    // 附件布局与图像实际布局一致：离屏颜色图固定 GENERAL，否则验证层报
    // VUID-vkCmdBeginRendering-pRenderingInfo-09592。正常 swapchain 用默认。
    vk::ImageLayout colorLayout = target.HasOffscreenColor()
                                      ? vk::ImageLayout::eGeneral
                                      : vk::ImageLayout::eColorAttachmentOptimal;

    VulkanRenderingInfo renderInfo;
    renderInfo.SetRenderArea(0, 0, extent.width, extent.height);
    renderInfo.AddColorAttachment(target.GetColorView().GetHandle(),
                                  shouldClear
                                      ? vk::AttachmentLoadOp::eClear
                                      : vk::AttachmentLoadOp::eLoad,
                                  vk::AttachmentStoreOp::eStore,
                                  clearValue,
                                  colorLayout);

    // 3D 模式下附加深度缓冲（load = 加载已有深度，保证 3D 场景里的遮挡正确）
    if (m_UseDepth && target.HasDepth()) {
        vk::ClearDepthStencilValue clearDS{1.0f, 0};
        renderInfo.SetDepthAttachment(
            target.GetDepthView().GetHandle(),
            vk::AttachmentLoadOp::eLoad,
            vk::AttachmentStoreOp::eStore,
            clearDS);
    }

    renderInfo.Begin(vkCmd);

    // ── 5. 绑定 pipeline layout ───────────────────────────────────────
    cmd.BindPipelineLayout(*m_PipelineLayout);

    auto &ps = cmd.GetPipelineState();
    auto colorFmt = target.GetColorFormat();

    // 混合附件：启用 alpha 混合（预乘 alpha 模式）
    // 着色器输出已预乘 alpha（rgb *= alpha），所以源因子用 eOne
    vk::PipelineColorBlendAttachmentState blendState{};
    blendState.colorWriteMask = vk::ColorComponentFlagBits::eR
                                | vk::ColorComponentFlagBits::eG
                                | vk::ColorComponentFlagBits::eB
                                | vk::ColorComponentFlagBits::eA;
    blendState.blendEnable = VK_TRUE;
    blendState.srcColorBlendFactor = vk::BlendFactor::eOne;
    blendState.dstColorBlendFactor = vk::BlendFactor::eOneMinusSrcAlpha;
    blendState.colorBlendOp = vk::BlendOp::eAdd;
    blendState.srcAlphaBlendFactor = vk::BlendFactor::eOne;
    blendState.dstAlphaBlendFactor = vk::BlendFactor::eOneMinusSrcAlpha;
    blendState.alphaBlendOp = vk::BlendOp::eAdd;

    // 配置管线状态（链式 API）
    vk::Format depthFmt = vk::Format::eUndefined;
    if (m_UseDepth && target.HasDepth()) {
        depthFmt = target.GetDepthFormat();
    }

    ps.setRenderingFormats({colorFmt}, depthFmt)
        .setInputAssembly(vk::PrimitiveTopology::eTriangleList)
        .setColorBlendAttachments({blendState})
        .setCullMode(vk::CullModeFlagBits::eNone)
        .setFrontFace(vk::FrontFace::eCounterClockwise)
        .setDepthTestEnable(m_UseDepth ? VK_TRUE : VK_FALSE)
        .setDepthWriteEnable(m_UseDepth ? VK_TRUE : VK_FALSE)
        .enableDynamicState(vk::DynamicState::eCullMode)
        .enableDynamicState(vk::DynamicState::eFrontFace)
        .enableDynamicState(vk::DynamicState::ePrimitiveTopology);

    // 顶点输入：从顶点着色器反射自动生成
    ps.setVertexInputFromShader(*m_VertShader);

    // 动态状态
    vk::Viewport vp;
    vp.width = static_cast<float>(extent.width);
    vp.height = static_cast<float>(extent.height);
    vp.minDepth = 0.0f;
    vp.maxDepth = 1.0f;
    cmd.SetViewport(0, {vp});

    vk::Rect2D scissor;
    scissor.extent.width = extent.width;
    scissor.extent.height = extent.height;
    cmd.SetScissor(0, {scissor});

    // 写入动态管线状态
    ps.setCullMode(vk::CullModeFlagBits::eNone);
    ps.setFrontFace(vk::FrontFace::eCounterClockwise);
    ps.setInputAssembly(vk::PrimitiveTopology::eTriangleList);

    // ── 6. 绑定顶点 buffer ────────────────────────────────────────────
    cmd.BindVertexBuffers(0,
                          {std::ref(vertexAlloc.get_buffer())},
                          {vertexAlloc.get_offset()});

    // ── 7. 绑定 UBO（所有批次共享同一个） ─────────────────────────────
    cmd.BindBuffer(uboAlloc.get_buffer(), uboAlloc.get_offset(),
                   uboAlloc.get_size(), 0, 0);

    // ── 8. 按纹理批次绘制 ─────────────────────────────────────────────
    // 每批次一个 draw call，同纹理的所有精灵合并绘制
    // 无纹理时使用默认 1x1 白色纹理，避免未定义采样行为
    Texture *fallback = m_DefaultWhiteTexture;
    for (const auto &batch : batchInfos) {
        Texture *tex = batch.texture ? batch.texture : fallback;
        if (tex) {
            cmd.BindImage(tex->GetImageView(),
                          tex->GetSampler(),
                          0, 1);
        }
        cmd.Draw(batch.vertexCount, 1, batch.vertexOffset, 0);
    }

    // ── 8b. 统计 draw call 与三角形数量（每批次一个 draw call） ─────────
    uint32_t triangles = 0;
    for (const auto &batch : batchInfos) {
        triangles += batch.vertexCount / 3;
    }
    Renderer::Get().AddStats2D(static_cast<uint32_t>(batchInfos.size()), triangles);

    // ── 9. 结束渲染 ───────────────────────────────────────────────────
    VulkanRenderingInfo::End(vkCmd);
}

// ============================================================================
// 内部辅助
// ============================================================================

void Renderer2D::AppendQuad(Texture *texture,
                            const glm::mat4 &transform,
                            const glm::vec4 &color) {
    // 四边形的 4 个角（中心在原点，宽高为 2，位于 xy 平面，对应 [-1, 1]）
    // 经过 transform 后变成实际大小、位置、方向
    struct Corner {
        glm::vec3 pos;
        glm::vec2 uv;
    };

    static constexpr Corner corners[4] = {
        {{-1.0f, -1.0f, 0.0f}, {0.0f, 0.0f}}, // 0: 左下
        {{1.0f, -1.0f, 0.0f}, {1.0f, 0.0f}}, // 1: 右下
        {{1.0f, 1.0f, 0.0f}, {1.0f, 1.0f}}, // 2: 右上
        {{-1.0f, 1.0f, 0.0f}, {0.0f, 1.0f}}, // 3: 左上
    };

    // 先把 4 个角都算出世界坐标（含 z）
    glm::vec3 positions[4];
    for (int i = 0; i < 4; ++i) {
        glm::vec4 p = transform * glm::vec4(corners[i].pos, 1.0f);
        positions[i] = glm::vec3(p.x, p.y, p.z);
    }

    // 6 个顶点（2 个三角形，CCW）：0-1-2, 0-2-3
    auto &verts = m_Batches[texture];
    verts.reserve(verts.size() + 6);

    // 三角形 1：左下 → 右下 → 右上
    verts.push_back({positions[0], corners[0].uv, color});
    verts.push_back({positions[1], corners[1].uv, color});
    verts.push_back({positions[2], corners[2].uv, color});

    // 三角形 2：左下 → 右上 → 左上
    verts.push_back({positions[0], corners[0].uv, color});
    verts.push_back({positions[2], corners[2].uv, color});
    verts.push_back({positions[3], corners[3].uv, color});
}

} // namespace GE
