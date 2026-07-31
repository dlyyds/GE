/**
 * @file Mesh.cpp
 * @brief 网格实现 —— 顶点/索引缓冲 + OBJ 加载。
 */

#include "Render/Mesh.h"
#include "Render/VulkanBase/VulkanCommandBuffer.h"
#include "Render/VulkanBase/VulkanQueue.h"

#include "tiny_obj_loader.h"

#include <iostream>
#include <unordered_map>

namespace GE {

// ============================================================================
// 辅助：上传数据到 GPU buffer（staging buffer 方式）
// ============================================================================

static std::unique_ptr<VulkanBuffer> UploadBuffer(
    VulkanDevice &device,
    vk::BufferUsageFlagBits usage,
    vk::DeviceSize size,
    const void *data)
{
    // 创建 staging buffer 并拷贝数据
    auto staging = VulkanBuffer::create_staging_buffer(device, size, data);

    // 创建目标 buffer（GPU 本地，用作顶点/索引缓冲 + 传输目标）
    auto dst = VulkanBufferBuilder(size)
        .with_usage(usage | vk::BufferUsageFlagBits::eTransferDst)
        .with_vma_usage(VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE)
        .build_unique(device);

    // 获取临时 command buffer
    auto &cmd = device.RequestCommandBuffer(vk::CommandBufferLevel::ePrimary, true);

    vk::BufferCopy copyRegion{};
    copyRegion.size = size;
    cmd.GetHandle().copyBuffer(staging.GetHandle(), dst->GetHandle(), copyRegion);

    // 提交并等待完成（按 command buffer 所属队列族提交）
    cmd.End();
    auto &queue = device.GetQueue(cmd.GetQueueFamilyIndex());
    device.FlushCommandBuffer(cmd, queue.GetHandle());

    // staging buffer 在此处自动析构
    return dst;
}

// ============================================================================
// 工厂方法：从文件加载
// ============================================================================

std::unique_ptr<Mesh> Mesh::LoadFromFile(VulkanDevice &device,
                                         const std::string &filepath)
{
    tinyobj::attrib_t                attrib;
    std::vector<tinyobj::shape_t>    shapes;
    std::vector<tinyobj::material_t> materials;
    std::string                      warn, err;

    bool ret = tinyobj::LoadObj(&attrib, &shapes, &materials, &warn, &err,
                                filepath.c_str());

    if (!warn.empty()) {
        std::cout << "[Mesh] Warning: " << warn << std::endl;
    }
    if (!err.empty()) {
        std::cerr << "[Mesh] Error loading '" << filepath << "': " << err << std::endl;
    }
    if (!ret) {
        return nullptr;
    }

    std::vector<Vertex>   vertices;
    std::vector<uint32_t> indices;
    std::unordered_map<Vertex, uint32_t> uniqueVertices;

    // 遍历所有形状（合并成一个 mesh）
    for (const auto &shape : shapes) {
        for (const auto &index : shape.mesh.indices) {
            Vertex v{};

            // 位置
            v.Position = {
                attrib.vertices[3 * index.vertex_index + 0],
                attrib.vertices[3 * index.vertex_index + 1],
                attrib.vertices[3 * index.vertex_index + 2],
            };

            // 法线（如果有）
            if (index.normal_index >= 0) {
                v.Normal = {
                    attrib.normals[3 * index.normal_index + 0],
                    attrib.normals[3 * index.normal_index + 1],
                    attrib.normals[3 * index.normal_index + 2],
                };
            }

            // 纹理坐标（如果有）
            if (index.texcoord_index >= 0) {
                v.TexCoord = {
                    attrib.texcoords[2 * index.texcoord_index + 0],
                    attrib.texcoords[2 * index.texcoord_index + 1],
                };
            }

            // 去重：相同顶点复用索引
            if (uniqueVertices.find(v) == uniqueVertices.end()) {
                uniqueVertices[v] = static_cast<uint32_t>(vertices.size());
                vertices.push_back(v);
            }
            indices.push_back(uniqueVertices[v]);
        }
    }

    if (vertices.empty() || indices.empty()) {
        std::cerr << "[Mesh] Empty mesh loaded from '" << filepath << "'" << std::endl;
        return nullptr;
    }

    auto mesh = std::unique_ptr<Mesh>(new Mesh());
    mesh->m_Vertices = std::move(vertices);
    mesh->m_Indices  = std::move(indices);

    if (!mesh->UploadToGPU(device)) {
        return nullptr;
    }

    return mesh;
}

// ============================================================================
// 工厂方法：从 CPU 端数据创建
// ============================================================================

std::unique_ptr<Mesh> Mesh::Create(VulkanDevice &device,
                                   const std::vector<Vertex> &vertices,
                                   const std::vector<uint32_t> &indices)
{
    if (vertices.empty() || indices.empty()) {
        return nullptr;
    }

    auto mesh = std::unique_ptr<Mesh>(new Mesh());
    mesh->m_Vertices = vertices;
    mesh->m_Indices  = indices;

    if (!mesh->UploadToGPU(device)) {
        return nullptr;
    }

    return mesh;
}

// ============================================================================
// 析构 / 移动
// ============================================================================

Mesh::~Mesh() = default;

Mesh::Mesh(Mesh &&other) noexcept :
    m_Vertices(std::move(other.m_Vertices)),
    m_Indices(std::move(other.m_Indices)),
    m_VertexBuffer(std::move(other.m_VertexBuffer)),
    m_IndexBuffer(std::move(other.m_IndexBuffer))
{
}

// ============================================================================
// 上传到 GPU
// ============================================================================

bool Mesh::UploadToGPU(VulkanDevice &device)
{
    vk::DeviceSize vbSize = m_Vertices.size() * sizeof(Vertex);
    vk::DeviceSize ibSize = m_Indices.size() * sizeof(uint32_t);

    m_VertexBuffer = UploadBuffer(device,
                                  vk::BufferUsageFlagBits::eVertexBuffer,
                                  vbSize, m_Vertices.data());
    if (!m_VertexBuffer) {
        return false;
    }

    m_IndexBuffer = UploadBuffer(device,
                                 vk::BufferUsageFlagBits::eIndexBuffer,
                                 ibSize, m_Indices.data());
    if (!m_IndexBuffer) {
        return false;
    }

    return true;
}

// ============================================================================
// 调试名称
// ============================================================================

void Mesh::SetDebugName(const std::string &name)
{
    if (m_VertexBuffer) {
        m_VertexBuffer->SetDebugName(name + "_VB");
    }
    if (m_IndexBuffer) {
        m_IndexBuffer->SetDebugName(name + "_IB");
    }
}

} // namespace GE
