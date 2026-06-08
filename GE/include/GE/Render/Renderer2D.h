#pragma once

#include "glm/glm.hpp"
#include <string>

#include "Render/VulkanBase/VulkanPipeline.h"
#include "Render/VulkanBase/VulkanBuffer.h"
#include "Render/Mesh.h"
#include "Render/Material.h"
#include "Render/VulkanBase/VulkanSwapchain.h"
#include "Render/VulkanBase/VulkanInstance.h"
#include "Render/VulkanBase/VulkanDevice.h"

namespace GE {

class Window;

/// Instance-based 2D/3D mesh renderer singleton.
/// Owns VulkanInstance, VulkanDevice, swapchain, and a shared UBO buffer.
/// Pipelines and descriptor sets live in Material — one per unique surface.
class Renderer2D {
public:
    static Renderer2D &Get();

    /// Full bootstrap: instance → surface → device → swapchain → renderer resources.
    void Init(Window &window);

    /// Full teardown.
    void Shutdown();

    void BeginScene(const glm::mat4 &view, const glm::mat4 &projection,
                    const glm::vec3 &view_pos,
                    const glm::vec4 &clear_color = {0.01f, 0.01f, 0.033f, 1.0f});

    void EndScene();

    /// Draw a mesh with the given material.
    void Draw(const Mesh &mesh, const Material &material, const glm::mat4 &model, const glm::vec4 &color);

    /// Factory: create a default pipeline (MeshVertex layout, UBO+texture descriptor).
    VulkanPipeline CreateDefaultPipeline(vk::Device dev, vk::Format color_format);

    /// Expose the shared UBO descriptor info so Material can bind to it.
    [[nodiscard]] vk::DescriptorBufferInfo GetUniformBufferInfo() const {
        return vk::DescriptorBufferInfo{
            .buffer = m_UniformBuffer.GetBuffer(),
            .offset = 0,
            .range = sizeof(UniformData),
        };
    }

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

    VulkanBuffer m_UniformBuffer;
    VulkanSwapchain m_Swapchain;

    // Scene state
    vk::CommandBuffer m_ActiveCmd{nullptr};
    glm::mat4 m_View{1.0f};
    glm::mat4 m_Projection{1.0f};
    glm::vec3 m_ViewPos{0.0f};
};

} // namespace GE
