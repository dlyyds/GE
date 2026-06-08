#pragma once

#include "glm/glm.hpp"
#include <string>

#include "Render/VulkanBase/VulkanPipeline.h"
#include "Render/VulkanBase/VulkanBuffer.h"
#include "Render/VulkanBase/VulkanRingBuffer.h"
#include "Render/Mesh.h"
#include "Render/Material.h"
#include "Render/VulkanBase/VulkanSwapchain.h"
#include "Render/VulkanBase/VulkanInstance.h"
#include "Render/VulkanBase/VulkanDevice.h"

namespace GE {

class Window;

/// 基于实例的 2D/3D 网格渲染器单例。
/// 拥有 VulkanInstance、VulkanDevice、swapchain 和环形 UBO buffer。
/// 管线和 descriptor set 存在于 Material 中，每个材质一套。
class Renderer2D {
public:
    static Renderer2D &Get();

    /// 完整初始化：instance → surface → device → swapchain → 渲染资源。
    void Init(Window &window);

    /// 完整销毁。
    void Shutdown();

    void BeginScene(const glm::mat4 &view, const glm::mat4 &projection,
                    const glm::vec3 &view_pos,
                    const glm::vec4 &clear_color = {0.01f, 0.01f, 0.033f, 1.0f});

    void EndScene();

    /// 用指定材质绘制网格。
    void Draw(const Mesh &mesh, const Material &material, const glm::mat4 &model, const glm::vec4 &color);

    /// 工厂方法：创建默认管线（MeshVertex 布局，UBO+纹理 descriptor）。
    VulkanPipeline CreateDefaultPipeline(vk::Device dev, vk::Format color_format);

    /// 每个 swapchain image 的 ring buffer 大小（4 MB，约 16000 次 Draw）
    static constexpr vk::DeviceSize RING_BUFFER_SIZE = 4 * 1024 * 1024;

    /// 返回当前帧的 UBO descriptor 信息，用于更新 Material。
    [[nodiscard]] vk::DescriptorBufferInfo GetUniformBufferInfo(uint32_t imageIndex) const {
        return vk::DescriptorBufferInfo{
            .buffer = m_RingBuffers[imageIndex].GetBuffer(),
            .offset = 0,
            .range = sizeof(UniformData),
        };
    }

    /// 返回所有 swapchain image 的 UBO descriptor 信息数组（给 Material::Init 用）。
    [[nodiscard]] std::vector<vk::DescriptorBufferInfo> GetUniformBufferInfos() const {
        std::vector<vk::DescriptorBufferInfo> infos(m_RingBuffers.size());
        for (uint32_t i = 0; i < m_RingBuffers.size(); i++)
            infos[i] = GetUniformBufferInfo(i);
        return infos;
    }

    [[nodiscard]] uint32_t GetSwapchainImageCount() const { return static_cast<uint32_t>(m_RingBuffers.size()); }

    [[nodiscard]] VulkanSwapchain &GetSwapchain() { return m_Swapchain; }

    // -- Forwarding accessors --
    [[nodiscard]] VulkanInstance &GetInstance() { return m_Instance; }
    [[nodiscard]] VulkanDevice &GetDevice() { return m_Device; }
    [[nodiscard]] vk::Instance GetVkInstance() const { return m_Instance.Get(); }
    [[nodiscard]] vk::Device GetVkDevice() const { return m_Device.GetDevice(); }
    [[nodiscard]] vk::PhysicalDevice GetVkGpu() const { return m_Device.GetGpu(); }
    [[nodiscard]] vk::Queue GetVkQueue() const { return m_Device.GetQueue(); }
    [[nodiscard]] int32_t GetGraphicsQueueIndex() const { return m_Device.GetGraphicsQueueIndex(); }
    [[nodiscard]] VmaAllocator GetVmaAllocator() const { return m_Device.GetVmaAllocator(); }

private:
    Renderer2D() = default;

    ~Renderer2D() = default;

    Renderer2D(const Renderer2D &) = delete;

    Renderer2D &operator=(const Renderer2D &) = delete;

    struct UniformData {
        glm::mat4 projection;
        glm::mat4 view;
        glm::mat4 model;
        glm::vec4 viewPos;
        float lodBias;
        float _padding[3];
    };

    VulkanInstance m_Instance;
    VulkanDevice m_Device;

    vk::Device m_VkDevice = nullptr;

    std::vector<VulkanRingBuffer> m_RingBuffers;
    VulkanSwapchain m_Swapchain;

    // Scene state
    vk::CommandBuffer m_ActiveCmd{nullptr};
    glm::mat4 m_View{1.0f};
    glm::mat4 m_Projection{1.0f};
    glm::vec3 m_ViewPos{0.0f};
};

} // namespace GE
