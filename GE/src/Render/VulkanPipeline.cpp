//
// Created by Lenovo on 2026/6/3.
//

#define VULKAN_HPP_DISPATCH_LOADER_DYNAMIC 1
#include "../../include/GE/Render/VulkanBase/VulkanPipeline.h"

#include "FileSystem/FileSystem.h"

#include "Debug/Assert.h"

namespace GE {

void VulkanPipeline::Init(vk::Device device, vk::Format color_format,
                          const VertexInputState &vertex_input, const std::string &vert_shader,
                          const std::string &frag_shader, const std::string &shader_folder) {
    m_Device = device;

    // DescriptorSetLayout must be set by the caller (via SetDescriptorSetLayout)
    // before calling Init.
    GE_ASSERT(m_DescriptorSetLayout, "DescriptorSetLayout must be set before Init");

    vk::PipelineLayoutCreateInfo layout_info{
        .setLayoutCount = 1,
        .pSetLayouts = &m_DescriptorSetLayout,
    };
    m_PipelineLayout = device.createPipelineLayout(layout_info);

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

    std::array<vk::PipelineShaderStageCreateInfo, 2> shader_stages = {{
        {.stage = vk::ShaderStageFlagBits::eVertex,
         .module = LoadShaderModule(device, shader_folder + "/" + vert_shader),
         .pName = "main"},
        {.stage = vk::ShaderStageFlagBits::eFragment,
         .module = LoadShaderModule(device, shader_folder + "/" + frag_shader),
         .pName = "main"},
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

    for (auto &shader_stage : shader_stages) {
        device.destroyShaderModule(shader_stage.module);
    }
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

void VulkanPipeline::SetDescriptorSetLayout(vk::DescriptorSetLayout layout) {
    if (m_Device && m_DescriptorSetLayout) {
        m_Device.destroyDescriptorSetLayout(m_DescriptorSetLayout);
    }
    m_DescriptorSetLayout = layout;
}

vk::ShaderModule VulkanPipeline::LoadShaderModule(vk::Device device, const std::string &path) {
    auto spirv = FileSystem::ReadBinaryU32(path);
    vk::ShaderModuleCreateInfo module_info{.codeSize = spirv.size() * sizeof(uint32_t), .pCode = spirv.data()};
    return device.createShaderModule(module_info);
}

} // namespace GE
