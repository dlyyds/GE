/**
 * @file Renderer3D_Uploads.cpp
 * @brief Renderer3D GPU Buffer 上传分片（Frame / Instance / Light / UBO）。
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
// ==================== GPU Buffer 上传 ====================
BufferAllocation Renderer3D::UploadShadowFrameUBO(VulkanRenderFrame &frame,
                                                  uint32_t cascade) {
    // 某级阴影 pass 专用 FrameUBO：projection = 单位阵、view = 该级光空间 view-proj。
    // 顶点着色器算 gl_Position = projection * view * worldPos = cascadeViewProj * worldPos，
    // 把几何从相机视角切到该级光源视角（复用 mesh.vert/mesh_skinned.vert，无需改动）。
    FrameUBO ubo{};
    ubo.projection = glm::mat4(1.0f);
    ubo.view = m_LightParams.cascadeViewProj[cascade];
    BufferAllocation alloc = frame.AllocateBuffer(
        vk::BufferUsageFlagBits::eUniformBuffer, sizeof(FrameUBO));
    alloc.update(ubo);
    return alloc;
}
BufferAllocation Renderer3D::UploadFrameUBO(VulkanRenderFrame &frame) {
    // ── 1. 分配 Frame UBO（所有网格共享） ─────────────────────────────
    FrameUBO frameUBO{};
    frameUBO.projection = m_Projection;
    frameUBO.view = m_View;
    frameUBO.viewPos = glm::vec4(m_ViewPos, 0.0f);
    frameUBO.dirLightDirection = glm::vec4(m_LightParams.dirLightDirection, 0.0f);
    frameUBO.dirLightColor = m_LightParams.dirLightColor;

    // 点光源数量（无编译期上限，本体在 SSBO 中按实际数量上传）
    frameUBO.lightCount = glm::vec4(
        static_cast<float>(m_LightParams.pointLights.size()), 0.0f, 0.0f, 0.0f);

    frameUBO.ambient = m_LightParams.ambient;

    // IBL 参数：x = 预滤波图最高 mip 索引（MAX_REFLECTION_LOD，= levelCount-1），
    // y = IBL 环境光强度（整体缩放 IBL 贡献）。无 IBL 时填 0，着色器 HAS_IBL
    // 变体不采样该值（走常量环境光分支）。
    frameUBO.iblParams = glm::vec4(
        m_EnvironmentMap ? static_cast<float>(m_EnvironmentMap->GetPrefilterLevels() - 1) : 0.0f,
        m_IBLIntensity, 0.0f, 0.0f);

    BufferAllocation frameUboAlloc = frame.AllocateBuffer(
        vk::BufferUsageFlagBits::eUniformBuffer, sizeof(FrameUBO));
    frameUboAlloc.update(frameUBO);

    return frameUboAlloc;
}
BufferAllocation Renderer3D::UploadInstanceBuffer(VulkanRenderFrame &frame,
                                                  const std::vector<InstanceData> &instances) {
    // 分配全局实例 SSBO 并一次性上传（所有批次共享）。无网格时分配 1 字节占位
    // 避免空缓冲（批次为空则不会被绑定，shader 不受影响）。
    BufferAllocation alloc = frame.AllocateBuffer(
        vk::BufferUsageFlagBits::eStorageBuffer,
        instances.empty() ? 1 : instances.size() * sizeof(InstanceData));
    alloc.update(instances);
    return alloc;
}
BufferAllocation Renderer3D::UploadLightBuffer(VulkanRenderFrame &frame) {
    // 点光源 SSBO（set 0, binding 1）——无上界动态数组，解除编译期数量上限。
    // 从 LightParams 收集全部点光源，转为 GPU 布局（2 个 vec4）一次性上传。
    // 无光源时分配 1 字节占位避免空缓冲；shader 循环 0 次不受影响。
    std::vector<LightGPU> lights;
    lights.reserve(m_LightParams.pointLights.size());
    for (const auto &pl : m_LightParams.pointLights) {
        lights.push_back(LightGPU{glm::vec4(pl.position, pl.radiusInv), pl.color});
    }
    BufferAllocation alloc = frame.AllocateBuffer(
        vk::BufferUsageFlagBits::eStorageBuffer,
        lights.empty() ? 1 : lights.size() * sizeof(LightGPU));
    if (!lights.empty()) {
        alloc.update(lights);
    }
    return alloc;
}
BufferAllocation Renderer3D::UploadLightingUBO(VulkanRenderFrame &frame) {
    // 延迟 Lighting UBO 复用前向光照字段，并额外携带反投影矩阵与背景色。
    // flags.x 由 CPU 端根据「天空盒开关 + 环境图就绪」生成，shader 无天空时
    // 直接输出 clearColor，与现前向路径的 DrawSkybox 门控一致。
    LightingUBO ubo{};
    ubo.invView = glm::inverse(glm::mat4(glm::mat3(m_View)));
    ubo.view = m_View;
    ubo.invProj = glm::inverse(m_Projection);
    ubo.clearColor = m_ClearColor;

    const Texture *skyTex = (m_EnvironmentMap && m_EnvironmentMap->IsReady())
                                ? &m_EnvironmentMap->GetSkybox()
                                : ((m_DefaultSkyboxTexture && m_DefaultSkyboxTexture->IsReady())
                                       ? m_DefaultSkyboxTexture.get()
                                       : nullptr);
    ubo.flags.x = (m_SkyboxEnabled && skyTex) ? 1.0f : 0.0f;

    // IBL 是否可用：环境图三张就绪且 IBL 开关打开。就绪时 flags.y 置位，
    // shader 走 split-sum IBL；否则回退常量环境光（与 DrawMeshInstances 的
    // useIbl 判定一致）。iblParams.x 携带预滤波最大 mip 数（MAX_REFLECTION_LOD）。
    const bool iblReady = (m_EnvironmentMap != nullptr) && m_EnvironmentMap->IsReady()
                          && m_IBLEnabled;
    ubo.flags.y = iblReady ? 1.0f : 0.0f;
    ubo.iblParams = glm::vec4(
        m_EnvironmentMap ? static_cast<float>(m_EnvironmentMap->GetPrefilterLevels() - 1)
                         : 0.0f,
        m_IBLIntensity, 0.0f, 0.0f);

    ubo.viewPos = glm::vec4(m_ViewPos, 0.0f);
    ubo.dirLightDirection = glm::vec4(m_LightParams.dirLightDirection, 0.0f);
    ubo.dirLightColor = m_LightParams.dirLightColor;
    ubo.lightCount = glm::vec4(
        static_cast<float>(m_LightParams.pointLights.size()), 0.0f, 0.0f, 0.0f);
    ubo.ambient = m_LightParams.ambient;

    // 方向光阴影（CSM C3）：每级光矩阵 + 切分距离 + 生效级数打包，shader 选片后按级
    // 采样对应深度图。shadowParams 各级共享（阶段 1：texel 尺寸/偏差/PCF 半径均用级 0
    // 值，每级独立偏差与 PCF 半径列 CSM 计划书 §7）。
    for (uint32_t c = 0; c < kMaxCascades; ++c) {
        ubo.cascadeViewProj[c] = m_LightParams.cascadeViewProj[c];
    }
    ubo.cascadeSplits = glm::vec4(
        m_LightParams.cascadeSplits[0], m_LightParams.cascadeSplits[1],
        m_LightParams.cascadeSplits[2], m_LightParams.cascadeSplits[3]);
    ubo.cascadeParams = glm::vec4(
        static_cast<float>(m_LightParams.cascadeCount), 0.0f, 0.0f, 0.0f);
    ubo.shadowParams = glm::vec4(
        static_cast<float>(m_ShadowMapSize),
        m_ShadowBias,
        m_LightParams.castShadow ? 1.0f : 0.0f,
        m_ShadowPcfRadius);

    BufferAllocation alloc = frame.AllocateBuffer(
        vk::BufferUsageFlagBits::eUniformBuffer, sizeof(LightingUBO));
    alloc.update(ubo);
    return alloc;
}
BufferAllocation Renderer3D::UploadTonemapUBO(VulkanRenderFrame &frame) {
    // Tonemap UBO：曝光来自场景/编辑器相机（m_Exposure，默认 1.0）。
    // flags.x = tonemap 开关（编辑器可关，关闭时透传线性 HDR），flags.y = 天空
    // alpha 旗标开关（保持天空不 tonemap 语义）。
    TonemapUBO ubo{};
    ubo.exposure.x = m_Exposure;
    ubo.flags.x = m_TonemapEnabled ? 1.0f : 0.0f;
    ubo.flags.y = 1.0f;

    BufferAllocation alloc = frame.AllocateBuffer(
        vk::BufferUsageFlagBits::eUniformBuffer, sizeof(TonemapUBO));
    alloc.update(ubo);
    return alloc;
}
BufferAllocation Renderer3D::UploadBloomUBO(VulkanRenderFrame &frame, vk::Extent2D extent) {
    // Bloom UBO：阈值 / 强度 / 开关 与当前 pass 的 texel 尺寸（由执行上下文 renderArea 传入）。
    BloomUBO ubo{};
    ubo.params.x = m_BloomThreshold;
    ubo.params.y = m_BloomIntensity;
    ubo.params.z = m_BloomEnabled ? 1.0f : 0.0f;
    ubo.texelSize.x = 1.0f / static_cast<float>(std::max(1u, extent.width));
    ubo.texelSize.y = 1.0f / static_cast<float>(std::max(1u, extent.height));

    BufferAllocation alloc = frame.AllocateBuffer(
        vk::BufferUsageFlagBits::eUniformBuffer, sizeof(BloomUBO));
    alloc.update(ubo);
    return alloc;
}
BufferAllocation Renderer3D::UploadUnderwaterUBO(VulkanRenderFrame &frame) {
    // UnderwaterFX：淹没量 / 雾密度（吸收深度的倒数）/ 雾色 / 预留焦散字段。
    // 阶段 0 只消费 submersion + fogDensity + deepColor，其余字段为后续阶段预留。
    UnderwaterUBO ubo{};
    ubo.invProj = glm::inverse(m_Projection);
    ubo.params = glm::vec4(
        m_WaterSubmersion,
        1.0f / std::max(m_WaterAbsorption * 3.0f, 0.25f),
        0.25f,   // desaturate (lightened: keep more scene color)
        0.15f);  // vignette (lightened)
    ubo.deepColor = glm::vec4(m_WaterDeepColor, 1.0f);
    ubo.caustics = glm::vec4(0.0f);
    ubo.sunDir = glm::vec4(m_LightParams.dirLightDirection, 0.0f);
    ubo.waterPlane = glm::vec4(m_WaterPlaneY, 0.0f, 0.0f, 0.0f);
    ubo.timeParams = glm::vec4(m_WaterTime, 0.0f, 0.0f, 0.0f);
    ubo.viewPos = glm::vec4(m_ViewPos, 1.0f);

    BufferAllocation alloc = frame.AllocateBuffer(
        vk::BufferUsageFlagBits::eUniformBuffer, sizeof(UnderwaterUBO));
    alloc.update(ubo);
    return alloc;
}
} // namespace GE
