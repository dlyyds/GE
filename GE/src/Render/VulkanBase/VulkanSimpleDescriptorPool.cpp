#include "Render/VulkanBase/VulkanSimpleDescriptorPool.h"

namespace GE {

void VulkanSimpleDescriptorPool::Init(vk::Device device, uint32_t maxSets,
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

void VulkanSimpleDescriptorPool::Cleanup() {
    if (m_Device && m_Pool) m_Device.destroyDescriptorPool(m_Pool);
    m_Pool = nullptr;
    m_Device = nullptr;
}

} // namespace GE
