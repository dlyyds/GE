/**
 * @file MeshManager.cpp
 * @brief 全局网格管理器实现。
 */

#include "Render/MeshManager.h"

#include "Render/AsyncUploadManager.h"
#include "Render/MaterialManager.h"
#include "Render/TextureManager.h"

#include "Render/VulkanBase/VulkanDevice.h"
#include "Core/Log.h"

#include <glm/glm.hpp>

#include <filesystem>

namespace GE {

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

    // 金属-粗糙度贴图：OBJ 的 map_Pm / map_Pr 是两张独立灰度图，无法直接
    // 合并进 glTF 惯例的 MR 纹理（B=金属度, G=粗糙度），此处暂不加载，
    // 仅用标量 metallic/roughness（已在上方设置）。
    (void)md.metallicMap;
    (void)md.roughnessMap;
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

    // 内置几何体：体积小、生成瞬时完成且被编辑器即时消费，保持同步立即就绪
    if (Mesh::IsBuiltinPath(filepath)) {
        auto mesh = Mesh::CreateBuiltin(*m_Device, Mesh::GetBuiltinType(filepath));
        if (!mesh) {
            GE_CORE_WARN("MeshManager: 内置网格创建失败: {}", filepath);
            return nullptr;
        }
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

    // 材质构建在数据安装完成后、置就绪前于主线程 finalize 中执行
    auto shell = Mesh::LoadFromFileAsync(*m_Device, *m_AsyncUpload, filepath,
                                         [this, filepath](Mesh &mesh) {
                                             BuildSubMeshMaterials(mesh, filepath);
                                         });
    if (!shell) {
        GE_CORE_WARN("MeshManager: 网格异步加载提交失败: {}", filepath);
        return nullptr;
    }

    Mesh *raw = shell.get();
    m_Meshes[filepath] = std::move(shell);
    return raw;
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