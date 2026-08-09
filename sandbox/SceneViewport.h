#pragma once

#include "GE/Render/RenderTarget.h"
#include "GE/Render/VulkanBase/VulkanSampler.h"

#include <vulkan/vulkan.hpp>
#include <memory>
#include <cstdint>

namespace GE {

class VulkanDevice;

/**
 * @brief 场景视口：管理一个离屏渲染目标及其 ImGui 显示。
 *
 * 把 3D 场景渲染进一张离屏纹理，再通过 ImGui::Image 显示在可拖拽缩放的
 * ImGui 窗口里。图像资源（颜色图 / 深度图）由 RenderTarget 持有，本类只负责：
 *   1. 创建 / 重建离屏 RenderTarget
 *   2. 把其颜色图注册成 ImGui 可显示的图片（ImGui_ImplVulkan_AddTexture）
 *   3. 窗口缩放时重建（旧图销毁、新图重建）
 */
class SceneViewport {
public:
    SceneViewport() = default;
    ~SceneViewport();

    SceneViewport(const SceneViewport &) = delete;
    SceneViewport &operator=(const SceneViewport &) = delete;

    /// 按指定尺寸创建离屏渲染目标并注册 ImGui 图片。宽高为 0 时忽略。
    void Create(VulkanDevice &device, uint32_t width, uint32_t height);

    /// 销毁离屏目标与 ImGui 图片。
    void Destroy();

    /// 尺寸变化时重建离屏目标（无变化或宽高为 0 时跳过）。
    void OnResize(uint32_t width, uint32_t height);

    [[nodiscard]] RenderTarget *GetRenderTarget() const { return m_Target.get(); }
    [[nodiscard]] VkDescriptorSet GetImGuiDescriptorSet() const { return m_DescriptorSet; }
    [[nodiscard]] uint32_t GetWidth() const { return m_Width; }
    [[nodiscard]] uint32_t GetHeight() const { return m_Height; }

private:
    VulkanDevice *m_Device = nullptr;                       ///< 用于重建的设备引用（Create 时记录）
    std::unique_ptr<RenderTarget> m_Target;                 ///< 离屏渲染目标（持有颜色图 / 深度图）
    std::unique_ptr<VulkanSampler> m_Sampler;               ///< 采样器（供 ImGui 采样颜色图）
    VkDescriptorSet m_DescriptorSet = VK_NULL_HANDLE;       ///< ImGui 图片描述符
    uint32_t m_Width = 0, m_Height = 0;                     ///< 视口尺寸
};

} // namespace GE