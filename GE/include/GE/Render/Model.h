#pragma once

#include <vector>
#include <string>
#include <vulkan/vulkan.hpp>
#include "vk_mem_alloc.h"

#include "Render/Mesh.h"

namespace GE {

/// Model 从 .obj 文件加载或程序化生成网格，上传到 GPU 成为 Mesh。
class Model {
public:
    Model() = default;

    ~Model() { Cleanup(); }

    Model(const Model &) = delete;

    Model &operator=(const Model &) = delete;

    Model(Model &&) = default;

    Model &operator=(Model &&) = default;

    /// 从 .obj 文件加载，上传到 GPU。
    void LoadFromFile(VmaAllocator allocator, const std::string &filepath);

    /// 生成一个细分球体（icosphere）作为测试用 3D 模型。
    void CreateSphere(VmaAllocator allocator, float radius = 1.0f, uint16_t subdivisions = 3);

    void Cleanup();

    [[nodiscard]] const Mesh &GetMesh() const { return m_Mesh; }

private:
    void BuildMesh(VmaAllocator allocator,
                   const std::vector<MeshVertex> &vertices,
                   const std::vector<uint32_t> &indices);

    Mesh m_Mesh;
};

} // namespace GE
