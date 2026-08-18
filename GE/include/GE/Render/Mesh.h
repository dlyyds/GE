/**
 * @file Mesh.h
 * @brief 网格封装 —— 顶点缓冲 + 索引缓冲 + 轻量摘要 + 被动异步状态。
 *
 * 只承载「可渲染资源」所需的全部：GPU 缓冲、渲染范围子网格、材质匹配摘要与
 * 异步加载生命周期。格式解析（OBJ/glTF）、加载编排（同步/异步分派）与内置几何
 * 生成已外移到 ModelLoader / OBJLoader / MeshManager，Mesh 不感知任何文件格式。
 */

#pragma once

#include "Render/VulkanBase/VulkanBuffer.h"
#include "Render/VulkanBase/VulkanDevice.h"

#include <vulkan/vulkan.hpp>

#include <atomic>
#include <cstdint>
#include <cstddef>
#include <cmath>
#include <memory>
#include <string>
#include <vector>
#include <glm/glm.hpp>

namespace GE {

class Material;

/**
 * @brief 从 OBJ/MTL 捕获的材质数据（POD，与 tinyobjloader 解耦）。
 *
 * 在 ParseOBJData 解析 MTL 时填充（同步 / 异步路径共用），供 MeshManager 按子网格的
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
     * 在格式解析构造顶点时一次性量化清洗数据，之后 operator==
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
        // 顶点已在格式解析时量化清洗过，这里直接对位模式精确哈希
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
 * @brief 格式解析器的统一输出（纯 CPU 载荷，构建期传递对象）。
 *
 * 所有格式解析器（OBJ / 未来的 glTF / FBX）输出同一种形式。vertices/indices 是
 * 构建期数据：完成切线计算与 GPU 上传后即随本对象析构释放，Mesh 只保留子网格、
 * 材质数据与顶点/索引计数等轻量摘要，整份 CPU 顶点副本不常驻内存。
 */
struct MeshData {
    std::vector<Vertex>      vertices;      ///< 已量化 + 去重（构建期用，上传后释放）
    std::vector<uint32_t>    indices;       ///< 索引数组
    std::vector<SubMesh>     subMeshes;     ///< 渲染范围 + materialName
    std::vector<MaterialData> materialData; ///< 材质匹配用（MeshManager 读取）
};

/**
 * @brief 网格封装 —— 持有顶点缓冲 + 索引缓冲，只负责可渲染资源与被动异步状态。
 *
 * 使用方式：
 * @code
 *   // 经 MeshManager / ModelLoader 加载，Mesh 自身不感知文件格式
 *   auto &meshMgr = Renderer::GetMeshManager();
 *   Mesh* mesh = meshMgr.GetBuiltin("cube");
 *   if (mesh && mesh->IsReady()) {
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
     * @brief 从 CPU 端解析数据（MeshData）同步创建网格。
     *
     * 完成切线计算、GPU 上传与摘要提取（顶点/索引计数、子网格、材质数据），
     * 创建结果立即就绪（IsReady()==true）。MeshData 为右值引用：其 vertices/indices
     * 在上传完成后随析构释放，Mesh 不持有整份 CPU 顶点副本。
     *
     * @param device Vulkan 设备
     * @param data   格式解析/几何生成产出的 CPU 载荷（std::move 入）
     * @return std::unique_ptr<Mesh>  数据为空或上传失败时返回 nullptr
     */
    static std::unique_ptr<Mesh> Create(VulkanDevice &device, MeshData &&data);

    /**
     * @brief 创建异步加载"空壳"网格（带注入槽位，IsReady()==false）。
     *
     * 造空壳 + 初始化 AsyncPendingSlot 的动作被保留在 Mesh（惰性状态机长在 Mesh），
     * 加载编排则由 MeshManager 接管：创建空壳后组装 UploadTask 提交后台，主线程
     * finalize 经槽位门控后调用 InstallAsyncData 注入 + BuildSubMeshMaterials + MarkReady。
     *
     * @param filepath 源文件路径（记录到网格，便于序列化）
     * @return 待注入的空壳 Mesh（不持 GPU 缓冲与 CPU 数据）
     */
    static std::unique_ptr<Mesh> CreateShell(const std::string &filepath);

    // ========================================================================
    // 析构 / 移动
    // ========================================================================

    ~Mesh();

    Mesh(Mesh &&other) noexcept;
    Mesh(const Mesh &) = delete;
    Mesh &operator=(Mesh &&) = delete;
    Mesh &operator=(const Mesh &) = delete;

    // ========================================================================
    // 就绪状态 / 注入（异步加载） —— 被动状态机保留在 Mesh
    // ========================================================================

    /**
     * @brief 异步加载注入槽位。
     *
     * 由空壳 Mesh 持有（shared_ptr），异步任务 finalize 亦持有同份引用。空壳被
     * 销毁时（如 MeshManager::Unload）析构函数将其标记作废，finalize 据此安全
     * 跳过注入与材质构建，避免对已销毁目标的 use-after-free。主线程 Poll() 与
     * unload 均跑在主线程，故 target/abandoned 无需原子。
     */
    struct AsyncPendingSlot {
        Mesh *target = nullptr;   ///< 待注入的目标空壳（析构时置空）
        bool abandoned = false;   ///< 目标已销毁，finalize 应跳过
    };

    /**
     * @brief 网格是否已完全就绪（GPU 缓冲 + 子网格材质可用）。
     *
     * 异步工厂返回的空壳网格在后台加载完成、主线程注入前为 false。渲染端绑定前
     * 必须检查：未就绪时跳过绘制。同步工厂 / 内置几何体创建的网格恒为 true。
     */
    bool IsReady() const { return m_Ready.load(std::memory_order_acquire); }

    /**
     * @brief 主线程置网格为就绪（注入与材质构建完成后调用）。
     *
     * 由 MeshManager 的 UploadTask finalize（主线程 Poll()）调用，置于
     * InstallAsyncData 与 BuildSubMeshMaterials 之后，保证渲染看到就绪网格时
     * 缓冲与材质均已安装完毕，下帧渲染自动可见。
     */
    void MarkReady() { m_Ready.store(true, std::memory_order_release); }

    /**
     * @brief 获取异步注入槽位（供编排方 finalize 门控，MeshManager 读取）。
     */
    std::shared_ptr<AsyncPendingSlot> GetAsyncSlot() const { return m_AsyncSlot; }

    /**
     * @brief 主线程安装后台异步加载完成的数据与 GPU 缓冲。
     *
     * 仅由异步任务 finalize（主线程 Poll()）调用，随后由调用方按序执行
     * BuildSubMeshMaterials 与 MarkReady。若本网格已被销毁（经 AsyncPendingSlot
     * 门控），调用方会跳过本方法。MeshData 的 vertices/indices 属构建期数据，
     * 安装时只回收计数与子网格/材质摘要，原始 CPU 数组随之释放。
     *
     * @param data          后台解析的 CPU 载荷（std::move 入，仅取摘要后释放顶点数组）
     * @param vertexBuffer  后台创建的本地顶点缓冲（接管所有权）
     * @param indexBuffer   后台创建的本地索引缓冲（接管所有权）
     */
    void InstallAsyncData(MeshData &&data,
                          std::unique_ptr<VulkanBuffer> vertexBuffer,
                          std::unique_ptr<VulkanBuffer> indexBuffer);

    // ========================================================================
    // 访问器
    // ========================================================================

    VulkanBuffer &GetVertexBuffer() { return *m_VertexBuffer; }
    const VulkanBuffer &GetVertexBuffer() const { return *m_VertexBuffer; }

    VulkanBuffer &GetIndexBuffer() { return *m_IndexBuffer; }
    const VulkanBuffer &GetIndexBuffer() const { return *m_IndexBuffer; }

    /// 顶点数量（摘要计数，Mesh 不持有整份 CPU 顶点数组）
    uint32_t GetVertexCount() const { return m_VertexCount; }
    /// 索引数量（摘要计数，Mesh 不持有整份 CPU 索引数组）
    uint32_t GetIndexCount() const { return m_IndexCount; }

    /**
     * @brief 获取子网格列表。
     *
     * 所有 mesh 至少包含一个子网格（含内置几何体 / CPU 直建，均生成一个
     * 覆盖全部索引的子网格）。渲染统一按子网格路径绘制。
     */
    const std::vector<SubMesh> &GetSubMeshes() const { return m_SubMeshes; }

    /**
     * @brief 获取从 MTL 捕获的材质数据（加载时填充）。
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
     * @brief 由格式解析/几何生成产出的 MeshData 构建最终网格（共享装配路径）。
     *
     * 完成切线计算、GPU 上传、摘要提取（顶点/索引计数）、文件路径记录。同步路径
     * （Mesh::Create）与内置几何体共用。异步路径在后台完成解析与上传后经
     * InstallAsyncData 注入，不走本函数。
     *
     * @param device   Vulkan 设备
     * @param data     CPU 载荷（std::move 入，上传后释放顶点数组）
     * @param filePath 源文件路径（std::move 入）
     * @return std::unique_ptr<Mesh>  数据为空或上传失败时返回 nullptr
     */
    static std::unique_ptr<Mesh> BuildMesh(VulkanDevice &device,
                                           MeshData &&data,
                                           std::string filePath);

    // ========================================================================
    // 成员
    // ========================================================================

    std::vector<SubMesh>  m_SubMeshes;      ///< 渲染范围（含 defaultMaterial 指针）
    std::vector<MaterialData> m_MaterialData; ///< 从 MTL 捕获的材质数据（加载时填充）
    uint32_t m_VertexCount = 0;             ///< 顶点数量摘要（替代整份 CPU 顶点数组）
    uint32_t m_IndexCount  = 0;             ///< 索引数量摘要（替代整份 CPU 索引数组）
    std::string           m_FilePath;       ///< 源文件路径（从文件加载时有值）

    std::unique_ptr<VulkanBuffer> m_VertexBuffer; ///< GPU 顶点缓冲
    std::unique_ptr<VulkanBuffer> m_IndexBuffer;  ///< GPU 索引缓冲

    // 异步加载状态（空壳网格专用；同步路径构造后 m_Ready 恒 true）
    std::atomic<bool> m_Ready{false}; ///< 异步加载是否已就绪
    std::shared_ptr<AsyncPendingSlot> m_AsyncSlot; ///< 异步注入槽位（空壳网格持有）
};

} // namespace GE