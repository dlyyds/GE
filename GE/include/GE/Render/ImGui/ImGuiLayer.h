#pragma once

#include "Core/Base.h"
#include "Events/ApplicationEvent.h"
#include "Events/KeyEvent.h"
#include "Events/MouseEvent.h"

namespace GE {

class Renderer;
class Window;
class VulkanContext;
class VulkanRenderContext;
class VulkanImageView;
class VulkanCommandBuffer;

/**
 * @brief ImGui 运行时（Renderer 内部组件）。
 *
 * 归并 Renderer 后不再是 Application 的 Layer：
 * - 生命周期由 Renderer 管理（构造尾部创建，WaitIdle 后销毁）
 * - Vulkan / swapchain / 窗口句柄来自 Renderer 持有的引用
 * - 帧调度由 Renderer::EndFrame 内部驱动（Begin → UI 回调 → End）
 * - 不再参与 LayerStack 的 OnUpdate/OnEvent 遍历；输入捕获以显式查询方式暴露
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

    /// 帧结束：Render + 绘制到当前 swapchain image。Renderer::EndFrame 内调用。
    void End();

    /// swapchain 重建（resize / present mode 变更）后刷新 backend 依赖的 image 信息。
    void OnSwapchainRecreated();

    /// 是否希望拦截鼠标/键盘输入（ImGui 窗口悬停或文本输入）。
    [[nodiscard]] bool WantCaptureImGuiInput() const;

    /// 渲染本帧 ImGui UI（统计面板；Renderer 把宿主 Layer 的 UI 回调挂在帧驱动上）。
    void OnImGuiRender();

private:
    void SetDarkThemeColors();

    Renderer &m_Renderer; ///< 资源来源（Vulkan 上下文 / swapchain / 每帧 cmd）
};

} // namespace GE
