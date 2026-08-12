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
#include <cmath>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

namespace GE {

// ============================================================================
// 辅助：计算顶点切线（法线贴图需要 TBN 切线空间）
// ============================================================================

/**
 * @brief 根据三角形 (位置, UV) 计算每个顶点的切线向量。
 *
 * 标准算法：对每个三角形计算切线与副切线插值，累加到三个顶点上，
 * 最后按顶点的法线用 Gram-Schmidt 正交化得到最终的切线方向，
 * 并以 w 分量记录手性符号（用于在着色器中重建正确的副切线方向）。
 *
 * @param vertices 顶点数组（就地修改 Tangent 字段）
 * @param indices  索引数组（每 3 个一组构成三角形）
 */
static void ComputeTangents(std::vector<Vertex> &vertices,
                            const std::vector<uint32_t> &indices) {
    std::vector<glm::vec3> tan1(vertices.size(), glm::vec3(0.0f));
    std::vector<glm::vec3> tan2(vertices.size(), glm::vec3(0.0f));

    for (size_t i = 0; i + 2 < indices.size(); i += 3) {
        const auto &v0 = vertices[indices[i + 0]];
        const auto &v1 = vertices[indices[i + 1]];
        const auto &v2 = vertices[indices[i + 2]];

        glm::vec3 edge1 = v1.Position - v0.Position;
        glm::vec3 edge2 = v2.Position - v0.Position;
        glm::vec2 duv1  = v1.TexCoord - v0.TexCoord;
        glm::vec2 duv2  = v2.TexCoord - v0.TexCoord;

        float r = 1.0f / (duv1.x * duv2.y - duv2.x * duv1.y);
        glm::vec3 tangent   = (edge1 * duv2.y - edge2 * duv1.y) * r;
        glm::vec3 bitangent = (edge2 * duv1.x - edge1 * duv2.x) * r;

        tan1[indices[i + 0]] += tangent;
        tan1[indices[i + 1]] += tangent;
        tan1[indices[i + 2]] += tangent;
        tan2[indices[i + 0]] += bitangent;
        tan2[indices[i + 1]] += bitangent;
        tan2[indices[i + 2]] += bitangent;
    }

    for (size_t i = 0; i < vertices.size(); ++i) {
        auto &v = vertices[i];
        glm::vec3 n = v.Normal;
        glm::vec3 t = tan1[i];

        // Gram-Schmidt 正交化：使切线垂直于法线
        t = glm::normalize(t - n * glm::dot(n, t));
        v.Tangent = glm::vec4(t, 1.0f);

        // 手性符号：根据切线/副切线/法线的相对方向判断左右手系
        float handedness = glm::dot(glm::cross(n, t), tan2[i]);
        v.Tangent.w = (handedness < 0.0f) ? -1.0f : 1.0f;
    }
}

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

    // 计算顶点切线（法线贴图需要）
    ComputeTangents(mesh->m_Vertices, mesh->m_Indices);

    if (!mesh->UploadToGPU(device)) {
        return nullptr;
    }

    // 记录源文件路径
    mesh->m_FilePath = filepath;

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

    // 计算顶点切线（法线贴图需要）
    ComputeTangents(mesh->m_Vertices, mesh->m_Indices);

    if (!mesh->UploadToGPU(device)) {
        return nullptr;
    }

    return mesh;
}

// ============================================================================
// 工厂方法：创建内置几何体
// ============================================================================

std::unique_ptr<Mesh> Mesh::CreateBuiltin(VulkanDevice &device, const std::string &type) {
    std::vector<Vertex>   vertices;
    std::vector<uint32_t> indices;

    if (type == "cube") {
        // 立方体：边长 2，中心在原点，6 个面各 4 顶点 = 24 顶点，36 索引
        vertices.reserve(24);
        indices.reserve(36);

        auto add_quad = [&](const glm::vec3 &p0, const glm::vec3 &p1,
                            const glm::vec3 &p2, const glm::vec3 &p3,
                            const glm::vec3 &normal) {
            uint32_t base = static_cast<uint32_t>(vertices.size());
            glm::vec2 uvs[] = {{0.0f, 0.0f}, {1.0f, 0.0f}, {1.0f, 1.0f}, {0.0f, 1.0f}};
            glm::vec3 pos[] = {p0, p1, p2, p3};
            for (int i = 0; i < 4; ++i) {
                vertices.push_back({pos[i], normal, uvs[i]});
            }
            indices.push_back(base + 0);
            indices.push_back(base + 1);
            indices.push_back(base + 2);
            indices.push_back(base + 0);
            indices.push_back(base + 2);
            indices.push_back(base + 3);
        };

        // +Z 面（前）
        add_quad({-1.0f, -1.0f, 1.0f}, {1.0f, -1.0f, 1.0f},
                 {1.0f, 1.0f, 1.0f}, {-1.0f, 1.0f, 1.0f},
                 {0.0f, 0.0f, 1.0f});
        // -Z 面（后）
        add_quad({1.0f, -1.0f, -1.0f}, {-1.0f, -1.0f, -1.0f},
                 {-1.0f, 1.0f, -1.0f}, {1.0f, 1.0f, -1.0f},
                 {0.0f, 0.0f, -1.0f});
        // +X 面（右）
        add_quad({1.0f, -1.0f, 1.0f}, {1.0f, -1.0f, -1.0f},
                 {1.0f, 1.0f, -1.0f}, {1.0f, 1.0f, 1.0f},
                 {1.0f, 0.0f, 0.0f});
        // -X 面（左）
        add_quad({-1.0f, -1.0f, -1.0f}, {-1.0f, -1.0f, 1.0f},
                 {-1.0f, 1.0f, 1.0f}, {-1.0f, 1.0f, -1.0f},
                 {-1.0f, 0.0f, 0.0f});
        // +Y 面（上）
        add_quad({-1.0f, 1.0f, 1.0f}, {1.0f, 1.0f, 1.0f},
                 {1.0f, 1.0f, -1.0f}, {-1.0f, 1.0f, -1.0f},
                 {0.0f, 1.0f, 0.0f});
        // -Y 面（下）
        add_quad({-1.0f, -1.0f, -1.0f}, {1.0f, -1.0f, -1.0f},
                 {1.0f, -1.0f, 1.0f}, {-1.0f, -1.0f, 1.0f},
                 {0.0f, -1.0f, 0.0f});
    } else if (type == "plane") {
        // 平面：XY 平面，边长 2，中心在原点，法线 +Z
        vertices = {
            {{-1.0f, -1.0f, 0.0f}, {0.0f, 0.0f, 1.0f}, {0.0f, 0.0f}},
            {{ 1.0f, -1.0f, 0.0f}, {0.0f, 0.0f, 1.0f}, {1.0f, 0.0f}},
            {{ 1.0f,  1.0f, 0.0f}, {0.0f, 0.0f, 1.0f}, {1.0f, 1.0f}},
            {{-1.0f,  1.0f, 0.0f}, {0.0f, 0.0f, 1.0f}, {0.0f, 1.0f}},
        };
        indices = {0, 1, 2, 0, 2, 3};
    } else if (type == "quad") {
        // 四边形（plane 的别名）
        vertices = {
            {{-1.0f, -1.0f, 0.0f}, {0.0f, 0.0f, 1.0f}, {0.0f, 0.0f}},
            {{ 1.0f, -1.0f, 0.0f}, {0.0f, 0.0f, 1.0f}, {1.0f, 0.0f}},
            {{ 1.0f,  1.0f, 0.0f}, {0.0f, 0.0f, 1.0f}, {1.0f, 1.0f}},
            {{-1.0f,  1.0f, 0.0f}, {0.0f, 0.0f, 1.0f}, {0.0f, 1.0f}},
        };
        indices = {0, 1, 2, 0, 2, 3};
    } else if (type == "sphere") {
        // 球体：半径 1，中心在原点，UV 球体
        const int latBands = 20;
        const int lonBands = 20;
        const float radius = 1.0f;

        for (int lat = 0; lat <= latBands; ++lat) {
            float theta = static_cast<float>(lat) * static_cast<float>(M_PI) / static_cast<float>(latBands);
            float sinTheta = std::sin(theta);
            float cosTheta = std::cos(theta);

            for (int lon = 0; lon <= lonBands; ++lon) {
                float phi = static_cast<float>(lon) * 2.0f * static_cast<float>(M_PI) / static_cast<float>(lonBands);
                float sinPhi = std::sin(phi);
                float cosPhi = std::cos(phi);

                glm::vec3 pos{
                    radius * cosPhi * sinTheta,
                    radius * cosTheta,
                    radius * sinPhi * sinTheta
                };
                glm::vec3 normal = glm::normalize(pos);
                glm::vec2 uv{
                    static_cast<float>(lon) / static_cast<float>(lonBands),
                    static_cast<float>(lat) / static_cast<float>(latBands)
                };
                vertices.push_back({pos, normal, uv});
            }
        }

        for (int lat = 0; lat < latBands; ++lat) {
            for (int lon = 0; lon < lonBands; ++lon) {
                // 注意：绕序必须与法线一致（CCW 朝外）。原实现 (first,second,first+1)
                // 的叉积法线朝内，导致外侧被当作背面剔除、法线背离相机，
                // 所有直接光照失效（只剩环境光）。这里交换 last two 顶点翻转绕序。
                uint32_t first  = static_cast<uint32_t>(lat * (lonBands + 1) + lon);
                uint32_t second = first + static_cast<uint32_t>(lonBands + 1);
                indices.push_back(first);
                indices.push_back(first + 1);
                indices.push_back(second);
                indices.push_back(second);
                indices.push_back(first + 1);
                indices.push_back(second + 1);
            }
        }
    } else {
        return nullptr; // 未知类型
    }

    auto mesh = Create(device, vertices, indices);
    if (mesh) {
        mesh->m_FilePath = "builtin:" + type;
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
