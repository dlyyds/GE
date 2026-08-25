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
#include "Render/Material.h"
#include "Render/Mesh.h"
#include "Render/EnvironmentMap.h"

#include "entt.hpp"

#include <glm/glm.hpp>

#include <vector>
#include <array>
#include <memory>
#include <cstdint>
#include <functional>
#include <unordered_map>

namespace GE {

class VulkanPipelineLayout;
class VulkanShaderModule;
class VulkanCommandBuffer;
class VulkanRenderFrame;
class RenderTarget;

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
    // 排序键（阶段1：按材质排序；阶段3：加入 mesh 分组以便 instancing）
    // ========================================================================
    //
    // 用 struct 而非位打包整数，彻底消除位预算限制：
    //   pipeline → material → mesh → depth
    // - pipeline 优先级最高：管线切换最贵（当前仅一套恒 0，阶段4 引入
    //   多管线后填入真实 id）
    // - material：按材质分组 → 减少管线/纹理切换
    // - mesh：同材质内同 mesh 实例连续，便于 instancing 合批
    // - depth：不透明物体从前往后（early-z 优化）
    //
    // material/mesh 用完整指针值（进程内唯一），无需折叠、无碰撞；
    // 合批分组仍以指针相等判断。

    /// 排序键：按 pipeline → material → mesh → submesh → depth 顺序比较。
    struct SortKey {
        uint8_t  pipelineId = 0;  ///< 管线 id（阶段4 引入多管线后使用）
        uint64_t materialId = 0;  ///< 材质指针值（分组用）
        uint64_t meshId     = 0;  ///< mesh 指针值（分组用）
        uint64_t submeshId  = 0;  ///< 子网格范围（firstIndex<<32 | indexCount，分组用）
        uint32_t depthBits  = 0;  ///< view 空间深度（正浮点 IEEE 位模式）

        /// 按优先级从高到低比较，供 std::sort 使用。
        bool operator<(const SortKey &o) const {
            if (pipelineId != o.pipelineId) return pipelineId < o.pipelineId;
            if (materialId != o.materialId) return materialId < o.materialId;
            if (meshId != o.meshId) return meshId < o.meshId;
            if (submeshId != o.submeshId) return submeshId < o.submeshId;
            return depthBits < o.depthBits;
        }
    };

    /**
     * @brief 单个点光源参数。
     */
    struct PointLight {
        glm::vec3 position    = {0.0f, 2.0f, 0.0f};           ///< 点光源世界坐标位置
        glm::vec4 color       = {1.0f, 1.0f, 1.0f, 1.0f};     ///< 点光源颜色(rgb) + 强度(a)
        float     radiusInv   = 0.5f;                          ///< 点光源半径倒数（衰减系数）
    };

    /**
     * @brief 光照参数配置。
     */
    struct LightParams {
        // 方向光
        glm::vec3 dirLightDirection = {0.0f, -1.0f, 0.0f};     ///< 方向光方向（指向光源的反方向）
        glm::vec4 dirLightColor     = {1.0f, 1.0f, 1.0f, 1.0f}; ///< 方向光颜色(rgb) + 强度(a)

        // 点光源数组（存入 SSBO 无编译期上限，按实际数量上传）
        std::vector<PointLight> pointLights{1}; ///< 点光源数组（默认 1 个）

        // 环境光
        glm::vec4 ambient = {0.3f, 0.3f, 0.3f, 1.0f};          ///< 环境光颜色(rgb) + 强度(a)
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

    /**
     * @brief 设置本次 EndScene 的渲染目标。
     *
     * 传 nullptr（默认）时渲染到当前帧的 swapchain 目标；传非空时渲染到
     * 指定的离屏目标（如把 3D 场景渲染进 ImGui 视口窗口）。
     * 每次 BeginScene 前设置，EndScene 后建议复位为 nullptr。
     *
     * @param target 渲染目标指针（不持有所有权），nullptr = 渲染到 swapchain
     */
    void SetRenderTarget(RenderTarget *target) { m_RenderTargetOverride = target; }

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
     * 与 DrawSubMesh 唯一区别是 skinKey：非空（entt::null 之外）时本实例标记为
     * 蒙皮，EndScene 路由到 mesh_skinned 管线并绑定该皮肤的关节矩阵 SSBO（set 2,
     * binding 1），顶点按 JOINTS/WEIGHTS 加权变形。
     *
     * @param transform  模型变换矩阵
     * @param mesh       网格资源（不能为空）
     * @param submesh    子网格（firstIndex / indexCount 划定索引范围）
     * @param material   材质（可为 nullptr，nullptr 时使用纯白色 fallback）
     * @param color      叠加颜色（tint），默认白色
     * @param skinKey    驱动本子网格的皮肤实体句柄（须配合 Scene 侧 SetSkinJointBuffer
     *                   注册的关节矩阵缓冲；entt::null = 退化为静态路径）
     */
    void DrawSkinnedSubMesh(const glm::mat4 &transform,
                            Mesh *mesh,
                            const SubMesh &submesh,
                            Material *material,
                            const glm::vec4 &color,
                            entt::entity skinKey);

    /**
     * @brief 注册本帧一个皮肤的关节矩阵 SSBO（EndScene 绑定用）。
     *
     * Scene 每帧在 BeginScene 之后、EndScene 之前调用，为本帧存在的每个
     * SkinComponent 提交其关节矩阵分配（由 UpdateSkins 计算上传）。同一皮肤
     * 的多个子网格绘制共享同一块缓冲，按 skinKey 寻址。
     */
    void SetSkinJointBuffer(entt::entity skinKey, const BufferAllocation &jointBuffer);

    /// 清空本帧皮肤关节矩阵注册表（BeginScene 时自动调用，Scene 亦可显式重置）。
    void ResetSkinJointBuffers();

    /**
     * @brief 结束场景：将所有提交的网格提交到 GPU 绘制。
     *
     * 内部流程：
     * 1. 从帧资源池分配 Frame UBO 和 Object UBO
     * 2. 开始动态渲染（颜色 + 深度附件）
     * 3. 设置管线状态（深度测试、背面剔除等）
     * 4. 逐个绑定网格并绘制
     */
    void EndScene();

private:
    // ========================================================================
    // UBO 结构体（std140 布局，16 字节对齐）
    // ========================================================================

    /// 帧级 UBO（每帧一个，所有网格共享）
    struct FrameUBO {
        glm::mat4 projection;                         ///< 投影矩阵
        glm::mat4 view;                               ///< 视图矩阵
        glm::vec4 viewPos;                            ///< 相机位置（xyz, w 未用）
        glm::vec4 dirLightDirection;                  ///< 方向光方向（xyz, w 未用）
        glm::vec4 dirLightColor;                      ///< 方向光颜色(rgb) + 强度(a)
        glm::vec4 lightCount;                    ///< x = 点光源数量，yzw 填充对齐（点光源本体在 SSBO）
        glm::vec4 ambient;                            ///< 环境光颜色(rgb) + 强度(a)
        glm::vec4 iblParams;                  ///< x = 预滤波最大 mip 数（MAX_REFLECTION_LOD），yzw 预留
    };
    static_assert(sizeof(FrameUBO) % 16 == 0, "FrameUBO 必须 16 字节对齐");

    /// per-instance 数据（阶段3，存入 SSBO，std430 布局）
    /// 必须与 GLSL InstanceData 块一致：mat4(64B) + vec4(16B) = 80B。
    /// 材质标量参数（shininess 等）已迁入 per-material UBO（MaterialUBO），
    /// 不再在此冗余存储，同时消除了 mat4 16B 对齐产生的 12B/实例 浪费。
    struct InstanceData {
        glm::mat4 model;             ///< 模型矩阵（列主序）
        glm::vec4 color;             ///< 叠加颜色（tint），与纹理颜色相乘
    };
    static_assert(sizeof(InstanceData) == 80, "InstanceData 必须与 std430 布局一致");

    /// 点光源 GPU 布局（与 GLSL PointLight 一致：2 个 vec4 = 32 字节）。
    /// 存入 set 0, binding 1 的光源 SSBO（无上界动态数组，解除编译期数量上限）。
    /// position.xyz = 世界位置，position.w = 半径倒数；color.rgb = 颜色，color.a = 强度。
    struct LightGPU {
        glm::vec4 position;  ///< xyz = 世界位置，w = 半径倒数
        glm::vec4 color;     ///< rgb = 颜色，a = 强度
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
        glm::vec4 params;            ///< x = shininess，y = specularStrength，z 预留，w = uvTiling
        glm::vec4 pbr;               ///< x = metallic，y = roughness（金属-粗糙度）
        glm::vec4 emissiveFactor;    ///< rgb = 自发光颜色因子 [R,G,B]，w 预留
    };
    static_assert(sizeof(MaterialUBO) == 48, "MaterialUBO 必须 16 字节对齐");

    /// 天空盒 UBO（std140 布局，set 0 binding 0）
    /// 只存反投影所需矩阵：仅旋转视图矩阵逆（invView）+ 投影矩阵逆（invProj）。
    /// 2 个 mat4 = 128 字节，16 字节对齐。
    struct SkyboxUBO {
        glm::mat4 invView;   ///< 仅旋转部分视图矩阵的逆（视方向 → 世界方向）
        glm::mat4 invProj;   ///< 投影矩阵的逆（NDC → 视空间视线）
    };
    static_assert(sizeof(SkyboxUBO) == 128, "SkyboxUBO 必须 16 字节对齐");

    /// 一个待绘制的网格实例（可指向网格的某个子网格范围）
    struct MeshInstance {
        glm::mat4 transform;   ///< 模型变换矩阵
        Mesh     *mesh;        ///< 网格资源
        uint32_t  firstIndex;  ///< 索引缓冲起始（元素索引）
        uint32_t  indexCount;  ///< 索引数量
        Material *material;    ///< 材质（可为 nullptr，nullptr 时使用白色 fallback）
        glm::vec4 color;       ///< 叠加颜色
        SortKey   sortKey;     ///< 排序键（EndScene 绘制前按此排序）
        entt::entity skinKey = entt::null; ///< 驱动本实例的皮肤实体（null = 静态网格，走非蒙皮管线）
    };

    /// 阶段3：一个 instancing 绘制批次（相同 mesh + 相同子网格 + 相同材质）
    struct RenderBatch {
        Mesh     *mesh;          ///< 网格资源
        uint32_t  firstIndex;    ///< 索引缓冲起始（元素索引）
        uint32_t  indexCount;    ///< 索引数量
        Material *material;      ///< 材质
        uint32_t  firstInstance; ///< 该批次在全局实例缓冲中的起始实例索引
        uint32_t  instanceCount; ///< 实例数量
        entt::entity skinKey = entt::null; ///< 驱动本批次的皮肤实体（null = 静态）
    };

    // ========================================================================
    // 内部工具方法
    // ========================================================================

    // ---------- EndScene 子步骤（拆分大函数，按渲染阶段） ----------

    /// 按排序键排序网格实例（材质 → mesh → 子网格 → 深度），使可合批实例连续
    void SortMeshes();

    /// 分配并上传帧级 UBO（投影 / 视图 / 相机位置 / 光照 / IBL 参数）
    BufferAllocation UploadFrameUBO(VulkanRenderFrame &frame);

    /// 单趟扫描把 (mesh, 子网格, material) 相等且连续的实例归成批次，
    /// 同时收集每个实例的 per-instance 数据（model + color）
    void CollectBatches(std::vector<InstanceData> &instances,
                        std::vector<RenderBatch> &batches) const;

    /// 分配并上传全局 per-instance SSBO（所有批次共享同一缓冲）
    BufferAllocation UploadInstanceBuffer(VulkanRenderFrame &frame,
                                          const std::vector<InstanceData> &instances);

    /// 分配并上传点光源 SSBO（无光源时分配 1 字节占位避免空缓冲）
    BufferAllocation UploadLightBuffer(VulkanRenderFrame &frame);

    /// 构建颜色 + 深度附件（按 m_ClearColor 决定 loadOp）并开始动态渲染
    void BeginDynamicRendering(VulkanCommandBuffer &cmd, RenderTarget &renderTarget);

    /// 绘制天空盒（全屏三角形背景，关闭深度测试/写入，先于网格）
    void DrawSkybox(VulkanCommandBuffer &cmd, VulkanRenderFrame &frame,
                    RenderTarget &renderTarget);

    /// 配置网格管线状态（附件格式 / 混合 / 顶点输入 / 光栅化 / 动态状态 / 视口剪刀）
    void ConfigureMeshPipeline(VulkanCommandBuffer &cmd, RenderTarget &renderTarget);

    /// 绑定网格共享描述符（Frame UBO + 点光源 SSBO，set 0）
    void BindSharedUniforms(VulkanCommandBuffer &cmd,
                            const BufferAllocation &frameUbo,
                            const BufferAllocation &lightBuffer);

    /// 逐批次 instanced 绘制（含管线路由 / 纹理材质绑定 / 材质 UBO）
    void DrawMeshInstances(VulkanCommandBuffer &cmd, VulkanRenderFrame &frame,
                           const std::vector<RenderBatch> &batches,
                           const BufferAllocation &instanceBuffer);

    /// 统计 draw call 与三角形数量（draw call = 批次数量）
    void RecordStats(const std::vector<RenderBatch> &batches);

    /**
     * @brief 提交一个网格实例（内部实现，按索引范围绘制）。
     *
     * DrawMesh / DrawSubMesh / DrawSkinnedSubMesh 均委托到此，统一走实例队列 + 排序键。
     * skinKey 非 null 时实例标记为蒙皮，参与蒙皮肤管线分组。
     */
    void DrawSubMeshImpl(const glm::mat4 &transform,
                         Mesh *mesh,
                         uint32_t firstIndex,
                         uint32_t indexCount,
                         Material *material,
                         const glm::vec4 &color,
                         entt::entity skinKey = entt::null);

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
    VulkanShaderModule   *m_VertShader = nullptr;

    /// 网格片元着色器（Blinn-Phong，由全局资源缓存管理，不拥有）
    VulkanShaderModule   *m_FragShader = nullptr;

    /// PBR 片元着色器（Cook-Torrance，由全局资源缓存管理，不拥有）
    VulkanShaderModule   *m_FragShaderPBR = nullptr;

    /// PBR-IBL 片元着色器（HAS_IBL 变体，由全局资源缓存管理，不拥有）
    VulkanShaderModule   *m_FragShaderPBR_IBL = nullptr;

    /// Blinn-Phong 管线布局（由全局资源缓存管理，不拥有）
    VulkanPipelineLayout *m_PipelineLayout = nullptr;

    /// PBR 管线布局（由全局资源缓存管理，不拥有）
    VulkanPipelineLayout *m_PipelineLayoutPBR = nullptr;

    /// PBR-IBL 管线布局（set 1 含 binding 5/6/7 的 IBL 采样器，由全局资源缓存管理，不拥有）
    VulkanPipelineLayout *m_PipelineLayoutPBR_IBL = nullptr;

    /// 蒙皮顶点着色器（mesh_skinned.vert，声明 location 4/5 + set2 binding1 关节矩阵，
    /// 由全局资源缓存管理，不拥有）
    VulkanShaderModule   *m_VertShaderSkinned = nullptr;

    /// 蒙皮肤管线布局（mesh_skinned.vert + 三种片元，由全局资源缓存管理，不拥有）
    VulkanPipelineLayout *m_PipelineLayoutSkinned = nullptr;
    VulkanPipelineLayout *m_PipelineLayoutSkinnedPBR = nullptr;
    VulkanPipelineLayout *m_PipelineLayoutSkinnedPBR_IBL = nullptr;

    /// 天空盒顶点着色器（由全局资源缓存管理，不拥有）
    VulkanShaderModule   *m_SkyboxVert = nullptr;

    /// 天空盒片元着色器（由全局资源缓存管理，不拥有）
    VulkanShaderModule   *m_SkyboxFrag = nullptr;

    /// 天空盒管线布局（由全局资源缓存管理，不拥有）
    VulkanPipelineLayout *m_SkyboxLayout = nullptr;

    /// 天空盒是否启用（false 时不绘制；纹理由环境图 EnvironmentMap 统一持有）
    bool m_SkyboxEnabled = false;

    /// 环境映射（IBL）资源（渲染器持有所有权；nullptr = 禁用 IBL）
    std::unique_ptr<EnvironmentMap> m_EnvironmentMap;

    /// 已退休待销毁的环境映射（延迟到 GPU 空闲 + 描述符池重置后销毁）
    std::vector<std::unique_ptr<EnvironmentMap>> m_RetiredEnvironments;

    /// 已加载环境的名称（用于 SetEnvironment 判断是否需重建）
    std::string m_EnvironmentName;

    /// IBL 光照开关（与 m_EnvironmentMap 非空共同决定是否走 IBL 变体）
    bool m_IBLEnabled = false;

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

    /// 当前帧视图矩阵
    glm::mat4 m_View{1.0f};

    /// 当前帧投影矩阵
    glm::mat4 m_Projection{1.0f};

    /// 当前帧相机位置
    glm::vec3 m_ViewPos{0.0f};

    /// 清屏颜色（r < 0 表示不清屏）
    glm::vec4 m_ClearColor{-1.0f};

    /// 渲染目标覆盖（nullptr 时渲染到 swapchain）。由 SetRenderTarget 设置。
    RenderTarget *m_RenderTargetOverride = nullptr;

    /// 光照参数
    LightParams m_LightParams{};

    /// 待绘制的网格列表
    std::vector<MeshInstance> m_Meshes;

    /// 本帧皮肤实体 → 关节矩阵 SSBO 分配（Scene 在 RenderMeshes1D 注册，EndScene 绑定用）。
    /// 仅本帧有效（帧池每帧重置），BeginScene 清空。
    std::unordered_map<entt::entity, BufferAllocation> m_SkinJointBuffers;

    /// 是否在 BeginScene / EndScene 之间
    bool m_InScene = false;
};

} // namespace GE
