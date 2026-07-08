#include "Render/VulkanBase/VulkanAllocated.h"
#include "Render/VulkanBase/VulkanDevice.h"
#include "Render/VulkanBase/PhysicalDevice.h"
#include "Core/Log.h"

#include <cstring>
#include <stdexcept>
#include <utility>

namespace GE {

// ============================================================================
// 构造函数：创建物理设备 → 初始化 → VMA
// ============================================================================

VulkanDevice::VulkanDevice(PhysicalDevice &gpu,
                           vk::SurfaceKHR surface,
                           std::unordered_map<std::string, RequestMode> const &requested_extensions,
                           const std::function<void(PhysicalDevice &)> &request_gpu_features,
                           std::unique_ptr<DebugUtils> debug_utils) :
    m_Gpu{gpu},
    m_Surface{surface},
    m_DebugUtils{debug_utils ? std::move(debug_utils) : std::make_unique<DummyDebugUtils>()} {
    Init(requested_extensions, request_gpu_features);
    InitVma();
}

// ============================================================================
// 析构函数
// ============================================================================

VulkanDevice::~VulkanDevice() {
    // 先关闭 allocated 单例（输出泄漏统计），再销毁 VMA
    allocated::shutdown();

    if (m_VmaAllocator) {
        vmaDestroyAllocator(m_VmaAllocator);
        m_VmaAllocator = nullptr;
    }

    m_GraphicsQueue = nullptr;
    if (m_Device) {
        m_Device.destroy();
        m_Device = nullptr;
    }
}

// ============================================================================
// Init — 参照 Vulkan-Samples Device::init() 模式
// ============================================================================

void VulkanDevice::Init(std::unordered_map<std::string, RequestMode> const &requested_extensions,
                        const std::function<void(PhysicalDevice &)> &request_gpu_features) {
    GE_CORE_INFO("Selected GPU: {}", m_Gpu.GetProperties().deviceName.data());

    // ---- 1. 准备所有队列族的创建信息 ----
    auto const &queue_family_properties = m_Gpu.GetQueueFamilyProperties();
    std::vector<vk::DeviceQueueCreateInfo> queue_create_infos;
    std::vector<std::vector<float> > queue_priorities;

    queue_create_infos.reserve(queue_family_properties.size());
    queue_priorities.reserve(queue_family_properties.size());

    for (uint32_t family_index = 0; family_index < queue_family_properties.size(); ++family_index) {
        auto const &qfp = queue_family_properties[family_index];

        queue_priorities.emplace_back(qfp.queueCount, 0.5f);

        // 如果启用了高优先级图形队列，将图形队列族的第一个队列设为 1.0
        if (m_Gpu.HasHighPriorityGraphicsQueue() && (qfp.queueFlags & vk::QueueFlagBits::eGraphics)) {
            queue_priorities.back()[0] = 1.0f;
        }

        queue_create_infos.push_back(vk::DeviceQueueCreateInfo{
            .queueFamilyIndex = family_index,
            .queueCount = qfp.queueCount,
            .pQueuePriorities = queue_priorities[family_index].data(),
        });
    }

    // ---- 2. 检查并启用扩展 ----
    // 2a. 基础必需扩展（Swapchain）
    std::vector<const char *> required_core_extensions = {
        VK_KHR_SWAPCHAIN_EXTENSION_NAME,
        VK_EXT_EXTENDED_DYNAMIC_STATE_EXTENSION_NAME,
    };

    // 检查必需扩展是否可用
    for (auto const &ext : required_core_extensions) {
        if (m_Gpu.IsExtensionSupported(ext)) {
            m_EnabledExtensions.emplace_back(ext);
        } else {
            throw std::runtime_error(std::string("Required device extension not available: ") + ext);
        }
    }

#if defined(VK_ENABLE_PORTABILITY)
    if (m_Gpu.IsExtensionSupported(VK_KHR_PORTABILITY_SUBSET_EXTENSION_NAME)) {
        m_EnabledExtensions.push_back(VK_KHR_PORTABILITY_SUBSET_EXTENSION_NAME);
    }
#endif

    // 2b. 可选扩展（Dedicated Allocation 等）
    bool can_get_memory_requirements = m_Gpu.IsExtensionSupported("VK_KHR_get_memory_requirements2");
    bool has_dedicated_allocation = m_Gpu.IsExtensionSupported("VK_KHR_dedicated_allocation");

    if (can_get_memory_requirements && has_dedicated_allocation) {
        m_EnabledExtensions.emplace_back("VK_KHR_get_memory_requirements2");
        m_EnabledExtensions.emplace_back("VK_KHR_dedicated_allocation");
        GE_CORE_INFO("Dedicated Allocation enabled");
    }

    // 2c. 请求的外部扩展
    for (auto const &[ext_name, is_optional] : requested_extensions) {
        if (m_Gpu.IsExtensionSupported(ext_name)) {
            // 避免重复添加
            auto already = std::ranges::find_if(m_EnabledExtensions,
                                                [ext_name](const char *enabled) { return strcmp(enabled, ext_name.c_str()) == 0; });
            if (already == m_EnabledExtensions.end()) {
                m_EnabledExtensions.push_back(ext_name.c_str());
            }
        } else if (is_optional == RequestMode::Required) {
            throw std::runtime_error(std::string("Required device extension not available: ") + ext_name);
        } else {
            GE_CORE_WARN("Optional device extension '{}' not available, some features may be disabled", ext_name);
        }
    }

    if (!m_EnabledExtensions.empty()) {
        GE_CORE_INFO("Device enabled extensions:");
        for (auto const &ext : m_EnabledExtensions) {
            GE_CORE_INFO("  \t{}", ext);
        }
    }

    // ---- 3. 调用外部特性请求回调 ----
    if (request_gpu_features) {
        request_gpu_features(m_Gpu);
    }

    // ---- 4. 创建逻辑设备 ----
    vk::DeviceCreateInfo create_info{
        .pNext = m_Gpu.GetExtensionFeatureChain(),
        .queueCreateInfoCount = static_cast<uint32_t>(queue_create_infos.size()),
        .pQueueCreateInfos = queue_create_infos.data(),
        .enabledExtensionCount = static_cast<uint32_t>(m_EnabledExtensions.size()),
        .ppEnabledExtensionNames = m_EnabledExtensions.data(),
        .pEnabledFeatures = &m_Gpu.GetRequestedFeatures(),
    };

    m_Device = m_Gpu.GetHandle().createDevice(create_info);

    VULKAN_HPP_DEFAULT_DISPATCHER.init(m_Device);

    // ---- 5. 获取图形队列 ----
    // 找第一个支持 Graphics 的队列族
    for (uint32_t family_index = 0; family_index < queue_family_properties.size(); ++family_index) {
        if (queue_family_properties[family_index].queueFlags & vk::QueueFlagBits::eGraphics) {
            m_GraphicsQueueIndex = static_cast<int32_t>(family_index);
            m_GraphicsQueue = m_Device.getQueue(family_index, 0);

            GE_CORE_INFO("Using graphics queue family index {}", family_index);
            break;
        }
    }

    if (m_GraphicsQueueIndex < 0) {
        throw std::runtime_error("No graphics queue family found on the device");
    }
}

// ============================================================================
// InitVma — VMA 分配器初始化
// ============================================================================

void VulkanDevice::InitVma() {
    VmaVulkanFunctions vk_funcs{};
    vk_funcs.vkGetInstanceProcAddr = VULKAN_HPP_DEFAULT_DISPATCHER.vkGetInstanceProcAddr;
    vk_funcs.vkGetDeviceProcAddr = VULKAN_HPP_DEFAULT_DISPATCHER.vkGetDeviceProcAddr;

    VmaAllocatorCreateInfo alloc_info{};
    alloc_info.vulkanApiVersion = VK_API_VERSION_1_3;
    alloc_info.instance = static_cast<VkInstance>(m_Gpu.GetInstance().GetHandle());
    alloc_info.physicalDevice = static_cast<VkPhysicalDevice>(m_Gpu.GetHandle());
    alloc_info.device = static_cast<VkDevice>(m_Device);
    alloc_info.pVulkanFunctions = &vk_funcs;

    VkResult result = vmaCreateAllocator(&alloc_info, &m_VmaAllocator);
    if (result != VK_SUCCESS) {
        throw std::runtime_error("Failed to create VMA allocator");
    }

    // 注册 VMA 分配器到 allocated 单例，供 Allocated 基类使用
    allocated::init(m_VmaAllocator);
}

// ============================================================================
// 扩展检查
// ============================================================================

bool VulkanDevice::IsExtensionEnabled(const char *extension) const {
    return std::ranges::find_if(m_EnabledExtensions,
                                [extension](std::string const &enabled) {
                                    return enabled == extension;
                                }) != m_EnabledExtensions.end();
}

// ============================================================================
// 等待空闲
// ============================================================================

void VulkanDevice::WaitIdle() const {
    if (m_Device) {
        m_Device.waitIdle();
    }
}

// ============================================================================
// 查找内存类型（静态工具函数）
// ============================================================================

uint32_t VulkanDevice::FindMemoryType(vk::PhysicalDevice gpu, uint32_t type_filter,
                                      vk::MemoryPropertyFlags properties) {
    vk::PhysicalDeviceMemoryProperties mem_properties = gpu.getMemoryProperties();

    for (uint32_t i = 0; i < mem_properties.memoryTypeCount; ++i) {
        if (type_filter & (1 << i)) {
            if ((mem_properties.memoryTypes[i].propertyFlags & properties) == properties) {
                return i;
            }
        }
    }

    throw std::runtime_error("Failed to find suitable memory type.");
}

} // namespace GE
