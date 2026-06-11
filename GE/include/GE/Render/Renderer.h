#pragma once

#include "glm/glm.hpp"
#include <string>

#include "Render/VulkanBase/VulkanPipeline.h"
#include "Render/VulkanBase/VulkanBuffer.h"
#include "Render/VulkanBase/VulkanImage.h"
#include "Render/VulkanBase/VulkanRingBuffer.h"
#include "Render/VulkanBase/VulkanDescriptorPool.h"
#include "Render/VulkanBase/VulkanDescriptorSet.h"
#include "Render/Mesh.h"
#include "Render/Material.h"
#include "Render/VulkanBase/VulkanSwapchain.h"

namespace GE {

class Window;
class VulkanContext;

/// 基于实例的 2D/3D 网格渲染器单例。
/// 持有 ring buffers 用于 UBO 分配，通过 VulkanContext 访问 Vulkan 全局对象。
/// 管线和 descriptor set 存在于 Material 中，每个材质一套。
class Renderer {
public:
    static Renderer &Get();

    /// 完整初始化：从 VulkanContext 获取设备、从 swapchain 获取 image 数量，创建 ring buffers。
    void Init(VulkanContext &ctx, VulkanSwapchain &swapchain);

    /// 接管 pipeline 的 set=0（Frame）和 set=2（Object）layout，创建 descriptor pool + sets。
    void InitDescriptorSets(vk::Device device,
                            vk::DescriptorSetLayout frameLayout,
                            vk::DescriptorSetLayout objectLayout);

    /// 完整销毁。
    void Shutdown();

    /// 开始一帧的场景渲染。
    void BeginScene(vk::CommandBuffer cmd, uint32_t imageIndex,
                    vk::Extent2D dimensions,
                    const glm::mat4 &view, const glm::mat4 &projection,
                    const glm::vec3 &view_pos,
                    const glm::vec4 &clear_color = {0.01f, 0.01f, 0.033f, 1.0f});

    void EndScene();

    /// 用指定材质绘制网格。
    void Draw(const Mesh &mesh, const Material &material, const glm::mat4 &model, const glm::vec4 &color);

    /// 工厂方法：创建默认管线（MeshVertex 布局，UBO+纹理 descriptor）。
    /// @param dev            Vulkan 逻辑设备
    /// @param color_format   颜色附件格式
    /// @param depth_format   深度附件格式，不填则不开启深度测试
    VulkanPipeline CreateDefaultPipeline(vk::Device dev, vk::Format color_format,
                                         vk::Format depth_format = vk::Format{});

    /// ring buffer 大小（4 MB，约 16000 次 Draw）
    static constexpr vk::DeviceSize RING_BUFFER_SIZE = 4 * 1024 * 1024;

    /// 默认深度格式
    static constexpr vk::Format DEPTH_FORMAT = vk::Format::eD32Sfloat;

    [[nodiscard]] uint32_t GetSwapchainImageCount() const { return static_cast<uint32_t>(m_Swapchain ? m_Swapchain->GetImageCount() : 0); }

    [[nodiscard]] VulkanSwapchain &GetSwapchain() { return *m_Swapchain; }

    [[nodiscard]] VulkanContext &GetContext() { return *m_Context; }

private:
    Renderer() = default;

    ~Renderer();

    Renderer(const Renderer &) = delete;

    Renderer &operator=(const Renderer &) = delete;

    // set=0: per-frame UBO（每帧更新，无 dynamic offset）
    struct FrameUniformData {
        glm::mat4 projection;
        glm::mat4 view;
        glm::vec4 viewPos;
    };

    VulkanBuffer m_FrameBuffer;
    VulkanDescriptorSet m_FrameSet;

    // set=2: per-object UBO（ring buffer + dynamic offset）
    struct ObjectUniformData {
        glm::mat4 model;
        float lodBias;
        float _padding[3];
    };

    VulkanRingBuffer m_RingBuffer;
    VulkanDescriptorSet m_ObjectSet;

    // 共用 descriptor pool（frame + object 各一个 set）
    VulkanDescriptorPool m_GlobalPool;

    // 深度 buffer（与 swapchain 尺寸一致）
    VulkanImage m_DepthImage;
    bool m_DepthImageTransitioned = false;

    VulkanContext *m_Context = nullptr; // 非拥有指针
    VulkanSwapchain *m_Swapchain = nullptr; // 非拥有指针

    // Scene state
    vk::CommandBuffer m_ActiveCmd{nullptr};
    uint32_t m_CurrentImageIndex = 0;
    vk::Extent2D m_ActiveDim{};
    glm::mat4 m_View{1.0f};
    glm::mat4 m_Projection{1.0f};
    glm::vec3 m_ViewPos{0.0f};
};

} // namespace GE
