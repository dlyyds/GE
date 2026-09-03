/**
 * @file ImageViewResource.h
 * @brief 外部图像的布局状态机（渲染图运行期同步的关键）。
 *
 * RenderGraph 本身按帧实例化、帧末析构，不记忆跨帧状态；但外部图像（swapchain
 * 图、SceneViewport 离屏图）的布局由上一帧的收尾决定，本帧必须知道它停在哪，
 * 才能安排首尾布局转换。
 *
 * 因此把「外部图像停在哪个布局」抽出为独立的 ImageViewResource：它跨帧存活，
 * 由渲染图在执行序结束后把 finalLayout 写回，从而让下一帧新构建的图能看到
 * 上一帧的落点。这是自研渲染图最容易漏的一环（见计划书 §4.6）。
 *
 * 该类只持裸指针，不拥有 VulkanImageView / VulkanImage；调用方保证其生命周期
 * 长于本对象（Renderer / SceneViewport 等资源持有方）。
 */

#pragma once

#include "Render/RenderGraph/RenderGraphTypes.h"
#include "Render/VulkanBase/VulkanImageView.h"

namespace GE {

/// 外部图像布局状态机（跨帧记忆布局）。
class ImageViewResource {
public:
    /// 构造：默认无跨帧记忆，帧首兜底布局 = ShaderReadOnlyOptimal（采样态）。
    explicit ImageViewResource(const std::string &name) : m_Name(name) {}

    ImageViewResource(const ImageViewResource &) = delete;
    ImageViewResource &operator=(const ImageViewResource &) = delete;

    // --- 绑定实际 view ---

    /// 绑定目标图像视图（不拥有所有权）。view 为 null 时表示「本帧不渲染到它」。
    void SetView(VulkanImageView *view) { m_View = view; }

    // --- 帧首布局 ---

    /// 设置本帧帧首布局。渲染图在图像从未被记录过（无 finalLayout 记忆）时用它兜底；
    /// 调用方把它设成该图当前的真实布局（如采样态 ShaderReadOnlyOptimal）。
    void SetInitialLayout(vk::ImageLayout layout) { m_InitialLayout = layout; }

    // --- 执行序后的收尾：跨帧记忆 ---

    /// 图在执行序末尾调用：把本帧最终布局写回，供下一帧读取。
    void RecordFinalLayout(vk::ImageLayout layout) {
        m_FinalLayout = layout;
        m_FinalLayoutValid = true;
    }

    /// 上一帧（或未进图时）停靠的布局；RecordFinalLayout 从未被调用时为 false。
    bool HasFinalLayout() const { return m_FinalLayoutValid; }
    vk::ImageLayout GetFinalLayout() const { return m_FinalLayout; }

    /// 名称（调试/错误定位）。
    const std::string &GetName() const { return m_Name; }

    // --- 目标图像访问（供屏障生成用，空 view 时返回空句柄） ---

    /// 目标图像视图（可能为 null，尚未绑定）。
    VulkanImageView *GetView() const { return m_View; }
    vk::Format GetFormat() const {
        return m_View ? m_View->get_format() : vk::Format::eUndefined;
    }

    /// 本帧初始布局。
    vk::ImageLayout GetInitialLayout() const { return m_InitialLayout; }

private:
    std::string m_Name;
    VulkanImageView *m_View = nullptr;

    /// 无跨帧记忆（从未被图记录过）时帧首兜底布局。
    vk::ImageLayout m_InitialLayout = vk::ImageLayout::eShaderReadOnlyOptimal;

    vk::ImageLayout m_FinalLayout = vk::ImageLayout::eUndefined;
    bool            m_FinalLayoutValid = false;
};

} // namespace GE
