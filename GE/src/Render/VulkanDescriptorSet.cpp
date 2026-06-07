#define VULKAN_HPP_DISPATCH_LOADER_DYNAMIC 1
#include "../../include/GE/Render/VulkanBase/VulkanDescriptorSet.h"

namespace GE {

void VulkanDescriptorSet::Init(vk::Device device, VulkanDescriptorPool &pool,
                               vk::DescriptorSetLayout layout) {
    m_Device = device;
    m_DescriptorPool = pool.Get();

    vk::DescriptorSetAllocateInfo alloc_info{
        .descriptorPool = pool.Get(),
        .descriptorSetCount = 1,
        .pSetLayouts = &layout,
    };
    std::vector<vk::DescriptorSet> descriptor_sets = device.allocateDescriptorSets(alloc_info);
    m_DescriptorSet = descriptor_sets[0];
}

void VulkanDescriptorSet::WriteBuffer(uint32_t binding, vk::DescriptorType type,
                                      vk::DescriptorBufferInfo buffer_info) {
    vk::WriteDescriptorSet write{
        .dstSet = m_DescriptorSet,
        .dstBinding = binding,
        .descriptorCount = 1,
        .descriptorType = type,
        .pBufferInfo = &buffer_info,
    };
    m_Device.updateDescriptorSets(write, nullptr);
}

void VulkanDescriptorSet::WriteImage(uint32_t binding, vk::DescriptorImageInfo image_info,
                                     vk::DescriptorType type) {
    vk::WriteDescriptorSet write{
        .dstSet = m_DescriptorSet,
        .dstBinding = binding,
        .descriptorCount = 1,
        .descriptorType = type,
        .pImageInfo = &image_info,
    };
    m_Device.updateDescriptorSets(write, nullptr);
}

void VulkanDescriptorSet::Destroy() {
    if (m_Device && m_DescriptorSet && m_DescriptorPool) {
        m_Device.freeDescriptorSets(
            m_DescriptorPool,
            m_DescriptorSet
            );
    }
    m_DescriptorPool = nullptr;
    m_DescriptorSet = nullptr;
    m_Device = nullptr;
}

} // namespace GE
