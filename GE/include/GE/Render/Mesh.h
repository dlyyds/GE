#pragma once

#include <vulkan/vulkan.hpp>

#include "glm/glm.hpp"
#include "Render/VulkanBase/VulkanBuffer.h"

namespace GE {

/// Vertex format used by the default pipeline.
struct MeshVertex {
    glm::vec3 position;
    glm::vec2 uv;
    glm::vec3 normal;
};

/// A GPU-ready mesh holding vertex and index buffers.
struct Mesh {
    VulkanBuffer vertices;
    VulkanBuffer indices;
    uint32_t indexCount = 0;

    static constexpr MeshVertex kVertices[4] = {
        {{-0.5f, -0.5f, 0.0f}, {0.0f, 1.0f}, {0.0f, 0.0f, 1.0f}},
        {{0.5f, -0.5f, 0.0f}, {1.0f, 1.0f}, {0.0f, 0.0f, 1.0f}},
        {{0.5f, 0.5f, 0.0f}, {1.0f, 0.0f}, {0.0f, 0.0f, 1.0f}},
        {{-0.5f, 0.5f, 0.0f}, {0.0f, 0.0f}, {0.0f, 0.0f, 1.0f}},
    };
    static constexpr uint16_t kIndices[6] = {0, 1, 2, 0, 2, 3};

    void Init(vk::Device device, vk::PhysicalDevice gpu,
              const void *vertexData, vk::DeviceSize vertexSize,
              const void *indexData, vk::DeviceSize indexSize,
              uint32_t indexCount);

    void Destroy();
};

} // namespace GE
