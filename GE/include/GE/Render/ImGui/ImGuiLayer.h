#pragma once

#include "Core/Base.h"

namespace GE {

class Renderer;
class Window;
class VulkanCommandBuffer;

/**
 * @brief ImGui 运行时（Renderer 内部组件）。
 *
 * 归并 Renderer 后不再是 Application 的 Layer：
 * - 生命周期由 Renderer 管理（构造尾部创建，WaitIdle 后销毁）
 * - Vulkan / swapchain / 窗口句柄来自 Renderer 持有的引用
 * - 帧调度由 Renderer::EndFrame 内部驱动（Begin → UI 回调 → End）
 * - 仅承载 ImGui 生命周期与 backend，不携带具体业务 UI（面板归宿主 / 编辑器层）
 */
class ImGuiLayer {
public:
    /**
     * @param renderer 持有 Vulkan 上下文 / swapchain / 窗口的渲染器（引用）。
     *                 ImGuiLayer 从中获取设备、swapchain、每帧 cmd/imageView。
     */
    explicit ImGuiLayer(Renderer &renderer);

    ~ImGuiLayer();

    ImGuiLayer(const ImGuiLayer &) = delete;
    ImGuiLayer &operator=(const ImGuiLayer &) = delete;

    /// 创建 ImGui 上下文 + 字体 + Vulkan/GLFW backend。Renderer 构造尾部调用。
    void OnAttach();

    /// 销毁 ImGui context 与 backend。Renderer 析构（WaitIdle 后）调用。
    void OnDetach();

    /// 帧起始：推进 platform 与 renderer 的 new frame。Renderer::EndFrame 前调用。
    static void Begin();

    /**
     * @brief 结束 UI 构建（纯 CPU）：设置 DisplaySize + ImGui::Render()。
     *
     * 真正的命令录制(RenderDrawData)延后到 RenderGraph 的 UIPass execute 回调里
     * 由 DrawUI 完成；此方法只结束 CPU 侧帧数据生成，必须在 Execute 前调用。
     */
    void EndUI();

    /**
     * @brief 把已构建的 UI 命令裸录制到 cmd（RenderGraph UIPass execute 回调内调用）。
     *
     * 前提：区间已由图打开（颜色附件 = swapchain，loadOp/eStore 已声明），backend
     * 只裸画(RenderDrawData)，不做任何布局转换/屏障/自开区间。
     */
    static void DrawUI(VulkanCommandBuffer &cmd);

    /// swapchain 重建（resize / present mode 变更）后刷新 backend 依赖的 image 信息。
    void OnSwapchainRecreated();

private:
    void SetDarkThemeColors();

    Renderer &m_Renderer; ///< 资源来源（Vulkan 上下文 / swapchain / 每帧 cmd）
};

} // namespace GE
