#pragma once

#include <vulkan/vulkan.hpp>
#include "vk_mem_alloc.h"

#include <memory>

#include "Render/VulkanBase/VulkanInstance.h"
#include "Render/VulkanBase/VulkanDevice.h"

namespace GE {

class Window;

/// Vulkan 全局上下文，持有 Instance、Surface 和 Device。
/// 负责 Vulkan 运行时生命周期，提供所有底层 Vulkan 句柄访问。
/// 由 Application 创建和管理，渲染器/层通过引用使用它。
class VulkanContext {
public:
    VulkanContext() = default;

    ~VulkanContext();

    VulkanContext(const VulkanContext &) = delete;
    VulkanContext &operator=(const VulkanContext &) = delete;

    VulkanContext(VulkanContext &&) = delete;
    VulkanContext &operator=(VulkanContext &&) = delete;

    /// 初始化：创建 Instance → Surface → Device → VMA
    void Init(Window &window);

    /// 销毁：Device → Surface → Instance
    void Destroy();

    [[nodiscard]] bool IsInitialized() const { return m_Device.IsInitialized(); }

    // -- Vulkan 句柄访问器 --
    [[nodiscard]] VulkanInstance &GetInstance() { return *m_Instance; }
    [[nodiscard]] VulkanDevice &GetDevice() { return m_Device; }
    [[nodiscard]] vk::Instance GetVkInstance() const { return m_Instance->GetHandle(); }
    [[nodiscard]] vk::SurfaceKHR GetSurface() const { return m_Surface; }
    [[nodiscard]] vk::Device GetVkDevice() const { return m_Device.GetDevice(); }
    [[nodiscard]] vk::PhysicalDevice GetVkGpu() const { return m_Device.GetGpu(); }
    [[nodiscard]] vk::Queue GetVkQueue() const { return m_Device.GetQueue(); }
    [[nodiscard]] int32_t GetGraphicsQueueIndex() const { return m_Device.GetGraphicsQueueIndex(); }
    [[nodiscard]] VmaAllocator GetVmaAllocator() const { return m_Device.GetVmaAllocator(); }

private:
    std::unique_ptr<VulkanInstance> m_Instance;
    vk::SurfaceKHR m_Surface = nullptr;
    VulkanDevice m_Device;
    vk::DebugUtilsMessengerEXT m_DebugCallback = nullptr;

    /// 使用完整参数创建 VulkanInstance（组装平台必需扩展、debug 扩展等）
    void CreateInstance();
};

} // namespace GE
