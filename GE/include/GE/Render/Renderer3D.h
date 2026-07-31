/**
 * @file Renderer3D.h
 * @brief 3D 网格渲染器。
 *
 * 提供基于 Blinn-Phong 光照模型的 3D 网格绘制接口，
 * 使用 BeginScene / DrawMesh / EndScene 三段式 API。
 *
 * 每个 DrawMesh 提交一个 draw call（按纹理批处理暂未实现），
 * 顶点数据使用 Mesh 自身的 GPU 缓冲，UBO 从当前帧 BufferPool 动态分配。
 *
 * 使用方式：
 * @code
 *   auto& renderer3d = Renderer::Get3DRenderer();
 *   renderer3d.BeginScene(view, projection, viewPos, clearColor);
 *   renderer3d.DrawMesh(transform, mesh, texture, color);
 *   renderer3d.EndScene();
 * @endcode
 */

#pragma once

#include "Core/Base.h"
#include "Render/Mesh.h"
#include "Render/Texture.h"

#include <glm/glm.hpp>

#include <vector>
#include <memory>

namespace GE {

class VulkanPipelineLayout;
class ShaderModule;

/**
 * @brief 3D 网格渲染器。
 *
 * 支持深度测试、背面剔除、Blinn-Phong 光照（方向光 + 点光源 + 环境光）。
 * 着色器资源：
 *   Set 0, Binding 0: FrameUBO（投影、视图、相机位置、光照参数）
 *   Set 1, Binding 0: samplerColor（主纹理）
 *   Set 2, Binding 0: ObjectUBO（模型矩阵、lodBias）
 */
class Renderer3D {
public:
    /**
     * @brief 光照参数配置。
     */
    struct LightParams {
        // 方向光
        glm::vec3 dirLightDirection = {0.0f, -1.0f, 0.0f};   ///< 方向光方向（指向光源的反方向）
        glm::vec4 dirLightColor     = {1.0f, 1.0f, 1.0f, 1.0f}; ///< 方向光颜色(rgb) + 强度(a)

        // 点光源
        glm::vec3 pointLightPosition = {0.0f, 2.0f, 0.0f};    ///< 点光源位置
        glm::vec4 pointLightColor    = {1.0f, 1.0f, 1.0f, 1.0f}; ///< 点光源颜色(rgb) + 强度(a)
        float     pointLightRadiusInv = 0.5f;                   ///< 点光源半径倒数（衰减系数）

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
     * @param texture    主纹理（nullptr 则使用纯白色）
     * @param color      叠加颜色（tint），默认白色
     */
    void DrawMesh(const glm::mat4 &transform,
                  Mesh *mesh,
                  Texture *texture,
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
        glm::mat4 projection;        ///< 投影矩阵
        glm::mat4 view;              ///< 视图矩阵
        glm::vec4 viewPos;           ///< 相机位置（xyz, w 未用）
        glm::vec4 dirLightDirection; ///< 方向光方向（xyz, w 未用）
        glm::vec4 dirLightColor;     ///< 方向光颜色(rgb) + 强度(a)
        glm::vec4 pointLightPosition;///< 点光源位置(xyz) + 半径倒数(w)
        glm::vec4 pointLightColor;   ///< 点光源颜色(rgb) + 强度(a)
        glm::vec4 ambient;           ///< 环境光颜色(rgb) + 强度(a)
    };
    static_assert(sizeof(FrameUBO) % 16 == 0, "FrameUBO 必须 16 字节对齐");

    /// 对象级 UBO（每个网格一个）
    struct ObjectUBO {
        glm::mat4 model;             ///< 模型矩阵
        float     lodBias;           ///< 纹理 LOD 偏置
        glm::vec3 _pad;              ///< 填充到 16 字节对齐
    };
    static_assert(sizeof(ObjectUBO) % 16 == 0, "ObjectUBO 必须 16 字节对齐");

    /// 一个待绘制的网格实例
    struct MeshInstance {
        glm::mat4 transform;   ///< 模型变换矩阵
        Mesh     *mesh;        ///< 网格资源
        Texture  *texture;     ///< 主纹理（可空）
        glm::vec4 color;       ///< 叠加颜色
    };

    // ========================================================================
    // 成员
    // ========================================================================

    /// 网格顶点着色器（由全局资源缓存管理，不拥有）
    ShaderModule         *m_VertShader = nullptr;

    /// 网格片元着色器（由全局资源缓存管理，不拥有）
    ShaderModule         *m_FragShader = nullptr;

    /// Pipeline layout（由全局资源缓存管理，不拥有）
    VulkanPipelineLayout *m_PipelineLayout = nullptr;

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
