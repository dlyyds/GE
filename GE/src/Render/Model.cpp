//
// Created by Lenovo on 2026/6/11.
//

#include "Render/Model.h"
#include "Core/Log.h"

#include "tiny_obj_loader.h"

#include <unordered_map>
#include <cstring>

namespace GE {

void Model::BuildMesh(VmaAllocator allocator,
                      const std::vector<MeshVertex> &vertices,
                      const std::vector<uint32_t> &indices) {
    // 转为 16-bit 索引（444 个顶点以内用 uint16_t 省一半显存）
    bool use16Bit = vertices.size() <= 65536;

    if (use16Bit) {
        std::vector<uint16_t> idx16(indices.size());
        for (size_t i = 0; i < indices.size(); i++)
            idx16[i] = static_cast<uint16_t>(indices[i]);

        m_Mesh.Init(allocator,
                    vertices.data(), vertices.size() * sizeof(MeshVertex),
                    idx16.data(), idx16.size() * sizeof(uint16_t),
                    static_cast<uint32_t>(indices.size()),
                    vk::IndexType::eUint16);
    } else {
        m_Mesh.Init(allocator,
                    vertices.data(), vertices.size() * sizeof(MeshVertex),
                    indices.data(), indices.size() * sizeof(uint32_t),
                    static_cast<uint32_t>(indices.size()),
                    vk::IndexType::eUint32);
    }
}

void Model::LoadFromFile(VmaAllocator allocator, const std::string &filepath) {
    Cleanup();

    tinyobj::ObjReaderConfig reader_config;
    reader_config.triangulate = true;
    reader_config.vertex_color = false;

    tinyobj::ObjReader reader;
    if (!reader.ParseFromFile(filepath, reader_config)) {
        if (!reader.Error().empty())
            GE_CORE_ERROR("Model::LoadFromFile: {}", reader.Error());
        return;
    }

    auto &attrib = reader.GetAttrib();
    auto &shapes = reader.GetShapes();

    if (shapes.empty()) {
        GE_CORE_ERROR("Model::LoadFromFile: no shapes in '{}'", filepath);
        return;
    }

    // vertex dedup 结构（全局去重，跨 shape 共享顶点）
    struct VertKey {
        int v, vt, vn;

        bool operator==(const VertKey &o) const {
            return v == o.v && vt == o.vt && vn == o.vn;
        }
    };
    struct VertKeyHash {
        size_t operator()(const VertKey &k) const {
            return static_cast<size_t>(k.v) ^ (static_cast<size_t>(k.vt) << 10) ^
                   (static_cast<size_t>(k.vn) << 20);
        }
    };

    std::vector<MeshVertex> vertices;
    std::vector<uint32_t> indices;
    std::unordered_map<VertKey, uint32_t, VertKeyHash> vertMap;

    size_t totalTriangles = 0;

    // 遍历所有 shape，合并到同一个 mesh
    for (auto &shape : shapes) {
        totalTriangles += shape.mesh.num_face_vertices.size();

        for (auto &idx : shape.mesh.indices) {
            VertKey key{idx.vertex_index, idx.texcoord_index, idx.normal_index};

            auto it = vertMap.find(key);
            if (it != vertMap.end()) {
                indices.push_back(it->second);
                continue;
            }

            // 创建新的 vertex
            MeshVertex v{};

            // position
            v.position = {
                attrib.vertices[3 * key.v + 0],
                attrib.vertices[3 * key.v + 1],
                attrib.vertices[3 * key.v + 2],
            };

            // texcoord (缺省则用 0)
            if (key.vt >= 0 && key.vt < static_cast<int>(attrib.texcoords.size() / 2)) {
                v.uv = {
                    attrib.texcoords[2 * key.vt + 0],
                    1.0f - attrib.texcoords[2 * key.vt + 1], // Vulkan Y 翻转
                };
            } else {
                v.uv = {0.0f, 0.0f};
            }

            // normal (缺省则算 0)
            if (key.vn >= 0 && key.vn < static_cast<int>(attrib.normals.size() / 3)) {
                v.normal = {
                    attrib.normals[3 * key.vn + 0],
                    attrib.normals[3 * key.vn + 1],
                    attrib.normals[3 * key.vn + 2],
                };
            } else {
                v.normal = {0.0f, 0.0f, 1.0f};
            }

            uint32_t newIdx = static_cast<uint32_t>(vertices.size());
            vertMap[key] = newIdx;
            vertices.push_back(v);
            indices.push_back(newIdx);
        }
    }

    GE_CORE_INFO("Model loaded: {} ({} shapes, {} source vertices, {} triangles, deduped to {} verts)",
                 filepath, shapes.size(), attrib.vertices.size() / 3,
                 totalTriangles, vertices.size());

    BuildMesh(allocator, vertices, indices);

    // 警告信息
    if (!reader.Warning().empty()) {
        GE_CORE_WARN("Model::LoadFromFile warning: {}", reader.Warning());
    }
}

void Model::Cleanup() {
    m_Mesh.Destroy();
}

} // namespace GE
