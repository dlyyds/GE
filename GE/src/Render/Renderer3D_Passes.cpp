/**
 * @file Renderer3D_Passes.cpp
 * @brief Renderer3D RenderGraph pass 执行分片（GBuffer / Shadow / Lighting / Tonemap / Bloom / Transparent）。
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
// ============== RenderGraph Pass 执行 ==============
void Renderer3D::FlushScene(PassExecuteContext &ctx) {
    GE_PROFILE_SCOPE("Renderer3D::FlushScene");

    // RenderGraph execute 回调内调用：图已为该 pass 打开动态渲染、转好布局。
    // 这里只做排序/上传/绘制，不再 begin/end、不做任何布局转换。渲染目标相关
    // 的附件格式/深度/extent 全部取自 ctx（本 pass 已由 RenderGraph 打开的实际
    // 附件），RenderTarget override 已移除，改由调用方 execute 回调传入。
    FlushRetiredEnvironments();

    // 附件格式与尺寸：由 execute 上下文直接给出。无颜色附件的 pass 不会走到 3D。
    GE_CORE_ASSERT(ctx.colorAttachmentView, "Scene3D pass 必须声明颜色附件");
    const vk::Format colorFormat = ctx.colorAttachmentView->get_format();
    const vk::Format depthFormat = ctx.depthAttachmentView
                                       ? ctx.depthAttachmentView->get_format()
                                       : vk::Format::eUndefined;
    RecordScene(*ctx.cmd, *ctx.frame, colorFormat, depthFormat, ctx.renderArea.extent);

    // 本帧批次已消费，清空防"一帧多次消费/下一帧重复绘制"（BeginScene 亦会清）。
    m_Meshes.clear();
}
void Renderer3D::FlushGBuffer(PassExecuteContext &ctx) {
    GE_PROFILE_SCOPE("Renderer3D::FlushGBuffer");

    FlushRetiredEnvironments();

    // GBuffer pass 需要完整 MRT 附件列表。上下文由 RenderGraph 填充，顺序与
    // colorAttachments 一致，避免只透传首个颜色附件导致管线格式错误。
    GE_CORE_ASSERT(ctx.colorAttachmentViews.size() >= 4,
                   "GBuffer pass 必须声明 4 个颜色附件");

    std::vector<vk::Format> colorFormats;
    colorFormats.reserve(ctx.colorAttachmentViews.size());
    for (const auto *view : ctx.colorAttachmentViews) {
        colorFormats.push_back(view->get_format());
    }

    const vk::Format depthFormat = ctx.depthAttachmentView
                                       ? ctx.depthAttachmentView->get_format()
                                       : vk::Format::eUndefined;

    // 批次/缓冲由公共入口计算并缓存：ShadowMap 先于本 pass 执行，此处直接复用。
    PrepareDeferredBatches(*ctx.frame);

    ConfigureGBufferPipeline(*ctx.cmd, colorFormats, depthFormat, ctx.renderArea.extent);

    // GBuffer 片元着色器不读点光源 SSBO，避免为 set0 binding1 生成无 layout 绑定。
    BindSharedUniforms(*ctx.cmd, m_CachedFrameUBO, m_CachedLightBuffer,
                       /*bindLights=*/false);
    if (!m_OpaqueBatches.empty()) {
        DrawMeshInstances(*ctx.cmd, *ctx.frame, m_OpaqueBatches,
                          m_CachedInstanceBuffer, /*gbuffer=*/true);
    }

    RecordStats(static_cast<uint32_t>(m_OpaqueBatches.size() + m_TransparentBatches.size()));
}
void Renderer3D::PrepareDeferredBatches(VulkanRenderFrame &frame) {
    // 延迟链三条 pass 共享一次「排序 + 切分 + 上传」（阴影贴图计划 §5.5/S3）：
    // ShadowMap 在 GBuffer 之前执行，先算好并缓存；GBuffer/Transparent 直接复用，
    // 避免各自重排 m_Meshes 造成批次不一致。幂等：m_HasDeferredBatches 标记已算过。
    if (m_HasDeferredBatches) {
        return;
    }

    SortMeshes(m_Meshes);
    m_CachedFrameUBO = UploadFrameUBO(frame);

    std::vector<InstanceData> instances;
    std::vector<RenderBatch> batches;
    CollectBatches(m_Meshes, instances, batches);

    // 按 pass 切分批次为「不透明前缀 + 透明后缀」：CollectBatches 沿排序后的
    // m_Meshes 顺序生成批次（不透明 run 先、Blend 逐实例后），首个 Blend 批次
    // 之前的全部批次即不透明段（与 RecordScene 的切分逻辑一致）。空场景 / 全不透明
    // 时切分点在 end，透明段自然为空。
    const auto transparentIt = std::find_if(
        batches.begin(), batches.end(), [](const RenderBatch &b) {
            return b.material && b.material->alphaMode == Material::AlphaMode::Blend;
        });
    const size_t opaqueCount =
        static_cast<size_t>(std::distance(batches.begin(), transparentIt));

    m_CachedInstanceBuffer = UploadInstanceBuffer(frame, instances);
    m_CachedLightBuffer = UploadLightBuffer(frame);

    m_OpaqueBatches.assign(batches.begin(),
                           batches.begin() + static_cast<ptrdiff_t>(opaqueCount));
    m_TransparentBatches.assign(
        batches.begin() + static_cast<ptrdiff_t>(opaqueCount), batches.end());

    // 每级阴影专用批次：for c in [0, cascadeCount)，对 m_ShadowMeshes[c] 排序 → 切
    // 不透明段 → 上传独立实例缓冲，与主集合分开（阴影集合含主视锥外物体，实例内容
    // 不同）。FlushShadow(ctx, c) 画对应级（CSM 计划书 §4.4）。阴影 pass 只画不透明段
    // （Blend 不投影），切分逻辑与主集合一致。级数 = 1 时即现状单级（第 0 级 = 全视锥）。
    const uint32_t cascadeCount = std::clamp(m_LightParams.cascadeCount, 1u, kMaxCascades);
    for (uint32_t c = 0; c < cascadeCount; ++c) {
        SortMeshes(m_ShadowMeshes[c]);
        std::vector<InstanceData> shadowInstances;
        std::vector<RenderBatch> shadowBatches;
        CollectBatches(m_ShadowMeshes[c], shadowInstances, shadowBatches);
        const auto shadowTransparentIt = std::find_if(
            shadowBatches.begin(), shadowBatches.end(), [](const RenderBatch &b) {
                return b.material && b.material->alphaMode == Material::AlphaMode::Blend;
            });
        const size_t shadowOpaqueCount =
            static_cast<size_t>(std::distance(shadowBatches.begin(), shadowTransparentIt));
        m_ShadowBatches[c].assign(
            shadowBatches.begin(),
            shadowBatches.begin() + static_cast<ptrdiff_t>(shadowOpaqueCount));
        m_ShadowInstanceBuffer[c] = UploadInstanceBuffer(frame, shadowInstances);
    }

    m_HasDeferredBatches = true;
}
void Renderer3D::FlushShadow(PassExecuteContext &ctx, uint32_t cascade) {
    GE_PROFILE_SCOPE("Renderer3D::FlushShadow");

    // 某级阴影 pass 画该级阴影专用批次（m_ShadowBatches[cascade] +
    // m_ShadowInstanceBuffer[cascade]，阴影剔除计划书 §4.4/§5 S3 / CSM 计划书 §4.4）：
    // 集合由 Scene 逐级遍历按该级阴影世界 AABB 剔除后提交，含主相机视锥外的投影物
    // ——否则其阴影整段丢失。批次在 PrepareDeferredBatches 构建（本 pass 先于 GBuffer
    // 执行，幂等先算一次）。只画不透明段（Opaque + Mask，Blend 不投影、不接收阴影）。
    PrepareDeferredBatches(*ctx.frame);

    const vk::Format depthFormat = ctx.depthAttachmentView
                                       ? ctx.depthAttachmentView->get_format()
                                       : vk::Format::eUndefined;

    const BufferAllocation shadowFrameUbo = UploadShadowFrameUBO(*ctx.frame, cascade);

    // 视口 = 该级深度图尺寸（每级 pass 各自的 renderArea，见 SceneLayer 逐级声明）
    ConfigureShadowPipeline(*ctx.cmd, depthFormat, ctx.renderArea.extent);

    // 阴影专用 FrameUBO（该级光空间）绑定 set0 b0；深度片元不读点光源 SSBO，
    // 不绑 set0 b1（其布局无该 binding，避免校验告警）。
    auto &cmd = *ctx.cmd;
    cmd.BindBuffer(shadowFrameUbo.get_buffer(), shadowFrameUbo.get_offset(),
                   shadowFrameUbo.get_size(), 0, 0);
    if (!m_ShadowBatches[cascade].empty()) {
        DrawMeshInstances(*ctx.cmd, *ctx.frame, m_ShadowBatches[cascade],
                          m_ShadowInstanceBuffer[cascade],
                          /*gbuffer=*/false, /*shadow=*/true);
    }
}
void Renderer3D::FlushLighting(PassExecuteContext &ctx) {
    GE_PROFILE_SCOPE("Renderer3D::FlushLighting");

    GE_CORE_ASSERT(ctx.colorAttachmentView, "Lighting pass 必须声明颜色附件");

    const vk::Format colorFormat = ctx.colorAttachmentView->get_format();
    const BufferAllocation lightingUboAlloc = UploadLightingUBO(*ctx.frame);

    ConfigureLightingPipeline(*ctx.cmd, colorFormat, ctx.renderArea.extent);

    // 延迟光照 UBO + 点光源 SSBO + 天空盒采样入口。
    auto &cmd = *ctx.cmd;
    cmd.BindBuffer(lightingUboAlloc.get_buffer(), lightingUboAlloc.get_offset(),
                   lightingUboAlloc.get_size(), 0, 0);
    if (!m_CachedLightBuffer.empty()) {
        cmd.BindBuffer(m_CachedLightBuffer.get_buffer(),
                       m_CachedLightBuffer.get_offset(),
                       m_CachedLightBuffer.get_size(), 0, 1);
    }

    const Texture *skyTex = (m_EnvironmentMap && m_EnvironmentMap->IsReady())
                                ? &m_EnvironmentMap->GetSkybox()
                                : ((m_DefaultSkyboxTexture && m_DefaultSkyboxTexture->IsReady())
                                       ? m_DefaultSkyboxTexture.get()
                                       : nullptr);
    if (skyTex) {
        cmd.BindImage(skyTex->GetImageView(), skyTex->GetSampler(), 0, 2);
    }

    // 绑定 G0~G3。采样器可复用默认白纹里的线性 2D 采样器，Vulkan 描述符中
    // sampler 与 image view 解耦；这些资源没有额外携带 sampler。
    if (m_DefaultWhiteTexture) {
        const auto &gbufSampler = m_DefaultWhiteTexture->GetSampler();
        const uint32_t gbufferBindingCount =
            static_cast<uint32_t>(std::min<size_t>(ctx.readImageViews.size(), 4));
        for (uint32_t i = 0; i < gbufferBindingCount; ++i) {
            cmd.BindImage(*ctx.readImageViews[i], gbufSampler, 1, i);
        }
    }

    // 阴影深度图数组（set 1, binding 7, samplerShadowDepth[kMaxCascades]）：SceneLayer
    // 在方向光阴影开启时才追加 hShadow_C0..C{N-1} 读，故 readImageViews 多于 4 项即有
    // 各级阴影图（G0~G3 + ShadowMap_C0..）。逐级绑到数组元素；**未激活级/无阴影图时也
    // 必须绑有效描述符**：shader 静态索引 binding 7（CascadePCF 内 texture()），Vulkan
    // 描述符有效性按静态引用判定，数组所有元素未更新都会触发 VUID-vkCmdDraw-None-08114
    // ——故用默认白纹兜底（shadowParams.z=0 或未选中时不被采样，内容无关）。采样器用
    // 最近邻（§5.6），PCF 逐 tap 硬比较在 shader 侧完成。
    if (m_ShadowSampler) {
        for (uint32_t c = 0; c < kMaxCascades; ++c) {
            const size_t viewIdx = 4u + c;
            VulkanImageView *shadowView =
                (viewIdx < ctx.readImageViews.size())
                    ? ctx.readImageViews[viewIdx]
                    : (m_DefaultWhiteTexture ? &m_DefaultWhiteTexture->GetImageView()
                                             : nullptr);
            if (shadowView) {
                cmd.BindImage(*shadowView, *m_ShadowSampler, 1, 7, c);
            }
        }
    }

    // IBL 三件套（set 1, binding 4/5/6）：辐照度与预滤波共绑预滤波 cubemap，
    // BRDF LUT 是 2D。就绪且开关打开时绑真实环境图；否则绑回退纹理保证描述符
    // 完整（shader 侧 flags.y=0 走常量环境光，不采样这三张）。
    const bool iblReady = (m_EnvironmentMap != nullptr) && m_EnvironmentMap->IsReady()
                          && m_IBLEnabled;
    if (iblReady) {
        auto &ibl = *m_EnvironmentMap;
        cmd.BindImage(ibl.GetPrefilter().GetImageView(),
                      ibl.GetPrefilter().GetSampler(), 1, 4); // 辐照度（最高 mip）
        cmd.BindImage(ibl.GetPrefilter().GetImageView(),
                      ibl.GetPrefilter().GetSampler(), 1, 5); // 预滤波（按粗糙度取 mip）
        cmd.BindImage(ibl.GetBrdfLUT().GetImageView(),
                      ibl.GetBrdfLUT().GetSampler(), 1, 6);   // BRDF LUT
    } else {
        if (m_DefaultSkyboxTexture && m_DefaultSkyboxTexture->IsReady()) {
            cmd.BindImage(m_DefaultSkyboxTexture->GetImageView(),
                          m_DefaultSkyboxTexture->GetSampler(), 1, 4);
            cmd.BindImage(m_DefaultSkyboxTexture->GetImageView(),
                          m_DefaultSkyboxTexture->GetSampler(), 1, 5);
        }
        if (m_DefaultWhiteTexture) {
            cmd.BindImage(m_DefaultWhiteTexture->GetImageView(),
                          m_DefaultWhiteTexture->GetSampler(), 1, 6);
        }
    }

    cmd.Draw(3, 1, 0, 0);
}
void Renderer3D::FlushSceneColorCopy(PassExecuteContext &ctx) {
    GE_PROFILE_SCOPE("Renderer3D::FlushSceneColorCopy");

    GE_CORE_ASSERT(ctx.colorAttachmentView, "SceneColorCopy pass must have a color attachment");
    const vk::Format colorFormat = ctx.colorAttachmentView->get_format();

    ConfigureBloomFullscreenPipeline(*ctx.cmd, m_SceneColorCopyLayout, m_LightingVert,
                                     colorFormat, ctx.renderArea.extent);

    auto &cmd = *ctx.cmd;
    if (m_DefaultWhiteTexture && !ctx.readImageViews.empty()) {
        cmd.BindImage(*ctx.readImageViews[0], m_DefaultWhiteTexture->GetSampler(), 0, 0);
    }
    cmd.Draw(3, 1, 0, 0);
}
void Renderer3D::FlushSceneDepthCopy(PassExecuteContext &ctx) {
    GE_PROFILE_SCOPE("Renderer3D::FlushSceneDepthCopy");

    GE_CORE_ASSERT(ctx.colorAttachmentView, "SceneDepthCopy pass must have a color attachment");
    const vk::Format colorFormat = ctx.colorAttachmentView->get_format();

    ConfigureBloomFullscreenPipeline(*ctx.cmd, m_SceneDepthCopyLayout, m_LightingVert,
                                     colorFormat, ctx.renderArea.extent);

    auto &cmd = *ctx.cmd;
    if (!ctx.readImageViews.empty()) {
        VulkanSampler *sampler = m_ShadowSampler ? m_ShadowSampler
                                                    : (m_DefaultWhiteTexture
                                                           ? &m_DefaultWhiteTexture->GetSampler()
                                                           : nullptr);
        if (sampler) {
            cmd.BindImage(*ctx.readImageViews[0], *sampler, 0, 0);
        }
    }
    cmd.Draw(3, 1, 0, 0);
}
void Renderer3D::FlushTonemap(PassExecuteContext &ctx) {
    GE_PROFILE_SCOPE("Renderer3D::FlushTonemap");

    GE_CORE_ASSERT(ctx.colorAttachmentView, "Tonemap pass 必须声明颜色附件");

    const vk::Format colorFormat = ctx.colorAttachmentView->get_format();
    const BufferAllocation tonemapUboAlloc = UploadTonemapUBO(*ctx.frame);

    ConfigureTonemapPipeline(*ctx.cmd, colorFormat, ctx.renderArea.extent);

    // 绑定 Tonemap UBO（set 0 binding 0）并采样 HDR 缓冲（set 0 binding 1）。
    // 首张 readImageView 即 Scene_HDR；采样器复用默认白色纹理的线性 2D 采样器。
    auto &cmd = *ctx.cmd;
    cmd.BindBuffer(tonemapUboAlloc.get_buffer(), tonemapUboAlloc.get_offset(),
                   tonemapUboAlloc.get_size(), 0, 0);
    if (m_DefaultWhiteTexture && !ctx.readImageViews.empty()) {
        cmd.BindImage(*ctx.readImageViews[0], m_DefaultWhiteTexture->GetSampler(), 0, 1);
    }

    cmd.Draw(3, 1, 0, 0);
}
void Renderer3D::FlushBloomExtract(PassExecuteContext &ctx) {
    GE_PROFILE_SCOPE("Renderer3D::FlushBloomExtract");

    GE_CORE_ASSERT(ctx.colorAttachmentView, "BloomExtract pass 必须声明颜色附件");
    const vk::Format colorFormat = ctx.colorAttachmentView->get_format();
    const BufferAllocation uboAlloc = UploadBloomUBO(*ctx.frame, ctx.renderArea.extent);

    ConfigureBloomFullscreenPipeline(*ctx.cmd, m_BloomExtractLayout, m_BloomVert,
                                     colorFormat, ctx.renderArea.extent);

    auto &cmd = *ctx.cmd;
    cmd.BindBuffer(uboAlloc.get_buffer(), uboAlloc.get_offset(), uboAlloc.get_size(), 0, 0);
    if (m_DefaultWhiteTexture && !ctx.readImageViews.empty()) {
        cmd.BindImage(*ctx.readImageViews[0], m_DefaultWhiteTexture->GetSampler(), 0, 1);
    }
    cmd.Draw(3, 1, 0, 0);
}
void Renderer3D::FlushBloomDownsample(PassExecuteContext &ctx, uint32_t mip) {
    GE_PROFILE_SCOPE("Renderer3D::FlushBloomDownsample");

    GE_CORE_ASSERT(ctx.colorAttachmentView, "BloomDownsample pass 必须声明颜色附件");
    const vk::Format colorFormat = ctx.colorAttachmentView->get_format();
    const BufferAllocation uboAlloc = UploadBloomUBO(*ctx.frame, ctx.renderArea.extent);

    ConfigureBloomFullscreenPipeline(*ctx.cmd, m_BloomDownsampleLayout, m_BloomVert,
                                     colorFormat, ctx.renderArea.extent);

    auto &cmd = *ctx.cmd;
    cmd.BindBuffer(uboAlloc.get_buffer(), uboAlloc.get_offset(), uboAlloc.get_size(), 0, 0);
    if (m_DefaultWhiteTexture && !ctx.readImageViews.empty()) {
        cmd.BindImage(*ctx.readImageViews[0], m_DefaultWhiteTexture->GetSampler(), 0, 1);
    }
    cmd.Draw(3, 1, 0, 0);
}
void Renderer3D::FlushBloomUpsample(PassExecuteContext &ctx, uint32_t mip) {
    GE_PROFILE_SCOPE("Renderer3D::FlushBloomUpsample");

    GE_CORE_ASSERT(ctx.colorAttachmentView, "BloomUpsample pass 必须声明颜色附件");
    const vk::Format colorFormat = ctx.colorAttachmentView->get_format();
    const BufferAllocation uboAlloc = UploadBloomUBO(*ctx.frame, ctx.renderArea.extent);

    ConfigureBloomFullscreenPipeline(*ctx.cmd, m_BloomUpsampleLayout, m_BloomVert,
                                     colorFormat, ctx.renderArea.extent);

    auto &cmd = *ctx.cmd;
    cmd.BindBuffer(uboAlloc.get_buffer(), uboAlloc.get_offset(), uboAlloc.get_size(), 0, 0);
    if (m_DefaultWhiteTexture) {
        // bindings: readImages[0] = 小层（升采样源），readImages[1] = 同尺寸粗层
        if (!ctx.readImageViews.empty()) {
            cmd.BindImage(*ctx.readImageViews[0], m_DefaultWhiteTexture->GetSampler(), 0, 1);
        }
        if (ctx.readImageViews.size() > 1) {
            cmd.BindImage(*ctx.readImageViews[1], m_DefaultWhiteTexture->GetSampler(), 0, 2);
        }
    }
    cmd.Draw(3, 1, 0, 0);
}
void Renderer3D::FlushBloomComposite(PassExecuteContext &ctx) {
    GE_PROFILE_SCOPE("Renderer3D::FlushBloomComposite");

    GE_CORE_ASSERT(ctx.colorAttachmentView, "BloomComposite pass 必须声明颜色附件");
    const vk::Format colorFormat = ctx.colorAttachmentView->get_format();
    const BufferAllocation uboAlloc = UploadBloomUBO(*ctx.frame, ctx.renderArea.extent);

    ConfigureBloomFullscreenPipeline(*ctx.cmd, m_BloomCompositeLayout, m_BloomVert,
                                     colorFormat, ctx.renderArea.extent);

    auto &cmd = *ctx.cmd;
    cmd.BindBuffer(uboAlloc.get_buffer(), uboAlloc.get_offset(), uboAlloc.get_size(), 0, 0);
    if (m_DefaultWhiteTexture) {
        // bindings: readImages[0] = Scene_HDR，readImages[1] = 最终泛光层 (Bloom_Up0)
        if (!ctx.readImageViews.empty()) {
            cmd.BindImage(*ctx.readImageViews[0], m_DefaultWhiteTexture->GetSampler(), 0, 1);
        }
        if (ctx.readImageViews.size() > 1) {
            cmd.BindImage(*ctx.readImageViews[1], m_DefaultWhiteTexture->GetSampler(), 0, 2);
        }
    }
    cmd.Draw(3, 1, 0, 0);
}
void Renderer3D::FlushTransparent(PassExecuteContext &ctx) {
    GE_PROFILE_SCOPE("Renderer3D::FlushTransparent");

    // 透明段使用同一帧已经上传的 FrameUBO / LightSSBO / 实例 SSBO。
    if (m_HasDeferredBatches) {
        GE_CORE_ASSERT(ctx.colorAttachmentView, "Transparent pass 必须声明颜色附件");
        const vk::Format colorFormat = ctx.colorAttachmentView->get_format();
        const vk::Format depthFormat = ctx.depthAttachmentView
                                           ? ctx.depthAttachmentView->get_format()
                                           : vk::Format::eUndefined;

        ConfigureMeshPipeline(*ctx.cmd, colorFormat, depthFormat,
                              ctx.renderArea.extent, /*transparent=*/true, /*hdrTransparent=*/true);
        BindSharedUniforms(*ctx.cmd, m_CachedFrameUBO, m_CachedLightBuffer,
                           /*bindLights=*/true);
        if (!m_TransparentBatches.empty()) {
            DrawMeshInstances(*ctx.cmd, *ctx.frame, m_TransparentBatches,
                              m_CachedInstanceBuffer, /*gbuffer=*/false, /*shadow=*/false,
                              /*hdrTransparent=*/true);
        }

        // 水面（透明段末）：HDR 变体输出到 Scene_HDR（Tonemap 前）。
        DrawWaterBatches(*ctx.cmd, *ctx.frame, m_CachedFrameUBO, m_CachedLightBuffer,
                         colorFormat, depthFormat, ctx.renderArea.extent,
                         ctx.readImageViews, /*hdrTransparent=*/true);
        m_WaterBatches.clear();

        m_OpaqueBatches.clear();
        m_TransparentBatches.clear();
        for (auto &shadowBatches : m_ShadowBatches) {
            shadowBatches.clear();
        }
        m_HasDeferredBatches = false;
    }

    // 所有延迟 pass 均已消费本帧网格，清空时机从 FlushScene 尾部移到这里。
    m_Meshes.clear();
    for (auto &shadowMeshes : m_ShadowMeshes) {
        shadowMeshes.clear();
    }
}
void Renderer3D::FlushUnderwaterFX(PassExecuteContext &ctx) {
    GE_PROFILE_SCOPE("Renderer3D::FlushUnderwaterFX");

    GE_CORE_ASSERT(ctx.colorAttachmentView, "UnderwaterFX pass 必须声明颜色附件");
    const vk::Format colorFormat = ctx.colorAttachmentView->get_format();
    const BufferAllocation uboAlloc = UploadUnderwaterUBO(*ctx.frame);

    // 全屏三角形，与 Bloom/SceneDepthCopy 共用配置流程；顶点阶段复用 m_LightingVert。
    ConfigureBloomFullscreenPipeline(*ctx.cmd, m_UnderwaterLayout, m_LightingVert,
                                     colorFormat, ctx.renderArea.extent);

    auto &cmd = *ctx.cmd;
    cmd.BindBuffer(uboAlloc.get_buffer(), uboAlloc.get_offset(),
                   uboAlloc.get_size(), 0, 0);

    // readImageViews[0] = Scene_HDR（线性采样），readImageViews[1] = SceneDepth（最近邻）。
    if (m_DefaultWhiteTexture && !ctx.readImageViews.empty()) {
        cmd.BindImage(*ctx.readImageViews[0], m_DefaultWhiteTexture->GetSampler(), 0, 1);
    }
    VulkanSampler *sceneDepthSampler = m_ShadowSampler
                                           ? m_ShadowSampler
                                           : (m_DefaultWhiteTexture
                                                  ? &m_DefaultWhiteTexture->GetSampler()
                                                  : nullptr);
    if (sceneDepthSampler && ctx.readImageViews.size() > 1 && ctx.readImageViews[1]) {
        cmd.BindImage(*ctx.readImageViews[1], *sceneDepthSampler, 0, 2);
    }

    cmd.Draw(3, 1, 0, 0);
}
} // namespace GE
