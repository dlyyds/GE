#include "pch.h"

#include "Render/SceneRenderPasses.h"

#include "Render/Renderer.h"
#include "Render/Renderer2D.h"
#include "Render/Renderer3D.h"
#include "Render/ShadowCascade.h"

namespace GE {

void RecordScenePasses(RenderGraphBuilder &b,
                       ResourceHandle hColor,
                       ResourceHandle hDepth,
                       vk::Extent2D extent,
                       vk::ImageLayout colorFinalLayout,
                       const glm::vec4 &clearColor) {
    // ── 向本帧渲染图注册场景 pass：Scene3D → Scene2D，或延迟链
    //    GBuffer → Lighting → Transparent(HDR) → Tonemap → Scene2D ──
    // 图对象与 Builder 由 Renderer 托管（GetFrameGraphBuilder），每帧 BeginFrame
    // 末尾已 Reset；这里只声明 pass 读写，命令录制与 Execute 统一在 Renderer::EndFrame
    // （ImGui 上屏前）完成。故此处不复位 defer / RenderTarget——复位也由 Renderer
    // 在 Execute 后统一做。
    vk::Rect2D renderArea{{0, 0}, {extent.width, extent.height}};

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

        // HDR 中间缓冲：Lighting 写入线性 RGBA16F，Tonemap 采样后写回颜色输出。
        // alpha 通道作为天空/几何元数据：天空=0，几何/透明合成=1（HDR 计划书 §5.3）。
        RenderGraphResourceDesc hdrDesc;
        hdrDesc.extent = extent;
        hdrDesc.samples = vk::SampleCountFlagBits::e1;
        hdrDesc.format = vk::Format::eR16G16B16A16Sfloat;
        ResourceHandle hHDR = b.CreateVirtualResource(hdrDesc, "Scene_HDR");
        // Stage 2: copies of the opaque scene used by the water pass.
        // Scene_HDR_Base is a snapshot of Lighting output; SceneDepth is the
        // opaque depth copied into a sampled color texture. Water reads these
        // instead of the live Transparent attachments to avoid self-dependency.
        RenderGraphResourceDesc hdrBaseDesc = hdrDesc;
        ResourceHandle hHDRBase = b.CreateVirtualResource(hdrBaseDesc, "Scene_HDR_Base");
        RenderGraphResourceDesc sceneDepthDesc;
        sceneDepthDesc.extent = extent;
        sceneDepthDesc.samples = vk::SampleCountFlagBits::e1;
        sceneDepthDesc.format = vk::Format::eR32Sfloat;
        ResourceHandle hSceneDepth = b.CreateVirtualResource(sceneDepthDesc, "SceneDepth");

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
                shadowDepth.loadOp = vk::AttachmentLoadOp::eClear; // 每帧清空重画
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

        // Pass2a1 "SceneColorCopy": Lighting -> Scene_HDR_Base (for water refraction).
        RenderPassDesc &sceneColorCopyPass = b.AddPass("SceneColorCopy");
        sceneColorCopyPass.renderArea = renderArea;
        sceneColorCopyPass.readImages.push_back(
            {hHDR, ResourceUsage::ShaderRead, vk::ImageLayout::eShaderReadOnlyOptimal});
        AttachmentDesc sceneColorCopyColor;
        sceneColorCopyColor.resource = hHDRBase;
        sceneColorCopyColor.usage = ResourceUsage::ColorAttachment;
        sceneColorCopyColor.loadOp = vk::AttachmentLoadOp::eClear;
        sceneColorCopyColor.storeOp = vk::AttachmentStoreOp::eStore;
        sceneColorCopyColor.clearValue.color = {0.0f, 0.0f, 0.0f, 0.0f};
        sceneColorCopyPass.colorAttachments.push_back(sceneColorCopyColor);
        sceneColorCopyPass.execute = [](PassExecuteContext &ctx) {
            Renderer::Get3DRenderer().FlushSceneColorCopy(ctx);
        };

        // Pass2a2 "SceneDepthCopy": opaque depth -> SceneDepth (sampled R32 color).
        RenderPassDesc &sceneDepthCopyPass = b.AddPass("SceneDepthCopy");
        sceneDepthCopyPass.renderArea = renderArea;
        sceneDepthCopyPass.readImages.push_back(
            {hDepth, ResourceUsage::ShaderRead, vk::ImageLayout::eShaderReadOnlyOptimal});
        AttachmentDesc sceneDepthCopyColor;
        sceneDepthCopyColor.resource = hSceneDepth;
        sceneDepthCopyColor.usage = ResourceUsage::ColorAttachment;
        sceneDepthCopyColor.loadOp = vk::AttachmentLoadOp::eClear;
        sceneDepthCopyColor.storeOp = vk::AttachmentStoreOp::eStore;
        sceneDepthCopyColor.clearValue.color = {0.0f, 0.0f, 0.0f, 0.0f};
        sceneDepthCopyPass.colorAttachments.push_back(sceneDepthCopyColor);
        sceneDepthCopyPass.execute = [](PassExecuteContext &ctx) {
            Renderer::Get3DRenderer().FlushSceneDepthCopy(ctx);
        };

        // Pass2b "Transparent"：在 Tonemap 前写回 Scene_HDR，在线性 HDR 空间
        // 合成。颜色 eLoad（Lighting 已清/写 HDR）、深度 eLoad，叠在前向透明
        // 混合管线上；FlushTransparent 以 hdrTransparent=true 配置混合，强制
        // alpha 通道收敛到 1，避免透明覆盖天空后被 Tonemap 误判为天空。
        RenderPassDesc &transparentPass = b.AddPass("Transparent");
        transparentPass.renderArea = renderArea;
        // Stage 2: water reads the Scene_HDR snapshot and SceneDepth from these inputs.
        transparentPass.readImages.push_back(
            {hHDRBase, ResourceUsage::ShaderRead, vk::ImageLayout::eShaderReadOnlyOptimal});
        transparentPass.readImages.push_back(
            {hSceneDepth, ResourceUsage::ShaderRead, vk::ImageLayout::eShaderReadOnlyOptimal});
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

        // Pass2b+ "UnderwaterFX"：Transparent 之后、Bloom 之前读 Scene_HDR + SceneDepth，
        // 写独立的 Underwater_HDR（RGBA16F）。alpha 哨兵原样直传，下游 Bloom/Tonemap 语义不变。
        // 仅当有水面批次 && 平滑淹没量 > 阈值 && 总开关打开时声明；否则保持 hHDR 直通。
        const bool underwaterEnabled = Renderer::Get3DRenderer().ShouldRunUnderwaterFx();
        ResourceHandle hPostProcessSource = hHDR;
        if (underwaterEnabled) {
            RenderGraphResourceDesc uwDesc = hdrDesc;
            ResourceHandle hUnderwater = b.CreateVirtualResource(uwDesc, "Underwater_HDR");
            RenderPassDesc &uwPass = b.AddPass("UnderwaterFX");
            uwPass.renderArea = renderArea;
            uwPass.readImages.push_back(
                {hHDR, ResourceUsage::ShaderRead, vk::ImageLayout::eShaderReadOnlyOptimal});
            uwPass.readImages.push_back(
                {hSceneDepth, ResourceUsage::ShaderRead, vk::ImageLayout::eShaderReadOnlyOptimal});
            AttachmentDesc uwColor;
            uwColor.resource = hUnderwater;
            uwColor.usage = ResourceUsage::ColorAttachment;
            uwColor.loadOp = vk::AttachmentLoadOp::eClear;
            uwColor.storeOp = vk::AttachmentStoreOp::eStore;
            uwColor.clearValue.color = {0.0f, 0.0f, 0.0f, 0.0f};
            uwPass.colorAttachments.push_back(uwColor);
            uwPass.execute = [](PassExecuteContext &ctx) {
                Renderer::Get3DRenderer().FlushUnderwaterFX(ctx);
            };
            hPostProcessSource = hUnderwater;
        }

        // Pass2b+ "Bloom"：Transparent 之后、Tonemap 之前读/写 Scene_HDR。
        // 只对几何像素（hdr.a>=0.5）提取高光，做 N 级降采样 → 升采样 → 加回 HDR。
        // 天空不参与 bloom，避免环境图被错误放大。
        const uint32_t bloomLevels = std::clamp(
            Renderer::Get3DRenderer().GetBloomMipLevels(),
            1u, Renderer3D::kMaxBloomMipLevels);
        const bool bloomEnabled = Renderer::Get3DRenderer().IsBloomEnabled();
        // Bloom 开启时用一个独立合成缓冲，避免在同一个 pass 内既采样又写入 Scene_HDR。
        ResourceHandle hHDRFinal = hPostProcessSource;
        if (bloomEnabled && bloomLevels > 0) {
            // 全分辨率提取高光，随后从半分辨率开始逐级 1/2；格式与 Scene_HDR 对齐（RGBA16F）。
            RenderGraphResourceDesc bloomDesc;
            bloomDesc.samples = vk::SampleCountFlagBits::e1;
            bloomDesc.format = vk::Format::eR16G16B16A16Sfloat;

            auto bloomMipExtent = [&extent](uint32_t i) {
                return vk::Extent2D{
                    std::max(1u, extent.width >> (i + 1)),
                    std::max(1u, extent.height >> (i + 1))};
            };

            std::array<ResourceHandle, Renderer3D::kMaxBloomMipLevels> hBloomDown{};
            std::array<ResourceHandle, Renderer3D::kMaxBloomMipLevels> hBloomUp{};
            for (uint32_t i = 0; i < bloomLevels; ++i) {
                bloomDesc.extent = bloomMipExtent(i);
                hBloomDown[i] = b.CreateVirtualResource(
                    bloomDesc, "Bloom_Down" + std::to_string(i));
                if (i + 1 < bloomLevels) {
                    hBloomUp[i] = b.CreateVirtualResource(
                        bloomDesc, "Bloom_Up" + std::to_string(i));
                }
            }

            // 全分辨率高光缓冲：先在全分辨率按像素阈值提亮，再用 Karis 降采样
            // 到半分辨率。相比“先降采样再阈值”，这避免了亚像素金属高光被邻域
            // 平均后压到阈值以下、随相机旋转突然消失引起的闪烁。
            bloomDesc.extent = extent;
            ResourceHandle hBloomFull = b.CreateVirtualResource(
                bloomDesc, "Bloom_Full");

            // 合成目标：BloomComposite 读 Scene_HDR + 泛光层，写出 Scene_AfterBloom，
            // 再由 Tonemap 采样；避免同一资源在同一 pass 自读自写。
            bloomDesc.extent = extent;
            ResourceHandle hAfterBloom = b.CreateVirtualResource(
                bloomDesc, "Scene_AfterBloom");
            hHDRFinal = hAfterBloom;

            // Extract：Scene_HDR → Bloom_Full（全分辨率），天空排除。
            RenderPassDesc &bloomExtractPass = b.AddPass("BloomExtract");
            bloomExtractPass.renderArea = renderArea;
            bloomExtractPass.readImages.push_back(
                {hPostProcessSource, ResourceUsage::ShaderRead, vk::ImageLayout::eShaderReadOnlyOptimal});
            AttachmentDesc bloomExtractColor;
            bloomExtractColor.resource = hBloomFull;
            bloomExtractColor.usage = ResourceUsage::ColorAttachment;
            bloomExtractColor.loadOp = vk::AttachmentLoadOp::eClear;
            bloomExtractColor.storeOp = vk::AttachmentStoreOp::eStore;
            bloomExtractColor.clearValue.color = {0.0f, 0.0f, 0.0f, 0.0f};
            bloomExtractPass.colorAttachments.push_back(bloomExtractColor);
            bloomExtractPass.execute = [](PassExecuteContext &ctx) {
                Renderer::Get3DRenderer().FlushBloomExtract(ctx);
            };

            // Downsample：Bloom_Full → Bloom_Down0（半分辨率），Karis 平均。
            RenderPassDesc &bloomFirstDown = b.AddPass("BloomDownsample0");
            bloomFirstDown.renderArea = vk::Rect2D{{0, 0}, bloomMipExtent(0)};
            bloomFirstDown.readImages.push_back(
            {hBloomFull, ResourceUsage::ShaderRead,
             vk::ImageLayout::eShaderReadOnlyOptimal});
            AttachmentDesc firstDownColor;
            firstDownColor.resource = hBloomDown[0];
            firstDownColor.usage = ResourceUsage::ColorAttachment;
            firstDownColor.loadOp = vk::AttachmentLoadOp::eClear;
            firstDownColor.storeOp = vk::AttachmentStoreOp::eStore;
            firstDownColor.clearValue.color = {0.0f, 0.0f, 0.0f, 0.0f};
            bloomFirstDown.colorAttachments.push_back(firstDownColor);
            bloomFirstDown.execute = [](PassExecuteContext &ctx) {
                Renderer::Get3DRenderer().FlushBloomDownsample(ctx, 0);
            };

            // Downsample：Bloom_Down[i-1] → Bloom_Down[i]，逐级 1/2。
            for (uint32_t i = 1; i < bloomLevels; ++i) {
                RenderPassDesc &pass = b.AddPass("BloomDownsample" + std::to_string(i));
                pass.renderArea = vk::Rect2D{{0, 0}, bloomMipExtent(i)};
                pass.readImages.push_back(
                {hBloomDown[i - 1], ResourceUsage::ShaderRead,
                 vk::ImageLayout::eShaderReadOnlyOptimal});
                AttachmentDesc color;
                color.resource = hBloomDown[i];
                color.usage = ResourceUsage::ColorAttachment;
                color.loadOp = vk::AttachmentLoadOp::eClear;
                color.storeOp = vk::AttachmentStoreOp::eStore;
                color.clearValue.color = {0.0f, 0.0f, 0.0f, 1.0f};
                pass.colorAttachments.push_back(color);
                pass.execute = [i](PassExecuteContext &ctx) {
                    Renderer::Get3DRenderer().FlushBloomDownsample(ctx, i);
                };
            }

            // Upsample：Bloom_Down[levels-1] 或上一级 Bloom_Up 升回，并与同尺寸粗层相加。
            // 输出索引从 levels-2 递减到 0；Bloom_Up[0] 为最终泛光层。
            if (bloomLevels > 1) {
                uint32_t m = bloomLevels - 1; // m 是输出 Up 的索引，首帧为 levels-2
                while (true) {
                    --m; // 首个输出 = levels-2（与 Bloom_Up[levels-2] 尺寸一致）
                    const ResourceHandle smallRes =
                        (m + 1 >= bloomLevels - 1)
                            ? hBloomDown[bloomLevels - 1]
                            : hBloomUp[m + 1];
                    RenderPassDesc &pass = b.AddPass("BloomUpsample" + std::to_string(m));
                    pass.renderArea = vk::Rect2D{{0, 0}, bloomMipExtent(m)};
                    pass.readImages.push_back(
                    {smallRes, ResourceUsage::ShaderRead,
                     vk::ImageLayout::eShaderReadOnlyOptimal});
                    pass.readImages.push_back(
                    {hBloomDown[m], ResourceUsage::ShaderRead,
                     vk::ImageLayout::eShaderReadOnlyOptimal});
                    AttachmentDesc color;
                    color.resource = hBloomUp[m];
                    color.usage = ResourceUsage::ColorAttachment;
                    color.loadOp = vk::AttachmentLoadOp::eClear;
                    color.storeOp = vk::AttachmentStoreOp::eStore;
                    color.clearValue.color = {0.0f, 0.0f, 0.0f, 1.0f};
                    pass.colorAttachments.push_back(color);
                    pass.execute = [m](PassExecuteContext &ctx) {
                        Renderer::Get3DRenderer().FlushBloomUpsample(ctx, m);
                    };
                    if (m == 0) {
                        break;
                    }
                }
            }

            // Composite：Scene_HDR + 最终泛光 → Scene_AfterBloom（只加几何像素）。
            const ResourceHandle finalBloom =
                bloomLevels > 1 ? hBloomUp[0] : hBloomDown[0];
            RenderPassDesc &bloomCompositePass = b.AddPass("BloomComposite");
            bloomCompositePass.renderArea = renderArea;
            bloomCompositePass.readImages.push_back(
                {hPostProcessSource, ResourceUsage::ShaderRead, vk::ImageLayout::eShaderReadOnlyOptimal});
            bloomCompositePass.readImages.push_back(
                {finalBloom, ResourceUsage::ShaderRead, vk::ImageLayout::eShaderReadOnlyOptimal});
            AttachmentDesc bloomCompositeColor;
            bloomCompositeColor.resource = hHDRFinal;
            bloomCompositeColor.usage = ResourceUsage::ColorAttachment;
            bloomCompositeColor.loadOp = vk::AttachmentLoadOp::eClear;
            bloomCompositeColor.storeOp = vk::AttachmentStoreOp::eStore;
            bloomCompositeColor.clearValue.color = {0.0f, 0.0f, 0.0f, 0.0f};
            bloomCompositePass.colorAttachments.push_back(bloomCompositeColor);
            bloomCompositePass.execute = [](PassExecuteContext &ctx) {
                Renderer::Get3DRenderer().FlushBloomComposite(ctx);
            };
        }

        // Pass2c "Tonemap"：采样 Scene_HDR（开启 Bloom 时为合成后的 Scene_AfterBloom），
        // 曝光 + ACES 后写入颜色输出。
        RenderPassDesc &tonemapPass = b.AddPass("Tonemap");
        tonemapPass.renderArea = renderArea;
        tonemapPass.readImages.push_back(
            {hHDRFinal, ResourceUsage::ShaderRead, vk::ImageLayout::eShaderReadOnlyOptimal});
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
    // 遮挡），UI 批 depthTest 关不读写；颜色收尾布局由调用方决定（编辑器转
    // ShaderReadOnlyOptimal 供 ImGui 采样视口图；运行时保持附件态交给 UIPass）。
    RenderPassDesc &scene2D = b.AddPass("Scene2D");
    scene2D.renderArea = renderArea;
    AttachmentDesc colorLoad;
    colorLoad.resource = hColor;
    colorLoad.usage = ResourceUsage::ColorAttachment;
    colorLoad.loadOp = vk::AttachmentLoadOp::eLoad;
    colorLoad.storeOp = vk::AttachmentStoreOp::eStore;
    colorLoad.finalLayout = colorFinalLayout;
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

} // namespace GE
