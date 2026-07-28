/**
 * @file Renderer2D.cpp
 * @brief 2D 精灵批处理渲染器实现。
 */

#include "Render/Renderer2D.h"

#include "Core/Log.h"
#include "Render/Renderer.h"
#include "Render/VulkanBase/VulkanCommandBuffer.h"
#include "Render/VulkanBase/VulkanPipelineLayout.h"
#include "Render/VulkanBase/VulkanRenderContext.h"
#include "Render/VulkanBase/VulkanRenderFrame.h"
#include "Render/VulkanBase/VulkanRenderingInfo.h"
#include "Render/VulkanBase/VulkanResourceCache.h"
#include "Render/VulkanBase/ShaderModule.h"

#include "tracy/Tracy.hpp"

#include <glm/gtc/matrix_transform.hpp>

namespace GE {

// ============================================================================
// 构造 / 析构
// ============================================================================

Renderer2D::Renderer2D() {
    ZoneScopedN("Renderer2D::Init");

    auto &device = Renderer::GetVulkanContext().GetDevice();
    auto &cache = device.GetResourceCache();

    // ── 1. 通过全局资源缓存请求精灵着色器 ──────────────────────────────
    // 引擎内置 sprite 着色器，编译产物在 GE/assets/shaders/glsl/ 下
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

void Renderer2D::BeginScene(const glm::mat4 &viewProjection,
                            const glm::vec4 &clearColor) {
    GE_CORE_ASSERT(!m_InScene, "Renderer2D::BeginScene called without EndScene!");
    m_InScene        = true;
    m_ViewProjection = viewProjection;
    m_ClearColor     = clearColor;

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
    GE_CORE_ASSERT(m_InScene, "DrawSprite called outside BeginScene/EndScene!");

    // 构建模型变换矩阵：先缩放，再旋转，再平移
    glm::mat4 transform = glm::translate(glm::mat4(1.0f), glm::vec3(position, 0.0f))
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
    ZoneScopedN("Renderer2D::EndScene");

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
    auto &swapchain = Renderer::GetSwapchain();
    auto extent = swapchain.GetExtent();
    auto &frame = Renderer::GetRenderContext().GetActiveFrame();

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
    // 因为我们在 CPU 端把 model 变换应用到了顶点，所以 UBO 中：
    //   model = 单位矩阵
    //   view  = 单位矩阵
    //   projection = viewProjection（作为投影矩阵传入）
    //   color = 白色
    UniformBlock ubo{};
    ubo.model = glm::mat4(1.0f);
    ubo.view = glm::mat4(1.0f);
    ubo.projection = m_ViewProjection;
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

    VulkanRenderingInfo renderInfo;
    renderInfo.SetRenderArea(0, 0, extent.width, extent.height);
    renderInfo.AddColorAttachment(Renderer::GetFrameImageView().GetHandle(),
                                  shouldClear ? vk::AttachmentLoadOp::eClear
                                              : vk::AttachmentLoadOp::eLoad,
                                  vk::AttachmentStoreOp::eStore,
                                  clearValue);
    renderInfo.Begin(vkCmd);

    // ── 5. 绑定 pipeline layout ───────────────────────────────────────
    cmd.BindPipelineLayout(*m_PipelineLayout);

    auto &ps = cmd.GetPipelineState();
    auto colorFmt = swapchain.GetFormat();

    // 附件格式
    ps.colorAttachmentFormats = {colorFmt};
    ps.depthFormat = {};
    ps.stencilFormat = {};

    // 顶点输入：从顶点着色器反射自动生成
    ps.SetVertexInputFromShader(*m_VertShader);

    // 混合附件：启用 alpha 混合（预乘 alpha 模式）
    vk::PipelineColorBlendAttachmentState blendState{};
    blendState.colorWriteMask = vk::ColorComponentFlagBits::eR
                                | vk::ColorComponentFlagBits::eG
                                | vk::ColorComponentFlagBits::eB
                                | vk::ColorComponentFlagBits::eA;
    blendState.blendEnable = VK_TRUE;
    blendState.srcColorBlendFactor = vk::BlendFactor::eOne; // 预乘 alpha: src = 1
    blendState.dstColorBlendFactor = vk::BlendFactor::eOneMinusSrcAlpha;
    blendState.colorBlendOp = vk::BlendOp::eAdd;
    blendState.srcAlphaBlendFactor = vk::BlendFactor::eOne;
    blendState.dstAlphaBlendFactor = vk::BlendFactor::eOneMinusSrcAlpha;
    blendState.alphaBlendOp = vk::BlendOp::eAdd;
    ps.SetBlendAttachments({blendState});

    // 2D 渲染：无背面剔除、无深度测试、三角形列表
    ps.cullMode.SetDynamic(true);
    ps.frontFace.SetDynamic(true);
    ps.topology.SetDynamic(true);
    ps.depthTestEnable = VK_FALSE;
    ps.depthWriteEnable = VK_FALSE;

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
    cmd.GetPipelineState().cullMode = vk::CullModeFlagBits::eNone;
    cmd.GetPipelineState().frontFace = vk::FrontFace::eCounterClockwise;
    cmd.GetPipelineState().topology = vk::PrimitiveTopology::eTriangleList;

    // ── 6. 绑定顶点 buffer ────────────────────────────────────────────
    cmd.BindVertexBuffers(0,
                          {std::ref(vertexAlloc.get_buffer())},
                          {vertexAlloc.get_offset()});

    // ── 7. 绑定 UBO（所有批次共享同一个） ─────────────────────────────
    cmd.BindBuffer(uboAlloc.get_buffer(), uboAlloc.get_offset(),
                   uboAlloc.get_size(), 0, 0);

    // ── 8. 按纹理批次绘制 ─────────────────────────────────────────────
    // 每批次一个 draw call，同纹理的所有精灵合并绘制
    for (const auto &batch : batchInfos) {
        if (batch.texture) {
            // 绑定纹理到 set 0, binding 1
            cmd.BindImage(batch.texture->GetImageView(),
                          batch.texture->GetSampler(),
                          0, 1);
        }
        // 无纹理时使用片元着色器默认采样（白色）

        cmd.Draw(batch.vertexCount, 1, batch.vertexOffset, 0);
    }

    // ── 9. 结束渲染 ───────────────────────────────────────────────────
    VulkanRenderingInfo::End(vkCmd);
}

// ============================================================================
// 内部辅助
// ============================================================================

void Renderer2D::AppendQuad(Texture *texture,
                            const glm::mat4 &transform,
                            const glm::vec4 &color) {
    // 单位四边形的 4 个顶点（中心在原点，宽高为 2，对应 [-1, 1]）
    // 经过 transform 后变成实际大小和位置
    //
    // 顶点顺序：左下 → 右下 → 右上 → 左上（CCW）
    // UV 顺序：  (0,0) → (1,0) → (1,1) → (0,1)
    struct BaseVertex {
        glm::vec2 pos;
        glm::vec2 uv;
    };

    static constexpr BaseVertex kQuadVerts[4] = {
        {{-1.0f, -1.0f}, {0.0f, 0.0f}}, // 左下
        {{1.0f, -1.0f}, {1.0f, 0.0f}}, // 右下
        {{1.0f, 1.0f}, {1.0f, 1.0f}}, // 右上
        {{-1.0f, 1.0f}, {0.0f, 1.0f}}, // 左上
    };

    auto &verts = m_Batches[texture];
    verts.reserve(verts.size() + 4);

    for (const auto &v : kQuadVerts) {
        glm::vec4 worldPos = transform * glm::vec4(v.pos, 0.0f, 1.0f);
        verts.push_back({
            .position = glm::vec2(worldPos.x, worldPos.y),
            .uv = v.uv,
            .color = color,
        });
    }
}

} // namespace GE
