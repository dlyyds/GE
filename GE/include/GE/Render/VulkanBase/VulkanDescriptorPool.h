#pragma once


#include <vulkan/vulkan.hpp>

#include <vector>

namespace GE {

class VulkanDescriptorPool {
public:
    VulkanDescriptorPool() = default;

    ~VulkanDescriptorPool() = default;

    VulkanDescriptorPool(const VulkanDescriptorPool &) = delete;

    VulkanDescriptorPool &operator=(const VulkanDescriptorPool &) = delete;

    void Init(vk::Device device, uint32_t maxSets,
              std::vector<vk::DescriptorPoolSize> sizes);

    void Cleanup();

    [[nodiscard]] vk::DescriptorPool Get() const { return m_Pool; }

private:
    vk::Device m_Device = nullptr;
    vk::DescriptorPool m_Pool = nullptr;
};

} // namespace GE
