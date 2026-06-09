//
// Created by Lenovo on 2026/6/9.
//

#include "Render/VulkanBase/VulkanContext.h"
#include "Core/GEWindow.h"

#include <stdexcept>

namespace GE {

VulkanContext::~VulkanContext() {
    if (IsInitialized())
        Destroy();
}

void VulkanContext::Init(Window &window) {
    // 1. 创建 Vulkan Instance
    m_Instance.Init("GE App");

    // 2. 从 Instance + Window 创建 Surface
    VkSurfaceKHR raw_surface = window.CreateVulkanSurface(m_Instance.Get());
    if (!raw_surface) {
        throw std::runtime_error("Failed to create window surface.");
    }
    m_Surface = raw_surface;

    // 3. 初始化 Device（传入 surface 用于队列族选择）
    m_Device.Init(m_Instance, m_Surface);
}

void VulkanContext::Destroy() {
    // 1. 销毁 Device
    m_Device.Destroy();

    // 2. 销毁 Surface
    if (m_Surface) {
        m_Instance.Get().destroySurfaceKHR(m_Surface);
        m_Surface = nullptr;
    }

    // 3. 销毁 Instance
    m_Instance.Destroy();
}

} // namespace GE
