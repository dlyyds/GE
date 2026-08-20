/**
 * @file GLTFLoader.cpp
 * @brief glTF 格式解析器实现（tinygltf v2）—— 装配 MeshData + 材质映射。
 *
 * 设计遵循 `docs/glTF模型导入计划书.md` §4B：
 * - LoadGLTFModel：统一载入 tinygltf::Model（.gltf→ASCII，.glb→Binary）。
 * - BuildGLTFMesh：把 model.meshes[meshIndex] 的每个 primitive 装配为一段连续
 *   子网格区间（primitive 属性流独立，故按段局部去重，firstVertex 真正指向段起点）。
 * - readAttr / readIndex：手写 accessor 内存解码。阶段 1 仅支持 FLOAT 顶点属性，
 *   索引支持 unsigned byte/short/int；int16/uint8 量化、normalized 留待阶段 2。
 * - AppendGLTFMaterial：metallic-roughness → MaterialData，贴图 URI 相对 glTF
 *   所在目录解析为绝对路径。
 */

#include "Render/GLTFLoader.h"

#include "Render/ModelLoader.h"
#include "Render/Mesh.h"
#include "Core/Log.h"

#include "tinygltf/tiny_gltf.h"

#include <filesystem>
#include <vector>
#include <cstring>
#include <glm/glm.hpp>

namespace GE {

namespace {

// ============================================================================
// accessor 内存解码
// ============================================================================

/**
 * @brief 返回 accessor 元素数据的起始字节指针（buffer + bufferView + accessor 偏移链）。
 *
 * bufferView 为 -1 的 accessor（纯 sparse / 无实际数据）返回 nullptr，调用方兜底。
 */
const uint8_t *AccessorBasePtr(const tinygltf::Model &m, const tinygltf::Accessor &acc) {
    if (acc.bufferView < 0 || acc.bufferView >= static_cast<int>(m.bufferViews.size())) {
        return nullptr;
    }
    const auto &bv = m.bufferViews[acc.bufferView];
    if (bv.buffer < 0 || bv.buffer >= static_cast<int>(m.buffers.size())) {
        return nullptr;
    }
    const auto &buf = m.buffers[bv.buffer];
    const size_t off = bv.byteOffset + acc.byteOffset;
    if (off >= buf.data.size()) {
        return nullptr;
    }
    return buf.data.data() + off;
}

/**
 * @brief 读取 accessor 第 i 个元素的前 n 个 float 分量到 out。
 *
 * 阶段 1 仅支持 TINYGLTF_COMPONENT_TYPE_FLOAT；其他类型（int16/uint8 量化、
 * normalized）清空输出并留 switch 分支待阶段 2 扩展。
 */
template <typename OutVec>
static void ReadFloatAttr(const tinygltf::Model &m, int accessorIdx, size_t i, OutVec &out) {
    constexpr int kN = sizeof(OutVec) / sizeof(float);
    float *dst = reinterpret_cast<float *>(&out);
    if (accessorIdx < 0 || accessorIdx >= static_cast<int>(m.accessors.size())) {
        std::memset(dst, 0, kN * sizeof(float));
        return;
    }
    const auto &acc = m.accessors[accessorIdx];
    const uint8_t *base = AccessorBasePtr(m, acc);
    int stride = acc.bufferView >= 0
        ? acc.ByteStride(m.bufferViews[acc.bufferView]) : -1;
    if (!base || stride < 0) {
        std::memset(dst, 0, kN * sizeof(float));
        return;
    }
    switch (acc.componentType) {
        case TINYGLTF_COMPONENT_TYPE_FLOAT: {
            const float *src = reinterpret_cast<const float *>(base + i * static_cast<size_t>(stride));
            for (int c = 0; c < kN; ++c) {
                dst[c] = src[c];
            }
            break;
        }
        // 阶段 2：int16/uint8 量化、normalized 在此分支解码
        default:
            std::memset(dst, 0, kN * sizeof(float));
            break;
    }
}

/**
 * @brief 读取索引 accessor 第 i 个元素（unsigned byte/short/int → uint32_t）。
 *
 * 用 memcpy 读取 16/32 位值，避免 glTF 缓冲可能未对齐导致的未对齐访问 UB。
 */
static uint32_t ReadIndex(const tinygltf::Model &m, int accessorIdx, size_t i) {
    if (accessorIdx < 0 || accessorIdx >= static_cast<int>(m.accessors.size())) {
        return 0;
    }
    const auto &acc = m.accessors[accessorIdx];
    const uint8_t *base = AccessorBasePtr(m, acc);
    int stride = acc.bufferView >= 0
        ? acc.ByteStride(m.bufferViews[acc.bufferView]) : -1;
    if (!base || stride < 0) {
        return 0;
    }
    const uint8_t *p = base + i * static_cast<size_t>(stride);
    switch (acc.componentType) {
        case TINYGLTF_COMPONENT_TYPE_UNSIGNED_BYTE:
            return *p;
        case TINYGLTF_COMPONENT_TYPE_UNSIGNED_SHORT: {
            uint16_t v;
            std::memcpy(&v, p, sizeof(uint16_t));
            return v;
        }
        case TINYGLTF_COMPONENT_TYPE_UNSIGNED_INT: {
            uint32_t v;
            std::memcpy(&v, p, sizeof(uint32_t));
            return v;
        }
        default:
            return 0;
    }
}

// ============================================================================
// 材质收集：metallic-roughness → MaterialData
// ============================================================================

/**
 * @brief 把 glTF 某个 material 捕获为 MaterialData 追加到 out（同名去重）。
 *
 * 贴图 URI 相对 glTF 文件所在目录解析为绝对路径；内嵌 bufferView 图像（GLB BIN）
 * 无 URI，返回值留空（安全默认，不因贴图缺失崩加载）。metallicRoughnessTexture
 * 与 OBJ 两张灰度图不兼容，暂不加载贴图、仅用标量 metallic/roughness。
 */
static void AppendGLTFMaterial(const tinygltf::Model &m, int materialIdx,
                               const std::string &filepath,
                               std::vector<MaterialData> &out) {
    if (materialIdx < 0 || materialIdx >= static_cast<int>(m.materials.size())) {
        return;
    }
    const tinygltf::Material &mat = m.materials[materialIdx];
    for (const auto &existing : out) {
        if (existing.name == mat.name) {
            return; // 多 primitive 共享同一材质 → 只留一份
        }
    }

    MaterialData md;
    md.name = mat.name;
    const auto &pbr = mat.pbrMetallicRoughness;
    md.baseColor = {static_cast<float>(pbr.baseColorFactor[0]),
                    static_cast<float>(pbr.baseColorFactor[1]),
                    static_cast<float>(pbr.baseColorFactor[2])};
    md.metallic  = static_cast<float>(pbr.metallicFactor);
    md.roughness = static_cast<float>(pbr.roughnessFactor);
    md.hasPBR    = true; // glTF 一律 PBR
    if (mat.emissiveFactor.size() >= 3) {
        md.emissive = {static_cast<float>(mat.emissiveFactor[0]),
                       static_cast<float>(mat.emissiveFactor[1]),
                       static_cast<float>(mat.emissiveFactor[2])};
    }

    // 贴图：TextureInfo.index → textures[i].source → images[src].uri → 绝对路径
    auto resTex = [&](int texIdx) -> std::string {
        if (texIdx < 0 || texIdx >= static_cast<int>(m.textures.size())) {
            return {};
        }
        const int src = m.textures[texIdx].source;
        if (src < 0 || src >= static_cast<int>(m.images.size())) {
            return {};
        }
        const std::string &uri = m.images[src].uri;
        if (uri.empty()) {
            return {}; // 内嵌 bufferView 图像无 URI，暂不加载
        }
        std::filesystem::path p(uri);
        if (p.is_absolute()) {
            return p.lexically_normal().string();
        }
        return (std::filesystem::path(filepath).parent_path() / uri)
            .lexically_normal().string();
    };
    md.albedoMap   = resTex(pbr.baseColorTexture.index);
    md.normalMap   = resTex(mat.normalTexture.index);
    md.emissiveMap = resTex(mat.emissiveTexture.index);

    out.push_back(std::move(md));
}

} // namespace

namespace GLTF {

// ============================================================================
// 文件载入：.gltf→ASCII，.glb→Binary
// ============================================================================

bool LoadModel(const std::string &filepath, tinygltf::Model &outModel, std::string *err) {
    tinygltf::TinyGLTF loader;
    std::string warn, errStr;

    std::string ext = std::filesystem::path(filepath).extension().string();
    for (char &c : ext) {
        if (c >= 'A' && c <= 'Z') {
            c += static_cast<char>('a' - 'A');
        }
    }
    bool ok = false;
    if (ext == ".glb") {
        ok = loader.LoadBinaryFromFile(&outModel, &errStr, &warn, filepath);
    } else {
        ok = loader.LoadASCIIFromFile(&outModel, &errStr, &warn, filepath);
    }
    if (!warn.empty()) {
        GE_CORE_WARN("[Mesh] glTF 警告: {}", warn);
    }
    if (!ok) {
        if (err) {
            *err = errStr.empty() ? "tinygltf 加载失败" : errStr;
        }
        return false;
    }
    return true;
}

// ============================================================================
// primitive → SubMesh 装配（逐段，各占一段连续区间）
// ============================================================================

bool BuildMesh(const tinygltf::Model &m, size_t mi, const std::string &filepath,
               MeshData &out) {
    if (mi >= m.meshes.size()) {
        GE_CORE_ERROR("[Mesh] glTF mesh 索引越界 {}", mi);
        return false;
    }
    const tinygltf::Mesh &mesh = m.meshes[mi];

    for (const auto &prim : mesh.primitives) {
        if (prim.mode != TINYGLTF_MODE_TRIANGLES) {
            GE_CORE_WARN("[Mesh] 跳过非三角 primitive (mode={})", prim.mode);
            continue;
        }
        auto posIt = prim.attributes.find("POSITION");
        if (posIt == prim.attributes.end()) {
            continue; // 无位置 = 无几何
        }

        const uint32_t firstVertex = static_cast<uint32_t>(out.vertices.size());
        const auto &posAcc = m.accessors[posIt->second];
        const size_t vertexCnt = posAcc.count;

        // 本 primitive 局部去重表（段内复用顶点）。glTF primitive 属性流各自独立，
        // 少共享顶点，故不做跨 primitive 全局去重（对比 OBJ 语义）。
        std::unordered_map<Vertex, uint32_t> local;
        std::vector<uint32_t> localIdx;
        localIdx.reserve(vertexCnt);

        for (size_t i = 0; i < vertexCnt; ++i) {
            Vertex v{};
            ReadFloatAttr(m, posIt->second, i, v.Position);
            auto nIt = prim.attributes.find("NORMAL");
            if (nIt != prim.attributes.end()) {
                ReadFloatAttr(m, nIt->second, i, v.Normal);
            }
            auto tIt = prim.attributes.find("TEXCOORD_0");
            if (tIt != prim.attributes.end()) {
                ReadFloatAttr(m, tIt->second, i, v.TexCoord);
                v.TexCoord.y = 1.0f - v.TexCoord.y; // glTF UV 左上原点 → 引擎习惯
            }
            auto gIt = prim.attributes.find("TANGENT");
            if (gIt != prim.attributes.end()) {
                ReadFloatAttr(m, gIt->second, i, v.Tangent);
            }
            // 缺 TANGENT / NORMAL 时不在此补算：Mesh::Create 装配路径经
            // Mesh::BuildMesh 统一 ComputeTangents，单一来源不重复计算。

            // 一次性量化清洗，使去重的精确比较 / 哈希正确判定
            v.Position = Vertex::Quantize(v.Position);
            v.Normal   = Vertex::Quantize(v.Normal);
            v.TexCoord = Vertex::Quantize(v.TexCoord);
            v.Tangent  = Vertex::Quantize(v.Tangent);

            auto it = local.find(v);
            uint32_t idx;
            if (it == local.end()) {
                idx = static_cast<uint32_t>(out.vertices.size() - firstVertex); // 段内相对索引
                local.emplace(v, idx);
                out.vertices.push_back(v);
            } else {
                idx = it->second;
            }
            localIdx.push_back(idx);
        }

        // 索引：有 indices accessor 解包并 +firstVertex；无则按 0..N
        const uint32_t firstIndex = static_cast<uint32_t>(out.indices.size());
        const size_t idxCount = (prim.indices >= 0)
            ? m.accessors[prim.indices].count : vertexCnt;
        for (size_t i = 0; i < idxCount; ++i) {
            uint32_t src = (prim.indices >= 0) ? ReadIndex(m, prim.indices, i)
                                               : static_cast<uint32_t>(i);
            out.indices.push_back(firstVertex + localIdx[src]);
        }

        // primitive → SubMesh（1:1，各占一段连续区间）
        out.subMeshes.push_back({
            firstVertex,
            static_cast<uint32_t>(out.vertices.size() - firstVertex),
            firstIndex,
            static_cast<uint32_t>(idxCount),
            (prim.material >= 0) ? m.materials[prim.material].name : "",
            nullptr,
        });

        AppendGLTFMaterial(m, prim.material, filepath, out.materialData);
    }

    if (out.vertices.empty() || out.indices.empty()) {
        GE_CORE_ERROR("[Mesh] glTF mesh 解析为空 (mesh {})", mi);
        return false;
    }
    return true;
}

// ============================================================================
// LoadModel + BuildMesh 便捷封装（单文件单 mesh，供 MeshManager::LoadGLTFMesh 复用）
// ============================================================================

bool BuildMeshData(const std::string &filepath, size_t meshIndex, MeshData &out) {
    tinygltf::Model model;
    std::string err;
    if (!LoadModel(filepath, model, &err)) {
        GE_CORE_ERROR("[Mesh] glTF 加载失败 '{}': {}", filepath, err);
        return false;
    }
    return BuildMesh(model, meshIndex, filepath, out);
}

} // namespace GLTF

// ============================================================================
// ModelLoader 对外接口
// ============================================================================

bool ModelLoader::ParseGLTF(const std::string &filepath, size_t meshIndex, MeshData &out) {
    tinygltf::Model model;
    std::string err;
    if (!GLTF::LoadModel(filepath, model, &err)) {
        GE_CORE_ERROR("[Mesh] glTF 加载失败 '{}': {}", filepath, err);
        return false;
    }
    return GLTF::BuildMesh(model, meshIndex, filepath, out);
}

size_t ModelLoader::GetGLTFMeshCount(const std::string &filepath) {
    tinygltf::Model model;
    if (!GLTF::LoadModel(filepath, model)) {
        return 0;
    }
    return model.meshes.size();
}

} // namespace GE
