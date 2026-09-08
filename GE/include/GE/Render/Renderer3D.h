/**
 * @file Renderer3D.h
 * @brief 3D 网格渲染器。
 *
 * 提供基于 Blinn-Phong 光照模型的 3D 网格绘制接口，
 * 使用 BeginScene / DrawMesh / EndScene 三段式 API。
 *
 * 相同 mesh + 相同材质的多个实例会合并为单个 vkCmdDrawIndexedInstanced
 * （阶段3 instancing）；per-instance 数据（model + color）存入 SSBO，
 * 用 gl_InstanceIndex 索引。EndScene 绘制前会按排序键（材质 → mesh → 深度）
 * 排序，使可合批的实例连续。
 * 顶点数据使用 Mesh 自身的 GPU 缓冲，SSBO 从当前帧 BufferPool 动态分配。
 *
 * 使用方式：
 * @code
 *   auto& renderer3d = Renderer::Get3DRenderer();
 *   renderer3d.BeginScene(view, projection, viewPos, clearColor);
 *   renderer3d.DrawMesh(transform, mesh, material, color);
 *   renderer3d.EndScene();
 * @endcode
 */

#pragma once

#include "Core/Base.h"
#include "Render/BufferPool.h"
#include "Render/EnvironmentMap.h"
#include "Render/Material.h"
#include "Render/Mesh.h"
#include "Render/ShadowCascade.h"

#include <glm/glm.hpp>

#include <vector>
#include <array>
#include <memory>
#include <cstdint>
#include <functional>
#include <unordered_map>
#include <algorithm>

namespace GE {

class VulkanPipelineLayout;
class VulkanShaderModule;
class VulkanCommandBuffer;
class VulkanRenderFrame;
class VulkanSampler;
struct PassExecuteContext;
struct WaterComponent;

/**
 * @brief 3D 网格渲染器。
 *
 * 支持深度测试、背面剔除、Blinn-Phong 光照（方向光 + 点光源 + 环境光）。
 * 着色器资源：
 *   Set 0, Binding 0: FrameUBO（投影、视图、相机位置、光照参数）
 *   Set 1, Binding 0/1/3: samplerColor（主纹理）/ samplerNormal（法线贴图）/ samplerEmissive（自发光贴图）
 *   Set 1, Binding 2: MaterialUBO（材质标量参数，如 shininess，按批次绑定）
 *   Set 2, Binding 0: InstanceData（SSBO，model + color，按实例）
 *   Set 2, Binding 1: JointBuffer（SSBO，每个皮肤一段连续关节矩阵，仅蒙皮肤管线使用）
 */
class Renderer3D {
public:
    // ========================================================================
    // 排序键（阶段1：按材质排序；阶段3：加入 mesh 分组以便 instancing；
    //          阶段4：加入 pass 分区，打通透明渲染）
    // ========================================================================
    //
    // 用 struct 而非位打包整数，彻底消除位预算限制：
    //   pass → pipeline → material → mesh → depth
    // - pass：0=不透明（MASK/Opaque），1=透明（Blend）。必须作为最高优先语义
    //   分区——不透明段先画（写深度、近→远吃 early-z），透明段后画（混合、
    //   深度写关、远→近）。若把 pipeline 提到 pass 之前，带蒙皮位的高 id
    //   （pipe2/3）会混进静态（pipe0）的透明段之后，切分点之后整段被透明
    //   管线绘制 → 蒙皮不透明物体混合开、深度写关，永远盖在透明物上方
    //   （透明材质 alpha≈1 = 全覆盖）。pass 在最高位保证两段都是干净连续前缀。
    // - pipeline：管线切换最贵（材质 PBR 位 + 蒙皮位）。段内才按管线分组，
    //   减少切换；Opaque/Mask 同为不透明段、天然共享同一混合附件状态。
    // - material：按材质分组 → 减少管线/纹理切换
    // - mesh：同材质内同 mesh 实例连续，便于 instancing 合批
    // - depth：不透明物体从前往后（early-z）；透明分区内 depthBits 取反，
    //   升序即从远到近（alpha 混合正确序）
    //
    // material/mesh 用完整指针值（进程内唯一），无需折叠、无碰撞；
    // 合批分组仍以指针相等判断。

    /// 渲染段（不透明 / 透明）。作为排序最高语义分区：透明段从不透明段
    /// 独立排序绘制，其深度方向也与不透明相反。
    enum class Pass : uint8_t {
        Opaque = 0, ///< 不透明段（Opaque + Mask，深度写、近→远 early-z）
        Transparent = 1, ///< 透明段（Blend，深度不写、远→近）
    };

    /// 排序键：按 pass → pipeline → material → mesh → submesh → depth 顺序比较。
    struct SortKey {
        uint8_t passId = 0; ///< 渲染段（0=不透明，1=透明；最高优先，保证两段连续前缀）
        uint8_t pipelineId = 0; ///< 管线 id（材质 PBR 位 + 蒙皮位；段内次高）
        uint64_t materialId = 0; ///< 材质指针值（分组用）
        uint64_t meshId = 0; ///< mesh 指针值（分组用）
        uint64_t submeshId = 0; ///< 子网格范围（firstIndex<<32 | indexCount，分组用）
        uint32_t depthBits = 0; ///< view 空间深度（不透明正序位模式；透明按位取反后仍升序排列 = 远→近）

        /// 按优先级从高到低比较，供 std::sort 使用。
        bool operator<(const SortKey &o) const {
            if (passId != o.passId)
                return passId < o.passId;
            if (pipelineId != o.pipelineId)
                return pipelineId < o.pipelineId;
            if (materialId != o.materialId)
                return materialId < o.materialId;
            if (meshId != o.meshId)
                return meshId < o.meshId;
            if (submeshId != o.submeshId)
                return submeshId < o.submeshId;
            return depthBits < o.depthBits;
        }
    };

    /**
     * @brief 单个点光源参数。
     */
    struct PointLight {
        glm::vec3 position = {0.0f, 2.0f, 0.0f}; ///< 点光源世界坐标位置
        glm::vec4 color = {1.0f, 1.0f, 1.0f, 1.0f}; ///< 点光源颜色(rgb) + 强度(a)
        float radiusInv = 0.5f; ///< 点光源半径倒数（衰减系数）
    };

    /**
     * @brief 光照参数配置。
     */
    struct LightParams {
        // 方向光
        glm::vec3 dirLightDirection = {0.0f, -1.0f, 0.0f}; ///< 方向光方向（指向光源的反方向）
        glm::vec4 dirLightColor = {1.0f, 1.0f, 1.0f, 1.0f}; ///< 方向光颜色(rgb) + 强度(a)

        // 方向光阴影
        bool castShadow = false; ///< 方向光是否投阴影（无方向光实体时为 false）

        // CSM（级联阴影映射，CSM 计划书 §4.2）：每级光矩阵与切分距离数组。
        // Scene::UpdateLightParams 每帧填充；C2 逐级 ShadowMap pass、C3 Lighting 级联
        // 采样消费。cascadeCount = 1 时 cascadeViewProj[0] 退化为全视锥，与现状单级一致
        // （§4.2 兼容回退）。
        std::array<glm::mat4, kMaxCascades> cascadeViewProj{}; ///< 每级光空间 view-proj（世界 → 该级光裁剪空间）
        std::array<float, kMaxCascades> cascadeSplits{}; ///< 每级远端切分距离（cascadeSplits[c] = 第 c 级远端；split[0]=near，末级=far）
        uint32_t cascadeCount = 3; ///< 生效级数（默认 3；1 = 现状单级回退；上限 kMaxCascades）
        float cascadeSplitLambda = 0.5f; ///< practical split 混合系数（0 = 均匀、1 = 对数，默认 0.5）

        // 点光源数组（存入 SSBO 无编译期上限，按实际数量上传）
        std::vector<PointLight> pointLights{1}; ///< 点光源数组（默认 1 个）

        // 环境光
        glm::vec4 ambient = {0.3f, 0.3f, 0.3f, 1.0f}; ///< 环境光颜色(rgb) + 强度(a)
    };

    /// 构造：初始化着色器、pipeline layout。
    Renderer3D();

    ~Renderer3D();

    Renderer3D(const Renderer3D &) = delete;

    Renderer3D &operator=(const Renderer3D &) = delete;

    Renderer3D(Renderer3D &&) = delete;

    Renderer3D &operator=(Renderer3D &&) = delete;

    // ========================================================================
    // 光照参数
    // ========================================================================

    /// 获取光照参数（可修改引用）。
    LightParams &GetLightParams() { return m_LightParams; }

    /// 设置光照参数。
    void SetLightParams(const LightParams &params) { m_LightParams = params; }

    /// 设置方向光阴影贴图尺寸（像素，阶段 1 取 2048）。
    void SetShadowMapSize(uint32_t size) { m_ShadowMapSize = size; }

    /// 当前方向光阴影贴图尺寸（像素）。
    uint32_t GetShadowMapSize() const { return m_ShadowMapSize; }

    /// 第 cascade 级阴影图尺寸（像素）：级 0 = 现状单级尺寸（m_ShadowMapSize，Lighting
    /// 采样的主图，保持现有分辨率/观感）；级 1.. 默认 2048（CSM 计划书 §4.4，独立可配）。
    uint32_t GetCascadeShadowSize(uint32_t cascade) const {
        return (cascade == 0) ? m_ShadowMapSize : m_CascadeShadowSize[cascade];
    }

    /// 设置第 cascade 级阴影图尺寸（像素，2 的幂）：级 0 即现状单级尺寸（同步
    /// m_ShadowMapSize，Lighting shadowParams.x 的 texel 尺寸随之一致）；级 1.. 写
    /// m_CascadeShadowSize。越界级号忽略。
    void SetCascadeShadowSize(uint32_t cascade, uint32_t size) {
        if (cascade == 0) {
            m_ShadowMapSize = size;
        } else if (cascade < kMaxCascades) {
            m_CascadeShadowSize[cascade] = size;
        }
    }

    /// 设置 CSM 生效级数（1..kMaxCascades；1 = 现状单级回退）。改动下帧经
    /// Scene::UpdateLightParams 重算各级切分/矩阵、SceneLayer 重声明 N 个 pass 生效。
    void SetCascadeCount(uint32_t count) {
        m_LightParams.cascadeCount = glm::clamp(count, 1u, kMaxCascades);
    }

    /// 当前 CSM 生效级数。
    uint32_t GetCascadeCount() const { return m_LightParams.cascadeCount; }

    /// 设置 practical split 混合系数（0 = 均匀、1 = 对数，默认 0.5；CSM 计划书 §4.1）。
    void SetCascadeSplitLambda(float lambda) {
        m_LightParams.cascadeSplitLambda = glm::clamp(lambda, 0.0f, 1.0f);
    }

    /// 当前 practical split 混合系数。
    float GetCascadeSplitLambda() const { return m_LightParams.cascadeSplitLambda; }

    /// 设置方向光阴影深度偏差（常量偏差，缓解自阴影花斑；各级共享，每级独立值列 §7）。
    void SetShadowBias(float bias) { m_ShadowBias = bias; }

    /// 当前方向光阴影深度偏差。
    float GetShadowBias() const { return m_ShadowBias; }

    /// 设置方向光阴影 PCF 半径（3×3 盒式核的采样半径，像素；各级共享，每级独立列 §7）。
    void SetShadowPcfRadius(float radius) { m_ShadowPcfRadius = radius; }

    /// 当前方向光阴影 PCF 半径。
    float GetShadowPcfRadius() const { return m_ShadowPcfRadius; }

    // ========================================================================
    // 天空盒
    // ========================================================================

    /// 运行时开关天空盒（false 时不再绘制天空盒）。
    void SetSkyboxEnabled(bool enabled) { m_SkyboxEnabled = enabled; }

    /// 当前天空盒是否启用。
    bool IsSkyboxEnabled() const { return m_SkyboxEnabled; }

    // ========================================================================
    // 环境映射（IBL）
    // ========================================================================

    /**
     * @brief 设置环境映射（IBL），渲染器取得所有权。
     *
     * 传 nullptr 时禁用 IBL，PBR 材质回退常量环境光（向后兼容）。
     * 传非空时启用 split-sum IBL：PBR 管线路由到 IBL 变体并绑定三张 IBL 图。
     *
     * @note 旧环境不立即销毁：其 ImageView 可能仍被上一帧 descriptor set 引用，
     *       立即销毁会触发 VUID-vkDestroyImageView-imageView-01026。改为延迟到
     *       下一帧 EndScene 的安全点（描述符池已重置 + GPU 空闲）再销毁。
     */
    void SetEnvironmentMap(EnvironmentMap *env);

    /// 当前环境映射（IBL）是否可用。
    bool HasEnvironmentMap() const { return m_EnvironmentMap != nullptr; }

    /// 开关 IBL 光照（与 HasEnvironmentMap 共同决定是否走 IBL 变体）。
    void SetIBLEnabled(bool enabled) { m_IBLEnabled = enabled; }

    /// IBL 光照是否启用。
    bool IsIBLEnabled() const { return m_IBLEnabled; }

    /// 设置 IBL 环境光强度（整体缩放 diffuse + specular 的 IBL 贡献，1.0 = 原样）。
    void SetIBLIntensity(float intensity) { m_IBLIntensity = intensity; }

    /// 当前 IBL 环境光强度。
    float GetIBLIntensity() const { return m_IBLIntensity; }

    /**
     * @brief 按环境名设置环境（天空盒 + IBL 三张图）。
     *
     * 环境名对应 assets/environments/<Name>/ 子文件夹，内部按命名约定加载
     * skybox.ktx2 / prefilter.ktx / brdf_lut.png。环境名未变时跳过（避免
     * 每帧重建）；失败时清空缓存以便下次重试。
     */
    void SetEnvironment(const std::string &name);

    // ========================================================================
    // 场景接口
    // ========================================================================

    /**
     * @brief 开始 3D 场景，清空绘制队列并设置相机和光照参数。
     *
     * @param view        视图矩阵
     * @param projection  投影矩阵
     * @param viewPos     相机世界坐标位置（用于光照计算）
     * @param clearColor  清屏颜色。传入非负值则在 EndScene 时清屏；
     *                    传负值则不清屏，叠加在已有渲染结果上。
     */
    void BeginScene(const glm::mat4 &view,
                    const glm::mat4 &projection,
                    const glm::vec3 &viewPos,
                    const glm::vec4 &clearColor = glm::vec4(-1.0f));

    /**
     * @brief 提交一个 3D 网格。
     *
     * @param transform  模型变换矩阵
     * @param mesh       网格资源（不能为空）
     * @param material   材质（可为 nullptr，nullptr 时使用纯白色 fallback）
     * @param color      叠加颜色（tint），默认白色；与材质纹理颜色相乘
     */
    void DrawMesh(const glm::mat4 &transform,
                  Mesh *mesh,
                  Material *material,
                  const glm::vec4 &color = {1.0f, 1.0f, 1.0f, 1.0f});

    /**
     * @brief 提交一个 3D 子网格（网格的索引子范围）。
     *
     * 用于多子网格网格（OBJ 按材质拆分 / glTF 按 primitive 拆分）的逐子网格绘制。
     * 顶点缓冲共享，仅绘制 submesh 划定的索引范围。
     *
     * @param transform  模型变换矩阵
     * @param mesh       网格资源（不能为空）
     * @param submesh    子网格（firstIndex / indexCount 划定索引范围）
     * @param material   材质（可为 nullptr，nullptr 时使用纯白色 fallback）
     * @param color      叠加颜色（tint），默认白色
     */
    void DrawSubMesh(const glm::mat4 &transform,
                     Mesh *mesh,
                     const SubMesh &submesh,
                     Material *material,
                     const glm::vec4 &color = {1.0f, 1.0f, 1.0f, 1.0f});

    /**
     * @brief 提交一个被皮肤驱动的 3D 子网格（蒙皮管线，顶点着色器做骨骼加权变形）。
     *
     * 与 DrawSubMesh 唯一区别是 skinKey：非空（nullptr 之外）时本实例标记为
     * 蒙皮，EndScene 路由到 mesh_skinned 管线并绑定该皮肤的关节矩阵 SSBO（set 2,
     * binding 1），顶点按 JOINTS/WEIGHTS 加权变形。skinKey 是共享皮肤定义
     * （SkinDef*）的指针；同一条皮肤被多 node 引用时共用同一值、同一块缓冲。
     *
     * @param transform  模型变换矩阵
     * @param mesh       网格资源（不能为空）
     * @param submesh    子网格（firstIndex / indexCount 划定索引范围）
     * @param material   材质（可为 nullptr，nullptr 时使用纯白色 fallback）
     * @param color      叠加颜色（tint），默认白色
     * @param skinKey    共享皮肤定义指针（SkinDef*，须配合 Scene 侧 SetSkinJointBuffer
     *                   注册的关节矩阵缓冲；nullptr = 退化为静态路径）
     */
    void DrawSkinnedSubMesh(const glm::mat4 &transform,
                            Mesh *mesh,
                            const SubMesh &submesh,
                            Material *material,
                            const glm::vec4 &color,
                            const void *skinKey);

    /**
     * @brief 提交一个 3D 子网格到某级阴影专用集合（阴影剔除计划书 §4.4 / CSM 计划书 §4.3）。
     *
     * 参数语义与 DrawSubMesh 一致，命中入 m_ShadowMeshes[cascade] 而非 m_Meshes，供
     * 对应级 ShadowMap pass 单独成批（阴影集合含主视锥外物体，实例缓冲与主集合分离）。
     * 由 Scene 逐级遍历按该级阴影世界 AABB 剔除后提交（cascade = 命中级号）。
     */
    void DrawShadowSubMesh(const glm::mat4 &transform,
                           Mesh *mesh,
                           const SubMesh &submesh,
                           Material *material,
                           const glm::vec4 &color = {1.0f, 1.0f, 1.0f, 1.0f},
                           uint32_t cascade = 0);
    /**
     * @brief 提交一个水面实例（阶段 1：Gerstner 波 + IBL 反射）。
     *
     * 与网格不同，水面不挂 MeshRendererComponent，而是实体带 WaterComponent 时
     * 由 Scene 生成水格网格并调用本方法。实例进入透明段（BLEND、深度写关、
     * 远→近），在标准网格透明段之后绘制。
     *
     * @param transform 模型变换矩阵（通常来自 TransformComponent.worldMatrix）
     * @param mesh      水格网格（MeshManager::CreateWaterGrid 生成；不能为空）
     * @param water     水面组件参数（仅拷贝，不持有实体引用）
     */
    void DrawWater(const glm::mat4 &transform,
                   Mesh *mesh,
                   const WaterComponent &water);

    /**
     * @brief 提交一个被皮肤驱动的 3D 子网格到某级阴影专用集合（阴影剔除计划书 §4.4）。
     *
     * 蒙皮实体绑定盒追不上变形，阴影遍历与主遍历一致跳过剔除、一律提交（§4.3）。
     * 其余语义同 DrawShadowSubMesh / DrawSkinnedSubMesh。
     */
    void DrawShadowSkinnedSubMesh(const glm::mat4 &transform,
                                  Mesh *mesh,
                                  const SubMesh &submesh,
                                  Material *material,
                                  const glm::vec4 &color,
                                  const void *skinKey,
                                  uint32_t cascade = 0);

    /**
     * @brief 注册本帧一个皮肤的关节矩阵 SSBO（EndScene 绑定用）。
     *
     * Scene 每帧在 BeginScene 之后、EndScene 之前调用，为本帧存在的每个
     * SkinDef 提交其关节矩阵分配（由 UpdateSkins 计算上传，同皮肤多 node 共享）。
     * 同一皮肤的多个子网格绘制共享同一块缓冲，按 skinKey（SkinDef*）寻址。
     */
    void SetSkinJointBuffer(const void *skinKey, const BufferAllocation &jointBuffer);

    /// 清空本帧皮肤关节矩阵注册表（BeginScene 时自动调用，Scene 亦可显式重置）。
    void ResetSkinJointBuffers();

    /**
     * @brief 结束本帧采集：EndScene 只结束采集（批次保留在 m_Meshes），
     * 不录制任何命令。命令录制延后到 RenderGraph Scene3D pass 的 execute 回调
     * 里调用 FlushScene 完成（动态渲染已由图打开）。
     */
    void EndScene();

    /**
     * @brief 把本帧已采集的网格批次录制到指定 cmd（RenderGraph execute 回调内调用）。
     *
     * 前提：① EndScene 已收集批次；② 图已为该 pass 打开动态渲染（本方法不再
     * begin/end，也不做任何布局转换）。目标附件格式/深度/extent 均取自 execute
     * 上下文 ctx（本 pass 已由 RenderGraph 打开的实际附件），不再依赖调用方预置
     * 渲染目标。内部完成排序、退休环境销毁、帧池上传与全部绘制命令录制。
     */
    void FlushScene(PassExecuteContext &ctx);

    /// 切换延迟渲染路径。
    void SetDeferred(bool enabled) { m_Deferred = enabled; }

    /// 当前是否走延迟渲染路径。
    bool IsDeferred() const { return m_Deferred; }

    /// 切换 Tonemap 开关（仅延迟 HDR 链生效；false 时 Tonemap pass 直接透传 HDR，不做 ACES）。
    void SetTonemapEnabled(bool enabled) { m_TonemapEnabled = enabled; }

    /// 当前 Tonemap 是否启用。
    bool IsTonemapEnabled() const { return m_TonemapEnabled; }

    /// 设置曝光系数（仅延迟 HDR 链的 Tonemap 生效；1.0 = 不改亮度）。
    void SetExposure(float exposure) { m_Exposure = exposure; }

    /// 当前曝光系数。
    float GetExposure() const { return m_Exposure; }

    // ========================================================================
    // Bloom（延迟 HDR 链：Transparent → Bloom → Tonemap）
    // ========================================================================

    /// Bloom mip 链可用的最大级数（SceneLayer 按此预留虚拟资源数组）。
    static constexpr uint32_t kMaxBloomMipLevels = 6;

    /// 开关 Bloom（仅延迟 HDR 链生效）。
    void SetBloomEnabled(bool enabled) { m_BloomEnabled = enabled; }

    /// 当前 Bloom 是否启用。
    bool IsBloomEnabled() const { return m_BloomEnabled; }

    /// 设置 Bloom 阈值（高于该 HDR 亮度的像素才被提取泛光）。
    void SetBloomThreshold(float threshold) { m_BloomThreshold = threshold; }

    /// 当前 Bloom 阈值。
    float GetBloomThreshold() const { return m_BloomThreshold; }

    /// 设置 Bloom 强度（合成时乘到泛光上，1.0 = 原样）。
    void SetBloomIntensity(float intensity) { m_BloomIntensity = intensity; }

    /// 当前 Bloom 强度。
    float GetBloomIntensity() const { return m_BloomIntensity; }

    /// 设置 Bloom mip 级数（1 = 只有半分辨率提取，级数越多光晕越柔）。
    void SetBloomMipLevels(uint32_t levels) {
        m_BloomMipLevels = std::clamp(levels, 1u, kMaxBloomMipLevels);
    }

    /// 当前 Bloom mip 级数。
    uint32_t GetBloomMipLevels() const { return m_BloomMipLevels; }

    /// 录制某级 ShadowMap pass（只画该级体积内的不透明段深度，零颜色附件 + 深度附件）。
    /// cascade 指定级号：画 m_ShadowBatches[cascade] + m_ShadowInstanceBuffer[cascade]，
    /// FrameUBO.view = cascadeViewProj[cascade]；视口 = 该级深度图尺寸（pass renderArea）。
    void FlushShadow(PassExecuteContext &ctx, uint32_t cascade);

    /// 录制 GBuffer pass（MRT 输出 + 写入深度）。
    void FlushGBuffer(PassExecuteContext &ctx);

    /// 录制 Lighting pass（采样 GBuffer，输出线性 HDR 到 Scene_HDR）。
    void FlushLighting(PassExecuteContext &ctx);

    /// 录制 Tonemap pass（采样 Scene_HDR，曝光 + ACES 后写入视口颜色）。
    void FlushTonemap(PassExecuteContext &ctx);

    /// 录制 Bloom 阈值提取 pass（读 Scene_HDR，写半分辨率 Bloom_Down0）。
    void FlushBloomExtract(PassExecuteContext &ctx);

    /// 录制 Bloom 降采样 pass（读 Bloom_Down[m-1]，写 Bloom_Down[m]）。
    void FlushBloomDownsample(PassExecuteContext &ctx, uint32_t mip);

    /// 录制 Bloom 升采样/模糊 pass（读小层 + 同尺寸粗层，写 Bloom_Up[m]）。
    void FlushBloomUpsample(PassExecuteContext &ctx, uint32_t mip);

    /// 录制 Bloom 合成 pass（读 Scene_HDR + 最终泛光层，写回 Scene_HDR）。
    void FlushBloomComposite(PassExecuteContext &ctx);

    /// 录制 Transparent pass（透明对象仍走前向 alpha 混合）。
    void FlushTransparent(PassExecuteContext &ctx);

private:
    // ========================================================================
    // UBO 结构体（std140 布局，16 字节对齐）
    // ========================================================================

    /// 帧级 UBO（每帧一个，所有网格共享）
    struct FrameUBO {
        glm::mat4 projection; ///< 投影矩阵
        glm::mat4 view; ///< 视图矩阵
        glm::vec4 viewPos; ///< 相机位置（xyz, w 未用）
        glm::vec4 dirLightDirection; ///< 方向光方向（xyz, w 未用）
        glm::vec4 dirLightColor; ///< 方向光颜色(rgb) + 强度(a)
        glm::vec4 lightCount; ///< x = 点光源数量，yzw 填充对齐（点光源本体在 SSBO）
        glm::vec4 ambient; ///< 环境光颜色(rgb) + 强度(a)
        glm::vec4 iblParams; ///< x = 预滤波最大 mip 数（MAX_REFLECTION_LOD），yzw 预留
    };

    static_assert(sizeof(FrameUBO) % 16 == 0, "FrameUBO 必须 16 字节对齐");

    /// 延迟渲染 Lighting UBO（std140 布局，set 0 binding 0）
    /// 除前向共享光照字段外，额外携带反投影矩阵与背景色。
    struct LightingUBO {
        glm::mat4 invView; ///< 视图矩阵逆（天空盒方向用）
        glm::mat4 view;    ///< 相机视图矩阵（CSM 选档用 world -> view）
        glm::mat4 invProj; ///< 投影矩阵逆（NDC -> 视空间）
        glm::vec4 clearColor; ///< 天空盒未启用时的背景色
        glm::vec4 flags; ///< x = skyboxEnabled，y = IBL 开关
        glm::vec4 viewPos; ///< 相机位置（xyz, w 未用）
        glm::vec4 dirLightDirection; ///< 方向光方向（xyz, w 未用）
        glm::vec4 dirLightColor; ///< 方向光颜色(rgb) + 强度(a)
        glm::vec4 lightCount; ///< x = 点光源数量
        glm::vec4 ambient; ///< 环境光颜色(rgb) + 强度(a)
        glm::vec4 iblParams; ///< x = 预滤波最大 mip 数（MAX_REFLECTION_LOD），yzw 预留
        // CSM 级联（C3）：std140 下 mat4[kMaxCascades] 每级 64B 连续、无额外填充，
        // 与 GLSL 块同步（CSM 计划书 §4.5）。
        std::array<glm::mat4, kMaxCascades> cascadeViewProj; ///< 每级光空间 view-proj（世界 → 该级光裁剪空间）
        glm::vec4 cascadeSplits; ///< 每级远端切分距离（x/y/z/w = split[0..3]，cascadeCount 之后作废）
        glm::vec4 cascadeParams; ///< x = 生效级数，yz 预留
        glm::vec4 shadowParams; ///< x = 级 0 阴影图尺寸（像素），y = 偏差，z = 阴影开关(0/1)，w = PCF 半径
    };

    static_assert(sizeof(LightingUBO) % 16 == 0, "LightingUBO 必须 16 字节对齐");

    /// HDR Tonemap UBO（std140 布局，set 0 binding 0）。
    /// exposure.x = 曝光系数（当前固定 1.0，后续接入相机/场景曝光）；
    /// flags.x = tonemap 开关，flags.y = 天空 alpha 旗标开关。
    struct TonemapUBO {
        glm::vec4 exposure; ///< x = 曝光系数，yzw 预留
        glm::vec4 flags;    ///< x = tonemap 开关，y = 天空旗标开关，zw 预留
    };

    static_assert(sizeof(TonemapUBO) % 16 == 0, "TonemapUBO 必须 16 字节对齐");

    /// HDR Bloom UBO（std140 布局，set 0 binding 0）。
    /// params.x = threshold，y = intensity，z = 启用(>0.5)，w 预留；
    /// texelSize.x/y = 当前 pass 使用的源/目标 mip 的 1/尺寸。
    struct BloomUBO {
        glm::vec4 params;    ///< x = threshold, y = intensity, z = enabled, w 预留
        glm::vec4 texelSize; ///< x,y = 1 / mip 尺寸
    };

    static_assert(sizeof(BloomUBO) % 16 == 0, "BloomUBO 必须 16 字节对齐");

    /// per-instance 数据（阶段3，存入 SSBO，std430 布局）
    /// 必须与 GLSL InstanceData 块一致：mat4(64B) + vec4(16B) = 80B。
    /// 材质标量参数（shininess 等）已迁入 per-material UBO（MaterialUBO），
    /// 不再在此冗余存储，同时消除了 mat4 16B 对齐产生的 12B/实例 浪费。
    struct InstanceData {
        glm::mat4 model; ///< 模型矩阵（列主序）
        glm::vec4 color; ///< 叠加颜色（tint），与纹理颜色相乘
    };

    static_assert(sizeof(InstanceData) == 80, "InstanceData 必须与 std430 布局一致");

    /// 点光源 GPU 布局（与 GLSL PointLight 一致：2 个 vec4 = 32 字节）。
    /// 存入 set 0, binding 1 的光源 SSBO（无上界动态数组，解除编译期数量上限）。
    /// position.xyz = 世界位置，position.w = 半径倒数；color.rgb = 颜色，color.a = 强度。
    struct LightGPU {
        glm::vec4 position; ///< xyz = 世界位置，w = 半径倒数
        glm::vec4 color; ///< rgb = 颜色，a = 强度
    };

    static_assert(sizeof(LightGPU) == 32, "LightGPU 必须与 GLSL PointLight 布局一致");

    /// 每材质 UBO（std140 布局，set 1 binding 2，按批次绑定）
    /// 存放材质标量参数：
    ///   params.x = shininess（高光指数，Blinn-Phong 使用）
    ///   params.y = specularStrength（镜面强度，Blinn-Phong 使用）
    ///   params.w = uvTiling（纹理平铺 / UV 缩放密度，采样前乘 UV）
    ///   pbr.x = metallic，pbr.y = roughness（PBR 材质使用）
    ///   emissiveFactor.rgb = 自发光颜色因子（乘自发光贴图颜色）
    struct MaterialUBO {
        glm::vec4 params; ///< x = shininess，y = specularStrength，z 预留，w = uvTiling
        glm::vec4 pbr; ///< x = metallic，y = roughness（金属-粗糙度）
        glm::vec4 emissiveFactor; ///< rgb = 自发光颜色因子 [R,G,B]，w 预留
    };

    static_assert(sizeof(MaterialUBO) == 48, "MaterialUBO 必须 16 字节对齐");
    /// 水面 UBO（std140 布局，set 0 binding 2，按批次绑定）
    ///   model = 水格模型矩阵
    ///   timeParams = x:时间, y:法线强度, z:粗糙度, w:折射强度
    ///   deepColor / shallowColor = 深/浅水色
    ///   sizeParams = x,y:水面尺寸, z:法线平铺, w:反射强度
    ///   foamParams = 阶段 2 岸线预留（FoamDistance/FoamIntensity/AbsorptionDepth/TimeScale）
    ///   waves[i] = x,y:传播方向, z:振幅, w:波长；waveSpeeds[i].x = 相速度
    struct WaterUBO {
        glm::mat4 model{1.0f};
        glm::vec4 timeParams{0.0f};
        glm::vec4 deepColor{0.0f};
        glm::vec4 shallowColor{0.0f};
        glm::vec4 sizeParams{0.0f};
        glm::vec4 foamParams{0.0f};
        std::array<glm::vec4, 4> waves{};
        std::array<glm::vec4, 4> waveSpeeds{};
    };

    static_assert(sizeof(WaterUBO) % 16 == 0, "WaterUBO 必须 16 字节对齐");

    /// 天空盒 UBO（std140 布局，set 0 binding 0）
    /// 只存反投影所需矩阵：仅旋转视图矩阵逆（invView）+ 投影矩阵逆（invProj）。
    /// 2 个 mat4 = 128 字节，16 字节对齐。
    struct SkyboxUBO {
        glm::mat4 invView; ///< 仅旋转部分视图矩阵的逆（视方向 → 世界方向）
        glm::mat4 invProj; ///< 投影矩阵的逆（NDC → 视空间视线）
    };

    static_assert(sizeof(SkyboxUBO) == 128, "SkyboxUBO 必须 16 字节对齐");

    /// 一个待绘制的网格实例（可指向网格的某个子网格范围）
    struct MeshInstance {
        glm::mat4 transform; ///< 模型变换矩阵
        Mesh *mesh; ///< 网格资源
        uint32_t firstIndex; ///< 索引缓冲起始（元素索引）
        uint32_t indexCount; ///< 索引数量
        Material *material; ///< 材质（可为 nullptr，nullptr 时使用白色 fallback）
        glm::vec4 color; ///< 叠加颜色
        SortKey sortKey; ///< 排序键（EndScene 绘制前按此排序）
        const void *skinKey = nullptr; ///< 共享皮肤定义指针（SkinDef*，nullptr = 静态网格，走非蒙皮管线）
    };

    /// 阶段3：一个 instancing 绘制批次（相同 mesh + 相同子网格 + 相同材质）
    struct RenderBatch {
        Mesh *mesh; ///< 网格资源
        uint32_t firstIndex; ///< 索引缓冲起始（元素索引）
        uint32_t indexCount; ///< 索引数量
        Material *material; ///< 材质
        uint32_t firstInstance; ///< 该批次在全局实例缓冲中的起始实例索引
        uint32_t instanceCount; ///< 实例数量
        const void *skinKey = nullptr; ///< 共享皮肤定义指针（SkinDef*，nullptr = 静态）
    };
    /// 单个 Gerstner 波参数。
    struct WaterGerstnerWave {
        glm::vec2 direction{1.0f, 0.0f};
        float amplitude = 0.5f;
        float wavelength = 8.0f;
        float speed = 1.2f;
    };

    /// 一个水面实例（阶段 1）。存储在渲染器内部，与 ECS 组件解耦。
    struct WaterBatch {
        glm::mat4 transform{1.0f};
        Mesh *mesh = nullptr;
        uint32_t firstIndex = 0;
        uint32_t indexCount = 0;
        glm::vec2 size{40.0f, 40.0f};   ///< 水格世界尺寸（XZ）
        std::array<WaterGerstnerWave, 4> waves{};
        float timeScale = 1.0f;
        glm::vec3 deepColor{0.012f, 0.055f, 0.09f};
        glm::vec3 shallowColor{0.05f, 0.30f, 0.38f};
        Texture *normalMap = nullptr;
        float normalTiling = 4.0f;
        float normalStrength = 0.55f;
        float roughness = 0.12f;
        float reflectionStrength = 0.85f;
        float refractionStrength = 0.25f;
        float absorptionDepth = 2.0f;
        float foamDistance = 0.8f;
        float foamIntensity = 0.9f;
    };

    // ========================================================================
    // 内部工具方法
    // ========================================================================

    // ---------- EndScene 子步骤（拆分大函数，按渲染阶段） ----------

    /// 按排序键排序网格实例（材质 → mesh → 子网格 → 深度），使可合批实例连续。
    /// 主集合（m_Meshes）与阴影集合（m_ShadowMeshes）各调一次。
    void SortMeshes(std::vector<MeshInstance> &meshes);

    /// 分配并上传帧级 UBO（投影 / 视图 / 相机位置 / 光照 / IBL 参数）
    BufferAllocation UploadFrameUBO(VulkanRenderFrame &frame);

    /// 单趟扫描把 (mesh, 子网格, material) 相等且连续的实例归成批次，
    /// 同时收集每个实例的 per-instance 数据（model + color）。meshes 为源实例列表
    /// （主/阴影集合通用）。
    void CollectBatches(const std::vector<MeshInstance> &meshes,
                        std::vector<InstanceData> &instances,
                        std::vector<RenderBatch> &batches) const;

    /// 分配并上传全局 per-instance SSBO（所有批次共享同一缓冲）
    BufferAllocation UploadInstanceBuffer(VulkanRenderFrame &frame,
                                          const std::vector<InstanceData> &instances);

    /// 分配并上传点光源 SSBO（无光源时分配 1 字节占位避免空缓冲）
    BufferAllocation UploadLightBuffer(VulkanRenderFrame &frame);

    /// 分配并上传延迟 Lighting UBO（反投影矩阵 + 光照参数 + 背景色）
    BufferAllocation UploadLightingUBO(VulkanRenderFrame &frame);

    /// 分配并上传 Tonemap UBO（曝光 + 开关旗标）
    BufferAllocation UploadTonemapUBO(VulkanRenderFrame &frame);

    /// 分配并上传 Bloom UBO（阈值 / 强度 / 开关 / mip texel 尺寸）
    BufferAllocation UploadBloomUBO(VulkanRenderFrame &frame, vk::Extent2D extent);

    /**
     * @brief 计算并缓存本帧延迟链批次（幂等）：排序 + 切不透明段 + 上传缓冲。
     *
     * ShadowMap 在 GBuffer 之前执行，需先算好批次；GBuffer/Transparent 直接复用
     * 缓存，避免各自重排 m_Meshes 造成批次不一致。m_HasDeferredBatches 标记已算过。
     */
    void PrepareDeferredBatches(VulkanRenderFrame &frame);

    /// 上传某级阴影 pass 专用 FrameUBO（projection=单位阵、view=该级光空间 view-proj）。
    BufferAllocation UploadShadowFrameUBO(VulkanRenderFrame &frame, uint32_t cascade);

    /**
     * @brief 录制本帧网格批次的公共绘制段（排序 + 上传 + 绘制）。
     *
     * FlushScene（渲染图 execute 回调）调用；动态渲染已由图打开，本方法不再
     * begin/end、不做布局转换。附件格式/深度/extent 取自 execute 上下文，
     * 本方法不再依赖渲染目标 override。
     */
    void RecordScene(VulkanCommandBuffer &cmd, VulkanRenderFrame &frame,
                     vk::Format colorFormat, vk::Format depthFormat,
                     vk::Extent2D extent);

    /// 绘制天空盒（全屏三角形背景，关闭深度测试/写入，先于网格）
    void DrawSkybox(VulkanCommandBuffer &cmd, VulkanRenderFrame &frame,
                    vk::Format colorFormat, vk::Format depthFormat,
                    vk::Extent2D extent);

    /**
     * @brief 配置网格管线状态（附件格式 / 顶点输入 / 光栅化 / 动态状态 / 视口剪刀）。
     *
     * @param transparent 是否为透明（alpha 混合）管线：
     *                    true  → 启用混合（SRC_ALPHA / ONE_MINUS_SRC_ALPHA）且深度写关
     *                    false → 不透明（= 现状：混合关 + 深度写开）
     */
    void ConfigureMeshPipeline(VulkanCommandBuffer &cmd,
                               vk::Format colorFormat, vk::Format depthFormat,
                               vk::Extent2D extent,
                               bool transparent = false,
                               bool hdrTransparent = false);

    /// 配置水面管线状态（透明混合、深度写关、顶点输入、动态状态）。
    /// 与 ConfigureMeshPipeline 类似，但绑定独立 water pipeline layout，
    /// hdrTransparent 选择 water_hdr.frag（线性 HDR 输出）。
    void ConfigureWaterPipeline(VulkanCommandBuffer &cmd,
                                vk::Format colorFormat, vk::Format depthFormat,
                                vk::Extent2D extent,
                                bool hdrTransparent);

    /// 配置 GBuffer MRT 管线状态（四个颜色附件 / 深度 / 视口剪刀）
    void ConfigureGBufferPipeline(VulkanCommandBuffer &cmd,
                                  const std::vector<vk::Format> &colorFormats,
                                  vk::Format depthFormat,
                                  vk::Extent2D extent);

    /// 配置阴影深度 pass 管线状态（零颜色附件 + 深度附件，视口 = 阴影图尺寸）
    void ConfigureShadowPipeline(VulkanCommandBuffer &cmd,
                                 vk::Format depthFormat,
                                 vk::Extent2D extent);

    /// 配置延迟 Lighting 管线状态（全屏三角形 / 单个颜色附件）
    void ConfigureLightingPipeline(VulkanCommandBuffer &cmd,
                                   vk::Format colorFormat,
                                   vk::Extent2D extent);

    /// 配置 Tonemap 管线状态（全屏三角形 / 单个颜色附件）
    void ConfigureTonemapPipeline(VulkanCommandBuffer &cmd,
                                  vk::Format colorFormat,
                                  vk::Extent2D extent);

    /// 配置 Bloom 任意 pass 的全屏三角形管线状态（layout/顶点着色器由调用方指定）。
    void ConfigureBloomFullscreenPipeline(VulkanCommandBuffer &cmd,
                                          VulkanPipelineLayout *layout,
                                          VulkanShaderModule *vert,
                                          vk::Format colorFormat,
                                          vk::Extent2D extent);

    /// 绑定网格共享描述符（Frame UBO + 点光源 SSBO，set 0）
    void BindSharedUniforms(VulkanCommandBuffer &cmd,
                            const BufferAllocation &frameUbo,
                            const BufferAllocation &lightBuffer,
                            bool bindLights = true);

    /**
     * @brief 逐批次 instanced 绘制（含管线路由 / 动态剔除 / 纹理材质绑定 / 材质 UBO）。
     *
     * @param batches 按 pass 已分割的一段批次（不透明段或透明段）。混合附件与深度
     *                写状态由调用方经 ConfigureMeshPipeline(transparent) 预置，这里
     *                只负责按批次切换管线/剔除/绑定并绘制。
     */
    void DrawMeshInstances(VulkanCommandBuffer &cmd, VulkanRenderFrame &frame,
                           const std::vector<RenderBatch> &batches,
                           const BufferAllocation &instanceBuffer,
                           bool gbuffer = false,
                           bool shadow = false,
                           bool hdrTransparent = false);

    /**
     * @brief 按 pass 模式与材质路由网格管线布局。
     *
     * 阴影 / GBuffer / 前向三种 pass 各有一套（静态 + 蒙皮）布局；前向再按
     * PBR / IBL 拆出 IBL 变体。顺序判定（非嵌套三元），避免深层嵌套难读。
     *
     * @param shadow  是否阴影深度 pass
     * @param gbuffer 是否 GBuffer pass
     * @param pbr     材质是否为 PBR
     * @param useIbl  IBL 是否启用（仅前向 PBR 相关）
     * @param skinned 是否为蒙皮批次
     * @param hdrTransparent 是否 HDR 透明合成（延迟 Transparent 在 Tonemap 前输出到 Scene_HDR）
     */
    VulkanPipelineLayout *ResolveMeshLayout(bool shadow, bool gbuffer,
                                            bool pbr, bool useIbl, bool skinned,
                                            bool hdrTransparent = false);
    /// 录制水面批次（阶段 1：逐水格单 draw）。在网格透明段之后、Tonemap 前调用，
    /// hdrTransparent 为 true 时写 Scene_HDR（water_hdr.frag），否则写前向帧缓冲（water.frag）。
    void DrawWaterBatches(VulkanCommandBuffer &cmd, VulkanRenderFrame &frame,
                          const BufferAllocation &frameUbo,
                          const BufferAllocation &lightBuffer,
                          vk::Format colorFormat, vk::Format depthFormat,
                          vk::Extent2D extent,
                          bool hdrTransparent);

    /// 统计 draw call 与三角形数量（draw call = 批次数量）
    void RecordStats(uint32_t drawCallCount);

    /**
     * @brief 提交一个网格实例（内部实现，按索引范围绘制）。
     *
     * DrawMesh / DrawSubMesh / DrawSkinnedSubMesh 均委托到此，统一走实例队列 + 排序键。
     * skinKey 非空时实例标记为蒙皮，参与蒙皮肤管线分组。
     * forShadow 为 true 时命中入 m_ShadowMeshes[cascade]（该级阴影专用集合）而非 m_Meshes。
     */
    void DrawSubMeshImpl(const glm::mat4 &transform,
                         Mesh *mesh,
                         uint32_t firstIndex,
                         uint32_t indexCount,
                         Material *material,
                         const glm::vec4 &color,
                         const void *skinKey = nullptr,
                         bool forShadow = false,
                         uint32_t cascade = 0);

    /**
     * @brief 计算某个网格实例的排序键。
     *
     * 由当前视图矩阵、变换矩阵、材质和网格计算：
     * pipeline（含蒙皮位）→ 材质 → mesh → view 空间深度。
     *
     * @param material 材质（可为 nullptr，nullptr 时材质 id 为 0）
     * @param mesh     网格（用于排序分组，使同材质同 mesh 的实例连续）
     * @param transform 模型变换矩阵
     * @param skinned  是否蒙皮（true 时 pipelineId 置蒙皮位，与其他蒙皮批次连续）
     */
    SortKey ComputeSortKey(const Material *material, const Mesh *mesh,
                           uint32_t firstIndex, uint32_t indexCount,
                           const glm::mat4 &transform, bool skinned) const;

    /**
     * @brief 解析材质对应的有效纹理。
     *
     * 优先取材质 Albedo 槽位纹理，无材质或无纹理时回退到默认白色纹理。
     */
    Texture *GetEffectiveTexture(const Material *material) const;

    /**
     * @brief 销毁上一帧已退休的环境映射（安全点：描述符池已重置 + GPU 空闲）。
     *
     * EndScene 开头调用：此时当前帧描述符池已在上方 BeginFrame 重置，上一帧
     * 引用旧环境 ImageView 的 descriptor set 已消失；再 WaitIdle 确保 GPU 完成
     * 所有引用旧环境的已提交命令，即可安全销毁退休环境。
     */
    void FlushRetiredEnvironments();

    /**
     * @brief 解析材质对应的有效法线贴图。
     *
     * 优先取材质 Normal 槽位纹理，无材质或无纹理时回退到默认
     * "平坦法线"纹理（RGB=(128,128,255)，映射回 (0,0,1)，无扰动）。
     */
    Texture *GetEffectiveNormalTexture(const Material *material) const;

    /**
     * @brief 解析材质对应的有效自发光贴图。
     *
     * 优先取材质 Emissive 槽位纹理，无材质或无纹理时回退到默认
     * 黑色纹理（RGB=(0,0,0)，即物体不发光）。
     */
    Texture *GetEffectiveEmissiveTexture(const Material *material) const;

    /**
     * @brief 解析材质对应的有效金属-粗糙度贴图。
     *
     * 优先取材质 MetallicRoughness 槽位纹理，无材质或无纹理时回退到默认
     * (G=1,B=1) 纹理，使 metallic/roughness 等于标量 pbr 系数原值。
     */
    Texture *GetEffectiveMetallicRoughnessTexture(const Material *material) const;

    /**
     * @brief 计算材质对应的管线 id。
     *
     * BlinnPhong → 0，PBR → 1。用于排序键分组与 EndScene 管线路由。
     * nullptr 材质视为 BlinnPhong（0）。
     */
    uint8_t GetPipelineId(const Material *material) const;

    // ========================================================================
    // 成员
    // ========================================================================

    /// 网格顶点着色器（由全局资源缓存管理，不拥有）
    VulkanShaderModule *m_VertShader = nullptr;

    /// 网格片元着色器（Blinn-Phong，由全局资源缓存管理，不拥有）
    VulkanShaderModule *m_FragShader = nullptr;

    /// PBR 片元着色器（Cook-Torrance，由全局资源缓存管理，不拥有）
    VulkanShaderModule *m_FragShaderPBR = nullptr;

    /// PBR-IBL 片元着色器（HAS_IBL 变体，由全局资源缓存管理，不拥有）
    VulkanShaderModule *m_FragShaderPBR_IBL = nullptr;

    /// PBR HDR 透明片元着色器（mesh_pbr_hdr.frag：线性 HDR 输出，不做 ACES）
    VulkanShaderModule *m_FragShaderPBR_HDR = nullptr;

    /// PBR-IBL HDR 透明片元着色器（mesh_pbr_ibl_hdr.frag：线性 HDR 输出，不做 ACES）
    VulkanShaderModule *m_FragShaderPBR_IBL_HDR = nullptr;
    /// 水面顶点着色器（water.vert：Gerstner 位移 + 法线重建）
    VulkanShaderModule *m_VertShaderWater = nullptr;

    /// 水面片元着色器（water.frag：ACES；water_hdr.frag：线性 HDR）
    VulkanShaderModule *m_FragShaderWater = nullptr;
    VulkanShaderModule *m_FragShaderWaterHDR = nullptr;

    /// 水面管线布局（前向 + HDR 透明各一）
    VulkanPipelineLayout *m_WaterLayout = nullptr;
    VulkanPipelineLayout *m_WaterLayoutHDR = nullptr;

    /// Blinn-Phong 管线布局（由全局资源缓存管理，不拥有）
    VulkanPipelineLayout *m_PipelineLayout = nullptr;

    /// PBR 管线布局（由全局资源缓存管理，不拥有）
    VulkanPipelineLayout *m_PipelineLayoutPBR = nullptr;

    /// PBR-IBL 管线布局（set 1 含 binding 5/6/7 的 IBL 采样器，由全局资源缓存管理，不拥有）
    VulkanPipelineLayout *m_PipelineLayoutPBR_IBL = nullptr;

    /// PBR HDR 透明管线布局（静态 + 蒙皮）
    VulkanPipelineLayout *m_PipelineLayoutPBR_HDR = nullptr;
    VulkanPipelineLayout *m_PipelineLayoutSkinnedPBR_HDR = nullptr;

    /// PBR-IBL HDR 透明管线布局（静态 + 蒙皮）
    VulkanPipelineLayout *m_PipelineLayoutPBR_IBL_HDR = nullptr;
    VulkanPipelineLayout *m_PipelineLayoutSkinnedPBR_IBL_HDR = nullptr;

    /// 蒙皮顶点着色器（mesh_skinned.vert，声明 location 4/5 + set2 binding1 关节矩阵，
    /// 由全局资源缓存管理，不拥有）
    VulkanShaderModule *m_VertShaderSkinned = nullptr;

    /// 蒙皮肤管线布局（mesh_skinned.vert + 三种片元，由全局资源缓存管理，不拥有）
    VulkanPipelineLayout *m_PipelineLayoutSkinned = nullptr;
    VulkanPipelineLayout *m_PipelineLayoutSkinnedPBR = nullptr;
    VulkanPipelineLayout *m_PipelineLayoutSkinnedPBR_IBL = nullptr;

    /// GBuffer 片元着色器（mesh_gbuffer.frag，MRT 输出，由全局资源缓存管理，不拥有）
    VulkanShaderModule *m_FragShaderGBuffer = nullptr;

    /// GBuffer 管线布局（mesh.vert/mesh_skinned.vert + mesh_gbuffer.frag，由全局资源缓存管理，不拥有）
    VulkanPipelineLayout *m_PipelineLayoutGBuffer = nullptr;
    VulkanPipelineLayout *m_PipelineLayoutSkinnedGBuffer = nullptr;

    /// 阴影深度片元着色器（depth_only.frag：只采样 Albedo + MASK discard，无颜色输出）
    VulkanShaderModule *m_FragShaderDepthOnly = nullptr;

    /// 阴影深度 pass 管线布局（mesh.vert/mesh_skinned.vert + depth_only.frag）
    VulkanPipelineLayout *m_PipelineLayoutShadow = nullptr;
    VulkanPipelineLayout *m_PipelineLayoutSkinnedShadow = nullptr;

    /// 阴影深度采样器（最近邻 + 边缘钳制；由全局资源缓存管理，不拥有）。
    /// 深度值不可线性插值，PCF 用逐 tap 硬比较，故采样器必须 nearest（§5.6）。
    VulkanSampler *m_ShadowSampler = nullptr;

    /// 延迟 Lighting 顶点/片元着色器（由全局资源缓存管理，不拥有）
    VulkanShaderModule *m_LightingVert = nullptr;
    VulkanShaderModule *m_LightingFrag = nullptr;

    /// 延迟 Lighting 管线布局（由全局资源缓存管理，不拥有）
    VulkanPipelineLayout *m_LightingLayout = nullptr;

    /// Tonemap 全屏三角形顶点/片元着色器（由全局资源缓存管理，不拥有）
    VulkanShaderModule *m_TonemapVert = nullptr;
    VulkanShaderModule *m_TonemapFrag = nullptr;

    /// Tonemap 管线布局（由全局资源缓存管理，不拥有）
    VulkanPipelineLayout *m_TonemapLayout = nullptr;

    /// Bloom 全屏三角形顶点着色器（由全局资源缓存管理，不拥有）
    VulkanShaderModule *m_BloomVert = nullptr;

    /// Bloom 各阶段片元着色器（由全局资源缓存管理，不拥有）
    VulkanShaderModule *m_BloomExtractFrag = nullptr;
    VulkanShaderModule *m_BloomDownsampleFrag = nullptr;
    VulkanShaderModule *m_BloomUpsampleFrag = nullptr;
    VulkanShaderModule *m_BloomCompositeFrag = nullptr;

    /// Bloom 各阶段管线布局（由全局资源缓存管理，不拥有）
    VulkanPipelineLayout *m_BloomExtractLayout = nullptr;
    VulkanPipelineLayout *m_BloomDownsampleLayout = nullptr;
    VulkanPipelineLayout *m_BloomUpsampleLayout = nullptr;
    VulkanPipelineLayout *m_BloomCompositeLayout = nullptr;

    /// 天空盒顶点着色器（由全局资源缓存管理，不拥有）
    VulkanShaderModule *m_SkyboxVert = nullptr;

    /// 天空盒片元着色器（由全局资源缓存管理，不拥有）
    VulkanShaderModule *m_SkyboxFrag = nullptr;

    /// 天空盒管线布局（由全局资源缓存管理，不拥有）
    VulkanPipelineLayout *m_SkyboxLayout = nullptr;

    /// 天空盒是否启用（false 时不绘制；纹理由环境图 EnvironmentMap 统一持有）
    bool m_SkyboxEnabled = false;

    /// 环境映射（IBL）资源（渲染器持有所有权；nullptr = 禁用 IBL）
    std::unique_ptr<EnvironmentMap> m_EnvironmentMap;

    /// 已退休待销毁的环境映射（延迟到 GPU 空闲 + 描述符池重置后销毁）
    std::vector<std::unique_ptr<EnvironmentMap> > m_RetiredEnvironments;

    /// 已加载环境的名称（用于 SetEnvironment 判断是否需重建）
    std::string m_EnvironmentName;

    /// IBL 光照开关（与 m_EnvironmentMap 非空共同决定是否走 IBL 变体）
    bool m_IBLEnabled = false;

    /// Tonemap 开关（仅延迟 HDR 链生效；经 TonemapUBO.flags.x 传着色器）
    bool m_TonemapEnabled = true;

    /// 曝光系数（由场景/编辑器相机每帧接入；经 TonemapUBO.exposure.x 传着色器）
    float m_Exposure = 1.0f;

    /// Bloom 开关（仅延迟 HDR 链生效；经 BloomUBO.params.z 传着色器）
    bool m_BloomEnabled = true;

    /// Bloom 阈值（高于该 HDR 亮度的像素被提取泛光）
    float m_BloomThreshold = 1.0f;

    /// Bloom 强度（合成时乘到泛光上）
    float m_BloomIntensity = 0.7f;

    /// Bloom mip 级数（1 = 只有半分辨率提取；越大光晕越柔顺）
    uint32_t m_BloomMipLevels = 5;

    /// IBL 环境光强度（缩放 IBL 贡献，经 iblParams.y 传给着色器）
    float m_IBLIntensity = 1.0f;

    /// 方向光阴影贴图尺寸（像素，阶段 1 取 4096；同时作为 CSM 级 0 的尺寸，Lighting
    /// 采样的主图，见 GetCascadeShadowSize）
    uint32_t m_ShadowMapSize = 4096;

    /// CSM 级 1.. 的阴影图尺寸（像素，默认 2048；级 0 走 m_ShadowMapSize 保持现状。
    /// 每级独立可配：近级给大分辨率、远级可小，配合 texel 密度取舍，CSM 计划书 §4.4）。
    std::array<uint32_t, kMaxCascades> m_CascadeShadowSize{2048, 2048, 2048, 2048};

    /// 方向光阴影深度偏差（常量偏差，经 shadowParams.y 传给着色器）
    float m_ShadowBias = 0.002f;

    /// 方向光阴影 PCF 半径（阶段 1 固定 3×3 盒式，半径 = 1；经 shadowParams.w 传给着色器）
    float m_ShadowPcfRadius = 1.0f;

    /// 默认 1x1 白色纹理（无纹理时的 fallback，由全局 TextureManager 持有，不拥有）
    Texture *m_DefaultWhiteTexture = nullptr;

    /// 默认 1x1 "平坦法线"纹理（无法线贴图时的 fallback，RGB=(128,128,255)）
    std::unique_ptr<Texture> m_DefaultNormalTexture;

    /// 默认 1x1 白色纹理（无自发光贴图时的 fallback，RGB=(1,1,1)，
    /// 使自发光 = emissiveFactor 颜色，即 glTF 的"仅因子发光"语义）
    std::unique_ptr<Texture> m_DefaultEmissiveTexture;

    /// 默认 1x1 金属-粗糙度纹理（无 MR 贴图时的 fallback，G=1,B=1，
    /// 使 metallic/roughness 等于标量 pbr 系数原值）
    std::unique_ptr<Texture> m_DefaultMetallicRoughnessTexture;

    /// 默认 1x1 白立方体贴图（延迟 Lighting 天空盒不可用时的 fallback）
    std::unique_ptr<Texture> m_DefaultSkyboxTexture;

    /// 当前帧视图矩阵
    glm::mat4 m_View{1.0f};

    /// 当前帧投影矩阵
    glm::mat4 m_Projection{1.0f};

    /// 当前帧相机位置
    glm::vec3 m_ViewPos{0.0f};

    /// 清屏颜色（r < 0 表示不清屏）
    glm::vec4 m_ClearColor{-1.0f};

    /// 光照参数
    LightParams m_LightParams{};

    /// 待绘制的网格列表
    std::vector<MeshInstance> m_Meshes;
    /// 待绘制的水面列表（阶段 1，独立于网格批，直接在透明段末绘制）
    std::vector<WaterBatch> m_WaterBatches;

    /// 水面动画时间（秒，每帧 BeginScene 用 steady_clock 刷新）
    float m_WaterTime = 0.0f;

    /// 每级阴影专用网格列表（Scene 逐级遍历按该级阴影世界 AABB 剔除后提交，FlushShadow
    /// 逐级单独成批；BeginScene 清空，阴影剔除计划书 §4.4 / CSM 计划书 §4.3）。
    std::array<std::vector<MeshInstance>, kMaxCascades> m_ShadowMeshes;

    /// 本帧共享皮肤定义 → 关节矩阵 SSBO 分配（Scene 在 RenderMeshes3D 注册，EndScene 绑定用）。
    /// 仅本帧有效（帧池每帧重置），BeginScene 清空。键为 SkinDef*（nullptr 无意义，不进表）。
    std::unordered_map<const void *, BufferAllocation> m_SkinJointBuffers;

    /// 是否在 BeginScene / EndScene 之间
    bool m_InScene = false;

    /// 是否走延迟渲染路径（默认关闭，保留前向路径；运行时可由 UI 切换）
    bool m_Deferred = false;

    /// 延迟三条 pass 之间缓存的本帧批次与 GPU 缓冲
    std::vector<RenderBatch> m_OpaqueBatches;
    std::vector<RenderBatch> m_TransparentBatches;
    BufferAllocation m_CachedFrameUBO;
    BufferAllocation m_CachedLightBuffer;
    BufferAllocation m_CachedInstanceBuffer;
    bool m_HasDeferredBatches = false;

    /// 每级阴影 pass 专用批次与实例缓冲（生命周期与 m_OpaqueBatches 一致，FlushTransparent
    /// 尾部清空）。FlushShadow(ctx, c) 画 m_ShadowBatches[c] + m_ShadowInstanceBuffer[c]；
    /// 集合由 Scene 逐级遍历按该级阴影世界 AABB 剔除后提交（阴影剔除计划书 §4.4 /
    /// CSM 计划书 §4.3）。
    std::array<std::vector<RenderBatch>, kMaxCascades> m_ShadowBatches;
    std::array<BufferAllocation, kMaxCascades> m_ShadowInstanceBuffer;
};

} // namespace GE
