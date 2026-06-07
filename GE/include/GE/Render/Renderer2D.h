#pragma once

#include "glm/glm.hpp"
#include <string>

#include "VulkanBase/VulkanPipeline.h"
#include "VulkanBase/VulkanBuffer.h"
#include "VulkanBase/VulkanDescriptorPool.h"
#include "VulkanBase/VulkanDescriptorSet.h"
#include "Render/Mesh.h"
#include "VulkanBase/VulkanSwapchain.h"
#include "VulkanBase/VulkanInstance.h"
#include "VulkanBase/VulkanDevice.h"

namespace GE {

class Window;

/// Instance-based 2D/3D mesh renderer singleton.
/// Owns VulkanInstance, VulkanDevice, and all rendering resources.
class Renderer2D {
public:
    static Renderer2D &Get();

    /// Full bootstrap: instance → surface → device → swapchain → renderer resources.
    void Init(Window &window);

    /// Full teardown: renderer resources → swapchain → device → surface → instance.
    void Shutdown();

    void BeginScene(const glm::mat4 &view, const glm::mat4 &projection,
                    const glm::vec3 &view_pos,
                    const glm::vec4 &clear_color = {0.01f, 0.01f, 0.033f, 1.0f});
    void EndScene();

    void SetTexture(vk::ImageView image_view, vk::Sampler sampler);

    void Draw(Mesh &mesh, const glm::mat4 &model, const glm::vec4 &color);

    /// Factory: create a pipeline with default vertex layout
    /// (MeshVertex: position/uv/normal) and the given shaders.
    /// The pipeline includes a built-in descriptor set layout
    /// (binding 0 = UBO, binding 1 = combined image sampler).
    VulkanPipeline CreateDefaultPipeline(vk::Device dev, vk::Format color_format);

    [[nodiscard]] VulkanSwapchain &GetSwapchain() { return m_Swapchain; }

    // -- Forwarding accessors for VulkanInstance / VulkanDevice --
    [[nodiscard]] VulkanInstance &GetInstance() { return m_Instance; }
    [[nodiscard]] VulkanDevice  &GetDevice()   { return m_Device; }
    [[nodiscard]] vk::Instance   GetVkInstance()   const { return m_Instance.Get(); }
    [[nodiscard]] vk::Device     GetVkDevice()     const { return m_Device.GetDevice(); }
    [[nodiscard]] vk::PhysicalDevice GetVkGpu()    const { return m_Device.GetGpu(); }
    [[nodiscard]] vk::Queue      GetVkQueue()      const { return m_Device.GetQueue(); }
    [[nodiscard]] int32_t        GetGraphicsQueueIndex() const { return m_Device.GetGraphicsQueueIndex(); }

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

    // Declaration order = destruction order: Instance outlives Device.
    VulkanInstance m_Instance;
    VulkanDevice  m_Device;

    vk::Device m_VkDevice = nullptr;

    VulkanPipeline m_Pipeline;
    VulkanBuffer m_UniformBuffer;
    VulkanDescriptorPool m_DescriptorPool;
    VulkanDescriptorSet m_DescriptorSet;
    VulkanSwapchain m_Swapchain;

    // Scene state
    vk::CommandBuffer m_ActiveCmd{nullptr};
    glm::mat4 m_View{1.0f};
    glm::mat4 m_Projection{1.0f};
    glm::vec3 m_ViewPos{0.0f};
};

} // namespace GE
