/**
 * @file Renderer3D.cpp
 * @brief 3D 网格渲染器实现（场景采集 / 排序键分片）。
 *
 * 实现按职责拆分，便于维护：
 *  - Renderer3D_Lifecycle.cpp ：构造 / 析构 / 环境映射
 *  - Renderer3D.cpp           ：场景采集 / 排序键（本文件）
 *  - Renderer3D_Passes.cpp    ：GBuffer / Shadow / Lighting / Tonemap / Bloom / Transparent
 *  - Renderer3D_Pipelines.cpp ：各管线配置
 *  - Renderer3D_Uploads.cpp   ：GPU Buffer 上传
 *  - Renderer3D_Record.cpp    ：命令记录与网格/水面绘制
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
// ============================================================
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

    // 清空上一帧的水面列表，并刷新水面动画时间
    m_WaterBatches.clear();
    const auto now = std::chrono::steady_clock::now();
    static const auto s_Start = now;
    m_WaterTime = std::chrono::duration<float>(now - s_Start).count();

    // 清空上一帧的每级阴影专用网格列表（本帧由 Scene 逐级阴影遍历重新提交）
    for (auto &shadowMeshes : m_ShadowMeshes) {
        shadowMeshes.clear();
    }

    // 清空上一帧的皮肤关节矩阵注册表（本帧由 Scene 重新注册）
    m_SkinJointBuffers.clear();
}
void Renderer3D::ResetSkinJointBuffers() {
    m_SkinJointBuffers.clear();
}
void Renderer3D::SetSkinJointBuffer(const void *skinKey,
                                    const BufferAllocation &jointBuffer) {
    m_SkinJointBuffers[skinKey] = jointBuffer;
}
void Renderer3D::DrawMesh(const glm::mat4 &transform,
                          Mesh *mesh,
                          Material *material,
                          const glm::vec4 &color) {
    if (!mesh) {
        return;
    }
    // 整网格绘制 = 单个覆盖全部索引的子网格
    DrawSubMeshImpl(transform, mesh,
                    0, mesh->GetIndexCount(), material, color);
}
void Renderer3D::DrawSubMesh(const glm::mat4 &transform,
                             Mesh *mesh,
                             const SubMesh &submesh,
                             Material *material,
                             const glm::vec4 &color) {
    DrawSubMeshImpl(transform, mesh,
                    submesh.firstIndex, submesh.indexCount, material, color);
}
void Renderer3D::DrawSkinnedSubMesh(const glm::mat4 &transform,
                                    Mesh *mesh,
                                    const SubMesh &submesh,
                                    Material *material,
                                    const glm::vec4 &color,
                                    const void *skinKey) {
    DrawSubMeshImpl(transform, mesh,
                    submesh.firstIndex, submesh.indexCount, material, color, skinKey);
}
void Renderer3D::DrawWater(const glm::mat4 &transform,
                           Mesh *mesh,
                           const WaterComponent &water) {
    GE_CORE_ASSERT(m_InScene, "DrawWater called outside BeginScene/EndScene!");

    if (!mesh || !mesh->IsReady() || mesh->GetIndexCount() == 0) {
        return;
    }

    // 拷贝组件参数进渲染器内部批次，渲染器不持有实体引用（改进可追踪性）。
    WaterBatch batch;
    batch.transform = transform;
    batch.mesh = mesh;
    batch.size = water.Size;
    batch.firstIndex = 0;
    batch.indexCount = mesh->GetIndexCount();
    batch.timeScale = water.TimeScale;
    batch.deepColor = water.DeepColor;
    batch.shallowColor = water.ShallowColor;
    batch.normalMap = water.NormalMap;
    batch.normalTiling = water.NormalTiling;
    batch.normalStrength = water.NormalStrength;
    batch.colorMap = water.ColorMap;
    batch.colorTiling = water.ColorTiling;
    batch.colorStrength = water.ColorStrength;
    batch.roughness = water.Roughness;
    batch.opacity = water.Opacity;
    batch.alphaCoverage = water.AlphaCoverage;
    batch.reflectionStrength = water.ReflectionStrength;
    batch.refractionStrength = water.RefractionStrength;
    batch.absorptionDepth = water.AbsorptionDepth;
    batch.foamDistance = water.FoamDistance;
    batch.foamIntensity = water.FoamIntensity;
    for (int i = 0; i < 4; ++i) {
        const auto &w = water.Waves[i];
        batch.waves[i] = {glm::normalize(w.Direction), w.Amplitude, w.Wavelength, w.Speed};
    }

    m_WaterBatches.push_back(std::move(batch));
}
void Renderer3D::DrawShadowSubMesh(const glm::mat4 &transform,
                                   Mesh *mesh,
                                   const SubMesh &submesh,
                                   Material *material,
                                   const glm::vec4 &color,
                                   uint32_t cascade) {
    DrawSubMeshImpl(transform, mesh,
                    submesh.firstIndex, submesh.indexCount, material, color,
                    /*skinKey=*/nullptr, /*forShadow=*/true, cascade);
}
void Renderer3D::DrawShadowSkinnedSubMesh(const glm::mat4 &transform,
                                          Mesh *mesh,
                                          const SubMesh &submesh,
                                          Material *material,
                                          const glm::vec4 &color,
                                          const void *skinKey,
                                          uint32_t cascade) {
    DrawSubMeshImpl(transform, mesh,
                    submesh.firstIndex, submesh.indexCount, material, color, skinKey,
                    /*forShadow=*/true, cascade);
}
void Renderer3D::DrawSubMeshImpl(const glm::mat4 &transform,
                                 Mesh *mesh,
                                 uint32_t firstIndex,
                                 uint32_t indexCount,
                                 Material *material,
                                 const glm::vec4 &color,
                                 const void *skinKey,
                                 bool forShadow,
                                 uint32_t cascade) {
    GE_CORE_ASSERT(m_InScene, "DrawMesh called outside BeginScene/EndScene!");

    if (!mesh || indexCount == 0) {
        return;
    }

    // 异步加载中的网格（空壳，缓冲未安装）直接跳过本批，就绪后下帧自动亮相
    if (!mesh->IsReady()) {
        return;
    }

    // 计算排序键（pipeline → 材质 → mesh → 子网格 → view 空间深度），用于 EndScene
    // 前分组排序，使同材质同 mesh 同子网格的实例连续，便于 instancing 合批。
    // 蒙皮实例的 pipelineId 带蒙皮位，与静态实例分组隔离（管线不同不可合批）。
    // 阴影集合复用同一排序键：阴影 pass 为深度只写无混合，实例序不影响结果，
    // 只保证同 mesh 同材质连续便于合批。
    const bool skinned = (skinKey != nullptr);
    SortKey sortKey = ComputeSortKey(material, mesh, firstIndex, indexCount, transform, skinned);

    const MeshInstance instance{
        transform, mesh, firstIndex, indexCount, material, color, sortKey, skinKey};
    if (forShadow) {
        // 命中入该级阴影集合（cascade 由 Scene 逐级遍历提交，CSM 计划书 §4.3）
        m_ShadowMeshes[cascade].push_back(instance);
    } else {
        m_Meshes.push_back(instance);
    }
}

// ============================================================================
// 内部工具方法（排序键相关）
// ============================================================================
Texture *Renderer3D::GetEffectiveTexture(const Material *material) const {
    // 优先取材质 Albedo 槽位纹理，无材质、无纹理或未就绪（异步加载中）时
    // 回退到默认白色纹理，避免绑定空句柄
    Texture *tex = (material ? material->GetTexture(Material::Albedo) : nullptr);
    return (tex && tex->IsReady()) ? tex : m_DefaultWhiteTexture;
}
Texture *Renderer3D::GetEffectiveNormalTexture(const Material *material) const {
    // 优先取材质 Normal 槽位纹理，无材质、无纹理或未就绪（异步加载中）时
    // 回退到默认平坦法线纹理
    Texture *tex = (material ? material->GetTexture(Material::Normal) : nullptr);
    return (tex && tex->IsReady()) ? tex : m_DefaultNormalTexture.get();
}
Texture *Renderer3D::GetEffectiveEmissiveTexture(const Material *material) const {
    // 优先取材质 Emissive 槽位纹理，无材质、无纹理或未就绪（异步加载中）时
    // 回退到默认白色纹理（RGB=(1,1,1)，自发光 = emissiveFactor 颜色）
    Texture *tex = (material ? material->GetTexture(Material::Emissive) : nullptr);
    return (tex && tex->IsReady()) ? tex : m_DefaultEmissiveTexture.get();
}
Texture *Renderer3D::GetEffectiveMetallicRoughnessTexture(const Material *material) const {
    // 优先取材质 MetallicRoughness 槽位纹理，无材质、无纹理或未就绪（异步
    // 加载中）时回退到默认 (G=1,B=1) 纹理，使 metallic/roughness 等于标量系数原值
    Texture *tex = (material ? material->GetTexture(Material::MetallicRoughness) : nullptr);
    return (tex && tex->IsReady()) ? tex : m_DefaultMetallicRoughnessTexture.get();
}
uint8_t Renderer3D::GetPipelineId(const Material *material) const {
    // PBR 材质路由到 PBR 管线（id=1），其余（BlinnPhong / nullptr）走默认管线（0）
    if (material && material->GetType() == Material::Type::PBR) {
        return detail::kPbrPipelineId;
    }
    return detail::kBlinnPipelineId;
}
Renderer3D::SortKey Renderer3D::ComputeSortKey(const Material *material, const Mesh *mesh,
                                               uint32_t firstIndex, uint32_t indexCount,
                                               const glm::mat4 &transform, bool skinned) const {
    SortKey key;

    // 渲染段分区：Blend 走透明段（不透明先画，透明独立 back-to-front 排序）；
    // Opaque/Mask 均为不透明段。passId 作为排序最高语义分区，不透明深度方向不
    // 受透明干扰。
    const bool transparent = (material && material->alphaMode == Material::AlphaMode::Blend);
    key.passId = static_cast<uint8_t>(transparent ? Pass::Transparent : Pass::Opaque);

    // pipeline 位在段内次高：段内同材质 layout 的批次（含蒙皮位 2/3）连续，
    // 减少管线切换。pass 必须优先于 pipeline——否则蒙皮（pipe2/3）会排到静态
    // 透明（pipe0/透明）之后，切分点之后整段被透明管线绘制（见 SortKey 注释）。
    key.pipelineId = detail::ComposePipelineId(GetPipelineId(material), skinned);

    // 材质分组：用材质指针值（进程内唯一）作分组 id，使同材质实例连续，
    // 减少管线/纹理切换。材质完整决定渲染状态（纹理组合、着色器类型、混合等）。
    // nullptr 材质统一视为 0，使其彼此相邻。
    if (material) {
        key.materialId = reinterpret_cast<uintptr_t>(material);
    }

    // mesh 分组：用 mesh 指针值作分组 id，使同材质内同 mesh 实例连续便于合批。
    // 仅用于排序，合批分组用指针相等判断。
    key.meshId = reinterpret_cast<uintptr_t>(mesh);

    // 子网格分组：同一 mesh 的不同子网格（索引范围）必须分开，否则同材质
    // 的同 mesh 子网格会被错误合批。打包 firstIndex 与 indexCount 为 64 位。
    key.submeshId = (static_cast<uint64_t>(firstIndex) << 32)
                    | static_cast<uint64_t>(indexCount);

    // 深度：取模型变换的平移分量转换到 view 空间，取反得到正值（越大越远）。
    // 正浮点数的 IEEE 位模式随值单调递增，故可直接按位作为排序键，
    // 升序排列即实现不透明物体从前往后（early-z 优化）。
    glm::vec4 viewPos = m_View * transform[3];
    uint32_t dist = std::bit_cast<uint32_t>(-viewPos.z);
    // 透明分区 depthBits 按位取反：位模式单调性反向，分区内升序即从远到近
    // （alpha 混合正确序），与不透明段的近→远方向解耦。
    key.depthBits = transparent ? ~dist : dist;

    return key;
}
void Renderer3D::EndScene() {
    GE_PROFILE_SCOPE("Renderer3D::EndScene");

    GE_CORE_ASSERT(m_InScene, "EndScene called without BeginScene!");
    m_InScene = false;

    // 恒为"采集器"形态：只结束采集，批次保留在 m_Meshes。命令录制延后到本帧
    // Scene3D pass 的 execute 回调里调用 FlushScene 完成（动态渲染已由图打开）。
}
} // namespace GE
