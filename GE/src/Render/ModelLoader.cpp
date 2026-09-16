/**
 * @file ModelLoader.cpp
 * @brief 模型加载统一入口实现：格式分派与共享 CPU 装配。
 */

#include "Render/ModelLoader.h"
#include "Render/GEMeshLoader.h"
#include "Render/AssetPathUtil.h"
#include "FileSystem/VFS.h"

#include "Core/Log.h"

#include <filesystem>
#include <vector>
#include <glm/glm.hpp>

namespace GE {

// ============================================================================
// 内嵌资产引用的归一（根无关兜底）
// ============================================================================

void ModelLoader::CanonicalizeAssetRef(std::string &ref, const std::filesystem::path &assetRoot) {
    if (ref.empty() || AssetPathUtil::IsPseudoKey(ref)) {
        return;
    }

    // 精确判定 + 根无关兜底（后者用 VFS 实际能否读到定夺候选，是地面真值）。
    // 与 AssetManager::ResolveCanonical 共用同一实现，避免这套微妙的启发式两处各写一份。
    const std::string original = ref;
    if (auto rel = AssetPathUtil::ToCanonicalLenient(ref, assetRoot, VFS::Exists)) {
        if (*rel != original) {
            GE_CORE_INFO("[Model] 内嵌资产引用归一: {0} → {1}", original, *rel);
        }
        ref = *rel;
        return;
    }

    // 切不出资源根段：保持原串，由读取方如实报"读不到"，不猜一个替代路径
    GE_CORE_WARN("[Model] 内嵌资产引用无法归一到资源根，保留原串: {0}", ref);
}

void ModelLoader::CanonicalizeEmbeddedMaterialRefs(MeshData &data,
                                                   const std::filesystem::path &assetRoot) {
    for (auto &md : data.materialData) {
        CanonicalizeAssetRef(md.albedoMap, assetRoot);
        CanonicalizeAssetRef(md.normalMap, assetRoot);
        CanonicalizeAssetRef(md.emissiveMap, assetRoot);
        CanonicalizeAssetRef(md.metallicMap, assetRoot);
        CanonicalizeAssetRef(md.roughnessMap, assetRoot);
        CanonicalizeAssetRef(md.metallicRoughnessMap, assetRoot);
    }
}

// ============================================================================
// 共享装配：计算顶点切线（法线贴图需要 TBN 切线空间）
// ============================================================================

void ModelLoader::ComputeTangents(MeshData &data) {
    auto &vertices = data.vertices;
    const auto &indices = data.indices;

    std::vector<glm::vec3> tan1(vertices.size(), glm::vec3(0.0f));
    std::vector<glm::vec3> tan2(vertices.size(), glm::vec3(0.0f));

    for (size_t i = 0; i + 2 < indices.size(); i += 3) {
        // 防御：索引越界（损坏的源模型 / 解析器 bug）时跳过该三角形，避免越界崩溃
        if (indices[i + 0] >= vertices.size() ||
            indices[i + 1] >= vertices.size() ||
            indices[i + 2] >= vertices.size()) {
            continue;
        }
        const auto &v0 = vertices[indices[i + 0]];
        const auto &v1 = vertices[indices[i + 1]];
        const auto &v2 = vertices[indices[i + 2]];

        glm::vec3 edge1 = v1.Position - v0.Position;
        glm::vec3 edge2 = v2.Position - v0.Position;
        glm::vec2 duv1 = v1.TexCoord - v0.TexCoord;
        glm::vec2 duv2 = v2.TexCoord - v0.TexCoord;

        // 退化三角形（UV 共线 / 面积为零）会导致除零 → NaN，切线失稳但不应崩溃
        float denom = duv1.x * duv2.y - duv2.x * duv1.y;
        if (denom == 0.0f) {
            continue;
        }
        float r = 1.0f / denom;
        glm::vec3 tangent = (edge1 * duv2.y - edge2 * duv1.y) * r;
        glm::vec3 bitangent = (edge2 * duv1.x - edge1 * duv2.x) * r;

        tan1[indices[i + 0]] += tangent;
        tan1[indices[i + 1]] += tangent;
        tan1[indices[i + 2]] += tangent;
        tan2[indices[i + 0]] += bitangent;
        tan2[indices[i + 1]] += bitangent;
        tan2[indices[i + 2]] += bitangent;
    }

    for (size_t i = 0; i < vertices.size(); ++i) {
        auto &v = vertices[i];
        glm::vec3 n = v.Normal;
        glm::vec3 t = tan1[i];

        // Gram-Schmidt 正交化：使切线垂直于法线
        t = glm::normalize(t - n * glm::dot(n, t));
        v.Tangent = glm::vec4(t, 1.0f);

        // 手性符号：根据切线/副切线/法线的相对方向判断左右手系
        float handedness = glm::dot(glm::cross(n, t), tan2[i]);
        v.Tangent.w = (handedness < 0.0f) ? -1.0f : 1.0f;
    }
}

// ============================================================================
// 解析总入口：按扩展名分派到各格式解析器（同步 / 异步共用）
// ============================================================================

bool ModelLoader::Parse(const std::string &filepath, MeshData &out) {
    // 文件扩展名统一转小写，避免 ".OBJ" 之类的混合大小写漏匹配
    std::string ext = std::filesystem::path(filepath).extension().string();
    for (char &c : ext) {
        if (c >= 'A' && c <= 'Z') {
            c += static_cast<char>('a' - 'A');
        }
    }

    if (ext == ".obj") {
        return ParseOBJ(filepath, out);
    }
    if (ext == ".gemesh") {
        return ParseGEMesh(filepath, out);
    }
    if (ext == ".gltf" || ext == ".glb") {
        // 默认解析第 0 个 mesh（单一网格资源语义）；多 mesh 由上层按 meshIndex 逐个取
        return ParseGLTF(filepath, 0, out);
    }

    GE_CORE_ERROR("[Mesh] 不支持的模型格式 '{}': {}", ext, filepath);
    return false;
}

// ============================================================================
// 导出工具：源模型 → .gemesh（解析 + 切线 + 包围盒 + 序列化）
// ============================================================================

bool ModelLoader::ConvertToGEMesh(const std::string &srcPath, const std::string &outPath,
                                  std::string *err, const std::filesystem::path &assetRoot,
                                  size_t meshIndex) {
    // glTF 走带 mesh 索引的解析（一个 glTF 可导出多个 .gemesh，场景里写作 foo.gltf#N）；
    // 其余格式按扩展名分派，索引无意义。
    std::string ext = std::filesystem::path(srcPath).extension().string();
    for (char &c : ext) {
        if (c >= 'A' && c <= 'Z') {
            c += static_cast<char>('a' - 'A');
        }
    }
    const bool isGltf = (ext == ".gltf" || ext == ".glb");

    MeshData data;
    if (!(isGltf ? ParseGLTF(srcPath, meshIndex, data) : Parse(srcPath, data))) {
        if (err) {
            *err = "源模型解析失败: " + srcPath;
        }
        return false;
    }

    // 切线计算（供法线贴图 TBN 使用），保证与运行时加载结果逐字节一致
    ComputeTangents(data);

    // 计算包围盒（导出器填 META chunk）
    GEMeshMeta meta;
    if (!data.vertices.empty()) {
        meta.aabbMin = meta.aabbMax = data.vertices[0].Position;
        for (const auto &v : data.vertices) {
            meta.aabbMin = glm::min(meta.aabbMin, v.Position);
            meta.aabbMax = glm::max(meta.aabbMax, v.Position);
        }
    }
    meta.sourceAsset = srcPath;

    if (assetRoot.empty()) {
        GE_CORE_WARN("[gemesh] 未提供资源根，跳过内嵌路径归一：产物可能含开发机绝对路径");
    } else {
        CanonicalizeGEMeshEmbeddedRefs(data, meta, assetRoot);
    }

    return SerializeGEMesh(outPath, data, meta, err);
}

} // namespace GE