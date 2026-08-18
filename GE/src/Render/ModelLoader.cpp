/**
 * @file ModelLoader.cpp
 * @brief 模型加载统一入口实现：格式分派、内置几何生成与共享 CPU 装配。
 */

#include "Render/ModelLoader.h"

#include "Core/Log.h"

#include <filesystem>
#include <vector>
#include <glm/glm.hpp>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

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

// ============================================================================
// 内置几何体：生成 CPU 顶点/索引数据（不带子网格，由 Mesh::Create 统一补全）
// ============================================================================

bool GenerateBuiltinMeshData(const std::string &type, MeshData &out) {
    auto &vertices = out.vertices;
    auto &indices = out.indices;

    if (type == "cube") {
        // 立方体：边长 2，中心在原点，6 个面各 4 顶点 = 24 顶点，36 索引
        vertices.reserve(24);
        indices.reserve(36);

        auto add_quad = [&](const glm::vec3 &p0, const glm::vec3 &p1,
                            const glm::vec3 &p2, const glm::vec3 &p3,
                            const glm::vec3 &normal) {
            uint32_t base = static_cast<uint32_t>(vertices.size());
            glm::vec2 uvs[] = {{0.0f, 0.0f}, {1.0f, 0.0f}, {1.0f, 1.0f}, {0.0f, 1.0f}};
            glm::vec3 pos[] = {p0, p1, p2, p3};
            for (int i = 0; i < 4; ++i) {
                vertices.push_back({pos[i], normal, uvs[i]});
            }
            indices.push_back(base + 0);
            indices.push_back(base + 1);
            indices.push_back(base + 2);
            indices.push_back(base + 0);
            indices.push_back(base + 2);
            indices.push_back(base + 3);
        };

        // +Z 面（前）
        add_quad({-1.0f, -1.0f, 1.0f}, {1.0f, -1.0f, 1.0f},
                 {1.0f, 1.0f, 1.0f}, {-1.0f, 1.0f, 1.0f},
                 {0.0f, 0.0f, 1.0f});
        // -Z 面（后）
        add_quad({1.0f, -1.0f, -1.0f}, {-1.0f, -1.0f, -1.0f},
                 {-1.0f, 1.0f, -1.0f}, {1.0f, 1.0f, -1.0f},
                 {0.0f, 0.0f, -1.0f});
        // +X 面（右）
        add_quad({1.0f, -1.0f, 1.0f}, {1.0f, -1.0f, -1.0f},
                 {1.0f, 1.0f, -1.0f}, {1.0f, 1.0f, 1.0f},
                 {1.0f, 0.0f, 0.0f});
        // -X 面（左）
        add_quad({-1.0f, -1.0f, -1.0f}, {-1.0f, -1.0f, 1.0f},
                 {-1.0f, 1.0f, 1.0f}, {-1.0f, 1.0f, -1.0f},
                 {-1.0f, 0.0f, 0.0f});
        // +Y 面（上）
        add_quad({-1.0f, 1.0f, 1.0f}, {1.0f, 1.0f, 1.0f},
                 {1.0f, 1.0f, -1.0f}, {-1.0f, 1.0f, -1.0f},
                 {0.0f, 1.0f, 0.0f});
        // -Y 面（下）
        add_quad({-1.0f, -1.0f, -1.0f}, {1.0f, -1.0f, -1.0f},
                 {1.0f, -1.0f, 1.0f}, {-1.0f, -1.0f, 1.0f},
                 {0.0f, -1.0f, 0.0f});
    } else if (type == "plane") {
        // 平面：XY 平面，边长 2，中心在原点，法线 +Z
        vertices = {
            {{-1.0f, -1.0f, 0.0f}, {0.0f, 0.0f, 1.0f}, {0.0f, 0.0f}},
            {{1.0f, -1.0f, 0.0f}, {0.0f, 0.0f, 1.0f}, {1.0f, 0.0f}},
            {{1.0f, 1.0f, 0.0f}, {0.0f, 0.0f, 1.0f}, {1.0f, 1.0f}},
            {{-1.0f, 1.0f, 0.0f}, {0.0f, 0.0f, 1.0f}, {0.0f, 1.0f}},
        };
        indices = {0, 1, 2, 0, 2, 3};
    } else if (type == "quad") {
        // 四边形（plane 的别名）
        vertices = {
            {{-1.0f, -1.0f, 0.0f}, {0.0f, 0.0f, 1.0f}, {0.0f, 0.0f}},
            {{1.0f, -1.0f, 0.0f}, {0.0f, 0.0f, 1.0f}, {1.0f, 0.0f}},
            {{1.0f, 1.0f, 0.0f}, {0.0f, 0.0f, 1.0f}, {1.0f, 1.0f}},
            {{-1.0f, 1.0f, 0.0f}, {0.0f, 0.0f, 1.0f}, {0.0f, 1.0f}},
        };
        indices = {0, 1, 2, 0, 2, 3};
    } else if (type == "sphere") {
        // 球体：半径 1，中心在原点，UV 球体
        const int latBands = 20;
        const int lonBands = 20;
        const float radius = 1.0f;

        for (int lat = 0; lat <= latBands; ++lat) {
            float theta = static_cast<float>(lat) * static_cast<float>(M_PI) / static_cast<float>(latBands);
            float sinTheta = std::sin(theta);
            float cosTheta = std::cos(theta);

            for (int lon = 0; lon <= lonBands; ++lon) {
                float phi = static_cast<float>(lon) * 2.0f * static_cast<float>(M_PI) / static_cast<float>(lonBands);
                float sinPhi = std::sin(phi);
                float cosPhi = std::cos(phi);

                glm::vec3 pos{
                    radius * cosPhi * sinTheta,
                    radius * cosTheta,
                    radius * sinPhi * sinTheta
                };
                glm::vec3 normal = glm::normalize(pos);
                glm::vec2 uv{
                    static_cast<float>(lon) / static_cast<float>(lonBands),
                    static_cast<float>(lat) / static_cast<float>(latBands)
                };
                vertices.push_back({pos, normal, uv});
            }
        }

        for (int lat = 0; lat < latBands; ++lat) {
            for (int lon = 0; lon < lonBands; ++lon) {
                // 注意：绕序必须与法线一致（CCW 朝外）。原实现 (first,second,first+1)
                // 的叉积法线朝内，导致外侧被当作背面剔除、法线背离相机，
                // 所有直接光照失效（只剩环境光）。这里交换 last two 顶点翻转绕序。
                uint32_t first = static_cast<uint32_t>(lat * (lonBands + 1) + lon);
                uint32_t second = first + static_cast<uint32_t>(lonBands + 1);
                indices.push_back(first);
                indices.push_back(first + 1);
                indices.push_back(second);
                indices.push_back(second);
                indices.push_back(first + 1);
                indices.push_back(second + 1);
            }
        }
    } else {
        return false; // 未知类型
    }

    return true;
}

} // namespace GE