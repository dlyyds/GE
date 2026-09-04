#pragma once

#include "Core/Base.h"
#include "Render/Texture.h"

#include <glm/glm.hpp>

#include <unordered_map>
#include <vector>
#include <memory>

namespace GE {

class VulkanPipelineLayout;
class VulkanShaderModule;
class VulkanCommandBuffer;
class VulkanRenderFrame;
class RenderTarget;

/**
 * @file Renderer2D.h
 * @brief 2D 精灵批处理渲染器。
 *
 * 提供简洁的 2D 精灵绘制接口，内部按纹理做批处理：
 *   BeginScene(viewProjection) → DrawSprite(...) × N → EndScene()
 *
 * 同一张纹理的所有精灵会合并到同一个 draw call 中，
 * 顶点数据从当前帧的 BufferPool 动态分配，每帧自动回收。
 *
 * 使用方式：
 * @code
 *   auto& renderer2d = Renderer::Get2DRenderer();
 *   renderer2d.BeginScene(viewProjection);
 *   renderer2d.DrawSprite({0, 0}, {1, 1}, 0.0f, texture, {1, 1, 1, 1});
 *   renderer2d.EndScene();
 * @endcode
 */
class Renderer2D {
public:
    /// 构造：初始化着色器、pipeline layout。
    Renderer2D();

    ~Renderer2D();

    Renderer2D(const Renderer2D &) = delete;
    Renderer2D &operator=(const Renderer2D &) = delete;
    Renderer2D(Renderer2D &&) = delete;
    Renderer2D &operator=(Renderer2D &&) = delete;

    // ========================================================================
    // 场景接口
    // ========================================================================

    /**
     * @brief 开始场景，清空批处理队列并设置视图/投影矩阵。
     *
     * @param view        视图矩阵（2D 模式传单位矩阵即可）
     * @param projection  投影矩阵
     * @param useDepth    是否启用深度测试（true=3D 世界空间，false=2D 屏幕空间/UI 叠加）
     * @param clearColor  清屏颜色（r < 0 表示不清屏，叠加在已有结果上）
     */
    void BeginScene(const glm::mat4 &view,
                    const glm::mat4 &projection,
                    bool useDepth,
                    const glm::vec4 &clearColor = glm::vec4(-1.0f));

    /**
     * @brief 提交一个 2D 精灵（位置 + 尺寸 + 旋转 + 纹理 + 颜色）。
     *
     * @param position  精灵中心位置（世界坐标，xy 平面）
     * @param size      精灵尺寸（宽高）
     * @param rotation  绕 Z 轴旋转角度（弧度）
     * @param texture   精灵纹理（nullptr 则绘制纯色矩形）
     * @param color     叠加颜色（tint），默认白色
     */
    void DrawSprite(const glm::vec2 &position,
                    const glm::vec2 &size,
                    float rotation,
                    Texture *texture,
                    const glm::vec4 &color = {1.0f, 1.0f, 1.0f, 1.0f});

    /**
     * @brief 提交一个 3D 空间中的 2D 精灵（3D 位置 + 尺寸 + 旋转 + 纹理 + 颜色）。
     *
     * 精灵位于 xy 平面，朝向 +Z 方向。如需 billboard（始终面向相机），
     * 请使用 DrawBillboard。
     *
     * @param position  精灵中心位置（世界坐标，xyz）
     * @param size      精灵尺寸（宽高）
     * @param rotation  绕 Z 轴旋转角度（弧度）
     * @param texture   精灵纹理
     * @param color     叠加颜色（tint）
     */
    void DrawSprite(const glm::vec3 &position,
                    const glm::vec2 &size,
                    float rotation,
                    Texture *texture,
                    const glm::vec4 &color = {1.0f, 1.0f, 1.0f, 1.0f});

    /**
     * @brief 提交一个 2D 精灵（4×4 变换矩阵 + 纹理 + 颜色）。
     *
     * @param transform  模型变换矩阵（应用于单位四边形）
     * @param texture    精灵纹理（nullptr 则绘制纯色矩形）
     * @param color      叠加颜色（tint），默认白色
     */
    void DrawSprite(const glm::mat4 &transform,
                    Texture *texture,
                    const glm::vec4 &color = {1.0f, 1.0f, 1.0f, 1.0f});

    /**
     * @brief 结束场景：将所有批处理的精灵提交到 GPU 绘制。
     *
     * 内部流程：
     * 1. 从帧资源池分配顶点 buffer
     * 2. 写入所有精灵顶点
     * 3. 绑定 pipeline + 顶点 buffer
     * 4. 按纹理分组，每组一个 draw call
     *
     * 延迟录制模式（m_DeferRecording）下不直接提交，而是把本批精灵的
     * 采集状态快照进 m_Sessions，等待渲染图 execute 回调调用 FlushScene。
     */
    void EndScene();

    /**
     * @brief 设置延迟录制模式。
     *
     * true（编辑器视口渲染图路径）时 EndScene 只把本批精灵快照进 session，
     * 不录制命令；false（默认，Sandbox / 直写 swapchain 路径）时保持原行为
     * 直接录制。同帧多次 BeginScene/EndScene（世界精灵 + UI 精灵）各自快照
     * 一个 session，FlushScene 按序重放。
     */
    void SetDeferRecording(bool defer) { m_DeferRecording = defer; }

    /**
     * @brief 把本帧已快照的全部精灵 session 录制到指定 cmd（RenderGraph execute 回调内调用）。
     *
     * 前提：① 已 SetRenderTarget(目标)；② EndScene 已快照（defer 模式）；
     * ③ 图已为该 pass 打开动态渲染（本方法不再 begin/end，也不做任何布局转换）。
     */
    void FlushScene(VulkanCommandBuffer &cmd, VulkanRenderFrame &frame);

    /**
     * @brief 设置本次 EndScene 的渲染目标。
     *
     * 传 nullptr（默认）时渲染到当前帧的 swapchain 目标；传非空时渲染到
     * 指定的离屏目标（如与 3D 一起渲染进 ImGui 视口窗口）。
     * 每次 BeginScene 前设置，EndScene 后建议复位为 nullptr。
     *
     * @param target 渲染目标指针（不持有所有权），nullptr = 渲染到 swapchain
     */
    void SetRenderTarget(RenderTarget *target) { m_RenderTargetOverride = target; }

private:
    /// 单个精灵的 4 个顶点
    struct SpriteVertex {
        glm::vec3 position;   ///< 位置（世界坐标，xyz）
        glm::vec2 uv;         ///< 纹理坐标
        glm::vec4 color;      ///< 顶点颜色
    };

    /// 每帧 UBO 数据（std140 布局，16 字节对齐）
    struct UniformBlock {
        glm::mat4 view;         ///< 视图矩阵（2D 模式下为单位矩阵）
        glm::mat4 projection;   ///< 投影矩阵
        glm::vec4 color;        ///< 叠加颜色（白色）
    };
    static_assert(sizeof(UniformBlock) % 16 == 0, "UBO 必须 16 字节对齐");

    /// 将一个精灵的 4 个顶点追加到指定批次
    void AppendQuad(Texture *texture,
                    const glm::mat4 &transform,
                    const glm::vec4 &color);

    /// 创建 2D 渲染管线状态（在 EndScene 首次调用或状态变化时配置）
    void SetupPipelineState();

    /// 录制单个精灵 session（顶点上传 + 状态绑定 + 逐纹理绘制）
    /// @param manageRendering true=自己 begin/end 动态渲染（直录路径）；
    ///                        false=动态渲染已由图打开，只录命令（渲染图路径）
    void RecordSpriteSession(VulkanCommandBuffer &cmd, VulkanRenderFrame &frame,
                             const glm::mat4 &view, const glm::mat4 &projection,
                             bool useDepth,
                             const std::unordered_map<Texture *, std::vector<SpriteVertex>> &batches,
                             bool manageRendering);

    // ========================================================================
    // 成员
    // ========================================================================

    /// 精灵顶点着色器（由全局资源缓存管理，不拥有）
    VulkanShaderModule   *m_VertShader = nullptr;

    /// 精灵片元着色器（由全局资源缓存管理，不拥有）
    VulkanShaderModule   *m_FragShader = nullptr;

    /// Pipeline layout（由全局资源缓存管理，不拥有）
    VulkanPipelineLayout *m_PipelineLayout = nullptr;

    /// 当前帧的视图矩阵（2D 模式下为单位矩阵）
    glm::mat4             m_View{1.0f};

    /// 当前帧的投影矩阵
    glm::mat4             m_Projection{1.0f};

    /// 清屏颜色（r < 0 表示不清屏）
    glm::vec4             m_ClearColor{-1.0f};

    /// 是否启用深度测试（3D 模式开启，2D 模式关闭）
    bool                  m_UseDepth = false;

    /// 默认 1x1 白色纹理（无纹理时的 fallback，由全局 TextureManager 持有，不拥有）
    Texture *m_DefaultWhiteTexture = nullptr;

    /// 批处理队列：纹理指针 → 该纹理的所有精灵顶点
    std::unordered_map<Texture *, std::vector<SpriteVertex>> m_Batches;

    /// 是否在 BeginScene / EndScene 之间
    bool m_InScene = false;

    /// 延迟录制模式（RenderGraph 编辑器路径置 true，EndScene 快照不录制）
    bool m_DeferRecording = false;

    /// 本帧已快照的精灵 session（延迟录制：世界/UI 各一批，FlushScene 按序重放）。
    /// 每条保留该批的相机状态与批次顶点快照，因 BeginScene 会清空 m_Batches。
    struct SpriteSession {
        glm::mat4 view{1.0f};
        glm::mat4 projection{1.0f};
        bool      useDepth = false;
        std::unordered_map<Texture *, std::vector<SpriteVertex>> batches;
    };
    std::vector<SpriteSession> m_Sessions;

    /// 渲染目标覆盖（nullptr 时渲染到 swapchain）。由 SetRenderTarget 设置。
    RenderTarget *m_RenderTargetOverride = nullptr;
};

} // namespace GE
