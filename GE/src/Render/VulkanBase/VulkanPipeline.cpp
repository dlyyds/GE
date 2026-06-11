//
// Created by Lenovo on 2026/6/3.
//

#define VULKAN_HPP_DISPATCH_LOADER_DYNAMIC 1
#include "../../../include/GE/Render/VulkanBase/VulkanPipeline.h"

#include <algorithm>
#include <map>

namespace GE {

void VulkanPipeline::Init(vk::Device device, vk::Format color_format,
                          const VulkanShader &vertShader, const VulkanShader &fragShader,
                          const std::vector<std::pair<uint32_t, uint32_t>> &dynamicBindings) {
    m_Device = device;

    // 反射 descriptor bindings 并合并 vertex + fragment
    auto vert_bindings = vertShader.ReflectDescriptorBindings();
    auto frag_bindings = fragShader.ReflectDescriptorBindings();
    m_DescriptorBindings = VulkanShader::MergeDescriptorBindings({vert_bindings, frag_bindings});

    // 将指定的 (set, binding) 转为 Dynamic 类型
    for (auto &b : m_DescriptorBindings) {
        auto it = std::ranges::find(dynamicBindings, std::pair(b.set, b.binding));
        if (it != dynamicBindings.end()) {
            if (b.descriptorType == vk::DescriptorType::eUniformBuffer)
                b.descriptorType = vk::DescriptorType::eUniformBufferDynamic;
            else if (b.descriptorType == vk::DescriptorType::eStorageBuffer)
                b.descriptorType = vk::DescriptorType::eStorageBufferDynamic;
        }
    }

    // 按 set 分组 bindings
    std::map<uint32_t, std::vector<DescriptorBindingInfo>> bindingsBySet;
    for (auto &b : m_DescriptorBindings) {
        bindingsBySet[b.set].push_back(b);
    }

    // 确定最大 set 编号，resize vector
    uint32_t maxSet = bindingsBySet.empty() ? 0 : static_cast<uint32_t>(bindingsBySet.rbegin()->first) + 1;
    m_DescriptorSetLayouts.assign(maxSet, nullptr);

    // 为每个 set 创建 DescriptorSetLayout
    for (auto &[set, bindings] : bindingsBySet) {
        std::vector<vk::DescriptorSetLayoutBinding> raw_bindings;
        raw_bindings.reserve(bindings.size());
        for (auto &b : bindings) {
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
        m_DescriptorSetLayouts[set] = device.createDescriptorSetLayout(layout_info);
    }

    // 创建 PipelineLayout（包含所有 set）
    vk::PipelineLayoutCreateInfo pipeline_layout_info{
        .setLayoutCount = maxSet,
        .pSetLayouts = m_DescriptorSetLayouts.data(),
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
        for (auto &layout : m_DescriptorSetLayouts) {
            if (layout)
                m_Device.destroyDescriptorSetLayout(layout);
        }
    }
    m_DescriptorSetLayouts.clear();
    m_PipelineLayout = nullptr;
    m_Pipeline = nullptr;
    m_Device = nullptr;
}

uint32_t VulkanPipeline::GetBindingByName(const std::string &name) const {
    for (auto &b : m_DescriptorBindings) {
        if (b.name == name)
            return b.binding;
    }
    return UINT32_MAX;
}

} // namespace GE
