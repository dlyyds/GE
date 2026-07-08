#pragma once

#include <vulkan/vulkan.hpp>
#include <vector>

namespace GE {

/// 简单的单池 descriptor pool 封装。
/// 创建并拥有一个 VkDescriptorPool，提供 Init / Cleanup / Get 接口。
/// 对于更复杂的多池管理，参见 VulkanDescriptorPool（改编自 Vulkan-Samples）。
class VulkanSimpleDescriptorPool {
public:
    VulkanSimpleDescriptorPool() = default;

    ~VulkanSimpleDescriptorPool() = default;

    VulkanSimpleDescriptorPool(const VulkanSimpleDescriptorPool &) = delete;

    VulkanSimpleDescriptorPool &operator=(const VulkanSimpleDescriptorPool &) = delete;

    void Init(vk::Device device, uint32_t maxSets,
              std::vector<vk::DescriptorPoolSize> sizes);

    void Cleanup();

    [[nodiscard]] vk::DescriptorPool Get() const { return m_Pool; }

private:
    vk::Device m_Device = nullptr;
    vk::DescriptorPool m_Pool = nullptr;
};

} // namespace GE
