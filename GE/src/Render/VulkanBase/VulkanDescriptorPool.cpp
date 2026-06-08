//
// Created by Lenovo on 2026/6/3.
//

#define VULKAN_HPP_DISPATCH_LOADER_DYNAMIC 1
#include "../../../include/GE/Render/VulkanBase/VulkanDescriptorPool.h"

namespace GE {

void VulkanDescriptorPool::Init(vk::Device device, uint32_t maxSets,
                                 std::vector<vk::DescriptorPoolSize> sizes) {
    m_Device = device;

    vk::DescriptorPoolCreateInfo info{
        .flags = vk::DescriptorPoolCreateFlagBits::eFreeDescriptorSet,
        .maxSets = maxSets,
        .poolSizeCount = static_cast<uint32_t>(sizes.size()),
        .pPoolSizes = sizes.data(),
    };
    m_Pool = device.createDescriptorPool(info);
}

void VulkanDescriptorPool::Cleanup() {
    if (m_Device && m_Pool) m_Device.destroyDescriptorPool(m_Pool);
    m_Pool = nullptr;
    m_Device = nullptr;
}

} // namespace GE
