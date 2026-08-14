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
 * @brief 顶点数据结构：位置 + 法线 + 纹理坐标 + 切线。
 *
 * 内存布局必须与顶点着色器的输入 location 顺序一致（由着色器反射紧密打包）：
 *   location 0: Position (vec3, 12B)
 *   location 1: Normal   (vec3, 12B)
 *   location 2: TexCoord (vec2,  8B)
 *   location 3: Tangent  (vec4, 16B) —— xyz=切线方向，w=手性符号(+1/-1)
 * 总大小 48B。
 */
struct Vertex {
    glm::vec3 Position{0.0f};   ///< 位置
    glm::vec3 Normal{0.0f};     ///< 法线
    glm::vec2 TexCoord{0.0f};   ///< 纹理坐标
    glm::vec4 Tangent{0.0f, 0.0f, 0.0f, 1.0f}; ///< 切线(xyz) + 手性符号(w)

    bool operator==(const Vertex &other) const {
        return Position == other.Position
            && Normal == other.Normal
            && TexCoord == other.TexCoord
            && Tangent == other.Tangent;
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
        size_t h9 = hash<float>()(v.Tangent.x);
        size_t h10 = hash<float>()(v.Tangent.y);
        size_t h11 = hash<float>()(v.Tangent.z);
        size_t h12 = hash<float>()(v.Tangent.w);
        // 简易组合哈希
        return h1 ^ (h2 << 1) ^ (h3 << 2) ^ (h4 << 3) ^ (h5 << 4) ^ (h6 << 5) ^ (h7 << 6) ^ (h8 << 7)
             ^ (h9 << 8) ^ (h10 << 9) ^ (h11 << 10) ^ (h12 << 11);
    }
};
} // namespace std

namespace GE {

/**
 * @brief 网格封装 —— 持有顶点缓冲 + 索引缓冲。
 *
 * 使用方式：
 * @code
 *   auto mesh = Mesh::LoadFromFile(device, "assets/models/cube.obj");
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

    /**
     * @brief 创建内置几何体网格。
     *
     * 支持的类型（type 参数）：
     * - "cube"   : 立方体（边长为 2，中心在原点）
     * - "plane"  : 平面（XY 平面，边长为 2，中心在原点，法线 +Z）
     * - "sphere" : 球体（半径为 1，中心在原点）
     * - "quad"   : 四边形（XY 平面，2×2，中心在原点）
     *
     * @param device  Vulkan 设备
     * @param type    内置几何体类型名称
     * @return std::unique_ptr<Mesh>  失败（未知类型）返回 nullptr
     */
    static std::unique_ptr<Mesh> CreateBuiltin(VulkanDevice &device, const std::string &type);

    /**
     * @brief 判断路径是否为内置几何体标识（"builtin:" 前缀）。
     */
    static bool IsBuiltinPath(const std::string &path) {
        return path.rfind("builtin:", 0) == 0;
    }

    /**
     * @brief 从内置路径中提取类型名（去掉 "builtin:" 前缀）。
     */
    static std::string GetBuiltinType(const std::string &path) {
        if (IsBuiltinPath(path)) {
            return path.substr(8); // "builtin:" 长度为 8
        }
        return {};
    }

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
     * @brief 获取网格源文件路径。
     *
     * - 从文件加载的网格：返回文件路径
     * - 内置几何体：返回 "builtin:<type>"
     * - 代码直接创建的网格：返回空字符串
     */
    const std::string &GetFilePath() const { return m_FilePath; }

    /**
     * @brief 手动设置网格文件路径（用于代码生成的网格标记为内置几何体等）。
     */
    void SetFilePath(const std::string &path) { m_FilePath = path; }

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
    std::string           m_FilePath;     ///< 源文件路径（LoadFromFile 时有值）

    std::unique_ptr<VulkanBuffer> m_VertexBuffer; ///< GPU 顶点缓冲
    std::unique_ptr<VulkanBuffer> m_IndexBuffer;  ///< GPU 索引缓冲
};

} // namespace GE
