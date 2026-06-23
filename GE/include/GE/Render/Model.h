#pragma once

#include <vector>
#include <string>
#include <vulkan/vulkan.hpp>
#include "vk_mem_alloc.h"

#include "glm/glm.hpp"
#include "Render/Mesh.h"

namespace GE {

/// 网格中的一段索引范围，对应一个材质。
struct SubMesh {
    uint32_t indexOffset = 0;  // 在全局 index buffer 中的起始位置
    uint32_t indexCount  = 0;  // 该子网格的索引数量
    int32_t  materialIndex = -1;  // 指向 MaterialData 数组，-1 = 无材质
};

/// 从 .mtl 文件加载的材质信息（CPU 侧）。
struct MaterialData {
    std::string name;
    glm::vec3 ambient  {0.0f, 0.0f, 0.0f};
    glm::vec3 diffuse  {0.8f, 0.8f, 0.8f};
    glm::vec3 specular {0.0f, 0.0f, 0.0f};
    float shininess = 32.0f;
    float dissolve  = 1.0f;
    std::string diffuseTexPath;  // map_Kd
};

/// Model 从 .obj 文件加载或程序化生成网格，上传到 GPU 成为 Mesh。
/// 支持按材质拆分子网格（SubMesh）。
class Model {
public:
    Model() = default;

    ~Model() { Cleanup(); }

    Model(const Model &) = delete;

    Model &operator=(const Model &) = delete;

    Model(Model &&) = default;

    Model &operator=(Model &&) = default;

    /// 从 .obj 文件加载，上传到 GPU，同时解析 .mtl 材质信息。
    void LoadFromFile(VmaAllocator allocator, const std::string &filepath);

    /// 生成一个细分球体（icosphere）。
    void CreateSphere(VmaAllocator allocator, float radius = 1.0f, uint16_t subdivisions = 3);

    void Cleanup();

    [[nodiscard]] const Mesh &GetMesh() const { return m_Mesh; }
    [[nodiscard]] const std::vector<SubMesh>     &GetSubMeshes() const { return m_SubMeshes; }
    [[nodiscard]] const std::vector<MaterialData> &GetMaterials() const { return m_Materials; }

private:
    void BuildMesh(VmaAllocator allocator,
                   const std::vector<MeshVertex> &vertices,
                   const std::vector<uint32_t> &indices);

    Mesh m_Mesh;
    std::vector<SubMesh>     m_SubMeshes;
    std::vector<MaterialData> m_Materials;
};

} // namespace GE
