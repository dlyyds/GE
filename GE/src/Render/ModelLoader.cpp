/**
 * @file ModelLoader.cpp
 * @brief 模型加载统一入口实现：格式分派与共享 CPU 装配。
 */

#include "Render/ModelLoader.h"

#include "Core/Log.h"

#include <filesystem>
#include <vector>
#include <glm/glm.hpp>

namespace GE {

// ============================================================================
// 共享装配：计算顶点切线（法线贴图需要 TBN 切线空间）
// ============================================================================

void ComputeTangents(MeshData &data) {
    auto &vertices = data.vertices;
    const auto &indices = data.indices;

    std::vector<glm::vec3> tan1(vertices.size(), glm::vec3(0.0f));
    std::vector<glm::vec3> tan2(vertices.size(), glm::vec3(0.0f));

    for (size_t i = 0; i + 2 < indices.size(); i += 3) {
        const auto &v0 = vertices[indices[i + 0]];
        const auto &v1 = vertices[indices[i + 1]];
        const auto &v2 = vertices[indices[i + 2]];

        glm::vec3 edge1 = v1.Position - v0.Position;
        glm::vec3 edge2 = v2.Position - v0.Position;
        glm::vec2 duv1 = v1.TexCoord - v0.TexCoord;
        glm::vec2 duv2 = v2.TexCoord - v0.TexCoord;

        float r = 1.0f / (duv1.x * duv2.y - duv2.x * duv1.y);
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

bool ParseModelData(const std::string &filepath, MeshData &out) {
    // 文件扩展名统一转小写，避免 ".OBJ" 之类的混合大小写漏匹配
    std::string ext = std::filesystem::path(filepath).extension().string();
    for (char &c : ext) {
        if (c >= 'A' && c <= 'Z') {
            c += static_cast<char>('a' - 'A');
        }
    }

    if (ext == ".obj") {
        return ParseOBJData(filepath, out);
    }

    GE_CORE_ERROR("[Mesh] 不支持的模型格式 '{}': {}", ext, filepath);
    return false;
}

} // namespace GE