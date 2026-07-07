#pragma once

#include <vulkan/vulkan.hpp>
#include "vk_mem_alloc.h"

#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "Render/VulkanBase/VulkanCommon.h"
#include "Render/VulkanBase/VulkanDebug.h"

namespace GE {

class PhysicalDevice;

/**
 * @brief Vulkan 逻辑设备，参照 Vulkan-Samples 的 Device 模式实现。
 *
 * 构造即创建逻辑设备，析构即销毁。
 * 生命周期由 VulkanContext 通过 unique_ptr 管理。
 * 保留 VMA 分配器集成（Vulkan-Samples 原版不使用 VMA，此处按项目需要保留）。
 */
class VulkanDevice {
public:
    /**
     * @brief 创建逻辑设备。
     * @param gpu                  已选定的物理设备（必须已构造且有效）。
     * @param surface              窗口 surface（用于 present 支持查询）。
     * @param requested_extensions 按扩展名 → 是否可选 映射的请求扩展列表。
     * @param request_gpu_features 在设备创建前调用，用于请求 GPU 扩展特性。
     * @param debug_utils          调试工具实例（DebugUtilsExt / DummyDebugUtils 等）。
     */
    VulkanDevice(PhysicalDevice &gpu,
                 vk::SurfaceKHR surface,
                 std::unordered_map<std::string, RequestMode> const &requested_extensions = {},
                 const std::function<void(PhysicalDevice &)> &request_gpu_features = {},
                 std::unique_ptr<DebugUtils> debug_utils = {});

    ~VulkanDevice();

    VulkanDevice(const VulkanDevice &) = delete;

    VulkanDevice &operator=(const VulkanDevice &) = delete;

    /// 获取 Vulkan 逻辑设备句柄。
    [[nodiscard]] vk::Device GetHandle() const { return m_Device; }

    /// 获取关联的 PhysicalDevice。
    [[nodiscard]] PhysicalDevice &GetGpu() const { return m_Gpu; }

    /// 获取图形队列。
    [[nodiscard]] vk::Queue GetQueue() const { return m_GraphicsQueue; }

    /// 获取图形队列对应的队列族索引。
    [[nodiscard]] int32_t GetGraphicsQueueIndex() const { return m_GraphicsQueueIndex; }

    /// 获取 VMA 分配器。
    [[nodiscard]] VmaAllocator GetVmaAllocator() const { return m_VmaAllocator; }

    /// 获取调试工具实例。
    [[nodiscard]] DebugUtils const &GetDebugUtils() const { return *m_DebugUtils; }

    /// 检查指定扩展是否已启用。
    [[nodiscard]] bool IsExtensionEnabled(const char *extension) const;

    /// 等待设备空闲。
    void WaitIdle() const;

    /// 在给定的物理设备上查找满足类型位掩码和内存属性要求的内存类型。
    static uint32_t FindMemoryType(vk::PhysicalDevice gpu, uint32_t type_filter,
                                   vk::MemoryPropertyFlags properties);

private:
    /// 内部初始化：队列创建、扩展检查、特性请求、设备创建、VMA 初始化。
    void Init(std::unordered_map<std::string, RequestMode> const &requested_extensions,
              const std::function<void(PhysicalDevice &)> &request_gpu_features);

    /// 初始化 VMA 分配器。
    void InitVma();

    PhysicalDevice &m_Gpu;
    vk::Device m_Device = nullptr;
    vk::SurfaceKHR m_Surface = nullptr;
    vk::Queue m_GraphicsQueue = nullptr;
    int32_t m_GraphicsQueueIndex = -1;

    std::unique_ptr<DebugUtils> m_DebugUtils;
    std::vector<const char *> m_EnabledExtensions;
    VmaAllocator m_VmaAllocator = nullptr;
};

} // namespace GE
