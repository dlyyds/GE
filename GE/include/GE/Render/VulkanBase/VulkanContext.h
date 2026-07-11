#pragma once

#include <vulkan/vulkan.hpp>
#include "vk_mem_alloc.h"

#include <memory>
#include <string>
#include <unordered_map>

#include "Render/VulkanBase/VulkanInstance.h"
#include "Render/VulkanBase/VulkanDevice.h"
#include "Render/VulkanBase/PhysicalDevice.h"
#include "Render/VulkanBase/VulkanCommon.h"

namespace GE {

class Window;

/// Vulkan 全局上下文，持有 Instance、PhysicalDevice、Surface 和 Device。
/// 负责 Vulkan 运行时生命周期，提供所有底层 Vulkan 句柄访问。
/// 由 Application 创建和管理，渲染器/层通过引用使用它。
///
/// 扩展管理：
/// - 使用 AddInstanceExtension / AddDeviceExtension 在 Init 前注册额外扩展
/// - 引擎默认扩展（surface、swapchain 等）自动填充，用户自定义不覆盖
class VulkanContext {
public:
    explicit VulkanContext(Window &window);

    ~VulkanContext();

    VulkanContext(const VulkanContext &) = delete;
    VulkanContext &operator=(const VulkanContext &) = delete;

    VulkanContext(VulkanContext &&) = delete;
    VulkanContext &operator=(VulkanContext &&) = delete;

    /// 销毁：Device → PhysicalDevice → Surface → Instance
    void Destroy();

    [[nodiscard]] bool IsInitialized() const { return m_Device != nullptr; }

    // ========================================================================
    // 扩展管理（在 Init 前调用）
    // ========================================================================

    /// 添加 Instance 扩展。若 Init 前未设置，引擎会填入默认值。
    void AddInstanceExtension(std::string name, RequestMode mode = RequestMode::Optional);

    /// 添加 Device 扩展。若 Init 前未设置，引擎会填入默认值（swapchain 等）。
    void AddDeviceExtension(std::string name, RequestMode mode = RequestMode::Optional);

    /// 返回 Instance 扩展列表的可修改引用（可在 Init 前直接操作）。
    [[nodiscard]] std::unordered_map<std::string, RequestMode> &GetInstanceExtensions() { return m_InstanceExtensions; }

    /// 返回 Device 扩展列表的可修改引用（可在 Init 前直接操作）。
    [[nodiscard]] std::unordered_map<std::string, RequestMode> &GetDeviceExtensions() { return m_DeviceExtensions; }

    // ========================================================================
    // 句柄访问器
    // ========================================================================

    [[nodiscard]] VulkanInstance    &GetInstance() { return *m_Instance; }
    [[nodiscard]] VulkanDevice      &GetDevice() { return *m_Device; }
    [[nodiscard]] PhysicalDevice    &GetPhysicalDevice() { return *m_PhysicalDevice; }
    [[nodiscard]] vk::Instance       GetVkInstance() const { return m_Instance->GetHandle(); }
    [[nodiscard]] vk::SurfaceKHR     GetSurface() const { return m_Surface; }
    [[nodiscard]] vk::Device         GetVkDevice() const { return m_Device->GetHandle(); }
    [[nodiscard]] vk::PhysicalDevice GetVkGpu() const { return m_Device->GetGpu().GetHandle(); }
    [[nodiscard]] vk::Queue          GetVkQueue() const { return m_Device->GetQueueByFlags(vk::QueueFlagBits::eGraphics, 0).GetHandle(); }
    [[nodiscard]] uint32_t           GetGraphicsQueueIndex() const { return m_Device->GetQueueByFlags(vk::QueueFlagBits::eGraphics, 0).GetFamilyIndex(); }
    [[nodiscard]] VmaAllocator       GetVmaAllocator() const { return m_Device->GetVmaAllocator(); }

private:
    /// 填充引擎默认扩展（用户已自定义的不覆盖）。
    void ApplyDefaultExtensions();

    /// 使用完整参数创建 VulkanInstance（组装平台必需扩展、debug 扩展等）。
    std::unique_ptr<VulkanInstance> CreateInstance();

    /// 创建 VulkanDevice（配置扩展特性、DebugUtils 等）。
    std::unique_ptr<VulkanDevice> CreateDevice();

    /// 选择支持 Vulkan 1.3 的 PhysicalDevice。
    std::unique_ptr<PhysicalDevice> SelectPhysicalDevice();

    // -- 扩展列表（在 Init 前由外部和 ApplyDefaultExtensions 共同填充）--
    std::unordered_map<std::string, RequestMode> m_InstanceExtensions;
    std::unordered_map<std::string, RequestMode> m_DeviceExtensions;

    // -- Vulkan 对象 --
    std::unique_ptr<VulkanInstance>   m_Instance;
    std::unique_ptr<PhysicalDevice>   m_PhysicalDevice;
    vk::SurfaceKHR                    m_Surface = nullptr;
    std::unique_ptr<VulkanDevice>     m_Device;
    vk::DebugUtilsMessengerEXT        m_DebugCallback = nullptr;
};

} // namespace GE
