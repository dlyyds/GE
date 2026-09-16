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
#include "Scene/Components.h"
#include "FileSystem/VFS.h"
#include "Core/Log.h"

#include "tinygltf/tiny_gltf.h"

#include <algorithm>
#include <filesystem>
#include <vector>
#include <cstring>
#include <limits>
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
    // 校验元素区整体落在 buffer 内（base + count*stride），而非只查起点。
    // bufferView 的 byteLength 可能小于 count*stride（sparse / 被钳制的 accessor），
    // 仅查起点会导致读越界、把垃圾数据喂给后续索引/顶点装配，最终越界崩溃。
    // ByteStride 在 byteStride==0 时按紧排返回元素字节大小，等效元素步长恒有值。
    const int byteStride = acc.ByteStride(bv);
    if (byteStride <= 0) {
        return nullptr;
    }
    if (off >= buf.data.size() ||
        acc.count > (buf.data.size() - off) / static_cast<size_t>(byteStride)) {
        return nullptr;
    }
    return buf.data.data() + off;
}

/**
 * @brief 读取 accessor 第 i 个元素的前 n 个 float 分量到 out。
 *
 * 支持 FLOAT 直读，以及 UNSIGNED_BYTE / UNSIGNED_SHORT 的 normalized（量化）解码
 * （glTF 常把 NORMAL / TEXCOORD / WEIGHTS 存为 8/16 位定点，按
 * value / (2^bits - 1) 归一化）。int16 带符号量化在项目内不产出，仅当出现
 * 非法分量类型时清空输出兜底。
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
    const uint8_t *p = base + i * static_cast<size_t>(stride);
    switch (acc.componentType) {
        case TINYGLTF_COMPONENT_TYPE_FLOAT: {
            const float *src = reinterpret_cast<const float *>(p);
            for (int c = 0; c < kN; ++c) {
                dst[c] = src[c];
            }
            break;
        }
        // 整数分量：仅当 accessor.normalized 时按 glTF 语义归一化到 [0,1]，
        // 否则按原始整数值读取（罕见，但必须尊重标志位，否则会把原始值当 0..1）。
        case TINYGLTF_COMPONENT_TYPE_UNSIGNED_BYTE: {
            for (int c = 0; c < kN; ++c) {
                dst[c] = acc.normalized ? p[c] / 255.0f : static_cast<float>(p[c]);
            }
            break;
        }
        case TINYGLTF_COMPONENT_TYPE_UNSIGNED_SHORT: {
            uint16_t s;
            for (int c = 0; c < kN; ++c) {
                std::memcpy(&s, p + c * sizeof(uint16_t), sizeof(uint16_t));
                dst[c] = acc.normalized ? s / 65535.0f : static_cast<float>(s);
            }
            break;
        }
        // int16 带符号量化、非法类型：清空输出兜底
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

/**
 * @brief 读取蒙皮关节索引 accessor 第 i 个元素的前 4 个分量到 out（uvec4）。
 *
 * glTF JOINTS_0 为 VEC4，componentType 取 UNSIGNED_BYTE / UNSIGNED_SHORT。
 * 分量宽度按 componentType 决定（分量本身在 stride 内连续，padding 在尾部），
 * 越界 / 非法 / 缺失时整组清零（与 ReadIndex 越界兜底一致）。
 */
static void ReadJoints(const tinygltf::Model &m, int accessorIdx, size_t i, glm::uvec4 &out) {
    out = glm::uvec4(0u);
    if (accessorIdx < 0 || accessorIdx >= static_cast<int>(m.accessors.size())) {
        return;
    }
    const auto &acc = m.accessors[accessorIdx];
    const uint8_t *base = AccessorBasePtr(m, acc);
    int stride = acc.bufferView >= 0
        ? acc.ByteStride(m.bufferViews[acc.bufferView]) : -1;
    if (!base || stride < 0 || acc.type != TINYGLTF_TYPE_VEC4) {
        return;
    }
    const uint8_t *p = base + i * static_cast<size_t>(stride);
    switch (acc.componentType) {
        case TINYGLTF_COMPONENT_TYPE_UNSIGNED_BYTE:
            out = {static_cast<uint32_t>(p[0]), static_cast<uint32_t>(p[1]),
                   static_cast<uint32_t>(p[2]), static_cast<uint32_t>(p[3])};
            break;
        case TINYGLTF_COMPONENT_TYPE_UNSIGNED_SHORT: {
            uint16_t v[4];
            std::memcpy(v, p, sizeof(v));
            out = {v[0], v[1], v[2], v[3]};
            break;
        }
        default:
            break; // 其他分量类型非法，保持清零
    }
}

// ============================================================================
// 动画采样 accessor 读取（阶段 A：动画键帧解码的数据路径）
// ============================================================================

/**
 * @brief 读取动画时间轴 accessor（SCALAR/FLOAT）整列到 out。
 *
 * 时间轴是浮点标量数组（秒）。componentType 非 FLOAT 或类型非 SCALAR 时返回
 * false，调用方容错跳过该 channel。
 */
static bool ReadAnimTimes(const tinygltf::Model &m, int accessorIdx,
                          std::vector<float> &out) {
    if (accessorIdx < 0 || accessorIdx >= static_cast<int>(m.accessors.size())) {
        return false;
    }
    const auto &acc = m.accessors[static_cast<size_t>(accessorIdx)];
    if (acc.type != TINYGLTF_TYPE_SCALAR || acc.componentType != TINYGLTF_COMPONENT_TYPE_FLOAT) {
        return false;
    }
    const uint8_t *base = AccessorBasePtr(m, acc);
    int stride = acc.bufferView >= 0 ? acc.ByteStride(m.bufferViews[acc.bufferView]) : -1;
    if (!base || stride < 0) {
        return false;
    }
    out.clear();
    out.reserve(acc.count);
    for (size_t i = 0; i < acc.count; ++i) {
        const uint8_t *p = base + i * static_cast<size_t>(stride);
        out.push_back(*reinterpret_cast<const float *>(p));
    }
    return true;
}

/**
 * @brief 读取动画输出 accessor 每个键帧的「值」到 out（跳过 CUBICSPLINE 的切线）。
 *
 * @param valueElemPerKey LINEAR/STEP 为 1；CUBICSPLINE 为 3（入切线/值/出切线），
 *                        此时每键帧取中间的「值」元素，切线留待 Hermite 采样阶段。
 * @param compCount       每元素 float 分量数（translation/scale=3，rotation=4）
 * @param keyCount        键帧数（时间轴长度），out 长度 = keyCount × compCount
 */
static bool ReadAnimValues(const tinygltf::Model &m, int accessorIdx,
                           size_t valueElemPerKey, int compCount, size_t keyCount,
                           std::vector<float> &out) {
    if (accessorIdx < 0 || accessorIdx >= static_cast<int>(m.accessors.size())) {
        return false;
    }
    const auto &acc = m.accessors[static_cast<size_t>(accessorIdx)];
    if (acc.componentType != TINYGLTF_COMPONENT_TYPE_FLOAT) {
        return false;
    }
    // 按 accessor 类型校验分量数（VEC3=3 / VEC4=4），与调用方期望一致才读
    const int typeComp = (acc.type == TINYGLTF_TYPE_VEC4 ? 4
                        : acc.type == TINYGLTF_TYPE_VEC3 ? 3
                        : 0);
    if (typeComp != compCount) {
        return false;
    }
    const uint8_t *base = AccessorBasePtr(m, acc);
    int stride = acc.bufferView >= 0 ? acc.ByteStride(m.bufferViews[acc.bufferView]) : -1;
    if (!base || stride < 0) {
        return false;
    }
    // 元素数须覆盖 valueElemPerKey × keyCount（CUBICSPLINE 输出 = 3 × 键帧数）
    if (acc.count < valueElemPerKey * keyCount) {
        return false;
    }
    out.clear();
    out.reserve(keyCount * static_cast<size_t>(compCount));
    // CUBICSPLINE 每键帧 3 个元素（入切线/值/出切线），值在第 2 个（跳过切线）
    const size_t valueElemInKey = (valueElemPerKey == 3) ? 1 : 0;
    for (size_t k = 0; k < keyCount; ++k) {
        const size_t elem = k * valueElemPerKey + valueElemInKey;
        const uint8_t *p = base + elem * static_cast<size_t>(stride);
        const float *src = reinterpret_cast<const float *>(p);
        for (int c = 0; c < compCount; ++c) {
            out.push_back(src[c]);
        }
    }
    return true;
}

// ============================================================================
// 材质收集：metallic-roughness → MaterialData
// ============================================================================

/**
 * @brief 把 glTF 某个 material 捕获为 MaterialData 追加到 out（同名去重）。
 *
 * 贴图 URI 相对 glTF 文件所在目录解析（结果是资产的**规范形**路径，正斜杠）；
 * 内嵌 bufferView 图像（GLB BIN）
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
        // generic_string()：反斜杠只会在 Windows 上出现，而这里是**资产的规范形**
        // 路径，必须一律正斜杠（Android 的 AAssetManager 大小写/分隔符都不通融）
        if (p.is_absolute()) {
            return p.lexically_normal().generic_string();
        }
        return (std::filesystem::path(filepath).parent_path() / uri)
            .lexically_normal().generic_string();
    };
    // 贴图索引：优先 Metallic-Roughness 的 baseColorTexture；无效时回退
    // KHR_materials_pbrSpecularGlossiness 扩展的 diffuseTexture（Blender/旧导出器
    // 产物，albedo 存在扩展而非 baseColorTexture，如 adamHead 系列模型）。
    int albedoTexIdx = pbr.baseColorTexture.index;
    if (albedoTexIdx < 0) {
        const auto sgIt = mat.extensions.find("KHR_materials_pbrSpecularGlossiness");
        if (sgIt != mat.extensions.end()) {
            const auto &diffuse = sgIt->second.Get("diffuseTexture");
            if (diffuse.IsObject()) {
                albedoTexIdx = diffuse.Get("index").GetNumberAsInt();
            }
        }
    }
    md.albedoMap   = resTex(albedoTexIdx);
    md.normalMap   = resTex(mat.normalTexture.index);
    md.emissiveMap = resTex(mat.emissiveTexture.index);
    md.metallicRoughnessMap = resTex(pbr.metallicRoughnessTexture.index);

    // ── 透明度语义（alphaMode / alphaCutoff / doubleSided / 基础 alpha）──
    // glTF 规范：alphaMode ∈ {OPAQUE, MASK, BLEND}，默认 OPAQUE；
    // baseColorFactor[3] 是材质级基础 alpha（默认 1），落到 dissolve
    // （OBJ `d` / 不透明度同语义，ApplyMaterialData 据此生成颜色）。
    md.alphaMode = (mat.alphaMode == "MASK") ? Material::AlphaMode::Mask
                 : (mat.alphaMode == "BLEND") ? Material::AlphaMode::Blend
                                              : Material::AlphaMode::Opaque;
    md.alphaCutoff = static_cast<float>(mat.alphaCutoff);
    md.doubleSided = mat.doubleSided;
    md.dissolve = static_cast<float>(pbr.baseColorFactor[3]);

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

    // 整个文件先经 VFS 读进内存，再把内存交给 tinygltf —— 引擎不再有"直接按路径
    // 打开资产文件"的地方（Android 上资产在 APK 内，tinygltf 的 file API 读不到）。
    // base_dir 传空：本工程开着 TINYGLTF_NO_EXTERNAL_IMAGE，贴图由纹理子系统单独
    // 加载，tinygltf 不会去解析外部 .png / .bin。
    std::vector<uint8_t> bytes;
    if (!VFS::ReadAll(filepath, bytes)) {
        if (err) {
            *err = "读不到文件: " + filepath;
        }
        GE_CORE_ERROR("[Mesh] glTF 读不到文件: {}", filepath);
        return false;
    }

    bool ok = false;
    if (ext == ".glb") {
        ok = loader.LoadBinaryFromMemory(&outModel, &errStr, &warn, bytes.data(),
                                         static_cast<unsigned int>(bytes.size()));
    } else {
        ok = loader.LoadASCIIFromString(&outModel, &errStr, &warn,
                                        reinterpret_cast<const char *>(bytes.data()),
                                        static_cast<unsigned int>(bytes.size()), "");
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
    bool hasSkinning = false; // 本 mesh 是否含蒙皮属性流（JOINTS_0+WEIGHTS_0）
    float weightSumMin = std::numeric_limits<float>::max();
    float weightSumMax = -std::numeric_limits<float>::max();

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

        // 蒙皮属性 JOINTS_0 / WEIGHTS_0 须成对存在才按蒙皮顶点读取；
        // 缺一即按静态顶点（JointIdx 保持 0、Weight 保持 0）。
        bool primSkinned = prim.attributes.count("JOINTS_0") > 0
                        && prim.attributes.count("WEIGHTS_0") > 0;
        if (primSkinned) {
            hasSkinning = true;
        }

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
                // glTF UV 原点在左上（V=0 顶部），引擎 stb + Vulkan 上传保持
                // 顶部原点（无 flip_vertically + 像素首行=图片顶部），故不翻转；
                // OBJ 的 v 在底部才需要 1-y（见 OBJLoader）。
            }
            auto gIt = prim.attributes.find("TANGENT");
            if (gIt != prim.attributes.end()) {
                ReadFloatAttr(m, gIt->second, i, v.Tangent);
            }
            // 缺 TANGENT / NORMAL 时不在此补算：Mesh::Create 装配路径经
            // Mesh::BuildMesh 统一 ComputeTangents，单一来源不重复计算。
            // 蒙皮属性 JOINTS_0 / WEIGHTS_0（须成对存在才读取；缺一即按静态顶点，
            // JointIdx 保持 0、Weight 保持 0，蒙皮越界索引在 ReadJoints 已清零兜底）。
            if (primSkinned) {
                ReadJoints(m, prim.attributes.find("JOINTS_0")->second, i, v.JointIdx);
                ReadFloatAttr(m, prim.attributes.find("WEIGHTS_0")->second, i, v.Weight);
                // A 阶段验证：追踪蒙皮权重和（应为 [0,1] 分量且和≈1）
                weightSumMin = std::min(weightSumMin,
                                        v.Weight.x + v.Weight.y + v.Weight.z + v.Weight.w);
                weightSumMax = std::max(weightSumMax,
                                        v.Weight.x + v.Weight.y + v.Weight.z + v.Weight.w);
            }

            // 一次性量化清洗，使去重的精确比较 / 哈希正确判定
            v.Position = Vertex::Quantize(v.Position);
            v.Normal   = Vertex::Quantize(v.Normal);
            v.TexCoord = Vertex::Quantize(v.TexCoord);
            v.Tangent  = Vertex::Quantize(v.Tangent);
            v.Weight   = Vertex::Quantize(v.Weight);

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
            // 钳制越界索引（sparse / 损坏的 accessor 会给出 >= vertexCnt 的垃圾值），
            // 防止 localIdx[src] 越界——越界值会经索引数组传导到 ComputeTangents 崩溃。
            if (src >= localIdx.size()) {
                src = 0;
            }
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

    // 本 mesh 含蒙皮属性流 → 标记为被皮肤驱动的网格（具体关联的皮肤索引，
    // 及 IBM 等数据，由 GLTFSceneImporter 在 node 层解析并挂 SkinComponent）。
    if (hasSkinning) {
        out.skinIndex = 0;
        // A 阶段验证日志：权重和的合法范围（分量 ∈[0,1]，正常应和≈1）
        if (weightSumMin < 0.5f || weightSumMax > 1.5f) {
            GE_CORE_WARN("[Mesh] glTF mesh {} 蒙皮权重异常: 和范围 [{}, {}] "
                         "(mesh {}, 权重应在 [0,1] 且和≈1)",
                         mi, weightSumMin, weightSumMax, mesh.name);
        } else {
            GE_CORE_INFO("[Mesh] glTF mesh {} 蒙皮加载: {} 顶点, JOINTS_0 权重和范围 [{}, {}]",
                         mi, out.vertices.size(), weightSumMin, weightSumMax);
        }
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

// ============================================================================
// 动画解析：animation → AnimationClip（阶段 A：只读键帧，不触及场景/渲染）
// ============================================================================

bool BuildAnimations(const tinygltf::Model &m, size_t animIdx,
                     AnimationClip &out, std::string *err) {
    if (animIdx >= m.animations.size()) {
        if (err) {
            *err = "animation 索引越界";
        }
        GE_CORE_ERROR("[Anim] animation 索引越界 {}", animIdx);
        return false;
    }

    const tinygltf::Animation &anim = m.animations[animIdx];
    out.name = anim.name;
    out.duration = 0.0f;
    out.channels.clear();

    for (const auto &ch : anim.channels) {
        // 目标路径：translation / rotation / scale；weights（morph）本计划不支撑
        AnimationChannel chan;
        if (ch.target_path == "translation") {
            chan.path = AnimationChannel::Path::Translation;
        } else if (ch.target_path == "rotation") {
            chan.path = AnimationChannel::Path::Rotation;
        } else if (ch.target_path == "scale") {
            chan.path = AnimationChannel::Path::Scale;
        } else {
            GE_CORE_WARN("[Anim] 跳过不支持的 channel 路径 '{}'", ch.target_path);
            continue;
        }
        chan.nodeIndex = ch.target_node;

        // sampler 与插值类型
        if (ch.sampler < 0 || ch.sampler >= static_cast<int>(anim.samplers.size())) {
            GE_CORE_WARN("[Anim] channel(path={}) 的 sampler 索引越界 {}", ch.target_path, ch.sampler);
            continue;
        }
        const tinygltf::AnimationSampler &sam = anim.samplers[static_cast<size_t>(ch.sampler)];
        if (sam.interpolation == "STEP") {
            chan.interp = AnimationChannel::Interp::Step;
        } else if (sam.interpolation == "CUBICSPLINE") {
            chan.interp = AnimationChannel::Interp::CubicSpline;
        } else if (sam.interpolation != "LINEAR") {
            // 未知插值容错按 LINEAR（既不会崩，也不会误当 STEP/CUBIC 走样）
            chan.interp = AnimationChannel::Interp::Linear;
            GE_CORE_WARN("[Anim] 未知插值 '{}' 按 LINEAR 处理", sam.interpolation);
        }

        // 时间轴（SCALAR/FLOAT）
        if (!ReadAnimTimes(m, sam.input, chan.times) || chan.times.empty()) {
            GE_CORE_WARN("[Anim] 时间轴读取失败, path={}", ch.target_path);
            continue;
        }
        const size_t keyCount = chan.times.size();
        const size_t valueElemPerKey =
            (chan.interp == AnimationChannel::Interp::CubicSpline) ? 3 : 1;

        // 采样值 → 按路径分存（rotation 换序进 quatKeys，其余进 vecKeys）
        std::vector<float> vals;
        if (chan.path == AnimationChannel::Path::Rotation) {
            if (!ReadAnimValues(m, sam.output, valueElemPerKey, 4, keyCount, vals)) {
                GE_CORE_WARN("[Anim] rotation 采样值读取失败, node={}", chan.nodeIndex);
                continue;
            }
            chan.quatKeys.reserve(keyCount);
            for (size_t k = 0; k < keyCount; ++k) {
                const float *q = vals.data() + k * 4;
                // glTF 四元数 [x,y,z,w] → glm::quat(w,x,y,z)
                chan.quatKeys.emplace_back(q[3], q[0], q[1], q[2]);
            }
        } else {
            if (!ReadAnimValues(m, sam.output, valueElemPerKey, 3, keyCount, vals)) {
                GE_CORE_WARN("[Anim] translation/scale 采样值读取失败, node={}", chan.nodeIndex);
                continue;
            }
            chan.vecKeys.reserve(keyCount);
            for (size_t k = 0; k < keyCount; ++k) {
                const float *v = vals.data() + k * 3;
                chan.vecKeys.emplace_back(v[0], v[1], v[2]);
            }
        }

        // clip 时长为全部 channel 时间轴末尾的最大值
        out.duration = std::max(out.duration, chan.times.back());
        out.channels.push_back(std::move(chan));
    }

    return true;
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
