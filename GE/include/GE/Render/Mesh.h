#pragma once

#include <vulkan/vulkan.hpp>
#include "vk_mem_alloc.h"

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
    vk::IndexType indexType = vk::IndexType::eUint16;

    static constexpr MeshVertex kVertices[4] = {
        {{-0.5f, -0.5f, 0.0f}, {0.0f, 1.0f}, {0.0f, 0.0f, 1.0f}},
        {{0.5f, -0.5f, 0.0f}, {1.0f, 1.0f}, {0.0f, 0.0f, 1.0f}},
        {{0.5f, 0.5f, 0.0f}, {1.0f, 0.0f}, {0.0f, 0.0f, 1.0f}},
        {{-0.5f, 0.5f, 0.0f}, {0.0f, 0.0f}, {0.0f, 0.0f, 1.0f}},
    };
    static constexpr uint16_t kIndices[6] = {0, 1, 2, 0, 2, 3};

    // Cube vertices: 每个面 4 个顶点 × 6 个面 = 24 顶点, 36 索引
    static constexpr MeshVertex kCubeVertices[24] = {
        // 前面 (z = +0.5)
        {{-0.5f, -0.5f, 0.5f}, {0.0f, 1.0f}, {0.0f, 0.0f, 1.0f}},
        {{0.5f, -0.5f, 0.5f}, {1.0f, 1.0f}, {0.0f, 0.0f, 1.0f}},
        {{0.5f, 0.5f, 0.5f}, {1.0f, 0.0f}, {0.0f, 0.0f, 1.0f}},
        {{-0.5f, 0.5f, 0.5f}, {0.0f, 0.0f}, {0.0f, 0.0f, 1.0f}},
        // 后面 (z = -0.5)
        {{0.5f, -0.5f, -0.5f}, {0.0f, 1.0f}, {0.0f, 0.0f, -1.0f}},
        {{-0.5f, -0.5f, -0.5f}, {1.0f, 1.0f}, {0.0f, 0.0f, -1.0f}},
        {{-0.5f, 0.5f, -0.5f}, {1.0f, 0.0f}, {0.0f, 0.0f, -1.0f}},
        {{0.5f, 0.5f, -0.5f}, {0.0f, 0.0f}, {0.0f, 0.0f, -1.0f}},
        // 左面 (x = -0.5)
        {{-0.5f, -0.5f, -0.5f}, {0.0f, 1.0f}, {-1.0f, 0.0f, 0.0f}},
        {{-0.5f, -0.5f, 0.5f}, {1.0f, 1.0f}, {-1.0f, 0.0f, 0.0f}},
        {{-0.5f, 0.5f, 0.5f}, {1.0f, 0.0f}, {-1.0f, 0.0f, 0.0f}},
        {{-0.5f, 0.5f, -0.5f}, {0.0f, 0.0f}, {-1.0f, 0.0f, 0.0f}},
        // 右面 (x = +0.5)
        {{0.5f, -0.5f, 0.5f}, {0.0f, 1.0f}, {1.0f, 0.0f, 0.0f}},
        {{0.5f, -0.5f, -0.5f}, {1.0f, 1.0f}, {1.0f, 0.0f, 0.0f}},
        {{0.5f, 0.5f, -0.5f}, {1.0f, 0.0f}, {1.0f, 0.0f, 0.0f}},
        {{0.5f, 0.5f, 0.5f}, {0.0f, 0.0f}, {1.0f, 0.0f, 0.0f}},
        // 上面 (y = +0.5)
        {{-0.5f, 0.5f, 0.5f}, {0.0f, 1.0f}, {0.0f, 1.0f, 0.0f}},
        {{0.5f, 0.5f, 0.5f}, {1.0f, 1.0f}, {0.0f, 1.0f, 0.0f}},
        {{0.5f, 0.5f, -0.5f}, {1.0f, 0.0f}, {0.0f, 1.0f, 0.0f}},
        {{-0.5f, 0.5f, -0.5f}, {0.0f, 0.0f}, {0.0f, 1.0f, 0.0f}},
        // 下面 (y = -0.5)
        {{-0.5f, -0.5f, -0.5f}, {0.0f, 1.0f}, {0.0f, -1.0f, 0.0f}},
        {{0.5f, -0.5f, -0.5f}, {1.0f, 1.0f}, {0.0f, -1.0f, 0.0f}},
        {{0.5f, -0.5f, 0.5f}, {1.0f, 0.0f}, {0.0f, -1.0f, 0.0f}},
        {{-0.5f, -0.5f, 0.5f}, {0.0f, 0.0f}, {0.0f, -1.0f, 0.0f}},
    };
    static constexpr uint16_t kCubeIndices[36] = {
        0, 1, 2, 0, 2, 3,      // 前面
        4, 5, 6, 4, 6, 7,      // 后面
        8, 9, 10, 8, 10, 11,    // 左面
        12, 13, 14, 12, 14, 15, // 右面
        16, 17, 18, 16, 18, 19, // 上面
        20, 21, 22, 20, 22, 23, // 下面
    };

    void Init(VmaAllocator allocator,
              const void *vertexData, vk::DeviceSize vertexSize,
              const void *indexData, vk::DeviceSize indexSize,
              uint32_t indexCount, vk::IndexType indexType);

    /// 使用内置的 cube 数据初始化。
    void InitCube(VmaAllocator allocator);

    void Destroy();
};

} // namespace GE
