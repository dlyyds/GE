/**
 * @file MeshManager.cpp
 * @brief 全局网格管理器实现。
 */

#include "Render/MeshManager.h"

#include "Render/ModelLoader.h"
#include "Render/GLTFLoader.h"
#include "Render/AsyncUploadManager.h"
#include "Render/MaterialManager.h"
#include "Render/TextureManager.h"

#include "Render/VulkanBase/VulkanDevice.h"
#include "Render/VulkanBase/VulkanCommandBuffer.h"
#include "Core/Log.h"

#include <glm/glm.hpp>

#include <cmath>
#include <filesystem>
#include <algorithm>
#include <memory>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

namespace GE {

// ============================================================================
// 异步加载共享数据容器（decode / upload / finalize 三阶段间传递）
// ============================================================================

/**
 * @brief 网格异步加载共享数据容器（decode / upload / finalize 三阶段间传递）。
 *
 * 由主线程组装参数，后台线程写入 decode / upload 结果，最终主线程 finalize 读取。
 * 经 shared_ptr 在阶段回调间共享，保证后台只产出局部对象（不持 Mesh 裸指针），
 * 所有写 Mesh 成员的动作都收敛到主线程 finalize。data 在 finalize 注入 Mesh 时
 * 只回收摘要（子网格/材质数据/计数），vertices/indices 随之释放。
 */
struct AsyncMeshLoadData {
    bool parsed = false; ///< 文件解析是否成功（decode 写入）

    // decode 输出（后台线程写入）
    MeshData data; ///< 解析/几何产物（已含切线）

    // upload 输出（后台线程写入，主线程 finalize 消费）
    std::unique_ptr<VulkanBuffer> stagingVB; ///< 顶点 staging buffer（GPU 用完后随桶释放）
    std::unique_ptr<VulkanBuffer> stagingIB; ///< 索引 staging buffer（GPU 用完后随桶释放）
    std::unique_ptr<VulkanBuffer> vertexBuffer; ///< 后台创建的本地顶点缓冲
    std::unique_ptr<VulkanBuffer> indexBuffer; ///< 后台创建的本地索引缓冲
};

MeshManager::MeshManager(VulkanDevice &device, MaterialManager &materialManager,
                         TextureManager &textureManager, AsyncUploadManager &upload)
    : m_Device(&device), m_Materials(&materialManager), m_Textures(&textureManager),
      m_AsyncUpload(&upload) {
}

// ============================================================================
// 辅助：按 MTL 材质数据填充 Material
// ============================================================================

/**
 * @brief 根据 OBJ/MTL 捕获的材质数据填充一个 Material。
 *
 * 决定材质类型（含 PBR 扩展参数 → PBR，否则 Blinn-Phong），设置标量参数，
 * 并加载 MTL 引用的纹理（漫反射 / 法线 / 自发光）。无漫反射贴图时用
 * 漫反射颜色的纯色纹理，使 MTL 中 Kd 颜色能正确显示。
 *
 * @param mat    目标材质（填充其属性）
 * @param md     MTL 材质数据
 * @param texMgr 纹理管理器（加载纹理）
 */
static void ApplyMaterialData(Material &mat, const MaterialData &md,
                              TextureManager &texMgr) {
    // 材质类型：含 PBR 扩展参数（非零 metallic/roughness 或 MR 贴图）→ PBR
    mat.SetType(md.hasPBR ? Material::Type::PBR : Material::Type::BlinnPhong);

    // 标量参数（SetType 已按类型补齐默认值，这里覆盖为 MTL 实际值）
    if (md.hasPBR) {
        mat.SetFloat("metallic", md.metallic);
        mat.SetFloat("roughness", md.roughness);
    } else {
        mat.SetFloat("shininess", md.shininess);
        // 高光强度取高光颜色三通道均值（无高光色时趋近 0，即无高光）
        mat.SetFloat("specularStrength",
                     (md.specular.r + md.specular.g + md.specular.b) / 3.0f);
    }
    // 自发光颜色因子：取 MTL 发光颜色（emissiveFactor，乘自发光贴图颜色）
    mat.SetEmissiveFactor(glm::vec3(md.emissive));

    // ── 透明度语义透传（glTF alphaMode 对齐）──
    // 半透明（Blend）材质除 alphaMode 外还需把基础 alpha（dissolve）作为因子：
    //  1. 采样侧最终 alpha = 纹理 alpha × 材质基础 alpha（baseAlpha）× 实例 tint alpha
    //     （baseAlpha 存浮点参数，Renderer3D 填 MaterialUBO.emissiveFactor.w 传给 shader）；
    //  2. 编辑器里对纯色无贴图材质，此处已用 glm::vec4(baseColor, dissolve) 生成带 alpha 纯色图。
    mat.alphaMode = md.alphaMode;
    mat.alphaCutoff = md.alphaCutoff;
    mat.doubleSided = md.doubleSided;
    mat.SetFloat("baseAlpha", md.dissolve);

    // 漫反射贴图：有 map_Kd 则加载；否则用漫反射颜色的纯色纹理（非白色时）。
    // 颜色贴图以 sRGB 格式加载，硬件采样时自动解码到线性空间（与输出侧
    // sRGB swapchain 的硬件编码配对成标准线性管线）。法线/金属度/粗糙度是
    // 数据而非颜色，仍保持 Unorm 不解码（见下方 normalMap 等分支）。
    if (!md.albedoMap.empty()) {
        if (Texture *t = texMgr.LoadAsync(md.albedoMap, vk::Format::eR8G8B8A8Srgb)) {
            mat.SetTexture(Material::Albedo, t);
        }
    } else {
        bool nearWhite = md.baseColor.r > 0.99f && md.baseColor.g > 0.99f
                       && md.baseColor.b > 0.99f;
        if (!nearWhite) {
            mat.SetTexture(Material::Albedo,
                           texMgr.GetSolidColor(glm::vec4(md.baseColor, md.dissolve),
                                                vk::Format::eR8G8B8A8Srgb));
        }
    }

    // 法线贴图（map_bump / norm）
    if (!md.normalMap.empty()) {
        if (Texture *t = texMgr.LoadAsync(md.normalMap)) {
            mat.SetTexture(Material::Normal, t);
        }
    }

    // 自发光贴图（map_Ke）
    if (!md.emissiveMap.empty()) {
        if (Texture *t = texMgr.LoadAsync(md.emissiveMap)) {
            mat.SetTexture(Material::Emissive, t);
        }
    }

    // 金属-粗糙度贴图：glTF 惯例合并贴图（B=金属度, G=粗糙度，Unorm 数据不解码）。
    // 有 MR 贴图则绑定；无则保留槽位为空，Renderer 侧绑定默认 (G=1,B=1) 纹理
    // 回退到标量 metallic/roughness。OBJ 的 map_Pm/map_Pr 是两张独立灰度图，
    // 无法直接合并进 glTF 惯例的 MR 纹理，此处暂不加载（仅用标量）。
    if (!md.metallicRoughnessMap.empty()) {
        if (Texture *t = texMgr.LoadAsync(md.metallicRoughnessMap)) {
            mat.SetTexture(Material::MetallicRoughness, t);
        }
    }
    (void)md.metallicMap;
    (void)md.roughnessMap;
}

// ============================================================================
// 内置几何体：生成 CPU 顶点/索引数据（不带子网格，由 Mesh::Create 统一补全）
// ============================================================================

bool MeshManager::GenerateBuiltinMeshData(const std::string &type, MeshData &out) {
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

MeshManager::~MeshManager() {
    Clear();
}

Mesh *MeshManager::Get(const std::string &filepath) const {
    auto it = m_Meshes.find(filepath);
    if (it != m_Meshes.end()) {
        return it->second.get();
    }
    return nullptr;
}

bool MeshManager::Has(const std::string &filepath) const {
    return m_Meshes.find(filepath) != m_Meshes.end();
}

Mesh *MeshManager::Load(const std::string &filepath) {
    if (filepath.empty()) {
        return nullptr;
    }

    // 已加载（含未就绪空壳）则直接返回缓存，同路径只异步加载一次
    if (Mesh *existing = Get(filepath)) {
        return existing;
    }

    if (!m_Device || !m_AsyncUpload) {
        GE_CORE_WARN("MeshManager: 无法加载网格 {}（未提供 VulkanDevice / AsyncUploadManager）",
                     filepath);
        return nullptr;
    }

    // 内置几何体：体积小、生成瞬时完成且被编辑器即时消费，保持同步立即就绪。
    // 几何数据由本类 GenerateBuiltinMeshData 生成，装配（切线 + 上传 + 摘要）由 Mesh::Create 完成。
    if (IsBuiltinPath(filepath)) {
        MeshData data;
        if (!GenerateBuiltinMeshData(GetBuiltinType(filepath), data)) {
            GE_CORE_WARN("MeshManager: 内置网格创建失败: {}", filepath);
            return nullptr;
        }
        auto mesh = Mesh::Create(*m_Device, std::move(data));
        if (!mesh) {
            GE_CORE_WARN("MeshManager: 内置网格创建失败: {}", filepath);
            return nullptr;
        }
        mesh->SetFilePath(filepath); // "builtin:<type>"
        BuildSubMeshMaterials(*mesh, filepath);
        Mesh *raw = mesh.get();
        m_Meshes[filepath] = std::move(mesh);
        return raw;
    }

    // 文件模型：异步加载（后台解析 + GPU 上传，主线程 Poll 回收后置就绪）。
    // 文件不存在直接返回 nullptr，保留同步失败语义（编辑器据此弹窗）；文件存在但
    // 内容非法时返回空壳（后台解析失败后保持未就绪，与纹理失败行为一致）。
    if (!std::filesystem::exists(filepath)) {
        GE_CORE_WARN("MeshManager: 网格文件不存在: {}", filepath);
        return nullptr;
    }

    // ── 编排：造空壳 → 组装 UploadTask（decode/upload/finalize）→ 提交 → 注册 ──
    // 状态机 / 时序与拆分前完全一致：后台解析 + 切线 + 上传，主线程 finalize 经
    // 槽位门控后注入数据、构建子网格材质、置就绪。
    auto shell = Mesh::CreateShell(filepath);
    auto bucket = std::make_shared<AsyncMeshLoadData>();
    auto slot = shell->GetAsyncSlot();

    AsyncUploadManager::UploadTask task;
    task.decode = [bucket, filepath] {
        // 后台：按扩展名分派解析（OBJ/MTL 解析 + 材质数据捕获，纯 CPU）
        bucket->parsed = ModelLoader::Parse(filepath, bucket->data);
        if (!bucket->parsed) {
            return;
        }
        // 切线计算（供法线贴图 TBN 使用），与同步路径 Mesh::Create 共用共享装配
        ModelLoader::ComputeTangents(bucket->data);
        GE_CORE_TRACE("网格异步解析完成: {0} ({1} 顶点 / {2} 索引)", filepath,
                      bucket->data.vertices.size(), bucket->data.indices.size());
    };

    task.upload = [this, bucket](VulkanCommandBuffer &cmd) {
        // 后台：创建本地顶点/索引缓冲 + staging，录拷贝命令。
        // 解析失败时跳过创建；finalize 见到空缓冲会跳过注入（与纹理失败行为一致）
        if (!bucket->parsed || bucket->data.vertices.empty() || bucket->data.indices.empty()) {
            return;
        }

        auto recordCopy = [this, &cmd, bucket](
            vk::BufferUsageFlagBits usage, vk::DeviceSize size, const void *data,
            std::unique_ptr<VulkanBuffer> &stagingOut,
            std::unique_ptr<VulkanBuffer> &dstOut) {
            stagingOut = std::make_unique<VulkanBuffer>(std::move(
                VulkanBuffer::create_staging_buffer(*m_Device, size, data)));
            dstOut = VulkanBufferBuilder(size)
                .with_usage(usage | vk::BufferUsageFlagBits::eTransferDst)
                .with_vma_usage(VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE)
                .build_unique(*m_Device);
            vk::BufferCopy copyRegion{};
            copyRegion.size = size;
            cmd.GetHandle().copyBuffer(stagingOut->GetHandle(), dstOut->GetHandle(), copyRegion);
        };

        recordCopy(vk::BufferUsageFlagBits::eVertexBuffer,
                   static_cast<vk::DeviceSize>(bucket->data.vertices.size() * sizeof(Vertex)),
                   bucket->data.vertices.data(), bucket->stagingVB, bucket->vertexBuffer);
        recordCopy(vk::BufferUsageFlagBits::eIndexBuffer,
                   static_cast<vk::DeviceSize>(bucket->data.indices.size() * sizeof(uint32_t)),
                   bucket->data.indices.data(), bucket->stagingIB, bucket->indexBuffer);
    };

    task.finalize = [this, filepath, slot, bucket] {
        // 主线程：空壳已销毁或加载失败（无数据/无缓冲）则跳过注入与材质构建
        if (slot->abandoned || !slot->target) {
            return;
        }
        Mesh *mesh = slot->target;
        if (!bucket->parsed || !bucket->vertexBuffer || !bucket->indexBuffer) {
            return; // 解析/上传失败，保持未就绪（与纹理失败行为一致）
        }
        mesh->InstallAsyncData(std::move(bucket->data),
                               std::move(bucket->vertexBuffer), std::move(bucket->indexBuffer));
        // 先构建子网格材质再置就绪，保证渲染看到就绪网格时材质已绑定
        BuildSubMeshMaterials(*mesh, filepath);
        mesh->MarkReady();
        GE_CORE_INFO("网格异步就绪: {0} ({1} 顶点 / {2} 索引)", filepath,
                     mesh->GetVertexCount(), mesh->GetIndexCount());
    };

    m_AsyncUpload->Submit(std::move(task));
    GE_CORE_INFO("网格异步加载提交: {0}", filepath);

    Mesh *raw = shell.get();
    m_Meshes[filepath] = std::move(shell);
    return raw;
}

// ============================================================================
// 同步加载 glTF 指定 mesh（场景导入器按 mesh 粒度复用 / 去重）
// ============================================================================

Mesh *MeshManager::FinalizeGLTFMesh(const std::string &filepath, size_t meshIndex,
                                    MeshData &&data) {
    // mesh 0 复用文件路径本身为键（与 Load("foo.gltf") 共享一份 GPU 网格）；
    // mesh N>0 用 "foo.gltf#N" 复合键，供多 mesh 场景导入去重。
    const std::string key = (meshIndex == 0)
        ? filepath
        : (filepath + "#" + std::to_string(meshIndex));

    if (Mesh *existing = Get(key)) {
        return existing;
    }

    auto mesh = Mesh::Create(*m_Device, std::move(data)); // BuildMesh 内部统一 ComputeTangents
    if (!mesh) {
        GE_CORE_WARN("MeshManager: glTF mesh 创建失败: {}", key);
        return nullptr;
    }
    mesh->SetFilePath(key);
    BuildSubMeshMaterials(*mesh, key);
    return Register(key, std::move(mesh));
}

Mesh *MeshManager::LoadGLTFMesh(const std::string &filepath, size_t meshIndex) {
    if (!m_Device || !m_Materials || !m_Textures) {
        GE_CORE_WARN("MeshManager: 无法加载 glTF mesh {}（缺少渲染子系统）", filepath);
        return nullptr;
    }

    MeshData data;
    if (!GLTF::BuildMeshData(filepath, meshIndex, data)) {
        GE_CORE_WARN("MeshManager: glTF mesh 解析失败: {}#{}", filepath, meshIndex);
        return nullptr;
    }
    return FinalizeGLTFMesh(filepath, meshIndex, std::move(data));
}

Mesh *MeshManager::LoadGLTFMesh(const std::string &filepath, size_t meshIndex,
                                const tinygltf::Model &model) {
    if (!m_Device || !m_Materials || !m_Textures) {
        GE_CORE_WARN("MeshManager: 无法加载 glTF mesh {}（缺少渲染子系统）", filepath);
        return nullptr;
    }

    // 复用调用方已解析的 model，跳过再一次 LoadModel 的磁盘 IO + 整文件解析
    MeshData data;
    if (!GLTF::BuildMesh(model, meshIndex, filepath, data)) {
        GE_CORE_WARN("MeshManager: glTF mesh 解析失败: {}#{}", filepath, meshIndex);
        return nullptr;
    }
    return FinalizeGLTFMesh(filepath, meshIndex, std::move(data));
}

// ============================================================================
// 辅助：为子网格构建默认材质（同步内置路径与异步 finalize 共用）
// ============================================================================

// 为各子网格创建材质（放进 MaterialManager，随模型生命周期走）。
// 材质 key 用「路径::材质名」避免跨模型同名材质冲突。
// 有材质名（OBJ MTL）→ 按 MTL 数据构建真实材质；无材质名（内置几何体 /
// 无 MTL 的 OBJ / CPU 直建）→ 绑一个默认（空白）材质，保证每个子网格都
// 有材质，而非走白色 fallback。
void MeshManager::BuildSubMeshMaterials(Mesh &mesh, const std::string &filepath) {
    if (!m_Materials) {
        return;
    }

    const auto &subMeshes = mesh.GetSubMeshes();
    const auto &matData   = mesh.GetMaterialData();
    for (size_t i = 0; i < subMeshes.size(); ++i) {
        const auto &name = subMeshes[i].materialName;
        const std::string matName = name.empty() ? "default" : name;
        const std::string key = filepath + "::" + matName;

        // 按子网格材质名匹配对应的 MTL 材质数据
        const MaterialData *md = nullptr;
        if (!name.empty()) {
            for (const auto &cand : matData) {
                if (cand.name == name) {
                    md = &cand;
                    break;
                }
            }
        }

        // 已存在则复用（同 key 首次创建时填充），否则新建并注册
        Material *mat = m_Materials->Get(key);
        if (!mat) {
            auto newMat = std::make_unique<Material>();
            if (md && m_Textures) {
                ApplyMaterialData(*newMat, *md, *m_Textures);
            }
            mat = m_Materials->Register(key, std::move(newMat));
        }
        mat->SetName(matName);
        mesh.SetSubMeshDefaultMaterial(static_cast<uint32_t>(i), mat);
    }
}

Mesh *MeshManager::GetBuiltin(const std::string &type) {
    return Load("builtin:" + type);
}

Mesh *MeshManager::CreateWaterGrid(uint32_t resolution) {
    const uint32_t n = std::max<uint32_t>(1u, resolution);
    // 统一生成 1×1 单位水格；实际尺寸由 Renderer3D_Record 在 water.model 中按 batch.size 缩放，
    // 避免每个连续尺寸取值都生成新网格 / 新材质（旧的 size 进 key 会导致材质爆炸）。
    const std::string key = "watergrid:" + std::to_string(n);

    if (Mesh *existing = Get(key)) {
        return existing;
    }
    if (!m_Device) {
        GE_CORE_WARN("MeshManager: 无法创建水格 {}（未提供 VulkanDevice）", key);
        return nullptr;
    }

    // ── 生成 CPU 顶点/索引数据 ──
    // XZ 平面，中心在原点，法线 +Y。UV 覆盖 [0,1]，供法线贴图平铺。
    MeshData data;
    auto &vertices = data.vertices;
    auto &indices = data.indices;

    vertices.reserve(static_cast<size_t>(n + 1) * (n + 1));
    for (uint32_t row = 0; row <= n; ++row) {
        const float v = static_cast<float>(row) / static_cast<float>(n);
        const float z = (v - 0.5f) * 1.0f;
        for (uint32_t col = 0; col <= n; ++col) {
            const float u = static_cast<float>(col) / static_cast<float>(n);
            const float x = (u - 0.5f) * 1.0f;

            Vertex vert;
            vert.Position = {x, 0.0f, z};
            vert.Normal = {0.0f, 1.0f, 0.0f};
            vert.TexCoord = {u, v};
            vert.Tangent = {1.0f, 0.0f, 0.0f, 1.0f};
            // JointIdx/Weight 保持默认 {0,0,0,0}，静态网格不参与蒙皮
            vertices.push_back(vert);
        }
    }

    indices.reserve(static_cast<size_t>(n) * n * 6);
    for (uint32_t row = 0; row < n; ++row) {
        for (uint32_t col = 0; col < n; ++col) {
            const uint32_t a = row * (n + 1) + col;
            const uint32_t b = a + 1;
            const uint32_t c = a + (n + 1);
            const uint32_t d = c + 1;
            // 逆时针（从 +Y 俯视）的两组三角（a,c,b / b,c,d），法线朝 +Y
            indices.push_back(a);
            indices.push_back(c);
            indices.push_back(b);
            indices.push_back(b);
            indices.push_back(c);
            indices.push_back(d);
        }
    }

    auto mesh = Mesh::Create(*m_Device, std::move(data));
    if (!mesh) {
        GE_CORE_WARN("MeshManager: 水格创建失败: {}", key);
        return nullptr;
    }
    mesh->SetFilePath(key);
    BuildSubMeshMaterials(*mesh, key);

    return Register(key, std::move(mesh));
}

Mesh *MeshManager::Register(const std::string &key,
                            std::unique_ptr<Mesh> mesh) {
    if (!mesh) {
        return nullptr;
    }
    Mesh *raw = mesh.get();
    m_Meshes[key] = std::move(mesh);
    return raw;
}

void MeshManager::Unload(const std::string &filepath) {
    m_Meshes.erase(filepath);
}

void MeshManager::Clear() {
    m_Meshes.clear();
}

std::vector<std::string> MeshManager::GetAllKeys() const {
    std::vector<std::string> keys;
    keys.reserve(m_Meshes.size());
    for (const auto &pair : m_Meshes) {
        keys.push_back(pair.first);
    }
    return keys;
}

} // namespace GE
