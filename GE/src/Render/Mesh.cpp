//
// Created by Lenovo on 2026/6/7.
//

#include "Render/Mesh.h"

namespace GE {

void Mesh::Init(VmaAllocator allocator,
                const void *vertexData, vk::DeviceSize vertexSize,
                const void *indexData, vk::DeviceSize indexSize,
                uint32_t indexCount) {
    this->indexCount = indexCount;

    vertices.Init(allocator, vertexSize,
                  vk::BufferUsageFlagBits::eVertexBuffer);
    vertices.Upload(vertexData, vertexSize);

    indices.Init(allocator, indexSize,
                 vk::BufferUsageFlagBits::eIndexBuffer);
    indices.Upload(indexData, indexSize);
}

void Mesh::Destroy() {
    indices.Destroy();
    vertices.Destroy();
    indexCount = 0;
}

} // namespace GE
