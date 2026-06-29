#pragma once

#include "GE/GE.h"
#include "GE/Render/VulkanBase/VulkanBuffer.h"
#include "GE/Render/VulkanBase/VulkanPipeline.h"
#include "GE/Render/VulkanBase/VulkanShader.h"

namespace GE {

/// 不使用 Renderer 的纯粹 Vulkan 三角形绘制层。
/// 演示如何直接操作 Vulkan API 来绘制一个彩色三角形。
class TriangleLayer : public Layer {
public:
    TriangleLayer();
    ~TriangleLayer() override = default;

    void OnAttach() override;
    void OnDetach() override;
    void OnUpdate(Timestep &ts) override;
    void OnEvent(Event &event) override;
    void OnImGuiRender() override;

private:
    VulkanShader   m_VertShader;
    VulkanShader   m_FragShader;
    VulkanPipeline m_Pipeline;
    VulkanBuffer   m_VertexBuffer;
};

} // namespace GE
