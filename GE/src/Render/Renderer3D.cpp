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

    // ── 2. 将 ObjectUBO 标记为 Dynamic（阶段2：同材质合批） ────────────
    //    必须在请求 PipelineLayout 之前设置：反射出的 DescriptorSetLayout
    //    会据此把 set 2 binding 0 创建为 eUniformBufferDynamic，使同材质
    //    的多个 mesh 共享同一 descriptor set，draw 间仅更新动态偏移。
    //    ObjectUBO 在顶点/片元着色器中都有声明，此处设置顶点着色器即可，
    //    PipelineLayout 合并同名资源时保留先出现资源的 mode。
    m_VertShader->set_resource_mode("ObjectUBO", ShaderResourceMode::Dynamic);

    // ── 3. 请求 PipelineLayout（通过反射自动构建） ─────────────────────
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

    // 计算排序键（管线 → 纹理 → view 空间深度），用于 EndScene 前的状态分组排序
    uint64_t sortKey = ComputeSortKey(GetEffectiveTexture(material), transform);

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

uint64_t Renderer3D::ComputeSortKey(Texture *texture, const glm::mat4 &transform) const {
    // 管线 id：当前仅一套管线状态，恒为 0（后续阶段从管线 hash 获取）
    uint64_t pipelineId = 0;

    // 纹理分组：纹理指针哈希折叠到 24 位，减少纹理绑定切换
    uint64_t texHash = 0;
    if (texture) {
        texHash = std::hash<const void *>{}(texture) & TEXTURE_HASH_MASK;
    }

    // 深度：取模型变换的平移分量转换到 view 空间，取反得到正值（越大越远）。
    // 正浮点数的 IEEE 位模式随值单调递增，故可直接按位作为排序键，
    // 升序排列即实现不透明物体从前往后（early-z 优化）。
    glm::vec4 viewPos = m_View * transform[3];
    uint32_t depthBits = std::bit_cast<uint32_t>(-viewPos.z);

    return (pipelineId << PIPELINE_ID_SHIFT)
         | (texHash << TEXTURE_HASH_SHIFT)
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

    // ── 0. 按排序键排序（管线 → 纹理 → 深度） ──────────────────────────
    //    使同材质的 mesh 连续排列，减少管线/纹理切换；深度从前往后，
    //    利用 early-z 减少过绘制。为后续 Dynamic UBO / 合批打基础。
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

    // ── 2. ObjectUBO 改为 Dynamic：按材质分组分配连续 UBO 块 ──────────
    //    set 2 binding 0 已标记为 eUniformBufferDynamic，因此同材质
    //    （同有效纹理）的 mesh 共享同一个 DescriptorSet，draw 间仅更新
    //    动态偏移，大幅减少 descriptor set 分配/绑定开销。
    //
    //    同一材质组内所有 mesh 的 ObjectUBO 写入一块连续内存，每个 mesh
    //    的偏移对齐到 minUniformBufferOffsetAlignment，一次性上传。
    auto &device = Renderer::GetVulkanContext().GetDevice();
    vk::DeviceSize uboAlign = device.GetGpu().GetProperties()
                                  .limits.minUniformBufferOffsetAlignment;
    vk::DeviceSize alignedUboSize = ((sizeof(ObjectUBO) + uboAlign - 1) / uboAlign) * uboAlign;

    // 记录每个 mesh 绘制时绑定的组缓冲 + 动态偏移
    struct ObjectUboBinding {
        BufferAllocation alloc;        ///< 所属材质组的 UBO 分配（组内共享）
        vk::DeviceSize   dynamicOffset; ///< 该 mesh 在组缓冲内的动态偏移
    };
    std::vector<ObjectUboBinding> objectUboBindings;
    objectUboBindings.reserve(m_Meshes.size());

    // 分组遍历：阶段1 排序已保证同材质 mesh 连续，识别组边界即可
    for (size_t i = 0; i < m_Meshes.size();) {
        Texture *groupTex = GetEffectiveTexture(m_Meshes[i].material);

        // 找到当前组的结束位置（有效纹理变化处）
        size_t groupStart = i;
        while (i < m_Meshes.size()
               && GetEffectiveTexture(m_Meshes[i].material) == groupTex) {
            ++i;
        }
        size_t groupCount = i - groupStart;

        // 为组内所有 mesh 分配一块连续 UBO 内存
        BufferAllocation groupAlloc = frame.AllocateBuffer(
            vk::BufferUsageFlagBits::eUniformBuffer,
            groupCount * alignedUboSize);

        // 写入组内每个 mesh 的 ObjectUBO（偏移按对齐后大小递增）
        for (size_t k = 0; k < groupCount; ++k) {
            const auto &instance = m_Meshes[groupStart + k];
            ObjectUBO ubo{};
            ubo.model = instance.transform;
            ubo.lodBias = 0.0f;
            ubo._pad = glm::vec3(0.0f);
            ubo.color = instance.color;

            groupAlloc.update(ubo, static_cast<uint32_t>(k * alignedUboSize));
            objectUboBindings.push_back(
                ObjectUboBinding{groupAlloc, k * alignedUboSize});
        }
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

    // ── 6. 逐个绘制网格 ───────────────────────────────────────────────
    vk::DeviceSize vertexOffset = 0;
    for (size_t i = 0; i < m_Meshes.size(); ++i) {
        const auto &instance = m_Meshes[i];
        const auto &ubo = objectUboBindings[i];

        // 绑定 Object UBO（set 2, binding 0，动态）
        // 传入组缓冲 + 该 mesh 的动态偏移；descriptor set 在组内复用，
        // bindDescriptorSets 每次传递新的动态偏移，draw 间不重复分配
        cmd.BindBuffer(ubo.alloc.get_buffer(), ubo.dynamicOffset,
                       alignedUboSize, 2, 0);

        // 绑定纹理（set 1, binding 0）
        // 从 material 的 Albedo 槽位取纹理，无材质或无纹理时使用
        // 默认 1x1 白色纹理，避免未定义行为
        Texture *tex = GetEffectiveTexture(instance.material);
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

    // ── 6b. 统计 draw call 与三角形数量（每个网格一个 draw call） ───────
    uint32_t triangles = 0;
    for (const auto &instance : m_Meshes) {
        triangles += instance.mesh->GetIndexCount() / 3;
    }
    Renderer::Get().AddStats3D(static_cast<uint32_t>(m_Meshes.size()), triangles);

    // 统计排序后的批次数（按有效纹理指针分组，反映同材质连续排列的程度，
    // 用于观察状态分组优化收益：批次数越少 → 纹理切换越少）
    uint32_t batches = 1;
    for (size_t i = 1; i < m_Meshes.size(); ++i) {
        if (GetEffectiveTexture(m_Meshes[i].material)
                != GetEffectiveTexture(m_Meshes[i - 1].material)) {
            ++batches;
        }
    }
    Renderer::Get().AddBatches3D(batches);

    // ── 7. 结束渲染 ───────────────────────────────────────────────────
    VulkanRenderingInfo::End(vkCmd);
}

} // namespace GE
