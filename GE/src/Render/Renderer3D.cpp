/**
 * @file Renderer3D.cpp
 * @brief 3D 网格渲染器实现。
 */

#include "Render/Renderer3D.h"

#include "Core/Log.h"
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

    GE_CORE_INFO("Renderer3D initialized");
}

Renderer3D::~Renderer3D() {
    GE_CORE_INFO("Renderer3D Shutdown");

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

    m_Meshes.push_back({transform, mesh, texture, color});
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
    frameUBO.pointLightPosition = glm::vec4(m_LightParams.pointLightPosition,
                                            m_LightParams.pointLightRadiusInv);
    frameUBO.pointLightColor = m_LightParams.pointLightColor;
    frameUBO.ambient = m_LightParams.ambient;

    BufferAllocation frameUboAlloc = frame.AllocateBuffer(
        vk::BufferUsageFlagBits::eUniformBuffer, sizeof(FrameUBO));
    frameUboAlloc.update(frameUBO);

    // ── 2. 为每个网格分配 Object UBO ──────────────────────────────────
    //    一次性分配一块大 buffer，每个网格占 sizeof(ObjectUBO) 字节
    size_t totalObjectUBOSize = m_Meshes.size() * sizeof(ObjectUBO);
    BufferAllocation objectUboAlloc = frame.AllocateBuffer(
        vk::BufferUsageFlagBits::eUniformBuffer, totalObjectUBOSize);

    std::vector<ObjectUBO> objectUBOs;
    objectUBOs.reserve(m_Meshes.size());
    for (const auto &instance : m_Meshes) {
        ObjectUBO ubo{};
        ubo.model = instance.transform;
        ubo.lodBias = 0.0f;
        ubo._pad = glm::vec3(0.0f);
        objectUBOs.push_back(ubo);
    }
    objectUboAlloc.update(objectUBOs);

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
        vk::DeviceSize uboOffset = objectUboAlloc.get_offset() + i * sizeof(ObjectUBO);

        // 绑定 Object UBO（set 2, binding 0）
        cmd.BindBuffer(objectUboAlloc.get_buffer(), uboOffset,
                       sizeof(ObjectUBO), 2, 0);

        // 绑定纹理（set 1, binding 0）
        if (instance.texture) {
            cmd.BindImage(instance.texture->GetImageView(),
                          instance.texture->GetSampler(),
                          1, 0);
        } else {
            // 无纹理时，纹理单元可能未定义行为
            // TODO: 提供一个默认的 1x1 白色纹理
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
