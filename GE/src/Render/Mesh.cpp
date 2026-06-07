//
// Created by Lenovo on 2026/6/7.
//

#define VULKAN_HPP_DISPATCH_LOADER_DYNAMIC 1
#include "Render/Mesh.h"

namespace GE {

void Mesh::Init(vk::Device device, vk::PhysicalDevice gpu,
                const void *vertexData, vk::DeviceSize vertexSize,
                const void *indexData, vk::DeviceSize indexSize,
                uint32_t indexCount) {
    this->indexCount = indexCount;

    vertices.Init(device, gpu, vertexSize,
                  vk::BufferUsageFlagBits::eVertexBuffer,
                  vk::MemoryPropertyFlagBits::eHostVisible | vk::MemoryPropertyFlagBits::eHostCoherent);
    vertices.Upload(vertexData, vertexSize);

    indices.Init(device, gpu, indexSize,
                 vk::BufferUsageFlagBits::eIndexBuffer,
                 vk::MemoryPropertyFlagBits::eHostVisible | vk::MemoryPropertyFlagBits::eHostCoherent);
    indices.Upload(indexData, indexSize);
}

void Mesh::Destroy() {
    indices.Destroy();
    vertices.Destroy();
    indexCount = 0;
}

} // namespace GE
