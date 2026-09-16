/**
 * @file OBJLoader.cpp
 * @brief OBJ 格式解析器（tinyobjloader）—— 独立于 Mesh 的格式加载文件。
 *
 * 与未来 glTF 的 GLTFRawLoader 同构：格式解析器一律独立成文件，统一输出 MeshData，
 * 由 ModelLoader::Parse 按扩展名分派。Mesh 不感知 OBJ/tinyobj 的任何细节。
 */

#include "Render/ModelLoader.h"

#include "FileSystem/VFS.h"
#include "Core/Log.h"

#include "tiny_obj_loader.h"

#include <sstream>
#include <unordered_map>
#include <filesystem>

namespace GE {

namespace {

/**
 * @brief tinyobj 的 MTL 读取器，改走 VFS。
 *
 * tinyobj 自带的 `MaterialFileReader` 内部用 `std::ifstream` 直接打开文件 —— 在
 * Android 上读不到 APK 内的资产，MTL 里的贴图会全部变成"无贴图"（模型发白）而
 * 不报错，属静默失败。这里改成 VFS 读整份文本再灌进 `istringstream`，
 * 几何解析与材质解析仍全交给 tinyobj。
 */
class VfsMaterialReader : public tinyobj::MaterialReader {
public:
    explicit VfsMaterialReader(std::string mtlBaseDir) : m_BaseDir(std::move(mtlBaseDir)) {}

    bool operator()(const std::string &matId, std::vector<tinyobj::material_t> *materials,
                    std::map<std::string, int> *matMap, std::string *warn,
                    std::string *err) override {
        // matId 是 mtllib 行里的名字，相对 OBJ 所在目录；先归一（可能含 "../"）
        std::filesystem::path p(matId);
        const std::string path =
            (m_BaseDir.empty() ? p : std::filesystem::path(m_BaseDir) / p)
                .lexically_normal()
                .generic_string();

        const std::string text = VFS::ReadText(path);
        if (text.empty()) {
            if (warn) {
                (*warn) += "Material file [ " + path + " ] not found in asset root\n";
            }
            return false;
        }

        std::istringstream mtlStream(text);
        tinyobj::LoadMtl(matMap, materials, &mtlStream, warn, err);
        return true;
    }

private:
    std::string m_BaseDir;
};

} // namespace

// ============================================================================
// OBJ 解析器：tinyobj 解析 + 顶点装配/量化/去重 + 子网格拆分 + MTL → MaterialData
// ============================================================================

bool ModelLoader::ParseOBJ(const std::string &filepath, MeshData &out) {
    auto &vertices = out.vertices;
    auto &indices = out.indices;
    auto &subMeshes = out.subMeshes;
    auto &materialData = out.materialData;

    tinyobj::attrib_t attrib;
    std::vector<tinyobj::shape_t> shapes;
    std::vector<tinyobj::material_t> materials;
    std::string warn, err;

    // OBJ 本体也走 VFS 读进内存再灌流（tinyobj 的文件重载会自己去 fopen）
    const std::string objText = VFS::ReadText(filepath);
    if (objText.empty()) {
        GE_CORE_ERROR("[Mesh] OBJ 读不到文件: {}", filepath);
        return false;
    }
    std::istringstream objStream(objText);

    // 以 OBJ 所在目录作为 MTL 搜索基准目录（tinyobj 默认搜工作目录，会导致模型目录
    // 下的 .mtl 找不到）。同时用于后续把 MTL 内的相对纹理路径拼成规范形。
    const std::string baseDir = std::filesystem::path(filepath).parent_path().generic_string();
    VfsMaterialReader matReader(baseDir);

    bool ret = tinyobj::LoadObj(&attrib, &shapes, &materials, &warn, &err,
                                &objStream, &matReader);

    if (!warn.empty()) {
        GE_CORE_WARN("[Mesh] tinyobj 警告 ({0}): {1}", filepath, warn);
    }
    if (!err.empty()) {
        GE_CORE_ERROR("[Mesh] tinyobj 错误 ({0}): {1}", filepath, err);
    }
    if (!ret) {
        return false;
    }

    std::unordered_map<Vertex, uint32_t> uniqueVertices;

    // 遍历所有形状，几何合并到共享缓冲，并按 (shape, material_id) 拆分子网格。
    // OBJ 材质（material_id）是逐面绑定的，一个 shape（o/g 对象）内部可能含多种材质，
    // 故需在 shape 内按 material_id 对连续面分组，每组一个子网格。
    auto makeMaterialName = [&](int matId) -> std::string {
        if (matId >= 0 && static_cast<size_t>(matId) < materials.size()) {
            return materials[matId].name;
        }
        return {};
    };

    for (const auto &shape : shapes) {
        const auto &shapeIndices = shape.mesh.indices;
        const auto &materialIds = shape.mesh.material_ids;

        // 当前材质组：material_id 变化时封存上一个连续段为一个子网格
        int curMaterial = -2; // 哨兵值，表示 shape 起始
        uint32_t groupStartIndex = static_cast<uint32_t>(indices.size());

        for (size_t fi = 0; fi + 2 < shapeIndices.size(); fi += 3) {
            // material_ids 为空（无 MTL）时视为 -1
            int matId = materialIds.empty() ? -1 : materialIds[fi / 3];

            // 材质切换：封存上一个材质组为子网格
            if (fi > 0 && matId != curMaterial) {
                uint32_t firstIndex = groupStartIndex;
                uint32_t indexCount = static_cast<uint32_t>(indices.size()) - groupStartIndex;
                subMeshes.push_back({0, static_cast<uint32_t>(vertices.size()),
                                     firstIndex, indexCount,
                                     makeMaterialName(curMaterial), nullptr});
                groupStartIndex = static_cast<uint32_t>(indices.size());
            }
            curMaterial = matId;

            // 处理当前面的 3 个顶点
            for (int k = 0; k < 3; ++k) {
                const auto &index = shapeIndices[fi + k];
                Vertex v{};

                // 位置
                v.Position = {
                    attrib.vertices[3 * index.vertex_index + 0],
                    attrib.vertices[3 * index.vertex_index + 1],
                    attrib.vertices[3 * index.vertex_index + 2],
                };

                // 法线（如果有）
                if (index.normal_index >= 0) {
                    v.Normal = {
                        attrib.normals[3 * index.normal_index + 0],
                        attrib.normals[3 * index.normal_index + 1],
                        attrib.normals[3 * index.normal_index + 2],
                    };
                }

                // 纹理坐标（如果有）
                if (index.texcoord_index >= 0) {
                    v.TexCoord = {
                        attrib.texcoords[2 * index.texcoord_index + 0],
                        1 - attrib.texcoords[2 * index.texcoord_index + 1],
                    };
                }

                // 一次性量化清洗：消除浮点精度误差导致的「逻辑相同但位表示不同」的顶点，
                // 使下方去重的精确比较 / 精确哈希能正确判定（见 Vertex::Quantize 注释）
                v.Position = Vertex::Quantize(v.Position);
                v.Normal = Vertex::Quantize(v.Normal);
                v.TexCoord = Vertex::Quantize(v.TexCoord);
                v.Tangent = Vertex::Quantize(v.Tangent);

                // 去重：相同顶点复用索引
                if (uniqueVertices.find(v) == uniqueVertices.end()) {
                    uniqueVertices[v] = static_cast<uint32_t>(vertices.size());
                    vertices.push_back(v);
                }
                indices.push_back(uniqueVertices[v]);
            }
        }

        // 封存 shape 内最后一个材质组
        if (!shapeIndices.empty()) {
            uint32_t firstIndex = groupStartIndex;
            uint32_t indexCount = static_cast<uint32_t>(indices.size()) - groupStartIndex;
            subMeshes.push_back({0, static_cast<uint32_t>(vertices.size()),
                                 firstIndex, indexCount,
                                 makeMaterialName(curMaterial), nullptr});
        }
    }

    if (vertices.empty() || indices.empty()) {
        GE_CORE_ERROR("[Mesh] 空网格: '{0}'", filepath);
        return false;
    }

    // 从 tinyobj 解析出的 MTL 材质捕获为引擎侧 MaterialData。
    // 各子网格的 materialName 即对应 MTL 材质名，MeshManager 据此匹配构建材质。
    auto resolveTex = [&](const std::string &tex) -> std::string {
        if (tex.empty()) {
            return {};
        }
        std::filesystem::path p(tex);
        // generic_string()：结果是**资产的规范形**路径，一律正斜杠
        if (p.is_absolute()) {
            return p.lexically_normal().generic_string();
        }
        return (std::filesystem::path(filepath).parent_path() / tex)
            .lexically_normal().generic_string();
    };

    materialData.reserve(materials.size());
    for (const auto &src : materials) {
        MaterialData md;
        md.name = src.name;
        md.baseColor = {src.diffuse[0], src.diffuse[1], src.diffuse[2]};
        md.specular = {src.specular[0], src.specular[1], src.specular[2]};
        md.emissive = {src.emission[0], src.emission[1], src.emission[2]};
        md.shininess = src.shininess > 0.0f ? src.shininess : 32.0f;
        md.dissolve = src.dissolve;
        // MTL 无 alphaMode 概念：dissolve < 1 即半透明（Blend），否则不透明。
        // （MASK 裁剪无 MTL 对应物，保持默认 Opaque。）
        if (src.dissolve < 1.0f) {
            md.alphaMode = Material::AlphaMode::Blend;
        }
        md.metallic = src.metallic;
        md.roughness = src.roughness > 0.0f ? src.roughness : 0.5f;
        // 存在 PBR 扩展参数（非零标量或 MR 贴图）即视为 PBR 材质
        md.hasPBR = (src.metallic > 0.0f || src.roughness > 0.0f
                     || !src.metallic_texname.empty() || !src.roughness_texname.empty());

        md.albedoMap = resolveTex(src.diffuse_texname);
        // 法线贴图：优先 norm，其次 map_bump
        md.normalMap = resolveTex(!src.normal_texname.empty()
                                      ? src.normal_texname
                                      : src.bump_texname);
        md.emissiveMap = resolveTex(src.emissive_texname);
        md.metallicMap = resolveTex(src.metallic_texname);
        md.roughnessMap = resolveTex(src.roughness_texname);

        materialData.push_back(std::move(md));
    }

    return true;
}

} // namespace GE