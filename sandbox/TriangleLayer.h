#pragma once

#include "GE/GE.h"
#include "GE/Render/VulkanBase/VulkanBuffer.h"
#include "GE/Render/VulkanBase/VulkanPipeline.h"
#include "GE/Render/VulkanBase/VulkanPipelineLayout.h"
#include "GE/Render/VulkanBase/ShaderModule.h"

namespace GE {

/// 使用新 API 的纯 Vulkan 三角形绘制层。
class TriangleLayer : public Layer {
public:
    TriangleLayer();
    ~TriangleLayer() override;

    void OnAttach() override;
    void OnDetach() override;
    void OnUpdate(Timestep &ts) override;
    void OnEvent(Event &event) override;
    void OnImGuiRender() override;

private:
    std::unique_ptr<ShaderModule>           m_VertShader;
    std::unique_ptr<ShaderModule>           m_FragShader;
    std::unique_ptr<VulkanPipelineLayout>   m_PipelineLayout;
    VulkanPipelineState                     m_PipelineState;
    std::unique_ptr<VulkanGraphicsPipeline> m_Pipeline;
    VulkanBuffer                            m_VertexBuffer;
};

} // namespace GE
