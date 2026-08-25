/**
 * @file Renderer3D.cpp
 * @brief 3D 网格渲染器实现。
 */

#include "Render/Renderer3D.h"

#include <algorithm>
#include <bit>
#include <filesystem>

#include <glm/gtc/matrix_inverse.hpp> // glm::inverse（矩阵求逆）

#include "Core/Log.h"
#include "Render/Texture.h"
#include "Render/Renderer.h"
#include "Render/AssetManager.h"
#include "Render/TextureManager.h"
#include "Render/VulkanBase/VulkanCommandBuffer.h"
#include "Render/VulkanBase/VulkanPipelineLayout.h"
#include "Render/VulkanBase/VulkanRenderContext.h"
#include "Render/VulkanBase/VulkanRenderFrame.h"
#include "Render/VulkanBase/VulkanRenderingInfo.h"
#include "Render/VulkanBase/VulkanResourceCache.h"
#include "Render/VulkanBase/VulkanShaderModule.h"

#include "Debug/Profiler.h"

namespace GE {

namespace {
/// pipelineId 蒙皮位（阶段 C）：低位 0 = 材质（0 Blinn / 1 PBR），
/// 叠加本高位（值 2）后为蒙皮肤管线（2 Blinn 蒙皮 / 3 PBR 蒙皮）。
constexpr uint8_t kSkinPipelineBit = 0x02;
} // namespace

// ============================================================================
// 构造 / 析构
// ============================================================================

Renderer3D::Renderer3D() {
    GE_PROFILE_SCOPE("Renderer3D::Init");

    auto &device = Renderer::GetVulkanContext().GetDevice();
    auto &cache = device.GetResourceCache();

    // ── 1. 通过全局资源缓存请求网格着色器 ──────────────────────────────
    m_VertShader = &cache.RequestShaderModule(
        vk::ShaderStageFlagBits::eVertex,
        ShaderSource(Renderer::GetAssetManager()
                         .ResolvePath(std::string(AssetPaths::Shaders) + "/mesh.vert.spv")
                         .string()),
        "main", ShaderVariant{});

    m_FragShader = &cache.RequestShaderModule(
        vk::ShaderStageFlagBits::eFragment,
        ShaderSource(Renderer::GetAssetManager()
                         .ResolvePath(std::string(AssetPaths::Shaders) + "/mesh.frag.spv")
                         .string()),
        "main", ShaderVariant{});

    // PBR 片元着色器（Cook-Torrance）。与 Blinn-Phong 并行，由材质类型路由。
    m_FragShaderPBR = &cache.RequestShaderModule(
        vk::ShaderStageFlagBits::eFragment,
        ShaderSource(Renderer::GetAssetManager()
                         .ResolvePath(std::string(AssetPaths::Shaders) + "/mesh_pbr.frag.spv")
                         .string()),
        "main", ShaderVariant{});

    // ── 3. 请求 PipelineLayout（通过反射自动构建） ─────────────────────
    //    注：阶段2 曾用 Dynamic ObjectUBO 存 per-instance 数据以降低
    //    descriptor set 数量；阶段3 已把 model/color 迁入 InstanceData SSBO，
    //    ObjectUBO 随之整个移除，set 2 仅保留 InstanceData SSBO。
    //    两套管线共享顶点着色器，仅片元着色器不同（Blinn-Phong / PBR），
    //    描述符布局仅 set 1 因 PBR 多一个 MR 纹理 binding 而不同。
    m_PipelineLayout = &cache.RequestPipelineLayout(
        {m_VertShader, m_FragShader});
    m_PipelineLayout->SetDebugName("Mesh3D_PipelineLayout");

    m_PipelineLayoutPBR = &cache.RequestPipelineLayout(
        {m_VertShader, m_FragShaderPBR});
    m_PipelineLayoutPBR->SetDebugName("Mesh3D_PipelineLayout_PBR");

    // PBR-IBL 片元着色器变体（HAS_IBL：Filament 式 split-sum 环境光）。
    // 其管线布局 set 1 额外含 binding 5/6/7（辐照度 / 预滤波 / BRDF LUT）。
    m_FragShaderPBR_IBL = &cache.RequestShaderModule(
        vk::ShaderStageFlagBits::eFragment,
        ShaderSource(Renderer::GetAssetManager()
                         .ResolvePath(std::string(AssetPaths::Shaders) + "/mesh_pbr_ibl.frag.spv")
                         .string()),
        "main", ShaderVariant{});

    m_PipelineLayoutPBR_IBL = &cache.RequestPipelineLayout(
        {m_VertShader, m_FragShaderPBR_IBL});
    m_PipelineLayoutPBR_IBL->SetDebugName("Mesh3D_PipelineLayout_PBR_IBL");

    // ── 3a. 蒙皮肤管线（阶段 C）：mesh_skinned.vert + 三种片元 ─────────
    //    蒙皮顶点着色器声明 location 4/5（关节索引/权重）与 set2 binding1
    //    （关节矩阵 SSBO），反射自动生成 80B 顶点输入与扩展描述符布局。
    //    pipelineId 蒙皮位（2/3）路由到这里，静态路径不受影响。
    m_VertShaderSkinned = &cache.RequestShaderModule(
        vk::ShaderStageFlagBits::eVertex,
        ShaderSource(Renderer::GetAssetManager()
                         .ResolvePath(std::string(AssetPaths::Shaders) + "/mesh_skinned.vert.spv")
                         .string()),
        "main", ShaderVariant{});

    m_PipelineLayoutSkinned = &cache.RequestPipelineLayout(
        {m_VertShaderSkinned, m_FragShader});
    m_PipelineLayoutSkinned->SetDebugName("Mesh3D_PipelineLayout_Skinned");

    m_PipelineLayoutSkinnedPBR = &cache.RequestPipelineLayout(
        {m_VertShaderSkinned, m_FragShaderPBR});
    m_PipelineLayoutSkinnedPBR->SetDebugName("Mesh3D_PipelineLayout_Skinned_PBR");

    m_PipelineLayoutSkinnedPBR_IBL = &cache.RequestPipelineLayout(
        {m_VertShaderSkinned, m_FragShaderPBR_IBL});
    m_PipelineLayoutSkinnedPBR_IBL->SetDebugName("Mesh3D_PipelineLayout_Skinned_PBR_IBL");

    // ── 2b. 天空盒着色器 + 管线布局 ─────────────────────────────────
    //    等距柱状投影天空盒：全屏三角形 + 反投影重建视线 + 采样全景图。
    //    管线布局由着色器反射自动构建（set 0 binding 0 = SkyboxUBO，binding 1 = sampler2D）。
    m_SkyboxVert = &cache.RequestShaderModule(
        vk::ShaderStageFlagBits::eVertex,
        ShaderSource(Renderer::GetAssetManager()
                         .ResolvePath(std::string(AssetPaths::Shaders) + "/skybox.vert.spv")
                         .string()),
        "main", ShaderVariant{});

    m_SkyboxFrag = &cache.RequestShaderModule(
        vk::ShaderStageFlagBits::eFragment,
        ShaderSource(Renderer::GetAssetManager()
                         .ResolvePath(std::string(AssetPaths::Shaders) + "/skybox.frag.spv")
                         .string()),
        "main", ShaderVariant{});

    m_SkyboxLayout = &cache.RequestPipelineLayout({m_SkyboxVert, m_SkyboxFrag});
    m_SkyboxLayout->SetDebugName("Skybox_PipelineLayout");

    // ── 3. 默认 1x1 白色纹理（无纹理时的 fallback），从全局纹理管理器获取 ──
    //    纹理由 TextureManager 去重缓存并持有，这里仅保存非拥有指针。
    m_DefaultWhiteTexture =
        Renderer::GetTextureManager().GetSolidColor(
            glm::vec4(1.0f), vk::Format::eR8G8B8A8Unorm,
            vk::Filter::eLinear, vk::Filter::eLinear);
    if (!m_DefaultWhiteTexture) {
        GE_CORE_ERROR("Renderer3D: 获取默认白色纹理失败！");
    }

    // ── 4. 创建默认 1x1 "平坦法线"纹理（无法线贴图时的 fallback） ──────
    //    RGB = (128, 128, 255)：采样后映射回 (0,0,1)，即几何法线本身，
    //    使未绑定法线贴图的材质表现得如同未使用法线贴图。
    uint32_t flatNormalPixel = 0xFFFF8080; // RGBA8: (128, 128, 255, 255)
    m_DefaultNormalTexture = Texture::LoadFromMemory(
        device, cache, &flatNormalPixel, 1, 1,
        vk::Format::eR8G8B8A8Unorm,
        vk::Filter::eLinear, vk::Filter::eLinear);
    if (m_DefaultNormalTexture) {
        m_DefaultNormalTexture->SetDebugName("DefaultNormalTexture");
    } else {
        GE_CORE_ERROR("Renderer3D: 创建默认平坦法线纹理失败！");
    }

    // ── 5. 创建默认 1x1 白色纹理（无自发光贴图时的 fallback） ──────────
    //    RGB = (1, 1, 1)：采样后乘 emissiveFactor 等于因子本身，即无贴图时
    //    用 emissiveFactor 颜色直接发光（glTF 语义）；有贴图时贴图颜色 × 因子，
    //    黑色区域仍不发光。
    // 像素字面量按 0xAABBGGRR 小端约定书写：0xFFFFFFFF = (255, 255, 255, 255) 白色不透明。
    uint32_t whitePixel = 0xFFFFFFFF; // RGBA8: (255, 255, 255, 255)
    m_DefaultEmissiveTexture = Texture::LoadFromMemory(
        device, cache, &whitePixel, 1, 1,
        vk::Format::eR8G8B8A8Unorm,
        vk::Filter::eLinear, vk::Filter::eLinear);
    if (m_DefaultEmissiveTexture) {
        m_DefaultEmissiveTexture->SetDebugName("DefaultEmissiveTexture");
    } else {
        GE_CORE_ERROR("Renderer3D: 创建默认白色自发光纹理失败！");
    }

    // ── 6. 创建默认 1x1 金属-粗糙度纹理（无 MR 贴图时的 fallback） ──────
    //    G = 1, B = 1：shader 里 metallic = mr.b * 系数、roughness = mr.g * 系数，
    //    回退时两者都乘以 1，即等于标量 pbr 系数原值，使无 MR 贴图的材质
    //    表现得如同只用标量 metallic/roughness。
    //    R 通道 shader 不读，任意值均可。像素字面量按 0xAABBGGRR 小端约定：
    //    0xFFFFFF00 = (R=0, G=255, B=255, A=255)。
    uint32_t mrPixel = 0xFFFFFF00; // RGBA8: (0, 255, 255, 255)
    m_DefaultMetallicRoughnessTexture = Texture::LoadFromMemory(
        device, cache, &mrPixel, 1, 1,
        vk::Format::eR8G8B8A8Unorm,
        vk::Filter::eLinear, vk::Filter::eLinear);
    if (m_DefaultMetallicRoughnessTexture) {
        m_DefaultMetallicRoughnessTexture->SetDebugName("DefaultMetallicRoughnessTexture");
    } else {
        GE_CORE_ERROR("Renderer3D: 创建默认金属-粗糙度纹理失败！");
    }

    GE_CORE_INFO("Renderer3D initialized");
}

// ============================================================================
// 天空盒
// ============================================================================
// 天空盒纹理由环境图 EnvironmentMap 统一持有，渲染器仅维护开关 m_SkyboxEnabled。

// ============================================================================
// 环境
// ============================================================================

void Renderer3D::SetEnvironment(const std::string &name) {
    // 环境名未变则跳过（避免每帧重建天空盒 + 预滤波 + BRDF LUT 三张图）
    if (name == m_EnvironmentName)
        return;

    auto &device = Renderer::GetVulkanContext().GetDevice();
    auto &cache  = device.GetResourceCache();
    auto &am     = Renderer::GetAssetManager();
    auto &upload = Renderer::GetAsyncUploadManager();

    // 按命名约定推导三张图路径（与 assets/environments/ 布局一致）
    const std::string envDir = "environments/" + name + "/";
    // 异步加载：立即返回空壳，三张图后台解码 + GPU 上传，就绪前天空盒 / IBL
    // 不显示，就绪后下帧自动切换
    auto env = EnvironmentMap::LoadFromFilesAsync(
        device, cache, upload,
        am.ResolvePath(envDir + "skybox.ktx2").string(),
        am.ResolvePath(envDir + "prefilter.ktx").string(),
        am.ResolvePath("environments/brdf_lut.png").string());
    if (!env) {
        GE_CORE_ERROR("Renderer3D: 环境异步加载提交失败: {0}", name);
        m_EnvironmentName.clear();   // 允许下次重试
        return;
    }

    SetEnvironmentMap(env.release());
    m_EnvironmentName = name;
}

void Renderer3D::SetEnvironmentMap(EnvironmentMap *env) {
    // 旧环境不立即销毁：其 ImageView 可能仍被上一帧 descriptor set 引用，
    // 立即销毁会触发 VUID-vkDestroyImageView-imageView-01026。退休到列表，
    // 由 FlushRetiredEnvironments 在下一帧安全点内存放。
    if (m_EnvironmentMap) {
        m_RetiredEnvironments.push_back(std::move(m_EnvironmentMap));
    }
    m_EnvironmentMap.reset(env);
}

void Renderer3D::FlushRetiredEnvironments() {
    if (m_RetiredEnvironments.empty()) {
        return;
    }
    // 安全点条件：
    // 1. 当前帧描述符池已在上方 BeginFrame 重置 —— 上一帧引用旧环境 ImageView
    //    的 descriptor set 已消失，ImageView 不再被任何 descriptor set 引用；
    // 2. WaitIdle 确保 GPU 完成所有引用旧环境的已提交命令。
    // 满足二者后销毁旧环境（含其纹理 ImageView）才是合法的。
    Renderer::Get().WaitIdle();
    m_RetiredEnvironments.clear();
    GE_CORE_INFO("Renderer3D: 已销毁退休环境映射");
}

Renderer3D::~Renderer3D() {
    GE_CORE_INFO("Renderer3D Shutdown");

    // 释放默认纹理
    // 注：m_DefaultWhiteTexture 由全局 TextureManager 持有，不属于本渲染器，无需释放
    m_DefaultNormalTexture.reset();
    m_DefaultEmissiveTexture.reset();
    m_DefaultMetallicRoughnessTexture.reset();

    // 释放环境映射（IBL）资源（含天空盒纹理）
    m_EnvironmentMap.reset();

    // 着色器和 pipeline layout 由全局资源缓存管理，不需要手动释放
    m_VertShader = nullptr;
    m_FragShader = nullptr;
    m_FragShaderPBR = nullptr;
    m_FragShaderPBR_IBL = nullptr;
    m_PipelineLayout = nullptr;
    m_PipelineLayoutPBR = nullptr;
    m_PipelineLayoutPBR_IBL = nullptr;
    m_SkyboxVert = nullptr;
    m_SkyboxFrag = nullptr;
    m_SkyboxLayout = nullptr;
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

void Renderer3D::DrawSubMeshImpl(const glm::mat4 &transform,
                                 Mesh *mesh,
                                 uint32_t firstIndex,
                                 uint32_t indexCount,
                                 Material *material,
                                 const glm::vec4 &color,
                                 const void *skinKey) {
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
    const bool skinned = (skinKey != nullptr);
    SortKey sortKey = ComputeSortKey(material, mesh, firstIndex, indexCount, transform, skinned);

    m_Meshes.push_back({transform, mesh, firstIndex, indexCount, material, color, sortKey, skinKey});
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
        return 1;
    }
    return 0;
}

Renderer3D::SortKey Renderer3D::ComputeSortKey(const Material *material, const Mesh *mesh,
                                               uint32_t firstIndex, uint32_t indexCount,
                                               const glm::mat4 &transform, bool skinned) const {
    // pipeline 位高2 = 蒙皮（值 2/3），低1 = 材质（0 Blinn / 1 PBR），
    // 使蒙皮与静态批次分组隔离（管线不同不可合批）。
    SortKey key;
    key.pipelineId = static_cast<uint8_t>(GetPipelineId(material) | (skinned ? kSkinPipelineBit : 0u));

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
    key.depthBits = std::bit_cast<uint32_t>(-viewPos.z);

    return key;
}

void Renderer3D::EndScene() {
    GE_PROFILE_SCOPE("Renderer3D::EndScene");

    GE_CORE_ASSERT(m_InScene, "EndScene called without BeginScene!");
    m_InScene = false;

    // 销毁上一帧切换环境时退休的旧环境（安全点，见 FlushRetiredEnvironments）
    FlushRetiredEnvironments();

    // 使同材质同 mesh 的实例连续排列（管线/纹理切换最少 + instancing 合批），
    // 深度从前往后，利用 early-z 减少过绘制。
    SortMeshes();

    auto &cmd = Renderer::GetFrameCmd();
    auto vkCmd = cmd.GetHandle();
    auto &frame = Renderer::GetRenderContext().GetActiveFrame();

    // 有效渲染目标：优先外部离屏目标，否则当前帧 swapchain 目标（视口/格式/附件均取自该目标）
    auto &renderTarget = m_RenderTargetOverride ? *m_RenderTargetOverride
                                                : frame.GetRenderTarget();

    // ── 共享描述符数据上传：Frame UBO / per-instance SSBO / 点光源 SSBO ──
    BufferAllocation frameUboAlloc = UploadFrameUBO(frame);

    std::vector<InstanceData> instances;
    std::vector<RenderBatch> batches;
    CollectBatches(instances, batches);
    BufferAllocation instanceBuffer = UploadInstanceBuffer(frame, instances);

    BufferAllocation lightBuffer = UploadLightBuffer(frame);

    // ── 渲染：开始动态渲染 → 天空盒背景 → 网格批次（管线 + 描述符 + 绘制） ──
    BeginDynamicRendering(cmd, renderTarget);

    // 天空盒：由"有无天空盒"控制（仅开关开启时提交；纹理是否就绪由 DrawSkybox 内部再判定）
    if (m_SkyboxEnabled) {
        DrawSkybox(cmd, frame, renderTarget);
    }

    ConfigureMeshPipeline(cmd, renderTarget);
    BindSharedUniforms(cmd, frameUboAlloc, lightBuffer);

    // 网格：由"有无批次"控制（无网格时整个实例循环无事可做）
    if (!batches.empty()) {
        DrawMeshInstances(cmd, frame, batches, instanceBuffer);
    }

    // ── 统计 draw call 与三角形数量 ──
    RecordStats(batches);

    // ── 7. 结束渲染 ───────────────────────────────────────────────────
    VulkanRenderingInfo::End(vkCmd);
}

void Renderer3D::SortMeshes() {
    // ── 0. 按排序键排序（材质 → mesh → 深度） ─────────────────────────
    //    使同材质同 mesh 的实例连续排列，既减少管线/纹理切换，又便于
    //    instancing 合批；深度从前往后，利用 early-z 减少过绘制。
    std::sort(m_Meshes.begin(), m_Meshes.end(),
              [](const MeshInstance &a, const MeshInstance &b) {
                  return a.sortKey < b.sortKey;
              });

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

    // IBL 参数：x = 预滤波图最高 mip 索引（MAX_REFLECTION_LOD，= levelCount-1）。
    // 无 IBL 时填 0，着色器 HAS_IBL 变体不采样该值（走常量环境光分支）。
    frameUBO.iblParams = glm::vec4(
        m_EnvironmentMap ? static_cast<float>(m_EnvironmentMap->GetPrefilterLevels() - 1) : 0.0f,
        0.0f, 0.0f, 0.0f);

    BufferAllocation frameUboAlloc = frame.AllocateBuffer(
        vk::BufferUsageFlagBits::eUniformBuffer, sizeof(FrameUBO));
    frameUboAlloc.update(frameUBO);

    return frameUboAlloc;
}

void Renderer3D::CollectBatches(std::vector<InstanceData> &instances,
                                 std::vector<RenderBatch> &batches) const {
    // ── 2. 阶段3：按 (mesh, material) 分组合批，构建 per-instance SSBO ──
    //    排序键已保证同材质同 mesh 的实例连续。单趟扫描把 (mesh, material)
    //    指针相等且连续的实例归为一个 RenderBatch，并把每个实例的 (model,
    //    color) 收集进 instances 数组，最终一次性上传到全局 storage buffer。
    instances.clear();
    instances.reserve(m_Meshes.size());

    batches.clear();
    batches.reserve(m_Meshes.size());

    for (size_t i = 0; i < m_Meshes.size();) {
        const auto &first = m_Meshes[i];
        Material *mat = first.material;
        Mesh *mesh = first.mesh;
        uint32_t firstIndex = first.firstIndex;
        uint32_t indexCount = first.indexCount;
        const void *skinKey = first.skinKey;

        // 找同 (mesh, 子网格, material, skin) 的连续区间。
        // skin 也参与分桶：不同皮肤不可合批（关节矩阵不同），且蒙皮/静态亦分离。
        size_t runStart = i;
        while (i < m_Meshes.size()
               && m_Meshes[i].material == mat
               && m_Meshes[i].mesh == mesh
               && m_Meshes[i].firstIndex == firstIndex
               && m_Meshes[i].indexCount == indexCount
               && m_Meshes[i].skinKey == skinKey) {
            ++i;
        }

        // 收集本批次实例的 per-instance 数据（model + color）
        // 材质标量参数（如 shininess）已迁入 per-material UBO，
        // 在绘制循环中按批次绑定，不在此冗余写入实例数据。
        uint32_t firstInstance = static_cast<uint32_t>(instances.size());
        for (size_t k = runStart; k < i; ++k) {
            const auto &inst = m_Meshes[k];
            instances.push_back(InstanceData{inst.transform, inst.color});
        }

        batches.push_back(RenderBatch{
            mesh, firstIndex, indexCount, mat, firstInstance,
            static_cast<uint32_t>(i - runStart), skinKey});
    }
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

void Renderer3D::BeginDynamicRendering(VulkanCommandBuffer &cmd, RenderTarget &renderTarget) {
    const auto extent = renderTarget.GetExtent();
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
        vk::ImageView colorView = renderTarget.GetColorResolveView().GetHandle();
        // 附件布局与图像实际布局一致：离屏颜色图固定 GENERAL，否则验证层报
        // VUID-vkCmdBeginRendering-pRenderingInfo-09592。正常 swapchain 用默认。
        renderInfo.AddColorAttachment(colorView,
                                      vk::AttachmentLoadOp::eClear,
                                      vk::AttachmentStoreOp::eStore,
                                      clearValue,
                                      renderTarget.HasOffscreenColor()
                                          ? vk::ImageLayout::eGeneral
                                          : vk::ImageLayout::eColorAttachmentOptimal);

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

    renderInfo.Begin(cmd.GetHandle());
}

void Renderer3D::DrawSkybox(VulkanCommandBuffer &cmd, VulkanRenderFrame &frame,
                             RenderTarget &renderTarget) {
    const auto extent = renderTarget.GetExtent();
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
                                ? &m_EnvironmentMap->GetSkybox() : nullptr;

    if (m_SkyboxEnabled && skyTex) {
        auto skyColorFmt = renderTarget.GetColorFormat();
        vk::Format skyDepthFmt = vk::Format::eUndefined;
        if (renderTarget.HasDepth()) {
            skyDepthFmt = renderTarget.GetDepthFormat();
        }

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
        skyVp.width  = static_cast<float>(extent.width);
        skyVp.height = static_cast<float>(extent.height);
        skyVp.minDepth = 0.0f;
        skyVp.maxDepth = 1.0f;
        cmd.SetViewport(0, {skyVp});

        vk::Rect2D skyScissor;
        skyScissor.extent.width  = extent.width;
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

void Renderer3D::ConfigureMeshPipeline(VulkanCommandBuffer &cmd, RenderTarget &renderTarget) {
    const auto extent = renderTarget.GetExtent();
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
    auto colorFmt = renderTarget.GetColorFormat();

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
    //     stride 以 C++ Vertex 结构体尺寸为权威：mesh.vert 仅声明 location 0-3
    //     （忽略蒙皮字段），反射求和会得到 48B 的错误 stride，故显式传入
    //     sizeof(Vertex)（80B）覆盖；属性 offset 仍由反射紧密打包（前 4 字段
    //     与结构体前 48B 一一对应）。蒙皮管线 mesh_skinned.vert 声明全部
    //     location 后无需额外覆盖，但保持一致做法无害。
    ps.setVertexInputFromShader(*m_VertShader, 0, vk::VertexInputRate::eVertex,
                                static_cast<uint32_t>(sizeof(Vertex)));

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
                                    const BufferAllocation &lightBuffer) {
    // ── 5. 绑定 Frame UBO（set 0, binding 0，所有网格共享） ───────────
    cmd.BindBuffer(frameUbo.get_buffer(), frameUbo.get_offset(),
                   frameUbo.get_size(), 0, 0);

    // 绑定点光源 SSBO（set 0, binding 1）——所有网格共享
    cmd.BindBuffer(lightBuffer.get_buffer(), lightBuffer.get_offset(),
                   lightBuffer.get_size(), 0, 1);
}

void Renderer3D::DrawMeshInstances(VulkanCommandBuffer &cmd, VulkanRenderFrame &frame,
                                    const std::vector<RenderBatch> &batches,
                                    const BufferAllocation &instanceBuffer) {
    // ── 6. 逐批次 instanced 绘制 ─────────────────────────────────────
    vk::DeviceSize vertexOffset = 0;

    // IBL 是否启用（全局）：需要已加载环境图且三张图均已就绪且 IBL 开关打开，
    // 此时 PBR 批次走 IBL 变体管线并绑定三张 IBL 图；否则回退无 IBL 变体
    // （常量环境光）。异步加载中环境图未就绪则本帧不启用 IBL，就绪后自动切换。
    const bool useIbl = (m_EnvironmentMap != nullptr) && m_EnvironmentMap->IsReady()
                        && m_IBLEnabled;

    // 当前绑定的管线 id（初始为 Blinn-Phong，已在上方绑定 *m_PipelineLayout）。
    // 排序键已按 pipelineId 分组，故同类型批次连续，切换频率最低。
    uint8_t currentPipelineId = 0;
    for (const auto &batch : batches) {
        // —— 管线路由：pipelineId 由「材质(PBR 位) + 是否蒙皮(蒙皮位)」决定。
        //    batch.skinKey 非 null ⇒ 蒙皮实例，走 mesh_skinned 管线并含蒙皮位。
        //    切换时同时按对应顶点着色器反射重设顶点输入（蒙皮肤管线有 location
        //    4/5，静态管线没有），否则蒙皮字段会被静态顶点输入丢弃。
        const bool skinned = (batch.skinKey != nullptr);
        const uint8_t pipelineId = static_cast<uint8_t>(
            GetPipelineId(batch.material) | (skinned ? kSkinPipelineBit : 0u));
        const bool pbr = ((pipelineId & 0x01u) != 0);

        if (pipelineId != currentPipelineId) {
            VulkanPipelineLayout *targetLayout = m_PipelineLayout;
            if (skinned) {
                targetLayout = pbr
                    ? (useIbl ? m_PipelineLayoutSkinnedPBR_IBL
                              : m_PipelineLayoutSkinnedPBR)
                    : m_PipelineLayoutSkinned;
            } else {
                targetLayout = pbr
                    ? (useIbl ? m_PipelineLayoutPBR_IBL : m_PipelineLayoutPBR)
                    : m_PipelineLayout;
            }

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
            if (pbr && useIbl) {
                auto &ibl = *m_EnvironmentMap;
                cmd.BindImage(ibl.GetPrefilter().GetImageView(),
                              ibl.GetPrefilter().GetSampler(), 1, 5); // 辐照度
                cmd.BindImage(ibl.GetPrefilter().GetImageView(),
                              ibl.GetPrefilter().GetSampler(), 1, 6); // 预滤波
                cmd.BindImage(ibl.GetBrdfLUT().GetImageView(),
                              ibl.GetBrdfLUT().GetSampler(), 1, 7);   // BRDF LUT
            }
        }

        // 绑定纹理（set 1, binding 0 = Albedo，binding 1 = Normal）
        // 从 material 对应槽位取纹理，无材质或无纹理时使用默认纹理 fallback
        Texture *tex = GetEffectiveTexture(batch.material);
        if (tex) {
            cmd.BindImage(tex->GetImageView(),
                          tex->GetSampler(),
                          1, 0);
        }

        // 法线贴图（set 1, binding 1）：无材质或无纹理时使用默认"平坦法线"
        // 纹理，其映射回 (0,0,1) 不改变光照，保证材质无需法线贴图也能正常渲染
        Texture *normalTex = GetEffectiveNormalTexture(batch.material);
        if (normalTex) {
            cmd.BindImage(normalTex->GetImageView(),
                          normalTex->GetSampler(),
                          1, 1);
        }

        // 自发光贴图（set 1, binding 3）：无材质或无纹理时使用默认白色纹理，
        // 采样为 (1,1,1)，自发光 = emissiveFactor 颜色，支持"仅因子发光"。
        // 有贴图时贴图颜色 × 因子，贴图黑色区域不发光。
        Texture *emissiveTex = GetEffectiveEmissiveTexture(batch.material);
        if (emissiveTex) {
            cmd.BindImage(emissiveTex->GetImageView(),
                          emissiveTex->GetSampler(),
                          1, 3);
        }

        // 金属-粗糙度贴图（set 1, binding 4）：仅 PBR 材质使用。无 MR 贴图时
        // 绑定默认 (G=1, B=1) 纹理，回退到标量 metallic/roughness。
        // Blinn-Phong 批次不绑定（其 set 1 布局无 binding 4，绑了也会被过滤，
        // 这里显式判断更清晰，避免多余绑定）。PBR 蒙皮肤也一样要绑。
        if (pbr) {
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
        materialUBO.params.w = batch.material
            ? batch.material->GetFloat("uvTiling", 1.0f)
            : 1.0f;
        materialUBO.pbr.x = batch.material
            ? batch.material->GetFloat("metallic", 0.0f)
            : 0.0f;
        materialUBO.pbr.y = batch.material
            ? batch.material->GetFloat("roughness", 0.5f)
            : 0.5f;
        materialUBO.emissiveFactor = batch.material
            ? glm::vec4(batch.material->GetEmissiveFactor(), 0.0f)
            : glm::vec4(0.0f);
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

void Renderer3D::RecordStats(const std::vector<RenderBatch> &batches) {
    // ── 6b. 统计 draw call 与三角形数量（draw call = 批次数量） ───────
    uint32_t triangles = 0;
    for (const auto &instance : m_Meshes) {
        triangles += instance.indexCount / 3;
    }
    Renderer::Get().AddStats3D(static_cast<uint32_t>(batches.size()), triangles);

    // 统计 instancing 批次数量（相同 mesh + 相同材质分一组），
    // 用于观察合批收益：批次数越少 → draw call 越少
    Renderer::Get().AddBatches3D(static_cast<uint32_t>(batches.size()));
}


} // namespace GE