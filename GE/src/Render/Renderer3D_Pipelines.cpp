/**
 * @file Renderer3D_Pipelines.cpp
 * @brief Renderer3D 各管线配置分片（Mesh / Water / Shadow / GBuffer / Lighting / Tonemap / Bloom）。
 */

#include "Render/Renderer3D.h"
#include "Renderer3DInternal.h"

#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <filesystem>

#include <glm/gtc/matrix_inverse.hpp> // glm::inverse（矩阵求逆）

#include "Scene/Components.h"
#include "Core/Log.h"
#include "Debug/Assert.h"
#include "Render/Texture.h"
#include "Render/Renderer.h"
#include "Render/RenderGraph/RenderPassDesc.h"
#include "Render/AssetManager.h"
#include "Render/TextureManager.h"
#include "Render/VulkanBase/VulkanCommandBuffer.h"
#include "Render/VulkanBase/VulkanPipelineLayout.h"
#include "Render/VulkanBase/VulkanRenderContext.h"
#include "Render/VulkanBase/VulkanRenderFrame.h"
#include "Render/VulkanBase/VulkanResourceCache.h"
#include "Render/VulkanBase/VulkanShaderModule.h"

#include "Debug/Profiler.h"

namespace GE {
// ==================== 管线配置 ====================
void Renderer3D::ConfigureShadowPipeline(VulkanCommandBuffer &cmd,
                                         vk::Format depthFormat,
                                         vk::Extent2D extent) {
    // 阴影深度 pass：零颜色附件 + 深度附件。顶点输入/动态状态与网格管线一致；
    // 剔除沿用逐批次 doubleSided 动态值（DrawMeshInstances 内设置），正面写深度。
    cmd.BindPipelineLayout(*m_PipelineLayoutShadow);

    auto &ps = cmd.GetPipelineState();
    ps.setRenderingFormats({}, depthFormat);

    // 无颜色附件 → 混合附件列表为空（与 0 颜色附件匹配）
    ps.setColorBlendAttachments({});

    ps.setVertexInputFromShader(*m_VertShader, 0, vk::VertexInputRate::eVertex,
                                static_cast<uint32_t>(sizeof(Vertex)));
    ps.setInputAssembly(vk::PrimitiveTopology::eTriangleList)
        .setCullMode(vk::CullModeFlagBits::eBack)
        .setFrontFace(vk::FrontFace::eCounterClockwise)
        .setDepthTestEnable(VK_TRUE)
        .setDepthWriteEnable(VK_TRUE)
        .setDepthCompareOp(vk::CompareOp::eLess);

    ps.enableDynamicState(vk::DynamicState::eViewport)
        .enableDynamicState(vk::DynamicState::eScissor)
        .enableDynamicState(vk::DynamicState::eCullMode)
        .enableDynamicState(vk::DynamicState::eFrontFace)
        .enableDynamicState(vk::DynamicState::ePrimitiveTopology)
        .enableDynamicState(vk::DynamicState::eDepthTestEnable)
        .enableDynamicState(vk::DynamicState::eDepthWriteEnable)
        .enableDynamicState(vk::DynamicState::eDepthCompareOp);

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
}
void Renderer3D::ConfigureMeshPipeline(VulkanCommandBuffer &cmd,
                                       vk::Format colorFormat, vk::Format depthFormat,
                                       vk::Extent2D extent,
                                       bool transparent,
                                       bool hdrTransparent) {
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
    //
    // transparent=true（透明段）：颜色混合启用（SRC_ALPHA / ONE_MINUS_SRC_ALPHA）
    // 且深度写关（保留深度测试，透明片元正确被不透明深缓冲遮挡）。两者都不是
    // 动态状态 → 混合/深度写参与管线 hash，首次 Draw 触发新的透明管线变体，
    // 同 layout 由资源缓存去重；Opaque/Mask 与 Blend 由此分流到不同管线。
    // ====================================================================

    cmd.BindPipelineLayout(*m_PipelineLayout);

    auto &ps = cmd.GetPipelineState();
    auto colorFmt = colorFormat;

    // —— 4a. 附件格式 ——
    vk::Format depthFmt = depthFormat;
    ps.setRenderingFormats({colorFmt}, depthFmt);

    // —— 4b. 颜色混合 ——
    vk::PipelineColorBlendAttachmentState blendState{};
    blendState.colorWriteMask = vk::ColorComponentFlagBits::eR
                                | vk::ColorComponentFlagBits::eG
                                | vk::ColorComponentFlagBits::eB
                                | vk::ColorComponentFlagBits::eA;
    if (transparent) {
        // 透明段：straight alpha 混合。srcAlpha=1（premultiplied 直通）让最终
        // 输出 alpha 就是片段 alpha 本身，back-to-front 排序下混合正确。
        blendState.blendEnable = VK_TRUE;
        blendState.srcColorBlendFactor = vk::BlendFactor::eSrcAlpha;
        blendState.dstColorBlendFactor = vk::BlendFactor::eOneMinusSrcAlpha;
        blendState.srcAlphaBlendFactor = vk::BlendFactor::eOne;
        if (hdrTransparent) {
            // HDR 透明：alpha 通道作为 Scene_HDR 的天空/几何元数据，强制收敛到 1，
            // 否则透明覆盖天空的像素会被 Tonemap 误判为天空而跳过 tonemap。
            blendState.dstAlphaBlendFactor = vk::BlendFactor::eZero;
        } else {
            blendState.dstAlphaBlendFactor = vk::BlendFactor::eOneMinusSrcAlpha;
        }
    }
    ps.setColorBlendAttachments({blendState});

    // —— 4c. 顶点输入（从顶点着色器反射自动生成）——
    //     stride 以 C++ Vertex 结构体尺寸为权威：mesh.vert 仅声明 location 0-3
    //     （忽略蒙皮字段），反射求和会得到 48B 的错误 stride，故显式传入
    //     sizeof(Vertex)（80B）覆盖；属性 offset 仍由反射紧密打包（前 4 字段
    //     与结构体前 48B 一一对应）。蒙皮管线 mesh_skinned.vert 声明全部
    //     location 后无需额外覆盖，但保持一致做法无害。
    ps.setVertexInputFromShader(*m_VertShader, 0, vk::VertexInputRate::eVertex,
                                static_cast<uint32_t>(sizeof(Vertex)));

    // —— 4d. 光栅化 + 深度/模板（默认值，同时作为动态状态初始值）——
    // 剔除 cullMode 是动态状态（值不参与 hash），段内实际值由 DrawMeshInstances
    // 按批次材质的 doubleSided 设置，这里仅设默认 eBack 作为兜底。
    ps.setInputAssembly(vk::PrimitiveTopology::eTriangleList)
        .setCullMode(vk::CullModeFlagBits::eBack)
        .setFrontFace(vk::FrontFace::eCounterClockwise)
        .setDepthTestEnable(VK_TRUE)
        .setDepthWriteEnable(transparent ? VK_FALSE : VK_TRUE)
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
    vp.width = static_cast<float>(extent.width);
    vp.height = static_cast<float>(extent.height);
    vp.minDepth = 0.0f;
    vp.maxDepth = 1.0f;
    cmd.SetViewport(0, {vp});

    vk::Rect2D scissor;
    scissor.extent.width = extent.width;
    scissor.extent.height = extent.height;
    cmd.SetScissor(0, {scissor});
}
void Renderer3D::ConfigureWaterPipeline(VulkanCommandBuffer &cmd,
                                        vk::Format colorFormat, vk::Format depthFormat,
                                        vk::Extent2D extent,
                                        bool hdrTransparent) {
    // 水面独立管线：绑定 water pipeline layout（water.vert + water.frag /
    // water_hdr.frag）。混合与深度语义与通用 Blend 透明段一致：alpha 混合、
    // 深度写关、深度测试开，远→近由透明段排序保证。
    VulkanPipelineLayout *layout = hdrTransparent ? m_WaterLayoutHDR : m_WaterLayout;
    if (!layout) {
        GE_CORE_WARN("ConfigureWaterPipeline: 水面管线布局未初始化，跳过");
        return;
    }
    cmd.BindPipelineLayout(*layout);

    auto &ps = cmd.GetPipelineState();
    ps.setRenderingFormats({colorFormat}, depthFormat);

    vk::PipelineColorBlendAttachmentState blendState{};
    blendState.colorWriteMask = vk::ColorComponentFlagBits::eR
                                | vk::ColorComponentFlagBits::eG
                                | vk::ColorComponentFlagBits::eB
                                | vk::ColorComponentFlagBits::eA;
    // 前向与 HDR 变体统一走 straight alpha 混合：混合因子来自片元 alpha
    // （water.frag / water_hdr.frag 按 Fresnel 逐像素输出，垂直透、掠射实）。
    // alpha 通道 dstAlpha=eOneMinusSrcAlpha 让 Scene_HDR 的天空/几何元数据
    // 自适应：底下是不透明几何 → 收敛到 1（Tonemap 当几何、正常曝光），底下是
    // 天空 → 保留片元 alpha（透到只剩天空时被 Tonemap 当天空直出）。
    blendState.blendEnable = VK_TRUE;
    blendState.srcColorBlendFactor = vk::BlendFactor::eSrcAlpha;
    blendState.dstColorBlendFactor = vk::BlendFactor::eOneMinusSrcAlpha;
    blendState.srcAlphaBlendFactor = vk::BlendFactor::eOne;
    blendState.dstAlphaBlendFactor = vk::BlendFactor::eOneMinusSrcAlpha;
    ps.setColorBlendAttachments({blendState});

    // 顶点输入：与通用网格一致，stride 以 C++ Vertex（80B）为权威
    ps.setVertexInputFromShader(*m_VertShaderWater, 0,
                                vk::VertexInputRate::eVertex,
                                static_cast<uint32_t>(sizeof(Vertex)));

    ps.setInputAssembly(vk::PrimitiveTopology::eTriangleList)
        .setCullMode(vk::CullModeFlagBits::eBack)
        .setFrontFace(vk::FrontFace::eCounterClockwise)
        .setDepthTestEnable(VK_TRUE)
        .setDepthWriteEnable(VK_FALSE)
        .setDepthCompareOp(vk::CompareOp::eLess);

    ps.enableDynamicState(vk::DynamicState::eViewport)
        .enableDynamicState(vk::DynamicState::eScissor)
        .enableDynamicState(vk::DynamicState::eCullMode)
        .enableDynamicState(vk::DynamicState::eFrontFace)
        .enableDynamicState(vk::DynamicState::ePrimitiveTopology)
        .enableDynamicState(vk::DynamicState::eDepthTestEnable)
        .enableDynamicState(vk::DynamicState::eDepthWriteEnable)
        .enableDynamicState(vk::DynamicState::eDepthCompareOp);

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
}
void Renderer3D::ConfigureGBufferPipeline(VulkanCommandBuffer &cmd,
                                          const std::vector<vk::Format> &colorFormats,
                                          vk::Format depthFormat,
                                          vk::Extent2D extent) {
    // GBuffer 管线与普通网格管线共享布局/顶点输入规则，但动态渲染附件格式是
    // MRT 四张图，且混合始终关闭。管线路由仍由 DrawMeshInstances 按蒙皮位切换
    // 到 m_PipelineLayoutSkinnedGBuffer，这里只预置静态网格顶点输入。
    cmd.BindPipelineLayout(*m_PipelineLayoutGBuffer);

    auto &ps = cmd.GetPipelineState();
    ps.setRenderingFormats(colorFormats, depthFormat);

    std::vector<vk::PipelineColorBlendAttachmentState> blendStates;
    blendStates.resize(colorFormats.size());
    for (auto &blendState : blendStates) {
        blendState.colorWriteMask = vk::ColorComponentFlagBits::eR
                                    | vk::ColorComponentFlagBits::eG
                                    | vk::ColorComponentFlagBits::eB
                                    | vk::ColorComponentFlagBits::eA;
    }
    ps.setColorBlendAttachments(blendStates);

    ps.setVertexInputFromShader(*m_VertShader, 0, vk::VertexInputRate::eVertex,
                                static_cast<uint32_t>(sizeof(Vertex)));
    ps.setInputAssembly(vk::PrimitiveTopology::eTriangleList)
        .setCullMode(vk::CullModeFlagBits::eBack)
        .setFrontFace(vk::FrontFace::eCounterClockwise)
        .setDepthTestEnable(VK_TRUE)
        .setDepthWriteEnable(VK_TRUE)
        .setDepthCompareOp(vk::CompareOp::eLess);

    ps.enableDynamicState(vk::DynamicState::eViewport)
        .enableDynamicState(vk::DynamicState::eScissor)
        .enableDynamicState(vk::DynamicState::eCullMode)
        .enableDynamicState(vk::DynamicState::eFrontFace)
        .enableDynamicState(vk::DynamicState::ePrimitiveTopology)
        .enableDynamicState(vk::DynamicState::eDepthTestEnable)
        .enableDynamicState(vk::DynamicState::eDepthWriteEnable)
        .enableDynamicState(vk::DynamicState::eDepthCompareOp);

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
}
void Renderer3D::ConfigureLightingPipeline(VulkanCommandBuffer &cmd,
                                           vk::Format colorFormat,
                                           vk::Extent2D extent) {
    // Lighting pass 不依赖顶点缓冲，也不需要深度附件：全屏三角形在天空分支
    // 直接输出背景，在几何分支覆盖 RGB 与 alpha。
    cmd.BindPipelineLayout(*m_LightingLayout);

    auto &ps = cmd.GetPipelineState();
    ps.setRenderingFormats({colorFormat});

    vk::PipelineColorBlendAttachmentState blendState{};
    blendState.colorWriteMask = vk::ColorComponentFlagBits::eR
                                | vk::ColorComponentFlagBits::eG
                                | vk::ColorComponentFlagBits::eB
                                | vk::ColorComponentFlagBits::eA;
    ps.setColorBlendAttachments({blendState});

    ps.setVertexInputFromShader(*m_LightingVert);
    ps.setInputAssembly(vk::PrimitiveTopology::eTriangleList)
        .setCullMode(vk::CullModeFlagBits::eNone)
        .setFrontFace(vk::FrontFace::eCounterClockwise)
        .setDepthTestEnable(VK_FALSE)
        .setDepthWriteEnable(VK_FALSE);

    ps.enableDynamicState(vk::DynamicState::eViewport)
        .enableDynamicState(vk::DynamicState::eScissor)
        .enableDynamicState(vk::DynamicState::eCullMode)
        .enableDynamicState(vk::DynamicState::eFrontFace)
        .enableDynamicState(vk::DynamicState::ePrimitiveTopology)
        .enableDynamicState(vk::DynamicState::eDepthTestEnable)
        .enableDynamicState(vk::DynamicState::eDepthWriteEnable)
        .enableDynamicState(vk::DynamicState::eDepthCompareOp);

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
}
void Renderer3D::ConfigureTonemapPipeline(VulkanCommandBuffer &cmd,
                                          vk::Format colorFormat,
                                          vk::Extent2D extent) {
    // Tonemap pass 不依赖顶点缓冲，也不需要深度附件；全屏三角形采样 HDR 后写出 LDR。
    cmd.BindPipelineLayout(*m_TonemapLayout);

    auto &ps = cmd.GetPipelineState();
    ps.setRenderingFormats({colorFormat});

    vk::PipelineColorBlendAttachmentState blendState{};
    blendState.colorWriteMask = vk::ColorComponentFlagBits::eR
                                | vk::ColorComponentFlagBits::eG
                                | vk::ColorComponentFlagBits::eB
                                | vk::ColorComponentFlagBits::eA;
    ps.setColorBlendAttachments({blendState});

    ps.setVertexInputFromShader(*m_TonemapVert);
    ps.setInputAssembly(vk::PrimitiveTopology::eTriangleList)
        .setCullMode(vk::CullModeFlagBits::eNone)
        .setFrontFace(vk::FrontFace::eCounterClockwise)
        .setDepthTestEnable(VK_FALSE)
        .setDepthWriteEnable(VK_FALSE);

    ps.enableDynamicState(vk::DynamicState::eViewport)
        .enableDynamicState(vk::DynamicState::eScissor)
        .enableDynamicState(vk::DynamicState::eCullMode)
        .enableDynamicState(vk::DynamicState::eFrontFace)
        .enableDynamicState(vk::DynamicState::ePrimitiveTopology)
        .enableDynamicState(vk::DynamicState::eDepthTestEnable)
        .enableDynamicState(vk::DynamicState::eDepthWriteEnable)
        .enableDynamicState(vk::DynamicState::eDepthCompareOp);

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
}
void Renderer3D::ConfigureBloomFullscreenPipeline(VulkanCommandBuffer &cmd,
                                                 VulkanPipelineLayout *layout,
                                                 VulkanShaderModule *vert,
                                                 vk::Format colorFormat,
                                                 vk::Extent2D extent) {
    // Bloom 各 pass 不依赖顶点缓冲 / 深度附件；全屏三角形采样一张或多张输入后写出。
    cmd.BindPipelineLayout(*layout);

    auto &ps = cmd.GetPipelineState();
    ps.setRenderingFormats({colorFormat});

    vk::PipelineColorBlendAttachmentState blendState{};
    blendState.colorWriteMask = vk::ColorComponentFlagBits::eR
                                | vk::ColorComponentFlagBits::eG
                                | vk::ColorComponentFlagBits::eB
                                | vk::ColorComponentFlagBits::eA;
    ps.setColorBlendAttachments({blendState});

    ps.setVertexInputFromShader(*vert);
    ps.setInputAssembly(vk::PrimitiveTopology::eTriangleList)
        .setCullMode(vk::CullModeFlagBits::eNone)
        .setFrontFace(vk::FrontFace::eCounterClockwise)
        .setDepthTestEnable(VK_FALSE)
        .setDepthWriteEnable(VK_FALSE);

    ps.enableDynamicState(vk::DynamicState::eViewport)
        .enableDynamicState(vk::DynamicState::eScissor)
        .enableDynamicState(vk::DynamicState::eCullMode)
        .enableDynamicState(vk::DynamicState::eFrontFace)
        .enableDynamicState(vk::DynamicState::ePrimitiveTopology)
        .enableDynamicState(vk::DynamicState::eDepthTestEnable)
        .enableDynamicState(vk::DynamicState::eDepthWriteEnable)
        .enableDynamicState(vk::DynamicState::eDepthCompareOp);

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
}
void Renderer3D::BindSharedUniforms(VulkanCommandBuffer &cmd,
                                    const BufferAllocation &frameUbo,
                                    const BufferAllocation &lightBuffer,
                                    bool bindLights) {
    // ── 5. 绑定 Frame UBO（set 0, binding 0，所有网格共享） ───────────
    cmd.BindBuffer(frameUbo.get_buffer(), frameUbo.get_offset(),
                   frameUbo.get_size(), 0, 0);

    // 绑定点光源 SSBO（set 0, binding 1）——所有网格共享。
    // GBuffer 阶段片元着色器不读光源，绑定会触发 layout 不匹配告警，故可关闭。
    if (bindLights) {
        cmd.BindBuffer(lightBuffer.get_buffer(), lightBuffer.get_offset(),
                       lightBuffer.get_size(), 0, 1);
    }
}
VulkanPipelineLayout *Renderer3D::ResolveMeshLayout(bool shadow, bool gbuffer,
                                                    bool pbr, bool useIbl,
                                                    bool skinned,
                                                    bool hdrTransparent) {
    // 优先级：阴影 > GBuffer > HDR 透明 > 前向。HDR 透明在 Tonemap 前输出到
    // Scene_HDR，必须用无 ACES 的片元变体（mesh_pbr_hdr / mesh_pbr_ibl_hdr）；
    // 每个分支取「蒙皮 / 静态」对应布局。顺序判定代替嵌套三元，直读。
    if (shadow) {
        return skinned ? m_PipelineLayoutSkinnedShadow : m_PipelineLayoutShadow;
    }
    if (gbuffer) {
        return skinned ? m_PipelineLayoutSkinnedGBuffer : m_PipelineLayoutGBuffer;
    }
    if (hdrTransparent && pbr && useIbl) {
        return skinned ? m_PipelineLayoutSkinnedPBR_IBL_HDR : m_PipelineLayoutPBR_IBL_HDR;
    }
    if (hdrTransparent && pbr) {
        return skinned ? m_PipelineLayoutSkinnedPBR_HDR : m_PipelineLayoutPBR_HDR;
    }
    if (pbr && useIbl) {
        return skinned ? m_PipelineLayoutSkinnedPBR_IBL : m_PipelineLayoutPBR_IBL;
    }
    if (pbr) {
        return skinned ? m_PipelineLayoutSkinnedPBR : m_PipelineLayoutPBR;
    }
    return skinned ? m_PipelineLayoutSkinned : m_PipelineLayout;
}
} // namespace GE
