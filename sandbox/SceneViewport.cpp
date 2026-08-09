#include "SceneViewport.h"

#include "GE/Render/Renderer.h"
#include "GE/Render/VulkanBase/VulkanDevice.h"
#include "GE/Render/VulkanBase/VulkanSwapchain.h"

#include <backends/imgui_impl_vulkan.h>

namespace GE {

SceneViewport::~SceneViewport() {
    Destroy();
}

void SceneViewport::Create(VulkanDevice &device, uint32_t width, uint32_t height) {
    // 忽略尺寸为 0 的创建（窗口最小化 / 未就绪）
    if (width == 0 || height == 0) {
        return;
    }

    m_Device = &device;
    m_Width = width;
    m_Height = height;

    // 颜色格式与 swapchain 一致，便于 ImGui 采样显示
    auto colorFormat = Renderer::GetSwapchain().GetFormat();

    RenderTargetDesc desc;
    desc.extent        = vk::Extent2D{width, height};
    desc.colorFormat   = colorFormat;
    desc.enableOffscreen = true;
    desc.enableDepth   = true;

    // 离屏渲染目标：自己创建颜色图（可采样）+ 深度图，不绑定 swapchain
    m_Target = std::make_unique<RenderTarget>(device, desc, nullptr);

    // 采样器（线性过滤，供 ImGui 采样颜色图）
    m_Sampler = std::make_unique<VulkanSampler>(device);

    // 注册为 ImGui 图片。布局 GENERAL 与 RenderTarget 离屏颜色图布局一致，
    // 渲染时当颜色附件、显示时被采样，无需布局切换。
    m_DescriptorSet = ImGui_ImplVulkan_AddTexture(
        m_Sampler->GetHandle(),
        m_Target->GetColorView().GetHandle(),
        VK_IMAGE_LAYOUT_GENERAL);
}

void SceneViewport::Destroy() {
    if (m_DescriptorSet != VK_NULL_HANDLE) {
        ImGui_ImplVulkan_RemoveTexture(m_DescriptorSet);
        m_DescriptorSet = VK_NULL_HANDLE;
    }
    m_Sampler.reset();
    m_Target.reset();
}

void SceneViewport::OnResize(uint32_t width, uint32_t height) {
    // 最小化 / 未就绪时跳过
    if (width == 0 || height == 0) {
        return;
    }
    // 尺寸无变化时跳过
    if (width == m_Width && height == m_Height) {
        return;
    }

    // 需要设备重建离屏目标
    if (!m_Device) {
        return;
    }
    Destroy();
    Create(*m_Device, width, height);
}

} // namespace GE