#pragma once

#include "GE/GE.h"
#include "GE/Render/VulkanBase/VulkanBuffer.h"
#include "GE/Render/VulkanBase/VulkanPipeline.h"
#include "GE/Render/VulkanBase/VulkanPipelineLayout.h"
#include "GE/Render/VulkanBase/ShaderModule.h"
#include "GE/Render/VulkanBase/Texture.h"

namespace GE {

/// 使用新 API 的 Vulkan 纹理绘制层 —— 显示棋盘纹理。
class TextureLayer : public Layer {
public:
    TextureLayer();
    ~TextureLayer() override;

    void OnAttach() override;
    void OnDetach() override;
    void OnUpdate(Timestep &ts) override;
    void OnEvent(Event &event) override;
    void OnImGuiRender() override;

private:
    // 着色器（由 VulkanResourceCache 管理生命周期）
    ShaderModule             *m_VertShader = nullptr;
    ShaderModule             *m_FragShader = nullptr;

    // 管线布局和管线（由 VulkanResourceCache 管理生命周期）
    VulkanPipelineLayout     *m_PipelineLayout = nullptr;
    VulkanGraphicsPipeline   *m_Pipeline = nullptr;

    // 管线状态
    VulkanPipelineState       m_PipelineState;

    // 顶点 / 索引 / uniform buffer（由本层持有）
    std::unique_ptr<VulkanBuffer> m_VertexBuffer;
    std::unique_ptr<VulkanBuffer> m_IndexBuffer;
    std::unique_ptr<VulkanBuffer> m_UniformBuffer;

    // 纹理（封装 Image + ImageView + Sampler）
    std::unique_ptr<Texture> m_Texture;

    // Descriptor set layout（由缓存管理）
    VulkanDescriptorSetLayout *m_DescriptorSetLayout = nullptr;

    // 描述符集（由 VulkanRenderFrame 每帧管理，在 OnUpdate 中获取）
};

} // namespace GE