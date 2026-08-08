/**
 * @file Renderer3D.cpp
 * @brief 3D 网格渲染器实现。
 */

#include "Render/Renderer3D.h"

#include <algorithm>
#include <bit>

#include "Core/Log.h"
#include "Render/Texture.h"
#include "Render/Renderer.h"
#include "Render/VulkanBase/VulkanCommandBuffer.h"
#include "Render/VulkanBase/VulkanPipelineLayout.h"
#include "Render/VulkanBase/VulkanRenderContext.h"
#include "Render/VulkanBase/VulkanRenderFrame.h"
#include "Render/VulkanBase/VulkanRenderingInfo.h"
#include "Render/VulkanBase/VulkanResourceCache.h"
#include "Render/VulkanBase/VulkanShaderModule.h"

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

    // ── 3. 请求 PipelineLayout（通过反射自动构建） ─────────────────────
    //    注：阶段2 曾将 ObjectUBO 设为 Dynamic 以降低 descriptor set 数量，
    //    阶段3 把 per-instance 数据（model/color）迁入 InstanceData SSBO 后，
    //    ObjectUBO 仅按材质共享 lodBias，改回普通 uniform buffer（每材质组一个）。
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
                          Material *material,
                          const glm::vec4 &color) {
    GE_CORE_ASSERT(m_InScene, "DrawMesh called outside BeginScene/EndScene!");

    if (!mesh || mesh->GetIndexCount() == 0) {
        return;
    }

    // 计算排序键（材质 → mesh → view 空间深度），用于 EndScene 前分组排序，
    // 使同材质同 mesh 的实例连续，便于 instancing 合批
    uint64_t sortKey = ComputeSortKey(material, mesh, transform);

    m_Meshes.push_back({transform, mesh, material, color, sortKey});
}

// ============================================================================
// 内部工具方法（排序键相关）
// ============================================================================

Texture *Renderer3D::GetEffectiveTexture(const Material *material) const {
    // 优先取材质 Albedo 槽位纹理，无材质或无纹理时回退到默认白色纹理
    Texture *tex = (material ? material->GetTexture(Material::Albedo) : nullptr);
    return tex ? tex : m_DefaultWhiteTexture.get();
}

uint64_t Renderer3D::ComputeSortKey(const Material *material, const Mesh *mesh, const glm::mat4 &transform) const {
    // 材质分组：材质指针哈希折叠到 20 位，减少管线/纹理切换。
    // 材质完整决定渲染状态（纹理组合、着色器类型、混合等），比纹理更精确。
    // nullptr 材质统一视为 0，使其彼此相邻。
    uint64_t matHash = 0;
    if (material) {
        matHash = std::hash<const void *>{}(material) & MATERIAL_HASH_MASK;
    }

    // mesh 分组：指针哈希折叠到 12 位，使同材质内同 mesh 实例连续便于合批。
    // 仅用于排序，合批分组用指针相等判断，哈希碰撞不会导致错误合批。
    uint64_t meshHash = std::hash<const void *>{}(mesh) & MESH_HASH_MASK;

    // 深度：取模型变换的平移分量转换到 view 空间，取反得到正值（越大越远）。
    // 正浮点数的 IEEE 位模式随值单调递增，故可直接按位作为排序键，
    // 升序排列即实现不透明物体从前往后（early-z 优化）。
    glm::vec4 viewPos = m_View * transform[3];
    uint32_t depthBits = std::bit_cast<uint32_t>(-viewPos.z);

    return (matHash << MATERIAL_HASH_SHIFT)
         | (meshHash << MESH_HASH_SHIFT)
         | static_cast<uint64_t>(depthBits);
}

void Renderer3D::EndScene() {
    ZoneScopedN("Renderer3D::EndScene");

    GE_CORE_ASSERT(m_InScene, "EndScene called without BeginScene!");
    m_InScene = false;

    // 没有网格需要绘制，直接返回
    if (m_Meshes.empty()) {
        return;
    }

    // ── 0. 按排序键排序（材质 → mesh → 深度） ─────────────────────────
    //    使同材质同 mesh 的实例连续排列，既减少管线/纹理切换，又便于
    //    instancing 合批；深度从前往后，利用 early-z 减少过绘制。
    std::sort(m_Meshes.begin(), m_Meshes.end(),
              [](const MeshInstance &a, const MeshInstance &b) {
                  return a.sortKey < b.sortKey;
              });

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
        frameUBO.pointLightColors[i] = pl.color;
    }
    frameUBO.pointLightCount = glm::vec4(static_cast<float>(lightCount), 0.0f, 0.0f, 0.0f);

    frameUBO.ambient = m_LightParams.ambient;

    BufferAllocation frameUboAlloc = frame.AllocateBuffer(
        vk::BufferUsageFlagBits::eUniformBuffer, sizeof(FrameUBO));
    frameUboAlloc.update(frameUBO);

    // ── 2. 阶段3：按 (mesh, material) 分组合批，构建 per-instance SSBO ──
    //    排序键已保证同材质同 mesh 的实例连续。单趟扫描把 (mesh, material)
    //    指针相等且连续的实例归为一个 RenderBatch，并把每个实例的 (model,
    //    color) 收集进 instances 数组，最终一次性上传到全局 storage buffer。
    //    ObjectUBO 按材质共享（仅 lodBias），材质变化时分配一个普通 UBO。
    std::vector<InstanceData> instances;
    instances.reserve(m_Meshes.size());

    std::vector<RenderBatch> batches;
    batches.reserve(m_Meshes.size());

    const Material *currentMat = nullptr;
    BufferAllocation currentObjectUbo;  // 当前材质的共享 ObjectUBO（lodBias）

    for (size_t i = 0; i < m_Meshes.size();) {
        const auto &first = m_Meshes[i];
        // 注意：RenderBatch.material 为非 const 指针（沿用 MeshInstance 风格），
        // 此处用非 const 引用赋值，避免丢弃 const 限定导致编译错误。
        Material *mat = first.material;
        Mesh     *mesh = first.mesh;

        // 找同 (mesh, material) 的连续区间
        size_t runStart = i;
        while (i < m_Meshes.size()
               && m_Meshes[i].material == mat
               && m_Meshes[i].mesh == mesh) {
            ++i;
        }

        // 材质变化：为该材质分配共享 ObjectUBO（lodBias）
        if (mat != currentMat) {
            currentMat = mat;
            currentObjectUbo = frame.AllocateBuffer(
                vk::BufferUsageFlagBits::eUniformBuffer, sizeof(ObjectUBO));
            ObjectUBO ubo{};
            ubo.lodBias = 0.0f;
            ubo._pad = glm::vec3(0.0f);
            currentObjectUbo.update(ubo);
        }

        // 收集本批次实例的 per-instance 数据
        uint32_t firstInstance = static_cast<uint32_t>(instances.size());
        for (size_t k = runStart; k < i; ++k) {
            const auto &inst = m_Meshes[k];
            instances.push_back(InstanceData{inst.transform, inst.color});
        }

        batches.push_back(RenderBatch{
            mesh, mat, firstInstance,
            static_cast<uint32_t>(i - runStart),
            currentObjectUbo});
    }

    // 分配全局实例 SSBO 并一次性上传（所有批次共享）
    BufferAllocation instanceBuffer = frame.AllocateBuffer(
        vk::BufferUsageFlagBits::eStorageBuffer,
        instances.size() * sizeof(InstanceData));
    instanceBuffer.update(instances);

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

    // ====================================================================
    // 4. 配置管线状态
    // ====================================================================
    //
    // 管线状态分两类：
    //   A. 管线创建态 —— 决定管线 hash，命中缓存则复用已有管线
    //      - 附件格式、颜色混合、顶点输入、光栅化/深度/模板的"非动态"部分
    //   B. 动态状态值 —— 运行时通过 vkCmdSet* 改变，不影响管线 hash
    //      - 视口、剪刀、剔除模式、正面方向、图元拓扑、深度测试开关等
    //
    // 注意：某些状态（如 cullMode、depthTestEnable）既参与管线创建（当未设为
    //       动态时），也作为动态状态的当前值。这里在 enableDynamicState 之后
    //       仍保留 set* 调用，是为了设置动态状态的"初始值"。
    // ====================================================================

    cmd.BindPipelineLayout(*m_PipelineLayout);

    auto &ps = cmd.GetPipelineState();
    auto colorFmt = swapchain.GetFormat();

    // —— 4a. 附件格式 ——
    vk::Format depthFmt = vk::Format::eUndefined;
    if (renderTarget.HasDepth()) {
        depthFmt = renderTarget.GetDepthFormat();
    }
    ps.setRenderingFormats({colorFmt}, depthFmt);

    // —— 4b. 颜色混合（3D 不透明物体：无 alpha 混合，全通道写入）——
    vk::PipelineColorBlendAttachmentState blendState{};
    blendState.colorWriteMask = vk::ColorComponentFlagBits::eR
                                | vk::ColorComponentFlagBits::eG
                                | vk::ColorComponentFlagBits::eB
                                | vk::ColorComponentFlagBits::eA;
    ps.setColorBlendAttachments({blendState});

    // —— 4c. 顶点输入（从顶点着色器反射自动生成）——
    ps.setVertexInputFromShader(*m_VertShader);

    // —— 4d. 光栅化 + 深度/模板（默认值，同时作为动态状态初始值）——
    ps.setInputAssembly(vk::PrimitiveTopology::eTriangleList)
      .setCullMode(vk::CullModeFlagBits::eBack)
      .setFrontFace(vk::FrontFace::eCounterClockwise)
      .setDepthTestEnable(VK_TRUE)
      .setDepthWriteEnable(VK_TRUE)
      .setDepthCompareOp(vk::CompareOp::eLess);

    // —— 4e. 启用动态状态（这些状态运行时可通过 vkCmdSet* 改变）——
    ps.enableDynamicState(vk::DynamicState::eViewport)
      .enableDynamicState(vk::DynamicState::eScissor)
      .enableDynamicState(vk::DynamicState::eCullMode)
      .enableDynamicState(vk::DynamicState::eFrontFace)
      .enableDynamicState(vk::DynamicState::ePrimitiveTopology)
      .enableDynamicState(vk::DynamicState::eDepthTestEnable)
      .enableDynamicState(vk::DynamicState::eDepthWriteEnable)
      .enableDynamicState(vk::DynamicState::eDepthCompareOp);

    // —— 4f. 视口 + 剪刀矩形（动态状态，直接写入 command buffer）——
    vk::Viewport vp;
    vp.width    = static_cast<float>(extent.width);
    vp.height   = static_cast<float>(extent.height);
    vp.minDepth = 0.0f;
    vp.maxDepth = 1.0f;
    cmd.SetViewport(0, {vp});

    vk::Rect2D scissor;
    scissor.extent.width  = extent.width;
    scissor.extent.height = extent.height;
    cmd.SetScissor(0, {scissor});

    // ── 5. 绑定 Frame UBO（set 0, binding 0，所有网格共享） ───────────
    cmd.BindBuffer(frameUboAlloc.get_buffer(), frameUboAlloc.get_offset(),
                   frameUboAlloc.get_size(), 0, 0);

    // ── 6. 逐批次 instanced 绘制 ─────────────────────────────────────
    vk::DeviceSize vertexOffset = 0;
    for (const auto &batch : batches) {
        // 绑定纹理（set 1, binding 0）
        // 从 material 的 Albedo 槽位取纹理，无材质或无纹理时使用
        // 默认 1x1 白色纹理，避免未定义行为
        Texture *tex = GetEffectiveTexture(batch.material);
        if (tex) {
            cmd.BindImage(tex->GetImageView(),
                          tex->GetSampler(),
                          1, 0);
        }

        // 绑定 ObjectUBO（set 2, binding 0，普通 uniform，按材质共享）
        cmd.BindBuffer(batch.objectUbo.get_buffer(), batch.objectUbo.get_offset(),
                       batch.objectUbo.get_size(), 2, 0);

        // 绑定全局实例 SSBO（set 2, binding 1）——所有批次共享同一缓冲
        cmd.BindBuffer(instanceBuffer.get_buffer(), instanceBuffer.get_offset(),
                       instanceBuffer.get_size(), 2, 1);

        // 绑定顶点缓冲 + 索引缓冲
        cmd.BindVertexBuffers(0,
                              {std::ref(batch.mesh->GetVertexBuffer())},
                              {vertexOffset});
        cmd.BindIndexBuffer(batch.mesh->GetIndexBuffer(), 0, vk::IndexType::eUint32);

        // 绘制：instanced。firstInstance 让 gl_InstanceIndex 从全局实例缓冲
        // 的起始索引开始，所有实例在单个 vkCmdDrawIndexedInstanced 中完成。
        cmd.DrawIndexed(batch.mesh->GetIndexCount(), batch.instanceCount,
                        0, 0, batch.firstInstance);
    }

    // ── 6b. 统计 draw call 与三角形数量（draw call = 批次数量） ───────
    uint32_t triangles = 0;
    for (const auto &instance : m_Meshes) {
        triangles += instance.mesh->GetIndexCount() / 3;
    }
    Renderer::Get().AddStats3D(static_cast<uint32_t>(batches.size()), triangles);

    // 统计 instancing 批次数量（相同 mesh + 相同材质分一组），
    // 用于观察合批收益：批次数越少 → draw call 越少
    Renderer::Get().AddBatches3D(static_cast<uint32_t>(batches.size()));

    // ── 7. 结束渲染 ───────────────────────────────────────────────────
    VulkanRenderingInfo::End(vkCmd);
}

} // namespace GE
