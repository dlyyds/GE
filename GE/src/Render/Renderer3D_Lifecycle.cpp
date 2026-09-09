/**
 * @file Renderer3D_Lifecycle.cpp
 * @brief Renderer3D 生命周期与环境映射实现分片。
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
// ==================== 构造 / 析构 / 环境 ====================
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

    // ── 延迟 HDR 透明合成：PBR/PBR-IBL 无 ACES 片元（mesh_pbr_hdr.frag）────────
    // 透明段在 Tonemap 前写入 Scene_HDR，必须保留线性 HDR；PBR-IBL 变体供 IBL 启用时使用。
    m_FragShaderPBR_HDR = &cache.RequestShaderModule(
        vk::ShaderStageFlagBits::eFragment,
        ShaderSource(Renderer::GetAssetManager()
            .ResolvePath(std::string(AssetPaths::Shaders) + "/mesh_pbr_hdr.frag.spv")
            .string()),
        "main", ShaderVariant{});

    m_FragShaderPBR_IBL_HDR = &cache.RequestShaderModule(
        vk::ShaderStageFlagBits::eFragment,
        ShaderSource(Renderer::GetAssetManager()
            .ResolvePath(std::string(AssetPaths::Shaders) + "/mesh_pbr_ibl_hdr.frag.spv")
            .string()),
        "main", ShaderVariant{});

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

    m_PipelineLayoutPBR_HDR = &cache.RequestPipelineLayout(
        {m_VertShader, m_FragShaderPBR_HDR});
    m_PipelineLayoutPBR_HDR->SetDebugName("Mesh3D_PipelineLayout_PBR_HDR");

    m_PipelineLayoutSkinnedPBR_HDR = &cache.RequestPipelineLayout(
        {m_VertShaderSkinned, m_FragShaderPBR_HDR});
    m_PipelineLayoutSkinnedPBR_HDR->SetDebugName("Mesh3D_PipelineLayout_Skinned_PBR_HDR");

    m_PipelineLayoutPBR_IBL_HDR = &cache.RequestPipelineLayout(
        {m_VertShader, m_FragShaderPBR_IBL_HDR});
    m_PipelineLayoutPBR_IBL_HDR->SetDebugName("Mesh3D_PipelineLayout_PBR_IBL_HDR");

    m_PipelineLayoutSkinnedPBR_IBL_HDR = &cache.RequestPipelineLayout(
        {m_VertShaderSkinned, m_FragShaderPBR_IBL_HDR});
    m_PipelineLayoutSkinnedPBR_IBL_HDR->SetDebugName("Mesh3D_PipelineLayout_Skinned_PBR_IBL_HDR");

    // ── 水面（阶段 1）：water.vert / water.frag / water_hdr.frag ──────
    // 水面拥有独立水格与 UBO（WaterUBO），不塞进通用 mesh 管线；前向用
    // water.frag（ACES），HDR 透明段用 water_hdr.frag（线性 HDR，交给
    // Tonemap 统一色调映射）。
    m_VertShaderWater = &cache.RequestShaderModule(
        vk::ShaderStageFlagBits::eVertex,
        ShaderSource(Renderer::GetAssetManager()
            .ResolvePath(std::string(AssetPaths::Shaders) + "/water.vert.spv")
            .string()),
        "main", ShaderVariant{});

    m_FragShaderWater = &cache.RequestShaderModule(
        vk::ShaderStageFlagBits::eFragment,
        ShaderSource(Renderer::GetAssetManager()
            .ResolvePath(std::string(AssetPaths::Shaders) + "/water.frag.spv")
            .string()),
        "main", ShaderVariant{});

    m_FragShaderWaterHDR = &cache.RequestShaderModule(
        vk::ShaderStageFlagBits::eFragment,
        ShaderSource(Renderer::GetAssetManager()
            .ResolvePath(std::string(AssetPaths::Shaders) + "/water_hdr.frag.spv")
            .string()),
        "main", ShaderVariant{});

    m_WaterLayout = &cache.RequestPipelineLayout(
        {m_VertShaderWater, m_FragShaderWater});
    m_WaterLayout->SetDebugName("Water_PipelineLayout");

    m_WaterLayoutHDR = &cache.RequestPipelineLayout(
        {m_VertShaderWater, m_FragShaderWaterHDR});
    m_WaterLayoutHDR->SetDebugName("Water_PipelineLayout_HDR");

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

    // Stage 2: Scene_HDR / SceneDepth snapshot passes used by the water Transparent pass.
    m_SceneColorCopyFrag = &cache.RequestShaderModule(
        vk::ShaderStageFlagBits::eFragment,
        ShaderSource(Renderer::GetAssetManager()
            .ResolvePath(std::string(AssetPaths::Shaders) + "/scene_color_copy.frag.spv")
            .string()),
        "main", ShaderVariant{});
    m_SceneDepthCopyFrag = &cache.RequestShaderModule(
        vk::ShaderStageFlagBits::eFragment,
        ShaderSource(Renderer::GetAssetManager()
            .ResolvePath(std::string(AssetPaths::Shaders) + "/scene_depth_copy.frag.spv")
            .string()),
        "main", ShaderVariant{});
    m_SceneColorCopyLayout = &cache.RequestPipelineLayout({m_LightingVert, m_SceneColorCopyFrag});
    m_SceneColorCopyLayout->SetDebugName("SceneColorCopy_PipelineLayout");
    m_SceneDepthCopyLayout = &cache.RequestPipelineLayout({m_LightingVert, m_SceneDepthCopyFrag});
    m_SceneDepthCopyLayout->SetDebugName("SceneDepthCopy_PipelineLayout");

    // ── 延迟渲染：Tonemap 全屏三角形着色器 + 管线布局 ──────────────
    m_TonemapVert = &cache.RequestShaderModule(
        vk::ShaderStageFlagBits::eVertex,
        ShaderSource(Renderer::GetAssetManager()
            .ResolvePath(std::string(AssetPaths::Shaders) + "/tonemap.vert.spv")
            .string()),
        "main", ShaderVariant{});

    m_TonemapFrag = &cache.RequestShaderModule(
        vk::ShaderStageFlagBits::eFragment,
        ShaderSource(Renderer::GetAssetManager()
            .ResolvePath(std::string(AssetPaths::Shaders) + "/tonemap.frag.spv")
            .string()),
        "main", ShaderVariant{});

    m_TonemapLayout = &cache.RequestPipelineLayout({m_TonemapVert, m_TonemapFrag});
    m_TonemapLayout->SetDebugName("Tonemap_PipelineLayout");

    // ── Bloom 全屏三角形着色器 + 管线布局（延迟 HDR 链：Transparent → Bloom → Tonemap）─
    m_BloomVert = &cache.RequestShaderModule(
        vk::ShaderStageFlagBits::eVertex,
        ShaderSource(Renderer::GetAssetManager()
            .ResolvePath(std::string(AssetPaths::Shaders) + "/bloom.vert.spv")
            .string()),
        "main", ShaderVariant{});

    m_BloomExtractFrag = &cache.RequestShaderModule(
        vk::ShaderStageFlagBits::eFragment,
        ShaderSource(Renderer::GetAssetManager()
            .ResolvePath(std::string(AssetPaths::Shaders) + "/bloom_extract.frag.spv")
            .string()),
        "main", ShaderVariant{});

    m_BloomDownsampleFrag = &cache.RequestShaderModule(
        vk::ShaderStageFlagBits::eFragment,
        ShaderSource(Renderer::GetAssetManager()
            .ResolvePath(std::string(AssetPaths::Shaders) + "/bloom_downsample.frag.spv")
            .string()),
        "main", ShaderVariant{});

    m_BloomUpsampleFrag = &cache.RequestShaderModule(
        vk::ShaderStageFlagBits::eFragment,
        ShaderSource(Renderer::GetAssetManager()
            .ResolvePath(std::string(AssetPaths::Shaders) + "/bloom_upsample.frag.spv")
            .string()),
        "main", ShaderVariant{});

    m_BloomCompositeFrag = &cache.RequestShaderModule(
        vk::ShaderStageFlagBits::eFragment,
        ShaderSource(Renderer::GetAssetManager()
            .ResolvePath(std::string(AssetPaths::Shaders) + "/bloom_composite.frag.spv")
            .string()),
        "main", ShaderVariant{});

    m_BloomExtractLayout = &cache.RequestPipelineLayout(
        {m_BloomVert, m_BloomExtractFrag});
    m_BloomExtractLayout->SetDebugName("Bloom_Extract_PipelineLayout");

    m_BloomDownsampleLayout = &cache.RequestPipelineLayout(
        {m_BloomVert, m_BloomDownsampleFrag});
    m_BloomDownsampleLayout->SetDebugName("Bloom_Downsample_PipelineLayout");

    m_BloomUpsampleLayout = &cache.RequestPipelineLayout(
        {m_BloomVert, m_BloomUpsampleFrag});
    m_BloomUpsampleLayout->SetDebugName("Bloom_Upsample_PipelineLayout");

    m_BloomCompositeLayout = &cache.RequestPipelineLayout(
        {m_BloomVert, m_BloomCompositeFrag});
    m_BloomCompositeLayout->SetDebugName("Bloom_Composite_PipelineLayout");

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
    m_SceneColorCopyFrag = nullptr;
    m_SceneDepthCopyFrag = nullptr;
    m_SceneColorCopyLayout = nullptr;
    m_SceneDepthCopyLayout = nullptr;
    m_SkyboxVert = nullptr;
    m_SkyboxFrag = nullptr;
    m_SkyboxLayout = nullptr;
}
} // namespace GE
