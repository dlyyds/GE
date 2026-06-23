//
// Created by Lenovo on 2026/6/11.
//

#include "Render/Model.h"
#include "Core/Log.h"

#include "tiny_obj_loader.h"

#include <unordered_map>
#include <cstring>
#include <filesystem>

namespace GE {

void Model::BuildMesh(VmaAllocator allocator,
                      const std::vector<MeshVertex> &vertices,
                      const std::vector<uint32_t> &indices) {
    // 转为 16-bit 索引（65536 个顶点以内用 uint16_t 省一半显存）
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

    // ---------- 1. 加载 .mtl 材质信息 ----------
    auto &mtlMaterials = reader.GetMaterials();
    m_Materials.clear();
    m_Materials.reserve(mtlMaterials.size());
    for (auto &mtl : mtlMaterials) {
        MaterialData md;
        md.name       = mtl.name;
        md.ambient    = {mtl.ambient[0], mtl.ambient[1], mtl.ambient[2]};
        md.diffuse    = {mtl.diffuse[0], mtl.diffuse[1], mtl.diffuse[2]};
        md.specular   = {mtl.specular[0], mtl.specular[1], mtl.specular[2]};
        md.shininess  = mtl.shininess;
        md.dissolve   = mtl.dissolve;

        // 纹理路径：相对于 .obj 目录解析
        if (!mtl.diffuse_texname.empty()) {
            std::filesystem::path modelDir = std::filesystem::path(filepath).parent_path();
            md.diffuseTexPath = (modelDir / mtl.diffuse_texname).string();
        }
        m_Materials.push_back(md);
    }

    // ---------- 2. 顶点去重 + 按材质拆 SubMesh ----------
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

    m_SubMeshes.clear();
    int currentMaterialId = -1;
    size_t totalTriangles = 0;

    for (auto &shape : shapes) {
        size_t faceCount = shape.mesh.num_face_vertices.size();
        totalTriangles += faceCount;

        for (size_t f = 0; f < faceCount; f++) {
            int matId = shape.mesh.material_ids[f];
            // material_ids 中的 -1 表示该面没有材质，映射到最后一个材质或单独处理
            if (matId < 0) matId = -1;

            // 材质切换 → 开始新的 SubMesh
            if (matId != currentMaterialId || m_SubMeshes.empty()) {
                // 关闭上一个 SubMesh
                if (!m_SubMeshes.empty()) {
                    m_SubMeshes.back().indexCount =
                        static_cast<uint32_t>(indices.size() - m_SubMeshes.back().indexOffset);
                }
                SubMesh sm;
                sm.indexOffset   = static_cast<uint32_t>(indices.size());
                sm.indexCount    = 0;
                sm.materialIndex = matId;
                m_SubMeshes.push_back(sm);
                currentMaterialId = matId;
            }

            // 处理该面的 3 个顶点索引
            for (int v = 0; v < 3; v++) {
                auto &idx = shape.mesh.indices[f * 3 + v];
                VertKey key{idx.vertex_index, idx.texcoord_index, idx.normal_index};

                auto it = vertMap.find(key);
                if (it != vertMap.end()) {
                    indices.push_back(it->second);
                    continue;
                }

                // 创建新的 vertex
                MeshVertex vert{};

                // position
                vert.position = {
                    attrib.vertices[3 * key.v + 0],
                    attrib.vertices[3 * key.v + 1],
                    attrib.vertices[3 * key.v + 2],
                };

                // texcoord (缺省则用 0)
                if (key.vt >= 0 && key.vt < static_cast<int>(attrib.texcoords.size() / 2)) {
                    vert.uv = {
                        attrib.texcoords[2 * key.vt + 0],
                        1.0f - attrib.texcoords[2 * key.vt + 1], // Vulkan Y 翻转
                    };
                } else {
                    vert.uv = {0.0f, 0.0f};
                }

                // normal (缺省则算 0)
                if (key.vn >= 0 && key.vn < static_cast<int>(attrib.normals.size() / 3)) {
                    vert.normal = {
                        attrib.normals[3 * key.vn + 0],
                        attrib.normals[3 * key.vn + 1],
                        attrib.normals[3 * key.vn + 2],
                    };
                } else {
                    vert.normal = {0.0f, 0.0f, 1.0f};
                }

                uint32_t newIdx = static_cast<uint32_t>(vertices.size());
                vertMap[key] = newIdx;
                vertices.push_back(vert);
                indices.push_back(newIdx);
            }
        }
    }

    // 关闭最后一个 SubMesh
    if (!m_SubMeshes.empty()) {
        m_SubMeshes.back().indexCount =
            static_cast<uint32_t>(indices.size() - m_SubMeshes.back().indexOffset);
    }

    GE_CORE_INFO("Model loaded: {} ({} shapes, {} materials, {} submeshes, {} triangles, deduped to {} verts)",
                 filepath, shapes.size(), m_Materials.size(),
                 m_SubMeshes.size(), totalTriangles, vertices.size());

    BuildMesh(allocator, vertices, indices);

    // 警告信息
    if (!reader.Warning().empty()) {
        GE_CORE_WARN("Model::LoadFromFile warning: {}", reader.Warning());
    }
}

void Model::Cleanup() {
    m_Mesh.Destroy();
    m_SubMeshes.clear();
    m_Materials.clear();
}

} // namespace GE
