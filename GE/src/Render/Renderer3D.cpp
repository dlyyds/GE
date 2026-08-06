/**
 * @file Renderer3D.cpp
 * @brief 3D 网格渲染器实现。
 */

#include "Render/Renderer3D.h"

#include <algorithm>

#include "Core/Log.h"
#include "Render/Texture.h"
#include "Render/Renderer.h"
#include "Render/VulkanBase/VulkanCommandBuffer.h"
#include "Render/VulkanBase/VulkanPipelineLayout.h"
#include "Render/VulkanBase/VulkanRenderContext.h"
#include "Render/VulkanBase/VulkanRenderFrame.h"
#include "Render/VulkanBase/VulkanRenderingInfo.h"
#include "Render/VulkanBase/VulkanResourceCache.h"
#include "Render/ShaderModule.h"

#include "tracy/Tracy.hpp"

namespace GE {

// ============================================================================
// 构造 / 析构
// ============================================================================

Renderer3D::Renderer3D() {
    ZoneScopedN("Renderer3D::Init");

    auto &device = Renderer::GetVulkanContext().GetDevice();
    auto &cache = device.GetResourceCache();

    // ── 1. 通过全局资源缓存请求网格着色器 ──────────────────────────────
    m_VertShader = &cache.RequestShaderModule(
        vk::ShaderStageFlagBits::eVertex,
        ShaderSource("assets/shaders/glsl/mesh.vert.spv"),
        "main", ShaderVariant{});

    m_FragShader = &cache.RequestShaderModule(
        vk::ShaderStageFlagBits::eFragment,
        ShaderSource("assets/shaders/glsl/mesh.frag.spv"),
        "main", ShaderVariant{});

    // ── 2. 请求 PipelineLayout（通过反射自动构建） ─────────────────────
    m_PipelineLayout = &cache.RequestPipelineLayout(
        {m_VertShader, m_FragShader});
    m_PipelineLayout->SetDebugName("Mesh3D_PipelineLayout");

    // ── 3. 创建默认 1x1 白色纹理（无纹理时的 fallback） ────────────────
    uint32_t whitePixel = 0xFFFFFFFF; // RGBA8: (255, 255, 255, 255)
    m_DefaultWhiteTexture = Texture::LoadFromMemory(
        device, cache, &whitePixel, 1, 1,
        vk::Format::eR8G8B8A8Unorm,
        vk::Filter::eLinear, vk::Filter::eLinear);
    if (m_DefaultWhiteTexture) {
        m_DefaultWhiteTexture->SetDebugName("DefaultWhiteTexture");
    } else {
        GE_CORE_ERROR("Renderer3D: 创建默认白色纹理失败！");
    }

    GE_CORE_INFO("Renderer3D initialized");
}

Renderer3D::~Renderer3D() {
    GE_CORE_INFO("Renderer3D Shutdown");

    // 释放默认白色纹理
    m_DefaultWhiteTexture.reset();

    // 着色器和 pipeline layout 由全局资源缓存管理，不需要手动释放
    m_VertShader = nullptr;
    m_FragShader = nullptr;
    m_PipelineLayout = nullptr;
}

// ============================================================================
// 场景接口
// ============================================================================

void Renderer3D::BeginScene(const glm::mat4 &view,
                            const glm::mat4 &projection,
                            const glm::vec3 &viewPos,
                            const glm::vec4 &clearColor) {
    GE_CORE_ASSERT(!m_InScene, "Renderer3D::BeginScene called without EndScene!");
    m_InScene = true;

    m_View = view;
    m_Projection = projection;
    m_ViewPos = viewPos;
    m_ClearColor = clearColor;

    // 清空上一帧的网格列表
    m_Meshes.clear();
}

void Renderer3D::DrawMesh(const glm::mat4 &transform,
                          Mesh *mesh,
                          Texture *texture,
                          const glm::vec4 &color) {
    GE_CORE_ASSERT(m_InScene, "DrawMesh called outside BeginScene/EndScene!");

    if (!mesh || mesh->GetIndexCount() == 0) {
        return;
    }

    // 旧接口：material 为 nullptr，texture 字段直接保存
    m_Meshes.push_back({transform, mesh, nullptr, texture, color});
}

void Renderer3D::DrawMesh(const glm::mat4 &transform,
                          Mesh *mesh,
                          Material *material,
                          const glm::vec4 &color) {
    GE_CORE_ASSERT(m_InScene, "DrawMesh called outside BeginScene/EndScene!");

    if (!mesh || mesh->GetIndexCount() == 0) {
        return;
    }

    m_Meshes.push_back({transform, mesh, material, nullptr, color});
}

void Renderer3D::EndScene() {
    ZoneScopedN("Renderer3D::EndScene");

    GE_CORE_ASSERT(m_InScene, "EndScene called without BeginScene!");
    m_InScene = false;

    // 没有网格需要绘制，直接返回
    if (m_Meshes.empty()) {
        return;
    }

    auto &cmd = Renderer::GetFrameCmd();
    auto vkCmd = cmd.GetHandle();
    auto &swapchain = Renderer::GetSwapchain();
    auto extent = swapchain.GetExtent();
    auto &frame = Renderer::GetRenderContext().GetActiveFrame();
    auto &renderTarget = frame.GetRenderTarget();

    // ── 1. 分配 Frame UBO（所有网格共享） ─────────────────────────────
    FrameUBO frameUBO{};
    frameUBO.projection = m_Projection;
    frameUBO.view = m_View;
    frameUBO.viewPos = glm::vec4(m_ViewPos, 0.0f);
    frameUBO.dirLightDirection = glm::vec4(m_LightParams.dirLightDirection, 0.0f);
    frameUBO.dirLightColor = m_LightParams.dirLightColor;

    // 填充点光源数组（不超过上限）
    size_t lightCount = std::min(m_LightParams.pointLightCount, MAX_POINT_LIGHTS);
    for (size_t i = 0; i < lightCount; i++) {
        const auto &pl = m_LightParams.pointLights[i];
        frameUBO.pointLightPositions[i] = glm::vec4(pl.position, pl.radiusInv);
        frameUBO.pointLightColors[i]    = pl.color;
    }
    frameUBO.pointLightCount = glm::vec4(static_cast<float>(lightCount), 0.0f, 0.0f, 0.0f);

    frameUBO.ambient = m_LightParams.ambient;

    BufferAllocation frameUboAlloc = frame.AllocateBuffer(
        vk::BufferUsageFlagBits::eUniformBuffer, sizeof(FrameUBO));
    frameUboAlloc.update(frameUBO);

    // ── 2. 为每个网格分配 Object UBO ──────────────────────────────────
    //    每个 UBO 单独从 BufferPool 分配，确保偏移满足
    //    minUniformBufferOffsetAlignment 对齐要求
    std::vector<BufferAllocation> objectUboAllocs;
    objectUboAllocs.reserve(m_Meshes.size());
    for (const auto &instance : m_Meshes) {
        ObjectUBO ubo{};
        ubo.model = instance.transform;
        ubo.lodBias = 0.0f;
        ubo._pad = glm::vec3(0.0f);
        ubo.color = instance.color;

        BufferAllocation alloc = frame.AllocateBuffer(
            vk::BufferUsageFlagBits::eUniformBuffer, sizeof(ObjectUBO));
        alloc.update(ubo);
        objectUboAllocs.push_back(std::move(alloc));
    }

    // ── 3. 开始动态渲染 ───────────────────────────────────────────────
    //    使用 FromRenderTarget 自动构建颜色 + 深度附件
    VulkanRenderingInfo renderInfo = VulkanRenderingInfo::FromRenderTarget(renderTarget);

    // 根据 m_ClearColor 决定颜色附件的 loadOp
    bool shouldClear = m_ClearColor.r >= 0.0f;
    if (shouldClear) {
        // 重新设置颜色附件的 clear 值和 loadOp
        vk::ClearValue clearValue{};
        clearValue.color = std::array<float, 4>{
            m_ClearColor.r, m_ClearColor.g, m_ClearColor.b, m_ClearColor.a};

        // 重置 renderInfo 并重新配置
        renderInfo.Reset();
        renderInfo.SetRenderArea(0, 0, extent.width, extent.height);

        const auto &desc = renderTarget.GetDesc();
        // 颜色附件：清除
        vk::ImageView colorView = renderTarget.GetSwapchainView().GetHandle();
        renderInfo.AddColorAttachment(colorView,
                                      vk::AttachmentLoadOp::eClear,
                                      vk::AttachmentStoreOp::eStore,
                                      clearValue);

        // 深度附件（如果有）：清除
        if (desc.enableDepth) {
            vk::ClearDepthStencilValue clearDS{1.0f, 0};
            if (desc.enableStencil) {
                renderInfo.SetDepthStencilAttachment(
                    renderTarget.GetDepthView().GetHandle(),
                    desc.depthLoadOp, desc.depthStoreOp,
                    desc.stencilLoadOp, desc.stencilStoreOp,
                    clearDS);
            } else {
                renderInfo.SetDepthAttachment(
                    renderTarget.GetDepthView().GetHandle(),
                    desc.depthLoadOp, desc.depthStoreOp,
                    clearDS);
            }
        }
    }

    renderInfo.Begin(vkCmd);

    // ── 4. 绑定 pipeline layout + 设置管线状态 ────────────────────────
    cmd.BindPipelineLayout(*m_PipelineLayout);

    auto &ps = cmd.GetPipelineState();
    auto colorFmt = swapchain.GetFormat();

    // 附件格式
    ps.colorAttachmentFormats = {colorFmt};
    if (renderTarget.HasDepth()) {
        ps.depthFormat = renderTarget.GetDepthFormat();
    } else {
        ps.depthFormat = {};
    }
    ps.stencilFormat = {};

    // 顶点输入：从顶点着色器反射自动生成
    ps.SetVertexInputFromShader(*m_VertShader);

    // 混合附件：无 alpha 混合（3D 不透明物体）
    vk::PipelineColorBlendAttachmentState blendState{};
    blendState.colorWriteMask = vk::ColorComponentFlagBits::eR
                                | vk::ColorComponentFlagBits::eG
                                | vk::ColorComponentFlagBits::eB
                                | vk::ColorComponentFlagBits::eA;
    ps.SetBlendAttachments({blendState});

    // 3D 渲染：背面剔除、深度测试、三角形列表
    ps.cullMode.SetDynamic(true);
    ps.frontFace.SetDynamic(true);
    ps.topology.SetDynamic(true);
    ps.depthTestEnable.SetDynamic(true);
    ps.depthWriteEnable.SetDynamic(true);
    ps.depthCompareOp.SetDynamic(true);

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
    ps.cullMode = vk::CullModeFlagBits::eBack;
    ps.frontFace = vk::FrontFace::eCounterClockwise;
    ps.topology = vk::PrimitiveTopology::eTriangleList;
    ps.depthTestEnable = VK_TRUE;
    ps.depthWriteEnable = VK_TRUE;
    ps.depthCompareOp = vk::CompareOp::eLess;

    // ── 5. 绑定 Frame UBO（set 0, binding 0，所有网格共享） ───────────
    cmd.BindBuffer(frameUboAlloc.get_buffer(), frameUboAlloc.get_offset(),
                   frameUboAlloc.get_size(), 0, 0);

    // ── 6. 逐个绘制网格 ───────────────────────────────────────────────
    vk::DeviceSize vertexOffset = 0;
    for (size_t i = 0; i < m_Meshes.size(); ++i) {
        const auto &instance = m_Meshes[i];
        auto &uboAlloc = objectUboAllocs[i];

        // 绑定 Object UBO（set 2, binding 0）
        // 每个 UBO 单独分配，offset 天然满足 minUniformBufferOffsetAlignment
        cmd.BindBuffer(uboAlloc.get_buffer(), uboAlloc.get_offset(),
                       uboAlloc.get_size(), 2, 0);

        // 绑定纹理（set 1, binding 0）
        // 优先使用 material 的 Albedo 槽位，其次使用旧接口的 texture，
        // 都没有则使用默认 1x1 白色纹理，避免未定义行为
        Texture *tex = nullptr;
        if (instance.material) {
            tex = instance.material->GetTexture(Material::Albedo);
        }
        if (!tex) {
            tex = instance.texture;
        }
        if (!tex) {
            tex = m_DefaultWhiteTexture.get();
        }
        if (tex) {
            cmd.BindImage(tex->GetImageView(),
                          tex->GetSampler(),
                          1, 0);
        }

        // 绑定顶点缓冲 + 索引缓冲
        cmd.BindVertexBuffers(0,
                              {std::ref(instance.mesh->GetVertexBuffer())},
                              {vertexOffset});
        cmd.BindIndexBuffer(instance.mesh->GetIndexBuffer(), 0, vk::IndexType::eUint32);

        // 绘制
        cmd.DrawIndexed(instance.mesh->GetIndexCount(), 1, 0, 0, 0);
    }

    // ── 7. 结束渲染 ───────────────────────────────────────────────────
    VulkanRenderingInfo::End(vkCmd);
}

} // namespace GE
