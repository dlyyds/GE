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

    // ── 延迟渲染：GBuffer 片元着色器 + 静态/蒙皮两份管线布局 ─────────
    // GBuffer 只输出 G-Buffer 属性，光照后置到 Lighting pass。静态路径复用
    // mesh.vert，蒙皮路径复用 mesh_skinned.vert，与现有批次路由保持一致。
    m_FragShaderGBuffer = &cache.RequestShaderModule(
        vk::ShaderStageFlagBits::eFragment,
        ShaderSource(Renderer::GetAssetManager()
            .ResolvePath(std::string(AssetPaths::Shaders) + "/mesh_gbuffer.frag.spv")
            .string()),
        "main", ShaderVariant{});

    m_PipelineLayoutGBuffer = &cache.RequestPipelineLayout(
        {m_VertShader, m_FragShaderGBuffer});
    m_PipelineLayoutGBuffer->SetDebugName("Mesh3D_PipelineLayout_GBuffer");

    m_PipelineLayoutSkinnedGBuffer = &cache.RequestPipelineLayout(
        {m_VertShaderSkinned, m_FragShaderGBuffer});
    m_PipelineLayoutSkinnedGBuffer->SetDebugName("Mesh3D_PipelineLayout_Skinned_GBuffer");

    // ── 阴影深度 pass：depth_only.frag + 静态/蒙皮两份管线布局 ────────
    // 顶点级复用 mesh.vert / mesh_skinned.vert（输出 varyings 是超集，多出的忽略），
    // 片元只采样 Albedo 做 MASK 镂空、无颜色输出；阴影 pass 只有深度附件。
    m_FragShaderDepthOnly = &cache.RequestShaderModule(
        vk::ShaderStageFlagBits::eFragment,
        ShaderSource(Renderer::GetAssetManager()
            .ResolvePath(std::string(AssetPaths::Shaders) + "/depth_only.frag.spv")
            .string()),
        "main", ShaderVariant{});

    m_PipelineLayoutShadow = &cache.RequestPipelineLayout(
        {m_VertShader, m_FragShaderDepthOnly});
    m_PipelineLayoutShadow->SetDebugName("Mesh3D_PipelineLayout_Shadow");

    m_PipelineLayoutSkinnedShadow = &cache.RequestPipelineLayout(
        {m_VertShaderSkinned, m_FragShaderDepthOnly});
    m_PipelineLayoutSkinnedShadow->SetDebugName("Mesh3D_PipelineLayout_Skinned_Shadow");

    // 阴影深度采样器：最近邻（深度不可线性插值，PCF 逐 tap 硬比较）+ 边缘钳制。
    // 出界 tap 钳到边缘 texel 无碍——shader 已按中心 UV 判出界（§3.4/§5.6）。
    m_ShadowSampler = &cache.RequestSampler(
        vk::Filter::eNearest, vk::Filter::eNearest, vk::SamplerMipmapMode::eNearest,
        vk::SamplerAddressMode::eClampToEdge, vk::SamplerAddressMode::eClampToEdge,
        vk::SamplerAddressMode::eClampToEdge);

    // ── 延迟渲染：Lighting 全屏三角形着色器 + 管线布局 ──────────────
    m_LightingVert = &cache.RequestShaderModule(
        vk::ShaderStageFlagBits::eVertex,
        ShaderSource(Renderer::GetAssetManager()
            .ResolvePath(std::string(AssetPaths::Shaders) + "/deferred_lighting.vert.spv")
            .string()),
        "main", ShaderVariant{});

    m_LightingFrag = &cache.RequestShaderModule(
        vk::ShaderStageFlagBits::eFragment,
        ShaderSource(Renderer::GetAssetManager()
            .ResolvePath(std::string(AssetPaths::Shaders) + "/deferred_lighting.frag.spv")
            .string()),
        "main", ShaderVariant{});

    m_LightingLayout = &cache.RequestPipelineLayout({m_LightingVert, m_LightingFrag});
    m_LightingLayout->SetDebugName("DeferredLighting_PipelineLayout");

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

    // ── 7. 加载默认 1x1 白立方体贴图（延迟 Lighting 天空盒不可用时的 fallback） ──
    //    延迟 Lighting shader 把天空盒采样器声明为 samplerCube；环境图不在时不能
    //    用普通 2D 白纹理填充，否则类型不匹配。这里加载一个极小的白 cubemap，
    //    保证 shader 的 samplerCube 始终可绑定。
    m_DefaultSkyboxTexture = Texture::LoadCubeMapFromFile(
        device, cache,
        Renderer::GetAssetManager().ResolvePath("environments/_default_cube/skybox.ktx2").string());
    if (m_DefaultSkyboxTexture) {
        m_DefaultSkyboxTexture->SetDebugName("DefaultSkyboxTexture");
    } else {
        GE_CORE_ERROR("Renderer3D: 加载默认天空盒纹理失败！");
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
    auto &cache = device.GetResourceCache();
    auto &am = Renderer::GetAssetManager();
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
        m_EnvironmentName.clear(); // 允许下次重试
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
    m_DefaultSkyboxTexture.reset();

    // 释放环境映射（IBL）资源（含天空盒纹理）
    m_EnvironmentMap.reset();

    // 着色器和 pipeline layout 由全局资源缓存管理，不需要手动释放
    m_VertShader = nullptr;
    m_FragShader = nullptr;
    m_FragShaderPBR = nullptr;
    m_FragShaderPBR_IBL = nullptr;
    m_FragShaderGBuffer = nullptr;
    m_PipelineLayout = nullptr;
    m_PipelineLayoutPBR = nullptr;
    m_PipelineLayoutPBR_IBL = nullptr;
    m_PipelineLayoutGBuffer = nullptr;
    m_PipelineLayoutSkinnedGBuffer = nullptr;
    m_LightingVert = nullptr;
    m_LightingFrag = nullptr;
    m_LightingLayout = nullptr;
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
        return 1;
    }
    return 0;
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

    // 阴影深度图（set 1, binding 7）：SceneLayer 在方向光阴影开启时才追加 hShadow 读，
    // 故 readImageViews 多于 4 项即有阴影图（G0~G3 + ShadowMap）。采样器用最近邻
    // （§5.6），PCF 逐 tap 硬比较在 shader 侧完成。无阴影时不绑，shader 走开关分支。
    if (ctx.readImageViews.size() > 4 && m_ShadowSampler) {
        cmd.BindImage(*ctx.readImageViews[4], *m_ShadowSampler, 1, 7);
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
                              ctx.renderArea.extent, /*transparent=*/true);
        BindSharedUniforms(*ctx.cmd, m_CachedFrameUBO, m_CachedLightBuffer,
                           /*bindLights=*/true);
        if (!m_TransparentBatches.empty()) {
            DrawMeshInstances(*ctx.cmd, *ctx.frame, m_TransparentBatches,
                              m_CachedInstanceBuffer, /*gbuffer=*/false);
        }

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

    // ── 统计 draw call 与三角形数量 ──
    RecordStats(static_cast<uint32_t>(batches.size()));
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

    // 方向光阴影（S1）：光空间 view-proj 与参数打包。阶段 1 shader 尚不采样
    // 这些字段（无视觉变化），S4 接阴影比较时消费。
    ubo.lightViewProj = m_LightParams.lightViewProj;
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

void Renderer3D::ConfigureMeshPipeline(VulkanCommandBuffer &cmd,
                                       vk::Format colorFormat, vk::Format depthFormat,
                                       vk::Extent2D extent,
                                       bool transparent) {
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
        blendState.dstAlphaBlendFactor = vk::BlendFactor::eOneMinusSrcAlpha;
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
                                                    bool skinned) {
    // 优先级：阴影 > GBuffer > 前向。前向按 PBR → IBL 逐级展开；
    // 每个分支取「蒙皮 / 静态」对应布局。顺序判定代替嵌套三元，直读。
    if (shadow) {
        return skinned ? m_PipelineLayoutSkinnedShadow : m_PipelineLayoutShadow;
    }
    if (gbuffer) {
        return skinned ? m_PipelineLayoutSkinnedGBuffer : m_PipelineLayoutGBuffer;
    }
    if (pbr && useIbl) {
        return skinned ? m_PipelineLayoutSkinnedPBR_IBL : m_PipelineLayoutPBR_IBL;
    }
    if (pbr) {
        return skinned ? m_PipelineLayoutSkinnedPBR : m_PipelineLayoutPBR;
    }
    return skinned ? m_PipelineLayoutSkinned : m_PipelineLayout;
}

void Renderer3D::DrawMeshInstances(VulkanCommandBuffer &cmd, VulkanRenderFrame &frame,
                                   const std::vector<RenderBatch> &batches,
                                   const BufferAllocation &instanceBuffer,
                                   bool gbuffer,
                                   bool shadow) {
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
            VulkanPipelineLayout *targetLayout =
                ResolveMeshLayout(shadow, gbuffer, pbr, useIbl, skinned);

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

void Renderer3D::RecordStats(uint32_t drawCallCount) {
    // ── 6b. 统计 draw call 与三角形数量 ───────────────────────────────
    // draw call = 不透明批次数 + 透明批次数（RecordScene 把两段合起来统计，
    // 半透明逐实例成批已计入；延迟链由 PrepareDeferredBatches 切分后的两段之和）。
    // 三角形按实例累计：m_Meshes 线性包含全部实例，两段不重复计数。
    uint32_t triangles = 0;
    for (const auto &instance : m_Meshes) {
        triangles += instance.indexCount / 3;
    }
    Renderer::Get().AddStats3D(drawCallCount, triangles);

    // 统计 instancing 批次数量（相同 mesh + 相同材质分一组），
    // 用于观察合批收益：批次数越少 → draw call 越少
    Renderer::Get().AddBatches3D(drawCallCount);
}


} // namespace GE
