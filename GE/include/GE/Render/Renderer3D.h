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
#include "Render/Material.h"
#include "Render/Mesh.h"

#include <glm/glm.hpp>

#include <vector>
#include <array>
#include <memory>
#include <cstdint>
#include <functional>

namespace GE {

class VulkanPipelineLayout;
class VulkanShaderModule;

/**
 * @brief 3D 网格渲染器。
 *
 * 支持深度测试、背面剔除、Blinn-Phong 光照（方向光 + 点光源 + 环境光）。
 * 着色器资源：
 *   Set 0, Binding 0: FrameUBO（投影、视图、相机位置、光照参数）
 *   Set 1, Binding 0: samplerColor（主纹理）
 *   Set 2, Binding 0: InstanceData（SSBO，model + color，按实例）
 */
class Renderer3D {
public:
    /// 点光源最大数量，必须与 GLSL 中的 MAX_POINT_LIGHTS 保持一致
    static constexpr size_t MAX_POINT_LIGHTS = 8;

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

    /// 排序键：按 pipeline → material → mesh → depth 顺序比较。
    struct SortKey {
        uint8_t  pipelineId = 0;  ///< 管线 id（阶段4 引入多管线后使用）
        uint64_t materialId = 0;  ///< 材质指针值（分组用）
        uint64_t meshId     = 0;  ///< mesh 指针值（分组用）
        uint32_t depthBits  = 0;  ///< view 空间深度（正浮点 IEEE 位模式）

        /// 按优先级从高到低比较，供 std::sort 使用。
        bool operator<(const SortKey &o) const {
            if (pipelineId != o.pipelineId) return pipelineId < o.pipelineId;
            if (materialId != o.materialId) return materialId < o.materialId;
            if (meshId != o.meshId) return meshId < o.meshId;
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

        // 点光源数组（最多 MAX_POINT_LIGHTS 个）
        std::array<PointLight, MAX_POINT_LIGHTS> pointLights{}; ///< 点光源数组
        size_t pointLightCount = 1;                             ///< 实际使用的点光源数量（默认 1 个）

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
        glm::vec4 pointLightPositions[MAX_POINT_LIGHTS]; ///< 点光源位置(xyz) + 半径倒数(w)
        glm::vec4 pointLightColors[MAX_POINT_LIGHTS];    ///< 点光源颜色(rgb) + 强度(a)
        glm::vec4 pointLightCount;                    ///< x = 实际点光源数量，yzw 填充对齐
        glm::vec4 ambient;                            ///< 环境光颜色(rgb) + 强度(a)
    };
    static_assert(sizeof(FrameUBO) % 16 == 0, "FrameUBO 必须 16 字节对齐");

    /// per-instance 数据（阶段3，存入 SSBO，std430 布局）
    /// 必须与 GLSL InstanceData 块一致：mat4(64B) + vec4(16B) = 80B
    struct InstanceData {
        glm::mat4 model;             ///< 模型矩阵（列主序）
        glm::vec4 color;             ///< 叠加颜色（tint），与纹理颜色相乘
    };
    static_assert(sizeof(InstanceData) == 80, "InstanceData 必须与 std430 布局一致");

    /// 一个待绘制的网格实例
    struct MeshInstance {
        glm::mat4 transform;   ///< 模型变换矩阵
        Mesh     *mesh;        ///< 网格资源
        Material *material;    ///< 材质（可为 nullptr，nullptr 时使用白色 fallback）
        glm::vec4 color;       ///< 叠加颜色
        SortKey   sortKey;     ///< 排序键（EndScene 绘制前按此排序）
    };

    /// 阶段3：一个 instancing 绘制批次（相同 mesh + 相同材质）
    struct RenderBatch {
        Mesh     *mesh;          ///< 网格资源
        Material *material;      ///< 材质
        uint32_t  firstInstance; ///< 该批次在全局实例缓冲中的起始实例索引
        uint32_t  instanceCount; ///< 实例数量
    };

    // ========================================================================
    // 内部工具方法
    // ========================================================================

    /**
     * @brief 计算某个网格实例的排序键。
     *
     * 由当前视图矩阵、变换矩阵、材质和网格计算：
     * pipeline（恒 0）→ 材质 → mesh → view 空间深度。
     *
     * @param material 材质（可为 nullptr，nullptr 时材质 id 为 0）
     * @param mesh     网格（用于排序分组，使同材质同 mesh 的实例连续）
     * @param transform 模型变换矩阵
     */
    SortKey ComputeSortKey(const Material *material, const Mesh *mesh, const glm::mat4 &transform) const;

    /**
     * @brief 解析材质对应的有效纹理。
     *
     * 优先取材质 Albedo 槽位纹理，无材质或无纹理时回退到默认白色纹理。
     */
    Texture *GetEffectiveTexture(const Material *material) const;

    // ========================================================================
    // 成员
    // ========================================================================

    /// 网格顶点着色器（由全局资源缓存管理，不拥有）
    VulkanShaderModule   *m_VertShader = nullptr;

    /// 网格片元着色器（由全局资源缓存管理，不拥有）
    VulkanShaderModule   *m_FragShader = nullptr;

    /// Pipeline layout（由全局资源缓存管理，不拥有）
    VulkanPipelineLayout *m_PipelineLayout = nullptr;

    /// 默认 1x1 白色纹理（无纹理时的 fallback）
    std::unique_ptr<Texture> m_DefaultWhiteTexture;

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

    /// 是否在 BeginScene / EndScene 之间
    bool m_InScene = false;
};

} // namespace GE
