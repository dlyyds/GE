/**
 * @file Mesh.h
 * @brief 网格封装 —— 顶点缓冲 + 索引缓冲，支持从文件加载。
 *
 * 封装 VulkanBuffer（顶点 + 索引），提供：
 * - LoadFromFile()：从 OBJ 文件加载网格（使用 tinyobjloader）
 * - 直接构造：从 CPU 端顶点/索引数组创建网格
 *
 * 内部自动处理：
 * - staging buffer 上传顶点/索引数据到 GPU
 * - 顶点布局为 Position(3) + Normal(3) + TexCoord(2)
 */

#pragma once

#include "Render/VulkanBase/VulkanBuffer.h"
#include "Render/VulkanBase/VulkanDevice.h"

#include <vulkan/vulkan.hpp>

#include <cstdint>
#include <cstddef>
#include <functional>
#include <memory>
#include <string>
#include <vector>
#include <glm/glm.hpp>

namespace GE {

/**
 * @brief 顶点数据结构：位置 + 法线 + 纹理坐标。
 */
struct Vertex {
    glm::vec3 Position{0.0f};   ///< 位置
    glm::vec3 Normal{0.0f};     ///< 法线
    glm::vec2 TexCoord{0.0f};   ///< 纹理坐标

    bool operator==(const Vertex &other) const {
        return Position == other.Position
            && Normal == other.Normal
            && TexCoord == other.TexCoord;
    }
};

} // namespace GE

// Vertex 的哈希函数（用于 unordered_map 去重）
namespace std {
template <>
struct hash<GE::Vertex> {
    size_t operator()(const GE::Vertex &v) const {
        size_t h1 = hash<float>()(v.Position.x);
        size_t h2 = hash<float>()(v.Position.y);
        size_t h3 = hash<float>()(v.Position.z);
        size_t h4 = hash<float>()(v.Normal.x);
        size_t h5 = hash<float>()(v.Normal.y);
        size_t h6 = hash<float>()(v.Normal.z);
        size_t h7 = hash<float>()(v.TexCoord.x);
        size_t h8 = hash<float>()(v.TexCoord.y);
        // 简易组合哈希
        return h1 ^ (h2 << 1) ^ (h3 << 2) ^ (h4 << 3) ^ (h5 << 4) ^ (h6 << 5) ^ (h7 << 6) ^ (h8 << 7);
    }
};
} // namespace std

namespace GE {

/**
 * @brief 网格封装 —— 持有顶点缓冲 + 索引缓冲。
 *
 * 使用方式：
 * @code
 *   auto mesh = Mesh::LoadFromFile(device, "assets/meshes/cube.obj");
 *   if (mesh) {
 *       // 绑定顶点缓冲 + 索引缓冲并绘制
 *       vkCmdBindVertexBuffers(cmdBuf, 0, 1, &mesh->GetVertexBuffer().GetHandle(), &offset);
 *       vkCmdBindIndexBuffer(cmdBuf, mesh->GetIndexBuffer().GetHandle(), 0, vk::IndexType::eUint32);
 *       vkCmdDrawIndexed(cmdBuf, mesh->GetIndexCount(), 1, 0, 0, 0);
 *   }
 * @endcode
 */
class Mesh {
public:
    // ========================================================================
    // 工厂方法
    // ========================================================================

    /**
     * @brief 从 OBJ 文件加载网格。
     *
     * @param device    Vulkan 设备
     * @param filepath  OBJ 文件路径
     * @return std::unique_ptr<Mesh>  失败时返回 nullptr
     */
    static std::unique_ptr<Mesh> LoadFromFile(VulkanDevice &device,
                                              const std::string &filepath);

    /**
     * @brief 从 CPU 端顶点/索引数组创建网格。
     *
     * @param device   Vulkan 设备
     * @param vertices 顶点数组
     * @param indices  索引数组（uint32_t）
     * @return std::unique_ptr<Mesh>
     */
    static std::unique_ptr<Mesh> Create(VulkanDevice &device,
                                        const std::vector<Vertex> &vertices,
                                        const std::vector<uint32_t> &indices);

    // ========================================================================
    // 析构 / 移动
    // ========================================================================

    ~Mesh();

    Mesh(Mesh &&other) noexcept;
    Mesh(const Mesh &) = delete;
    Mesh &operator=(Mesh &&) = delete;
    Mesh &operator=(const Mesh &) = delete;

    // ========================================================================
    // 访问器
    // ========================================================================

    VulkanBuffer &GetVertexBuffer() { return *m_VertexBuffer; }
    const VulkanBuffer &GetVertexBuffer() const { return *m_VertexBuffer; }

    VulkanBuffer &GetIndexBuffer() { return *m_IndexBuffer; }
    const VulkanBuffer &GetIndexBuffer() const { return *m_IndexBuffer; }

    uint32_t GetVertexCount() const { return static_cast<uint32_t>(m_Vertices.size()); }
    uint32_t GetIndexCount() const { return static_cast<uint32_t>(m_Indices.size()); }

    const std::vector<Vertex> &GetVertices() const { return m_Vertices; }
    const std::vector<uint32_t> &GetIndices() const { return m_Indices; }

    /**
     * @brief 设置调试名称（同时作用于顶点缓冲和索引缓冲）。
     *
     * 顶点缓冲 → name + "_VB"
     * 索引缓冲 → name + "_IB"
     */
    void SetDebugName(const std::string &name);

private:
    // ========================================================================
    // 私有构造
    // ========================================================================

    Mesh() = default;

    /**
     * @brief 从 CPU 端顶点/索引数据创建 GPU 缓冲。
     */
    bool UploadToGPU(VulkanDevice &device);

    // ========================================================================
    // 成员
    // ========================================================================

    std::vector<Vertex>   m_Vertices;     ///< CPU 端顶点数据
    std::vector<uint32_t> m_Indices;      ///< CPU 端索引数据

    std::unique_ptr<VulkanBuffer> m_VertexBuffer; ///< GPU 顶点缓冲
    std::unique_ptr<VulkanBuffer> m_IndexBuffer;  ///< GPU 索引缓冲
};

} // namespace GE
