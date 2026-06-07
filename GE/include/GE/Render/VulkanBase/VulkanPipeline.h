#pragma once


#include <vulkan/vulkan.hpp>

#include <array>
#include <string>
#include <vector>

namespace GE {

struct VertexInputState {
    uint32_t stride;
    std::vector<vk::VertexInputAttributeDescription> attributes;
};

class VulkanPipeline {
public:
    VulkanPipeline() = default;

    ~VulkanPipeline() = default;

    VulkanPipeline(const VulkanPipeline &) = delete;
    VulkanPipeline &operator=(const VulkanPipeline &) = delete;

    VulkanPipeline(VulkanPipeline &&) = default;
    VulkanPipeline &operator=(VulkanPipeline &&) = default;

    void Init(vk::Device device, vk::Format color_format, const VertexInputState &vertex_input,
              const std::string &vert_shader = "triangle.vert.spv",
              const std::string &frag_shader = "triangle.frag.spv",
              const std::string &shader_folder = "assets/shaders/glsl");

    void Cleanup();

    void Bind(vk::CommandBuffer cmd) const { cmd.bindPipeline(vk::PipelineBindPoint::eGraphics, m_Pipeline); }

    [[nodiscard]] vk::Pipeline GetPipeline() const { return m_Pipeline; }
    [[nodiscard]] vk::PipelineLayout GetLayout() const { return m_PipelineLayout; }
    [[nodiscard]] vk::DescriptorSetLayout GetDescriptorSetLayout() const { return m_DescriptorSetLayout; }

    /// Takes ownership of the layout handle. Destroys any previously owned layout.
    void SetDescriptorSetLayout(vk::DescriptorSetLayout layout);

private:
    static vk::ShaderModule LoadShaderModule(vk::Device device, const std::string &path);

    vk::Device m_Device = nullptr;
    vk::DescriptorSetLayout m_DescriptorSetLayout = nullptr;
    vk::PipelineLayout m_PipelineLayout = nullptr;
    vk::Pipeline m_Pipeline = nullptr;
};

} // namespace GE
