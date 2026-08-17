/**
 * @file Mesh.h
 * @brief 网格封装 —— 顶点缓冲 + 索引缓冲，支持从文件加载。
 *
 * 封装 VulkanBuffer（顶点 + 索引），提供：
 * - LoadFromFile()：从模型文件加载网格（按扩展名分派，当前支持 OBJ）
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
#include <cmath>
#include <functional>
#include <memory>
#include <string>
#include <vector>
#include <glm/glm.hpp>

namespace GE {

class Material;

/**
 * @brief 从 OBJ/MTL 捕获的材质数据（POD，与 tinyobjloader 解耦）。
 *
 * 在 Mesh::LoadFromOBJ 解析 MTL 时填充，供 MeshManager 按子网格的
 * materialName 匹配后构建真正的 Material（含纹理加载）。纹理路径已
 * 在此处解析为绝对路径（相对 OBJ 所在目录）。
 */
struct MaterialData {
    std::string name;               ///< MTL 材质名（与 SubMesh::materialName 对应）
    glm::vec3   baseColor{1.0f};    ///< 漫反射颜色（Kd）
    glm::vec3   specular{0.0f};     ///< 高光颜色（Ks）
    glm::vec3   emissive{0.0f};     ///< 自发光颜色（Ke）
    float       shininess  = 32.0f; ///< 高光指数（Ns）
    float       dissolve   = 1.0f;  ///< 不透明度（d，1=不透明）
    float       metallic   = 0.0f;  ///< 金属度（Pm，PBR 扩展）
    float       roughness  = 0.5f;  ///< 粗糙度（Pr，PBR 扩展）
    bool        hasPBR     = false; ///< 是否含 PBR 扩展参数（决定材质类型）

    std::string albedoMap;    ///< 漫反射贴图（map_Kd，绝对路径）
    std::string normalMap;    ///< 法线贴图（map_bump / norm，绝对路径）
    std::string emissiveMap;  ///< 自发光贴图（map_Ke，绝对路径）
    std::string metallicMap;  ///< 金属度贴图（map_Pm，绝对路径）
    std::string roughnessMap; ///< 粗糙度贴图（map_Pr，绝对路径）
};

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

    /**
     * @brief 量化精度：将坐标投影到 1/10000 的均匀网格上。
     *
     * 用于消除浮点精度误差导致的「逻辑相同但位表示不同」的顶点。
     * 在 LoadFromFile 构造顶点时一次性量化清洗数据，之后 operator==
     * 与 hash 即可安全地对位模式做精确比较。
     */
    static constexpr float kQuantScale = 10000.0f;

    /// 将单个浮点量化到网格上（roundf 取整后除以精度，结果位模式确定）
    static float Quantize(float v) { return std::roundf(v * kQuantScale) / kQuantScale; }
    static glm::vec3 Quantize(const glm::vec3 &v) {
        return {Quantize(v.x), Quantize(v.y), Quantize(v.z)};
    }
    static glm::vec2 Quantize(const glm::vec2 &v) {
        return {Quantize(v.x), Quantize(v.y)};
    }
    static glm::vec4 Quantize(const glm::vec4 &v) {
        return {Quantize(v.x), Quantize(v.y), Quantize(v.z), Quantize(v.w)};
    }

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
        // 顶点已在 LoadFromFile 时量化清洗过，这里直接对位模式精确哈希
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
 * @brief 子网格：共享同一顶点/索引缓冲，仅用索引范围区分，带默认材质。
 *
 * 一个 Mesh 可含多个子网格（如 OBJ 按 (shape, material_id) 拆分、未来 glTF
 * 按 primitive）。所有子网格共用 Mesh 持有的单个顶点缓冲 + 索引缓冲，
 * 仅通过 firstIndex / indexCount 划定各自的索引范围进行绘制。
 *
 * defaultMaterial 为模型加载时的默认材质（MaterialManager 持有，只读借用）；
 * 实体可通过 MeshRendererComponent.materialOverrides 覆写某个子网格的材质。
 * materialName 为加载期记录的源材质名（OBJ 材质名 / 未来 glTF 材质名）。
 */
struct SubMesh {
    uint32_t  firstVertex = 0;   ///< 顶点缓冲起始（共享缓冲，通常为 0）
    uint32_t  vertexCount = 0;   ///< 顶点数量
    uint32_t  firstIndex  = 0;   ///< 索引缓冲起始（元素索引，非字节）
    uint32_t  indexCount  = 0;   ///< 索引数量
    std::string materialName;    ///< 源材质名（加载期填充，用于创建/查找材质）
    Material *defaultMaterial = nullptr; ///< 默认材质（MeshManager 填充，MaterialManager 持有）
};

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
     * @brief 从模型文件加载网格（按文件扩展名分派到对应格式加载器）。
     *
     * 当前支持的格式：
     * - .obj：使用 tinyobjloader 解析
     *
     * 后续接入 glTF / FBX 等新格式时，只需在 LoadFromFile 中按扩展名
     * 注册对应的私有加载函数；各格式解析出的数据统一交给 BuildMesh
     * 完成切线计算 / GPU 上传等格式无关的装配流程。
     *
     * @param device    Vulkan 设备
     * @param filepath  模型文件路径
     * @return std::unique_ptr<Mesh>  失败或不支持的格式时返回 nullptr
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
     * @brief 获取子网格列表。
     *
     * 所有 mesh 至少包含一个子网格（含内置几何体 / CPU 直建，均生成一个
     * 覆盖全部索引的子网格）。渲染统一按子网格路径绘制。
     */
    const std::vector<SubMesh> &GetSubMeshes() const { return m_SubMeshes; }

    /**
     * @brief 获取从 MTL 捕获的材质数据（LoadFromFile 填充）。
     *
     * 供 MeshManager 构建子网格材质时，按 materialName 匹配对应的材质属性。
     * 无 MTL 的网格（内置几何体 / CPU 直建）该列表为空。
     */
    const std::vector<MaterialData> &GetMaterialData() const { return m_MaterialData; }

    /**
     * @brief 设置指定子网格的默认材质。
     *
     * @param index 子网格索引
     * @param mat   默认材质指针（由 MaterialManager 持有，Mesh 不拥有，只读）
     */
    void SetSubMeshDefaultMaterial(uint32_t index, Material *mat) {
        if (index < m_SubMeshes.size()) {
            m_SubMeshes[index].defaultMaterial = mat;
        }
    }

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
    // 格式加载器（LoadFromFile 按扩展名分派到各私有加载函数）
    // ========================================================================

    /**
     * @brief 从 OBJ 文件加载（tinyobjloader 解析 + MTL 材质捕获）。
     *
     * @param device    Vulkan 设备
     * @param filepath  OBJ 文件路径
     * @return std::unique_ptr<Mesh>  解析失败时返回 nullptr
     */
    static std::unique_ptr<Mesh> LoadFromOBJ(VulkanDevice &device,
                                             const std::string &filepath);

    /**
     * @brief 由格式加载器解析出的几何数据构建最终网格（共享装配路径）。
     *
     * 完成切线计算、GPU 上传与文件路径记录。OBJ / 未来的 glTF 等
     * 各格式加载器解析出顶点、索引、子网格与材质数据后统一调用本函数，
     * 避免每个格式重复装配逻辑。
     *
     * @param device        Vulkan 设备
     * @param vertices      顶点数组（std::move 入）
     * @param indices       索引数组（std::move 入）
     * @param subMeshes     子网格列表（std::move 入）
     * @param materialData  材质数据（std::move 入）
     * @param filePath      源文件路径（std::move 入）
     * @return std::unique_ptr<Mesh>  数据为空或上传失败时返回 nullptr
     */
    static std::unique_ptr<Mesh> BuildMesh(VulkanDevice &device,
                                           std::vector<Vertex> vertices,
                                           std::vector<uint32_t> indices,
                                           std::vector<SubMesh> subMeshes,
                                           std::vector<MaterialData> materialData,
                                           std::string filePath);

    // ========================================================================
    // 成员
    // ========================================================================

    std::vector<Vertex>   m_Vertices;     ///< CPU 端顶点数据
    std::vector<uint32_t> m_Indices;      ///< CPU 端索引数据
    std::vector<SubMesh>  m_SubMeshes;    ///< 子网格列表（空 = 单整体网格）
    std::vector<MaterialData> m_MaterialData; ///< 从 MTL 捕获的材质数据（LoadFromFile 填充）
    std::string           m_FilePath;     ///< 源文件路径（LoadFromFile 时有值）

    std::unique_ptr<VulkanBuffer> m_VertexBuffer; ///< GPU 顶点缓冲
    std::unique_ptr<VulkanBuffer> m_IndexBuffer;  ///< GPU 索引缓冲
};

} // namespace GE
