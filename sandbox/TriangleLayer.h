#pragma once

#include "GE/GE.h"
#include "GE/Render/VulkanBase/VulkanBuffer.h"
#include "GE/Render/VulkanBase/VulkanPipeline.h"
#include "GE/Render/VulkanBase/VulkanPipelineLayout.h"
#include "GE/Render/VulkanBase/VulkanShaderModule.h"

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
    VulkanShaderModule             *m_VertShader = nullptr;
    VulkanShaderModule             *m_FragShader = nullptr;
    VulkanPipelineLayout           *m_PipelineLayout = nullptr;
    VulkanPipelineState             m_PipelineState;
    VulkanGraphicsPipeline         *m_Pipeline = nullptr;
    std::unique_ptr<VulkanBuffer>   m_VertexBuffer;
};

} // namespace GE