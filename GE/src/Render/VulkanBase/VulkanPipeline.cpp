//
// Created by Lenovo on 2026/6/3.
//

#define VULKAN_HPP_DISPATCH_LOADER_DYNAMIC 1
#include "../../../include/GE/Render/VulkanBase/VulkanPipeline.h"

#include <algorithm>

namespace GE {

void VulkanPipeline::Init(vk::Device device, vk::Format color_format,
                          const VulkanShader &vertShader, const VulkanShader &fragShader,
                          const std::vector<uint32_t> &dynamicBindings) {
    m_Device = device;

    // 反射 descriptor bindings 并合并 vertex + fragment
    auto vert_bindings = vertShader.ReflectDescriptorBindings();
    auto frag_bindings = fragShader.ReflectDescriptorBindings();
    m_DescriptorBindings = VulkanShader::MergeDescriptorBindings({vert_bindings, frag_bindings});

    // 将指定的 binding 转为 Dynamic 类型
    for (auto &b : m_DescriptorBindings) {
        if (std::ranges::find(dynamicBindings, b.binding) != dynamicBindings.end()) {
            if (b.descriptorType == vk::DescriptorType::eUniformBuffer)
                b.descriptorType = vk::DescriptorType::eUniformBufferDynamic;
            else if (b.descriptorType == vk::DescriptorType::eStorageBuffer)
                b.descriptorType = vk::DescriptorType::eStorageBufferDynamic;
        }
    }

    // 从反射结果创建 DescriptorSetLayout（转为 vk 原生类型）
    std::vector<vk::DescriptorSetLayoutBinding> raw_bindings;
    raw_bindings.reserve(m_DescriptorBindings.size());
    for (auto &b : m_DescriptorBindings) {
        raw_bindings.push_back(vk::DescriptorSetLayoutBinding{
            .binding = b.binding,
            .descriptorType = b.descriptorType,
            .descriptorCount = b.descriptorCount,
            .stageFlags = b.stageFlags,
        });
    }
    vk::DescriptorSetLayoutCreateInfo layout_info{
        .bindingCount = static_cast<uint32_t>(raw_bindings.size()),
        .pBindings = raw_bindings.data(),
    };
    m_DescriptorSetLayout = device.createDescriptorSetLayout(layout_info);

    // 创建 PipelineLayout
    vk::PipelineLayoutCreateInfo pipeline_layout_info{
        .setLayoutCount = 1,
        .pSetLayouts = &m_DescriptorSetLayout,
    };
    m_PipelineLayout = device.createPipelineLayout(pipeline_layout_info);

    // 从 vertex shader 自动反射 vertex input layout
    VertexInputState vertex_input = vertShader.ReflectVertexInput();

    vk::VertexInputBindingDescription binding_description{
        .binding = 0, .stride = vertex_input.stride, .inputRate = vk::VertexInputRate::eVertex};

    vk::PipelineVertexInputStateCreateInfo vertex_input_state{
        .vertexBindingDescriptionCount = 1,
        .pVertexBindingDescriptions = &binding_description,
        .vertexAttributeDescriptionCount = static_cast<uint32_t>(vertex_input.attributes.size()),
        .pVertexAttributeDescriptions = vertex_input.attributes.data()};

    vk::PipelineInputAssemblyStateCreateInfo input_assembly{.topology = vk::PrimitiveTopology::eTriangleList};
    vk::PipelineRasterizationStateCreateInfo raster{.polygonMode = vk::PolygonMode::eFill, .lineWidth = 1.0f};

    std::vector<vk::DynamicState> dynamic_states = {
        vk::DynamicState::eViewport, vk::DynamicState::eScissor, vk::DynamicState::eCullMode,
        vk::DynamicState::eFrontFace, vk::DynamicState::ePrimitiveTopology};

    vk::PipelineColorBlendAttachmentState blend_attachment{
        .colorWriteMask = vk::ColorComponentFlagBits::eR | vk::ColorComponentFlagBits::eG |
                          vk::ColorComponentFlagBits::eB | vk::ColorComponentFlagBits::eA};

    vk::PipelineColorBlendStateCreateInfo blend{.attachmentCount = 1, .pAttachments = &blend_attachment};
    vk::PipelineViewportStateCreateInfo viewport{.viewportCount = 1, .scissorCount = 1};
    vk::PipelineDepthStencilStateCreateInfo depth_stencil{.depthCompareOp = vk::CompareOp::eAlways};
    vk::PipelineMultisampleStateCreateInfo multisample{.rasterizationSamples = vk::SampleCountFlagBits::e1};

    vk::PipelineDynamicStateCreateInfo dynamic_state_info{
        .dynamicStateCount = static_cast<uint32_t>(dynamic_states.size()),
        .pDynamicStates = dynamic_states.data()};

    // 使用 VulkanShader 提供的 stage create info
    std::array<vk::PipelineShaderStageCreateInfo, 2> shader_stages = {{
        vertShader.GetStageCreateInfo(),
        fragShader.GetStageCreateInfo(),
    }};

    vk::PipelineRenderingCreateInfo pipeline_rendering_info{.colorAttachmentCount = 1,
                                                            .pColorAttachmentFormats = &color_format};

    vk::GraphicsPipelineCreateInfo pipeline_create_info{
        .pNext = &pipeline_rendering_info,
        .stageCount = static_cast<uint32_t>(shader_stages.size()),
        .pStages = shader_stages.data(),
        .pVertexInputState = &vertex_input_state,
        .pInputAssemblyState = &input_assembly,
        .pViewportState = &viewport,
        .pRasterizationState = &raster,
        .pMultisampleState = &multisample,
        .pDepthStencilState = &depth_stencil,
        .pColorBlendState = &blend,
        .pDynamicState = &dynamic_state_info,
        .layout = m_PipelineLayout,
        .renderPass = VK_NULL_HANDLE,
        .subpass = 0,
    };

    auto result = device.createGraphicsPipeline(vk::PipelineCache{}, pipeline_create_info);
    if (result.result != vk::Result::eSuccess) {
        throw std::runtime_error("Failed to create graphics pipeline");
    }
    m_Pipeline = result.value;
}

void VulkanPipeline::Cleanup() {
    if (m_Device) {
        if (m_Pipeline)
            m_Device.destroyPipeline(m_Pipeline);
        if (m_PipelineLayout)
            m_Device.destroyPipelineLayout(m_PipelineLayout);
        if (m_DescriptorSetLayout)
            m_Device.destroyDescriptorSetLayout(m_DescriptorSetLayout);
    }
    m_Pipeline = nullptr;
    m_PipelineLayout = nullptr;
    m_DescriptorSetLayout = nullptr;
    m_Device = nullptr;
}

uint32_t VulkanPipeline::GetBindingByName(const std::string &name) const {
    for (auto &b : m_DescriptorBindings) {
        if (b.name == name)
            return b.binding;
    }
    return UINT32_MAX;
}

void VulkanPipeline::SetDescriptorSetLayout(vk::DescriptorSetLayout layout) {
    if (m_Device && m_DescriptorSetLayout) {
        m_Device.destroyDescriptorSetLayout(m_DescriptorSetLayout);
    }
    m_DescriptorSetLayout = layout;
}

} // namespace GE
