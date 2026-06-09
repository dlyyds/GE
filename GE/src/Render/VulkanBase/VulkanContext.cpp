//
// Created by Lenovo on 2026/6/9.
//

#include "Render/VulkanBase/VulkanContext.h"
#include "Core/GEWindow.h"

namespace GE {

VulkanContext::~VulkanContext() {
    if (IsInitialized())
        Destroy();
}

void VulkanContext::Init(Window &window) {
    m_Instance.Init("GE App");
    m_Device.Init(m_Instance, window);
}

void VulkanContext::Destroy() {
    m_Device.Destroy();
    m_Instance.Destroy();
}

} // namespace GE
