/**
 * @file Renderer3D_Record.cpp
 * @brief Renderer3D 命令记录与网格/水面绘制分片。
 */

#include "Render/Renderer3D.h"
#include "Renderer3DInternal.h"

#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <filesystem>

#include <glm/gtc/matrix_inverse.hpp> // glm::inverse（矩阵求逆）
#include <glm/gtc/matrix_transform.hpp> // glm::scale（水面单位网格按 Size 缩放）

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
// ==================== 记录 / 绘制 ====================
void Renderer3D::RecordScene(VulkanCommandBuffer &cmd, VulkanRenderFrame &frame,
                             vk::Format colorFormat, vk::Format depthFormat,
                             vk::Extent2D extent) {
    GE_PROFILE_SCOPE("Renderer3D::RecordScene");

    SortMeshes(m_Meshes);

    // ── 共享描述符数据上传：Frame UBO / per-instance SSBO / 点光源 SSBO ──
    BufferAllocation frameUboAlloc = UploadFrameUBO(frame);

    std::vector<InstanceData> instances;
    std::vector<RenderBatch> batches;
    CollectBatches(m_Meshes, instances, batches);

    // 按 pass 切分批次为「不透明前缀 + 透明后缀」：CollectBatches 沿排序后的
    // m_Meshes 顺序生成批次（不透明 run 先、Blend 逐实例后），批次序与实例序
    // 一致 → 首个 Blend 批次之前的全部批次即不透明段。注意必须以批次自身判定
    // 而非实例下标：不透明合批会让实例数 ≠ 批次数，无法用实例下标等价切分。
    // 空场景 / 全不透明时切分点在 end，透明段自然为空。
    const auto transparentIt = std::find_if(
        batches.begin(), batches.end(), [](const RenderBatch &b) {
            return b.material && b.material->alphaMode == Material::AlphaMode::Blend;
        });
    const size_t opaqueCount =
        static_cast<size_t>(std::distance(batches.begin(), transparentIt));

    BufferAllocation instanceBuffer = UploadInstanceBuffer(frame, instances);

    BufferAllocation lightBuffer = UploadLightBuffer(frame);

    // ── 绘制：天空盒背景 → 网格批次（管线 + 描述符 + 绘制）。动态渲染已由图打开 ──
    // 天空盒：由"有无天空盒"控制（仅开关开启时提交；纹理是否就绪由 DrawSkybox 内部再判定）
    if (m_SkyboxEnabled) {
        DrawSkybox(cmd, frame, colorFormat, depthFormat, extent);
    }

    // ── 段 1：不透明 pass（Opaque + Mask）──────────────────────────────
    // 混合关 + 深度写开（= 现状），近→远排序吃 early-z。存在不透明批次才录制。
    ConfigureMeshPipeline(cmd, colorFormat, depthFormat, extent, /*transparent=*/false);
    BindSharedUniforms(cmd, frameUboAlloc, lightBuffer);
    if (opaqueCount > 0) {
        const std::vector<RenderBatch> opaqueBatches(
            batches.begin(), batches.begin() + static_cast<ptrdiff_t>(opaqueCount));
        DrawMeshInstances(cmd, frame, opaqueBatches, instanceBuffer);
    }

    // ── 段 2：透明 pass（Blend）────────────────────────────────────────
    // alpha 混合（SRC_ALPHA / ONE_MINUS_SRC_ALPHA）+ 深度写关（保留深度测试），
    // 远→近排序。ConfigureMeshPipeline(transparent=true) 只改混合附件与深度写
    // （两者均非动态）→ 首次 Draw 即触发透明管线变体，同 layout 由资源缓存去重。
    if (opaqueCount < batches.size()) {
        ConfigureMeshPipeline(cmd, colorFormat, depthFormat, extent, /*transparent=*/true);
        const std::vector<RenderBatch> transparentBatches(
            batches.begin() + static_cast<ptrdiff_t>(opaqueCount), batches.end());
        DrawMeshInstances(cmd, frame, transparentBatches, instanceBuffer);
    }

    // ── 段 3：水面（透明段末）────────────────────────────────
    // 水面不在通用网格批次中：独立水格 + 独立 WaterUBO，但同样走 Blend 透明段。
    DrawWaterBatches(cmd, frame, frameUboAlloc, lightBuffer,
                     colorFormat, depthFormat, extent, {}, /*hdrTransparent=*/false);
    // ── 统计 draw call 与三角形数量（含水面批次）──
    RecordStats(static_cast<uint32_t>(batches.size() + m_WaterBatches.size()));
    m_WaterBatches.clear();
}
void Renderer3D::SortMeshes(std::vector<MeshInstance> &meshes) {
    // ── 0. 按排序键排序（pass → 材质 → mesh → 深度方向）───────────────
    //    SortKey 的 operator< 依次比较 pass → pipeline → material → mesh →
    //    submesh → depthBits：pass 在最高位，使不透明（Opaque/Mask）与透明
    //    （Blend）各成干净连续前缀（不透明先、透明后），RecordScene 才能用
    //    首个 Blend 批次切分两段。段内按 pipeline/material/mesh 连续（便于
    //    instancing 合批与减切换），深度方向正确（不透明近→远吃 early-z；
    //    透明远→近供 alpha 混合）。
    std::sort(meshes.begin(), meshes.end(),
              [](const MeshInstance &a, const MeshInstance &b) {
                  return a.sortKey < b.sortKey;
              });

}
void Renderer3D::CollectBatches(const std::vector<MeshInstance> &meshes,
                                std::vector<InstanceData> &instances,
                                std::vector<RenderBatch> &batches) const {
    // ── 2. 阶段3：按 (mesh, material) 分组合批，构建 per-instance SSBO ──
    //    排序键已保证同材质同 mesh 的实例连续。单趟扫描把 (mesh, material)
    //    指针相等且连续的实例归为一个 RenderBatch，并把每个实例的 (model,
    //    color) 收集进 instances 数组，最终一次性上传到全局 storage buffer。
    instances.clear();
    instances.reserve(meshes.size());

    batches.clear();
    batches.reserve(meshes.size());

    for (size_t i = 0; i < meshes.size();) {
        const auto &first = meshes[i];
        Material *mat = first.material;
        Mesh *mesh = first.mesh;
        uint32_t firstIndex = first.firstIndex;
        uint32_t indexCount = first.indexCount;
        const void *skinKey = first.skinKey;

        // 找同 (mesh, 子网格, material, skin) 的连续区间。
        // skin 也参与分桶：不同皮肤不可合批（关节矩阵不同），且蒙皮/静态亦分离。
        // 注：batchKey（混合附件状态）在 ComputeSortKey 中排序聚拢、保证 Opaque/Mask
        // 混合状态只切换一次，但**不参与合批**（见下 transparent 分支）。
        size_t runStart = i;
        while (i < meshes.size()
               && meshes[i].material == mat
               && meshes[i].mesh == mesh
               && meshes[i].firstIndex == firstIndex
               && meshes[i].indexCount == indexCount
               && meshes[i].skinKey == skinKey) {
            ++i;
        }

        // 透明（Blend）实例不合并相邻同键实例，一律逐实例成批（instanceCount=1）。
        // 原因：同一 instanced 批次内部实例深度排序被"压扁"成批次位置；当同材质
        // 同 mesh 的远、近透明实例间穿插别的透明物体时，正确顺序需跨 mesh 交错
        // 插值，instancing 做不到。不透明实例（可合批、吃 early-z）保持原合并逻辑。
        if (mat && mat->alphaMode == Material::AlphaMode::Blend) {
            uint32_t firstInstance = static_cast<uint32_t>(instances.size());
            for (size_t k = runStart; k < i; ++k) {
                const auto &inst = meshes[k];
                instances.push_back(InstanceData{inst.transform, inst.color});
                // 每个透明实例各自一批：RunEnd 紧邻 RunStart，instanceCount=1。
                batches.push_back(RenderBatch{
                    inst.mesh, inst.firstIndex, inst.indexCount, inst.material,
                    firstInstance, 1, inst.skinKey});
                ++firstInstance;
            }
            continue;
        }

        // 收集本批次实例的 per-instance 数据（model + color）
        // 材质标量参数（如 shininess）已迁入 per-material UBO，
        // 在绘制循环中按批次绑定，不在此冗余写入实例数据。
        uint32_t firstInstance = static_cast<uint32_t>(instances.size());
        for (size_t k = runStart; k < i; ++k) {
            const auto &inst = meshes[k];
            instances.push_back(InstanceData{inst.transform, inst.color});
        }

        batches.push_back(RenderBatch{
            mesh, firstIndex, indexCount, mat, firstInstance,
            static_cast<uint32_t>(i - runStart), skinKey});
    }
}
void Renderer3D::DrawSkybox(VulkanCommandBuffer &cmd, VulkanRenderFrame &frame,
                            vk::Format colorFormat, vk::Format depthFormat,
                            vk::Extent2D extent) {
    // ====================================================================
    // 3b. 天空盒绘制（自包含块，先于网格，作为背景）
    // ====================================================================
    //    全屏三角形 + 反投影重建视线方向 + 采样等距柱状全景图。
    //    关闭深度测试/写入，天空盒始终位于最远背景；网格随后以 clear 后的
    //    深度（远平面）正常深度测试并覆盖。此块设置完整管线状态（含渲染
    //    格式、动态状态、视口/剪刀），随后网格配置块会重新覆盖为网格状态，
    //    两者互不干扰。
    // 天空盒纹理由环境图 EnvironmentMap 统一持有
    const Texture *skyTex = (m_EnvironmentMap && m_EnvironmentMap->IsReady())
                                ? &m_EnvironmentMap->GetSkybox()
                                : nullptr;

    if (m_SkyboxEnabled && skyTex) {
        auto skyColorFmt = colorFormat;
        vk::Format skyDepthFmt = depthFormat;

        // 分配天空盒 UBO：仅旋转的视图矩阵逆 + 投影矩阵逆
        //   mat3(m_View) 去掉平移，使天空盒不受相机位置影响（始终"无限远"）
        SkyboxUBO skyboxUBO{};
        skyboxUBO.invView = glm::inverse(glm::mat4(glm::mat3(m_View)));
        skyboxUBO.invProj = glm::inverse(m_Projection);
        BufferAllocation skyboxUboAlloc = frame.AllocateBuffer(
            vk::BufferUsageFlagBits::eUniformBuffer, sizeof(SkyboxUBO));
        skyboxUboAlloc.update(skyboxUBO);

        // 绑定天空盒管线布局
        cmd.BindPipelineLayout(*m_SkyboxLayout);
        auto &skyPs = cmd.GetPipelineState();
        skyPs.setRenderingFormats({skyColorFmt}, skyDepthFmt);

        // 颜色混合（不透明，全通道写入）
        vk::PipelineColorBlendAttachmentState skyBlend{};
        skyBlend.colorWriteMask = vk::ColorComponentFlagBits::eR
                                  | vk::ColorComponentFlagBits::eG
                                  | vk::ColorComponentFlagBits::eB
                                  | vk::ColorComponentFlagBits::eA;
        skyPs.setColorBlendAttachments({skyBlend});

        // 顶点输入（全屏三角形无顶点缓冲，反射为空）
        skyPs.setVertexInputFromShader(*m_SkyboxVert);

        // 光栅化：背面剔除关闭（全屏三角形风序不固定）、深度测试/写入关闭
        skyPs.setInputAssembly(vk::PrimitiveTopology::eTriangleList)
            .setCullMode(vk::CullModeFlagBits::eNone)
            .setFrontFace(vk::FrontFace::eCounterClockwise)
            .setDepthTestEnable(VK_FALSE)
            .setDepthWriteEnable(VK_FALSE);

        // 启用动态状态（视口/剪刀/剔除/深度等运行时设置）
        skyPs.enableDynamicState(vk::DynamicState::eViewport)
            .enableDynamicState(vk::DynamicState::eScissor)
            .enableDynamicState(vk::DynamicState::eCullMode)
            .enableDynamicState(vk::DynamicState::eFrontFace)
            .enableDynamicState(vk::DynamicState::ePrimitiveTopology)
            .enableDynamicState(vk::DynamicState::eDepthTestEnable)
            .enableDynamicState(vk::DynamicState::eDepthWriteEnable)
            .enableDynamicState(vk::DynamicState::eDepthCompareOp);

        // 视口 + 剪刀（与网格一致，覆盖整个渲染目标）
        vk::Viewport skyVp;
        skyVp.width = static_cast<float>(extent.width);
        skyVp.height = static_cast<float>(extent.height);
        skyVp.minDepth = 0.0f;
        skyVp.maxDepth = 1.0f;
        cmd.SetViewport(0, {skyVp});

        vk::Rect2D skyScissor;
        skyScissor.extent.width = extent.width;
        skyScissor.extent.height = extent.height;
        cmd.SetScissor(0, {skyScissor});

        // 绑定天空盒 UBO + 等距纹理，绘制全屏三角形（3 顶点，无顶点缓冲）
        cmd.BindBuffer(skyboxUboAlloc.get_buffer(), skyboxUboAlloc.get_offset(),
                       skyboxUboAlloc.get_size(), 0, 0);
        cmd.BindImage(skyTex->GetImageView(),
                      skyTex->GetSampler(), 0, 1);
        cmd.Draw(3, 1, 0, 0);
    }
}
void Renderer3D::DrawMeshInstances(VulkanCommandBuffer &cmd, VulkanRenderFrame &frame,
                                   const std::vector<RenderBatch> &batches,
                                   const BufferAllocation &instanceBuffer,
                                   bool gbuffer,
                                   bool shadow,
                                   bool hdrTransparent) {
    // ── 6. 逐批次 instanced 绘制 ─────────────────────────────────────
    vk::DeviceSize vertexOffset = 0;

    // IBL 是否启用（全局）：需要已加载环境图且三张图均已就绪且 IBL 开关打开，
    // 此时 PBR 批次走 IBL 变体管线并绑定三张 IBL 图；否则回退无 IBL 变体
    // （常量环境光）。异步加载中环境图未就绪则本帧不启用 IBL，就绪后自动切换。
    // 阴影深度 pass 不做 IBL。
    const bool useIbl = !gbuffer && !shadow
                        && (m_EnvironmentMap != nullptr) && m_EnvironmentMap->IsReady()
                        && m_IBLEnabled;

    // 当前绑定的管线 id（初始为 Blinn-Phong，已在上方绑定 *m_PipelineLayout）。
    // 排序键已按 pipelineId 分组，故同类型批次连续，切换频率最低。
    uint8_t currentPipelineId = detail::kBlinnPipelineId;

    for (const auto &batch : batches) {
        // —— 管线路由：pipelineId 由「材质(PBR 位) + 是否蒙皮(蒙皮位)」决定。
        //    batch.skinKey 非 null ⇒ 蒙皮实例，走 mesh_skinned 管线并含蒙皮位。
        //    切换时同时按对应顶点着色器反射重设顶点输入（蒙皮肤管线有 location
        //    4/5，静态管线没有），否则蒙皮字段会被静态顶点输入丢弃。
        const bool skinned = (batch.skinKey != nullptr);
        const uint8_t pipelineId = detail::ComposePipelineId(
            GetPipelineId(batch.material), skinned);
        const bool pbr = detail::IsPbrPipeline(pipelineId);

        if (pipelineId != currentPipelineId) {
            VulkanPipelineLayout *targetLayout =
                ResolveMeshLayout(shadow, gbuffer, pbr, useIbl, skinned,
                                  hdrTransparent);

            auto &ps = cmd.GetPipelineState();
            if (skinned) {
                ps.setVertexInputFromShader(*m_VertShaderSkinned, 0,
                                            vk::VertexInputRate::eVertex,
                                            static_cast<uint32_t>(sizeof(Vertex)));
            } else {
                ps.setVertexInputFromShader(*m_VertShader, 0,
                                            vk::VertexInputRate::eVertex,
                                            static_cast<uint32_t>(sizeof(Vertex)));
            }

            cmd.BindPipelineLayout(*targetLayout);
            currentPipelineId = pipelineId;

            // 路由到 PBR-IBL 时绑定 IBL 三件套（set 1, binding 5/6/7）。
            // 排序保证同管线批次连续，故只在首次切换时绑一次；非 IBL 布局
            // （含 Blinn）的 set 1 无这些 binding，不会读到。
            // 辐照度复用预滤波 cubemap（漫反射采样其最高 mip）。
            if (!gbuffer && pbr && useIbl) {
                auto &ibl = *m_EnvironmentMap;
                cmd.BindImage(ibl.GetPrefilter().GetImageView(),
                              ibl.GetPrefilter().GetSampler(), 1, 5); // 辐照度
                cmd.BindImage(ibl.GetPrefilter().GetImageView(),
                              ibl.GetPrefilter().GetSampler(), 1, 6); // 预滤波
                cmd.BindImage(ibl.GetBrdfLUT().GetImageView(),
                              ibl.GetBrdfLUT().GetSampler(), 1, 7); // BRDF LUT
            }
        }

        // —— 动态剔除（doubleSided 接线）——
        // cullMode 是动态状态（值不参与管线 hash）：同 layout 的双面 / 单面批次
        // 只改动态值即可，无需新建管线变体。doubleSided 材质关背面剔除（内外都
        // 渲染），否则保持默认背面剔除。Draw 前 Flush 会把本值刷入 cmd buffer，
        // 逐批次无条件写入（值即使不变也只是重复一次 vkCmdSet，成本可忽略）。
        const bool doubleSided = (batch.material && batch.material->doubleSided);
        cmd.GetPipelineState().setCullMode(
            doubleSided ? vk::CullModeFlags(vk::CullModeFlagBits::eNone)
                        : vk::CullModeFlags(vk::CullModeFlagBits::eBack));

        // 绑定纹理（set 1, binding 0 = Albedo，binding 1 = Normal）
        // 从 material 对应槽位取纹理，无材质或无纹理时使用默认纹理 fallback
        Texture *tex = GetEffectiveTexture(batch.material);
        if (tex) {
            cmd.BindImage(tex->GetImageView(),
                          tex->GetSampler(),
                          1, 0);
        }

        // 法线贴图（set 1, binding 1）：无材质或无纹理时使用默认"平坦法线"
        // 纹理，其映射回 (0,0,1) 不改变光照，保证材质无需法线贴图也能正常渲染。
        // 阴影深度片元不读法线（depth_only.frag 布局无 binding 1），跳过。
        if (!shadow) {
            Texture *normalTex = GetEffectiveNormalTexture(batch.material);
            if (normalTex) {
                cmd.BindImage(normalTex->GetImageView(),
                              normalTex->GetSampler(),
                              1, 1);
            }
        }

        // 自发光贴图（set 1, binding 3）：无材质或无纹理时使用默认白色纹理，
        // 采样为 (1,1,1)，自发光 = emissiveFactor 颜色，支持"仅因子发光"。
        // 有贴图时贴图颜色 × 因子，贴图黑色区域不发光。阴影深度片元不读，跳过。
        if (!shadow) {
            Texture *emissiveTex = GetEffectiveEmissiveTexture(batch.material);
            if (emissiveTex) {
                cmd.BindImage(emissiveTex->GetImageView(),
                              emissiveTex->GetSampler(),
                              1, 3);
            }
        }

        // 金属-粗糙度贴图（set 1, binding 4）：仅 PBR 材质使用。无 MR 贴图时
        // 绑定默认 (G=1, B=1) 纹理，回退到标量 metallic/roughness。
        // Blinn-Phong 批次不绑定（其 set 1 布局无 binding 4，绑了也会被过滤，
        // 这里显式判断更清晰，避免多余绑定）。PBR 蒙皮肤也一样要绑。
        // 阴影深度片元不读 MR（depth_only.frag 布局无 binding 4），跳过。
        if ((pbr || gbuffer) && !shadow) {
            Texture *mrTex = GetEffectiveMetallicRoughnessTexture(batch.material);
            if (mrTex) {
                cmd.BindImage(mrTex->GetImageView(),
                              mrTex->GetSampler(),
                              1, 4);
            }
        }

        // 绑定材质 UBO（set 1, binding 2）：存材质标量参数。按批次写入，
        // 同批次的实例共享同一材质，故值恒定，无需 per-instance。
        //   params: shininess / specularStrength / uvTiling（Blinn-Phong 共用）
        //   pbr:    metallic / roughness（PBR）
        //   emissiveFactor: 自发光颜色因子（乘自发光贴图颜色，两类型共用）
        MaterialUBO materialUBO{};
        materialUBO.params.x = batch.material
                                   ? batch.material->GetFloat("shininess", 32.0f)
                                   : 32.0f;
        materialUBO.params.y = batch.material
                                   ? batch.material->GetFloat("specularStrength", 0.5f)
                                   : 0.5f;
        // params.z = alphaCutoff（MASK 裁剪阈值）。Opaque/Blend 填 -1 关闭 discard
        // （shader 侧以 params.z >= 0 判定是否启用裁剪）；Mask 填实际阈值。
        materialUBO.params.z = (batch.material
                                && batch.material->alphaMode == Material::AlphaMode::Mask)
                                   ? batch.material->alphaCutoff
                                   : -1.0f;
        materialUBO.params.w = batch.material
                                   ? batch.material->GetFloat("uvTiling", 1.0f)
                                   : 1.0f;
        materialUBO.pbr.x = batch.material
                                ? batch.material->GetFloat("metallic", 0.0f)
                                : 0.0f;
        materialUBO.pbr.y = batch.material
                                ? batch.material->GetFloat("roughness", 0.5f)
                                : 0.5f;
        // pbr.z 作为着色模型标志：GBuffer 片元据此写 G0.a 的 PBR/Blinn 哨兵。
        // 前向片元着色器不使用该字段，延迟 GBuffer 路径则是必需输入。
        materialUBO.pbr.z = (batch.material
                             && batch.material->GetType() == Material::Type::PBR)
                                ? 1.0f
                                : 0.0f;
        // emissiveFactor.rgb = 自发光颜色因子；.w = 材质基础 alpha（baseAlpha，
        // glTF baseColorFactor[3] / OBJ dissolve 语义），shader 侧乘进最终 alpha。
        glm::vec4 emissive(0.0f);
        if (batch.material) {
            emissive = glm::vec4(batch.material->GetEmissiveFactor(),
                                 batch.material->GetFloat("baseAlpha", 1.0f));
        }
        materialUBO.emissiveFactor = emissive;
        BufferAllocation materialUboAlloc = frame.AllocateBuffer(
            vk::BufferUsageFlagBits::eUniformBuffer, sizeof(MaterialUBO));
        materialUboAlloc.update(materialUBO);
        cmd.BindBuffer(materialUboAlloc.get_buffer(), materialUboAlloc.get_offset(),
                       materialUboAlloc.get_size(), 1, 2);

        // 绑定全局实例 SSBO（set 2, binding 0）——所有批次共享同一缓冲
        cmd.BindBuffer(instanceBuffer.get_buffer(), instanceBuffer.get_offset(),
                       instanceBuffer.get_size(), 2, 0);

        // 蒙皮批次：绑定该皮肤的关节矩阵 SSBO（set 2, binding 1）。
        // 每个皮肤一段连续区间（Scene 每帧 UpdateSkins 上传、此处按 skinKey 查出），
        // 顶点着色器以顶点关节索引直接寻址（baseJoint ≡ 0）。
        if (skinned) {
            auto it = m_SkinJointBuffers.find(batch.skinKey);
            if (it != m_SkinJointBuffers.end()) {
                const BufferAllocation &joint = it->second;
                cmd.BindBuffer(joint.get_buffer(), joint.get_offset(),
                               joint.get_size(), 2, 1);
            } else {
                // 皮肤未注册（关节实体被删等）：跳过绘制，避免读到脏矩阵
                GE_CORE_WARN("蒙皮批次缺少关节矩阵缓冲（皮肤 {} 未注册或已失效），跳过",
                             reinterpret_cast<uintptr_t>(batch.skinKey));
                continue;
            }
        }

        // 绑定顶点缓冲 + 索引缓冲
        cmd.BindVertexBuffers(0,
                              {std::ref(batch.mesh->GetVertexBuffer())},
                              {vertexOffset});
        cmd.BindIndexBuffer(batch.mesh->GetIndexBuffer(), 0, vk::IndexType::eUint32);

        // 绘制：instanced。firstInstance 让 gl_InstanceIndex 从全局实例缓冲
        // 的起始索引开始，所有实例在单个 vkCmdDrawIndexedInstanced 中完成。
        // firstIndex 定位到子网格的索引范围起始，indexCount 为其索引数量。
        cmd.DrawIndexed(batch.indexCount, batch.instanceCount,
                        batch.firstIndex, 0, batch.firstInstance);
    }
}
void Renderer3D::DrawWaterBatches(VulkanCommandBuffer &cmd, VulkanRenderFrame &frame,
                                  const BufferAllocation &frameUbo,
                                  const BufferAllocation &lightBuffer,
                                  vk::Format colorFormat, vk::Format depthFormat,
                                  vk::Extent2D extent,
                                  const std::vector<VulkanImageView *> &readImages,
                                  bool hdrTransparent) {
    if (m_WaterBatches.empty()) {
        return;
    }

    // 水面走独立混合管线，但共享 FrameUBO / LightBuffer 与 IBL 反射贴图。
    ConfigureWaterPipeline(cmd, colorFormat, depthFormat, extent, hdrTransparent);
    BindSharedUniforms(cmd, frameUbo, lightBuffer);

    // IBL 预滤波 cubemap（反射）：有环境图且就绪时绑环境图，否则绑默认 1x1
    // 白色立方体 fallback，避免 descriptor set 出现未写入的 binding。
    const bool useIbl = (m_EnvironmentMap != nullptr) && m_EnvironmentMap->IsReady() && m_IBLEnabled;
    Texture *iblTex = useIbl ? &m_EnvironmentMap->GetPrefilter() : m_DefaultSkyboxTexture.get();
    if (iblTex) {
        cmd.BindImage(iblTex->GetImageView(), iblTex->GetSampler(), 1, 1);
    }

    for (const auto &batch : m_WaterBatches) {
        if (!batch.mesh || batch.indexCount == 0) {
            continue;
        }

        // 法线细节贴图（set 1 binding 0）：无绑定或未就绪时回退默认平坦法线。
        Texture *normalTex = (batch.normalMap && batch.normalMap->IsReady())
                                 ? batch.normalMap : m_DefaultNormalTexture.get();
        if (normalTex) {
            cmd.BindImage(normalTex->GetImageView(), normalTex->GetSampler(), 1, 0);
        }

        // 色彩/固有色贴图（set 1 binding 2）：无绑定或未就绪时回退默认 1x1 白色。
        const bool colorReady = batch.colorMap && batch.colorMap->IsReady();
        Texture *colorTex = colorReady ? batch.colorMap : m_DefaultWhiteTexture;
        if (colorTex) {
            cmd.BindImage(colorTex->GetImageView(), colorTex->GetSampler(), 1, 2);
        }
        // Stage 2 refraction inputs (HDR transparent path only):
        // readImageViews[0] = Scene_HDR copy, readImageViews[1] = SceneDepth copy.
        // Scene color uses a linear sampler; depth uses nearest to avoid interpolation.
        if (hdrTransparent && m_DefaultWhiteTexture) {
            if (readImages.size() >= 1 && readImages[0]) {
                cmd.BindImage(*readImages[0], m_DefaultWhiteTexture->GetSampler(), 1, 3);
            }
            VulkanSampler *sceneDepthSampler = m_ShadowSampler
                                                   ? m_ShadowSampler
                                                   : &m_DefaultWhiteTexture->GetSampler();
            if (readImages.size() >= 2 && readImages[1] && sceneDepthSampler) {
                cmd.BindImage(*readImages[1], *sceneDepthSampler, 1, 4);
            }
        }


        // 填充水面 UBO（std140）。waveSpeeds 只用到 x，其余补 0。
        WaterUBO ubo{};
        ubo.model = glm::scale(batch.transform, glm::vec3(batch.size.x, 1.0f, batch.size.y));
        ubo.timeParams = glm::vec4(m_WaterTime * batch.timeScale,
                                   batch.normalStrength,
                                   batch.roughness,
                                   batch.refractionStrength);
        ubo.deepColor = glm::vec4(batch.deepColor, batch.opacity); // a 通道承载整体不透明度
        ubo.shallowColor = glm::vec4(batch.shallowColor, 1.0f);
        ubo.sizeParams = glm::vec4(batch.size.x, batch.size.y,
                                   batch.normalTiling, batch.reflectionStrength);
        ubo.foamParams = glm::vec4(batch.foamDistance, batch.foamIntensity,
                                   batch.absorptionDepth, batch.timeScale);
        ubo.invProj = glm::inverse(m_Projection);  // for SceneDepth -> view-space reconstruction
        // 未绑/未就绪色彩贴图时强度强制 0 → mix 结果 = 纯深水色，老场景视觉不变。
        ubo.colorParams = glm::vec4(batch.colorTiling,
                                    colorReady ? batch.colorStrength : 0.0f,
                                    batch.alphaCoverage,
                                    batch.causticsEnabled ? batch.causticsIntensity : 0.0f);
        for (int i = 0; i < 4; ++i) {
            const auto &w = batch.waves[i];
            ubo.waves[i] = glm::vec4(w.direction.x, w.direction.y,
                                     w.amplitude, w.wavelength);
            ubo.waveSpeeds[i] = glm::vec4(w.speed, 0.0f, 0.0f, 0.0f);
        }

        BufferAllocation uboAlloc = frame.AllocateBuffer(
            vk::BufferUsageFlagBits::eUniformBuffer, sizeof(WaterUBO));
        uboAlloc.update(ubo);
        cmd.BindBuffer(uboAlloc.get_buffer(), uboAlloc.get_offset(),
                       uboAlloc.get_size(), 0, 2);

        // 单 water draw：水面面积大、实例少，不参与 instancing 合批。
        cmd.BindVertexBuffers(0,
                              {std::ref(batch.mesh->GetVertexBuffer())},
                              {vk::DeviceSize(0)});
        cmd.BindIndexBuffer(batch.mesh->GetIndexBuffer(), 0, vk::IndexType::eUint32);
        cmd.DrawIndexed(batch.indexCount, 1, batch.firstIndex, 0, 0);
    }
}
void Renderer3D::RecordStats(uint32_t drawCallCount) {
    // ── 6b. 统计 draw call 与三角形数量 ───────────────────────────────
    // draw call = 不透明批次数 + 透明批次数（RecordScene 把两段合起来统计，
    // 半透明逐实例成批已计入；延迟链由 PrepareDeferredBatches 切分后的两段之和）。
    // 三角形按实例累计：m_Meshes 线性包含全部实例，两段不重复计数。
    uint32_t triangles = 0;
    for (const auto &instance : m_Meshes) {
        triangles += instance.indexCount / 3;
    }
    // 水面批次（WaterBatch 独立于 m_Meshes，按实例累计）。
    for (const auto &water : m_WaterBatches) {
        triangles += water.indexCount / 3;
    }
    Renderer::Get().AddStats3D(drawCallCount, triangles);

    // 统计 instancing 批次数量（相同 mesh + 相同材质分一组），
    // 用于观察合批收益：批次数越少 → draw call 越少
    Renderer::Get().AddBatches3D(drawCallCount);
}

} // namespace GE
