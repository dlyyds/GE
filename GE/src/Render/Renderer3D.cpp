/**
 * @file Renderer3D.cpp
 * @brief 3D 网格渲染器实现。
 */

#include "Render/Renderer3D.h"

#include <algorithm>
#include <bit>

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

    // ── 5. 创建默认 1x1 黑色纹理（无自发光贴图时的 fallback） ──────────
    //    RGB = (0, 0, 0)：采样后加色为 0，不改变光照结果，即物体不发光，
    //    使未绑定自发光贴图的材质表现得如同未使用自发光。
    // 像素字面量按 0xAABBGGRR 小端约定书写：0xFF000000 = (0, 0, 0, 255) 黑色不透明。
    // 注意：切勿写成 0x000000FF，那会解析成 (255,0,0,0) 红色透明，导致无自发光
    // 贴图时物体发出红光（shader 仅采样 .rgb，alpha 被忽略）。
    uint32_t blackPixel = 0xFF000000; // RGBA8: (0, 0, 0, 255)
    m_DefaultEmissiveTexture = Texture::LoadFromMemory(
        device, cache, &blackPixel, 1, 1,
        vk::Format::eR8G8B8A8Unorm,
        vk::Filter::eLinear, vk::Filter::eLinear);
    if (m_DefaultEmissiveTexture) {
        m_DefaultEmissiveTexture->SetDebugName("DefaultEmissiveTexture");
    } else {
        GE_CORE_ERROR("Renderer3D: 创建默认黑色自发光纹理失败！");
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

void Renderer3D::SetSkybox(const std::string &filepath) {
    auto &device = Renderer::GetVulkanContext().GetDevice();
    auto &cache  = device.GetResourceCache();

    // 先解析为绝对路径（相对资源根），Texture::LoadFromFile 用 stbi_load 直接读
    // 文件，不会自动解析相对路径，必须在此转成绝对路径
    const std::string resolved =
        Renderer::GetAssetManager().ResolvePath(filepath).string();

    // 加载等距柱状投影纹理（Unorm 直接采样，与现有纹理一致；全屏图跳过 mipmap）
    auto tex = Texture::LoadFromFile(device, cache, resolved,
                                     vk::Format::eR8G8B8A8Unorm,
                                     vk::Filter::eLinear, vk::Filter::eLinear,
                                     /*generate_mipmaps*/ false);
    if (!tex) {
        GE_CORE_ERROR("Renderer3D: 天空盒纹理加载失败: {0}", filepath);
        m_SkyboxEnabled = false;
        return;
    }

    tex->SetDebugName("Skybox_Equirect");
    m_SkyboxTexture = std::move(tex);
    m_SkyboxPath    = filepath;
    m_SkyboxEnabled = true;
    GE_CORE_INFO("Renderer3D: 天空盒已启用: {0}", filepath);
}

Renderer3D::~Renderer3D() {
    GE_CORE_INFO("Renderer3D Shutdown");

    // 释放默认纹理
    // 注：m_DefaultWhiteTexture 由全局 TextureManager 持有，不属于本渲染器，无需释放
    m_DefaultNormalTexture.reset();
    m_DefaultEmissiveTexture.reset();
    m_DefaultMetallicRoughnessTexture.reset();

    // 释放天空盒纹理
    m_SkyboxTexture.reset();

    // 着色器和 pipeline layout 由全局资源缓存管理，不需要手动释放
    m_VertShader = nullptr;
    m_FragShader = nullptr;
    m_FragShaderPBR = nullptr;
    m_PipelineLayout = nullptr;
    m_PipelineLayoutPBR = nullptr;
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

void Renderer3D::DrawSubMeshImpl(const glm::mat4 &transform,
                                 Mesh *mesh,
                                 uint32_t firstIndex,
                                 uint32_t indexCount,
                                 Material *material,
                                 const glm::vec4 &color) {
    GE_CORE_ASSERT(m_InScene, "DrawMesh called outside BeginScene/EndScene!");

    if (!mesh || indexCount == 0) {
        return;
    }

    // 计算排序键（pipeline → 材质 → mesh → 子网格 → view 空间深度），用于 EndScene
    // 前分组排序，使同材质同 mesh 同子网格的实例连续，便于 instancing 合批
    SortKey sortKey = ComputeSortKey(material, mesh, firstIndex, indexCount, transform);

    m_Meshes.push_back({transform, mesh, firstIndex, indexCount, material, color, sortKey});
}

// ============================================================================
// 内部工具方法（排序键相关）
// ============================================================================

Texture *Renderer3D::GetEffectiveTexture(const Material *material) const {
    // 优先取材质 Albedo 槽位纹理，无材质或无纹理时回退到默认白色纹理
    Texture *tex = (material ? material->GetTexture(Material::Albedo) : nullptr);
    return tex ? tex : m_DefaultWhiteTexture;
}

Texture *Renderer3D::GetEffectiveNormalTexture(const Material *material) const {
    // 优先取材质 Normal 槽位纹理，无材质或无纹理时回退到默认平坦法线纹理
    Texture *tex = (material ? material->GetTexture(Material::Normal) : nullptr);
    return tex ? tex : m_DefaultNormalTexture.get();
}

Texture *Renderer3D::GetEffectiveEmissiveTexture(const Material *material) const {
    // 优先取材质 Emissive 槽位纹理，无材质或无纹理时回退到默认黑色纹理
    // （RGB=(0,0,0)，物体不发光）
    Texture *tex = (material ? material->GetTexture(Material::Emissive) : nullptr);
    return tex ? tex : m_DefaultEmissiveTexture.get();
}

Texture *Renderer3D::GetEffectiveMetallicRoughnessTexture(const Material *material) const {
    // 优先取材质 MetallicRoughness 槽位纹理，无材质或无纹理时回退到默认
    // (G=1,B=1) 纹理，使 metallic/roughness 等于标量系数原值
    Texture *tex = (material ? material->GetTexture(Material::MetallicRoughness) : nullptr);
    return tex ? tex : m_DefaultMetallicRoughnessTexture.get();
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
                                               const glm::mat4 &transform) const {
    // pipeline：按材质类型路由（BlinnPhong=0 / PBR=1），使同类型连续，
    // 减少管线切换（管线切换最贵）。
    SortKey key;
    key.pipelineId = GetPipelineId(material);

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

    // 没有网格也没有天空盒时，直接返回（仅天空盒时仍需走完渲染流程）
    if (m_Meshes.empty() && !m_SkyboxEnabled) {
        return;
    }

    // ── 0. 按排序键排序（材质 → mesh → 深度） ─────────────────────────
    //    使同材质同 mesh 的实例连续排列，既减少管线/纹理切换，又便于
    //    instancing 合批；深度从前往后，利用 early-z 减少过绘制。
    std::sort(m_Meshes.begin(), m_Meshes.end(),
              [](const MeshInstance &a, const MeshInstance &b) {
                  return a.sortKey < b.sortKey;
              });

    auto &cmd = Renderer::GetFrameCmd();
    auto vkCmd = cmd.GetHandle();
    auto &frame = Renderer::GetRenderContext().GetActiveFrame();

    // 有效渲染目标：优先使用外部指定的目标（离屏），否则使用当前帧的 swapchain 目标。
    // 视口/渲染区域/颜色格式/颜色附件均取自该目标，便于把场景渲染进离屏纹理。
    auto &renderTarget = m_RenderTargetOverride ? *m_RenderTargetOverride
                                                : frame.GetRenderTarget();
    auto extent = renderTarget.GetExtent();

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

    BufferAllocation frameUboAlloc = frame.AllocateBuffer(
        vk::BufferUsageFlagBits::eUniformBuffer, sizeof(FrameUBO));
    frameUboAlloc.update(frameUBO);

    // ── 2. 阶段3：按 (mesh, material) 分组合批，构建 per-instance SSBO ──
    //    排序键已保证同材质同 mesh 的实例连续。单趟扫描把 (mesh, material)
    //    指针相等且连续的实例归为一个 RenderBatch，并把每个实例的 (model,
    //    color) 收集进 instances 数组，最终一次性上传到全局 storage buffer。
    std::vector<InstanceData> instances;
    instances.reserve(m_Meshes.size());

    std::vector<RenderBatch> batches;
    batches.reserve(m_Meshes.size());

    for (size_t i = 0; i < m_Meshes.size();) {
        const auto &first = m_Meshes[i];
        Material *mat = first.material;
        Mesh *mesh = first.mesh;
        uint32_t firstIndex = first.firstIndex;
        uint32_t indexCount = first.indexCount;

        // 找同 (mesh, 子网格, material) 的连续区间
        size_t runStart = i;
        while (i < m_Meshes.size()
               && m_Meshes[i].material == mat
               && m_Meshes[i].mesh == mesh
               && m_Meshes[i].firstIndex == firstIndex
               && m_Meshes[i].indexCount == indexCount) {
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
            static_cast<uint32_t>(i - runStart)});
    }

    // 分配全局实例 SSBO 并一次性上传（所有批次共享）
    BufferAllocation instanceBuffer = frame.AllocateBuffer(
        vk::BufferUsageFlagBits::eStorageBuffer,
        instances.size() * sizeof(InstanceData));
    instanceBuffer.update(instances);

    // 点光源 SSBO（set 0, binding 1）——无上界动态数组，解除编译期数量上限。
    // 从 LightParams 收集全部点光源，转为 GPU 布局（2 个 vec4）一次性上传。
    // 无光源时分配 1 字节占位避免空缓冲；shader 循环 0 次不受影响。
    std::vector<LightGPU> lights;
    lights.reserve(m_LightParams.pointLights.size());
    for (const auto &pl : m_LightParams.pointLights) {
        lights.push_back(LightGPU{glm::vec4(pl.position, pl.radiusInv), pl.color});
    }
    BufferAllocation lightBuffer = frame.AllocateBuffer(
        vk::BufferUsageFlagBits::eStorageBuffer,
        lights.empty() ? 1 : lights.size() * sizeof(LightGPU));
    if (!lights.empty()) {
        lightBuffer.update(lights);
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

    renderInfo.Begin(vkCmd);

    // ====================================================================
    // 3b. 天空盒绘制（自包含块，先于网格，作为背景）
    // ====================================================================
    //    全屏三角形 + 反投影重建视线方向 + 采样等距柱状全景图。
    //    关闭深度测试/写入，天空盒始终位于最远背景；网格随后以 clear 后的
    //    深度（远平面）正常深度测试并覆盖。此块设置完整管线状态（含渲染
    //    格式、动态状态、视口/剪刀），随后网格配置块会重新覆盖为网格状态，
    //    两者互不干扰。
    if (m_SkyboxEnabled && m_SkyboxTexture) {
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
        cmd.BindImage(m_SkyboxTexture->GetImageView(),
                      m_SkyboxTexture->GetSampler(), 0, 1);
        cmd.Draw(3, 1, 0, 0);
    }

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
    vp.width = static_cast<float>(extent.width);
    vp.height = static_cast<float>(extent.height);
    vp.minDepth = 0.0f;
    vp.maxDepth = 1.0f;
    cmd.SetViewport(0, {vp});

    vk::Rect2D scissor;
    scissor.extent.width = extent.width;
    scissor.extent.height = extent.height;
    cmd.SetScissor(0, {scissor});

    // ── 5. 绑定 Frame UBO（set 0, binding 0，所有网格共享） ───────────
    cmd.BindBuffer(frameUboAlloc.get_buffer(), frameUboAlloc.get_offset(),
                   frameUboAlloc.get_size(), 0, 0);

    // 绑定点光源 SSBO（set 0, binding 1）——所有网格共享
    cmd.BindBuffer(lightBuffer.get_buffer(), lightBuffer.get_offset(),
                   lightBuffer.get_size(), 0, 1);

    // ── 6. 逐批次 instanced 绘制 ─────────────────────────────────────
    vk::DeviceSize vertexOffset = 0;

    // 当前绑定的管线 id（初始为 Blinn-Phong，已在上方绑定 *m_PipelineLayout）。
    // 排序键已按 pipelineId 分组，故同类型批次连续，切换频率最低。
    uint8_t currentPipelineId = 0;
    for (const auto &batch : batches) {
        // —— 管线路由：材质类型变化时切换管线布局（进而切换管线 / 片元着色器）——
        uint8_t pipelineId = GetPipelineId(batch.material);
        if (pipelineId != currentPipelineId) {
            cmd.BindPipelineLayout(
                pipelineId == 1 ? *m_PipelineLayoutPBR : *m_PipelineLayout);
            currentPipelineId = pipelineId;
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

        // 自发光贴图（set 1, binding 3）：无材质或无纹理时使用默认黑色纹理，
        // 其颜色为 (0,0,0)，加色为 0，保证材质无需自发光贴图也能正常渲染
        Texture *emissiveTex = GetEffectiveEmissiveTexture(batch.material);
        if (emissiveTex) {
            cmd.BindImage(emissiveTex->GetImageView(),
                          emissiveTex->GetSampler(),
                          1, 3);
        }

        // 金属-粗糙度贴图（set 1, binding 4）：仅 PBR 材质使用。无 MR 贴图时
        // 绑定默认 (G=1, B=1) 纹理，回退到标量 metallic/roughness。
        // Blinn-Phong 批次不绑定（其 set 1 布局无 binding 4，绑了也会被过滤，
        // 这里显式判断更清晰，避免多余绑定）。
        if (pipelineId == 1) {
            Texture *mrTex = GetEffectiveMetallicRoughnessTexture(batch.material);
            if (mrTex) {
                cmd.BindImage(mrTex->GetImageView(),
                              mrTex->GetSampler(),
                              1, 4);
            }
        }

        // 绑定材质 UBO（set 1, binding 2）：存材质标量参数。按批次写入，
        // 同批次的实例共享同一材质，故值恒定，无需 per-instance。
        //   params: shininess / specularStrength（Blinn-Phong）、emissiveStrength（共用）
        //   pbr:    metallic / roughness（PBR）
        MaterialUBO materialUBO{};
        materialUBO.params.x = batch.material
            ? batch.material->GetFloat("shininess", 32.0f)
            : 32.0f;
        materialUBO.params.y = batch.material
            ? batch.material->GetFloat("specularStrength", 0.5f)
            : 0.5f;
        materialUBO.params.z = batch.material
            ? batch.material->GetFloat("emissiveStrength", 0.0f)
            : 0.0f;
        materialUBO.pbr.x = batch.material
            ? batch.material->GetFloat("metallic", 0.0f)
            : 0.0f;
        materialUBO.pbr.y = batch.material
            ? batch.material->GetFloat("roughness", 0.5f)
            : 0.5f;
        BufferAllocation materialUboAlloc = frame.AllocateBuffer(
            vk::BufferUsageFlagBits::eUniformBuffer, sizeof(MaterialUBO));
        materialUboAlloc.update(materialUBO);
        cmd.BindBuffer(materialUboAlloc.get_buffer(), materialUboAlloc.get_offset(),
                       materialUboAlloc.get_size(), 1, 2);

        // 绑定全局实例 SSBO（set 2, binding 0）——所有批次共享同一缓冲
        cmd.BindBuffer(instanceBuffer.get_buffer(), instanceBuffer.get_offset(),
                       instanceBuffer.get_size(), 2, 0);

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

    // ── 6b. 统计 draw call 与三角形数量（draw call = 批次数量） ───────
    uint32_t triangles = 0;
    for (const auto &instance : m_Meshes) {
        triangles += instance.indexCount / 3;
    }
    Renderer::Get().AddStats3D(static_cast<uint32_t>(batches.size()), triangles);

    // 统计 instancing 批次数量（相同 mesh + 相同材质分一组），
    // 用于观察合批收益：批次数越少 → draw call 越少
    Renderer::Get().AddBatches3D(static_cast<uint32_t>(batches.size()));

    // ── 7. 结束渲染 ───────────────────────────────────────────────────
    VulkanRenderingInfo::End(vkCmd);
}

} // namespace GE
